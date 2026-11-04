#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise real camera buffering, PNG encoding, calibration and ownership."""
import argparse
import re
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
p.add_argument('cjson', type=Path)
p.add_argument('mbedtls', type=Path)
a = p.parse_args()
repo = a.core.parents[1]
store = (a.core / 'ny_product_store.c').read_text()
key = re.search(r'static int ny_product_store_key\([^;]+?\n\{.*?\n\}', store, re.S)
assert key
code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
int task_create(const char *, int, int, int (*)(int, char **), char *const []);
#include "ny_product_camera.c"
static bool camera_allowed = true;
bool ny_product_privacy_camera_allowed(void) { return camera_allowed; }
#include "ny_product_camera_gaze.c"
struct camera_test_task { int (*entry)(int, char **); };
static pthread_t tasks[16]; static int task_count;
static void *camera_test_run(void *arg) { struct camera_test_task *task=arg;int (*entry)(int,char**)=task->entry;free(task);entry(0,NULL);return NULL; }
int task_create(const char *name,int priority,int stack,int (*entry)(int,char**),char *const argv[]) {
 (void)argv;assert(!strcmp(name,"ny_camera")&&priority==100&&stack==16384&&task_count<16);
 struct camera_test_task *task=malloc(sizeof(*task));assert(task);task->entry=entry;
 int ret=pthread_create(&tasks[task_count],NULL,camera_test_run,task);assert(ret==0);return ++task_count;
}
static atomic_int fake_failure;
static int save_error;
static cJSON *saved;
static uint64_t saved_revision;
uint64_t ny_product_time_ms(bool monotonic)
{
  (void)monotonic;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
int camenc_capture_run(const struct camenc_capture_s *capture)
{
  assert(capture->mode==6 && capture->file==NULL);
  if(atomic_load(&fake_failure))return -ENODEV;
  uint8_t *nv12=malloc(NY_CAMERA_RAW_SIZE);assert(nv12);
  memset(nv12,16,NY_CAMERA_WIDTH*NY_CAMERA_HEIGHT);
  memset(nv12+NY_CAMERA_WIDTH*NY_CAMERA_HEIGHT,128,NY_CAMERA_RAW_SIZE/3);
  const uint8_t init[]={1,2,3,4};const uint8_t fragment[]={5,6,7,8,9};
  assert(capture->emit(capture->arg,CAMENC_SEG_INIT,init,sizeof(init),false)==0);
  unsigned int n=0;
  while(!capture->stop(capture->arg)){
    assert(capture->frame(capture->arg,nv12,NY_CAMERA_RAW_SIZE,640,480,640)==0);
    assert(capture->emit(capture->arg,CAMENC_SEG_FRAGMENT,fragment,sizeof(fragment),n++%5==0)==0);
    usleep(1000);
  }
  free(nv12);return 0;
}
int ny_product_store_read(const char *domain,cJSON **out,uint64_t *revision)
{
  assert(!strcmp(domain,"camera-gaze"));*revision=saved_revision;
  *out=saved?cJSON_Duplicate(saved,true):NULL;return saved?0:-ENOENT;
}
int ny_product_store_write(const char *domain,const cJSON *value,uint64_t expected,uint64_t *revision)
{
  assert(!strcmp(domain,"camera-gaze"));if(save_error)return save_error;
  if(expected!=saved_revision)return -ESTALE;
  cJSON_Delete(saved);saved=cJSON_Duplicate(value,true);*revision=++saved_revision;return 0;
}
static double gaze_x,gaze_y;
static int gaze_hold;
int ny_product_eyes_request(const struct ny_product_caller_s *caller,const char *topic,const cJSON *data,cJSON **result)
{
  assert(caller->role==NY_PRODUCT_OWNER&&!strcmp(topic,"eyes.gaze"));
  gaze_x=cJSON_GetObjectItemCaseSensitive(data,"x")->valuedouble;
  gaze_y=cJSON_GetObjectItemCaseSensitive(data,"y")->valuedouble;
  gaze_hold=cJSON_GetObjectItemCaseSensitive(data,"hold_ms")->valueint;
  assert(gaze_hold>=0&&gaze_hold<=5000);
  *result=cJSON_CreateObject();return 0;
}
''' + key.group() + r'''
static int call(const char *topic,const char *text,enum ny_product_role_e role,cJSON **out)
{
  struct ny_product_caller_s caller={"test",role,true};
  cJSON *in=cJSON_Parse(text);assert(in);*out=NULL;
  int ret=ny_product_camera_request(&caller,topic,in,out);
  if(ret==-ENOSYS)ret=ny_product_camera_gaze_request(&caller,topic,in,out);
  cJSON_Delete(in);return ret;
}
static void wait_stopped(void)
{
  for(int i=0;i<1000;i++){
    nxmutex_lock(&g_camera_lock);bool running=g_camera_running;nxmutex_unlock(&g_camera_lock);
    if(!running)return;
    usleep(1000);
  }
  assert(!"camera did not stop");
}
int main(int argc,char **argv)
{
  assert(argc==2);cJSON *out=NULL;char keybuf[64];
  assert(ny_product_store_key("camera-gaze",keybuf,sizeof(keybuf))==0);
  assert(ny_product_store_key("camera-gaze-other",keybuf,sizeof(keybuf))==-EINVAL);
  assert(call("camera.photo.capture","{}",NY_PRODUCT_GUEST,&out)==-EACCES);
  assert(call("camera.status","{}",NY_PRODUCT_FAMILY,&out)==-EACCES);
  assert(!g_camera_running);

  /* Deterministic ring: a late reader starts on an IDR, not a P frame. */
  g_camera_running=true;g_camera_readers=1;g_camera_generation=1;
  const uint8_t init[]={1,2,3,4};uint8_t frame[25000];memset(frame,23,sizeof(frame));
  struct ny_camera_client_s client={.active=true};
  assert(ny_camera_emit(NULL,CAMENC_SEG_INIT,init,sizeof(init),false)==0);
  assert(ny_camera_emit(NULL,CAMENC_SEG_FRAGMENT,frame,sizeof(frame),false)==0);
  assert(ny_product_camera_next(&client,&out)==-EAGAIN&&!out);
  assert(ny_camera_emit(NULL,CAMENC_SEG_FRAGMENT,frame,sizeof(frame),true)==0);
  assert(ny_product_camera_next(&client,&out)==0);
  assert(!strcmp(cJSON_GetObjectItem(out,"kind")->valuestring,"init"));
  cJSON_Delete(out);assert(client.sequence==2);
  for(int n=0;n<3;n++){
    assert(ny_product_camera_next(&client,&out)==0);
    assert(cJSON_GetObjectItem(out,"offset")->valueint==n*NY_CAMERA_CHUNK);
    assert(cJSON_GetObjectItem(out,"total")->valueint==25000);
    cJSON_Delete(out);
  }
  assert(client.sequence==3&&client.offset==0);
  assert(ny_product_camera_next(&client,&out)==-EAGAIN);
  for(int i=0;i<150;i++)assert(ny_camera_emit(NULL,CAMENC_SEG_FRAGMENT,frame,sizeof(frame),i%15==0)==0);
  assert(g_camera_bytes<=NY_CAMERA_BUFFER_MAX&&g_camera_last-g_camera_first<NY_CAMERA_SEGMENTS);
  assert(ny_product_camera_next(&client,&out)==0);
  assert(!strcmp(cJSON_GetObjectItem(out,"kind")->valuestring,"init"));cJSON_Delete(out);
  assert(ny_camera_emit(NULL,CAMENC_SEG_FRAGMENT,frame,NY_CAMERA_SEGMENT_MAX+1,false)==-EFBIG);
  ny_product_camera_close(&client);assert(ny_camera_stop(NULL));
  g_camera_running=false;ny_camera_clear();assert(g_camera_bytes==0);

  /* Real worker lifetime and snapshot producer, with synthetic sensor input. */
  assert(ny_product_camera_subscribe(&client)==0);
  assert(ny_product_camera_subscribe(&client)==0);
  struct ny_camera_client_s other={0};assert(ny_product_camera_subscribe(&other)==0);
  assert(g_camera_readers==2);
  uint8_t *png=NULL;size_t size=0;
  assert(ny_product_camera_photo(2,&png,&size)==0&&size>57);
  nxmutex_lock(&g_camera_lock);assert(g_camera_frames>=NY_CAMERA_PHOTO_WARMUP_FRAMES);nxmutex_unlock(&g_camera_lock);
  uint8_t *raw=malloc(NY_CAMERA_RAW_SIZE);assert(raw);uint64_t raw_sequence=0,raw_at=0;
  assert(ny_product_camera_raw_next(&other,raw,NY_CAMERA_RAW_SIZE,&raw_sequence,&raw_at)==-EAGAIN);
  int raw_ret=-EAGAIN;
  for(int n=0;n<100&&raw_ret==-EAGAIN;n++){usleep(1000);raw_ret=ny_product_camera_raw_next(&other,raw,NY_CAMERA_RAW_SIZE,&raw_sequence,&raw_at);}
  assert(raw_ret==0);
  assert(raw_sequence>0&&raw_at>0&&raw[0]==16&&raw[NY_CAMERA_WIDTH*NY_CAMERA_HEIGHT]==128);
  assert(ny_product_camera_raw_next(&other,raw,NY_CAMERA_RAW_SIZE-1,&raw_sequence,&raw_at)==-EINVAL);
  camera_allowed=false;assert(ny_product_camera_raw_next(&other,raw,NY_CAMERA_RAW_SIZE,&raw_sequence,&raw_at)==-EACCES);camera_allowed=true;
  struct ny_camera_client_s stale=other;stale.generation++;
  assert(ny_product_camera_raw_next(&stale,raw,NY_CAMERA_RAW_SIZE,&raw_sequence,&raw_at)==-EPIPE);free(raw);
  assert(!memcmp(png,"\x89PNG\r\n\x1a\n",8));
  FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(png,1,size,f)==size);fclose(f);free(png);
  assert(g_camera_readers==2&&g_camera_running);
  assert(call("camera.photo.capture","{}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);
  assert(call("camera.photo.read","{\"photoId\":1,\"offset\":0}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);
  assert(call("camera.photo.read","{\"photoId\":1,\"offset\":-1}",NY_PRODUCT_OWNER,&out)==-EINVAL);
  assert(call("camera.photo.read","{\"photoId\":1,\"offset\":0.1}",NY_PRODUCT_OWNER,&out)==-EINVAL);
  assert(call("camera.photo.read","{\"photoId\":2,\"offset\":0}",NY_PRODUCT_OWNER,&out)==-ENOENT);
  assert(call("camera.photo.clear","{}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);
  assert(call("camera.photo.read","{\"photoId\":1,\"offset\":0}",NY_PRODUCT_OWNER,&out)==-ENOENT);
  ny_product_camera_close(&client);assert(g_camera_readers==1);
  ny_product_camera_close(&other);wait_stopped();ny_product_camera_shutdown();assert(!g_camera_raw);
  atomic_store(&fake_failure,1);assert(ny_product_camera_subscribe(&client)==0);wait_stopped();
  assert(ny_product_camera_next(&client,&out)==-ENODEV);ny_product_camera_close(&client);
  assert(ny_product_camera_photo(1,&png,&size)==-ENODEV&&!png&&size==0);
  ny_product_camera_shutdown();assert(g_camera_readers==0);

  /* Fit five pairs, retaining a measurable centre residual. */
  struct ny_camera_gaze_point_s points[5]={{.1,.1,-.8,-.8},{.9,.1,.8,-.8},{.9,.9,.8,.8},{.1,.9,-.8,.8},{.5,.5,0,0}};
  struct ny_camera_gaze_s map;double x,y;
  assert(ny_camera_gaze_fit(points,&map)==0&&map.rms<1e-12);
  assert(ny_camera_gaze_map(&map,.2,.7,&x,&y)==0&&fabs(x+.6)<1e-12&&fabs(y-.4)<1e-12);
  assert(ny_camera_gaze_map(&map,NAN,.5,&x,&y)==-ERANGE);
  assert(ny_camera_gaze_map(&map,1.01,.5,&x,&y)==-ERANGE);
  for(int i=0;i<5;i++)points[i].x=-points[i].x;
  assert(ny_camera_gaze_fit(points,&map)==0);
  assert(ny_camera_gaze_map(&map,.2,.7,&x,&y)==0&&fabs(x-.6)<1e-12);
  points[4].x=.3;assert(ny_camera_gaze_fit(points,&map)==0&&map.rms>.1);
  points[4]=points[0];assert(ny_camera_gaze_fit(points,&map)==-EINVAL);
  for(int i=0;i<5;i++)points[i]=(struct ny_camera_gaze_point_s){i*.2,i*.2,i*.4-1,i*.4-1};
  assert(ny_camera_gaze_fit(points,&map)==-EINVAL);
  points[0].u=NAN;assert(ny_camera_gaze_fit(points,&map)==-ERANGE);
  assert(call("camera.calibration.get","{}",NY_PRODUCT_OWNER,&out)==0);
  assert(cJSON_IsFalse(cJSON_GetObjectItem(out,"calibrated")));cJSON_Delete(out);
  assert(call("camera.aim","{\"x\":0.3,\"y\":-0.2}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);
  assert(fabs(gaze_x-.3)<1e-12&&fabs(gaze_y+.2)<1e-12);
  assert(gaze_hold==1000);
  assert(call("camera.aim","{\"x\":0.3,\"y\":-0.2,\"hold_ms\":3000}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);assert(gaze_hold==3000);
  assert(call("camera.aim","{\"x\":0,\"y\":0,\"hold_ms\":0}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);assert(gaze_hold==0);
  assert(call("camera.aim","{\"x\":0,\"y\":0,\"hold_ms\":5001}",NY_PRODUCT_OWNER,&out)==-EINVAL);
  assert(call("camera.aim","{\"x\":2,\"y\":0}",NY_PRODUCT_OWNER,&out)==-EINVAL);
  assert(call("camera.aim","{\"x\":0,\"y\":0}",NY_PRODUCT_GUEST,&out)==-EACCES);
  const char *calibration="{\"revision\":0,\"width\":640,\"height\":480,\"mode\":6,\"points\":[{\"u\":0.1,\"v\":0.1,\"x\":-0.8,\"y\":-0.8},{\"u\":0.9,\"v\":0.1,\"x\":0.8,\"y\":-0.8},{\"u\":0.9,\"v\":0.9,\"x\":0.8,\"y\":0.8},{\"u\":0.1,\"v\":0.9,\"x\":-0.8,\"y\":0.8},{\"u\":0.5,\"v\":0.5,\"x\":0,\"y\":0}]}";
  assert(call("camera.calibration.set",calibration,NY_PRODUCT_GUEST,&out)==-EACCES);
  assert(call("camera.calibration.fit",calibration,NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);assert(saved_revision==0);
  save_error=-EIO;assert(call("camera.calibration.set",calibration,NY_PRODUCT_OWNER,&out)==-EIO);assert(!saved);save_error=0;
  assert(call("camera.calibration.set",calibration,NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);assert(saved_revision==1);
  assert(call("camera.calibration.set",calibration,NY_PRODUCT_OWNER,&out)==-ESTALE);
  assert(call("camera.point","{\"u\":0.2,\"v\":0.7,\"width\":640,\"height\":480,\"mode\":6}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);
  assert(fabs(gaze_x+.6)<1e-12&&fabs(gaze_y-.4)<1e-12);
  assert(call("camera.point","{\"u\":0.2,\"v\":0.7,\"width\":1280,\"height\":720,\"mode\":3}",NY_PRODUCT_OWNER,&out)==-EINVAL);
  assert(call("camera.calibration.clear","{\"revision\":1}",NY_PRODUCT_OWNER,&out)==0);cJSON_Delete(out);
  assert(call("camera.point","{}",NY_PRODUCT_OWNER,&out)==-ENODATA);
  cJSON_Delete(saved);
  uint8_t input[6]={16,16,16,16,128,128};
  assert(ny_camera_png(input,5,2,2,2,1,&png,&size)==-EINVAL);
  assert(ny_camera_png(input,6,2,2,2,3,&png,&size)==-EINVAL);
  for(int i=0;i<task_count;i++)pthread_join(tasks[i],NULL);
 camera_allowed=false;struct ny_camera_client_s denied={0};
 assert(ny_product_camera_subscribe(&denied)==-EACCES);
 cJSON *blocked=NULL;assert(ny_product_camera_next(&denied,&blocked)==-EACCES&&!blocked);
 uint8_t *blocked_png=NULL;size_t blocked_length=0;
 assert(ny_product_camera_photo(1,&blocked_png,&blocked_length)==-EACCES&&!blocked_png);
 puts("CAMERA_PASS bounded_media idr_resync chunks shared_capture stop restart_failure png permissions calibration cas geometry");
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='ny-camera-') as tmp:
    t = Path(tmp)
    (t / 'nuttx').mkdir()
    (t / 'netutils').mkdir()
    (t / 'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_PRODUCT 1\n#define CONFIG_EXAMPLES_CAMENC_PRIORITY 100\n')
    (t / 'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
    (t / 'netutils/cJSON.h').write_text('#include <cJSON.h>\n')
    (t / 'test.c').write_text(code)
    for name, flags in [('plain', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]:
        binary = t / name
        subprocess.run(['cc', '-D_DEFAULT_SOURCE', '-DFAR=', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-Wl,--gc-sections', *flags,
                        '-I'+str(t), '-I'+str(a.core), '-I'+str(a.cjson),
                        '-I'+str(repo/'app/camenc'), '-I'+str(a.mbedtls/'include'),
                        str(t/'test.c'), str(a.core/'ny_camera_png.c'), str(a.core/'ny_camera_gaze.c'),
                        str(a.cjson/'cJSON.c'), str(a.mbedtls/'library/base64.c'),
                        str(a.mbedtls/'library/constant_time.c'),
                        '-pthread', '-lz', '-lm', '-o', str(binary)], check=True)
        png = t / 'frame.png'
        subprocess.run([str(binary), str(png)], check=True)
        content = png.read_bytes()
        assert content[:8] == b'\x89PNG\r\n\x1a\n'
        position = 8
        packed = b''
        kinds = []
        while position < len(content):
            length, = struct.unpack_from('>I', content, position)
            kind = content[position+4:position+8]
            data = content[position+8:position+8+length]
            crc, = struct.unpack_from('>I', content, position+8+length)
            assert crc == zlib.crc32(kind+data)
            kinds.append(kind)
            if kind == b'IHDR':
                assert struct.unpack('>IIBBBBB', data) == (320, 240, 8, 2, 0, 0, 0)
            if kind == b'IDAT':
                packed += data
            position += length + 12
        assert kinds == [b'IHDR', b'IDAT', b'IEND']
        raw = zlib.decompress(packed)
        assert len(raw) == (320*3+1)*240 and not any(raw)
        print(f'PNG_DECODE_PASS {name} 320x240 RGB black CRC deflate')
