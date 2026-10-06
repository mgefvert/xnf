cmake_minimum_required(VERSION 3.20)
project(xnf LANGUAGES NONE)

find_program(NASM_EXECUTABLE nasm REQUIRED)
find_program(QEMU_EXECUTABLE qemu-system-i386 REQUIRED)

set(KERNEL_BIN "${CMAKE_BINARY_DIR}/kernel.bin")
set(KERNEL_PADDED "${CMAKE_BINARY_DIR}/kernel.padded.bin")
set(KERNEL_SECTORS_INCLUDE "${CMAKE_BINARY_DIR}/kernel_sectors.inc")
set(BOOT_BIN "${CMAKE_BINARY_DIR}/boot.bin")
set(DISK_IMAGE "${CMAKE_BINARY_DIR}/xnf.img")

add_custom_command(
    OUTPUT "${KERNEL_BIN}"
    COMMAND "${NASM_EXECUTABLE}" -f bin "-I${CMAKE_CURRENT_SOURCE_DIR}/" -o "${KERNEL_BIN}" "${CMAKE_CURRENT_SOURCE_DIR}/kernel.asm"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernel.asm"
        "${CMAKE_CURRENT_SOURCE_DIR}/conio.asm"
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
