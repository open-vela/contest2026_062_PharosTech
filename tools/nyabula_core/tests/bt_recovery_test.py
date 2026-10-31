#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check dry-output recovery, rebuffering, hold and failed reopen."""
from pathlib import Path
import subprocess,sys,tempfile
core=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temp:
 root=Path(temp);(root/'nuttx').mkdir()
 (root/'nuttx/config.h').write_text('''#define CONFIG_NYABULA_CORE_BT 1
#define CONFIG_NYABULA_CORE_BT_A2DP_PREBUFFER_MS 20
#define CONFIG_NYABULA_CORE_BT_AUDIO_PRIORITY 120
#define CONFIG_NYABULA_CORE_BT_GATE_THRESHOLD 500
#define CONFIG_NYABULA_CORE_BT_GATE_ATTENUATION_DB 24
#define CONFIG_NYABULA_CORE_BT_GATE_HANG_MS 250
#define CONFIG_NYABULA_CORE_BT_INPUT_DEVICE "input"
#define CONFIG_NYABULA_CORE_BT_OUTPUT_DEVICE "output"
''')
 (root/'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock(m) pthread_mutex_lock(m)\n#define nxmutex_unlock(m) pthread_mutex_unlock(m)\n')
 source=root/'test.c';source.write_text('''#include <assert.h>
#include <stdio.h>
#include "ny_product_bt_audio.c"
struct ny_audio_stream_s {int unused;};
static struct ny_audio_stream_s device;
static size_t queued;
static int opens, closes, resets, fail_open, starved;
int ny_audio_stream_open(struct ny_audio_stream_s **s,const struct ny_audio_stream_config_s *c){if(fail_open)return -EBUSY;assert(c->rate==44100);*s=&device;starved=0;opens++;return 0;}
void ny_audio_stream_close(struct ny_audio_stream_s *s){assert(s==&device);closes++;}
size_t ny_audio_stream_queued(struct ny_audio_stream_s *s){return queued+128;}
void ny_audio_stream_stats(struct ny_audio_stream_s *s,struct ny_audio_stream_stats_s *st){memset(st,0,sizeof(*st));st->queued_bytes=queued;st->starved=starved;}
ssize_t ny_audio_stream_write(struct ny_audio_stream_s*s,const void*p,size_t n,int t){queued+=n;return n;}
int ny_sbc_decoder_reset(struct ny_sbc_decoder_s *d){resets++;return 0;}
int ny_sbc_decode(struct ny_sbc_decoder_s*d,const uint8_t*p,size_t n,size_t*u,int16_t*o,size_t c,struct ny_sbc_format_s*f){memset(f,0,sizeof(*f));f->rate=44100;f->channels=2;*u=n;return 128;}
static void fill(void){uint8_t p[2]={15,0};assert(ny_pcm_ring_put(&g_bt_audio.media_ring,p,2));g_bt_audio.media_buffered_frames=15;}
int main(void){
 struct ny_bt_audio_worker_s w={0};
 assert(ny_pcm_ring_init(&g_bt_audio.media_ring,4096)==0);
 g_bt_audio.media_rate=44100;g_bt_audio.media_frame_samples=128;
 fill();ny_bt_audio_media_service(&w);assert(opens==1&&resets==1&&w.playing);
 queued=0;ny_bt_audio_media_service(&w);
 assert(g_bt_audio.status.media_underruns==1);
 assert(w.speaker==NULL && closes==1 && g_bt_audio.device_closed);
 ny_bt_audio_media_service(&w);assert(g_bt_audio.status.media_underruns==1);
 fill();ny_bt_audio_media_service(&w);assert(opens==2&&resets==2&&w.playing);
 queued=0;ny_bt_audio_media_service(&w);assert(closes==2);
 fill();g_bt_audio.held=true;ny_bt_audio_media_service(&w);assert(opens==2);
 g_bt_audio.held=false;fail_open=1;ny_bt_audio_media_service(&w);assert(opens==2&&w.speaker==NULL&&g_bt_audio.device_closed);
 fail_open=0;w.retry_at=0;ny_bt_audio_media_service(&w);assert(opens==3&&resets==3);
 fill();starved=1;ny_bt_audio_media_service(&w);assert(opens==4&&resets==4&&closes==3);assert(g_bt_audio.status.media_recoveries==3);
 ny_pcm_ring_free(&g_bt_audio.media_ring);puts("NY_BT_RECOVERY_PASS");return 0;}

''')
 binary=root/'test';subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(root),'-I'+str(core),str(source),str(core/'ny_pcm.c'),'-pthread','-lm','-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)


