#include <unistd.h>

int main(void)
{
    write(1, "Hello World!\n", 13);
    _exit(0);
    return 0;
}