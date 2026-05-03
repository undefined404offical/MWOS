#include "kernel.h"
#include "thread.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43
#define PIT_FREQ 1193182

static volatile uint64_t timer_ticks = 0;

void timer_callback(interrupt_frame_t *frame)
{
    (void)frame;
    timer_ticks++;
    outb(0x20, 0x20);
    scheduler_tick(frame);
}

// 初始化 PIT
void timer_init(uint32_t frequency)
{
    register_interrupt_handler(32, timer_callback);

    uint32_t divisor = PIT_FREQ / frequency;

    outb(PIT_COMMAND, 0x36);

    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    serial_puts("PIT: Timer initialized at ");
    serial_putdec64(frequency);
    serial_puts(" Hz\n");
}

uint64_t timer_get_ticks(void)
{
    return timer_ticks;
}

void sleep_ms(uint32_t ms)
{
    uint64_t target = timer_ticks + ms;
    while (timer_ticks < target)
    {
        asm volatile("hlt");
    }
}

uint64_t timer_ms(void)
{
    return timer_get_ticks();
}
