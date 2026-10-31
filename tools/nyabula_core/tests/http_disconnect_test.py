#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""An HTTP peer disconnect must return an error, not kill the Web service."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
p=argparse.ArgumentParser();p.add_argument('core',type=Path);a=p.parse_args()
s=(a.core/'ny_web_http.c').read_text()
m=re.search(r'int ny_web_http_write\([^;]*?\n\{',s);assert m
end,depth=m.end(),1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
code=r'''
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define NY_HTTP_SEND_MS 15000
static uint64_t nyabula_eye_ws_now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
'''+s[m.start():end]+r'''
int main(void) {
 signal(SIGPIPE,SIG_DFL);
 int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
 close(pair[1]);assert(ny_web_http_write(pair[0],"x",1)==-EPIPE);close(pair[0]);
 assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
 assert(!ny_web_http_write(pair[0],"payload",7));char b[8]={0};assert(read(pair[1],b,7)==7);assert(!strcmp(b,"payload"));
 close(pair[0]);close(pair[1]);puts("HTTP_DISCONNECT_PASS no_SIGPIPE normal_send_preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-http-disconnect-') as d:
    root=Path(d);c=root/'test.c';c.write_text(code);binary=root/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)])
    print('HTTP_DISCONNECT_EXIT',result.returncode,flush=True)
    assert result.returncode==0,'Disconnected HTTP peer terminated the process'
