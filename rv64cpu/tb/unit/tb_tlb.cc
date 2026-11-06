#include <cassert>
#include <cstdint>
#include <iostream>

#include <verilated.h>

#include "Vtlb.h"

namespace {

constexpr uint8_t MODE_BARE = 0;
constexpr uint8_t MODE_SV39 = 8;
constexpr uint8_t MODE_SV48 = 9;
constexpr uint8_t MODE_SV57 = 10;
constexpr uint8_t MODE_SV64 = 11;

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

void tick(Vtlb* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

void clear_req(Vtlb* dut) {
    dut->req_valid = 0;
    dut->req_vaddr = 0;
    dut->req_asid = 0;
    dut->req_mode = MODE_SV39;
    dut->req_priv = 1;
    dut->req_is_fetch = 0;
    dut->req_is_store = 0;
    dut->req_sum = 0;
    dut->req_mxr = 0;
}

void clear_refill(Vtlb* dut) {
    dut->refill_valid = 0;
    dut->refill_vpn = 0;
    dut->refill_ppn = 0;
    dut->refill_level = 0;
    dut->refill_mode = MODE_SV39;
    dut->refill_asid = 0;
    dut->refill_g = 0;
    dut->refill_u = 0;
    dut->refill_r = 0;
    dut->refill_w = 0;
    dut->refill_x = 0;
    dut->refill_a = 0;
    dut->refill_d = 0;
}

void clear_flush(Vtlb* dut) {
    dut->flush_all = 0;
    dut->flush_asid_valid = 0;
    dut->flush_asid = 0;
    dut->flush_vpn_valid = 0;
    dut->flush_vpn = 0;
    dut->flush_mode = MODE_SV39;
}

void reset(Vtlb* dut) {
    dut->rst = 1;
    clear_req(dut);
    clear_refill(dut);
    clear_flush(dut);
    tick(dut);
    tick(dut);
    dut->rst = 0;
    dut->eval();
}

uint64_t sv39_vpn(uint64_t vaddr) {
    return (vaddr >> 12) & ((1ULL << 27) - 1ULL);
}

void refill(Vtlb* dut, uint64_t vpn, uint64_t ppn, bool r, bool w, bool x, bool a, bool d) {
    clear_refill(dut);
    dut->refill_valid = 1;
    dut->refill_vpn = vpn;
    dut->refill_ppn = ppn;
    dut->refill_level = 0;
    dut->refill_mode = MODE_SV39;
    dut->refill_asid = 1;
    dut->refill_r = r;
    dut->refill_w = w;
    dut->refill_x = x;
    dut->refill_a = a;
    dut->refill_d = d;
    tick(dut);
    clear_refill(dut);
    dut->eval();
}

void request(Vtlb* dut, uint64_t vaddr, uint8_t mode, bool fetch, bool store, bool mxr = false) {
    clear_req(dut);
    dut->req_valid = 1;
    dut->req_vaddr = vaddr;
    dut->req_asid = 1;
    dut->req_mode = mode;
    dut->req_priv = 1;
    dut->req_is_fetch = fetch;
    dut->req_is_store = store;
    dut->req_mxr = mxr;
    dut->eval();
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* dut = new Vtlb;

    std::cout << "----------------------------------\n";
    std::cout << "      L1 TLB Unit Test\n";
    std::cout << "----------------------------------\n";

    reset(dut);

    request(dut, 0x123456789abcdef0ULL, MODE_BARE, false, false);
    check(dut->resp_hit == 1, "Bare_Bypass_Hit");
    check(dut->resp_page_fault == 0, "Bare_Bypass_NoFault");
    check(dut->resp_paddr == 0x123456789abcdef0ULL, "Bare_Bypass_Paddr");

    request(dut, 0x0000000040401000ULL, MODE_SV48, false, false);
    check(dut->resp_page_fault == 1, "Sv48_Stub_Fault");
    request(dut, 0x0000000040401000ULL, MODE_SV57, false, false);
    check(dut->resp_page_fault == 1, "Sv57_Stub_Fault");
    request(dut, 0x0000000040401000ULL, MODE_SV64, false, false);
    check(dut->resp_page_fault == 1, "Sv64_Stub_Fault");

    const uint64_t va = 0x0000000040401234ULL;
    const uint64_t ppn = 0x45670ULL;
    request(dut, va, MODE_SV39, false, false);
    check(dut->resp_hit == 0, "Sv39_Miss_NoHit");
    check(dut->resp_need_ptw == 1, "Sv39_Miss_NeedPTW");
    check(dut->resp_page_fault == 0, "Sv39_Miss_NoFault");

    refill(dut, sv39_vpn(va), ppn, true, true, false, true, true);
    request(dut, va, MODE_SV39, false, false);
    check(dut->resp_hit == 1, "Sv39_Refill_Hit");
    check(dut->resp_paddr == ((ppn << 12) | (va & 0xfffULL)), "Sv39_Refill_Paddr");

    refill(dut, sv39_vpn(0x0000000040500000ULL), 0x50000ULL, true, true, false, true, false);
    request(dut, 0x0000000040500000ULL, MODE_SV39, false, true);
    check(dut->resp_page_fault == 1, "Sv39_Store_D0_Fault");

    refill(dut, sv39_vpn(0x0000000040600000ULL), 0x60000ULL, false, false, true, true, false);
    request(dut, 0x0000000040600000ULL, MODE_SV39, false, false, true);
    check(dut->resp_hit == 1, "Sv39_MXR_LoadFromXOnly");

    request(dut, 0x0000008000000000ULL, MODE_SV39, false, false);
    check(dut->resp_page_fault == 1, "Sv39_NonCanonical_Fault");

    std::cout << "----------------------------------\n";
    std::cout << "All " << g_pass_count << " TLB tests passed!\n";
    std::cout << "----------------------------------\n";

    delete dut;
    return 0;
}
