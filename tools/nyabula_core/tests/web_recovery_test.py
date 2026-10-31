#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fault-inject the actual Web listener/recovery functions over host TCP.

Only protocol handlers and product/auth dependencies are fixtures. The owner,
listener, admission, cleanup, backoff and stop code are extracted unchanged.
Run on the designated Linux builder; no device or credential is accessed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
a = p.parse_args()
source = (a.core / 'ny_web.c').read_text()
def function(name):
    match = re.search(r'(?:static )?(?:bool|int|uint64_t) ' + name + r'\([^;]*?\n\{', source)
    assert match, name
    at, depth = match.end(), 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[match.start():at]
names = ['ny_web_stop', 'ny_web_stop_requested', 'ny_web_monotonic_ms',
         'ny_web_recovery_wait', 'ny_web_accept_retryable', 'ny_web_listen', 'ny_web_run']
constants = '\n'.join(line for line in source.splitlines() if re.match(
    r'#define (NYABULA_WS_TOKEN_SIZE|NY_WEB_(?:MAX_CLIENTS|ACCEPT_RETRY_MS|ACCEPT_MAX_MS|RESTART_MIN_MS|RESTART_MAX_MS|STABLE_MS|STOP_POLL_MS))\s', line))
assert re.search(r'^#define\s+NY_WEB_RESTART_MAX_MS\s+30000\s*$', constants, re.M)
struct = re.search(r'struct ny_web_client_args_s\n\{.*?\n\};', source, re.S).group()
fixture = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
typedef pthread_mutex_t mutex_t;
#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define nxmutex_lock pthread_mutex_lock
#define nxmutex_unlock pthread_mutex_unlock
#define CONFIG_NYABULA_CORE_PRODUCT 1
#define CONFIG_NYABULA_CORE_CLOUD 1
#define CONFIG_NYABULA_CORE_WEB_PORT test_port
#define CONFIG_NYABULA_CORE_WEB_TOKEN_PATH "fixture-token"
#define CONFIG_NYABULA_CORE_WEB_AUTH_PATH "fixture-auth"
static int test_port;
static atomic_int poll_fault, accept_fault, socket_faults, listeners, failures;
static atomic_int token_loads, token_creates, auth_loads, last_delay;
static bool missing_token;
static int fixture_poll(struct pollfd *p, nfds_t n, int t) {
 int f=atomic_exchange(&poll_fault,0);
 if(f==1){errno=EBADF;return -1;}
 if(f==2){p->revents=POLLIN|POLLHUP;return 1;}
 return poll(p,n,t);
}
static int fixture_accept(int fd,struct sockaddr *a,socklen_t *l){
 int e=atomic_exchange(&accept_fault,0);if(e){errno=e;return -1;}return accept(fd,a,l);
}
static int fixture_socket(int d,int t,int p){
 int left=atomic_load(&socket_faults);
 if(left>0){atomic_fetch_sub(&socket_faults,1);atomic_fetch_add(&failures,1);errno=ENOMEM;return -1;}
 return socket(d,t,p);
}
static int fixture_listen(int fd,int backlog){int r=listen(fd,backlog);if(!r)atomic_fetch_add(&listeners,1);return r;}
static void fixture_log(int pri,const char *fmt,...){
 (void)pri;va_list ap;va_start(ap,fmt);char line[256];vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
 unsigned int delay;int code;
 if(sscanf(line,"nyabula_web: listener exited: %d; restart in %u ms",&code,&delay)==2)atomic_store(&last_delay,delay);
 fputs(line,stdout);fflush(stdout);
}
static void ny_web_store_wait(const char *p){(void)p;}
static void ny_web_auth_load(const char *p){(void)p;atomic_fetch_add(&auth_loads,1);}
static int ny_web_token_load(const char *p,char *token){(void)p;atomic_fetch_add(&token_loads,1);if(missing_token)return -ENOENT;memset(token,'a',64);token[64]=0;return 0;}
static int ny_web_token_create(const char *p,char *token){(void)p;(void)token;atomic_fetch_add(&token_creates,1);return -EIO;}
static int ny_product_start(void){return 0;}
static int ny_cloud_start(void){return 0;}
static void ny_cloud_stop(void){}
static void ny_web_client_limits(int fd){struct timeval t={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));}
'''
fixture += constants + '\n' + struct + r'''
static mutex_t g_web_lock=NXMUTEX_INITIALIZER;
static bool g_web_running,g_web_stopping,g_web_active;
static int g_web_clients[NY_WEB_MAX_CLIENTS];
static char g_web_product_token[NYABULA_WS_TOKEN_SIZE+1];
static void *ny_web_client_worker(void *v){
 struct ny_web_client_args_s *a=v;
 assert(!(fcntl(a->fd,F_GETFL,0)&O_NONBLOCK));
 assert(strlen(a->token)==64&&a->token[0]=='a');
 char b[256];if(recv(a->fd,b,sizeof(b),0)>0){const char *r="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";send(a->fd,r,strlen(r),MSG_NOSIGNAL);}
 close(a->fd);pthread_mutex_lock(&g_web_lock);g_web_clients[a->slot]=-1;pthread_mutex_unlock(&g_web_lock);free(a);return NULL;
}
#define poll fixture_poll
#define accept fixture_accept
#define socket fixture_socket
#define listen fixture_listen
#define syslog fixture_log
'''
fixture += '\n\n'.join(function(n) for n in names)
fixture += r'''
#undef poll
#undef accept
#undef socket
#undef listen
#undef syslog
static char *product[]={(char *)"nyabula_web"};
static char port_text[16];
static char *operator_args[]={(char *)"nyabula_web",port_text,(char *)"fixture-token",(char *)"http://127.0.0.1:5180"};
static void *run(void *mode){int argc=mode?4:1;return (void *)(intptr_t)ny_web_run(argc,mode?operator_args:product);}
static int http(void){
 int s=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(test_port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 struct timeval timeout={2,0};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 if(connect(s,(void *)&a,sizeof(a))<0){close(s);return 0;}
 const char *q="GET / HTTP/1.1\r\nHost: fixture\r\n\r\n";send(s,q,strlen(q),MSG_NOSIGNAL);char b[128]={0};int n=recv(s,b,sizeof(b)-1,0);close(s);return n>0&&strstr(b,"200 OK")!=NULL;
}
static void ready(int minimum){
 uint64_t end=ny_web_monotonic_ms()+25000;
 while(ny_web_monotonic_ms()<end){if(atomic_load(&listeners)>=minimum&&http())return;usleep(20000);}assert(!"listener did not recover");
}
static void joined(pthread_t t,int code){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=3;void *r;assert(!pthread_timedjoin_np(t,&r,&until));assert((intptr_t)r==code);}
static int fdcount(void){DIR*d=opendir("/proc/self/fd");assert(d);int n=0;while(readdir(d))n++;closedir(d);return n;}
int main(void){
 int reserve=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};assert(!bind(reserve,(void*)&a,sizeof(a)));socklen_t len=sizeof(a);getsockname(reserve,(void*)&a,&len);test_port=ntohs(a.sin_port);close(reserve);snprintf(port_text,sizeof(port_text),"%d",test_port);
 int before=fdcount();pthread_t thread;assert(!pthread_create(&thread,NULL,run,NULL));ready(1);
 int loads=atomic_load(&token_loads),auth=atomic_load(&auth_loads);
 assert(ny_web_run(1,product)==EXIT_FAILURE);assert(atomic_load(&token_loads)==loads);
 for(int i=0;i<4;i++){const int errors[]={ECONNABORTED,EMFILE,ENOBUFS,EAGAIN};atomic_store(&accept_fault,errors[i]);assert(http());assert(atomic_load(&listeners)==1);}
 atomic_store(&poll_fault,1);ready(2);assert(atomic_load(&last_delay)==1000);
 atomic_store(&socket_faults,2);atomic_store(&poll_fault,2);ready(3);
 assert(atomic_load(&last_delay)==8000);assert(atomic_load(&failures)==2);
 assert(atomic_load(&token_loads)==loads&&atomic_load(&auth_loads)==auth&&atomic_load(&token_creates)==0);
 atomic_store(&poll_fault,2);
 uint64_t until=ny_web_monotonic_ms()+2000;
 while(atomic_load(&last_delay)!=16000&&ny_web_monotonic_ms()<until)usleep(10000);
 assert(atomic_load(&last_delay)==16000);
 assert(ny_web_run(1,product)==EXIT_FAILURE);assert(!ny_web_stop());joined(thread,EXIT_SUCCESS);
 int count=atomic_load(&listeners);usleep(150000);assert(atomic_load(&listeners)==count);assert(!g_web_active&&!g_web_running);
 assert(!pthread_create(&thread,NULL,run,NULL));ready(count+1);assert(!ny_web_stop());joined(thread,EXIT_SUCCESS);
 count=atomic_load(&listeners);assert(!pthread_create(&thread,NULL,run,(void*)1));ready(count+1);atomic_store(&poll_fault,1);joined(thread,EXIT_FAILURE);usleep(150000);assert(atomic_load(&listeners)==count+1);
 missing_token=true;int creates=atomic_load(&token_creates);assert(ny_web_run(4,operator_args)==EXIT_FAILURE);assert(atomic_load(&token_creates)==creates);
 assert(fdcount()==before);
 puts("WEB_RECOVERY_PASS transient_accept fatal_poll rebuild_backoff duplicate_owner stop_during_backoff explicit_stop operator_single_attempt same_token no_fd_leak");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-web-recovery-') as directory:
    c = Path(directory) / 'test.c'
    c.write_text(fixture)
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=45)
