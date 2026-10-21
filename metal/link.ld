ENTRY(_start)
SECTIONS {
  . = 1M;
  .multiboot2 : { KEEP(*(.multiboot2)) }
  .text   : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) KEEP(*(.gcc_except_table .gcc_except_table.*)) }
  .eh_frame : { PROVIDE(__eh_frame_start = .); KEEP(*(.eh_frame .eh_frame.*)) LONG(0) }
  .init_array : { PROVIDE(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE(__init_array_end = .); }
  .data   : { *(.data .data.* .got .got.plt .data.rel.ro .data.rel.ro.*) }
  /* The thread-local storage image. Each core gets a copy and points FS at it
   * (metal/main.cpp metal_make_tls_block); thread_locals read through FS. */
  .tdata  : ALIGN(4096) { PROVIDE(__tdata_start = .); *(.tdata .tdata.* .gnu.linkonce.td.*) PROVIDE(__tdata_end = .); }
  .tbss   : ALIGN(4096) { PROVIDE(__tbss_start = .); *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) PROVIDE(__tbss_end = .); }
  .bss    : { PROVIDE(__bss_start = .); *(COMMON) *(.bss .bss.*) . = ALIGN(8); PROVIDE(__kernel_end = .); }
  /DISCARD/ : { *(.comment) *(.note.*) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.rela.*) }
}
