extern void kputs(const char *message);

void kernel_main(void)
{
    kputs("Hello from C!");
}
