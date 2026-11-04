#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Test the production RKNN adapter with reordered and NHWC SDK tensors."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parent
header=r"""#pragma once
#include <cstdint>
using rknn_context=std::uint64_t;
enum {RKNN_QUERY_IN_OUT_NUM,RKNN_QUERY_INPUT_ATTR,RKNN_QUERY_OUTPUT_ATTR};
enum {RKNN_TENSOR_NCHW,RKNN_TENSOR_NHWC};
enum {RKNN_TENSOR_UINT8,RKNN_TENSOR_FLOAT32};
struct rknn_input_output_num{unsigned n_input,n_output;};
struct rknn_tensor_attr{unsigned index=0,n_elems=0,n_dims=0,dims[16]={};int fmt=0;};
struct rknn_input{unsigned index=0,size=0;int type=0,fmt=0;void *buf=nullptr;};
struct rknn_output{unsigned index=0,size=0;int want_float=0;void *buf=nullptr;};
"""
code=r"""
#include <rknn_api.h>
#include <cassert>
#include <cstdio>
#include <cstring>
static int mode;static bool bad;
int rknn_init(rknn_context *c,void *p,unsigned,unsigned,void*){const char*s=static_cast<char*>(p);*c=strstr(s,"template")?1:strstr(s,"search")?2:3;return 0;}
int rknn_destroy(rknn_context){return 0;}
int rknn_query(rknn_context c,int command,void *data,unsigned){
 if(command==RKNN_QUERY_IN_OUT_NUM){auto *n=static_cast<rknn_input_output_num*>(data);n->n_input=n->n_output=c==3?2:1;return 0;}
 auto *a=static_cast<rknn_tensor_attr*>(data);unsigned slot=c==3&&mode?1-a->index:a->index;
 if(command==RKNN_QUERY_INPUT_ATTR){a->n_elems=c==1?(bad?255*255*3:127*127*3):c==2?255*255*3:slot?48*16*16:48*8*8;return 0;}
 unsigned channels=c==3?(slot?4:2):48,side=c==1?8:16;a->n_elems=channels*side*side;a->n_dims=4;a->dims[0]=1;
 a->fmt=mode==2?RKNN_TENSOR_NHWC:RKNN_TENSOR_NCHW;
 a->dims[1]=mode==2?side:channels;a->dims[2]=side;a->dims[3]=mode==2?channels:side;return 0;
}
int rknn_inputs_set(rknn_context c,unsigned count,rknn_input *inputs){
 if(c==3){assert(count==2);for(unsigned i=0;i<2;i++){assert(inputs[i].index==(mode?1-i:i));assert(inputs[i].size==(i?48*16*16:48*8*8)*sizeof(float));}}
 return 0;
}
int rknn_run(rknn_context,void*){return 0;}
int rknn_outputs_get(rknn_context c,unsigned count,rknn_output *outputs,void*){
 for(unsigned i=0;i<count;i++){
  rknn_tensor_attr a;a.index=outputs[i].index;rknn_query(c,RKNN_QUERY_OUTPUT_ATTR,&a,sizeof(a));
  unsigned slot=c==3&&mode?1-outputs[i].index:outputs[i].index,side=c==1?8:16,channels=c==3?(slot?4:2):48;
  float *p=new float[a.n_elems];outputs[i].buf=p;outputs[i].size=a.n_elems*sizeof(float);
  for(unsigned ch=0;ch<channels;ch++)for(unsigned j=0;j<side*side;j++)p[mode==2?j*channels+ch:ch*side*side+j]=slot*1000+ch*side*side+j;
 }
 return 0;
}
int rknn_outputs_release(rknn_context,unsigned count,rknn_output *outputs){for(unsigned i=0;i<count;i++)delete[] static_cast<float*>(outputs[i].buf);return 0;}
#include "nyamp_tracker_rknn.cpp"
int main(){
 for(mode=0;mode<3;mode++){
  nyamp::vision::RknnTrackerBackend b;assert(b.Load("/fixture"));
  std::vector<float> exemplar(48*8*8),search(48*16*16),scores,boxes;
  assert(b.Match(exemplar,search,&scores,&boxes));assert(scores.size()==512&&boxes.size()==1024);
  assert(scores[256]==256&&boxes[512]==1512);
 }
 bad=true;nyamp::vision::RknnTrackerBackend b;assert(!b.Load("/fixture"));
 puts("TRACKER_RKNN_BINDING_PASS reordered_inputs_outputs nchw_nhwc wrong_shape_rejected");
}
"""
with tempfile.TemporaryDirectory(prefix='tracker-rknn-') as directory:
 t=Path(directory);(t/'rknn_api.h').write_text(header);(t/'test.cpp').write_text(code)
 for flags in [['-O2'],['-O1','-g','-fsanitize=address,undefined']]:
  exe=t/'test';subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(t),'-I'+str(root),str(t/'test.cpp'),str(root/'nyamp_tracker.cpp'),str(root/'nyamp_vision.cpp'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
