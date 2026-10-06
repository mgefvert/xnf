if(NOT DEFINED KERNEL_INPUT OR NOT DEFINED KERNEL_PADDED OR NOT DEFINED KERNEL_INCLUDE OR NOT DEFINED NASM_EXECUTABLE)
    message(FATAL_ERROR "PrepareKernel.cmake requires KERNEL_INPUT, KERNEL_PADDED, KERNEL_INCLUDE, and NASM_EXECUTABLE")
endif()

file(SIZE "${KERNEL_INPUT}" KERNEL_SIZE)
if(KERNEL_SIZE LESS 1)
    message(FATAL_ERROR "kernel.bin is empty")
endif()

math(EXPR KERNEL_SECTORS "(${KERNEL_SIZE} + 511) / 512")
if(KERNEL_SECTORS GREATER 127)
    message(FATAL_ERROR "kernel.bin is ${KERNEL_SIZE} bytes (${KERNEL_SECTORS} sectors); the one-request BIOS loader limit is 127 sectors")
endif()

math(EXPR PAD_BYTES "(${KERNEL_SECTORS} * 512) - ${KERNEL_SIZE}")
if(PAD_BYTES GREATER 0)
    set(PADDING_ASM "${CMAKE_CURRENT_BINARY_DIR}/kernel_padding.asm")
    set(PADDING_BIN "${CMAKE_CURRENT_BINARY_DIR}/kernel_padding.bin")
    file(WRITE "${PADDING_ASM}" "BITS 16\ntimes ${PAD_BYTES} db 0\n")
    execute_process(
        COMMAND "${NASM_EXECUTABLE}" -f bin -o "${PADDING_BIN}" "${PADDING_ASM}"
        RESULT_VARIABLE NASM_RESULT
        ERROR_VARIABLE NASM_ERROR
    )
    if(NOT NASM_RESULT EQUAL 0)
        message(FATAL_ERROR "NASM could not create kernel padding: ${NASM_ERROR}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E cat "${KERNEL_INPUT}" "${PADDING_BIN}"
        OUTPUT_FILE "${KERNEL_PADDED}"
        RESULT_VARIABLE CAT_RESULT
        ERROR_VARIABLE CAT_ERROR
    )
    file(REMOVE "${PADDING_ASM}" "${PADDING_BIN}")
    if(NOT CAT_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not pad kernel.bin: ${CAT_ERROR}")
    endif()
else()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy "${KERNEL_INPUT}" "${KERNEL_PADDED}"
        RESULT_VARIABLE COPY_RESULT
        ERROR_VARIABLE COPY_ERROR
    )
    if(NOT COPY_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not copy kernel.bin: ${COPY_ERROR}")
    endif()
endif()

file(WRITE "${KERNEL_INCLUDE}" "KERNEL_SECTORS equ ${KERNEL_SECTORS}\n")
message(STATUS "Kernel size: ${KERNEL_SIZE} bytes; loading ${KERNEL_SECTORS} sector(s)")
