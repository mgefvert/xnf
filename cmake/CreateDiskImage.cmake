# -----------------------------------------------------------------------------
# Assemble the raw bootable image from a boot sector and padded kernel.
# Size checks keep BIOS sector addressing consistent with the bytes written to disk.
# -----------------------------------------------------------------------------

if(NOT DEFINED BOOT_BIN OR NOT DEFINED KERNEL_PADDED OR NOT DEFINED DISK_IMAGE)
    message(FATAL_ERROR "CreateDiskImage.cmake requires BOOT_BIN, KERNEL_PADDED, and DISK_IMAGE")
endif()

file(SIZE "${BOOT_BIN}" BOOT_SIZE)
if(NOT BOOT_SIZE EQUAL 512)
    message(FATAL_ERROR "boot.bin must be exactly 512 bytes; got ${BOOT_SIZE}")
endif()

file(SIZE "${KERNEL_PADDED}" KERNEL_PADDED_SIZE)
math(EXPR KERNEL_REMAINDER "${KERNEL_PADDED_SIZE} % 512")
if(NOT KERNEL_REMAINDER EQUAL 0)
    message(FATAL_ERROR "Padded kernel size must be a multiple of 512 bytes; got ${KERNEL_PADDED_SIZE}")
endif()

# Concatenation preserves LBA placement: boot is sector 0, kernel starts at 1.
# Padding is part of the kernel input, so every byte has a defined sector.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E cat "${BOOT_BIN}" "${KERNEL_PADDED}"
    OUTPUT_FILE "${DISK_IMAGE}"
    RESULT_VARIABLE CAT_RESULT
    ERROR_VARIABLE CAT_ERROR
)
if(NOT CAT_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create xnf.img: ${CAT_ERROR}")
endif()

file(SIZE "${DISK_IMAGE}" IMAGE_SIZE)
math(EXPR EXPECTED_IMAGE_SIZE "512 + ${KERNEL_PADDED_SIZE}")
if(NOT IMAGE_SIZE EQUAL EXPECTED_IMAGE_SIZE)
    message(FATAL_ERROR "xnf.img has unexpected size ${IMAGE_SIZE}; expected ${EXPECTED_IMAGE_SIZE}")
endif()
