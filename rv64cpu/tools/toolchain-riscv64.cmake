# ─── RISC-V 64 Bare-Metal Toolchain File ─────────────────────────────────────────
#
# Usage: cmake -B build_sw -DCMAKE_TOOLCHAIN_FILE=tools/toolchain-riscv64.cmake
#

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# ─── Toolchain binaries ─────────────────────────────────────────────────────────
set(CROSS_PREFIX "riscv64-unknown-elf-" CACHE STRING "Cross compiler prefix")

set(CMAKE_C_COMPILER       ${CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER     ${CROSS_PREFIX}g++)
set(CMAKE_ASM_COMPILER     ${CROSS_PREFIX}gcc)
set(CMAKE_OBJCOPY          ${CROSS_PREFIX}objcopy)
set(CMAKE_OBJDUMP          ${CROSS_PREFIX}objdump)
set(CMAKE_SIZE             ${CROSS_PREFIX}size)

# ─── Architecture flags ─────────────────────────────────────────────────────────
set(RISCV_ARCH  "rv64ima"   CACHE STRING "RISC-V ISA string")
set(RISCV_ABI   "lp64"     CACHE STRING "RISC-V ABI")

set(CMAKE_C_FLAGS_INIT     "-march=${RISCV_ARCH} -mabi=${RISCV_ABI} -nostdlib -ffreestanding -fno-builtin -mcmodel=medany")
set(CMAKE_CXX_FLAGS_INIT   "${CMAKE_C_FLAGS_INIT} -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT   "-march=${RISCV_ARCH} -mabi=${RISCV_ABI}")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostartfiles -static -Wl,--gc-sections")

# ─── Search paths ───────────────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM  NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY  ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE  ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE  ONLY)

# ─── Disable compiler checks (bare-metal can't run test binaries) ────────────────
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
