#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real client query and daemon over a raw PTY, not RPMsg HW."""
import os
import pathlib
import pty
import subprocess
import sys
import tempfile
import tty

repo=pathlib.Path(sys.argv[1]).resolve()
daemon=pathlib.Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="amp-app-pty-") as directory:
    root=pathlib.Path(directory)
    (root/'nuttx/rpmsg').mkdir(parents=True)
    (root/'nuttx/config.h').write_text('')
    (root/'nuttx/rpmsg/rpmsg.h').write_text('''#include <stdint.h>
#define RPMSG_ADDR_ANY 0xffffffffU
#define RPMSG_CREATE_DEV_IOCTL 1
struct rpmsg_endpoint_info {char name[32]; uint32_t src,dst;};
''')
    source=repo/'app/nyampctl/nyampctl_main.c'
    (root/'client.c').write_text(f'''#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include "nyamp_protocol.h"
/* A PTY is a stream; preserve the message boundaries provided by RPMsg. */
static ssize_t nyamp_test_read(int fd,void *buffer,size_t capacity) {{
 uint8_t *wire=buffer;
 size_t used=0, needed=NYAMP_WIRE_HEADER_SIZE;
 while(used<needed) {{
  ssize_t got=read(fd,wire+used,needed-used);
  if(got<=0)return got;
  used+=(size_t)got;
  if(used==NYAMP_WIRE_HEADER_SIZE) {{
   needed+=wire[36]|((uint32_t)wire[37]<<8)|((uint32_t)wire[38]<<16)|((uint32_t)wire[39]<<24);
   if(needed>capacity){{errno=EMSGSIZE;return -1;}}
  }}
 }}
 return (ssize_t)used;
}}
#define read nyamp_test_read
#define main unused_nyampctl_main
#include "{source}"
#undef main
''')
    protocol=repo/'tools/amp/protocol'
    client_dir=repo/'app/nyampctl'
    (root/'driver.c').write_text(f'''#include "nyamp_protocol.h"
#include "nyampctl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* argv[1] is fd, argv[2] selects the entry point under test. */
int main(int argc,char **argv) {{
 int fd, rc;
 if(argc<3)return 2;
 fd=atoi(argv[1]);
 if(strcmp(argv[2],"health")==0) {{
  if(argc!=4)return 2;
  rc=nyampctl_query(fd,(uint16_t)atoi(argv[3]));
 }} else if(strcmp(argv[2],"load")==0) {{
  if(argc!=4)return 2;
  rc=nyampctl_llm_load(fd,argv[3]);
 }} else if(strcmp(argv[2],"generate")==0) {{
  if(argc!=4)return 2;
  rc=nyampctl_llm_generate(fd,argv[3],4,0);
 }} else {{
  return 2;
 }}
 if(rc<0){{fprintf(stderr,"nyampctl: failed: %d\\n",rc);return 1;}}
 return 0;
}}
''')
    # The real shared-region client is excluded here: it reaches the region
    # directly through a chip header that only exists in a firmware build, and
    # this harness exercises the transport, not the region.
    (root / 'shmem_stub.c').write_text(
        '#include "nyampctl.h"\n'
        'int nyampctl_shmem_test(bool keep)\n'
        '{\n'
        '  (void)keep;\n'
        '  return -1;\n'
        '}\n')
    subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-Wall','-Werror',
                    '-I',str(root),'-I',str(protocol),'-I',str(client_dir),
                    str(root/'client.c'), str(root/'driver.c'),
                    str(client_dir/'nyampctl_llm.c'), str(root/'shmem_stub.c'),
                    str(protocol/'nyamp_protocol.c'),'-o',str(root/'client')],
                   check=True)
    master,slave=pty.openpty()
    tty.setraw(slave)
    process=subprocess.Popen([str(daemon),os.ttyname(slave)],stderr=subprocess.PIPE,text=True)
    try:
        for opcode in (1,2):
            result=subprocess.run([str(root/'client'),str(master),'health',str(opcode)],
                                  pass_fds=(master,),capture_output=True,text=True,timeout=10)
            assert result.returncode==0, result
            assert ('nyamp health ok:' if opcode==1 else 'online=') in result.stdout
            print(result.stdout,flush=True)

        # The real daemon has no LLM backend compiled in, so a load must be
        # refused rather than reported as working.  This still exercises the
        # client's real framing and response correlation on the wire.
        load=subprocess.run([str(root/'client'),str(master),'load','/data/model'],
                            pass_fds=(master,),capture_output=True,text=True,timeout=10)
        assert load.returncode!=0, load
        assert 'status=-9' in load.stderr, load.stderr
        print('llm load refused as unsupported without a backend',flush=True)

        print('NYAMP_APPLICATION_PTY_PASS')
    finally:
        process.terminate()
        process.communicate(timeout=5)
        os.close(master)
        os.close(slave)
