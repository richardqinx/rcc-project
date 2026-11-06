//     Richard CPU
// Copyright (C) 2026  Richard Qin
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// File path: tb/compliance/compliance_runner.cc
// Minimal compliance runner bootstrap.
// This runner intentionally keeps behavior simple so the toolchain flow can be
// integrated first, then replaced with full ELF/signature handling.

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <sstream>
#include <vector>
#include <verilated.h>

#include "Vrichard_core.h"
#include "Vrichard_core___024root.h"

namespace {

struct Options {
    uint64_t max_cycles = 2000;
    std::string elf_path;
    std::string signature_path;
    uint64_t trace_pc_count = 0;
    bool show_help = false;
};

struct Symbols {
    uint64_t begin_signature = 0;
    uint64_t end_signature = 0;
    uint64_t tohost = 0;
};

class ByteMemory {
public:
    static constexpr uint64_t UART_THR = 0x10000000ULL;
    static constexpr uint64_t UART_LSR = 0x10000005ULL;
    static constexpr uint64_t CLINT_MTIME = 0x0200BFF8ULL;

    void write_byte(uint64_t addr, uint8_t value) {
        if (addr == UART_THR) {
            return;
        }
        mem_[addr] = value;
    }

    uint8_t read_byte(uint64_t addr) const {
        if (addr == UART_LSR) {
            return 0x20;
        }
        auto it = mem_.find(addr);
        if (it == mem_.end()) {
            return 0;
        }
        return it->second;
    }

    uint32_t read_u32(uint64_t addr) const {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(read_byte(addr + i)) << (8 * i);
        }
        return value;
    }

    uint64_t read_u64(uint64_t addr) const {
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(read_byte(addr + i)) << (8 * i);
        }
        return value;
    }

    void write_store(uint64_t addr, uint64_t wdata, uint8_t size_code) {
        uint64_t bytes = 0;
        switch (size_code) {
            case 0b000:
            case 0b100:
                bytes = 1;
                break;
            case 0b001:
            case 0b101:
                bytes = 2;
                break;
            case 0b010:
            case 0b110:
                bytes = 4;
                break;
            case 0b011:
            default:
                bytes = 8;
                break;
        }

        const uint64_t offset = addr & 0x7ULL;
        const uint64_t aligned = addr & ~0x7ULL;
        for (uint64_t i = 0; i < bytes; ++i) {
            const uint64_t lane = offset + i;
            const uint8_t byte_value = static_cast<uint8_t>((wdata >> (lane * 8)) & 0xFFULL);
            write_byte(aligned + lane, byte_value);
        }
    }

private:
    std::map<uint64_t, uint8_t> mem_;
};

#pragma pack(push, 1)
struct Elf64Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct Elf64Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct Elf64Sym {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};
#pragma pack(pop)

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t SHT_SYMTAB = 2;

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            opts.show_help = true;
            continue;
        }
        if (arg == "--max-cycles" && (i + 1) < argc) {
            opts.max_cycles = static_cast<uint64_t>(std::stoull(argv[++i]));
            continue;
        }
        if (arg == "--elf" && (i + 1) < argc) {
            opts.elf_path = argv[++i];
            continue;
        }
        if (arg == "--signature" && (i + 1) < argc) {
            opts.signature_path = argv[++i];
            continue;
        }
        if (arg == "--trace-pc" && (i + 1) < argc) {
            opts.trace_pc_count = static_cast<uint64_t>(std::stoull(argv[++i]));
            continue;
        }
    }
    return opts;
}

void print_help() {
    std::cout
        << "compliance_runner (bootstrap)\n"
        << "Usage:\n"
        << "  compliance_runner --elf PATH [--max-cycles N] [--signature PATH] [--trace-pc N]\n";
}

std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

void dump_selected_gprs(const Vrichard_core* top, const char* prefix) {
    const auto& regs = top->rootp->richard_core__DOT__u_id_stage__DOT__u_regfile__DOT__regs;
    std::cerr << prefix << ": selected GPRs\n"
              << "  x2/sp=" << hex64(regs[2])
              << " x3/gp=" << hex64(regs[3])
              << " x4/tp=" << hex64(regs[4])
              << " x5/t0=" << hex64(regs[5]) << "\n"
              << "  x6/t1=" << hex64(regs[6])
              << " x7/t2=" << hex64(regs[7])
              << " x8/s0=" << hex64(regs[8])
              << " x9/s1=" << hex64(regs[9]) << "\n"
              << "  x10/a0=" << hex64(regs[10])
              << " x11/a1=" << hex64(regs[11])
              << " x20/s4=" << hex64(regs[20])
              << " x21/s5=" << hex64(regs[21]) << "\n";
    std::cerr << "  mstatus=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mstatus_reg)
              << " sstatus=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__sstatus_reg)
              << " mcause=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mcause_reg)
              << " mepc=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mepc_reg)
              << "\n";
    std::cerr << "  priv=" << static_cast<unsigned>(top->rootp->richard_core__DOT__u_csr_unit__DOT__priv_mode_q)
              << " medeleg=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__medeleg_reg)
              << " mideleg=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mideleg_reg)
              << " mtvec=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mtvec_reg)
              << " stvec=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__stvec_reg)
              << "\n";
    std::cerr << "  mstateen0=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mstateen_reg[0])
              << " sstateen0=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__sstateen_reg[0])
              << " menvcfg=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__menvcfg_reg)
              << " senvcfg=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__senvcfg_reg)
              << "\n";
}

bool load_elf64(const std::string& path, ByteMemory* memory, Symbols* symbols) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Failed to open ELF: " << path << "\n";
        return false;
    }

    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) {
        std::cerr << "Empty ELF file: " << path << "\n";
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) {
        std::cerr << "Failed to read ELF contents: " << path << "\n";
        return false;
    }

    if (data.size() < sizeof(Elf64Ehdr)) {
        std::cerr << "ELF too small: " << path << "\n";
        return false;
    }

    const auto* ehdr = reinterpret_cast<const Elf64Ehdr*>(data.data());
    if (!(ehdr->e_ident[0] == 0x7F && ehdr->e_ident[1] == 'E' && ehdr->e_ident[2] == 'L' && ehdr->e_ident[3] == 'F')) {
        std::cerr << "Invalid ELF magic: " << path << "\n";
        return false;
    }
    if (ehdr->e_ident[4] != 2 || ehdr->e_ident[5] != 1) {
        std::cerr << "Only ELF64 little-endian is supported: " << path << "\n";
        return false;
    }

    const uint64_t ph_end = ehdr->e_phoff + static_cast<uint64_t>(ehdr->e_phnum) * ehdr->e_phentsize;
    if (ph_end > data.size()) {
        std::cerr << "Program header table out of range\n";
        return false;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const uint64_t off = ehdr->e_phoff + static_cast<uint64_t>(i) * ehdr->e_phentsize;
        const auto* phdr = reinterpret_cast<const Elf64Phdr*>(data.data() + off);
        if (phdr->p_type != PT_LOAD) {
            continue;
        }
        if (phdr->p_offset + phdr->p_filesz > data.size()) {
            std::cerr << "LOAD segment out of range\n";
            return false;
        }

        const uint64_t base = (phdr->p_paddr != 0) ? phdr->p_paddr : phdr->p_vaddr;
        for (uint64_t j = 0; j < phdr->p_filesz; ++j) {
            memory->write_byte(base + j, data[phdr->p_offset + j]);
        }
        for (uint64_t j = phdr->p_filesz; j < phdr->p_memsz; ++j) {
            memory->write_byte(base + j, 0);
        }
    }

    const uint64_t sh_end = ehdr->e_shoff + static_cast<uint64_t>(ehdr->e_shnum) * ehdr->e_shentsize;
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0 || sh_end > data.size()) {
        return true;
    }

    for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
        const uint64_t off = ehdr->e_shoff + static_cast<uint64_t>(i) * ehdr->e_shentsize;
        const auto* shdr = reinterpret_cast<const Elf64Shdr*>(data.data() + off);
        if (shdr->sh_type != SHT_SYMTAB || shdr->sh_entsize == 0) {
            continue;
        }
        if (shdr->sh_offset + shdr->sh_size > data.size() || shdr->sh_link >= ehdr->e_shnum) {
            continue;
        }

        const uint64_t str_off = ehdr->e_shoff + static_cast<uint64_t>(shdr->sh_link) * ehdr->e_shentsize;
        const auto* str_shdr = reinterpret_cast<const Elf64Shdr*>(data.data() + str_off);
        if (str_shdr->sh_offset + str_shdr->sh_size > data.size()) {
            continue;
        }

        const char* strtab = reinterpret_cast<const char*>(data.data() + str_shdr->sh_offset);
        const uint64_t sym_count = shdr->sh_size / shdr->sh_entsize;
        for (uint64_t s = 0; s < sym_count; ++s) {
            const uint64_t sym_off = shdr->sh_offset + s * shdr->sh_entsize;
            const auto* sym = reinterpret_cast<const Elf64Sym*>(data.data() + sym_off);
            if (sym->st_name >= str_shdr->sh_size) {
                continue;
            }
            const std::string name(strtab + sym->st_name);
            if (name == "begin_signature") {
                symbols->begin_signature = sym->st_value;
            } else if (name == "end_signature") {
                symbols->end_signature = sym->st_value;
            } else if (name == "tohost") {
                symbols->tohost = sym->st_value;
            }
        }
    }

    return true;
}

void drive_memory_response(Vrichard_core* top, ByteMemory* memory) {
    top->imem_rdata = memory->read_u32(top->imem_addr);
    top->dmem_rdata = top->dmem_read_en ? memory->read_u64(top->dmem_addr & ~0x7ULL) : 0;
}

void tick(Vrichard_core* top, ByteMemory* memory) {
    drive_memory_response(top, memory);
    top->m_timer_interrupt = 0;
    top->m_software_interrupt = 0;
    top->m_external_interrupt = 0;
    top->s_timer_interrupt = 0;
    top->s_software_interrupt = 0;
    top->s_external_interrupt = 0;

    top->clk = 0;
    top->eval();
    drive_memory_response(top, memory);
    top->eval();
    top->clk = 1;
    top->eval();

    if (top->dmem_write_en) {
        memory->write_store(top->dmem_addr, top->dmem_wdata, top->dmem_size);
        if ((top->dmem_addr <= ByteMemory::CLINT_MTIME + 7ULL) &&
            (top->dmem_addr + 7ULL >= ByteMemory::CLINT_MTIME)) {
            top->rootp->richard_core__DOT__u_csr_unit__DOT__time_reg =
                memory->read_u64(ByteMemory::CLINT_MTIME);
        }
    }
}

void dump_signature(const ByteMemory& memory, const Symbols& symbols, const std::string& signature_path) {
    if (signature_path.empty()) {
        return;
    }

    std::ofstream sig(signature_path);
    if (!sig) {
        std::cerr << "Failed to open signature file for writing: " << signature_path << "\n";
        return;
    }

    if (symbols.begin_signature == 0 || symbols.end_signature <= symbols.begin_signature) {
        sig << "00000000\n";
        return;
    }

    const uint64_t bytes = symbols.end_signature - symbols.begin_signature;
    for (uint64_t off = 0; off + 8 <= bytes; off += 8) {
        const uint64_t value = memory.read_u64(symbols.begin_signature + off);
        sig << std::hex << std::setfill('0') << std::setw(16) << value << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const Options opts = parse_args(argc, argv);

    if (opts.show_help) {
        print_help();
        return 0;
    }
    if (opts.elf_path.empty()) {
        // Keep compatibility with CTest smoke invocation that runs the binary
        // without arguments to validate toolchain/linking only.
        std::cout << "compliance_runner: no --elf provided, smoke mode pass\n";
        return 0;
    }

    auto* top = new Vrichard_core;
    ByteMemory memory;
    Symbols symbols;

    if (!load_elf64(opts.elf_path, &memory, &symbols)) {
        delete top;
        return 2;
    }

    top->rst = 1;
    for (int i = 0; i < 5; ++i) {
        tick(top, &memory);
    }
    top->rst = 0;

    bool saw_tohost = false;
    bool test_pass = false;
    uint64_t store_trace_printed = 0;
    uint64_t read_trace_printed = 0;
    uint64_t wb_trace_printed = 0;
    uint64_t trap_trace_printed = 0;
    std::string uart_trace;
    std::vector<uint64_t> pc_trace;
    if (opts.trace_pc_count != 0) {
        pc_trace.reserve(static_cast<size_t>(opts.trace_pc_count));
    }

    for (uint64_t i = 0; i < opts.max_cycles; ++i) {
        if (opts.trace_pc_count != 0) {
            if (pc_trace.size() == opts.trace_pc_count) {
                pc_trace.erase(pc_trace.begin());
            }
            pc_trace.push_back(top->imem_addr);
        }
        tick(top, &memory);
        if (opts.trace_pc_count != 0 &&
            top->dmem_read_en &&
            (top->dmem_addr >= 0x000000008004cd20ULL) &&
            (top->dmem_addr < 0x000000008004cd40ULL) &&
            read_trace_printed < 32) {
            std::cerr << "load cycle=" << i
                      << " pc=" << hex64(top->imem_addr)
                      << " addr=" << hex64(top->dmem_addr)
                      << " rdata=" << hex64(top->dmem_rdata)
                      << " size=" << static_cast<unsigned>(top->dmem_size)
                      << "\n";
            ++read_trace_printed;
        }
        if (opts.trace_pc_count != 0 && top->dmem_write_en && top->dmem_addr == ByteMemory::UART_THR) {
            uart_trace.push_back(static_cast<char>(top->dmem_wdata & 0xFFULL));
        }
        if (opts.trace_pc_count != 0 &&
            top->dmem_write_en &&
            top->dmem_addr >= 0x00000000800115e0ULL &&
            top->dmem_addr < 0x0000000080011608ULL) {
            std::cerr << "scratch-store cycle=" << i
                      << " pc=" << hex64(top->imem_addr)
                      << " addr=" << hex64(top->dmem_addr)
                      << " data=" << hex64(top->dmem_wdata)
                      << " size=" << static_cast<unsigned>(top->dmem_size)
                      << "\n";
        }
        if (opts.trace_pc_count != 0 &&
            top->rootp->richard_core__DOT__csr_req &&
            (((top->rootp->richard_core__DOT__wb_instr_in >> 20) & 0xFFFU) == 0x341 ||
             ((top->rootp->richard_core__DOT__wb_instr_in >> 20) & 0xFFFU) == 0x141 ||
             ((top->rootp->richard_core__DOT__wb_instr_in >> 20) & 0xFFFU) == 0x300 ||
             ((top->rootp->richard_core__DOT__wb_instr_in >> 20) & 0xFFFU) == 0x100)) {
            std::cerr << "csr cycle=" << i
                      << " pc=" << hex64(top->rootp->richard_core__DOT__wb_pc_in)
                      << " addr=0x" << std::hex << std::setw(3) << std::setfill('0')
                      << ((top->rootp->richard_core__DOT__wb_instr_in >> 20) & 0xFFFU)
                      << " op=" << std::dec
                      << static_cast<unsigned>((top->rootp->richard_core__DOT__wb_instr_in >> 12) & 0x7U)
                      << " rs1=" << hex64(top->rootp->richard_core__DOT__csr_rs1_data)
                      << " mepc_old=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mepc_reg)
                      << " sepc_old=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__sepc_reg)
                      << " instr=0x" << std::hex << std::setw(8) << std::setfill('0')
                      << top->rootp->richard_core__DOT__wb_instr_in
                      << std::dec << std::setfill(' ') << "\n";
        }
        if (opts.trace_pc_count != 0 &&
            (top->rootp->richard_core__DOT__trap_ctrl_mret_en ||
             top->rootp->richard_core__DOT__trap_ctrl_sret_en)) {
            std::cerr << "xret cycle=" << i
                      << " pc=" << hex64(top->rootp->richard_core__DOT__wb_pc_in)
                      << " mret=" << static_cast<unsigned>(top->rootp->richard_core__DOT__trap_ctrl_mret_en)
                      << " sret=" << static_cast<unsigned>(top->rootp->richard_core__DOT__trap_ctrl_sret_en)
                      << " target=" << hex64(top->rootp->richard_core__DOT__hu_pc_redirect_target)
                      << " mepc=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mepc_reg)
                      << " sepc=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__sepc_reg)
                      << "\n";
        }
        if (opts.trace_pc_count != 0 &&
            top->rootp->richard_core__DOT__wb_pc_in == 0x000000008000e1b4ULL &&
            trap_trace_printed < 128) {
            const auto& regs = top->rootp->richard_core__DOT__u_id_stage__DOT__u_regfile__DOT__regs;
            const uint64_t sig_ptr = regs[6];
            std::cerr << "trap-check cycle=" << i
                      << " t1=" << hex64(sig_ptr)
                      << " cause=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mcause_reg)
                      << " mepc=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__mepc_reg)
                      << " stored_msig=" << hex64(memory.read_u64(0x00000000800115f0ULL))
                      << " exp_status=" << hex64(memory.read_u64(sig_ptr))
                      << " exp_cause=" << hex64(memory.read_u64(sig_ptr + 8))
                      << " exp_epc=" << hex64(memory.read_u64(sig_ptr + 16))
                      << "\n";
            ++trap_trace_printed;
        }
        if (opts.trace_pc_count != 0 &&
            (top->rootp->richard_core__DOT__wb_pc_in == 0x000000008000eb84ULL ||
             top->rootp->richard_core__DOT__wb_pc_in == 0x000000008000eb88ULL ||
             top->rootp->richard_core__DOT__wb_pc_in == 0x000000008000ebc8ULL ||
             top->rootp->richard_core__DOT__wb_pc_in == 0x000000008000ebccULL ||
             top->rootp->richard_core__DOT__wb_pc_in == 0x000000008000ebecULL ||
             top->rootp->richard_core__DOT__wb_pc_in == 0x000000008000ec60ULL) &&
            trap_trace_printed < 256) {
            const auto& regs = top->rootp->richard_core__DOT__u_id_stage__DOT__u_regfile__DOT__regs;
            std::cerr << "strap-debug cycle=" << i
                      << " pc=" << hex64(top->rootp->richard_core__DOT__wb_pc_in)
                      << " sp=" << hex64(regs[2])
                      << " t1=" << hex64(regs[6])
                      << " t2=" << hex64(regs[7])
                      << " s0=" << hex64(regs[8])
                      << " s1=" << hex64(regs[9])
                      << " a0=" << hex64(regs[10])
                      << " a1=" << hex64(regs[11])
                      << " sscratch=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__sscratch_reg)
                      << " scause=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__scause_reg)
                      << " sepc=" << hex64(top->rootp->richard_core__DOT__u_csr_unit__DOT__sepc_reg)
                      << " mem[Strap_sig]=" << hex64(memory.read_u64(0x00000000800115f0ULL))
                      << " mem[Ssig_bgn]=" << hex64(memory.read_u64(0x00000000800115d0ULL))
                      << " mem[Sbase]=" << hex64(memory.read_u64(0x00000000800115b0ULL))
                      << "\n";
            ++trap_trace_printed;
        }
        if (opts.trace_pc_count != 0 &&
            top->rootp->richard_core__DOT__wb_reg_write_en_out &&
            (top->rootp->richard_core__DOT__wb_rd_addr_in == 8 ||
             top->rootp->richard_core__DOT__wb_rd_addr_in == 4 ||
             top->rootp->richard_core__DOT__wb_rd_addr_in == 13 ||
             top->rootp->richard_core__DOT__wb_rd_addr_in == 20) &&
            wb_trace_printed < 64) {
            std::cerr << "wb cycle=" << i
                      << " pc=" << hex64(top->rootp->richard_core__DOT__wb_pc_in)
                      << " rd=x" << static_cast<unsigned>(top->rootp->richard_core__DOT__wb_rd_addr_in)
                      << " data=" << hex64(top->rootp->richard_core__DOT__wb_rd_wdata)
                      << " instr=0x" << std::hex << std::setw(8) << std::setfill('0')
                      << top->rootp->richard_core__DOT__wb_instr_in
                      << std::dec << std::setfill(' ') << "\n";
            ++wb_trace_printed;
        }
        if (top->dmem_write_en && symbols.tohost != 0) {
            const uint64_t addr = top->dmem_addr;
            if (opts.trace_pc_count != 0 && store_trace_printed < 128) {
                std::cerr << "store cycle=" << i
                          << " pc=" << hex64(top->imem_addr)
                          << " addr=" << hex64(addr)
                          << " data=" << hex64(top->dmem_wdata)
                          << " size=" << static_cast<unsigned>(top->dmem_size)
                          << "\n";
                ++store_trace_printed;
            }
            const bool overlaps_tohost = (addr <= symbols.tohost + 3ULL) && (addr + 7ULL >= symbols.tohost);
            if (overlaps_tohost) {
                const uint32_t status = static_cast<uint32_t>(memory.read_u64(symbols.tohost) & 0xFFFFFFFFULL);
                if (status == 1U) {
                    saw_tohost = true;
                    test_pass = true;
                    break;
                }
                if (status == 3U) {
                    saw_tohost = true;
                    test_pass = false;
                    break;
                }
            }
        }
    }

    dump_signature(memory, symbols, opts.signature_path);

    if (opts.trace_pc_count != 0 && !uart_trace.empty()) {
        std::cerr << "uart output:\n" << uart_trace << "\n";
    }
    if (!saw_tohost) {
        if (!pc_trace.empty()) {
            std::cerr << "timeout: last " << pc_trace.size() << " fetch PCs\n";
            for (uint64_t pc : pc_trace) {
                std::cerr << "  " << hex64(pc) << "\n";
            }
            dump_selected_gprs(top, "timeout");
        }
        delete top;
        return 3;
    }
    if (opts.trace_pc_count != 0 && !test_pass) {
        dump_selected_gprs(top, "fail");
    }
    delete top;
    return test_pass ? 0 : 1;
}
