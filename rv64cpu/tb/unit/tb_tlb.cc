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


#include <cassert>
#include <cstdint>
#include <iostream>

#include <verilated.h>

#include "Vtlb.h"

namespace {

constexpr uint8_t MODE_BARE = 0;
constexpr uint8_t MODE_SV32 = 1;
constexpr uint8_t MODE_SV48 = 9;
constexpr uint8_t MODE_SV57 = 10;

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

void eval_comb(Vtlb* dut) {
    dut->eval();
}

void tick(Vtlb* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

uint64_t make_paddr(uint64_t ppn, uint64_t vaddr) {
    return (ppn << 12) | (vaddr & 0xFFFULL);
}

uint64_t sv48_vpn(uint64_t vaddr) {
    return (vaddr >> 12) & ((1ULL << 36) - 1ULL);
}

uint64_t sv32_vpn(uint64_t vaddr) {
    return (vaddr >> 12) & ((1ULL << 20) - 1ULL);
}

uint64_t sv57_vpn(uint64_t vaddr) {
    return (vaddr >> 12) & ((1ULL << 45) - 1ULL);
}

void clear_req(Vtlb* dut) {
    dut->req_valid = 0;
    dut->req_vaddr = 0;
    dut->req_asid = 0;
    dut->req_mode = MODE_SV48;
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
    dut->refill_mode = MODE_SV48;
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
    dut->flush_mode = MODE_SV48;
}

void apply_flush_all(Vtlb* dut) {
    clear_flush(dut);
    dut->flush_all = 1;
    tick(dut);
    dut->flush_all = 0;
    eval_comb(dut);
}

void apply_flush_asid(Vtlb* dut, uint16_t asid) {
    clear_flush(dut);
    dut->flush_asid_valid = 1;
    dut->flush_asid = asid;
    tick(dut);
    clear_flush(dut);
    eval_comb(dut);
}

void apply_flush_vpn(Vtlb* dut, uint64_t vpn, uint8_t mode) {
    clear_flush(dut);
    dut->flush_vpn_valid = 1;
    dut->flush_vpn = vpn;
    dut->flush_mode = mode;
    tick(dut);
    clear_flush(dut);
    eval_comb(dut);
}

void reset(Vtlb* dut) {
    dut->rst = 1;
    clear_req(dut);
    clear_refill(dut);
    clear_flush(dut);
    tick(dut);
    tick(dut);
    dut->rst = 0;
    eval_comb(dut);
}

void do_refill(
    Vtlb* dut,
    uint64_t vpn,
    uint64_t ppn,
    uint8_t level,
    uint8_t mode,
    uint16_t asid,
    bool g,
    bool u,
    bool r,
    bool w,
    bool x,
    bool a,
    bool d
) {
    clear_refill(dut);
    dut->refill_valid = 1;
    dut->refill_vpn = vpn;
    dut->refill_ppn = ppn;
    dut->refill_level = level;
    dut->refill_mode = mode;
    dut->refill_asid = asid;
    dut->refill_g = g;
    dut->refill_u = u;
    dut->refill_r = r;
    dut->refill_w = w;
    dut->refill_x = x;
    dut->refill_a = a;
    dut->refill_d = d;
    tick(dut);
    dut->refill_valid = 0;
    eval_comb(dut);
}

void issue_load(Vtlb* dut, uint64_t vaddr, uint16_t asid, uint8_t mode) {
    clear_req(dut);
    dut->req_valid = 1;
    dut->req_vaddr = vaddr;
    dut->req_asid = asid;
    dut->req_mode = mode;
    dut->req_priv = 1;  // S mode
    dut->req_is_fetch = 0;
    dut->req_is_store = 0;
    eval_comb(dut);
}

void issue_req(
    Vtlb* dut,
    uint64_t vaddr,
    uint16_t asid,
    uint8_t mode,
    uint8_t priv,
    bool is_fetch,
    bool is_store,
    bool sum,
    bool mxr
) {
    clear_req(dut);
    dut->req_valid = 1;
    dut->req_vaddr = vaddr;
    dut->req_asid = asid;
    dut->req_mode = mode;
    dut->req_priv = priv;
    dut->req_is_fetch = is_fetch;
    dut->req_is_store = is_store;
    dut->req_sum = sum;
    dut->req_mxr = mxr;
    eval_comb(dut);
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* dut = new Vtlb;

    std::cout << "----------------------------------\n";
    std::cout << "      L1 TLB Unit Test\n";
    std::cout << "----------------------------------\n";

    reset(dut);

    // 1) Bare mode bypass
    clear_req(dut);
    dut->req_valid = 1;
    dut->req_mode = MODE_BARE;
    dut->req_vaddr = 0x1234'5678'9ABC'DEF0ULL;
    dut->req_priv = 1;
    eval_comb(dut);
    check(dut->resp_hit == 1, "Bare_Bypass_Hit");
    check(dut->resp_page_fault == 0, "Bare_Bypass_NoFault");
    check(dut->resp_need_ptw == 0, "Bare_Bypass_NoPTW");
    check(dut->resp_paddr == dut->req_vaddr, "Bare_Bypass_PaddrEqualsVaddr");

    // 2) Unsupported mode faults
    clear_req(dut);
    dut->req_valid = 1;
    dut->req_mode = 4;
    dut->req_vaddr = 0x8000'0000ULL;
    eval_comb(dut);
    check(dut->resp_page_fault == 1, "UnsupportedMode_PageFault");

    // 3) Miss => need PTW
    issue_load(dut, 0x0000'0000'8040'1000ULL, 1, MODE_SV48);
    check(dut->resp_hit == 0, "Miss_NoHit");
    check(dut->resp_need_ptw == 1, "Miss_NeedPTW");
    check(dut->resp_page_fault == 0, "Miss_NoFault");

    // 4) Refill then hit
    const uint64_t va1 = 0x0000'0000'8040'1234ULL;
    const uint64_t vpn1 = sv48_vpn(va1);
    const uint64_t ppn1 = 0x12345ULL;
    do_refill(dut, vpn1, ppn1, 0, MODE_SV48, 1, false, false, true, true, false, true, true);
    issue_load(dut, va1, 1, MODE_SV48);
    check(dut->resp_hit == 1, "Refill_Hit");
    check(dut->resp_page_fault == 0, "Refill_NoFault");
    check(dut->resp_paddr == make_paddr(ppn1, va1), "Refill_PaddrMatch");

    // 5) ASID mismatch miss when non-global
    issue_load(dut, va1, 2, MODE_SV48);
    check(dut->resp_hit == 0, "AsidMismatch_Miss");
    check(dut->resp_need_ptw == 1, "AsidMismatch_NeedPTW");

    // 6) Global entry ignores ASID
    const uint64_t va2 = 0x0000'0000'9000'3456ULL;
    const uint64_t vpn2 = sv48_vpn(va2);
    const uint64_t ppn2 = 0x23456ULL;
    do_refill(dut, vpn2, ppn2, 0, MODE_SV48, 9, true, false, true, false, true, true, true);
    issue_load(dut, va2, 99, MODE_SV48);
    check(dut->resp_hit == 1, "Global_AsidIgnore_Hit");

    // 7) Store permission requires W and D
    const uint64_t va3 = 0x0000'0000'A000'2222ULL;
    const uint64_t vpn3 = sv48_vpn(va3);
    do_refill(dut, vpn3, 0x34567ULL, 0, MODE_SV48, 3, false, false, true, true, false, true, false);
    clear_req(dut);
    dut->req_valid = 1;
    dut->req_vaddr = va3;
    dut->req_asid = 3;
    dut->req_mode = MODE_SV48;
    dut->req_priv = 1;
    dut->req_is_store = 1;
    eval_comb(dut);
    check(dut->resp_page_fault == 1, "Store_DBitFault");

    // 8) MXR allows load from execute-only page
    const uint64_t va4 = 0x0000'0000'B000'0100ULL;
    const uint64_t vpn4 = sv48_vpn(va4);
    do_refill(dut, vpn4, 0x45678ULL, 0, MODE_SV48, 4, false, false, false, false, true, true, true);
    clear_req(dut);
    dut->req_valid = 1;
    dut->req_vaddr = va4;
    dut->req_asid = 4;
    dut->req_mode = MODE_SV48;
    dut->req_priv = 1;
    dut->req_mxr = 1;
    eval_comb(dut);
    check(dut->resp_hit == 1, "MXR_LoadFromXOnly_Hit");
    check(dut->resp_page_fault == 0, "MXR_LoadFromXOnly_NoFault");

    // 9) Superpage match (Sv48 level=1) ignores low VPN[8:0]
    const uint64_t va5a = 0x0000'0000'C123'4000ULL;
    const uint64_t va5b = 0x0000'0000'C123'8ABCULL;  // same high VPN, different low VPN bits
    const uint64_t vpn5 = sv48_vpn(va5a);
    do_refill(dut, vpn5, 0x56780ULL, 1, MODE_SV48, 5, false, false, true, true, true, true, true);
    issue_load(dut, va5b, 5, MODE_SV48);
    check(dut->resp_hit == 1, "Superpage_Level1_Hit");

    // 10) Flush all invalidates
    apply_flush_all(dut);
    issue_load(dut, va1, 1, MODE_SV48);
    check(dut->resp_hit == 0, "FlushAll_Invalidates");

    // 11) Flush by ASID only invalidates matching non-global entries
    const uint64_t va6a = 0x0000'0000'D000'1111ULL;
    const uint64_t va6b = 0x0000'0000'D000'2222ULL;
    do_refill(dut, sv48_vpn(va6a), 0x60100ULL, 0, MODE_SV48, 7, false, false, true, true, false, true, true);
    do_refill(dut, sv48_vpn(va6b), 0x60200ULL, 0, MODE_SV48, 8, false, false, true, true, false, true, true);
    apply_flush_asid(dut, 7);
    issue_load(dut, va6a, 7, MODE_SV48);
    check(dut->resp_hit == 0, "FlushAsid_TargetInvalidated");
    issue_load(dut, va6b, 8, MODE_SV48);
    check(dut->resp_hit == 1, "FlushAsid_OtherAsidPreserved");

    // 12) Flush by VPN only invalidates matching VPN in the selected mode
    const uint64_t va7a = 0x0000'0000'E100'0123ULL;
    const uint64_t va7b = 0x0000'0000'E200'0456ULL;
    do_refill(dut, sv48_vpn(va7a), 0x70100ULL, 0, MODE_SV48, 9, false, false, true, true, false, true, true);
    do_refill(dut, sv48_vpn(va7b), 0x70200ULL, 0, MODE_SV48, 9, false, false, true, true, false, true, true);
    apply_flush_vpn(dut, sv48_vpn(va7a), MODE_SV48);
    issue_load(dut, va7a, 9, MODE_SV48);
    check(dut->resp_hit == 0, "FlushVpn_TargetInvalidated");
    issue_load(dut, va7b, 9, MODE_SV48);
    check(dut->resp_hit == 1, "FlushVpn_OtherVpnPreserved");

    // 13) Sv32 superpage boundary: level=1 ignores low VPN[9:0]
    const uint64_t va32a = 0x0000000081234000ULL;
    const uint64_t va32b = 0x00000000813FFABCULL;
    do_refill(dut, sv32_vpn(va32a), 0x31000ULL, 1, MODE_SV32, 11, false, false, true, true, false, true, true);
    issue_load(dut, va32b, 11, MODE_SV32);
    check(dut->resp_hit == 1, "Sv32_Superpage_Level1_Hit");
    check(dut->resp_page_fault == 0, "Sv32_Superpage_NoFault");

    // 14) Sv57 superpage boundary: level=2 ignores low VPN[17:0]
    const uint64_t va57a = 0x0012'3456'7800'4000ULL;
    const uint64_t va57b = 0x0012'3456'7BFF'8ABCULL;
    do_refill(dut, sv57_vpn(va57a), 0x4A1234ULL, 2, MODE_SV57, 12, false, false, true, true, false, true, true);
    issue_load(dut, va57b, 12, MODE_SV57);
    check(dut->resp_hit == 1, "Sv57_Superpage_Level2_Hit");
    check(dut->resp_page_fault == 0, "Sv57_Superpage_NoFault");

    // 15) Flush VPN mode filter: Sv32 flush must not touch Sv48 entry with same low VPN
    const uint64_t va8 = 0x0000'0000'8123'4567ULL;
    const uint64_t vpn32 = sv32_vpn(va8);
    const uint64_t vpn48_same_low = vpn32;
    do_refill(dut, vpn32, 0x81000ULL, 0, MODE_SV32, 13, false, false, true, true, false, true, true);
    do_refill(dut, vpn48_same_low, 0x82000ULL, 0, MODE_SV48, 13, false, false, true, true, false, true, true);
    apply_flush_vpn(dut, vpn32, MODE_SV32);
    issue_load(dut, va8, 13, MODE_SV32);
    check(dut->resp_hit == 0, "FlushVpn_ModeFilter_Sv32Invalidated");
    issue_load(dut, va8, 13, MODE_SV48);
    check(dut->resp_hit == 1, "FlushVpn_ModeFilter_Sv48Preserved");

    // 16) S-mode access to U=1 page requires SUM for data access
    const uint64_t va9 = 0x0000'0000'8333'0100ULL;
    do_refill(dut, sv48_vpn(va9), 0x83000ULL, 0, MODE_SV48, 14, false, true, true, true, false, true, true);
    issue_req(dut, va9, 14, MODE_SV48, 1, false, false, false, false);
    check(dut->resp_page_fault == 1, "SUM_SmodeLoadUPage_Sum0_Fault");
    issue_req(dut, va9, 14, MODE_SV48, 1, false, false, true, false);
    check(dut->resp_hit == 1, "SUM_SmodeLoadUPage_Sum1_Hit");
    check(dut->resp_page_fault == 0, "SUM_SmodeLoadUPage_Sum1_NoFault");

    // 17) SUM must not allow S-mode instruction fetch from U page
    issue_req(dut, va9, 14, MODE_SV48, 1, true, false, true, false);
    check(dut->resp_page_fault == 1, "SUM_SmodeFetchUPage_AlwaysFault");

    // 18) U-mode cannot access U=0 page
    const uint64_t va10 = 0x0000'0000'8444'0200ULL;
    do_refill(dut, sv48_vpn(va10), 0x84000ULL, 0, MODE_SV48, 15, false, false, true, true, true, true, true);
    issue_req(dut, va10, 15, MODE_SV48, 0, false, false, false, false);
    check(dut->resp_page_fault == 1, "Umode_LoadSupervisorPage_Fault");

    // 19) U-mode can load U=1 page when permissions allow
    const uint64_t va11 = 0x0000'0000'8555'0300ULL;
    do_refill(dut, sv48_vpn(va11), 0x85000ULL, 0, MODE_SV48, 16, false, true, true, false, false, true, true);
    issue_req(dut, va11, 16, MODE_SV48, 0, false, false, false, false);
    check(dut->resp_hit == 1, "Umode_LoadUserPage_Hit");
    check(dut->resp_page_fault == 0, "Umode_LoadUserPage_NoFault");

    // 20) MXR matrix: execute-only page load faults when mxr=0, passes when mxr=1
    const uint64_t va12 = 0x0000'0000'8666'0400ULL;
    do_refill(dut, sv48_vpn(va12), 0x86000ULL, 0, MODE_SV48, 17, false, false, false, false, true, true, true);
    issue_req(dut, va12, 17, MODE_SV48, 1, false, false, false, false);
    check(dut->resp_page_fault == 1, "MXR_LoadXOnly_Mxr0_Fault");
    issue_req(dut, va12, 17, MODE_SV48, 1, false, false, false, true);
    check(dut->resp_hit == 1, "MXR_LoadXOnly_Mxr1_Hit");
    check(dut->resp_page_fault == 0, "MXR_LoadXOnly_Mxr1_NoFault");

    // 21) A-bit required for any access type (load/fetch)
    const uint64_t va13 = 0x0000'0000'8777'0500ULL;
    do_refill(dut, sv48_vpn(va13), 0x87000ULL, 0, MODE_SV48, 18, false, false, true, true, true, false, true);
    issue_req(dut, va13, 18, MODE_SV48, 1, false, false, false, false);
    check(dut->resp_page_fault == 1, "ABit_LoadFault_WhenA0");
    issue_req(dut, va13, 18, MODE_SV48, 1, true, false, false, false);
    check(dut->resp_page_fault == 1, "ABit_FetchFault_WhenA0");

    // 22) ASID-targeted flush must preserve global entries
    const uint64_t va14 = 0x0000'0000'8888'0600ULL;
    do_refill(dut, sv48_vpn(va14), 0x88000ULL, 0, MODE_SV48, 33, true, false, true, true, false, true, true);
    apply_flush_asid(dut, 33);
    issue_load(dut, va14, 99, MODE_SV48);
    check(dut->resp_hit == 1, "FlushAsid_GlobalPreserved");

    // 23) Refill pressure should eventually evict oldest entry (round-robin)
    apply_flush_all(dut);
    const uint64_t va15 = 0x0000'0000'9000'1000ULL;
    do_refill(dut, sv48_vpn(va15), 0x90001ULL, 0, MODE_SV48, 40, false, false, true, true, false, true, true);
    for (int n = 0; n < 32; ++n) {
        const uint64_t va_fill = 0x0000'0001'0000'0000ULL + (static_cast<uint64_t>(n) << 12);
        const uint64_t ppn_fill = 0x91000ULL + static_cast<uint64_t>(n);
        do_refill(dut, sv48_vpn(va_fill), ppn_fill, 0, MODE_SV48, static_cast<uint16_t>(50 + n), false, false, true, true, false, true, true);
    }
    issue_load(dut, va15, 40, MODE_SV48);
    check(dut->resp_hit == 0, "ReplacementPressure_OldestEvicted");

    // 24) Newest refill remains accessible after pressure
    const uint64_t va16 = 0x0000'0001'0001'F000ULL;
    do_refill(dut, sv48_vpn(va16), 0xA001FULL, 0, MODE_SV48, 77, false, false, true, true, false, true, true);
    issue_load(dut, va16, 77, MODE_SV48);
    check(dut->resp_hit == 1, "ReplacementPressure_NewestEntryHits");

    // 25) flush_vpn should invalidate Sv48 level-1 superpage for any VPN within same range
    apply_flush_all(dut);
    const uint64_t va17_base = 0x0000'0001'2340'0000ULL;
    const uint64_t va17_same_superpage = 0x0000'0001'2341'2ABCULL;
    const uint64_t va17_other_superpage = 0x0000'0001'2440'0555ULL;
    do_refill(dut, sv48_vpn(va17_base), 0xB1000ULL, 1, MODE_SV48, 88, false, false, true, true, false, true, true);
    do_refill(dut, sv48_vpn(va17_other_superpage), 0xB2000ULL, 1, MODE_SV48, 88, false, false, true, true, false, true, true);
    apply_flush_vpn(dut, sv48_vpn(va17_same_superpage), MODE_SV48);
    issue_load(dut, va17_base, 88, MODE_SV48);
    check(dut->resp_hit == 0, "FlushVpn_Sv48L1_SameSuperpageInvalidated");
    issue_load(dut, va17_other_superpage, 88, MODE_SV48);
    check(dut->resp_hit == 1, "FlushVpn_Sv48L1_OtherSuperpagePreserved");

    // 26) flush_vpn should invalidate Sv57 level-2 superpage with 18-bit VPN wildcard
    apply_flush_all(dut);
    const uint64_t va18_base = 0x0012'3456'7800'1000ULL;
    const uint64_t va18_same_superpage = 0x0012'3456'7BFF'8FFFULL;
    const uint64_t va18_other_superpage = 0x0012'345A'0000'2000ULL;
    do_refill(dut, sv57_vpn(va18_base), 0xC1234ULL, 2, MODE_SV57, 99, false, false, true, true, false, true, true);
    do_refill(dut, sv57_vpn(va18_other_superpage), 0xC5678ULL, 2, MODE_SV57, 99, false, false, true, true, false, true, true);
    apply_flush_vpn(dut, sv57_vpn(va18_same_superpage), MODE_SV57);
    issue_load(dut, va18_base, 99, MODE_SV57);
    check(dut->resp_hit == 0, "FlushVpn_Sv57L2_SameSuperpageInvalidated");
    issue_load(dut, va18_other_superpage, 99, MODE_SV57);
    check(dut->resp_hit == 1, "FlushVpn_Sv57L2_OtherSuperpagePreserved");

    // 27) flush_vpn with exact-page entry must not over-invalidate neighboring pages
    apply_flush_all(dut);
    const uint64_t va19_a = 0x0000'0000'9900'1000ULL;
    const uint64_t va19_b = 0x0000'0000'9900'2000ULL;
    do_refill(dut, sv48_vpn(va19_a), 0xD1000ULL, 0, MODE_SV48, 120, false, false, true, true, false, true, true);
    do_refill(dut, sv48_vpn(va19_b), 0xD2000ULL, 0, MODE_SV48, 120, false, false, true, true, false, true, true);
    apply_flush_vpn(dut, sv48_vpn(va19_a), MODE_SV48);
    issue_load(dut, va19_a, 120, MODE_SV48);
    check(dut->resp_hit == 0, "FlushVpn_Level0_OnlyTargetInvalidated");
    issue_load(dut, va19_b, 120, MODE_SV48);
    check(dut->resp_hit == 1, "FlushVpn_Level0_NeighborPreserved");

    std::cout << "\n----------------------------------\n";
    std::cout << "TLB tests passed: " << g_pass_count << "\n";
    std::cout << "----------------------------------\n";

    delete dut;
    return 0;
}
