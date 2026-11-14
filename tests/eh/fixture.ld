/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { *(.text) }
  .eh_frame_hdr : { __eh_frame_hdr = .; *(.eh_frame_hdr) }
  .eh_frame : { *(.eh_frame) }
  .gcc_except_table : { *(.gcc_except_table) }
  .got : { *(.got) }
}
