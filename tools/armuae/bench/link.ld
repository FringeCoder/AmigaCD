MEMORY { rom (rx) : ORIGIN = 0xF80000, LENGTH = 512K
         ram (rw) : ORIGIN = 0x200000, LENGTH = 512K }
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4); } > rom
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > ram AT > rom
  _sdata_rom = LOADADDR(.data);
  .bss : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > ram
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
