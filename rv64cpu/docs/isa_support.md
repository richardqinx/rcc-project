# ISA Support Matrix

## Target ISA String

```
RV64GCZicsr_Zifencei
```

Equivalent to `RV64IMAFDCZicsr_Zifencei`

---

## Extension Implementation Status

| Extension | Full Name | Status | Description |
|---|---|---|---|
| **I** | Base Integer (RV64I) | 🟡 In progress | Five-stage RV64I pipeline, ALU, and branch unit implemented and covered by unit tests ([rtl/core/alu.sv](rtl/core/alu.sv), [tb/unit/tb_alu.cc](tb/unit/tb_alu.cc), [tb/unit/tb_branch.cc](tb/unit/tb_branch.cc)) |
| **M** | Multiply/Divide | 🔴 Not implemented | Multiply, divide, remainder |
| **A** | Atomic | 🔴 Not implemented | LR/SC, AMO atomic instructions |
| **F** | Single-Precision Float | 🔴 Not implemented | 32-bit IEEE 754 floating-point |
| **D** | Double-Precision Float | 🔴 Not implemented | 64-bit IEEE 754 floating-point |
| **C** | Compressed | 🔴 Not implemented | 16-bit compressed instructions |
| **Zicsr** | CSR Instructions | 🟢 Implemented | Full CSR unit with delegation, FCSR, and dedicated testbench ([rtl/core/csr_unit.sv](rtl/core/csr_unit.sv), [tb/unit/tb_csr.cc](tb/unit/tb_csr.cc)) |
| **Zifencei** | Instruction Fence | 🟡 Decoding only | Decoder recognizes FENCE.I; cache invalidation hookup pending |

---

## Recent Progress Highlights (2026-03)

- **CSR subsystem**: `csr_unit` now handles delegation, privilege transitions, and floating-point CSRs, verified by [tb/unit/tb_csr.cc](tb/unit/tb_csr.cc).
- **Integer execution path**: ALU, branch, and decode paths for RV64I arithmetic/logic instructions are implemented with passing unit tests ([tb/unit/tb_alu.cc](tb/unit/tb_alu.cc), [tb/unit/tb_branch.cc](tb/unit/tb_branch.cc)).
- **Decoder coverage**: Load/store, immediate, and system opcode paths exist with clear TODO markers for upcoming M/A/F extensions, see [rtl/core/decoder.sv](rtl/core/decoder.sv).

---

## RV64I Instruction List

### R-type Arithmetic/Logic
| Instruction | Operation | Status |
|---|---|---|
| ADD | rd = rs1 + rs2 | ⬜ |
| SUB | rd = rs1 - rs2 | ⬜ |
| AND | rd = rs1 & rs2 | ⬜ |
| OR | rd = rs1 \| rs2 | ⬜ |
| XOR | rd = rs1 ^ rs2 | ⬜ |
| SLL | rd = rs1 << rs2[5:0] | ⬜ |
| SRL | rd = rs1 >> rs2[5:0] (logical) | ⬜ |
| SRA | rd = rs1 >> rs2[5:0] (arith) | ⬜ |
| SLT | rd = (rs1 < rs2) ? 1 : 0 (signed) | ⬜ |
| SLTU | rd = (rs1 < rs2) ? 1 : 0 (unsigned) | ⬜ |

### R-type RV64 Word Operations
| Instruction | Operation | Status |
|---|---|---|
| ADDW | rd = sext((rs1+rs2)[31:0]) | ⬜ |
| SUBW | rd = sext((rs1-rs2)[31:0]) | ⬜ |
| SLLW | rd = sext((rs1 << rs2[4:0])[31:0]) | ⬜ |
| SRLW | rd = sext((rs1[31:0] >> rs2[4:0])) | ⬜ |
| SRAW | rd = sext((rs1[31:0] >>> rs2[4:0])) | ⬜ |

### I-type Immediate Arithmetic
| Instruction | Operation | Status |
|---|---|---|
| ADDI | rd = rs1 + imm | ⬜ |
| ANDI | rd = rs1 & imm | ⬜ |
| ORI | rd = rs1 \| imm | ⬜ |
| XORI | rd = rs1 ^ imm | ⬜ |
| SLLI | rd = rs1 << shamt[5:0] | ⬜ |
| SRLI | rd = rs1 >> shamt[5:0] (logical) | ⬜ |
| SRAI | rd = rs1 >> shamt[5:0] (arith) | ⬜ |
| SLTI | rd = (rs1 < imm) ? 1 : 0 (signed) | ⬜ |
| SLTIU | rd = (rs1 < imm) ? 1 : 0 (unsigned) | ⬜ |
| ADDIW | rd = sext((rs1+imm)[31:0]) | ⬜ |
| SLLIW | rd = sext((rs1 << shamt[4:0])[31:0]) | ⬜ |
| SRLIW | rd = sext((rs1[31:0] >> shamt[4:0])) | ⬜ |
| SRAIW | rd = sext((rs1[31:0] >>> shamt[4:0])) | ⬜ |

### Load
| Instruction | Width | Sign Extension | Status |
|---|---|---|---|
| LB | 8-bit | Signed | ⬜ |
| LBU | 8-bit | Zero-extended | ⬜ |
| LH | 16-bit | Signed | ⬜ |
| LHU | 16-bit | Zero-extended | ⬜ |
| LW | 32-bit | Signed | ⬜ |
| LWU | 32-bit | Zero-extended | ⬜ |
| LD | 64-bit | — | ⬜ |

### Store
| Instruction | Width | Status |
|---|---|---|
| SB | 8-bit | ⬜ |
| SH | 16-bit | ⬜ |
| SW | 32-bit | ⬜ |
| SD | 64-bit | ⬜ |

### Branch
| Instruction | Condition | Status |
|---|---|---|
| BEQ | rs1 == rs2 | ⬜ |
| BNE | rs1 != rs2 | ⬜ |
| BLT | rs1 < rs2 (signed) | ⬜ |
| BGE | rs1 >= rs2 (signed) | ⬜ |
| BLTU | rs1 < rs2 (unsigned) | ⬜ |
| BGEU | rs1 >= rs2 (unsigned) | ⬜ |

### Upper Immediate / Jump
| Instruction | Operation | Status |
|---|---|---|
| LUI | rd = imm << 12 | ⬜ |
| AUIPC | rd = pc + (imm << 12) | ⬜ |
| JAL | rd = pc+4; pc = pc + imm | ⬜ |
| JALR | rd = pc+4; pc = (rs1 + imm) & ~1 | ⬜ |

### System
| Instruction | Status |
|---|---|
| ECALL | ⬜ |
| EBREAK | ⬜ |
| MRET | ⬜ |
| SRET | ⬜ |
| WFI | ⬜ |
| FENCE | ⬜ |
| FENCE.I | ⬜ |
| SFENCE.VMA | ⬜ |

---

## M Extension (Multiply/Divide)

| Instruction | Operation | Status |
|---|---|---|
| MUL | rd = (rs1 × rs2)[63:0] | ⬜ |
| MULH | rd = (rs1 × rs2)[127:64] signed | ⬜ |
| MULHSU | rd = (rs1 × rs2)[127:64] signed×unsigned | ⬜ |
| MULHU | rd = (rs1 × rs2)[127:64] unsigned | ⬜ |
| DIV | rd = rs1 / rs2 signed | ⬜ |
| DIVU | rd = rs1 / rs2 unsigned | ⬜ |
| REM | rd = rs1 % rs2 signed | ⬜ |
| REMU | rd = rs1 % rs2 unsigned | ⬜ |
| MULW | rd = sext((rs1×rs2)[31:0]) | ⬜ |
| DIVW / DIVUW | 32-bit division | ⬜ |
| REMW / REMUW | 32-bit remainder | ⬜ |

---

## A Extension (Atomic Instructions)

| Instruction | Status |
|---|---|
| LR.W / LR.D | ⬜ |
| SC.W / SC.D | ⬜ |
| AMOSWAP.W/D | ⬜ |
| AMOADD.W/D | ⬜ |
| AMOAND.W/D | ⬜ |
| AMOOR.W/D | ⬜ |
| AMOXOR.W/D | ⬜ |
| AMOMAX[U].W/D | ⬜ |
| AMOMIN[U].W/D | ⬜ |

---

## F/D Extension (Floating-Point)

| Category | F (32-bit) Instructions | D (64-bit) Instructions | Status |
|---|---|---|---|
| Load/Store | FLW, FSW | FLD, FSD | ⬜ |
| Arithmetic | FADD.S, FSUB.S, FMUL.S, FDIV.S, FSQRT.S | FADD.D, ... | ⬜ |
| Fused Multiply-Add | FMADD.S, FMSUB.S, FNMADD.S, FNMSUB.S | FMADD.D, ... | ⬜ |
| Sign Manipulation | FSGNJ[N/X].S | FSGNJ[N/X].D | ⬜ |
| Compare | FEQ.S, FLT.S, FLE.S | FEQ.D, FLT.D, FLE.D | ⬜ |
| Classify | FCLASS.S | FCLASS.D | ⬜ |
| Conversion | FCVT.W/WU/L/LU.S, FCVT.S.W/WU/L/LU | FCVT.*.D | ⬜ |
| F↔D Conversion | — | FCVT.S.D, FCVT.D.S | ⬜ |
| Move | FMV.X.W, FMV.W.X | FMV.X.D, FMV.D.X | ⬜ |
| MIN/MAX | FMIN.S, FMAX.S | FMIN.D, FMAX.D | ⬜ |

### Floating-Point CSRs
| CSR | Address | Description |
|---|---|---|
| `fflags` | 0x001 | Floating-point exception flags (NV, DZ, OF, UF, NX) |
| `frm` | 0x002 | Floating-point rounding mode |
| `fcsr` | 0x003 | fflags + frm combined |

---

## C Extension (Compressed Instructions)

16-bit encoded subset of common instructions, improving code density. Requires handling 16/32-bit mixed instruction stream alignment at the IF stage.

| Quadrant | Selected Instructions | Status |
|---|---|---|
| C0 | C.LW, C.LD, C.SW, C.SD, C.ADDI4SPN | ⬜ |
| C1 | C.ADDI, C.LI, C.LUI, C.SRLI, C.SRAI, C.ANDI, C.SUB, C.XOR, C.OR, C.AND, C.J, C.BEQZ, C.BNEZ | ⬜ |
| C2 | C.SLLI, C.LDSP, C.LWSP, C.JR, C.MV, C.JALR, C.ADD, C.SDSP, C.SWSP | ⬜ |

---

## Zicsr Instructions

| Instruction | Operation | Status |
|---|---|---|
| CSRRW | t=CSR; CSR=rs1; rd=t | ⬜ |
| CSRRS | t=CSR; CSR=t\|rs1; rd=t | ⬜ |
| CSRRC | t=CSR; CSR=t&~rs1; rd=t | ⬜ |
| CSRRWI | t=CSR; CSR=zimm; rd=t | ⬜ |
| CSRRSI | t=CSR; CSR=t\|zimm; rd=t | ⬜ |
| CSRRCI | t=CSR; CSR=t&~zimm; rd=t | ⬜ |
