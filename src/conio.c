#include "types.h"
#include "conio.h"

#include <stdarg.h>

#define CONIO_SCREEN_WIDTH 80u
#define CONIO_SCREEN_HEIGHT 25u
#define CONIO_SCREEN_CELLS (CONIO_SCREEN_WIDTH * CONIO_SCREEN_HEIGHT)
#define CONIO_VIDEO_MEMORY ((volatile uint16_t *)0x000B8000u)

static uint8_t conio_color = 0x07;
static uint16_t conio_cursor;

static inline void outb(uint16_t port, uint8_t value)
{
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

    outb(0x3D4, 0x0A);
    value = inb(0x3D5);
    outb(0x3D5, (uint8_t)(value & 0xDF));

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)conio_cursor);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(conio_cursor >> 8));
}

static void conio_scroll(void)
{
    uint32_t index;

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
    kputc('0');
    kputc('x');
    kprint_hex_dword((uint32_t)(value >> 32));
    kprint_hex_dword((uint32_t)value);
}

void kprint_dec(uint32_t value)
{
    char digits[10];
    uint32_t count = 0;

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
            kputc((unsigned int)va_arg(arguments, int));
            break;
        case 'd': {
            int value = va_arg(arguments, int);
            uint32_t magnitude;
            if (value < 0) {
                kputc('-');
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
    conio_cursor = (uint16_t)(column + row * CONIO_SCREEN_WIDTH);
    conio_sync_hw_cursor();
}

void kgetcursor(unsigned int *column, unsigned int *row)
{
    *column = conio_cursor % CONIO_SCREEN_WIDTH;
    *row = conio_cursor / CONIO_SCREEN_WIDTH;
}
