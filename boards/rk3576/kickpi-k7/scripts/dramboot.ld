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
  /* NuttX is loaded by u-boot to a fixed RAM address and started exactly
   * like a Linux kernel image placed at the same spot (it is no longer a
   * BL33 stage handed off by ATF). U-boot copies the image to 0x40200000
   * and jumps there, so the link address must match CONFIG_RAM_START
   * (0x40200000). 
   *
   * (Historically this board also ran NuttX as BL33 in the vendor boot
   * chain idbloader/SPL -> BL31/ATF -> NuttX, packed into the "uboot" slot
   * of the vendor FIT image at the same 0x40200000 load address. The
   * address requirement is identical in either scheme, which is why the
   * link base must equal CONFIG_RAM_START regardless of how the image is
   * launched.)
   */
  . = 0x40200000;  /* = CONFIG_RAM_START; u-boot loads and jumps to NuttX here */
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

  /* BSS is NOLOAD: arm64_data_initialize() in arm64_head.S zeroes only
   * the _sbss.._ebss range at runtime, so BSS must NOT be stored in the
   * image.  The init stack is also NOLOAD: it carries no meaningful
   * initial content (the boot code simply points the SP at
   * g_idle_topstack), so persisting it would only add dead zeros.
   * Keeping either section inside a PT_LOAD segment instead expands a
   * huge all-zero region into nuttx.bin, which (a) wastes flash and
   * (b) enlarges the packed image past the bootloader's small SRAM buffer
   * and corrupts the boot chain (see tools/k7_sdpack/build_sd.sh notes).
   */

  .bss (NOLOAD) : {           /* BSS */
       . = ALIGN(8);
       _sbss = .;
       *(.bss .bss.*)
       . = ALIGN(8);
       _ebss = .;
  } :text
  _szbss = _ebss - _sbss;

  .initstack (NOLOAD) : {   /* INIT STACK */
       . = ALIGN(8);
       _s_initstack = .;
       *(.initstack)
       . = ALIGN(16);
  } :text
  . = ALIGN(4096);
  _e_initstack = . ;
  g_idle_topstack = . ;

  /* NOTE: _szdata must span data + bss + initstack (i.e. up to
   * _e_initstack), NOT just _edata.  arm64_mmu.c() builds the MMU
   * read-write mapping for the whole NuttX data region from _szdata
   * (see g_mmu_nxrt_regions "nx_data").  If _szdata were truncated to
   * _edata - _sdata (16 bytes), bss/initstack would end up unmapped and
   * the first access after MMU is enabled faults.  NOLOAD only removes the
   * zero-fill from the on-media image; it does not change the runtime
   * VMA layout, so the runtime extent must still reach _e_initstack.
   *
   * _e_initstack must also be page (4096) aligned: init_xlat_tables()
   * asserts that every map region's size is aligned to PAGE_SIZE (see
   * arm64_mmu.c __MMU_ASSERT at "address/size are not page aligned").
   * Keeping ALIGN(4096) before _e_initstack satisfies both the MMU
   * assert and keeps _szdata page-aligned.
   */

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

