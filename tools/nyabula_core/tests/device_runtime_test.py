#!/usr/bin/env python3
"""Exercise the production SMP load aggregator with known idle samples."""
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[3]
source = (root / 'app/nyabula_core/ny_product_device.c').read_text()
start = source.index('static cJSON *ny_product_device_cpu(void)\n{')
end = source.index('/****************************************************************************', start)
code = r'''#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_SMP 1
#define CONFIG_SMP_NCPUS 4
typedef struct {bool available; double percent; int cores;} cJSON;
struct cpuload_s {long total; long active;};
static struct cpuload_s samples[4]; static int failed=-1; static cJSON result;
static cJSON *cJSON_CreateObject(void){memset(&result,0,sizeof(result));return &result;}
static void cJSON_AddBoolToObject(cJSON *v,const char*k,bool b){(void)k;v->available=b;}
static void cJSON_AddNumberToObject(cJSON*v,const char*k,double n){if(!strcmp(k,"percent"))v->percent=n;if(!strcmp(k,"cores"))v->cores=n;}
static void cJSON_AddStringToObject(cJSON*v,const char*k,const char*n){(void)v;(void)k;(void)n;}
static int clock_cpuload(int i,struct cpuload_s*out){*out=samples[i];return i==failed?-1:0;}
'''+source[start:end]+r'''
int main(void){
 for(int i=0;i<4;i++)samples[i]=(struct cpuload_s){400,100};
 assert(ny_product_device_cpu()->available && result.percent==0 && result.cores==4);
 samples[0].active=0;ny_product_device_cpu();assert(result.percent==25);
 for(int i=0;i<4;i++)samples[i].active=0;
 ny_product_device_cpu();assert(result.percent==100);
 failed=2;assert(!ny_product_device_cpu()->available);failed=-1;
 samples[0].total=0;assert(!ny_product_device_cpu()->available);
 for(int i=0;i<4;i++)samples[i]=(struct cpuload_s){100,100};
 ny_product_device_cpu();assert(result.percent==0);
 puts("DEVICE_RUNTIME_SMP_PASS all_idle one_busy all_busy failed zero_samples clamp");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
