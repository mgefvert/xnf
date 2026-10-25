/*
 * VGA text console and small freestanding formatting helpers.
 * Maintains screen cells and a software cursor, mirrored to the VGA hardware cursor.
 */

#include "types.h"
#include "conio.h"

#include <stdarg.h>

#define CONIO_SCREEN_WIDTH 80u
#define CONIO_SCREEN_HEIGHT 25u
#define CONIO_SCREEN_CELLS (CONIO_SCREEN_WIDTH * CONIO_SCREEN_HEIGHT)
#define CONIO_VIDEO_MEMORY ((volatile uint16_t *)0x000B8000u)

/* VGA cells are 16 bits: low byte = character, high byte = color attribute.
   Volatile keeps stores observable by hardware even if C never reads them. */
static uint8_t conio_color = 0x07;
static uint16_t conio_cursor;

static inline void outb(uint16_t port, uint8_t value)
{
    /* "a" selects AL; "Nd" permits an immediate port or DX. Port I/O uses
       CPU instructions, unlike VGA cell writes through a memory address. */
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void conio_sync_hw_cursor(void)
{
    uint8_t value;

    /* The CRTC uses an index/data pair: choose a register at 0x3D4, then
       access it at 0x3D5. Clear cursor-disable bit 5 without changing shape. */
    outb(0x3D4, 0x0A);
    value = inb(0x3D5);
    outb(0x3D5, (uint8_t)(value & 0xDF));

    /* The linear cell index is split into low/high bytes, not pixel units. */
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)conio_cursor);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(conio_cursor >> 8));
}

static void conio_scroll(void)
{
    uint32_t index;

    /* Copy toward lower addresses so overlapping rows are not overwritten
       before they are read. Blank the last row, then keep the cursor there. */
    for (index = 0; index < CONIO_SCREEN_WIDTH * (CONIO_SCREEN_HEIGHT - 1); ++index) {
        CONIO_VIDEO_MEMORY[index] = CONIO_VIDEO_MEMORY[index + CONIO_SCREEN_WIDTH];
    }
    for (; index < CONIO_SCREEN_CELLS; ++index) {
        CONIO_VIDEO_MEMORY[index] = (uint16_t)(' ' | ((uint16_t)conio_color << 8));
    }
    conio_cursor = CONIO_SCREEN_WIDTH * (CONIO_SCREEN_HEIGHT - 1);
}

void clear_screen(void)
{
    uint32_t index;
    uint16_t cell = (uint16_t)(' ' | ((uint16_t)conio_color << 8));

    for (index = 0; index < CONIO_SCREEN_CELLS; ++index) {
        CONIO_VIDEO_MEMORY[index] = cell;
    }
    conio_cursor = 0;
    conio_sync_hw_cursor();
}

void kputc(unsigned int character)
{
    uint32_t cursor;

    /* Newline advances to the next row's first cell; carriage return moves
       within the current row. Backspace erases only when a previous cell exists. */
    if ((uint8_t)character == '\n') {
        cursor = conio_cursor;
        conio_cursor = (uint16_t)(((cursor / CONIO_SCREEN_WIDTH) + 1) * CONIO_SCREEN_WIDTH);
        if (conio_cursor >= CONIO_SCREEN_CELLS) {
            conio_scroll();
        }
    } else if ((uint8_t)character == '\r') {
        conio_cursor = (uint16_t)((conio_cursor / CONIO_SCREEN_WIDTH) * CONIO_SCREEN_WIDTH);
    } else if ((uint8_t)character == '\b') {
        if (conio_cursor != 0) {
            --conio_cursor;
            CONIO_VIDEO_MEMORY[conio_cursor] = (uint16_t)(' ' | ((uint16_t)conio_color << 8));
        }
    } else {
        /* Shift the color into the high byte, leaving the character below it. */
        CONIO_VIDEO_MEMORY[conio_cursor] = (uint16_t)((uint8_t)character | ((uint16_t)conio_color << 8));
        ++conio_cursor;
        if (conio_cursor >= CONIO_SCREEN_CELLS) {
            conio_scroll();
        }
    }

    conio_sync_hw_cursor();
}

void kputs(const char *string)
{
    while (*string != '\0') {
        kputc((uint8_t)*string++);
    }
}

static void kprint_hex_digit(uint32_t digit)
{
    kputc(digit < 10 ? '0' + digit : 'A' + digit - 10);
}

static void kprint_hex_dword(uint32_t value)
{
    /* One hexadecimal digit represents four bits. Extract nibbles from the
       most significant down to the least, keeping leading zeros visible. */
    for (int shift = 28; shift >= 0; shift -= 4) {
        kprint_hex_digit((value >> shift) & 0x0F);
    }
}

void kprint_hex(uint32_t value)
{
    kputc('0');
    kputc('x');
    kprint_hex_dword(value);
}

void kprint_hex64(unsigned long long value)
{
    /* Printing two 32-bit halves avoids needing 64-bit division helpers from
       a compiler runtime which this freestanding kernel does not link. */
    kputc('0');
    kputc('x');
    kprint_hex_dword((uint32_t)(value >> 32));
    kprint_hex_dword((uint32_t)value);
}

void kprint_dec(uint32_t value)
{
    char digits[10];
    uint32_t count = 0;

    /* Repeated division discovers decimal digits right to left. A do/while
       still emits one digit for zero; reverse the buffer for normal order. */
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count != 0) {
        kputc((uint8_t)digits[--count]);
    }
}

void kprintf(const char *format, ...)
{
    va_list arguments;

    /* The format string is the only type information for variadic arguments.
       Callers must match each conversion, especially the 64-bit X extension. */
    va_start(arguments, format);
    while (*format != '\0') {
        uint8_t conversion;

        if (*format != '%') {
            kputc((uint8_t)*format++);
            continue;
        }

        ++format;
        conversion = (uint8_t)*format;
        if (conversion == '\0') {
            kputc('%');
            break;
        }
        ++format;

        switch (conversion) {
        case '%':
            kputc('%');
            break;
        case 's':
            kputs(va_arg(arguments, const char *));
            break;
        case 'c':
            /* C promotes a char passed through ... to int. */
            kputc((unsigned int)va_arg(arguments, int));
            break;
        case 'd': {
            int value = va_arg(arguments, int);
            uint32_t magnitude;
            if (value < 0) {
                kputc('-');
                /* Unsigned subtraction also handles INT_MIN; negating it as
                   a signed int would overflow before printing its magnitude. */
                magnitude = 0u - (uint32_t)value;
            } else {
                magnitude = (uint32_t)value;
            }
            kprint_dec(magnitude);
            break;
        }
        case 'x':
            kprint_hex(va_arg(arguments, uint32_t));
            break;
        case 'X':
            /* XNF's %X consumes 64 bits; it is not standard printf's %X. */
            kprint_hex64(va_arg(arguments, unsigned long long));
            break;
        default:
            kputc('%');
            kputc(conversion);
            break;
        }
    }
    va_end(arguments);
}

void ksetcolor(unsigned int attribute)
{
    conio_color = (uint8_t)attribute;
}

unsigned int kgetcolor(void)
{
    return conio_color;
}

void ksetcursor(unsigned int column, unsigned int row)
{
    if (column >= CONIO_SCREEN_WIDTH) {
        column = CONIO_SCREEN_WIDTH - 1;
    }
    if (row >= CONIO_SCREEN_HEIGHT) {
        row = CONIO_SCREEN_HEIGHT - 1;
    }
    /* Row-major layout: every preceding row contributes 80 cells. Clamp
       before computing the index so later printing stays inside VGA memory. */
    conio_cursor = (uint16_t)(column + row * CONIO_SCREEN_WIDTH);
    conio_sync_hw_cursor();
}

void kgetcursor(unsigned int *column, unsigned int *row)
{
    *column = conio_cursor % CONIO_SCREEN_WIDTH;
    *row = conio_cursor / CONIO_SCREEN_WIDTH;
}
