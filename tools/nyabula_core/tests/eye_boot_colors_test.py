#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Saved per-eye colours must precede the engine's first animation callback."""
from pathlib import Path
import re,subprocess,tempfile,sys
root=Path(sys.argv[1]).resolve()
def body(path,name):
 s=(root/path).read_text();m=re.search(r'\b'+name+r'\([^;]*?\)\n\{',s);assert m,name
 depth=1;i=m.end()
 for t in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',s[i:],re.S):
  if t.group()=='{':depth+=1
  elif t.group()=='}':depth-=1
  if depth==0:return s[m.start():i+t.end()]
 raise AssertionError(name)
C=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#define NYABULA_EYE_COUNT 2
#define NYABULA_EYE_LEFT 0
#define NYABULA_EYE_RIGHT 1
#define NYABULA_EYE_MASK_BOTH 3
#define NYABULA_EYE_EXPRESSION_IDLE 0
#define NYABULA_EYE_DEFAULT_IRIS_RGB 0x38e06e
#define NYABULA_EYE_DEFAULT_LIGHT .55f
#define NYABULA_EYE_FIRST_BLINK_MS 1000
#define NYABULA_EYE_SACCADE_FIRST_MS 2000
#define NYABULA_EYE_TICK_MS 16
#define LV_EVENT_REFR_START 1
#define NY_PRODUCT_IRIS_DEFAULT 0x56ffb2
#define NY_PRODUCT_IRIS_DOMAIN "eyes"
#define NY_PRODUCT_IRIS_BOOT_WAIT_MS 15000
#define NY_PRODUCT_IRIS_BOOT_POLL_MS 100
#define CONFIG_NYABULA_CORE_STATE_DATABASE "/data/nyabula/core.sqlite"
#define MSDOS_SUPER_MAGIC 0x4d44
struct statfs {int f_type;};
static int polls,ready_after,reads,store_error;
static int statfs(const char*p,struct statfs*i){assert(!strcmp(p,"/data"));i->f_type=polls>=ready_after?MSDOS_SUPER_MAGIC:0;return 0;}
static int usleep(unsigned n){assert(n==100000);polls++;return 0;}
typedef struct {double valuedouble;bool number;} cJSON;
static cJSON left,right,record;
static int ny_product_store_read(const char*d,cJSON**r,uint64_t*v){assert(!strcmp(d,"eyes"));reads++;*r=store_error?NULL:&record;*v=7;return store_error;}
static const cJSON *cJSON_GetObjectItemCaseSensitive(cJSON*r,const char*k){return !r?NULL:!strcmp(k,"left")?&left:&right;}
#define cJSON_IsNumber(p) ((p)!=NULL && (p)->number)
static void cJSON_Delete(cJSON*p){(void)p;}
typedef struct {void*display;} lv_obj_t;
typedef struct {void*user;} lv_timer_t;
struct nyabula_eye_engine_s {
 void*renderer;void*disp[2];lv_timer_t*timer;
 int expression;float ambient_light,ambient_current;bool auto_blink;int blink_eyes;
 uint32_t random_state;float sleep_end_top,sleep_end_bottom;
 uint32_t start_tick,last_tick,mode_start,scene_start,previous_scene_start,scene_fade_start,scene_phase_start,next_blink,next_saccade;
 uint32_t iris_rgb[2];struct {int value;} current,target;
};
static unsigned frames;static uint32_t first[2];static lv_timer_t timer;
static void *nyabula_eye_renderer_create(lv_obj_t*a,lv_obj_t*b){assert(a&&b);return &timer;}
static void *lv_obj_get_display(lv_obj_t*p){return p->display;}
static uint32_t lv_tick_get(void){return 100;}
static void nyabula_eye_engine_apply_expression(struct nyabula_eye_engine_s*e,float t){(void)e;(void)t;}
static void nyabula_eye_engine_animation_cb(lv_timer_t*t){struct nyabula_eye_engine_s*e=t->user;frames++;memcpy(first,e->iris_rgb,sizeof(first));}
static lv_timer_t *lv_timer_create(void(*cb)(lv_timer_t*),int ms,void*p){(void)cb;(void)ms;timer.user=p;return &timer;}
static void nyabula_eye_engine_refr_start_cb(void*p){(void)p;}
static void lv_display_add_event_cb(void*d,void(*cb)(void*),int event,void*p){(void)d;(void)cb;(void)event;(void)p;}
static void nyabula_eye_engine_destroy(struct nyabula_eye_engine_s*e){free(e);}
struct nyabula_core_s {int unused;};static struct nyabula_core_s core;
static struct nyabula_core_s *nyabula_core_create_with_iris(struct nyabula_eye_engine_s*e,const uint32_t*colors){assert(e);if(colors)assert(!memcmp(colors,e->iris_rgb,8));return &core;}
static int g_eye_lock;static pthread_t g_eye_owner;
static struct nyabula_eye_engine_s*g_eye_engine;static struct nyabula_core_s*g_eye_control;
static int nxmutex_lock(int*p){(void)p;return 0;}static int nxmutex_unlock(int*p){(void)p;return 0;}
'''
C+='\nstatic int '+body('app/nyabula_core/ny_product_eyes.c','ny_product_iris_read')
C+='\nint '+body('app/nyabula_core/ny_product_eyes.c','nyabula_eye_service_boot_colors')
C+='\nstruct nyabula_eye_engine_s *'+body('app/nyabula/src/nyabula_eye_engine.c','nyabula_eye_engine_create_dual_with_iris')
C+='\nstruct nyabula_eye_engine_s *'+body('app/nyabula/src/nyabula_eye_engine.c','nyabula_eye_engine_create_dual')
C+='\nint '+body('app/nyabula/src/nyabula_eye_service.c','nyabula_eye_service_attach')
C+=r'''
int main(void){
 lv_obj_t a={&left},b={&right};left=(cJSON){0x6fd3ff,true};right=(cJSON){0xff42aa,true};ready_after=3;
 assert(nyabula_eye_service_attach(&a,&b)==0);assert(polls==3&&reads==1&&frames==1);assert(first[0]==0x6fd3ff&&first[1]==0xff42aa);
 nyabula_eye_engine_destroy(g_eye_engine);g_eye_engine=NULL;g_eye_control=NULL;
 store_error=-EIO;assert(nyabula_eye_service_attach(&a,&b)==-EIO);assert(frames==1&&g_eye_engine==NULL);
 store_error=-ENOENT;assert(nyabula_eye_service_attach(&a,&b)==0);assert(first[0]==NY_PRODUCT_IRIS_DEFAULT&&first[1]==NY_PRODUCT_IRIS_DEFAULT);
 nyabula_eye_engine_destroy(g_eye_engine);g_eye_engine=NULL;g_eye_control=NULL;
 struct nyabula_eye_engine_s*e=nyabula_eye_engine_create_dual(&a,&b);assert(first[0]==NYABULA_EYE_DEFAULT_IRIS_RGB);nyabula_eye_engine_destroy(e);
 puts("EYE_BOOT_FIRST_FRAME_PASS saved_distinct_colours delayed_mount read_error standalone_defaults");
}
'''
CPP=r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <semaphore.h>
#include <cstdio>
#define NYABULA_EYE_COUNT 2
#define NYABULA_EYE_LEFT 0
#define NYABULA_EYE_RIGHT 1
#define NYABULA_EYE_MASK_LEFT 1
#define NYABULA_EYE_MASK_RIGHT 2
#define NYABULA_EYE_MASK_BOTH 3
#define NYABULA_EYE_EXPRESSION_IDLE 0
#define NYABULA_EYE_SCENE_NONE 0
struct nyabula_eye_engine_s {uint32_t colours[2];float light;};
struct nyabula_core_s {sem_t lock;nyabula_eye_engine_s*eye_engine;int expression_winner,scene_winner;struct {int expression,scene,blink_eyes;bool auto_blink;float ambient_light;uint32_t iris_rgb[2];}snapshot;};
static void nyabula_eye_engine_set_ambient_light(nyabula_eye_engine_s*e,float v){e->light=v;}
static void nyabula_eye_engine_set_iris_color(nyabula_eye_engine_s*e,int mask,uint32_t c){for(int i=0;i<2;i++)if(mask&(1<<i))e->colours[i]=c;}
'''
CPP+='\nextern "C" struct nyabula_core_s *'+body('app/nyabula/src/nyabula_core.cxx','nyabula_core_create_with_iris')
CPP+='\nextern "C" struct nyabula_core_s *'+body('app/nyabula/src/nyabula_core.cxx','nyabula_core_create')
CPP+=r'''
int main(){nyabula_eye_engine_s e{};uint32_t c[2]={0x6fd3ff,0xff42aa};auto*core=nyabula_core_create_with_iris(&e,c);assert(core&&core->snapshot.iris_rgb[0]==c[0]&&core->snapshot.iris_rgb[1]==c[1]);assert(e.colours[0]==c[0]&&e.colours[1]==c[1]);sem_destroy(&core->lock);free(core);core=nyabula_core_create(&e);assert(e.colours[0]==0x56ffb2&&e.colours[1]==0x56ffb2);sem_destroy(&core->lock);free(core);puts("EYE_BOOT_SNAPSHOT_PASS");}
'''
with tempfile.TemporaryDirectory(prefix='ny-eye-boot-') as temp:
 p=Path(temp)
 for name,text,compiler in [('engine.c',C,'cc'),('snapshot.cpp',CPP,'c++')]:
  source=p/name;source.write_text(text);binary=p/(name+'.out')
  subprocess.run([compiler,'-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-pthread','-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
