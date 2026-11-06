#include <cassert>
#include <cstdint>
#include <iostream>
#include <unordered_map>

#include <verilated.h>

#include "Vmmu_top.h"

namespace {

constexpr uint8_t MODE_BARE = 0;
constexpr uint8_t MODE_SV39 = 8;
constexpr uint8_t MODE_SV48 = 9;

constexpr uint64_t FLAG_V = 1ULL << 0;
constexpr uint64_t FLAG_R = 1ULL << 1;
constexpr uint64_t FLAG_W = 1ULL << 2;
constexpr uint64_t FLAG_X = 1ULL << 3;
constexpr uint64_t FLAG_A = 1ULL << 6;
constexpr uint64_t FLAG_D = 1ULL << 7;

int g_pass_count = 0;

void check(bool cond, const char* name) {
    if (cond) {
        std::cout << "\033[32m[PASS] " << name << "\033[0m\n";
        ++g_pass_count;
    } else {
        std::cout << "\033[31m[FAIL] " << name << "\033[0m\n";
        assert(false);
    }
}

uint64_t make_satp(uint8_t mode, uint16_t asid, uint64_t ppn) {
    return (uint64_t(mode) << 60) | (uint64_t(asid) << 44) | (ppn & ((1ULL << 44) - 1ULL));
}

uint64_t make_pte(uint64_t ppn, uint64_t flags) {
    return ((ppn & ((1ULL << 44) - 1ULL)) << 10) | (flags & 0x3ffULL);
}

void tick(Vmmu_top* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

void reset(Vmmu_top* dut) {
    dut->rst = 1;
    dut->i_req_valid = 0;
    dut->d_req_valid = 0;
    dut->d_req_is_store = 0;
    dut->flush_all = 0;
    dut->flush_asid_valid = 0;
    dut->flush_asid = 0;
    dut->flush_vpn_valid = 0;
    dut->flush_vpn = 0;
    dut->ptw_mem_req_ready = 1;
    dut->ptw_mem_resp_valid = 0;
    dut->ptw_mem_resp_data = 0;
    tick(dut);
    tick(dut);
    dut->rst = 0;
    tick(dut);
}

struct StepResult {
    bool issued;
    uint64_t req_addr;
    bool i_resp_valid;
    bool i_resp_hit;
    bool i_resp_page_fault;
    uint64_t i_resp_paddr;
    bool d_resp_valid;
    bool d_resp_hit;
    bool d_resp_page_fault;
    uint64_t d_resp_paddr;
};

StepResult step(Vmmu_top* dut, bool inject_resp, uint64_t resp_data) {
    dut->ptw_mem_req_ready = 1;
    dut->ptw_mem_resp_valid = inject_resp ? 1 : 0;
    dut->ptw_mem_resp_data = resp_data;

    dut->clk = 0;
    dut->eval();
    const bool issued = dut->ptw_mem_req_valid && dut->ptw_mem_req_ready;
    const uint64_t req_addr = dut->ptw_mem_req_addr;
    StepResult result{
        issued,
        req_addr,
        bool(dut->i_resp_valid),
        bool(dut->i_resp_hit),
        bool(dut->i_resp_page_fault),
        dut->i_resp_paddr,
        bool(dut->d_resp_valid),
        bool(dut->d_resp_hit),
        bool(dut->d_resp_page_fault),
        dut->d_resp_paddr,
    };

    dut->clk = 1;
    dut->eval();

    return result;
}

void add_sv39_mapping(
    std::unordered_map<uint64_t, uint64_t>& memory,
    uint64_t root_ppn,
    uint64_t vaddr,
    uint64_t l1_ppn,
    uint64_t l0_ppn,
    uint64_t leaf_ppn,
    uint64_t leaf_flags
) {
    const uint64_t vpn2 = (vaddr >> 30) & 0x1ffULL;
    const uint64_t vpn1 = (vaddr >> 21) & 0x1ffULL;
    const uint64_t vpn0 = (vaddr >> 12) & 0x1ffULL;
    memory[(root_ppn << 12) + (vpn2 << 3)] = make_pte(l1_ppn, FLAG_V);
    memory[(l1_ppn << 12) + (vpn1 << 3)] = make_pte(l0_ppn, FLAG_V);
    memory[(l0_ppn << 12) + (vpn0 << 3)] = make_pte(leaf_ppn, leaf_flags);
}

StepResult run_until_response(
    Vmmu_top* dut,
    const std::unordered_map<uint64_t, uint64_t>& memory,
    bool want_d
) {
    bool inject_resp = false;
    uint64_t next_resp = 0;
    StepResult last{};

    for (int cycle = 0; cycle < 100; ++cycle) {
        last = step(dut, inject_resp, next_resp);
        if ((!want_d && last.i_resp_valid) || (want_d && last.d_resp_valid)) {
            return last;
        }

        inject_resp = false;
        next_resp = 0;
        if (last.issued) {
            const auto it = memory.find(last.req_addr);
            next_resp = (it == memory.end()) ? 0 : it->second;
            inject_resp = true;
        }
    }

    check(false, "MMU_ResponseTimeout");
    return last;
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* dut = new Vmmu_top;

    std::cout << "----------------------------------\n";
    std::cout << "        MMU Top Unit Test\n";
    std::cout << "----------------------------------\n";

    dut->satp_value = make_satp(MODE_BARE, 0, 0);
    dut->priv_mode = 1;
    dut->sum = 0;
    dut->mxr = 0;
    reset(dut);

    dut->i_req_valid = 1;
    dut->i_req_vaddr = 0x80001234ULL;
    dut->d_req_valid = 1;
    dut->d_req_vaddr = 0x90005678ULL;
    dut->d_req_is_store = 0;
    dut->eval();
    check(dut->i_resp_valid && dut->i_resp_hit && dut->i_resp_paddr == 0x80001234ULL, "Bare_I_Bypass");
    check(dut->d_resp_valid && dut->d_resp_hit && dut->d_resp_paddr == 0x90005678ULL, "Bare_D_Bypass");
    dut->i_req_valid = 0;
    dut->d_req_valid = 0;
    tick(dut);

    dut->satp_value = make_satp(MODE_SV48, 1, 0x100);
    dut->i_req_valid = 1;
    dut->i_req_vaddr = 0x0000000040401000ULL;
    dut->eval();
    check(dut->i_resp_valid && dut->i_resp_page_fault, "Sv48_Stub_IFault");
    dut->i_req_valid = 0;
    tick(dut);

    const uint64_t root_ppn = 0x100;
    const uint64_t i_va = 0x0000000040401234ULL;
    const uint64_t d_va = 0x0000000080505678ULL;
    const uint64_t i_leaf_ppn = 0x45670;
    const uint64_t d_leaf_ppn = 0x56780;
    std::unordered_map<uint64_t, uint64_t> memory;
    add_sv39_mapping(memory, root_ppn, i_va, 0x210, 0x220, i_leaf_ppn,
                     FLAG_V | FLAG_R | FLAG_X | FLAG_A);
    add_sv39_mapping(memory, root_ppn, d_va, 0x230, 0x240, d_leaf_ppn,
                     FLAG_V | FLAG_R | FLAG_W | FLAG_A | FLAG_D);

    dut->satp_value = make_satp(MODE_SV39, 1, root_ppn);
    dut->i_req_valid = 1;
    dut->i_req_vaddr = i_va;
    StepResult i_resp = run_until_response(dut, memory, false);
    check(i_resp.i_resp_hit && !i_resp.i_resp_page_fault, "Sv39_I_Walk_Hit");
    check(i_resp.i_resp_paddr == ((i_leaf_ppn << 12) | (i_va & 0xfffULL)), "Sv39_I_Walk_Paddr");
    dut->i_req_valid = 0;
    tick(dut);

    dut->d_req_valid = 1;
    dut->d_req_vaddr = d_va;
    dut->d_req_is_store = 1;
    StepResult d_resp = run_until_response(dut, memory, true);
    check(d_resp.d_resp_hit && !d_resp.d_resp_page_fault, "Sv39_D_Walk_Hit");
    check(d_resp.d_resp_paddr == ((d_leaf_ppn << 12) | (d_va & 0xfffULL)), "Sv39_D_Walk_Paddr");

    dut->d_req_valid = 0;
    tick(dut);
    dut->i_req_valid = 1;
    dut->i_req_vaddr = i_va;
    dut->d_req_valid = 1;
    dut->d_req_vaddr = d_va;
    dut->d_req_is_store = 1;
    dut->eval();
    check(dut->i_resp_valid && dut->i_resp_hit, "Sv39_I_TLB_Hit");
    check(dut->d_resp_valid && dut->d_resp_hit, "Sv39_D_TLB_Hit");
    check(dut->ptw_mem_req_valid == 0, "Sv39_TLB_Hit_NoPTW");

    std::cout << "----------------------------------\n";
    std::cout << "All " << g_pass_count << " MMU top tests passed!\n";
    std::cout << "----------------------------------\n";

    delete dut;
    return 0;
}
