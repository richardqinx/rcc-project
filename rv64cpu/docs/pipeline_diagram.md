# Pipeline Timing and Datapath

## Five-Stage Pipeline Datapath

```
                 ┌────────────────────────────────────────────────────────────────┐
                 │                    HAZARD UNIT                                 │
                 │  Forward A/B, Stall, Flush, FPU interlock                     │
                 └──┬─────────┬──────────┬──────────┬──────────┬────────────────┘
                    │         │          │          │          │
              ┌─────▼──┐ ┌───▼───┐ ┌────▼───┐ ┌───▼────┐ ┌──▼─────┐
              │   IF   │ │  ID   │ │   EX   │ │  MEM   │ │   WB   │
              │        │ │       │ │        │ │        │ │        │
  ┌──────┐    │ PC Gen │ │Decode │ │  ALU   │ │D-Cache │ │ Write  │
  │ iTLB ├───►│   ↓    │ │   ↓   │ │ Branch │ │  LSU   │ │  Back  │
  │      │    │I-Cache │ │RegFile│ │ MulDiv │ │  dTLB  │ │        │
  └──────┘    │ Fetch  │ │ Read  │ │  FPU   │ │        │ │        │
              │        │ │ ImmEx │ │        │ │        │ │        │
              └───┬────┘ └───┬───┘ └────┬───┘ └───┬────┘ └───┬────┘
                  │          │          │          │          │
              ┌───▼──────────▼──┐ ┌─────▼──────────▼──┐ ┌────▼─────┐
              │  IF/ID Reg     │ │  EX/MEM Reg       │ │  MEM/WB  │
              │                │ │                    │ │   Reg    │
              └────────────────┘ └────────────────────┘ └──────────┘
                                      ↑
                              ┌───────┴───────┐
                              │   CSR Unit    │
                              │  Trap Ctrl    │
                              └───────────────┘
```

---

## Inter-Stage Registers

### IF/ID Register

| Field | Width | Description |
|---|---|---|
| `pc` | 64 | Current instruction PC |
| `instr` | 32 | Fetched instruction |
| `valid` | 1 | Instruction valid |
| `exception` | 1 | IF stage exception (e.g., instruction page fault) |
| `exc_code` | 4 | Exception code |
| `predicted_taken` | 1 | Branch prediction result |
| `predicted_target` | 64 | Predicted target address |

### ID/EX Register

| Field | Width | Description |
|---|---|---|
| `pc` | 64 | |
| `rs1_data` | 64 | Register source 1 value |
| `rs2_data` | 64 | Register source 2 value |
| `rs3_data` | 64 | FPU source 3 value (FMA) |
| `imm` | 64 | Sign-extended immediate |
| `rd_addr` | 5 | Destination register address |
| `rs1_addr` | 5 | Source 1 address (for forwarding) |
| `rs2_addr` | 5 | Source 2 address (for forwarding) |
| `ctrl` | struct | Control signal bundle (ALU op, mem r/w, branch type, etc.) |
| `valid` | 1 | |
| `is_fp` | 1 | Floating-point instruction flag |

### EX/MEM Register

| Field | Width | Description |
|---|---|---|
| `pc` | 64 | |
| `alu_result` | 64 | ALU/FPU computation result |
| `rs2_data` | 64 | Store data |
| `rd_addr` | 5 | |
| `ctrl_mem` | struct | Memory access control (read/write, size, sign_ext) |
| `ctrl_wb` | struct | Writeback control (reg_write, wb_sel) |
| `valid` | 1 | |

### MEM/WB Register

| Field | Width | Description |
|---|---|---|
| `pc` | 64 | |
| `alu_result` | 64 | |
| `mem_data` | 64 | Data read from D-Cache |
| `rd_addr` | 5 | |
| `ctrl_wb` | struct | Writeback control |
| `valid` | 1 | |

---

## Forwarding Paths

```
          EX Stage                MEM Stage               WB Stage
         ┌────────┐             ┌─────────┐             ┌────────┐
         │ ALU    │             │alu_result│             │ wb_data│
         │ result ├────────┐   │ or       ├────────┐   │        │
         └────────┘        │   │ mem_data │        │   └────┬───┘
                           │   └─────────┘        │        │
                     ┌─────▼──────────────────────▼────────▼─┐
                     │           Forward MUX                  │
                     │  sel = hazard_unit.forward_a/b         │
                     └─────────────────┬─────────────────────┘
                                       │
                                       ▼
                              EX Stage rs1/rs2 input
```

| Forwarding Source | Condition |
|---|---|
| EX→EX | `ex_mem.rd == id_ex.rs1/rs2` and `ex_mem.reg_write` |
| MEM→EX | `mem_wb.rd == id_ex.rs1/rs2` and `mem_wb.reg_write` and no EX forwarding |
| Load-use | `id_ex.mem_read` and `id_ex.rd == if_id.rs1/rs2` → **stall** |

---

## Branch Handling Timing

```
Cycle:    1     2     3     4     5
         ┌─────┬─────┬─────┬─────┬─────┐
Branch   │ IF  │ ID  │ EX  │ MEM │ WB  │  ← Branch resolved at EX stage
         └─────┼─────┼─────┼─────┼─────┘
Instr+1        │ IF  │ ID  │ ██  │        ← Misprediction: flush (2-cycle penalty)
               └─────┼─────┼─────┘
Instr+2              │ IF  │ ██  │        ← flush
                     └─────┴─────┘
Target                     │ IF  │ ID  │ EX  │ ...  ← Correct path begins
                           └─────┴─────┴─────┘
```

---

## Cache Miss Timing

```
Cycle:   1    2    3    4    5    6    7    8    ...   N    N+1
        ┌────┬────┬────┬────────────────────────────┬────┬────┐
Instr   │ IF │ ID │ EX │ MEM (D-Cache MISS)         │    │ WB │
        └────┴────┴────┤    Stall pipeline...        ├────┴────┘
                       │    AXI4 Burst read (8 beats)│
                       │    Fill cache line           │
                       └─────────────────────────────┘
```

---

## TLB Miss → PTW Timing

```
Cycle:   1    2    3    ...        N    N+1   N+2
        ┌────┬────────────────────────┬────┬────┐
Instr   │ IF │ iTLB MISS → PTW walk  │ IF │ ID │ ...
        └────┤  (3-level page table   ├────┴────┘
             │   × memory latency)    │
             │  Pipeline stalled      │
             └────────────────────────┘
```
