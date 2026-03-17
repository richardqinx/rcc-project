//     Richard CPU
//     Copyright (C) 2026  Richard Qin
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.

//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.

//     You should have received a copy of the GNU General Public License
//     along with this program.  If not, see <https://www.gnu.org/licenses/>.

// File path: tb/unit/tb_hazard_unit.cc
// Description: Hazard Unit testbench — exhaustive forwarding, stall, and flush tests

#include <iostream>
#include <cassert>
#include <verilated.h>
#include "Vhazard_unit.h"

int pass_count = 0;

void check(bool cond, const char* name) {
    if (cond) {
        std::cout << "\033[32m[PASS] " << name << "\033[0m" << std::endl;
        pass_count++;
    } else {
        std::cout << "\033[31m[FAIL] " << name << "\033[0m" << std::endl;
        assert(false);
    }
}

void reset(Vhazard_unit* dut) {
    dut->id_rs1_addr = 0;
    dut->id_rs2_addr = 0;
    dut->ex_rs1_addr = 0;
    dut->ex_rs2_addr = 0;
    dut->ex_rd_addr = 0;
    dut->ex_reg_write_en = 0;
    dut->ex_is_load = 0;
    dut->ex_branch_taken = 0;
    dut->mem_rd_addr = 0;
    dut->mem_reg_write_en = 0;
    dut->mem_stall_req = 0;
    dut->wb_rd_addr = 0;
    dut->wb_reg_write_en = 0;
    dut->eval();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vhazard_unit* dut = new Vhazard_unit;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "      Hazard Unit Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // -----------------------
    // 1. No hazard: all quiet
    // -----------------------
    reset(dut);
    check(dut->forward_a_sel == 0, "NoHazard_FwdA_None");
    check(dut->forward_b_sel == 0, "NoHazard_FwdB_None");
    check(dut->if_stall_en == 0, "NoHazard_NoStall");
    check(dut->if_id_flush_en == 0, "NoHazard_NoFlush");
    check(dut->id_ex_flush_en == 0, "NoHazard_NoIDEXFlush");
    check(dut->id_ex_stall_en == 0, "NoHazard_NoIDEXStall");
    check(dut->ex_mem_stall_en == 0, "NoHazard_NoEXMEMStall");

    // -----------------------
    // 2. Forward A from MEM stage
    // -----------------------
    reset(dut);
    dut->ex_rs1_addr = 5;
    dut->mem_rd_addr = 5;
    dut->mem_reg_write_en = 1;
    dut->eval();
    check(dut->forward_a_sel == 1, "FwdA_From_MEM");
    check(dut->forward_b_sel == 0, "FwdA_From_MEM_B_None");

    // -----------------------
    // 3. Forward A from WB stage
    // -----------------------
    reset(dut);
    dut->ex_rs1_addr = 7;
    dut->wb_rd_addr = 7;
    dut->wb_reg_write_en = 1;
    dut->eval();
    check(dut->forward_a_sel == 2, "FwdA_From_WB");

    // -----------------------
    // 4. Forward B from MEM stage
    // -----------------------
    reset(dut);
    dut->ex_rs2_addr = 10;
    dut->mem_rd_addr = 10;
    dut->mem_reg_write_en = 1;
    dut->eval();
    check(dut->forward_b_sel == 1, "FwdB_From_MEM");
    check(dut->forward_a_sel == 0, "FwdB_From_MEM_A_None");

    // -----------------------
    // 5. Forward B from WB stage
    // -----------------------
    reset(dut);
    dut->ex_rs2_addr = 12;
    dut->wb_rd_addr = 12;
    dut->wb_reg_write_en = 1;
    dut->eval();
    check(dut->forward_b_sel == 2, "FwdB_From_WB");

    // -----------------------
    // 6. MEM > WB priority for same register
    // -----------------------
    reset(dut);
    dut->ex_rs1_addr = 3;
    dut->mem_rd_addr = 3;
    dut->mem_reg_write_en = 1;
    dut->wb_rd_addr = 3;
    dut->wb_reg_write_en = 1;
    dut->eval();
    check(dut->forward_a_sel == 1, "FwdA_MEM_Priority_Over_WB");

    reset(dut);
    dut->ex_rs2_addr = 3;
    dut->mem_rd_addr = 3;
    dut->mem_reg_write_en = 1;
    dut->wb_rd_addr = 3;
    dut->wb_reg_write_en = 1;
    dut->eval();
    check(dut->forward_b_sel == 1, "FwdB_MEM_Priority_Over_WB");

    // -----------------------
    // 7. No forward to x0 (hardwired zero)
    // -----------------------
    reset(dut);
    dut->ex_rs1_addr = 0;
    dut->mem_rd_addr = 0;
    dut->mem_reg_write_en = 1;
    dut->eval();
    check(dut->forward_a_sel == 0, "NoFwd_x0_A_MEM");

    reset(dut);
    dut->ex_rs2_addr = 0;
    dut->wb_rd_addr = 0;
    dut->wb_reg_write_en = 1;
    dut->eval();
    check(dut->forward_b_sel == 0, "NoFwd_x0_B_WB");

    // -----------------------
    // 8. No forward if reg_write_en is 0
    // -----------------------
    reset(dut);
    dut->ex_rs1_addr = 5;
    dut->mem_rd_addr = 5;
    dut->mem_reg_write_en = 0;
    dut->eval();
    check(dut->forward_a_sel == 0, "NoFwd_Write_Disabled_MEM");

    reset(dut);
    dut->ex_rs2_addr = 5;
    dut->wb_rd_addr = 5;
    dut->wb_reg_write_en = 0;
    dut->eval();
    check(dut->forward_b_sel == 0, "NoFwd_Write_Disabled_WB");

    // -----------------------
    // 9. Forward A and B simultaneously
    // -----------------------
    reset(dut);
    dut->ex_rs1_addr = 8;
    dut->ex_rs2_addr = 9;
    dut->mem_rd_addr = 8;
    dut->mem_reg_write_en = 1;
    dut->wb_rd_addr = 9;
    dut->wb_reg_write_en = 1;
    dut->eval();
    check(dut->forward_a_sel == 1, "FwdA_MEM_And_FwdB_WB_A");
    check(dut->forward_b_sel == 2, "FwdA_MEM_And_FwdB_WB_B");

    // -----------------------
    // 10. Load-Use stall (rs1 match)
    // -----------------------
    reset(dut);
    dut->ex_is_load = 1;
    dut->ex_rd_addr = 4;
    dut->id_rs1_addr = 4;
    dut->eval();
    check(dut->if_stall_en == 1, "LoadUse_RS1_IF_Stall");
    check(dut->if_id_stall_en == 1, "LoadUse_RS1_IFID_Stall");
    check(dut->id_ex_flush_en == 1, "LoadUse_RS1_IDEX_Flush");
    check(dut->id_ex_stall_en == 0, "LoadUse_RS1_IDEX_NoStall");

    // -----------------------
    // 11. Load-Use stall (rs2 match)
    // -----------------------
    reset(dut);
    dut->ex_is_load = 1;
    dut->ex_rd_addr = 6;
    dut->id_rs2_addr = 6;
    dut->eval();
    check(dut->if_stall_en == 1, "LoadUse_RS2_IF_Stall");
    check(dut->if_id_stall_en == 1, "LoadUse_RS2_IFID_Stall");
    check(dut->id_ex_flush_en == 1, "LoadUse_RS2_IDEX_Flush");

    // -----------------------
    // 12. No load-use stall to x0
    // -----------------------
    reset(dut);
    dut->ex_is_load = 1;
    dut->ex_rd_addr = 0;
    dut->id_rs1_addr = 0;
    dut->eval();
    check(dut->if_stall_en == 0, "LoadUse_x0_NoStall");

    // -----------------------
    // 13. No load-use stall if not a load
    // -----------------------
    reset(dut);
    dut->ex_is_load = 0;
    dut->ex_rd_addr = 4;
    dut->id_rs1_addr = 4;
    dut->eval();
    check(dut->if_stall_en == 0, "NotLoad_NoStall");

    // -----------------------
    // 14. Branch taken flush
    // -----------------------
    reset(dut);
    dut->ex_branch_taken = 1;
    dut->eval();
    check(dut->if_id_flush_en == 1, "Branch_IFID_Flush");
    check(dut->id_ex_flush_en == 1, "Branch_IDEX_Flush");
    check(dut->if_stall_en == 0, "Branch_IF_NoStall");
    check(dut->if_id_stall_en == 0, "Branch_IFID_NoStall");

    // -----------------------
    // 15. Branch overrides load-use stall
    // -----------------------
    reset(dut);
    dut->ex_branch_taken = 1;
    dut->ex_is_load = 1;
    dut->ex_rd_addr = 4;
    dut->id_rs1_addr = 4;
    dut->eval();
    check(dut->if_id_flush_en == 1, "BranchOverride_IFID_Flush");
    check(dut->id_ex_flush_en == 1, "BranchOverride_IDEX_Flush");
    check(dut->if_stall_en == 0, "BranchOverride_IF_NoStall");
    check(dut->if_id_stall_en == 0, "BranchOverride_IFID_NoStall");

    // -----------------------
    // 16. Memory stall (D-Cache miss)
    // -----------------------
    reset(dut);
    dut->mem_stall_req = 1;
    dut->eval();
    check(dut->if_stall_en == 1, "MemStall_IF_Stall");
    check(dut->if_id_stall_en == 1, "MemStall_IFID_Stall");
    check(dut->id_ex_stall_en == 1, "MemStall_IDEX_Stall");
    check(dut->ex_mem_stall_en == 1, "MemStall_EXMEM_Stall");
    check(dut->if_id_flush_en == 0, "MemStall_NoFlush");
    check(dut->id_ex_flush_en == 0, "MemStall_NoIDEXFlush");

    // -----------------------
    // 17. Mem stall suppresses load-use flush
    // -----------------------
    reset(dut);
    dut->mem_stall_req = 1;
    dut->ex_is_load = 1;
    dut->ex_rd_addr = 4;
    dut->id_rs1_addr = 4;
    dut->eval();
    check(dut->if_stall_en == 1, "MemStall_Suppresses_LoadUse_Stall");
    check(dut->id_ex_stall_en == 1, "MemStall_Suppresses_LoadUse_IDEXStall");
    check(dut->id_ex_flush_en == 0, "MemStall_Suppresses_LoadUse_Flush");

    // -----------------------
    // 18. Branch overrides mem stall for flush
    // -----------------------
    reset(dut);
    dut->ex_branch_taken = 1;
    dut->mem_stall_req = 1;
    dut->eval();
    check(dut->if_id_flush_en == 1, "Branch_MemStall_IFID_Flush");
    check(dut->id_ex_flush_en == 1, "Branch_MemStall_IDEX_Flush");
    check(dut->if_stall_en == 0, "Branch_MemStall_IF_NoStall");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
