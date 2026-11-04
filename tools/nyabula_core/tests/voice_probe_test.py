#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Voice test lifecycle: real-input counters, bounded time and state restoration."""
from pathlib import Path
import re,subprocess,tempfile,sys
core=Path(sys.argv[1]).resolve();source=(core/'ny_voice.c').read_text()
def fn(name):
 m=re.search(r'static (?:bool|int|void) '+name+r'\([^;]*?\)\n\{',source);assert m,name
 i=m.end();depth=1
 for t in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',source[i:],re.S):
  if t.group()=='{':depth+=1
  elif t.group()=='}':depth-=1
  if depth==0:return source[m.start():i+t.end()]
 raise AssertionError(name)
code=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "ny_voice_probe.h"
enum ny_voice_state_e {NY_VOICE_OFF,NY_VOICE_IDLE,NY_VOICE_LISTENING,NY_VOICE_SPEAKING};
#define NY_VOICE_MODEL_ERROR 3
#define NY_VOICE_EV_ENABLE 1
static uint64_t now;static int start_error,enabled_events;
static struct {
 int lock;enum ny_voice_state_e state;bool running,stopping,capture_hold,capture_open,streaming;
 int capture_error;unsigned claims;struct {bool enabled;}settings;struct {int state,error;}kws;
 struct ny_voice_probe_s probe;bool probe_temporary,probe_guard,probe_finish,kws_restart;
}g_voice;
static int nxmutex_lock(int*p){(void)p;return 0;}
static int nxmutex_unlock(int*p){(void)p;return 0;}
static uint64_t ny_voice_now_ms(void){return now;}
static int ny_voice_start(void){return start_error;}
static void ny_voice_post(int event,bool ok,bool empty,int job){(void)ok;(void)empty;(void)job;assert(event==NY_VOICE_EV_ENABLE);enabled_events++;}
'''
code+='\n'+fn('ny_voice_diagnostics_end_locked')+'\n'+fn('ny_voice_diagnostics_tick')+'\n'+fn('ny_voice_diagnostics_start')
code+=r'''
int main(void){
 struct ny_voice_probe_s p={0};
 assert(ny_voice_probe_begin(&p,NY_VOICE_PROBE_NONE,1000,0)==-EINVAL);
 assert(ny_voice_probe_begin(&p,NY_VOICE_PROBE_WAKE,2000,0)==0);
 assert(ny_voice_probe_begin(&p,NY_VOICE_PROBE_MIC,1000,0)==-EBUSY);
 assert(!ny_voice_probe_tick(&p,1000,false,0)&&!p.ready);
 assert(!ny_voice_probe_tick(&p,1100,true,0)&&p.until_ms==3100);
 ny_voice_probe_hit(&p,"nihao_openvela",14);assert(p.detections==1&&!strcmp(p.word,"nihao_openvela"));
 int16_t samples[]={-32768,0,32767,100};ny_voice_probe_feed(&p,samples,4,200);
 assert(p.samples==4&&p.peak==32768&&p.clipped==2&&p.max_rms==200&&p.squares==2147428113ULL);
 assert(!ny_voice_probe_tick(&p,3099,true,0));assert(ny_voice_probe_tick(&p,3100,true,0));
 ny_voice_probe_feed(&p,samples,4,300);assert(p.samples==4);
 assert(ny_voice_probe_begin(&p,NY_VOICE_PROBE_MIC,1000,4000)==0);
 assert(ny_voice_probe_tick(&p,34000,false,0)&&p.error==-ETIMEDOUT);
 assert(ny_voice_probe_begin(&p,NY_VOICE_PROBE_MIC,1000,0)==0);ny_voice_probe_cancel(&p);assert(p.cancelled&&!p.active);
 g_voice.state=NY_VOICE_OFF;assert(ny_voice_diagnostics_start(NY_VOICE_PROBE_WAKE,2000)==0);
 assert(g_voice.probe_temporary&&!g_voice.settings.enabled);
 g_voice.capture_open=true;g_voice.streaming=true;assert(!ny_voice_diagnostics_tick());
 now=2000;assert(ny_voice_diagnostics_tick());assert(g_voice.state==NY_VOICE_OFF&&g_voice.stopping&&!g_voice.settings.enabled);
 memset(&g_voice,0,sizeof(g_voice));g_voice.state=NY_VOICE_IDLE;g_voice.running=true;g_voice.settings.enabled=true;
 assert(ny_voice_diagnostics_start(NY_VOICE_PROBE_MIC,1000)==0);
 g_voice.claims=1;assert(!ny_voice_diagnostics_tick());assert(g_voice.probe.error==-EBUSY&&g_voice.kws_restart&&g_voice.probe_guard);
 assert(g_voice.settings.enabled&&!g_voice.stopping);
 memset(&g_voice,0,sizeof(g_voice));g_voice.state=NY_VOICE_SPEAKING;
 assert(ny_voice_diagnostics_start(NY_VOICE_PROBE_WAKE,1000)==-EBUSY&&!g_voice.probe.active);
 g_voice.state=NY_VOICE_OFF;start_error=-ENOMEM;assert(ny_voice_diagnostics_start(NY_VOICE_PROBE_MIC,1000)==-ENOMEM&&!g_voice.probe.active);
 puts("VOICE_PROBE_PASS ready_deadline metrics clipping cancellation restore_disabled preserve_enabled playback_busy");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-voice-probe-') as d:
 p=Path(d);c=p/'test.c';c.write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(core),str(c),str(core/'ny_voice_probe.c'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
