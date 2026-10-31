/*
 * Public VGA console, cursor, color, and formatting interface.
 * Functions use 32-bit cdecl so both assembly and C can print without a C runtime.
 */

#ifndef XNF_CONIO_H
#define XNF_CONIO_H

void clear_screen(void);
/* Each normal-context call excludes thread switches while it updates output;
   IRQ handlers must not use the console. Fatal exception output is terminal. */
void kputc(unsigned int character);
void kputs(const char *string);
void kprint_hex(unsigned int value);
void kprint_hex64(unsigned long long value);
void kprint_dec(unsigned int value);
/* Supported: %s, %c, signed %d, prefixed 32-bit %x, 64-bit %X, and %%. */
void kprintf(const char *format, ...);
/* VGA attribute: foreground in bits 0..3; background in bits 4..6, with bit 7
   interpreted by the current VGA blink/intensity configuration. */
void ksetcolor(unsigned int attribute);
unsigned int kgetcolor(void);
/* Coordinates are cells, not pixels; setter clamps, getter requires writable
   output pointers for both column and row. */
void ksetcursor(unsigned int column, unsigned int row);
void kgetcursor(unsigned int *column, unsigned int *row);

#endif
