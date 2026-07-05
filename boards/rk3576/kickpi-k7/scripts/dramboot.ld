/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/scripts/dramboot.ld
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

OUTPUT_ARCH(aarch64)

ENTRY(__start)

PHDRS
{
  text PT_LOAD ;
}

SECTIONS
{
  /* NuttX runs as BL33 in the Rockchip vendor boot chain
   * (idbloader/SPL -> BL31/ATF -> NuttX). It is packed into the "uboot"
   * slot of the vendor FIT image, whose load address is 0x40200000 (verified
   * on hardware with dumpimage). BL31 exits to this address at EL2, so the
   * link address must match CONFIG_RAM_START (0x40200000); the previous
   * 0x02080000 was inherited from the rk3399 port and left NuttX linked at an
   * address it was never loaded to, hanging silently right after the BL31 handoff.
   */
  . = 0x40200000;  /* = CONFIG_RAM_START; BL31 hands off to NuttX (BL33) here */
  _start = .;
  .text : {
        _stext = .;            /* Text section */
       *(.start .start.*)      /* Place __start here */
       *(.text)
       *(.text.cold)
       *(.text.unlikely)
       *(.fixup)
       *(.gnu.warning)
  } :text = 0x9090

  . = ALIGN(4096);

  .init_section : {
        _sinit = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        _einit = ABSOLUTE(.);
  }

  . = ALIGN(4096);

  .vector : {
        _vector_start = .;
        KEEP(*(.exc_vector_table))
        KEEP(*(".exc_vector_table.*"))
        KEEP(*(.vectors))
       _vector_end = .;
  } :text
  . = ALIGN(4096);
  _etext = .; /* End_1 of .text */
  _sztext = _etext - _stext;

  . = ALIGN(4096);
  .rodata : {
        _srodata = .;          /* Read-only data */
       *(.rodata)
       *(.rodata.*)
       *(.data.rel.ro)
       *(.data.rel.ro.*)
  } :text
  . = ALIGN(4096);
  _erodata = .;                /* End of read-only data */
  _szrodata = _erodata - _srodata;
  _eronly = .;  /* End of read-only data */

  . = ALIGN(4096);
  .data : {                    /* Data */
        _sdata = .;
       *(.data.page_aligned)
       *(.data)
       . = ALIGN(8);
       *(.data.rel)
       *(.data.rel.*)
       CONSTRUCTORS
  } :text
  _edata = .; /* End+1 of .data */

  .bss : {                     /* BSS */
       . = ALIGN(8);
       _sbss = .;
       *(.bss)
       . = ALIGN(8);
  } :text
  . = ALIGN(4096);
  _ebss = .;
  _szbss = _ebss - _sbss;

  .initstack : {             /* INIT STACK */
       _s_initstack = .;
       *(.initstack)
       . = ALIGN(16);
  } :text
  . = ALIGN(4096);
  _e_initstack = . ;
  g_idle_topstack = . ;

  _szdata = _e_initstack - _sdata;

  /* Sections to be discarded */
  /DISCARD/ : {
       *(.exit.text)
       *(.exit.data)
       *(.exitcall.exit)
       *(.eh_frame)
       *(.note.gnu.build-id)
  }

  /* Stabs debugging sections.  */
  .stab 0 : { *(.stab) }
  .stabstr 0 : { *(.stabstr) }
  .stab.excl 0 : { *(.stab.excl) }
  .stab.exclstr 0 : { *(.stab.exclstr) }
  .stab.index 0 : { *(.stab.index) }
  .stab.indexstr 0 : { *(.stab.indexstr) }
  .comment 0 : { *(.comment) }
}

