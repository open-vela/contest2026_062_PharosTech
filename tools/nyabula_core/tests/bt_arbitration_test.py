#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the real Bluetooth hold/release path with overlapping owners."""
from pathlib import Path
import subprocess,sys,tempfile
core=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temp:
 root=Path(temp);(root/'nuttx').mkdir()
 (root/'nuttx/config.h').write_text('''#define CONFIG_NYABULA_CORE_BT 1
#define CONFIG_NYABULA_CORE_BT_A2DP_PREBUFFER_MS 200
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
int main(void){
 assert(ny_bt_audio_hold(0)==0);assert(ny_bt_audio_hold(0)==0);assert(g_bt_audio.holds==2);
 ny_bt_audio_release();assert(g_bt_audio.held);assert(g_bt_audio.holds==1);
 g_bt_audio.want_call=true;assert(ny_bt_audio_hold(0)==-EBUSY);assert(g_bt_audio.holds==1);g_bt_audio.want_call=false;
 sem_init(&g_bt_audio.wake,0,0);g_bt_audio.running=true;g_bt_audio.device_closed=false;
 assert(ny_bt_audio_hold(0)==-ETIMEDOUT);assert(g_bt_audio.holds==1);assert(g_bt_audio.held);
 g_bt_audio.running=false;ny_bt_audio_release();assert(!g_bt_audio.held);assert(g_bt_audio.holds==0);sem_destroy(&g_bt_audio.wake);
 puts("NY_BT_ARBITRATION_PASS");return 0;}
''')
 binary=root/'test';subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(root),'-I'+str(core),str(source),str(core/'ny_pcm.c'),'-pthread','-lm','-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
