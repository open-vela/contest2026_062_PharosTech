/****************************************************************************
 * app/nyabula_core/ny_compute.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * The control-domain end of the AMP compute link.
 *
 * Three jobs share one receive task because they share one receive queue:
 *
 *   1. Link state.  The compute domain announces a generation when its
 *      daemon starts (READY) and repeats it in every HEALTH response.  A new
 *      generation means a new daemon: everything the old one held is void.
 *
 *   2. Demultiplexing.  Responses and events go to the local port whose
 *      request_id they carry.  Nothing is ever sent in reply to a response
 *      or an event: both domains answer requests now, and an error reply to
 *      a reply is how two responders would loop.
 *
 *   3. The BLOB service.  The compute domain has no storage; this side owns
 *      the eMMC and /data.  It asks for a model file by name, grants a window
 *      of the shared arena per READ, and this task fills the window straight
 *      from the file.  The compute domain stays the only allocator of the
 *      arena -- this side never chooses an offset, it only refuses ones that
 *      would touch the arena header or leave the arena.
 *
 * The receive task must never block for long: while it is blocked no local
 * requester gets its response.  Hashing a large file on OPEN (875 MB at about
 * 75 MiB/s is twelve seconds) therefore runs on a worker thread, and OPEN is
 * answered when the digest is ready.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/mutex.h>
#include <nuttx/rpmsg/rpmsg.h>

#include <arch/chip/rk3576_shmem.h>
#include <arch/chip/rk3576_shmem_layout.h>

#include <crypto/sha2.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ny_compute.h"
#include "ny_product.h"
#include "nyamp_protocol.h"

#ifdef CONFIG_NYABULA_CORE_COMPUTE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_COMPUTE_CTRL_PATH     "/dev/rpmsg/linux"
#define NY_COMPUTE_ENDPOINT_NAME "rpmsg-raw"
#define NY_COMPUTE_ENDPOINT_PATH "/dev/rpmsg-rpmsg-raw"
#define NY_COMPUTE_OPEN_RETRIES  50
#define NY_COMPUTE_OPEN_DELAY_US 100000

#define NY_COMPUTE_TASK_PRIORITY 100
#define NY_COMPUTE_TASK_STACK    16384
#define NY_COMPUTE_HASH_STACK    8192

/* How long the task sleeps in poll().  It bounds the latency of a stop
 * request and of the link probe, nothing else: a frame wakes it at once.
 */

#define NY_COMPUTE_POLL_MS 200

/* An idle link is probed with a HEALTH query, and is reported down when
 * nothing at all has arrived for the longer interval.  The margin is wide
 * because the daemon answers nothing while a synchronous model load blocks
 * its loop, and that can take several probe periods.
 */

#define NY_COMPUTE_PROBE_MS        3000
#define NY_COMPUTE_LINK_TIMEOUT_MS 20000
#define NY_COMPUTE_RETRY_MS        2000

#define NY_COMPUTE_SEND_RETRIES    100
#define NY_COMPUTE_SEND_DELAY_US   10000

#define NY_COMPUTE_HEALTH_OPCODE   1
#define NY_COMPUTE_HEALTH_SIZE     12

/* A pull is one blob at a time; a few slots cover a requester that lost its
 * CLOSE and the diagnostic running beside a model load.
 */

#define NY_COMPUTE_MAX_BLOBS   4
#define NY_COMPUTE_MAX_PORTS   4
#define NY_COMPUTE_PORT_DEPTH  16

#define NY_COMPUTE_HASH_CHUNK  65536
#define NY_COMPUTE_BENCH_BLOCK 4096
#define NY_COMPUTE_SHA256_HEX  64

/* Root, separator, the longest legal name, the longest suffix, NUL. */

#define NY_COMPUTE_PATH_MAX \
  (sizeof(NY_COMPUTE_BLOB_ROOT) + 1 + NYAMP_BLOB_MAX_NAME + 8)

#define NY_COMPUTE_DIGEST_SUFFIX ".sha256"

/* A blob path plus the digest suffix, and a directory plus one entry name. */

#define NY_COMPUTE_SIDECAR_MAX \
  (NY_COMPUTE_PATH_MAX + sizeof(NY_COMPUTE_DIGEST_SUFFIX))
#define NY_COMPUTE_CHILD_MAX (NY_COMPUTE_PATH_MAX + NAME_MAX + 2)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_compute_blob_s
{
  uint32_t id; /* Zero marks a free slot. */
  int fd;
  uint64_t size;
  char name[NY_COMPUTE_NAME_MAX + 1];
};

/* The one OPEN whose digest is still being computed. */

struct ny_compute_hash_s
{
  bool active;
  bool abort;
  struct nyamp_header_s request;
  uint32_t generation;
  char name[NY_COMPUTE_NAME_MAX + 1];
  char path[NY_COMPUTE_PATH_MAX];
  uint64_t size;
  uint64_t mtime;
  uint64_t done;
};

struct ny_compute_frame_s
{
  uint16_t size;
  uint8_t data[NYAMP_RPMSG_MTU];
};

struct ny_compute_port_s
{
  pid_t owner;
  int fd;              /* Write side, opened by the owner task. */
  uint64_t request_id; /* Frames carrying this id are delivered here. */
  sem_t ready;
  unsigned int head;
  unsigned int count;
  struct ny_compute_frame_s queue[NY_COMPUTE_PORT_DEPTH];
};

struct ny_compute_s
{
  mutex_t lock;   /* Everything below except the write path. */
  mutex_t txlock; /* One frame at a time on the task's descriptor. */

  bool running;
  bool stopping;
  int fd;

  uint32_t generation;
  uint32_t capabilities;
  uint64_t last_rx_ms;
  uint64_t last_probe_ms;
  uint64_t probe_id;

  struct ny_compute_blob_s blobs[NY_COMPUTE_MAX_BLOBS];
  uint32_t next_blob_id;

  struct ny_compute_hash_s hash;
  sem_t hash_wake;
  pthread_t hash_thread;
  bool hash_thread_live;

  struct ny_compute_port_s *ports[NY_COMPUTE_MAX_PORTS];

  /* What the panel shows. */

  char active_name[NY_COMPUTE_NAME_MAX + 1];
  uint64_t active_offset;
  uint64_t active_size;
  uint32_t bytes_per_sec;
  uint64_t rate_start_ms;
  uint64_t rate_bytes;

  uint32_t generation_changes;
  uint32_t dropped_frames;
  int last_error;
  char last_error_text[NY_COMPUTE_ERROR_MAX];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t ny_compute_now_ms(void);
static uint32_t ny_compute_get_le32(const uint8_t *source);
static void ny_compute_error(int code, const char *text);
static int ny_compute_send(const uint8_t *wire, size_t size);
static int ny_compute_respond(const struct nyamp_header_s *request,
                              int32_t status, const uint8_t *body,
                              size_t body_size);
static void ny_compute_blob_release(struct ny_compute_blob_s *blob);
static void ny_compute_drop_blobs(void);
static void ny_compute_set_generation(uint32_t generation);
static struct ny_compute_blob_s *ny_compute_blob_find(uint32_t id);
static struct ny_compute_blob_s *ny_compute_blob_alloc(void);
static int ny_compute_blob_path(char *path, size_t size, const char *name,
                                size_t name_length);
static bool ny_compute_has_suffix(const char *name, size_t length,
                                  const char *suffix);
static int ny_compute_digest_load(const char *path, uint64_t file_mtime,
                                  uint8_t *digest);
static void ny_compute_digest_save(const char *path, const uint8_t *digest);
static int ny_compute_window(const struct nyamp_buffer_s *buffer,
                             uint32_t generation, uint8_t **window);
static int ny_compute_blob_publish(const struct nyamp_header_s *request,
                                   const char *name, const char *path,
                                   uint64_t size, uint64_t mtime,
                                   const uint8_t *digest);
static void ny_compute_blob_open(const struct nyamp_header_s *request,
                                 const uint8_t *payload);
static void ny_compute_blob_read(const struct nyamp_header_s *request,
                                 const uint8_t *payload);
static void ny_compute_blob_close(const struct nyamp_header_s *request,
                                  const uint8_t *payload);
static void ny_compute_blob_list(const struct nyamp_header_s *request,
                                 const uint8_t *payload);
static void ny_compute_blob_bench(const struct nyamp_header_s *request,
                                  const uint8_t *payload);
static void ny_compute_blob_request(const struct nyamp_header_s *request,
                                    const uint8_t *payload);
static void ny_compute_deliver(const struct nyamp_header_s *header,
                               const uint8_t *wire, size_t size);
static void ny_compute_health_response(const struct nyamp_header_s *header,
                                       const uint8_t *payload);
static void ny_compute_frame(const uint8_t *wire, size_t size);
static void ny_compute_probe(void);
static void ny_compute_link_lost(void);
static void *ny_compute_hash_worker(void *arg);
static int ny_compute_task(int argc, char **argv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ny_compute_s g_compute = {
  .lock = NXMUTEX_INITIALIZER,
  .txlock = NXMUTEX_INITIALIZER,
  .fd = -1,
};

/* Only the receive task touches this, so it can be static instead of living
 * on a stack that also has to hold two RPMsg frames and a path.
 */

static uint8_t g_compute_block[NY_COMPUTE_BENCH_BLOCK];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t ny_compute_now_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t ny_compute_get_le32(const uint8_t *source)
{
  return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
         ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

/****************************************************************************
 * Name: ny_compute_error
 *
 * Description:
 *   Remember the most recent failure for compute.status.  The compute domain
 *   has no console and this task has no user, so without this a refused pull
 *   would be visible nowhere.
 *
 ****************************************************************************/

static void ny_compute_error(int code, const char *text)
{
  nxmutex_lock(&g_compute.lock);
  g_compute.last_error = code;
  strlcpy(g_compute.last_error_text, text, sizeof(g_compute.last_error_text));
  nxmutex_unlock(&g_compute.lock);
}

/****************************************************************************
 * Name: ny_compute_send
 *
 * Description:
 *   Write one frame on the task's descriptor.  The descriptor is
 *   non-blocking so the receive loop can never hang in write(); EAGAIN means
 *   the endpoint is not bound yet or the transmit buffers are momentarily
 *   exhausted, and both clear by themselves.
 *
 ****************************************************************************/

static int ny_compute_send(const uint8_t *wire, size_t size)
{
  ssize_t written = -1;
  int attempt;
  int ret;

  ret = nxmutex_lock(&g_compute.txlock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_compute.fd < 0)
    {
      nxmutex_unlock(&g_compute.txlock);
      return -ENOTCONN;
    }

  for (attempt = 0; attempt < NY_COMPUTE_SEND_RETRIES; attempt++)
    {
      written = write(g_compute.fd, wire, size);
      if (written >= 0 || (errno != EAGAIN && errno != EINTR))
        {
          break;
        }

      usleep(NY_COMPUTE_SEND_DELAY_US);
    }

  ret = written == (ssize_t)size ? 0 : written < 0 ? -errno : -EIO;
  nxmutex_unlock(&g_compute.txlock);
  return ret;
}

static int ny_compute_respond(const struct nyamp_header_s *request,
                              int32_t status, const uint8_t *body,
                              size_t body_size)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  uint8_t *destination;
  size_t capacity;
  struct nyamp_header_s header;

  if (nyamp_status_encode(wire + NYAMP_WIRE_HEADER_SIZE, NYAMP_INLINE_MAX,
                          status, &destination, &capacity) != NYAMP_OK ||
      body_size > capacity)
    {
      return -EMSGSIZE;
    }

  if (body_size != 0)
    {
      memcpy(destination, body, body_size);
    }

  /* The generation echoed is the request's: it is the compute domain's, the
   * only generation this link has, and the requester checks it.
   */

  header = *request;
  header.flags = NYAMP_FLAG_RESPONSE | (status == 0 ? 0 : NYAMP_FLAG_ERROR);
  header.payload_size = (uint32_t)(NYAMP_STATUS_SIZE + body_size);
  if (nyamp_header_encode(wire, sizeof(wire), &header) != NYAMP_OK)
    {
      return -EINVAL;
    }

  return ny_compute_send(wire, NYAMP_WIRE_HEADER_SIZE + header.payload_size);
}

static void ny_compute_blob_release(struct ny_compute_blob_s *blob)
{
  if (blob->id != 0)
    {
      close(blob->fd);
      blob->id = 0;
      blob->fd = -1;
    }
}

/****************************************************************************
 * Name: ny_compute_drop_blobs
 *
 * Description:
 *   Close every blob and abandon a digest in progress.  Called with the lock
 *   held whenever the compute domain that held them is gone.
 *
 ****************************************************************************/

static void ny_compute_drop_blobs(void)
{
  int index;

  for (index = 0; index < NY_COMPUTE_MAX_BLOBS; index++)
    {
      ny_compute_blob_release(&g_compute.blobs[index]);
    }

  if (g_compute.hash.active)
    {
      g_compute.hash.abort = true;
    }

  g_compute.active_name[0] = '\0';
  g_compute.active_offset = 0;
  g_compute.active_size = 0;
  g_compute.bytes_per_sec = 0;
}

/****************************************************************************
 * Name: ny_compute_set_generation
 *
 * Description:
 *   Adopt the compute domain's generation.  A different value means its
 *   daemon restarted: its blob ids, its leases and its windows all belonged
 *   to a process that no longer exists.  Called with the lock held.
 *
 ****************************************************************************/

static void ny_compute_set_generation(uint32_t generation)
{
  if (generation == 0 || generation == g_compute.generation)
    {
      return;
    }

  if (g_compute.generation != 0)
    {
      g_compute.generation_changes++;
    }

  ny_compute_drop_blobs();
  g_compute.generation = generation;
  g_compute.capabilities = 0;
}

static struct ny_compute_blob_s *ny_compute_blob_find(uint32_t id)
{
  int index;

  for (index = 0; id != 0 && index < NY_COMPUTE_MAX_BLOBS; index++)
    {
      if (g_compute.blobs[index].id == id)
        {
          return &g_compute.blobs[index];
        }
    }

  return NULL;
}

static struct ny_compute_blob_s *ny_compute_blob_alloc(void)
{
  int index;

  for (index = 0; index < NY_COMPUTE_MAX_BLOBS; index++)
    {
      if (g_compute.blobs[index].id == 0)
        {
          return &g_compute.blobs[index];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: ny_compute_blob_path
 *
 * Description:
 *   Turn a blob name into a path under the blob root.
 *
 *   The codec has already refused "..", ".", empty components, a leading
 *   '/', backslashes and control characters, so the name cannot climb out of
 *   the root lexically.  What is left is a link planted inside the root:
 *   every component is checked with lstat() and a symbolic link anywhere on
 *   the way is refused.  FAT has none, but the root is a path and not a
 *   promise about which file system is mounted there.
 *
 ****************************************************************************/

static int ny_compute_blob_path(char *path, size_t size, const char *name,
                                size_t name_length)
{
  struct stat status;
  size_t root = sizeof(NY_COMPUTE_BLOB_ROOT) - 1;
  size_t index;

  if (nyamp_blob_name_check(name, name_length) != NYAMP_OK ||
      root + 1 + name_length + 1 > size)
    {
      return -EINVAL;
    }

  memcpy(path, NY_COMPUTE_BLOB_ROOT, root);
  path[root] = '/';
  memcpy(path + root + 1, name, name_length);
  path[root + 1 + name_length] = '\0';

  for (index = root + 1; index <= root + 1 + name_length; index++)
    {
      char saved = path[index];

      if (saved != '/' && saved != '\0')
        {
          continue;
        }

      path[index] = '\0';
      if (lstat(path, &status) < 0)
        {
          int error = errno;

          path[index] = saved;
          return -error;
        }

      path[index] = saved;
      if (S_ISLNK(status.st_mode))
        {
          return -ELOOP;
        }
    }

  return 0;
}

static bool ny_compute_has_suffix(const char *name, size_t length,
                                  const char *suffix)
{
  size_t suffix_length = strlen(suffix);

  return length >= suffix_length &&
         memcmp(name + length - suffix_length, suffix, suffix_length) == 0;
}

/****************************************************************************
 * Name: ny_compute_digest_load
 *
 * Description:
 *   Read "<file>.sha256", the digest the model uploader leaves beside every
 *   file it has verified.  It is believed only when it is at least as new as
 *   the file: a model replaced behind the uploader's back (adb push, a
 *   copy from the shell) keeps its old sidecar, and handing that digest out
 *   would make the compute domain reject a perfectly good file forever.
 *
 ****************************************************************************/

static int ny_compute_digest_load(const char *path, uint64_t file_mtime,
                                  uint8_t *digest)
{
  char sidecar[NY_COMPUTE_SIDECAR_MAX];
  char text[NY_COMPUTE_SHA256_HEX];
  struct stat status;
  ssize_t count;
  int index;
  int fd;

  snprintf(sidecar, sizeof(sidecar), "%s%s", path, NY_COMPUTE_DIGEST_SUFFIX);
  if (stat(sidecar, &status) < 0 || !S_ISREG(status.st_mode) ||
      (uint64_t)status.st_mtime < file_mtime)
    {
      return -ENOENT;
    }

  fd = open(sidecar, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  count = read(fd, text, sizeof(text));
  close(fd);
  if (count != (ssize_t)sizeof(text))
    {
      return -EINVAL;
    }

  for (index = 0; index < NYAMP_BLOB_SHA256_SIZE; index++)
    {
      int value = 0;
      int digit;

      for (digit = 0; digit < 2; digit++)
        {
          char c = text[index * 2 + digit];

          value <<= 4;
          if (c >= '0' && c <= '9')
            {
              value |= c - '0';
            }
          else if (c >= 'a' && c <= 'f')
            {
              value |= c - 'a' + 10;
            }
          else if (c >= 'A' && c <= 'F')
            {
              value |= c - 'A' + 10;
            }
          else
            {
              return -EINVAL;
            }
        }

      digest[index] = (uint8_t)value;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_compute_digest_save
 *
 * Description:
 *   Cache a digest this side had to compute, in the uploader's format
 *   (exactly 64 lowercase hex characters), so the next OPEN of the same file
 *   is answered at once.  Best effort: a read-only volume only costs time.
 *
 ****************************************************************************/

static void ny_compute_digest_save(const char *path, const uint8_t *digest)
{
  static const char digits[] = "0123456789abcdef";
  char sidecar[NY_COMPUTE_SIDECAR_MAX];
  char text[NY_COMPUTE_SHA256_HEX];
  int index;
  int fd;

  for (index = 0; index < NYAMP_BLOB_SHA256_SIZE; index++)
    {
      text[index * 2] = digits[digest[index] >> 4];
      text[index * 2 + 1] = digits[digest[index] & 0x0f];
    }

  snprintf(sidecar, sizeof(sidecar), "%s%s", path, NY_COMPUTE_DIGEST_SUFFIX);
  fd = open(sidecar, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    {
      return;
    }

  if (write(fd, text, sizeof(text)) != (ssize_t)sizeof(text))
    {
      close(fd);
      unlink(sidecar);
      return;
    }

  close(fd);
}

/****************************************************************************
 * Name: ny_compute_window
 *
 * Description:
 *   Validate a granted window and return its address.
 *
 *   The compute domain is the only allocator of the arena, so this side does
 *   not second-guess WHERE inside the data area a window sits.  It enforces
 *   the two things that would corrupt the link if they were wrong: the first
 *   4 KiB are the arena header both sides validate against and must never be
 *   written, and nothing may be written past the end of the region.  The
 *   arithmetic is done in a way that cannot wrap.
 *
 ****************************************************************************/

static int ny_compute_window(const struct nyamp_buffer_s *buffer,
                             uint32_t generation, uint8_t **window)
{
  if ((buffer->flags & NYAMP_BUFFER_IN_SHMEM) == 0 || buffer->lease == 0 ||
      buffer->generation != generation || buffer->length != 0 ||
      buffer->capacity == 0)
    {
      return -EINVAL;
    }

  if (buffer->offset < NYAMP_SLOT_HEADER + NYAMP_SLOT_HEADER_SIZE ||
      buffer->offset >= NYAMP_SHMEM_SIZE ||
      buffer->capacity > NYAMP_SHMEM_SIZE - buffer->offset)
    {
      return -ERANGE;
    }

  /* The header may not have been validated yet when the peer was slower to
   * claim the region than this domain was to boot.
   */

  if (!rk3576_shmem_ready() && rk3576_shmem_initialize() < 0)
    {
      return -ENODEV;
    }

  *window = (uint8_t *)rk3576_shmem_base() + buffer->offset;
  return 0;
}

/****************************************************************************
 * Name: ny_compute_blob_publish
 *
 * Description:
 *   Open the file, give it a blob id and answer the OPEN.  Shared by the
 *   immediate path (cached digest) and the hash worker.  Takes the lock.
 *
 ****************************************************************************/

static int ny_compute_blob_publish(const struct nyamp_header_s *request,
                                   const char *name, const char *path,
                                   uint64_t size, uint64_t mtime,
                                   const uint8_t *digest)
{
  struct ny_compute_blob_s *blob;
  struct nyamp_blob_info_s info;
  uint8_t body[NYAMP_BLOB_INFO_SIZE];
  size_t body_size = 0;
  int fd;

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  nxmutex_lock(&g_compute.lock);
  blob = request->generation == g_compute.generation ? ny_compute_blob_alloc()
                                                     : NULL;
  if (blob == NULL)
    {
      nxmutex_unlock(&g_compute.lock);
      close(fd);
      return -EBUSY;
    }

  if (++g_compute.next_blob_id == 0)
    {
      g_compute.next_blob_id = 1;
    }

  blob->id = g_compute.next_blob_id;
  blob->fd = fd;
  blob->size = size;
  strlcpy(blob->name, name, sizeof(blob->name));

  strlcpy(g_compute.active_name, name, sizeof(g_compute.active_name));
  g_compute.active_offset = 0;
  g_compute.active_size = size;
  g_compute.bytes_per_sec = 0;
  g_compute.rate_start_ms = ny_compute_now_ms();
  g_compute.rate_bytes = 0;

  memset(&info, 0, sizeof(info));
  info.blob_id = blob->id;
  info.size = size;
  info.mtime = mtime;
  memcpy(info.sha256, digest, NYAMP_BLOB_SHA256_SIZE);
  nxmutex_unlock(&g_compute.lock);

  nyamp_blob_info_encode(body, sizeof(body), &body_size, &info);
  return ny_compute_respond(request, NYAMP_MODEL_OK, body, body_size);
}

static void ny_compute_blob_open(const struct nyamp_header_s *request,
                                 const uint8_t *payload)
{
  uint8_t digest[NYAMP_BLOB_SHA256_SIZE];
  char path[NY_COMPUTE_PATH_MAX];
  char name[NY_COMPUTE_NAME_MAX + 1];
  const char *wire_name;
  size_t name_length;
  struct stat status;
  uint32_t flags;
  int ret;

  if (nyamp_blob_open_decode(&flags, &wire_name, &name_length, payload,
                             request->payload_size) != NYAMP_OK)
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  memcpy(name, wire_name, name_length);
  name[name_length] = '\0';

  /* An upload in progress is not a model yet, whatever its name says. */

  if (ny_compute_has_suffix(name, name_length, ".part") ||
      ny_compute_has_suffix(name, name_length, ".part.json"))
    {
      ny_compute_respond(request, NYAMP_MODEL_NOT_READY, NULL, 0);
      return;
    }

  ret = ny_compute_blob_path(path, sizeof(path), name, name_length);
  if (ret < 0 || stat(path, &status) < 0)
    {
      ny_compute_error(ret < 0 ? ret : -errno, name);
      ny_compute_respond(request,
                         ret == -ELOOP || ret == -EINVAL
                             ? NYAMP_MODEL_INVALID
                             : NYAMP_MODEL_NOT_READY,
                         NULL, 0);
      return;
    }

  /* UNSUPPORTED is the explicit "this is a directory, list it" answer. */

  if (S_ISDIR(status.st_mode))
    {
      ny_compute_respond(request, NYAMP_MODEL_UNSUPPORTED, NULL, 0);
      return;
    }

  if (!S_ISREG(status.st_mode))
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  if (ny_compute_digest_load(path, (uint64_t)status.st_mtime, digest) == 0)
    {
      ret = ny_compute_blob_publish(request, name, path,
                                    (uint64_t)status.st_size,
                                    (uint64_t)status.st_mtime, digest);
      if (ret < 0)
        {
          ny_compute_error(ret, name);
          ny_compute_respond(request,
                             ret == -EBUSY ? NYAMP_MODEL_BUSY
                                           : NYAMP_MODEL_BACKEND_ERROR,
                             NULL, 0);
        }

      return;
    }

  /* No usable digest: hand the file to the worker and return to the receive
   * loop.  The response is sent when the digest exists.
   */

  nxmutex_lock(&g_compute.lock);
  if (!g_compute.hash_thread_live)
    {
      /* Queueing for a worker that does not exist would wedge every later
       * OPEN behind a job nobody will finish.
       */

      nxmutex_unlock(&g_compute.lock);
      ny_compute_respond(request, NYAMP_MODEL_BACKEND_ERROR, NULL, 0);
      return;
    }

  if (g_compute.hash.active || ny_compute_blob_alloc() == NULL)
    {
      nxmutex_unlock(&g_compute.lock);
      ny_compute_respond(request, NYAMP_MODEL_BUSY, NULL, 0);
      return;
    }

  memset(&g_compute.hash, 0, sizeof(g_compute.hash));
  g_compute.hash.active = true;
  g_compute.hash.request = *request;
  g_compute.hash.generation = request->generation;
  g_compute.hash.size = (uint64_t)status.st_size;
  g_compute.hash.mtime = (uint64_t)status.st_mtime;
  strlcpy(g_compute.hash.name, name, sizeof(g_compute.hash.name));
  strlcpy(g_compute.hash.path, path, sizeof(g_compute.hash.path));

  strlcpy(g_compute.active_name, name, sizeof(g_compute.active_name));
  g_compute.active_offset = 0;
  g_compute.active_size = (uint64_t)status.st_size;
  g_compute.bytes_per_sec = 0;
  nxmutex_unlock(&g_compute.lock);

  sem_post(&g_compute.hash_wake);
}

/****************************************************************************
 * Name: ny_compute_blob_read
 *
 * Description:
 *   Fill the granted window from the file.  By default pread() writes
 *   straight into the shared region, which is ordinary DRAM mapped
 *   non-cacheable, so no bounce buffer and no extra copy are involved.
 *
 ****************************************************************************/

static void ny_compute_blob_read(const struct nyamp_header_s *request,
                                 const uint8_t *payload)
{
  struct ny_compute_blob_s *blob;
  struct nyamp_blob_read_s read_request;
  uint8_t body[NYAMP_BLOB_READ_SIZE];
  size_t body_size = 0;
  uint8_t *window;
  uint64_t offset;
  uint64_t now;
  size_t filled = 0;
  size_t want;
  int ret;

  if (nyamp_blob_read_decode(&read_request, payload, request->payload_size) !=
          NYAMP_OK ||
      read_request.flags != 0)
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  ret = ny_compute_window(&read_request.buffer, request->generation, &window);
  if (ret < 0)
    {
      ny_compute_error(ret, "blob window refused");
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  /* Only this task closes blobs while it is running, so the slot stays valid
   * after the lock is dropped for the (long) read below.
   */

  nxmutex_lock(&g_compute.lock);
  blob = ny_compute_blob_find(read_request.blob_id);
  nxmutex_unlock(&g_compute.lock);

  offset = read_request.file_offset;
  if (blob == NULL || offset > blob->size)
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  want = read_request.buffer.capacity;
  if ((uint64_t)want > blob->size - offset)
    {
      want = (size_t)(blob->size - offset);
    }

  while (filled < want)
    {
      ssize_t got;

#ifdef CONFIG_NYABULA_CORE_COMPUTE_BOUNCE
      size_t chunk = want - filled;

      if (chunk > sizeof(g_compute_block))
        {
          chunk = sizeof(g_compute_block);
        }

      got = pread(blob->fd, g_compute_block, chunk, (off_t)(offset + filled));
      if (got > 0)
        {
          memcpy(window + filled, g_compute_block, (size_t)got);
        }
#else
      got = pread(blob->fd, window + filled, want - filled,
                  (off_t)(offset + filled));
#endif

      if (got < 0 && errno == EINTR)
        {
          continue;
        }

      if (got < 0)
        {
          ny_compute_error(-errno, blob->name);
          ny_compute_respond(request, NYAMP_MODEL_BACKEND_ERROR, NULL, 0);
          return;
        }

      if (got == 0)
        {
          break;
        }

      filled += (size_t)got;
    }

  /* Nothing read short of the end means the file shrank under the transfer;
   * a zero-length success would make the requester spin.
   */

  if (filled == 0 && offset < blob->size)
    {
      ny_compute_error(-EIO, blob->name);
      ny_compute_respond(request, NYAMP_MODEL_BACKEND_ERROR, NULL, 0);
      return;
    }

  read_request.buffer.length = (uint32_t)filled;
  if (offset + filled >= blob->size)
    {
      read_request.flags = NYAMP_BLOB_READ_EOF;
      read_request.buffer.flags |= NYAMP_BUFFER_LAST;
    }

  now = ny_compute_now_ms();
  nxmutex_lock(&g_compute.lock);
  strlcpy(g_compute.active_name, blob->name, sizeof(g_compute.active_name));
  g_compute.active_offset = offset + filled;
  g_compute.active_size = blob->size;
  g_compute.rate_bytes += filled;
  if (now - g_compute.rate_start_ms >= 1000)
    {
      g_compute.bytes_per_sec = (uint32_t)(g_compute.rate_bytes * 1000 /
                                           (now - g_compute.rate_start_ms));
      g_compute.rate_start_ms = now;
      g_compute.rate_bytes = 0;
    }

  nxmutex_unlock(&g_compute.lock);

  if (nyamp_blob_read_encode(body, sizeof(body), &body_size, &read_request) !=
      NYAMP_OK)
    {
      ny_compute_respond(request, NYAMP_MODEL_BACKEND_ERROR, NULL, 0);
      return;
    }

  ny_compute_respond(request, NYAMP_MODEL_OK, body, body_size);
}

static void ny_compute_blob_close(const struct nyamp_header_s *request,
                                  const uint8_t *payload)
{
  struct ny_compute_blob_s *blob;
  uint32_t blob_id;

  if (nyamp_blob_close_decode(&blob_id, payload, request->payload_size) !=
      NYAMP_OK)
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  nxmutex_lock(&g_compute.lock);
  blob = ny_compute_blob_find(blob_id);
  if (blob != NULL)
    {
      ny_compute_blob_release(blob);
      g_compute.bytes_per_sec = 0;
    }

  nxmutex_unlock(&g_compute.lock);
  ny_compute_respond(
      request, blob != NULL ? NYAMP_MODEL_OK : NYAMP_MODEL_INVALID, NULL, 0);
}

/****************************************************************************
 * Name: ny_compute_blob_list
 *
 * Description:
 *   One page of a directory.  The cursor is the raw readdir index of the
 *   first entry that did not fit, so a page boundary neither skips nor
 *   repeats an entry as long as the directory is not modified meanwhile.
 *   The uploader's bookkeeping files are not models and are left out.
 *
 ****************************************************************************/

static void ny_compute_blob_list(const struct nyamp_header_s *request,
                                 const uint8_t *payload)
{
  uint8_t body[NYAMP_BLOB_LIST_BODY_MAX];
  char path[NY_COMPUTE_PATH_MAX];
  char child[NY_COMPUTE_CHILD_MAX];
  struct nyamp_blob_entry_s entry;
  const char *prefix;
  size_t prefix_length;
  size_t body_size = 0;
  struct dirent *item;
  struct stat status;
  uint32_t cursor;
  uint32_t index = 0;
  uint32_t next = 0;
  DIR *directory;
  int ret = 0;

  if (nyamp_blob_list_decode(&cursor, &prefix, &prefix_length, payload,
                             request->payload_size) != NYAMP_OK)
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  if (prefix_length == 0)
    {
      strlcpy(path, NY_COMPUTE_BLOB_ROOT, sizeof(path));
    }
  else
    {
      ret = ny_compute_blob_path(path, sizeof(path), prefix, prefix_length);
    }

  directory = ret < 0 ? NULL : opendir(path);
  if (directory == NULL)
    {
      ny_compute_respond(request,
                         ret == -ELOOP || ret == -EINVAL || errno == ENOTDIR
                             ? NYAMP_MODEL_INVALID
                             : NYAMP_MODEL_NOT_READY,
                         NULL, 0);
      return;
    }

  nyamp_blob_list_body_begin(body, sizeof(body), &body_size);
  while ((item = readdir(directory)) != NULL)
    {
      size_t length = strlen(item->d_name);

      if (index++ < cursor)
        {
          continue;
        }

      if (item->d_name[0] == '.' ||
          ny_compute_has_suffix(item->d_name, length, ".part") ||
          ny_compute_has_suffix(item->d_name, length, ".part.json") ||
          ny_compute_has_suffix(item->d_name, length,
                                NY_COMPUTE_DIGEST_SUFFIX) ||
          nyamp_blob_name_check(item->d_name, length) != NYAMP_OK ||
          strlen(path) + 1 + length + 1 > sizeof(child))
        {
          continue;
        }

      snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
      if (lstat(child, &status) < 0 ||
          (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)))
        {
          continue;
        }

      entry.size = S_ISREG(status.st_mode) ? (uint64_t)status.st_size : 0;
      entry.flags = S_ISDIR(status.st_mode) ? NYAMP_BLOB_ENTRY_DIRECTORY : 0;
      entry.name = item->d_name;
      entry.name_length = (uint16_t)length;
      if (nyamp_blob_list_body_append(body, sizeof(body), &body_size,
                                      &entry) == NYAMP_EMSGSIZE)
        {
          /* Resume at this entry: it was read but not reported. */

          next = index - 1;
          break;
        }
    }

  closedir(directory);
  nyamp_blob_list_body_finish(body, body_size, next);
  ny_compute_respond(request, NYAMP_MODEL_OK, body, body_size);
}

/****************************************************************************
 * Name: ny_compute_blob_bench
 *
 * Description:
 *   The two measurements the window size has to be tuned against.  ECHO is
 *   the bare message round trip.  FILL writes a position-dependent pattern
 *   into the granted window a cached block at a time, which is how fast this
 *   CPU can feed the non-cacheable region with the eMMC out of the picture.
 *
 ****************************************************************************/

static void ny_compute_blob_bench(const struct nyamp_header_s *request,
                                  const uint8_t *payload)
{
  struct nyamp_blob_bench_s bench;
  uint8_t body[NYAMP_BLOB_BENCH_FILL_SIZE];
  size_t body_size = 0;
  uint8_t *window;
  uint32_t block;

  if (nyamp_blob_bench_decode(&bench, payload, request->payload_size) !=
      NYAMP_OK)
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  if (bench.mode == NYAMP_BLOB_BENCH_FILL)
    {
      if ((bench.buffer.capacity % NY_COMPUTE_BENCH_BLOCK) != 0 ||
          ny_compute_window(&bench.buffer, request->generation, &window) < 0)
        {
          ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
          return;
        }

      for (block = 0; block < bench.buffer.capacity / NY_COMPUTE_BENCH_BLOCK;
           block++)
        {
          nyamp_blob_bench_pattern(g_compute_block, NY_COMPUTE_BENCH_BLOCK / 4,
                                   bench.seed,
                                   block * (NY_COMPUTE_BENCH_BLOCK / 4));
          memcpy(window + (size_t)block * NY_COMPUTE_BENCH_BLOCK,
                 g_compute_block, NY_COMPUTE_BENCH_BLOCK);
        }

      bench.buffer.length = bench.buffer.capacity;
    }

  if (nyamp_blob_bench_encode(body, sizeof(body), &body_size, &bench) !=
      NYAMP_OK)
    {
      ny_compute_respond(request, NYAMP_MODEL_BACKEND_ERROR, NULL, 0);
      return;
    }

  ny_compute_respond(request, NYAMP_MODEL_OK, body, body_size);
}

static void ny_compute_blob_request(const struct nyamp_header_s *request,
                                    const uint8_t *payload)
{
  bool stale;

  /* A BLOB request can only come from the compute domain; an id without the
   * origin bit is a local frame that was somehow looped back.
   */

  if ((request->request_id & NYAMP_REQUEST_ID_COMPUTE) == 0)
    {
      ny_compute_respond(request, NYAMP_MODEL_INVALID, NULL, 0);
      return;
    }

  /* The first request of a daemon whose READY was consumed before this task
   * started teaches the generation.  After that only an exact match is
   * served: READY always precedes a new daemon's requests on this ordered
   * link, so a mismatch is a stale requester, and the probe below resolves
   * the rare case where it is this side that is behind.
   */

  nxmutex_lock(&g_compute.lock);
  if (g_compute.generation == 0)
    {
      ny_compute_set_generation(request->generation);
    }

  stale =
      request->generation == 0 || request->generation != g_compute.generation;
  if (stale)
    {
      g_compute.last_probe_ms = 0;
    }

  nxmutex_unlock(&g_compute.lock);

  if (stale)
    {
      ny_compute_respond(request, NYAMP_MODEL_STALE_GENERATION, NULL, 0);
      return;
    }

  switch (request->opcode)
    {
      case NYAMP_BLOB_OPEN:
        ny_compute_blob_open(request, payload);
        break;

      case NYAMP_BLOB_READ:
        ny_compute_blob_read(request, payload);
        break;

      case NYAMP_BLOB_CLOSE:
        ny_compute_blob_close(request, payload);
        break;

      case NYAMP_BLOB_LIST:
        ny_compute_blob_list(request, payload);
        break;

      case NYAMP_BLOB_BENCH:
        ny_compute_blob_bench(request, payload);
        break;

      default:
        ny_compute_respond(request, NYAMP_MODEL_UNSUPPORTED, NULL, 0);
        break;
    }
}

/****************************************************************************
 * Name: ny_compute_deliver
 *
 * Description:
 *   Queue a response or an event for the port that sent the request.  A
 *   frame nobody is waiting for is dropped silently; a full queue drops the
 *   frame and counts it, because blocking here would stall every other
 *   requester and the blob service with it.
 *
 ****************************************************************************/

static void ny_compute_deliver(const struct nyamp_header_s *header,
                               const uint8_t *wire, size_t size)
{
  int index;

  nxmutex_lock(&g_compute.lock);
  for (index = 0; index < NY_COMPUTE_MAX_PORTS; index++)
    {
      struct ny_compute_port_s *port = g_compute.ports[index];
      struct ny_compute_frame_s *slot;

      if (port == NULL || port->request_id != header->request_id)
        {
          continue;
        }

      if (port->count >= NY_COMPUTE_PORT_DEPTH)
        {
          g_compute.dropped_frames++;
          continue;
        }

      slot = &port->queue[(port->head + port->count) % NY_COMPUTE_PORT_DEPTH];
      slot->size = (uint16_t)size;
      memcpy(slot->data, wire, size);
      port->count++;
      sem_post(&port->ready);
    }

  nxmutex_unlock(&g_compute.lock);
}

static void ny_compute_health_response(const struct nyamp_header_s *header,
                                       const uint8_t *payload)
{
  if (header->flags != NYAMP_FLAG_RESPONSE ||
      header->payload_size != NY_COMPUTE_HEALTH_SIZE ||
      ny_compute_get_le32(payload) != 0 ||
      ny_compute_get_le32(payload + 4) != header->generation)
    {
      return;
    }

  nxmutex_lock(&g_compute.lock);
  ny_compute_set_generation(header->generation);
  g_compute.capabilities = ny_compute_get_le32(payload + 8);
  nxmutex_unlock(&g_compute.lock);
}

static void ny_compute_frame(const uint8_t *wire, size_t size)
{
  struct nyamp_header_s header;
  const uint8_t *payload = wire + NYAMP_WIRE_HEADER_SIZE;
  uint32_t kind;

  if (nyamp_header_decode(&header, wire, size) != NYAMP_OK ||
      size != NYAMP_WIRE_HEADER_SIZE + header.payload_size)
    {
      nxmutex_lock(&g_compute.lock);
      g_compute.dropped_frames++;
      nxmutex_unlock(&g_compute.lock);
      return;
    }

  nxmutex_lock(&g_compute.lock);
  g_compute.last_rx_ms = ny_compute_now_ms();
  nxmutex_unlock(&g_compute.lock);

  kind = header.flags & NYAMP_FLAG_KIND_MASK;
  switch (kind)
    {
      case NYAMP_FLAG_EVENT:
        if (header.service == NYAMP_SERVICE_HEALTH &&
            header.opcode == NYAMP_HEALTH_READY)
          {
            nxmutex_lock(&g_compute.lock);
            ny_compute_set_generation(header.generation);

            /* Learn the new daemon's capabilities without waiting a period. */

            g_compute.last_probe_ms = 0;
            nxmutex_unlock(&g_compute.lock);
            break;
          }

        ny_compute_deliver(&header, wire, size);
        break;

      case NYAMP_FLAG_RESPONSE:
        if (header.service == NYAMP_SERVICE_HEALTH &&
            header.request_id == g_compute.probe_id)
          {
            ny_compute_health_response(&header, payload);
            break;
          }

        ny_compute_deliver(&header, wire, size);
        break;

      case NYAMP_FLAG_REQUEST:
        if (header.service == NYAMP_SERVICE_BLOB)
          {
            ny_compute_blob_request(&header, payload);
          }
        else
          {
            ny_compute_respond(&header, NYAMP_MODEL_UNSUPPORTED, NULL, 0);
          }

        break;

      case NYAMP_FLAG_CANCEL:

        /* The requester gave up on an OPEN that is still hashing. */

        nxmutex_lock(&g_compute.lock);
        if (header.service == NYAMP_SERVICE_BLOB && g_compute.hash.active &&
            g_compute.hash.request.request_id == header.request_id)
          {
            g_compute.hash.abort = true;
          }

        nxmutex_unlock(&g_compute.lock);
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: ny_compute_probe
 *
 * Description:
 *   Ask for HEALTH when the link has been quiet.  It is how this side learns
 *   the generation when the READY event predates it (an earlier nyampctl run
 *   consumed it), and how it notices a peer that stopped answering.
 *
 ****************************************************************************/

static void ny_compute_probe(void)
{
  uint8_t wire[NYAMP_WIRE_HEADER_SIZE];
  struct nyamp_header_s header;
  uint64_t now = ny_compute_now_ms();
  bool due;

  nxmutex_lock(&g_compute.lock);
  due = now - g_compute.last_probe_ms >= NY_COMPUTE_PROBE_MS &&
        (g_compute.last_probe_ms == 0 || g_compute.capabilities == 0 ||
         now - g_compute.last_rx_ms >= NY_COMPUTE_PROBE_MS);
  if (due)
    {
      g_compute.last_probe_ms = now;
    }

  nxmutex_unlock(&g_compute.lock);
  if (!due)
    {
      return;
    }

  /* Only this task reads or writes probe_id, and the id generator takes the
   * state lock itself, so it is called with the lock released.
   */

  g_compute.probe_id = ny_compute_request_id();

  memset(&header, 0, sizeof(header));
  header.service = NYAMP_SERVICE_HEALTH;
  header.opcode = NY_COMPUTE_HEALTH_OPCODE;
  header.flags = NYAMP_FLAG_REQUEST;
  header.request_id = g_compute.probe_id;
  if (nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK)
    {
      /* A failed probe is not an error worth keeping: before the peer's
       * first message the endpoint has no destination address at all.
       */

      ny_compute_send(wire, sizeof(wire));
    }
}

static void ny_compute_link_lost(void)
{
  nxmutex_lock(&g_compute.txlock);
  if (g_compute.fd >= 0)
    {
      close(g_compute.fd);
      g_compute.fd = -1;
    }

  nxmutex_unlock(&g_compute.txlock);

  nxmutex_lock(&g_compute.lock);
  ny_compute_drop_blobs();
  g_compute.generation = 0;
  g_compute.capabilities = 0;
  nxmutex_unlock(&g_compute.lock);
}

/****************************************************************************
 * Name: ny_compute_hash_worker
 *
 * Description:
 *   Compute the digest of the file a pending OPEN named, then answer that
 *   OPEN.  The abort flag is looked at once per chunk, so a cancel or a new
 *   generation stops a twelve-second hash within a few milliseconds.
 *
 ****************************************************************************/

static void *ny_compute_hash_worker(void *arg)
{
  uint8_t *chunk = malloc(NY_COMPUTE_HASH_CHUNK);

  (void)arg;
  for (;;)
    {
      struct ny_compute_hash_s job;
      uint8_t digest[NYAMP_BLOB_SHA256_SIZE];
      struct stat status;
      SHA2_CTX hash;
      int32_t result = NYAMP_MODEL_OK;
      bool aborted = false;
      bool stopping;
      bool current;
      int fd;

      while (sem_wait(&g_compute.hash_wake) < 0 && errno == EINTR)
        {
        }

      nxmutex_lock(&g_compute.lock);
      stopping = g_compute.stopping;
      job = g_compute.hash;
      nxmutex_unlock(&g_compute.lock);
      if (stopping)
        {
          break;
        }

      if (!job.active)
        {
          continue;
        }

      fd = chunk == NULL ? -1 : open(job.path, O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        {
          result = NYAMP_MODEL_BACKEND_ERROR;
        }
      else
        {
          sha256init(&hash);
          for (;;)
            {
              ssize_t got = read(fd, chunk, NY_COMPUTE_HASH_CHUNK);

              if (got < 0 && errno == EINTR)
                {
                  continue;
                }

              if (got < 0)
                {
                  result = NYAMP_MODEL_BACKEND_ERROR;
                  break;
                }

              if (got == 0)
                {
                  break;
                }

              sha256update(&hash, chunk, (size_t)got);

              nxmutex_lock(&g_compute.lock);
              g_compute.hash.done += (uint64_t)got;
              g_compute.active_offset = g_compute.hash.done;
              aborted = g_compute.hash.abort || g_compute.stopping;
              nxmutex_unlock(&g_compute.lock);
              if (aborted)
                {
                  break;
                }
            }

          close(fd);
          sha256final(digest, &hash);
        }

      /* A file that changed while it was being read has no single digest.
       * Refusing is the honest answer; the requester may simply try again.
       */

      if (result == NYAMP_MODEL_OK && !aborted &&
          (stat(job.path, &status) < 0 ||
           (uint64_t)status.st_size != job.size ||
           (uint64_t)status.st_mtime != job.mtime))
        {
          result = NYAMP_MODEL_BACKEND_ERROR;
        }

      if (result == NYAMP_MODEL_OK && !aborted)
        {
          ny_compute_digest_save(job.path, digest);
        }

      /* Free the slot first: publishing needs a blob slot, and the OPEN that
       * follows a refusal must not be told the worker is still busy.
       */

      nxmutex_lock(&g_compute.lock);
      current = job.generation == g_compute.generation;
      aborted = aborted || g_compute.hash.abort || !current;
      g_compute.hash.active = false;
      nxmutex_unlock(&g_compute.lock);

      if (aborted)
        {
          /* Nobody is waiting under a new generation; after a cancel the
           * answer is only a courtesy the requester will drop.
           */

          if (current)
            {
              ny_compute_respond(&job.request, NYAMP_MODEL_CANCELLED, NULL, 0);
            }

          continue;
        }

      if (result == NYAMP_MODEL_OK)
        {
          int ret = ny_compute_blob_publish(&job.request, job.name, job.path,
                                            job.size, job.mtime, digest);

          if (ret >= 0)
            {
              continue;
            }

          result =
              ret == -EBUSY ? NYAMP_MODEL_BUSY : NYAMP_MODEL_BACKEND_ERROR;
        }

      ny_compute_error(-EIO, job.name);
      ny_compute_respond(&job.request, result, NULL, 0);
    }

  free(chunk);
  return NULL;
}

/****************************************************************************
 * Name: ny_compute_task
 *
 * Description:
 *   The receive task.  It owns the descriptor, so it is also the task the
 *   hash worker is created in: a thread shares its task's descriptors, which
 *   is what lets the worker send the OPEN response itself.
 *
 ****************************************************************************/

static int ny_compute_task(int argc, char **argv)
{
  pthread_attr_t attr;
  uint8_t wire[NYAMP_RPMSG_MTU];
  uint64_t retry_at = 0;
  int index;

  (void)argc;
  (void)argv;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, NY_COMPUTE_HASH_STACK);
  g_compute.hash_thread_live =
      pthread_create(&g_compute.hash_thread, &attr, ny_compute_hash_worker,
                     NULL) == 0;
  pthread_attr_destroy(&attr);
  if (!g_compute.hash_thread_live)
    {
      ny_compute_error(-ENOMEM, "hash worker not started");
    }

  for (;;)
    {
      struct pollfd pollfd;
      bool stopping;
      int ready;

      nxmutex_lock(&g_compute.lock);
      stopping = g_compute.stopping;
      nxmutex_unlock(&g_compute.lock);
      if (stopping)
        {
          break;
        }

      if (g_compute.fd < 0)
        {
          int fd;

          if (ny_compute_now_ms() < retry_at)
            {
              usleep(NY_COMPUTE_POLL_MS * 1000);
              continue;
            }

          fd = ny_compute_endpoint_open(O_RDWR | O_NONBLOCK | O_CLOEXEC);
          if (fd < 0)
            {
              ny_compute_error(fd, "AMP endpoint unavailable");
              retry_at = ny_compute_now_ms() + NY_COMPUTE_RETRY_MS;
              continue;
            }

          nxmutex_lock(&g_compute.txlock);
          g_compute.fd = fd;
          nxmutex_unlock(&g_compute.txlock);
          ny_compute_error(0, "");
        }

      pollfd.fd = g_compute.fd;
      pollfd.events = POLLIN;
      pollfd.revents = 0;
      ready = poll(&pollfd, 1, NY_COMPUTE_POLL_MS);
      if (ready > 0 && (pollfd.revents & POLLIN) != 0)
        {
          for (;;)
            {
              ssize_t size = read(g_compute.fd, wire, sizeof(wire));

              if (size > 0)
                {
                  ny_compute_frame(wire, (size_t)size);
                  continue;
                }

              if (size < 0 && (errno == EAGAIN || errno == EINTR))
                {
                  break;
                }

              /* The endpoint was unbound: the peer's transport went away. */

              ny_compute_error(size < 0 ? -errno : -EPIPE, "AMP link lost");
              ny_compute_link_lost();
              retry_at = ny_compute_now_ms() + NY_COMPUTE_RETRY_MS;
              break;
            }
        }
      else if (ready > 0 && (pollfd.revents & (POLLERR | POLLNVAL)) != 0)
        {
          ny_compute_error(-EPIPE, "AMP link lost");
          ny_compute_link_lost();
          retry_at = ny_compute_now_ms() + NY_COMPUTE_RETRY_MS;
        }

      if (g_compute.fd >= 0)
        {
          ny_compute_probe();
        }
    }

  /* Wake the worker so it sees the stop flag, then take the link down. */

  if (g_compute.hash_thread_live)
    {
      sem_post(&g_compute.hash_wake);
      pthread_join(g_compute.hash_thread, NULL);
      g_compute.hash_thread_live = false;
    }

  sem_destroy(&g_compute.hash_wake);

  ny_compute_link_lost();

  nxmutex_lock(&g_compute.lock);
  for (index = 0; index < NY_COMPUTE_MAX_PORTS; index++)
    {
      /* Wake blocked receivers; their ports stay theirs to close. */

      if (g_compute.ports[index] != NULL)
        {
          sem_post(&g_compute.ports[index]->ready);
        }
    }

  g_compute.hash.active = false;
  g_compute.running = false;
  g_compute.stopping = false;
  nxmutex_unlock(&g_compute.lock);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_compute_endpoint_open(int oflags)
{
  struct rpmsg_endpoint_info info;
  int retry;
  int ctrl;
  int fd = -1;
  int ret;

  ctrl = open(NY_COMPUTE_CTRL_PATH, O_RDWR | O_CLOEXEC);
  if (ctrl < 0)
    {
      return -errno;
    }

  memset(&info, 0, sizeof(info));
  strlcpy(info.name, NY_COMPUTE_ENDPOINT_NAME, sizeof(info.name));
  info.src = RPMSG_ADDR_ANY;
  info.dst = RPMSG_ADDR_ANY;

  /* EEXIST is the normal case after the first caller: the endpoint outlives
   * the descriptor that created it.
   */

  ret = ioctl(ctrl, RPMSG_CREATE_DEV_IOCTL, (unsigned long)&info);
  if (ret < 0 && errno != EEXIST)
    {
      ret = -errno;
      close(ctrl);
      return ret;
    }

  /* The device node appears once the transport has created the endpoint. */

  for (retry = 0; retry < NY_COMPUTE_OPEN_RETRIES; retry++)
    {
      fd = open(NY_COMPUTE_ENDPOINT_PATH, oflags);
      if (fd >= 0)
        {
          break;
        }

      usleep(NY_COMPUTE_OPEN_DELAY_US);
    }

  ret = fd < 0 ? -errno : fd;
  close(ctrl);
  return ret;
}

int ny_compute_start(void)
{
  int ret = nxmutex_lock(&g_compute.lock);

  if (ret < 0)
    {
      return ret;
    }

  if (g_compute.running)
    {
      ret = g_compute.stopping ? -EBUSY : 0;
      nxmutex_unlock(&g_compute.lock);
      return ret;
    }

  sem_init(&g_compute.hash_wake, 0, 0);
  g_compute.stopping = false;
  g_compute.running = true;
  g_compute.last_probe_ms = 0;
  g_compute.last_rx_ms = 0;
  ret = task_create("nycompute", NY_COMPUTE_TASK_PRIORITY,
                    NY_COMPUTE_TASK_STACK, ny_compute_task, NULL);
  if (ret < 0)
    {
      ret = -errno;
      g_compute.running = false;
    }
  else
    {
      ret = 0;
    }

  nxmutex_unlock(&g_compute.lock);
  return ret;
}

int ny_compute_stop(void)
{
  uint64_t deadline;
  int ret = nxmutex_lock(&g_compute.lock);

  if (ret < 0)
    {
      return ret;
    }

  g_compute.stopping = g_compute.running;
  if (g_compute.hash.active)
    {
      g_compute.hash.abort = true;
    }

  nxmutex_unlock(&g_compute.lock);

  deadline = ny_compute_now_ms() + 5000;
  while (ny_compute_running())
    {
      if (ny_compute_now_ms() >= deadline)
        {
          return -ETIMEDOUT;
        }

      usleep(20000);
    }

  return 0;
}

bool ny_compute_running(void)
{
  bool running;

  nxmutex_lock(&g_compute.lock);
  running = g_compute.running;
  nxmutex_unlock(&g_compute.lock);
  return running;
}

int ny_compute_status(struct ny_compute_status_s *status)
{
  uint64_t now = ny_compute_now_ms();
  int index;
  int ret;

  if (status == NULL)
    {
      return -EINVAL;
    }

  memset(status, 0, sizeof(*status));
  ret = nxmutex_lock(&g_compute.lock);
  if (ret < 0)
    {
      return ret;
    }

  status->running = g_compute.running;
  status->generation = g_compute.generation;
  status->capabilities = g_compute.capabilities;
  status->linked = g_compute.running && g_compute.generation != 0 &&
                   g_compute.last_rx_ms != 0 &&
                   now - g_compute.last_rx_ms < NY_COMPUTE_LINK_TIMEOUT_MS;

  status->blob_hashing = g_compute.hash.active;
  status->blob_active = g_compute.hash.active;
  for (index = 0; index < NY_COMPUTE_MAX_BLOBS; index++)
    {
      status->blob_active =
          status->blob_active || g_compute.blobs[index].id != 0;
    }

  strlcpy(status->blob_name, g_compute.active_name, sizeof(status->blob_name));
  status->blob_offset = g_compute.active_offset;
  status->blob_size = g_compute.active_size;
  status->blob_bytes_per_sec =
      status->blob_active ? g_compute.bytes_per_sec : 0;

  status->generation_changes = g_compute.generation_changes;
  status->dropped_frames = g_compute.dropped_frames;
  status->last_error = g_compute.last_error;
  strlcpy(status->last_error_text, g_compute.last_error_text,
          sizeof(status->last_error_text));
  nxmutex_unlock(&g_compute.lock);
  return 0;
}

uint64_t ny_compute_request_id(void)
{
  static uint32_t counter;
  uint32_t sequence;

  /* The counter is what makes two ids from one task in one millisecond
   * differ; the pid keeps tasks apart.  Bit 63 stays clear: that is the
   * compute domain's origin mark.
   */

  nxmutex_lock(&g_compute.lock);
  if (counter == 0)
    {
      counter = (uint32_t)ny_compute_now_ms();
    }

  sequence = ++counter;
  if (sequence == 0)
    {
      sequence = ++counter;
    }

  nxmutex_unlock(&g_compute.lock);
  return (((uint64_t)(uint32_t)getpid() & 0x7fffffffU) << 32) | sequence;
}

int ny_compute_port_open(struct ny_compute_port_s **port)
{
  struct ny_compute_port_s *created;
  int index;
  int slot = -1;

  if (port == NULL)
    {
      return -EINVAL;
    }

  *port = NULL;
  if (!ny_compute_running())
    {
      return -ENOTCONN;
    }

  created = calloc(1, sizeof(*created));
  if (created == NULL)
    {
      return -ENOMEM;
    }

  /* The endpoint exists once the receive task has opened it; until then
   * there is nothing to send through.
   */

  created->fd =
      open(NY_COMPUTE_ENDPOINT_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (created->fd < 0)
    {
      int error = errno;

      free(created);
      return error == ENOENT ? -ENOTCONN : -error;
    }

  created->owner = getpid();
  sem_init(&created->ready, 0, 0);

  nxmutex_lock(&g_compute.lock);
  for (index = 0; index < NY_COMPUTE_MAX_PORTS; index++)
    {
      struct ny_compute_port_s *other = g_compute.ports[index];

      /* A diagnostic that was killed never closed its port.  Its descriptor
       * died with it; only the queue is left to reclaim.
       */

      if (other != NULL && kill(other->owner, 0) < 0 && errno == ESRCH)
        {
          sem_destroy(&other->ready);
          free(other);
          g_compute.ports[index] = NULL;
          other = NULL;
        }

      if (other == NULL && slot < 0)
        {
          slot = index;
        }
    }

  if (slot >= 0)
    {
      g_compute.ports[slot] = created;
    }

  nxmutex_unlock(&g_compute.lock);

  if (slot < 0)
    {
      close(created->fd);
      sem_destroy(&created->ready);
      free(created);
      return -EBUSY;
    }

  *port = created;
  return 0;
}

void ny_compute_port_close(struct ny_compute_port_s *port)
{
  int index;

  if (port == NULL)
    {
      return;
    }

  nxmutex_lock(&g_compute.lock);
  for (index = 0; index < NY_COMPUTE_MAX_PORTS; index++)
    {
      if (g_compute.ports[index] == port)
        {
          g_compute.ports[index] = NULL;
        }
    }

  nxmutex_unlock(&g_compute.lock);

  close(port->fd);
  sem_destroy(&port->ready);
  free(port);
}

int ny_compute_port_send(struct ny_compute_port_s *port, const uint8_t *wire,
                         size_t size)
{
  struct nyamp_header_s header;
  ssize_t written = -1;
  int attempt;

  if (port == NULL || nyamp_header_decode(&header, wire, size) != NYAMP_OK ||
      (header.request_id & NYAMP_REQUEST_ID_COMPUTE) != 0)
    {
      return -EINVAL;
    }

  /* Route before sending: the response may arrive before write() returns. */

  nxmutex_lock(&g_compute.lock);
  port->request_id = header.request_id;
  nxmutex_unlock(&g_compute.lock);

  for (attempt = 0; attempt < NY_COMPUTE_SEND_RETRIES; attempt++)
    {
      written = write(port->fd, wire, size);
      if (written >= 0 || (errno != EAGAIN && errno != EINTR))
        {
          break;
        }

      usleep(NY_COMPUTE_SEND_DELAY_US);
    }

  return written == (ssize_t)size ? 0 : written < 0 ? -errno : -EIO;
}

ssize_t ny_compute_port_recv(struct ny_compute_port_s *port, uint8_t *wire,
                             size_t capacity, int timeout_ms)
{
  struct ny_compute_frame_s *slot;
  struct timespec deadline;
  ssize_t size;

  if (port == NULL || wire == NULL)
    {
      return -EINVAL;
    }

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
  if (deadline.tv_nsec >= 1000000000)
    {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000;
    }

  while (sem_timedwait(&port->ready, &deadline) < 0)
    {
      if (errno == ETIMEDOUT)
        {
          return 0;
        }

      if (errno != EINTR)
        {
          return -errno;
        }
    }

  nxmutex_lock(&g_compute.lock);
  if (port->count == 0)
    {
      /* Woken by a stopping receive task rather than by a frame. */

      nxmutex_unlock(&g_compute.lock);
      return -ENOTCONN;
    }

  slot = &port->queue[port->head];
  size = slot->size;
  if ((size_t)size > capacity)
    {
      size = -EMSGSIZE;
    }
  else
    {
      memcpy(wire, slot->data, (size_t)size);
    }

  port->head = (port->head + 1) % NY_COMPUTE_PORT_DEPTH;
  port->count--;
  nxmutex_unlock(&g_compute.lock);
  return size;
}

/****************************************************************************
 * Name: ny_compute_request
 *
 * Description:
 *   Panel topics.
 *
 *   compute.status  any role  {running, linked, generation, capabilities,
 *                              capabilityMask, blob:{active, hashing, name,
 *                              offset, size, bytesPerSec}, lastError,
 *                              generationChanges, droppedFrames}
 *   compute.start   owner     start the receive task, then the status
 *   compute.stop    owner     stop it, then the status
 *
 ****************************************************************************/

int ny_compute_request(const struct ny_product_caller_s *caller,
                       const char *topic, const cJSON *data, cJSON **result)
{
  struct ny_compute_status_s status;
  char text[NY_COMPUTE_ERROR_MAX + 32];
  cJSON *capabilities;
  cJSON *root;
  cJSON *blob;
  bool start = strcmp(topic, "compute.start") == 0;
  bool stop = strcmp(topic, "compute.stop") == 0;
  int ret;

  (void)data;
  if (!start && !stop && strcmp(topic, "compute.status") != 0)
    {
      return -ENOSYS;
    }

  if (start || stop)
    {
      if (caller->role != NY_PRODUCT_OWNER)
        {
          return -EACCES;
        }

      ret = start ? ny_compute_start() : ny_compute_stop();
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = ny_compute_status(&status);
  if (ret < 0)
    {
      return ret;
    }

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  capabilities = cJSON_AddArrayToObject(root, "capabilities");
  blob = cJSON_AddObjectToObject(root, "blob");
  if (capabilities == NULL || blob == NULL ||
      !cJSON_AddBoolToObject(root, "running", status.running) ||
      !cJSON_AddBoolToObject(root, "linked", status.linked) ||
      !cJSON_AddNumberToObject(root, "generation", status.generation) ||
      !cJSON_AddNumberToObject(root, "capabilityMask", status.capabilities) ||
      !cJSON_AddBoolToObject(blob, "active", status.blob_active) ||
      !cJSON_AddBoolToObject(blob, "hashing", status.blob_hashing) ||
      !cJSON_AddStringToObject(blob, "name", status.blob_name) ||
      !cJSON_AddNumberToObject(blob, "offset", (double)status.blob_offset) ||
      !cJSON_AddNumberToObject(blob, "size", (double)status.blob_size) ||
      !cJSON_AddNumberToObject(blob, "bytesPerSec",
                               status.blob_bytes_per_sec) ||
      !cJSON_AddNumberToObject(root, "generationChanges",
                               status.generation_changes) ||
      !cJSON_AddNumberToObject(root, "droppedFrames", status.dropped_frames))
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  if (((status.capabilities & NY_COMPUTE_CAP_HEALTH) != 0 &&
       !cJSON_AddItemToArray(capabilities, cJSON_CreateString("health"))) ||
      ((status.capabilities & NY_COMPUTE_CAP_LLM) != 0 &&
       !cJSON_AddItemToArray(capabilities, cJSON_CreateString("llm"))) ||
      ((status.capabilities & NY_COMPUTE_CAP_BLOB) != 0 &&
       !cJSON_AddItemToArray(capabilities, cJSON_CreateString("blob"))))
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  if (status.last_error == 0)
    {
      ret = cJSON_AddNullToObject(root, "lastError") != NULL;
    }
  else
    {
      snprintf(text, sizeof(text), "%d: %s", status.last_error,
               status.last_error_text);
      ret = cJSON_AddStringToObject(root, "lastError", text) != NULL;
    }

  if (!ret)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  *result = root;
  return 0;
}

#endif /* CONFIG_NYABULA_CORE_COMPUTE */
