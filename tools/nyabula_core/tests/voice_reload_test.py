#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the production pump transition before starting a new KWS stream."""
import subprocess,tempfile,sys
from pathlib import Path
core=Path(sys.argv[1]);s=(core/'ny_voice.c').read_text()
a=s.index('static void *ny_voice_pump_thread(void *arg)\n{')
a=s.index('      nxmutex_lock(&g_voice.lock);',a)
b=s.index('      if (!streaming)\n',a)
fragment=s[a:b]
prefix=r'''#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
enum ny_voice_state_e { NY_VOICE_OFF, NY_VOICE_IDLE };
#define NYAMP_SERVICE_KWS 10
#define NY_VOICE_MODEL_UNLOADED 0
static struct {int lock;enum ny_voice_state_e state;bool streaming,kws_reload,kws_restart,stopping;int kws;} g_voice;
static struct {struct {int request_id;}wire;}port;
static int opened,unloaded,ended,failure;
static void nxmutex_lock(int*p){(void)p;}static void nxmutex_unlock(int*p){(void)p;}
static void usleep(int n){(void)n;}
static int ny_voice_port_open(void*p){(void)p;opened++;return failure;}
static int ny_voice_wire_unload(void*p,int s){(void)p;(void)s;unloaded++;return 0;}
static void ny_voice_wire_kws_end(void*p){(void)p;ended++;}
static void ny_voice_pump_events(void*p,int n){(void)p;(void)n;}
static void ny_voice_model_set(int*p,int state,int e){(void)e;*p=state;}
static void step(void){enum ny_voice_state_e state;bool streaming,reload,restart;int ret;for(int once=0;once<1;once++){
'''
suffix=r'''}}
int main(void){
 g_voice.kws_reload=true;g_voice.state=NY_VOICE_OFF;step();assert(g_voice.kws_reload&&unloaded==0);
 g_voice.state=NY_VOICE_IDLE;step();assert(!g_voice.kws_reload&&unloaded==1&&opened==1);
 g_voice.streaming=true;g_voice.kws_reload=true;step();assert(ended==1&&unloaded==2&&!g_voice.streaming);
 g_voice.kws_reload=true;failure=-ENOTCONN;step();assert(g_voice.kws_reload&&unloaded==2);
 failure=0;step();assert(!g_voice.kws_reload&&unloaded==3);
 puts("VOICE_RELOAD_PASS disabled_edit stopped_loaded_model active_stream retry");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(prefix+fragment+suffix)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
