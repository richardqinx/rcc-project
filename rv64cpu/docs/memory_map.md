# Memory Map

## Physical Address Space

```
0xFFFF_FFFF_FFFF_FFFF ┌─────────────────────────┐
                      │                         │
                      │      (unused)            │
                      │                         │
0x0000_0000_1000_2000 ├─────────────────────────┤
                      │  GPIO          4 KB     │
0x0000_0000_1000_1000 ├─────────────────────────┤
                      │  UART0         4 KB     │
0x0000_0000_1000_0000 ├─────────────────────────┤
                      │                         │
                      │      (unused)            │
                      │                         │
0x0000_0000_0C00_0000 ├─────────────────────────┤
                      │  PLIC          64 MB    │
0x0000_0000_0BFF_FFFF ├─────────────────────────┤
                      │                         │
                      │      (unused)            │
                      │                         │
0x0000_0000_0200_FFFF ├─────────────────────────┤
                      │  CLINT         64 KB    │
0x0000_0000_0200_0000 ├─────────────────────────┤
                      │                         │
                      │      (unused)            │
                      │                         │
0x0000_0000_0FFF_FFFF ├─────────────────────────┤
                      │                         │
                      │  SRAM / RAM    ~256 MB  │
                      │  (Boot ROM at lowest    │
                      │        64KB)            │
                      │                         │
0x0000_0000_0000_0000 └─────────────────────────┘
```

## Address Decode Table

| Start Address | End Address | Size | Peripheral | Bus | Description |
|---|---|---|---|---|---|
| `0x0000_0000` | `0x0FFF_FFFF` | 256 MB | SRAM Controller | AXI4-Full | Boot ROM (0x0~0xFFFF) + RAM |
| `0x0200_0000` | `0x0200_FFFF` | 64 KB | CLINT | AXI4-Lite | mtime, mtimecmp, MSIP |
| `0x0C00_0000` | `0x0FFF_FFFF` | 64 MB | PLIC | AXI4-Lite | External interrupt controller |
| `0x1000_0000` | `0x1000_0FFF` | 4 KB | UART0 | AXI4-Lite | 16550 compatible |
| `0x1000_1000` | `0x1000_1FFF` | 4 KB | GPIO | AXI4-Lite | General-purpose I/O |

> **Note**: The address ranges for CLINT and PLIC follow the RISC-V community recommended layout, compatible with Linux/OpenSBI.

## CLINT Register Layout

| Offset | Register | Description |
|---|---|---|
| `0x0000` | `msip[0]` | Machine software interrupt pending |
| `0x4000` | `mtimecmp[0]` | Machine timer compare value (64-bit) |
| `0xBFF8` | `mtime` | Machine timer current value (64-bit) |

## PLIC Register Layout (Simplified)

| Offset | Register | Description |
|---|---|---|
| `0x000000` | `priority[1..N]` | Interrupt source priority |
| `0x001000` | `pending[0..N/32]` | Interrupt pending bitfield |
| `0x002000` | `enable[ctx][0..N/32]` | Interrupt enable bitfield |
| `0x200000` | `threshold[ctx]` | Priority threshold |
| `0x200004` | `claim/complete[ctx]` | Interrupt claim/complete |

## UART0 Register Layout (16550)

| Offset | Register | Read/Write | Description |
|---|---|---|---|
| `0x00` | RBR / THR | R / W | Receive/transmit data |
| `0x01` | IER | R/W | Interrupt enable |
| `0x02` | IIR / FCR | R / W | Interrupt identification / FIFO control |
| `0x03` | LCR | R/W | Line control |
| `0x05` | LSR | R | Line status |

## Linker Script Correspondence

The MEMORY regions in `sw/common/link.ld` correspond to this table:
- `ROM`: `0x0000_0000`, 64 KB
- `RAM`: `0x0001_0000`, 256 MB - 64 KB

## Address Decode Implementation

The address decode logic in RTL is located in `rtl/soc/mem_map.sv` and `rtl/bus/axi4_xbar.sv`.

Constants are defined in `mem_map.sv`:

```systemverilog
package mem_map_pkg;
    // SRAM
    localparam logic [63:0] SRAM_BASE   = 64'h0000_0000;
    localparam logic [63:0] SRAM_SIZE   = 64'h1000_0000;   // 256 MB

    // CLINT
    localparam logic [63:0] CLINT_BASE  = 64'h0200_0000;
    localparam logic [63:0] CLINT_SIZE  = 64'h0001_0000;   // 64 KB

    // PLIC
    localparam logic [63:0] PLIC_BASE   = 64'h0C00_0000;
    localparam logic [63:0] PLIC_SIZE   = 64'h0400_0000;   // 64 MB

    // UART0
    localparam logic [63:0] UART0_BASE  = 64'h1000_0000;
    localparam logic [63:0] UART0_SIZE  = 64'h0000_1000;   // 4 KB

    // GPIO
    localparam logic [63:0] GPIO_BASE   = 64'h1000_1000;
    localparam logic [63:0] GPIO_SIZE   = 64'h0000_1000;   // 4 KB
endpackage
```
