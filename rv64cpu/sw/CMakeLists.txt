# ─── Software Build (Bare-Metal) ─────────────────────────────────────────────────
#
# This CMakeLists.txt is only included when BUILD_SW=ON and requires
# the RISC-V cross-compilation toolchain:
#
#   cmake -B build_sw -DBUILD_SW=ON \
#         -DCMAKE_TOOLCHAIN_FILE=tools/toolchain-riscv64.cmake
#
# ─────────────────────────────────────────────────────────────────────────────────

enable_language(ASM)

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/common/link.ld)
set(STARTUP_SRC   ${CMAKE_CURRENT_SOURCE_DIR}/common/start.S)

# ─── Helper: create a bare-metal firmware target ─────────────────────────────────
function(add_firmware)
    cmake_parse_arguments(FW "" "NAME" "SOURCES" ${ARGN})

    add_executable(${FW_NAME}.elf
        ${STARTUP_SRC}
        ${FW_SOURCES}
    )

    target_include_directories(${FW_NAME}.elf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
    )

    target_link_options(${FW_NAME}.elf PRIVATE
        -T${LINKER_SCRIPT}
        -Wl,-Map=${FW_NAME}.map
    )

    # Generate .bin
    add_custom_command(TARGET ${FW_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary
                ${FW_NAME}.elf ${FW_NAME}.bin
        COMMENT "Generating ${FW_NAME}.bin"
    )

    # Generate .hex (for $readmemh)
    add_custom_command(TARGET ${FW_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O verilog
                ${FW_NAME}.elf ${FW_NAME}.hex
        COMMENT "Generating ${FW_NAME}.hex"
    )

    # Print size
    add_custom_command(TARGET ${FW_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_SIZE} ${FW_NAME}.elf
    )
endfunction()

# ═══════════════════════════════════════════════════════════════════════════════════
# Firmware Targets — uncomment as you create the source files
# ═══════════════════════════════════════════════════════════════════════════════════
#
# add_firmware(
#     NAME    hello_uart
#     SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/baremetal/hello_uart/main.c
# )
#
# add_firmware(
#     NAME    timer_irq
#     SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/baremetal/timer_irq/main.c
# )
#
# add_firmware(
#     NAME    cache_test
#     SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/baremetal/cache_test/main.c
# )
