#ifndef XNF_CONIO_H
#define XNF_CONIO_H

void clear_screen(void);
void kputc(unsigned int character);
void kputs(const char *string);
void kprint_hex(unsigned int value);
void kprint_hex64(unsigned long long value);
void kprint_dec(unsigned int value);
void kprintf(const char *format, ...);
void ksetcolor(unsigned int attribute);
unsigned int kgetcolor(void);
void ksetcursor(unsigned int column, unsigned int row);
void kgetcursor(unsigned int *column, unsigned int *row);

#endif
