#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prove AVRCP passthrough dispatch does not depend on the subunit callback."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
BEFORE = 'static void avrcp_pass_through_rsp_handler(struct bt_avrcp *avrcp, uint8_t tid, struct net_buf *buf)\n{\n\tstruct bt_avrcp_header *avrcp_hdr;\n\tstruct bt_avrcp_passthrough_rsp *rsp;\n\tbt_avrcp_rsp_t result;\n\n\tavrcp_hdr = net_buf_pull_mem(buf, sizeof(*avrcp_hdr));\n\n\tif ((avrcp_ct_cb != NULL) && (avrcp_ct_cb->subunit_info_rsp != NULL)) {\n\t\tif (buf->len < sizeof(*rsp)) {\n\t\t\tLOG_ERR("Invalid passthrough length: %d", buf->len);\n\t\t\treturn;\n\t\t}\n\n\t\tresult = BT_AVRCP_HDR_GET_CTYPE_OR_RSP(avrcp_hdr);\n\t\trsp = (struct bt_avrcp_passthrough_rsp *)buf->data;\n\n\t\tavrcp_ct_cb->passthrough_rsp(get_avrcp_ct(avrcp), tid, result, rsp);\n\t}\n}'
PREFIX = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define LOG_ERR(...) ((void)0)
#define BT_AVRCP_HDR_GET_CTYPE_OR_RSP(h) ((h)->byte0 & 15)
typedef int bt_avrcp_rsp_t;
struct bt_avrcp_header {uint8_t byte0, byte1, opcode;};
struct bt_avrcp_passthrough_rsp {uint8_t opid_state, data_len;};
struct bt_avrcp_ct {int id;};
struct bt_avrcp {struct bt_avrcp_ct ct;};
struct net_buf {uint8_t *data;size_t len;};
static void *net_buf_pull_mem(struct net_buf *b,size_t n){assert(b->len>=n);void*p=b->data;b->data+=n;b->len-=n;return p;}
static struct bt_avrcp_ct *get_avrcp_ct(struct bt_avrcp*a){return &a->ct;}
static unsigned calls;
static void received(struct bt_avrcp_ct*c,uint8_t tid,bt_avrcp_rsp_t result,const struct bt_avrcp_passthrough_rsp*r){assert(c->id==7);assert(tid==3);assert(result==9);assert(r->opid_state==0xc4);calls++;}
static void unrelated(void){}
struct callbacks {void(*subunit_info_rsp)(void);void(*passthrough_rsp)(struct bt_avrcp_ct*,uint8_t,bt_avrcp_rsp_t,const struct bt_avrcp_passthrough_rsp*);};
static struct callbacks cb;
static struct callbacks *avrcp_ct_cb=&cb;
"""
MAIN = r"""
static void run(size_t length){uint8_t wire[]={9,0x48,0x7c,0xc4,0};struct net_buf b={wire,length};struct bt_avrcp a={.ct={7}};avrcp_pass_through_rsp_handler(&a,3,&b);}
int main(void){
 cb.passthrough_rsp=received;run(5);
#ifdef BASELINE
 assert(calls==0);puts("AVRCP_BASELINE_DROPS_REGISTERED_PASSTHROUGH");
#else
 assert(calls==1);
 cb.passthrough_rsp=NULL;cb.subunit_info_rsp=unrelated;run(5);assert(calls==1);
 cb.passthrough_rsp=received;run(4);assert(calls==1);
 avrcp_ct_cb=NULL;run(5);assert(calls==1);
 puts("AVRCP_PASSTHROUGH_CALLBACK_PASS dispatched null_callback truncated missing_callbacks");
#endif
}
"""
with tempfile.TemporaryDirectory(prefix='ny-avrcp-callback-') as directory:
    root=Path(directory)
    target=root/'subsys/bluetooth/host/classic/avrcp.c'
    target.parent.mkdir(parents=True)
    target.write_text(BEFORE+'\n')
    patch=ROOT/'patches/zblue-avrcp-passthrough-callback.patch'
    subprocess.run(['patch','--batch','-p1','-i',str(patch)],cwd=root,check=True,capture_output=True)
    after=target.read_text()
    for label,handler,flags in [('before',BEFORE,['-DBASELINE']),('after',after,[]),('sanitized',after,['-fsanitize=address,undefined'])]:
        source=root/(label+'.c');source.write_text(PREFIX+handler+MAIN)
        binary=root/label
        subprocess.run(['cc','-std=c11','-Wall','-Wextra',*flags,str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
