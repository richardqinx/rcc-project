# RTL Signal Naming Convention

This document defines the naming rules for all SystemVerilog signals, ports, types,
and modules in the Richard-CPU project. Follow these rules when writing new RTL or
modifying existing code.

## 1. General Rules

| Rule | Example |
|---|---|
| All names use `snake_case` (lowercase + underscores) | `alu_result`, `rs1_addr` |
| Names should be descriptive; avoid single-letter names except loop indices | `i` in `for(int i = ...)` is acceptable |
| Abbreviations are allowed **only** from the approved list (Section 7) | `addr` not `address`, `imm` not `immediate` |
| No trailing numbers for disambiguation; use descriptive suffixes instead | `read_port_a` not `read_port_1` |

### 1.1 Semantic Clarity Priority

When multiple names are technically valid, prefer the name with the clearest
semantic meaning:

1. Prefer full words over unclear abbreviations unless listed in Section 7.
2. Prefer domain-accurate nouns over generic placeholders.
3. Prefer names that communicate role and timing (source, destination, stage).

Examples:
- `interrupt_cause` is preferred over `int_cause`
- `trap_target_pc` is preferred over `trap_next_value`
- `csr_write_data` is preferred over `csr_write_val`

## 2. Module & Package Names

| Item | Rule | Example |
|---|---|---|
| Module | `snake_case`, named after function | `alu`, `regfile`, `decoder`, `if_stage` |
| Package | `snake_case` ending in `_pkg` | `rv64_pkg` |
| Typedef | `snake_case` ending in `_t` | `alu_op_t`, `opcode_t` |
| Enum members | `UPPER_SNAKE_CASE` with a common prefix | `ALU_ADD`, `OPCODE_LUI` |
| Parameters / localparams | `UPPER_SNAKE_CASE` | `XLEN`, `INST_WIDTH` |

## 3. Port & Signal Names

### 3.1 Standard Suffixes

Use these suffixes consistently to indicate the **role** of a signal:

| Suffix | Meaning | Example |
|---|---|---|
| `_addr` | Address (register index or memory address) | `rs1_addr`, `rd_addr` |
| `_data` | Data bus (generic read/write payload) | `mem_rdata`, `mem_wdata` |
| `_rdata` | Read data (output from a storage element) | `rs1_rdata`, `rs2_rdata` |
| `_wdata` | Write data (input to a storage element) | `rd_wdata` |
| `_op` | Operation selector (typically an enum or funct3) | `alu_op`, `branch_op`, `csr_op` |
| `_sel` | Mux select signal | `alu_src1_sel`, `wb_sel` |
| `_en` | Enable (active-high gating signal) | `reg_write_en`, `mem_read_en` |
| `_val` | **Deprecated — do not use for new code.** Use `_rdata`/`_wdata`/`_data` instead | — |

Additional clarity rules:
- `*_value` is allowed for architectural/state snapshots when `*_data` would be
	ambiguous. Example: `csr_mstatus_value`.
- For storage read/write payloads, always prefer `_rdata`/`_wdata`/`_data` over
	`_value`.
- `_flag` is deprecated for new boolean signals. Use `is_<noun>`, `has_<noun>`,
	or `<adjective>_<noun>`.

### 3.2 Standard Prefixes

| Prefix | Meaning | Example |
|---|---|---|
| `is_` | Boolean flag indicating instruction type or state | `is_load`, `is_branch`, `is_csr` |
| `has_` | Boolean flag indicating a condition is met | `has_exception`, `has_interrupt` |

### 3.3 Rules for Boolean / Control Signals

All single-bit control signals should follow **one** of these patterns:

| Pattern | When to Use | Example |
|---|---|---|
| `is_<noun>` | Classifying an instruction or state | `is_load`, `is_jump`, `is_ecall` |
| `<noun>_en` | Enabling a write or action | `reg_write_en`, `mem_write_en` |
| `<adjective>_<noun>` | Describing a property | `illegal_instr`, `branch_taken` |

> **Consistency rule**: For write-enable signals on storage elements (register file,
> memory, CSR), always use the `_en` suffix. Example: `reg_write_en`, not `reg_write`.

Additional boolean rules:
- Avoid suffix-only boolean naming such as `*_flag` and `*_bool` in new code.
- `has_` is for condition presence (`has_interrupt`), `is_` is for classification
	(`is_interrupt` for a classified event or type).

### 3.4 Clock & Reset

| Signal | Name | Description |
|---|---|---|
| Clock | `clk` | Single system clock |
| Reset | `rst_n` (preferred) or `rst` | Active-low (`rst_n`) is preferred; if active-high, use `rst` |

> Current codebase uses active-high `rst`. If you later switch to active-low, rename
> to `rst_n` everywhere in a single commit.

## 4. Pipeline Stage Prefixes

When signals cross pipeline stages, prefix them with the stage abbreviation:

| Stage | Prefix | Example |
|---|---|---|
| Instruction Fetch | `if_` | `if_pc`, `if_instr` |
| Instruction Decode | `id_` | `id_rs1_addr`, `id_imm` |
| Execute | `ex_` | `ex_alu_result`, `ex_branch_taken` |
| Memory Access | `mem_` | `mem_rdata`, `mem_addr` |
| Write Back | `wb_` | `wb_rd_wdata`, `wb_rd_addr` |

Inter-stage pipeline registers use a combined prefix: `id_ex_reg_write_en`,
`ex_mem_alu_result`.

## 5. ALU Port Naming

The ALU is a **generic compute unit**. Its inputs are not always register values —
they may come from the PC or an immediate after mux selection. Use neutral names:

| Port | Current (non-conforming) | Recommended |
|---|---|---|
| Operand 1 input | `rs1_val` | `operand_a` |
| Operand 2 input | `rs2_val` | `operand_b` |
| Result output | `alu_res` | `alu_result` |

## 6. Register File Port Naming

| Port | Name | Description |
|---|---|---|
| Read address 1 | `rs1_addr` | Source register 1 index |
| Read data 1 | `rs1_rdata` | Source register 1 data output |
| Read address 2 | `rs2_addr` | Source register 2 index |
| Read data 2 | `rs2_rdata` | Source register 2 data output |
| Write address | `rd_addr` | Destination register index |
| Write data | `rd_wdata` | Destination register data input |
| Write enable | `reg_write_en` | Write enable (active high) |

## 7. Approved Abbreviations

Only these abbreviations are permitted. All other words should be spelled in full.

| Abbreviation | Full Form |
|---|---|
| `addr` | address |
| `alu` | arithmetic logic unit |
| `clk` | clock |
| `csr` | control and status register |
| `en` | enable |
| `ex` | execute (pipeline stage only) |
| `fpu` | floating-point unit |
| `id` | instruction decode (pipeline stage only) |
| `if` | instruction fetch (pipeline stage only) |
| `imm` | immediate |
| `instr` | instruction |
| `mem` | memory |
| `mmu` | memory management unit |
| `mux` | multiplexer |
| `op` | operation |
| `pc` | program counter |
| `rd` | destination register |
| `rdata` | read data |
| `reg` | register |
| `res` | **Deprecated — use `result`** |
| `rs1` / `rs2` | source register 1 / 2 |
| `rst` | reset |
| `sel` | select |
| `tlb` | translation lookaside buffer |
| `val` | **Deprecated — use `rdata`/`wdata`/`data`** |
| `wb` | write back (pipeline stage only) |
| `wdata` | write data |

Not approved for new code:
- `int` (use `interrupt`)
- `flag` suffix for booleans (use Section 3.3 patterns)

## 7.1 Architecture-Defined Bit Names (Exception)

Some names come directly from ISA-defined bit semantics and are allowed in narrow
scopes even if they are single-letter:

- PTE-like permission bits: `u`, `r`, `w`, `x`, `a`, `d`, `g`

Scope limits:
1. Allowed only inside tightly scoped structs, packed fields, or helper function
	parameters tied to ISA semantics.
2. For module ports and cross-module buses, prefer descriptive names such as
	`pte_user`, `pte_read`, `pte_write`, `pte_exec`, `pte_accessed`, `pte_dirty`,
	`pte_global`.

## 8. Current Codebase Non-Conformances

The following table lists signals in the existing codebase that do not yet follow
this convention. These should be renamed during the next refactoring pass.

| File | Current Name | Recommended Name | Reason |
|---|---|---|---|
|None|None|None|None|
