#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""FAT missing-parent ENOTDIR must not quarantine a fresh plugin."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-health-') as directory:
    root = Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'nuttx/config.h').write_text(
        '#define CONFIG_NYABULA_CORE_HEALTH_STORE "/unused"\n'
        '#define CONFIG_NYABULA_CORE_QUARANTINE_FAILURES 3\n')
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock(m) (-pthread_mutex_lock(m))\n'
        '#define nxmutex_unlock(m) (-pthread_mutex_unlock(m))\n')
    fixture = root / 'test.c'
    fixture.write_text(r'''
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
static size_t test_strlcpy(char *dst,const char *src,size_t size)
{size_t n=strlen(src);if(size)snprintf(dst,size,"%s",src);return n;}
static int test_open(const char *path,int flags,...)
{
 if ((flags & O_ACCMODE)==O_RDONLY && strstr(path,"missing")) {errno=ENOTDIR;return -1;}
 return open(path,flags,0600);
}
#define strlcpy test_strlcpy
#define open test_open
#include "ny_health.c"
int main(int argc,char **argv)
{
 (void)argc;char path[4096];unsigned int failures=99;
 snprintf(path,sizeof(path),"%s/missing/plug/1.failures",argv[1]);
 assert(ny_health_read(path,&failures)==0 && failures==0);
 snprintf(path,sizeof(path),"%s/blocked",argv[1]);
 FILE *file=fopen(path,"w");assert(file);fputs("not a directory",file);fclose(file);
 snprintf(path,sizeof(path),"%s/blocked/plug/1.failures",argv[1]);
 assert(ny_health_read(path,&failures)==-ENOTDIR);
 snprintf(path,sizeof(path),"%s/existing.failures",argv[1]);
 file=fopen(path,"w");assert(file);fputs("3\n",file);fclose(file);
 assert(ny_health_read(path,&failures)==0 && failures==3);
 file=fopen(path,"w");assert(file);fputs("corrupt",file);fclose(file);
 assert(ny_health_read(path,&failures)==-EINVAL);
 puts("NYABULA_HEALTH_MISSING_PASS");return 0;
}
''')
    binary = root / 'test'
    subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g',
        '-fsanitize=address,undefined', '-I' + str(root), '-I' + str(a.core),
        str(fixture), '-pthread', '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(root)], check=True)
