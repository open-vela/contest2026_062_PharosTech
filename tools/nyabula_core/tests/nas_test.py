#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real NAS service and HTTP transport over a controlled USB/mount fixture."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
parser.add_argument('cjson', type=Path)
args = parser.parse_args()
http = (args.core / 'ny_web_http.c').read_text()
match = re.search(r'const char \*ny_web_http_header\([^;]+?\n\{', http)
assert match
end, depth = match.end(), 1
while depth:
    depth += (http[end] == '{') - (http[end] == '}')
    end += 1
header_function = http[match.start():end]
code = r'''
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ny_nas.h"
#include "ny_web_auth.h"
static bool present = true;
static uint64_t clock_ms = 100000, revision;
static cJSON *stored;
static uint32_t auth_generation;
uint32_t ny_web_auth_generation(void) { return auth_generation; }
static int fixture_lstat(const char *path, struct stat *st)
{
  if (!strcmp(path, "/dev/sda")) {
    if (!present) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st)); st->st_mode = S_IFBLK | 0600; return 0;
  }
  return lstat(path, st);
}
static DIR *fixture_opendir(const char *path)
{
  return opendir(!strcmp(path, "/dev") ? TEST_DEV : path);
}
static int fixture_mkdir(const char *path, mode_t mode)
{
  return !strcmp(path, "/mnt") ? 0 : mkdir(path, mode);
}
static int fixture_mount(const char *source, const char *target, const char *type,
                          unsigned long flags, const void *data)
{
  (void)data;
  assert(!strcmp(source, "/dev/sda") && !strcmp(type, "fatfs"));
  assert(flags == 0 || flags == MS_RDONLY);
  assert(!strcmp(target, CONFIG_NYABULA_CORE_NAS_MOUNT));
  assert(rmdir(target) == 0);
  return rename(TEST_VOLUME, target);
}
static int fixture_umount(const char *path)
{
  assert(!strcmp(path, CONFIG_NYABULA_CORE_NAS_MOUNT));
  if (rename(path, TEST_VOLUME) < 0) return -1;
  return mkdir(path, 0700);
}
uint64_t ny_product_time_ms(bool monotonic)
{
  (void)monotonic; return clock_ms;
}
int ny_product_store_read(const char *domain, cJSON **value, uint64_t *version)
{
  assert(!strcmp(domain, "nas")); *value = stored ? cJSON_Duplicate(stored, true) : NULL;
  *version = revision; return 0;
}
int ny_product_store_write(const char *domain, const cJSON *value,
                           uint64_t expected, uint64_t *version)
{
  assert(!strcmp(domain, "nas"));
  if (expected != revision) return -ESTALE;
  cJSON_Delete(stored); stored = cJSON_Duplicate(value, true);
  assert(stored); *version = ++revision; return 0;
}
#define lstat fixture_lstat
#define opendir fixture_opendir
#define mkdir fixture_mkdir
#define mount fixture_mount
#define umount fixture_umount
#include "ny_nas.c"
#undef lstat
#undef opendir
#undef mkdir
#undef mount
#undef umount
bool ny_web_auth_accept_token(const char *pair, const char *offered, size_t size)
{
  return size == 64 && !memcmp(pair, offered, 64);
}
uint64_t nyabula_eye_ws_now(void) { return clock_ms; }
int ny_web_http_write(int fd, const void *data, size_t size)
{
  size_t done = 0;
  while (done < size) {
    ssize_t count = send(fd, (const char *)data + done, size - done, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return -EIO;
    done += count;
  }
  return 0;
}
HEADER_FUNCTION
#include "ny_web_nas.c"
static struct ny_product_caller_s owner = {"fixture", NY_PRODUCT_OWNER, true};
static cJSON *call(const char *topic, const char *text, int expected)
{
  cJSON *data = cJSON_Parse(text), *result = NULL;
  assert(data); int ret = ny_nas_request(&owner, topic, data, &result);
  if (ret != expected) fprintf(stderr, "%s returned %d expected %d\n", topic, ret, expected);
  assert(ret == expected); cJSON_Delete(data); return result;
}
static void configure(bool enabled, bool readonly)
{
  char data[160];
  snprintf(data, sizeof(data), "{\"enabled\":%s,\"readOnly\":%s,\"device\":\"auto\",\"revision\":%llu}",
    enabled ? "true" : "false", readonly ? "true" : "false", (unsigned long long)revision);
  cJSON *result = call("nas.configure", data, 0); assert(result); cJSON_Delete(result);
}
static const char pair[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
struct http_job { int fd; char head[1024]; int result; };
static void *serve(void *argument)
{
  struct http_job *job = argument;
  job->result = ny_web_nas_serve(job->fd, job->head, NULL, 0, pair);
  close(job->fd); return NULL;
}
static char *http_call(const char *method, const char *path, const char *extra,
                       const char *data, size_t length, size_t expected)
{
  int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  struct http_job job = {.fd = sockets[0]};
  char authorization[128] = "";
  if (strcmp(extra, "NOAUTH"))
    snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s\r\n", pair);
  else
    extra = "";
  snprintf(job.head, sizeof(job.head), "%s /nas/file?path=%s HTTP/1.1\r\n"
    "%sContent-Length: %zu\r\n%s\r\n",
    method, path, authorization, expected, extra);
  pthread_t thread; assert(pthread_create(&thread, NULL, serve, &job) == 0);
  if (length) assert(ny_web_http_write(sockets[1], data, length) == 0);
  shutdown(sockets[1], SHUT_WR);
  char *response = calloc(1, 1100000); assert(response);
  size_t used = 0; ssize_t count;
  while ((count = read(sockets[1], response + used, 1099999 - used)) > 0) used += count;
  close(sockets[1]); pthread_join(thread, NULL);
  assert(g_nas_transfers == 0 && g_nas_uploads == 0);
  return response;
}
int main(void)
{
  assert(ny_nas_device_name("/dev/sda") && ny_nas_device_name("/dev/sda1"));
  assert(!ny_nas_device_name("/dev/mmcsd1") && !ny_nas_device_name("/dev/sda/../mmcsd1"));
  configure(true, true); assert(ny_nas_ready());
  struct ny_nas_file_s *file = NULL; uint64_t size;
  assert(ny_nas_file_open("../secret", false, 0, &file, &size) == -EINVAL);
  assert(ny_nas_file_open("link", false, 0, &file, &size) == -EACCES);
  assert(ny_nas_file_open("new.bin", true, 4, &file, &size) == -EROFS);
  cJSON *ticket = call("nas.download", "{\"path\":\"original.txt\"}", 0);
  char query[160];
  snprintf(query, sizeof(query), "original.txt&ticket=%s",
           ny_nas_string(ticket, "ticket"));
  char *unauthorized = http_call("GET", "original.txt", "NOAUTH", NULL, 0, 0);
  assert(strstr(unauthorized, "401 Unauthorized")); free(unauthorized);
  char *authorized = http_call("GET", query, "NOAUTH", NULL, 0, 0);
  assert(strstr(authorized, "200 OK")); free(authorized);
  assert(!ny_nas_ticket_authorized("other.txt", ny_nas_string(ticket, "ticket")));
  auth_generation++;
  authorized = http_call("GET", query, "NOAUTH", NULL, 0, 0);
  assert(strstr(authorized, "401 Unauthorized")); free(authorized);
  cJSON_Delete(ticket);
  assert(ny_nas_file_open("original.txt", false, 0, &file, &size) == 0);
  char data[32] = {0}; assert(ny_nas_file_read(file, data, sizeof(data)) == 8);
  assert(!strcmp(data, "original")); assert(ny_nas_file_close(file, false) == 0);
  configure(true, false);
  assert(ny_nas_file_open("original.txt", true, 4, &file, &size) == -EEXIST);
  assert(ny_nas_file_open("partial.bin", true, 4, &file, &size) == 0);
  assert(ny_nas_file_write(file, "ab", 2) == 2);
  assert(ny_nas_file_close(file, true) == -ENODATA);
  assert(access(CONFIG_NYABULA_CORE_NAS_MOUNT "/partial.bin", F_OK) < 0);
  char *payload = malloc(1024 * 1024); assert(payload);
  for (int i = 0; i < 1024 * 1024; i++) payload[i] = 'A' + i % 26;
  char *response = http_call("PUT", "transfer.bin", "", payload, 1024 * 1024, 1024 * 1024);
  assert(strstr(response, "201 Created")); free(response);
  response = http_call("GET", "transfer.bin", "", NULL, 0, 0);
  char *body = strstr(response, "\r\n\r\n"); assert(body);
  assert(!memcmp(body + 4, payload, 1024 * 1024)); free(response); free(payload);
  response = http_call("GET", "original.txt", "Range: bytes=1-3\r\n", NULL, 0, 0);
  assert(strstr(response, "206 Partial Content") && strstr(response, "Content-Length: 3\r\n"));
  assert(!strcmp(strstr(response, "\r\n\r\n") + 4, "rig")); free(response);
  response = http_call("GET", "original.txt", "Range: bytes=-3\r\n", NULL, 0, 0);
  assert(!strcmp(strstr(response, "\r\n\r\n") + 4, "nal")); free(response);
  response = http_call("HEAD", "original.txt", "", NULL, 0, 0);
  assert(strstr(response, "Content-Length: 8\r\n") && !*(strstr(response, "\r\n\r\n") + 4)); free(response);
  response = http_call("GET", "original.txt", "Range: bytes=99-\r\n", NULL, 0, 0);
  assert(strstr(response, "416 Range Not Satisfiable")); free(response);
  response = http_call("GET", "%2e%2e/secret", "", NULL, 0, 0);
  assert(strstr(response, "400 Error")); free(response);
  response = http_call("PUT", "aborted.bin", "", "short", 5, 20);
  assert(strstr(response, "500 Error")); free(response);
  assert(access(CONFIG_NYABULA_CORE_NAS_MOUNT "/aborted.bin", F_OK) < 0);
  assert(ny_nas_file_open("hot.bin", true, 4, &file, &size) == 0);
  char config[160];
  snprintf(config, sizeof(config), "{\"enabled\":false,\"readOnly\":true,\"device\":\"auto\",\"revision\":%llu}",
            (unsigned long long)revision);
  cJSON_Delete(call("nas.configure", config, -EBUSY));
  atomic_store(&g_nas_lost, true); present = false;
  assert(ny_nas_file_write(file, "test", 4) == -ENODEV);
  assert(ny_nas_file_close(file, false) == 0);
  clock_ms += 31000; assert(ny_nas_tick() == 0 && !g_nas_mounted);
  present = true; clock_ms += 31000; assert(ny_nas_tick() == 0 && ny_nas_ready());
  assert(ny_nas_file_open("original.txt", false, 0, &file, &size) == 0 && size == 8);
  assert(ny_nas_file_close(file, false) == 0);
  cJSON *listing = call("nas.list", "{\"path\":\"\"}", 0);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(listing, "entries")) == 2);
  cJSON_Delete(listing); configure(false, true); cJSON_Delete(stored);
  puts("NAS_FIXTURE_1MIB_RANGE_SCOPED_TICKETS_RO_TRAVERSAL_NO_OVERWRITE_ABORT_HOTPLUG_PASS");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ny-nas-') as directory:
    root = Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'netutils').mkdir()
    (root / 'nuttx/config.h').write_text('')
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock pthread_mutex_lock\n'
        '#define nxmutex_unlock pthread_mutex_unlock\n')
    (root / 'netutils/cJSON.h').write_text('#include <cJSON.h>\n')
    (root / 'test.c').write_text(code.replace('HEADER_FUNCTION', header_function))
    for name, flags in [('plain', ['-O2']), ('sanitized',
            ['-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]:
        fixture = root / name
        fixture.mkdir()
        volume = fixture / 'volume'
        devices = fixture / 'devices'
        volume.mkdir(); devices.mkdir()
        (devices / 'sda').touch()
        (volume / 'original.txt').write_text('original')
        (volume / 'link').symlink_to(fixture / 'secret')
        binary = root / (name + '.exe')
        subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-Wall', '-Wextra',
                        '-Werror', *flags, '-I' + str(root), '-I' + str(args.core),
                        '-I' + str(args.cjson),
                        '-DCONFIG_NYABULA_CORE_NAS_MOUNT="' + str(fixture / 'mount') + '"',
                        '-DTEST_DEV="' + str(devices) + '"',
                        '-DTEST_VOLUME="' + str(volume) + '"',
                        str(root / 'test.c'), str(args.cjson / 'cJSON.c'),
                        '-pthread', '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
