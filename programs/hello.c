extern void serial_puts(const char* s);

int main(int argc, char** argv) {
    serial_puts("================================\n");
    serial_puts("  Hello from MWP program!\n");
    serial_puts("  MWOS Program Format Demo\n");
    serial_puts("================================\n");
    return 0;
}
