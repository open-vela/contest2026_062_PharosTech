#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise production per-peer ownership and AVRCP key transactions."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
core=Path(sys.argv[1]).resolve()
s=(core/'ny_product_bt.c').read_text()
def function(name):
    m=re.search(r'(?m)^(?:static\s+)?[\w\s*]+?\b'+name+r'\([^;]*?\)\n\{',s)
    assert m,name
    begin=m.start();i=m.end();depth=1
    for t in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',s[i:],re.S):
        if t.group()=='{':depth+=1
        elif t.group()=='}':depth-=1
        if depth==0:return s[begin:i+t.end()].strip()
    raise AssertionError(name)
functions=['ny_bt_peer_'+n for n in ['acl','a2dp','ct','tg','hf','call','sco','stream','address','add']]
functions += ['ny_bt_selected','ny_bt_select_media','ny_bt_connected','ny_bt_disconnected',
 'ny_bt_a2dp_connected','ny_bt_a2dp_disconnected','ny_bt_a2dp_established',
 'ny_bt_a2dp_released','ny_bt_a2dp_started','ny_bt_a2dp_suspended','ny_bt_a2dp_recv',
 'ny_bt_tid','ny_bt_ct_connected','ny_bt_ct_disconnected','ny_bt_ct_passthrough_rsp',
 'ny_bt_ct_event','ny_bt_ct_notification','ny_bt_ct_changed','ny_bt_passthrough','ny_bt_tick_avrcp',
 'ny_bt_tick_peer','ny_bt_call_set','ny_bt_hf_connected','ny_bt_hf_disconnected',
 'ny_bt_hf_incoming','ny_bt_hf_outgoing','ny_bt_hf_accept','ny_bt_hf_ended',
 'ny_bt_hf_service','ny_bt_hf_signal','ny_bt_hf_battery']
parts=[function(n) for n in functions]
prototypes='\n'.join(x[:x.index('\n{')]+';' for x in parts)
types=s[s.index('enum ny_bt_state_e'):s.index('/****************************************************************************\n * Private Function Prototypes')]
defines=s[s.index('#define NY_BT_DOMAIN'):s.index('/****************************************************************************\n * Private Types')]
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#define CONFIG_BT_MAX_CONN 2
#define BT_ADDR_STR_LEN 18
#define BT_CONN_TYPE_BR 1
#define BT_HCI_ERR_CONN_LIMIT_EXCEEDED 9
#define BT_HCI_ERR_CONN_TIMEOUT 8
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 19
#define BT_HFP_HF_CODEC_CVSD 1
#define BT_AVRCP_RSP_ACCEPTED 9
#define BT_AVRCP_RSP_NOT_IMPLEMENTED 8
#define BT_AVRCP_STATUS_SUCCESS 0
#define BT_AVRCP_OPID_PAUSE 0x46
#define BT_AVRCP_OPID_PLAY 0x44
#define BT_AVRCP_BUTTON_PRESSED 0
#define BT_AVRCP_BUTTON_RELEASED 1
#define BT_AVRCP_EVT_PLAYBACK_STATUS_CHANGED 1
#define BT_AVRCP_EVT_TRACK_CHANGED 2
#define BT_AVRCP_CAP_EVENTS_SUPPORTED 3
#define BT_AVRCP_MEDIA_ATTR_TITLE 1
#define BT_AVRCP_MEDIA_ATTR_ARTIST 2
#define LOG_INFO 6
#define LOG_DEBUG 7
static void syslog(int level,const char *format,...){(void)level;(void)format;}
typedef int bt_avrcp_rsp_t;
typedef pthread_mutex_t mutex_t;
#define nxmutex_lock pthread_mutex_lock
#define nxmutex_unlock pthread_mutex_unlock
struct bt_conn { char address[18];int refs; };
struct bt_conn_info {int type;};
struct bt_a2dp { struct bt_conn *conn; };
struct bt_avrcp_ct {int id;};struct bt_avrcp_tg {int id;};
struct bt_hfp_hf {int id;};struct bt_hfp_hf_call {int id;};
struct bt_a2dp_stream { struct bt_a2dp *a2dp; };
struct net_buf { uint8_t *data;size_t len; };
struct bt_avrcp_passthrough_rsp {uint8_t opid_state;uint8_t data_len;};
struct bt_avrcp_event_data {uint8_t play_status;};
static uint64_t now_ms;
static unsigned starts,stops,packets,disconnects;
static uint32_t selected_rate;
static uint64_t ny_product_time_ms(bool monotonic){(void)monotonic;return now_ms;}
static struct bt_conn *bt_conn_ref(struct bt_conn*c){c->refs++;return c;}
static void bt_conn_unref(struct bt_conn*c){c->refs--;assert(c->refs>=0);}
static int bt_conn_get_info(struct bt_conn*c,struct bt_conn_info*i){(void)c;i->type=BT_CONN_TYPE_BR;return 0;}
static int bt_conn_disconnect(struct bt_conn*c,int reason){(void)c;(void)reason;disconnects++;return 0;}
static struct bt_conn *bt_a2dp_get_conn(struct bt_a2dp*a){return a->conn;}
static void ny_bt_address(const struct bt_conn*c,char*out){strcpy(out,c->address);}
static void ny_bt_audio_media_stop(void){stops++;}
static void ny_bt_audio_media_start(void){starts++;}
static void ny_bt_audio_media_configure(uint32_t rate,uint8_t channels,uint16_t frames){(void)channels;(void)frames;selected_rate=rate;}
static void ny_bt_audio_media_packet(const void*p,size_t n){(void)p;(void)n;packets++;}
static unsigned call_stops;
static void ny_bt_audio_call_stop(void){call_stops++;}
struct sent_s {struct bt_avrcp_ct*ct;uint8_t tid,opid,state;};
static struct sent_s sent[32];static unsigned sent_count;static int send_error;
static int bt_avrcp_ct_passthrough(struct bt_avrcp_ct*ct,uint8_t tid,uint8_t opid,uint8_t state,const void*p,size_t n){
 (void)p;(void)n;if(send_error)return send_error;
 sent[sent_count++]=(struct sent_s){ct,tid,opid,state};return 0;
}
static int bt_avrcp_ct_get_caps(struct bt_avrcp_ct*c,uint8_t tid,uint8_t cap){(void)c;(void)tid;(void)cap;return 0;}
static int bt_avrcp_ct_register_notification(struct bt_avrcp_ct*c,uint8_t tid,uint8_t event,uint32_t interval,void(*cb)(struct bt_avrcp_ct*,uint8_t,struct bt_avrcp_event_data*)){(void)c;(void)tid;(void)event;(void)interval;(void)cb;return 0;}
static struct net_buf *bt_avrcp_create_vendor_pdu(void*p){(void)p;return NULL;}
static void net_buf_add_be32(struct net_buf*b,uint32_t v){(void)b;(void)v;}
static void net_buf_add_u8(struct net_buf*b,uint8_t v){(void)b;(void)v;}
static void net_buf_unref(struct net_buf*b){(void)b;}
static int bt_avrcp_ct_get_element_attrs(struct bt_avrcp_ct*c,uint8_t tid,struct net_buf*b){(void)c;(void)tid;(void)b;return 0;}
static int bt_avrcp_ct_get_play_status(struct bt_avrcp_ct*c,uint8_t tid){(void)c;(void)tid;return 0;}
'''
setup=r'''
static struct ny_bt_s g_bt={.lock=PTHREAD_MUTEX_INITIALIZER};
static const char *const g_bt_call_names[]={"idle","incoming","dialing","alerting","active","held"};
typedef struct {uint8_t val[6];} bt_addr_t;
static void ny_bt_tick_volume(struct ny_bt_peer_s*p){(void)p;}
static void ny_product_media_sleep(void){}
#define NY_PRODUCT_MEDIA_ALERT_ONCE 1
static void ny_product_media_alert(int alert){(void)alert;}
static void ny_product_notification_post(const char*id,const char*kind,const char*title,uint64_t until){(void)id;(void)kind;(void)title;(void)until;}
static const bt_addr_t *bt_conn_get_dst_br(struct bt_conn*c){(void)c;return NULL;}
static void ny_bt_remote_name(const bt_addr_t*a,const char*n){(void)a;(void)n;}
static int bt_br_remote_name_request(const bt_addr_t*a,void(*cb)(const bt_addr_t*,const char*)){(void)a;(void)cb;return 0;}
static struct bt_a2dp *bt_a2dp_connect(struct bt_conn*c){(void)c;return NULL;}
static int bt_avrcp_connect(struct bt_conn*c){(void)c;return 0;}

static struct ny_bt_device_s *ny_bt_device(const char*address,bool create){
 (void)create;for(unsigned i=0;i<NY_BT_DEVICES_MAX;i++){
 if(!g_bt.devices[i].address[0] || !strcmp(g_bt.devices[i].address,address)){
 strcpy(g_bt.devices[i].address,address);return &g_bt.devices[i];}}
 return NULL;
}
'''
main=r'''
int main(void){
 struct bt_conn a={"AA:00:00:00:00:01",1},b={"BB:00:00:00:00:02",1},third={"CC:00:00:00:00:03",1};
 struct bt_a2dp ad={&a},bd={&b};struct bt_avrcp_ct ac={1},bc={2};
 ny_bt_connected(&a,0);ny_bt_connected(&b,0);
 struct ny_bt_peer_s *pa=ny_bt_peer_acl(&a),*pb=ny_bt_peer_acl(&b);
 assert(pa && pb && pa!=pb && g_bt.selected==pa && a.refs==2 && b.refs==2);
 ny_bt_connected(&third,0);assert(disconnects==1 && third.refs==1);
 ny_bt_ct_connected(&a,&ac);ny_bt_ct_connected(&b,&bc);
 ny_bt_a2dp_connected(&ad,0);ny_bt_a2dp_connected(&bd,0);
 pa->stream.a2dp=&ad;pb->stream.a2dp=&bd;
 pa->a2dp_rate=44100;pb->a2dp_rate=48000;pa->frame_samples=pb->frame_samples=128;
 pa->a2dp_channels=pb->a2dp_channels=2;
 strcpy(pa->title,"Laptop title");strcpy(pb->title,"Phone title");
 ny_bt_a2dp_started(&pa->stream);assert(starts==1);
 ny_bt_a2dp_started(&pb->stream);assert(starts==1);
 uint8_t bytes[2]={1,2};struct net_buf packet={bytes,2};
 ny_bt_a2dp_recv(&pb->stream,&packet,0,0);assert(packets==0);
 ny_bt_a2dp_recv(&pa->stream,&packet,0,0);assert(packets==1);
 struct bt_avrcp_event_data ev={3};ny_bt_ct_event(&bc,BT_AVRCP_EVT_PLAYBACK_STATUS_CHANGED,&ev);
 assert(pb->play_status==3 && !pa->play_status_known && !strcmp(pa->title,"Laptop title"));
 unsigned before=stops;ny_bt_a2dp_suspended(&pb->stream);assert(stops==before);
 ny_bt_a2dp_started(&pb->stream);ny_bt_select_media(pb);
 assert(selected_rate==48000 && starts==2 && g_bt.selected==pb);
 ny_bt_a2dp_recv(&pa->stream,&packet,0,0);assert(packets==1);
 ny_bt_a2dp_recv(&pb->stream,&packet,0,0);assert(packets==2);
 assert(ny_bt_passthrough(pa,BT_AVRCP_OPID_PAUSE)==0);
 assert(ny_bt_passthrough(pa,BT_AVRCP_OPID_PLAY)==-EBUSY);
 assert(ny_bt_passthrough(pb,BT_AVRCP_OPID_PLAY)==0);
 assert(sent_count==2 && sent[0].ct==&ac && sent[1].ct==&bc);
 now_ms=NY_BT_KEY_RELEASE_MS;ny_bt_tick_avrcp(pa,0);
 assert(sent_count==3 && sent[2].ct==&ac && sent[2].state==BT_AVRCP_BUTTON_RELEASED);
 struct bt_avrcp_passthrough_rsp rsp={BT_AVRCP_OPID_PAUSE|0x80,0};
 ny_bt_ct_passthrough_rsp(&bc,sent[2].tid,BT_AVRCP_RSP_ACCEPTED,&rsp);assert(pa->key_pending);
 ny_bt_ct_passthrough_rsp(&ac,sent[2].tid+1,BT_AVRCP_RSP_ACCEPTED,&rsp);assert(pa->key_pending);
 ny_bt_ct_passthrough_rsp(&ac,sent[2].tid,BT_AVRCP_RSP_ACCEPTED,&rsp);assert(!pa->key_pending && pb->key_pending);
 assert(ny_bt_passthrough(pa,BT_AVRCP_OPID_PLAY)==0);
 rsp.opid_state=BT_AVRCP_OPID_PLAY;
 ny_bt_ct_passthrough_rsp(&ac,pa->key_tid,BT_AVRCP_RSP_NOT_IMPLEMENTED,&rsp);
 assert(!pa->key_pending && pa->control_error==-ENOTSUP);
 send_error=-ENOMEM;assert(ny_bt_passthrough(pa,BT_AVRCP_OPID_PLAY)==-ENOMEM);assert(!pa->key_pending);send_error=0;
 now_ms=NY_BT_KEY_TIMEOUT_MS+1;ny_bt_tick_avrcp(pb,0);
 assert(!pb->key_pending && pb->control_error==-ETIMEDOUT);
 before=stops;ny_bt_ct_disconnected(&ac);ny_bt_a2dp_disconnected(&ad);
 assert(stops==before && pb->ct==&bc && !strcmp(pb->title,"Phone title"));
 ny_bt_disconnected(&a,0);assert(g_bt.selected==pb && a.refs==1);
 ny_bt_ct_passthrough_rsp(&ac,0,BT_AVRCP_RSP_ACCEPTED,&rsp);
 assert(g_bt.selected==pb && pb->ct==&bc);
 ny_bt_disconnected(&b,0);assert(g_bt.selected==NULL && b.refs==1);
 // A timed-out page must release the slot even without a host callback.
 ny_bt_connected(&a,0);ny_bt_connected(&b,0);pa=ny_bt_peer_acl(&a);pb=ny_bt_peer_acl(&b);
 pa->connected=false;pa->acl_attempt_until=1;now_ms=2;
 ny_bt_tick_peer(pa,now_ms);assert(a.refs==1 && ny_bt_peer_acl(&a)==NULL && g_bt.selected==pb);
 ny_bt_connected(&third,0);struct ny_bt_peer_s*pc=ny_bt_peer_acl(&third);assert(pc);
 ny_bt_disconnected(&a,0);assert(ny_bt_peer_acl(&third)==pc);
 ny_bt_disconnected(&third,0);
 // Background HFP state and disconnects cannot overwrite another call.
 ny_bt_connected(&a,0);pa=ny_bt_peer_acl(&a);
 struct bt_hfp_hf ah={1},bh={2};struct bt_hfp_hf_call ca={1},cbcall={2};
 ny_bt_hf_connected(&a,&ah);ny_bt_hf_connected(&b,&bh);
 ny_bt_hf_service(&bh,1);ny_bt_hf_signal(&bh,5);assert(pb->hfp_signal==5 && pa->hfp_signal==-1);
 ny_bt_hf_incoming(&ah,&ca);ny_bt_hf_accept(&ca);assert(g_bt.call_peer==pa && pa->call_state==NY_BT_CALL_ACTIVE);
 ny_bt_hf_incoming(&bh,&cbcall);assert(g_bt.call_peer==pa);
 ny_bt_hf_ended(&cbcall);ny_bt_hf_disconnected(&bh);assert(g_bt.call_peer==pa && call_stops==0);
 ny_bt_hf_ended(&ca);assert(g_bt.call_peer==NULL);
 ny_bt_disconnected(&a,0);ny_bt_disconnected(&b,0);
 puts("BT_PEER_CONTROL_PASS independent_profiles selected_packets targeted_keys fallback_release stale_callbacks");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-bt-peers-') as directory:
    p=Path(directory);c=p/'test.c'
    c.write_text(code+defines+types+setup+prototypes+'\n'+'\n\n'.join(parts)+main)
    for name,flags in [('plain',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=p/name
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,str(c),'-pthread','-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=15)
