# -----------------------------------------------------------------------------
# Build the freestanding BIOS kernel with NASM and MinGW, without runtime libraries.
# Also models sources for CLion and optionally includes disposable guest test entry points.
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)
project(xnf LANGUAGES C)

option(XNF_BUILD_TESTS "Include disposable kernel/QEMU test entry points" OFF)

find_program(NASM_EXECUTABLE nasm REQUIRED)
find_program(QEMU_EXECUTABLE qemu-system-i386 REQUIRED)

# The available CLion MinGW GCC targets x86_64 by default; -m32 emits
# 32-bit i386 objects. Linking uses the matching MinGW GNU tools.
file(GLOB XNF_MINGW_HINT_DIRS "$ENV{ProgramFiles}/JetBrains/CLion*/bin/mingw/bin")
find_program(KERNEL_C_COMPILER
    NAMES x86_64-w64-mingw32-gcc gcc
    HINTS ${XNF_MINGW_HINT_DIRS}
    REQUIRED
)
get_filename_component(KERNEL_C_COMPILER_DIR "${KERNEL_C_COMPILER}" DIRECTORY)
find_program(KERNEL_LINKER
    NAMES ld
    HINTS "${KERNEL_C_COMPILER_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)
find_program(KERNEL_OBJCOPY
    NAMES objcopy
    HINTS "${KERNEL_C_COMPILER_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

set(KERNEL_BIN "${CMAKE_BINARY_DIR}/kernel.bin")
set(KERNEL_ASM_OBJECT "${CMAKE_BINARY_DIR}/kernel_asm.obj")
set(KERNEL_LINKED_EXE "${CMAKE_BINARY_DIR}/kernel_linked.exe")
set(KERNEL_PADDED "${CMAKE_BINARY_DIR}/kernel.padded.bin")
set(KERNEL_SECTORS_INCLUDE "${CMAKE_BINARY_DIR}/kernel_sectors.inc")
set(BOOT_BIN "${CMAKE_BINARY_DIR}/boot.bin")
set(DISK_IMAGE "${CMAKE_BINARY_DIR}/xnf.img")

add_custom_command(
    OUTPUT "${KERNEL_ASM_OBJECT}"
    COMMAND "${NASM_EXECUTABLE}"
        -f win32
        "-I${CMAKE_CURRENT_SOURCE_DIR}/asm/"
        -o "${KERNEL_ASM_OBJECT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/asm/kernel.asm"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/asm/kernel.asm"
        "${CMAKE_CURRENT_SOURCE_DIR}/asm/paging.asm"
        "${CMAKE_CURRENT_SOURCE_DIR}/asm/interrupts.asm"
        "${CMAKE_CURRENT_SOURCE_DIR}/asm/threads.asm"
    VERBATIM
)

# CONFIGURE_DEPENDS makes CMake regenerate this list when a .c file is added
# to or removed from src. Track the headers too so changes rebuild the C objects.
file(GLOB KERNEL_C_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)
if(XNF_BUILD_TESTS)
    file(GLOB KERNEL_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/kernel/*.c")
    list(APPEND KERNEL_C_SOURCES ${KERNEL_TEST_SOURCES})
endif()
file(GLOB KERNEL_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/inc/*.h"
)

# Keep the C files in CMake's code model so CLion can resolve their headers.
# The real kernel objects are still built by the custom MinGW commands below.
add_library(kernel_c_model OBJECT EXCLUDE_FROM_ALL ${KERNEL_C_SOURCES})
target_include_directories(kernel_c_model PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc")
# Explicit i686 minimum; assembly checks CPUID before entering C. Keep the
# IDE model and actual freestanding compile flags identical.
set(KERNEL_C_FLAGS
    # Freestanding compilation still needs explicit exclusions: GCC must not
    # emit PIE relocations, stack-protector calls, or SSE/MMX state we do not set up.
    -m32 -march=i686 -mno-sse -mno-mmx
    -ffreestanding -fno-builtin -fno-pie -fno-stack-protector
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -Wall -Wextra -Werror
)
if(XNF_BUILD_TESTS)
    list(APPEND KERNEL_C_FLAGS -DXNF_BUILD_TESTS=1)
endif()
target_compile_options(kernel_c_model PRIVATE ${KERNEL_C_FLAGS})

set(KERNEL_C_OBJECTS)
# Each custom compile is the real build; kernel_c_model exists only for the IDE.
foreach(KERNEL_C_SOURCE IN LISTS KERNEL_C_SOURCES)
    get_filename_component(KERNEL_C_NAME "${KERNEL_C_SOURCE}" NAME_WE)
    set(KERNEL_C_OBJECT "${CMAKE_BINARY_DIR}/${KERNEL_C_NAME}_c.obj")

    add_custom_command(
        OUTPUT "${KERNEL_C_OBJECT}"
        COMMAND "${KERNEL_C_COMPILER}"
            ${KERNEL_C_FLAGS}
            "-I${CMAKE_CURRENT_SOURCE_DIR}/inc"
            -c "${KERNEL_C_SOURCE}"
            -o "${KERNEL_C_OBJECT}"
        DEPENDS
            "${KERNEL_C_SOURCE}"
            ${KERNEL_HEADERS}
        VERBATIM
    )

    list(APPEND KERNEL_C_OBJECTS "${KERNEL_C_OBJECT}")
endforeach()

add_custom_command(
    OUTPUT "${KERNEL_BIN}"
    # MinGW ld is PE-targeted. Its temporary image uses physical section
    # addresses; objcopy shifts only the file offsets to make a raw kernel.
    COMMAND "${KERNEL_LINKER}"
        -mi386pe
        --image-base 0
        --disable-reloc-section
        "-T${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld"
        -o "${KERNEL_LINKED_EXE}"
        "${KERNEL_ASM_OBJECT}"
        ${KERNEL_C_OBJECTS}
    COMMAND "${KERNEL_OBJCOPY}"
        --change-addresses -0x10000
        -O binary
        "${KERNEL_LINKED_EXE}"
        "${KERNEL_BIN}"
    DEPENDS
        "${KERNEL_ASM_OBJECT}"
        ${KERNEL_C_OBJECTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld"
    VERBATIM
)

add_custom_command(
    OUTPUT "${KERNEL_PADDED}" "${KERNEL_SECTORS_INCLUDE}"
    # Sector count is generated from the actual flat image, not a guessed size.
    COMMAND "${CMAKE_COMMAND}"
        "-DKERNEL_INPUT=${KERNEL_BIN}"
        "-DKERNEL_PADDED=${KERNEL_PADDED}"
        "-DKERNEL_INCLUDE=${KERNEL_SECTORS_INCLUDE}"
        "-DNASM_EXECUTABLE=${NASM_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareKernel.cmake"
    DEPENDS
        "${KERNEL_BIN}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareKernel.cmake"
    VERBATIM
)

add_custom_command(
    OUTPUT "${BOOT_BIN}"
    # Reassemble when the count changes: the boot DAP embeds that exact count.
    COMMAND "${NASM_EXECUTABLE}"
        -f bin
        "-I${CMAKE_BINARY_DIR}/"
        -o "${BOOT_BIN}"
        "${CMAKE_CURRENT_SOURCE_DIR}/asm/boot.asm"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/asm/boot.asm"
        "${KERNEL_SECTORS_INCLUDE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${DISK_IMAGE}"
    COMMAND "${CMAKE_COMMAND}"
        "-DBOOT_BIN=${BOOT_BIN}"
        "-DKERNEL_PADDED=${KERNEL_PADDED}"
        "-DDISK_IMAGE=${DISK_IMAGE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateDiskImage.cmake"
    DEPENDS
        "${BOOT_BIN}"
        "${KERNEL_PADDED}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateDiskImage.cmake"
    VERBATIM
)

add_custom_target(boot ALL DEPENDS "${DISK_IMAGE}")

add_custom_target(run
    COMMAND "${QEMU_EXECUTABLE}" -drive "format=raw,file=xnf.img"
    DEPENDS boot
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM
)
