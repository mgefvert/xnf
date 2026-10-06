cmake_minimum_required(VERSION 3.20)
project(xnf LANGUAGES NONE)

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
set(KERNEL_C_OBJECT "${CMAKE_BINARY_DIR}/kernel_c.obj")
set(KERNEL_LINKED_EXE "${CMAKE_BINARY_DIR}/kernel_linked.exe")
set(KERNEL_PADDED "${CMAKE_BINARY_DIR}/kernel.padded.bin")
set(KERNEL_SECTORS_INCLUDE "${CMAKE_BINARY_DIR}/kernel_sectors.inc")
set(BOOT_BIN "${CMAKE_BINARY_DIR}/boot.bin")
set(DISK_IMAGE "${CMAKE_BINARY_DIR}/xnf.img")

add_custom_command(
    OUTPUT "${KERNEL_ASM_OBJECT}"
    COMMAND "${NASM_EXECUTABLE}" -f win32 "-I${CMAKE_CURRENT_SOURCE_DIR}/" -o "${KERNEL_ASM_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/kernel.asm"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernel.asm"
        "${CMAKE_CURRENT_SOURCE_DIR}/conio.asm"
    VERBATIM
)

add_custom_command(
    OUTPUT "${KERNEL_C_OBJECT}"
    COMMAND "${KERNEL_C_COMPILER}"
        -m32
        -ffreestanding
        -fno-builtin
        -fno-pie
        -fno-stack-protector
        -fno-asynchronous-unwind-tables
        -fno-unwind-tables
        -c "${CMAKE_CURRENT_SOURCE_DIR}/kernel.c"
        -o "${KERNEL_C_OBJECT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernel.c"
    VERBATIM
)

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
        "${KERNEL_C_OBJECT}"
    COMMAND "${KERNEL_OBJCOPY}"
        --change-addresses -0x10000
        -O binary
        "${KERNEL_LINKED_EXE}"
        "${KERNEL_BIN}"
    DEPENDS
        "${KERNEL_ASM_OBJECT}"
        "${KERNEL_C_OBJECT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld"
    VERBATIM
)

add_custom_command(
    OUTPUT "${KERNEL_PADDED}" "${KERNEL_SECTORS_INCLUDE}"
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
    COMMAND "${NASM_EXECUTABLE}" -f bin "-I${CMAKE_BINARY_DIR}/" -o "${BOOT_BIN}" "${CMAKE_CURRENT_SOURCE_DIR}/boot.asm"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/boot.asm"
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
