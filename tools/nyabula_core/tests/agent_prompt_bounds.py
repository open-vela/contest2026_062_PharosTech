#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile actual agent prompt sources with isolated host fixtures and ASan."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('agent', type=pathlib.Path)
parser.add_argument('cjson', type=pathlib.Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='nyabot-prompt-bounds-') as temp:
    root = pathlib.Path(temp)
    skills = root / 'skills'
    skills.mkdir()
    for index in range(40):
        (skills / f'skill-{index}.md').write_text('# ' + 'T' * 120 + '\n' + 'D' * 250 + '\n')
    (root / 'SOUL.md').write_text('S' * 12000)
    (root / 'USER.md').write_text('U' * 12000)
    (root / 'agent_config.h').write_text(
        f'#define AGENT_DATA_DIR "{root}"\n'
        f'#define AGENT_MEMORY_DIR "{root}"\n'
        f'#define AGENT_SKILLS_DIR "{skills}/"\n'
        f'#define AGENT_SOUL_FILE "{root}/SOUL.md"\n'
        f'#define AGENT_USER_FILE "{root}/USER.md"\n'
        '#define AGENT_TIMEZONE "Asia/Shanghai"\n')
    harness = root / 'bounds.c'
    harness.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/context_builder.h"
#include "tools/skill_loader.h"
void tool_registry_invalidate(void) {}
char *tool_registry_get_tools_json(void) {
    char *result = malloc(6000);
    strcpy(result, "[{\"name\":\"");
    memset(result + strlen(result), 'N', 4000);
    strcpy(result + 4010, "\"}]");
    return result;
}
int memory_read_long_term(char *buf, size_t size) {
    if (size) { memset(buf, 'M', size - 1); buf[size - 1] = 0; }
    return 0;
}
int memory_read_recent(char *buf, size_t size, int days) {
    (void)days; return memory_read_long_term(buf, size);
}
int main(void) {
    const size_t sizes[] = {1, 2, 8, 64, 1024, 4096, 32768};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        char *buf = malloc(n);
        memset(buf, 0xa5, n);
        size_t used = skill_loader_build_summary(buf, n);
        assert(used < n && buf[used] == 0 && strlen(buf) == used);
        memset(buf, 0xa5, n);
        assert(context_build_system_prompt(buf, n) == 0);
        assert(memchr(buf, 0, n) != NULL);
        assert(context_build_messages("[]", "hello", buf, n) == 0);
        assert(memchr(buf, 0, n) != NULL);
        free(buf);
    }
    assert(skill_loader_build_summary(NULL, 0) == 0);
    assert(context_build_system_prompt(NULL, 0) < 0);
    assert(context_build_messages("[]", "hello", NULL, 0) < 0);
    puts("NYABOT_PROMPT_BOUNDS_PASS sizes=1,2,8,64,1024,4096,32768 skills=40 long_tools=true");
    return 0;
}
''')
    output = root / 'bounds'
    subprocess.run(['cc', '-g', '-fsanitize=address,undefined', '-DOK=0', '-DERROR=-1',
                    '-I' + str(root), '-I' + str(args.agent / 'include'),
                    '-I' + str(args.agent / 'src'), '-I' + str(args.cjson.parent),
                    str(harness), str(args.agent / 'src/core/context_builder.c'),
                    str(args.agent / 'src/tools/skill_loader.c'), str(args.cjson),
                    '-pthread', '-lm', '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)
