/* ═══════════════════════════════════════════════════════════════════════════════
 * richard-cpu Linker Script
 *
 * Memory map matches rtl/soc/mem_map.sv
 * ═══════════════════════════════════════════════════════════════════════════════ */

OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    /* Boot ROM / SRAM — 256 MB total addressable, practical limit is FPGA BRAM */
    ROM  (rx)  : ORIGIN = 0x00000000, LENGTH = 64K
    RAM  (rwx) : ORIGIN = 0x00010000, LENGTH = 256M - 64K
}

SECTIONS
{
    /* ── Code ──────────────────────────────────────────────────────────────── */
    .text : ALIGN(4)
    {
        *(.text.init)       /* _start / trap vector MUST be first */
        *(.text .text.*)
    } > ROM

    /* ── Read-only data ───────────────────────────────────────────────────── */
    .rodata : ALIGN(8)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > ROM

    /* ── Initialized data (load from ROM, copy to RAM) ────────────────────── */
    .data : ALIGN(8)
    {
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        _data_end = .;
    } > RAM AT > ROM

    _data_load = LOADADDR(.data);

    /* ── BSS (zero-initialized) ──────────────────────────────────────────── */
    .bss (NOLOAD) : ALIGN(8)
    {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    } > RAM

    /* ── Stack (grows downward) ──────────────────────────────────────────── */
    . = ALIGN(16);
    _stack_bottom = .;
    . = . + 8K;
    _stack_top = .;

    /* ── Heap ────────────────────────────────────────────────────────────── */
    _heap_start = .;

    /* ── tohost / fromhost (Compliance test interface) ────────────────────── */
    .tohost (NOLOAD) : ALIGN(8)
    {
        PROVIDE(tohost   = .); . += 8;
        PROVIDE(fromhost = .); . += 8;
    } > RAM

    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
