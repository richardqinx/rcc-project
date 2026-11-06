#include <cassert>
#include <cstdint>
#include <iostream>
#include <unordered_map>

#include <verilated.h>

#include "Vptw.h"

namespace {

constexpr uint8_t MODE_BARE = 0;
constexpr uint8_t MODE_SV39 = 8;
constexpr uint8_t MODE_SV48 = 9;
constexpr uint8_t MODE_SV57 = 10;
constexpr uint8_t MODE_SV64 = 11;

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

void tick(Vptw* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

void reset(Vptw* dut) {
    dut->rst = 1;
    dut->req_valid = 0;
    dut->mem_req_ready = 1;
    dut->mem_resp_valid = 0;
    dut->mem_resp_data = 0;
    tick(dut);
    tick(dut);
    dut->rst = 0;
    tick(dut);
}

void launch(
    Vptw* dut,
    uint64_t vaddr,
    uint8_t mode,
    bool fetch,
    bool store,
    uint64_t satp
) {
    for (int i = 0; i < 8 && !dut->req_ready; ++i) {
        dut->req_valid = 0;
        dut->mem_resp_valid = 0;
        tick(dut);
    }

    dut->req_valid = 1;
    dut->req_vaddr = vaddr;
    dut->req_asid = 1;
    dut->req_mode = mode;
    dut->req_priv = 1;
    dut->req_is_fetch = fetch;
    dut->req_is_store = store;
    dut->req_sum = 0;
    dut->req_mxr = 0;
    dut->satp_value = satp;
    tick(dut);
    dut->req_valid = 0;
}

void run_walk(Vptw* dut, const std::unordered_map<uint64_t, uint64_t>& memory) {
    bool inject_resp = false;
    uint64_t next_resp_data = 0;

    for (int cycle = 0; cycle < 80; ++cycle) {
        dut->mem_req_ready = 1;
        dut->mem_resp_valid = inject_resp ? 1 : 0;
        dut->mem_resp_data = next_resp_data;

        dut->clk = 0;
        dut->eval();

        if (dut->resp_valid) {
            dut->mem_resp_valid = 0;
            return;
        }

        const bool issued = dut->mem_req_valid && dut->mem_req_ready;
        const uint64_t req_addr = dut->mem_req_addr;

        dut->clk = 1;
        dut->eval();

        inject_resp = false;
        next_resp_data = 0;
        if (issued) {
            const auto it = memory.find(req_addr);
            next_resp_data = (it == memory.end()) ? 0 : it->second;
            inject_resp = true;
        }
    }

    check(false, "PTW_Timeout");
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

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* dut = new Vptw;

    std::cout << "----------------------------------\n";
    std::cout << "          PTW Unit Test\n";
    std::cout << "----------------------------------\n";

    reset(dut);

    launch(dut, 0x0000000040401000ULL, MODE_SV48, false, false, make_satp(MODE_BARE, 1, 0x100));
    run_walk(dut, {});
    check(dut->resp_fault == 1, "Sv48_Stub_Fault");

    launch(dut, 0x0000000040401000ULL, MODE_SV57, false, false, make_satp(MODE_BARE, 1, 0x100));
    run_walk(dut, {});
    check(dut->resp_fault == 1, "Sv57_Stub_Fault");

    launch(dut, 0x0000000040401000ULL, MODE_SV64, false, false, make_satp(MODE_BARE, 1, 0x100));
    run_walk(dut, {});
    check(dut->resp_fault == 1, "Sv64_Stub_Fault");

    const uint64_t root_ppn = 0x100;
    const uint64_t va = 0x0000000040401234ULL;
    const uint64_t leaf_ppn = 0x45670;
    std::unordered_map<uint64_t, uint64_t> mem_ok;
    add_sv39_mapping(mem_ok, root_ppn, va, 0x210, 0x220, leaf_ppn,
                     FLAG_V | FLAG_R | FLAG_W | FLAG_A | FLAG_D);

    launch(dut, va, MODE_SV39, false, false, make_satp(MODE_SV39, 1, root_ppn));
    run_walk(dut, mem_ok);
    check(dut->resp_fault == 0, "Sv39_Success_NoFault");
    check(dut->resp_paddr == ((leaf_ppn << 12) | (va & 0xfffULL)), "Sv39_Success_Paddr");
    check(dut->refill_valid == 1, "Sv39_Success_Refill");
    check(dut->refill_level == 0, "Sv39_Success_Level0");

    std::unordered_map<uint64_t, uint64_t> mem_no_x;
    add_sv39_mapping(mem_no_x, root_ppn, va, 0x230, 0x240, 0x50000, FLAG_V | FLAG_R | FLAG_A);
    launch(dut, va, MODE_SV39, true, false, make_satp(MODE_SV39, 1, root_ppn));
    run_walk(dut, mem_no_x);
    check(dut->resp_fault == 1, "Sv39_Fetch_NoX_Fault");
    check(dut->refill_valid == 0, "Sv39_Fetch_NoX_NoRefill");

    std::unordered_map<uint64_t, uint64_t> mem_store_d0;
    add_sv39_mapping(mem_store_d0, root_ppn, va, 0x250, 0x260, 0x60000,
                     FLAG_V | FLAG_R | FLAG_W | FLAG_A);
    launch(dut, va, MODE_SV39, false, true, make_satp(MODE_SV39, 1, root_ppn));
    run_walk(dut, mem_store_d0);
    check(dut->resp_fault == 1, "Sv39_Store_D0_Fault");

    launch(dut, 0x0000008000000000ULL, MODE_SV39, false, false, make_satp(MODE_SV39, 1, root_ppn));
    run_walk(dut, {});
    check(dut->resp_fault == 1, "Sv39_NonCanonical_Fault");

    std::cout << "----------------------------------\n";
    std::cout << "All " << g_pass_count << " PTW tests passed!\n";
    std::cout << "----------------------------------\n";

    delete dut;
    return 0;
}
