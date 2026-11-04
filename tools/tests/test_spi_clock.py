#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real SPI divider writer and rate reader with clock fixtures."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
args = parser.parse_args()
source = args.source.read_text()
model = re.search(r"^#define RK3576_SPI_SCLK_DIV .*", source, re.M)
functions = []
for name in ['rk3576_spi_setbaudr', 'rk3576_spi_getbaudr']:
    match = re.search(r'static (?:void|uint32_t)\s+' + name + r'\([^;]+?\n\{.*?\n\}', source, re.S)
    assert match, name
    functions.append(match.group())
code = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define DEBUGASSERT assert
#define spiwarn(...) ((void)0)
#define RK3576_SPI_NUM_CLKSRC 4
#define RK3576_SPI_BAUDR_MAX 65534
#define RK3576_SPI_BAUDR_MASK 65535
#define RK3576_SPI_BAUDR_OFFSET 16
#define RK3576_SPI_ENR_OFFSET 8
struct clk_s { uint32_t rate; int num_parents; struct clk_s *parent; struct clk_s *parents[4]; };
struct rk3576_spi_priv_s { struct clk_s *sclk; };
static uint32_t enabled=1, divisor;
static struct clk_s *clk_get_parent(struct clk_s *c) { return c->parent; }
static struct clk_s *clk_get_parent_by_index(struct clk_s *c, int i) { return c->parents[i]; }
static uint32_t clk_get_rate(struct clk_s *c) { return c->rate ? c->rate : c->parent ? clk_get_rate(c->parent) : 0; }
static void clk_set_parent(struct clk_s *c, struct clk_s *p) { assert(enabled==0); c->parent=p; }
static uint32_t spi_getreg(struct rk3576_spi_priv_s *p, unsigned int off) { (void)p; return off==8 ? enabled : divisor; }
static void spi_putreg(struct rk3576_spi_priv_s *p, unsigned int off, uint32_t v) {
 (void)p; if(off==8)enabled=v;else {assert(off==16&&enabled==0);divisor=v;}
}
""" + (model.group() + "\n" if model else "") + '\n'.join(functions) + r"""
int main(void) {
 struct clk_s c100={.rate=100000000}, c24={.rate=24000000};
 struct clk_s mux={.num_parents=1,.parent=&c100,.parents={&c100}};
 struct clk_s gate={.parent=&mux};struct rk3576_spi_priv_s p={.sclk=&gate};
 rk3576_spi_setbaudr(&p,6400000);
 assert(enabled==1 && divisor==16 && rk3576_spi_getbaudr(&p)==6250000);
 rk3576_spi_setbaudr(&p,1000000);
 assert(divisor==100 && rk3576_spi_getbaudr(&p)==1000000);
 mux.num_parents=2;mux.parents[1]=&c24;
 rk3576_spi_setbaudr(&p,4000000);
 assert(mux.parent==&c24 && divisor==6 && rk3576_spi_getbaudr(&p)==4000000);
 divisor=0;assert(rk3576_spi_getbaudr(&p)==0);
 puts("SPI_CLOCK_PASS source_over_baudr even_divisor restore_enable parent_selection");
}
"""
with tempfile.TemporaryDirectory(prefix='spi-clock-') as directory:
    root=Path(directory);(root/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(root/'test.c'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
