#include <stdint.h>
#include <string.h>
#include "print.h"
#include "usbd/usbd.h"
#include "device/hid/keyboard.h"

#define GPIO_BASE   0x20200000

// Act LED is GPIO 47
#define GPFSEL4     (volatile uint32_t *)(GPIO_BASE + 0x10)
#define GPSET1      (volatile uint32_t *)(GPIO_BASE + 0x20)
#define GPCLR1      (volatile uint32_t *)(GPIO_BASE + 0x2c)

#define SYSTMR_BASE 0x20003000

#define SYSTMR_CS   (volatile uint32_t *)(SYSTMR_BASE + 0x00)
#define SYSTMR_CLO  (volatile uint32_t *)(SYSTMR_BASE + 0x04)
#define SYSTMR_C0   (volatile uint32_t *)(SYSTMR_BASE + 0x0c)
#define SYSTMR_C1   (volatile uint32_t *)(SYSTMR_BASE + 0x10)
#define SYSTMR_C2   (volatile uint32_t *)(SYSTMR_BASE + 0x14)
#define SYSTMR_C3   (volatile uint32_t *)(SYSTMR_BASE + 0x18)

#define MAIL0_BASE  0x2000b880

#define MAIL0_READ      (volatile uint32_t *)(MAIL0_BASE + 0x00)
#define MAIL0_STATUS    (volatile uint32_t *)(MAIL0_BASE + 0x18)
#define MAIL0_WRITE     (volatile uint32_t *)(MAIL0_BASE + 0x20)

#define MAIL0_CH_FB     1
#define MAIL0_CH_PROP   8

#define ARMTMR_BASE 0x2000b000

#define ARMTMR_LOAD (volatile uint32_t *)(ARMTMR_BASE + 0x400)
#define ARMTMR_VAL  (volatile uint32_t *)(ARMTMR_BASE + 0x404)
#define ARMTMR_CTRL (volatile uint32_t *)(ARMTMR_BASE + 0x408)
#define ARMTMR_IRQC (volatile uint32_t *)(ARMTMR_BASE + 0x40c)

#define INT_BASE    0x2000b000
#define INT_IRQBASPEND  (volatile uint32_t *)(INT_BASE + 0x200)
#define INT_IRQPEND1    (volatile uint32_t *)(INT_BASE + 0x204)
#define INT_IRQENAB1    (volatile uint32_t *)(INT_BASE + 0x210)
#define INT_IRQBASENAB  (volatile uint32_t *)(INT_BASE + 0x218)

#define INT_IRQ_ARMTMR  1

#define DMB() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 5" : : "r" (0) : "memory")
#define DSB() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory")

void _enable_int();
void _enable_mmu(uint32_t table_base_addr);
void _set_domain_access(uint32_t control);
void _flush_mmu_table();
void _standby();
uint32_t _get_mode();
void _enter_user_mode();

uint32_t mmu_table[4096] __attribute__((aligned(1 << 14)));

void mmu_table_section(uint32_t vaddr, uint32_t paddr, uint32_t flags)
{
    uint32_t *table_addr = (uint32_t *)((uint8_t *)mmu_table + (vaddr >> 18));
    uint32_t table_val = paddr | flags | 2;
    // 2 = Section; see ARM ARM B4-35
    *table_addr = table_val;
}

void send_mail(uint32_t data, uint8_t channel)
{
    DSB();
    while ((*MAIL0_STATUS) & (1u << 31)) { }
    *MAIL0_WRITE = (data << 4) | (channel & 15);
    DMB();
}

uint32_t recv_mail(uint8_t channel)
{
    DSB();
    do {
        while ((*MAIL0_STATUS) & (1u << 30)) { }
        uint32_t data = *MAIL0_READ;
        if ((data & 15) == channel) {
            DMB();
            return (data >> 4);
        }
    } while (1);
}

struct fb {
    uint32_t pwidth;
    uint32_t pheight;
    uint32_t vwidth;
    uint32_t vheight;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t xoffs;
    uint32_t yoffs;
    uint32_t buf;
    uint32_t size;
};

struct fb f;
static uint8_t gbuf[128 * 128 * 8];

void wait(uint32_t ticks)
{
    DSB();
    uint32_t val = *SYSTMR_CLO;
    *SYSTMR_C0 = val + ticks;
    while ((*SYSTMR_CS) & 1) { *SYSTMR_CS = 1; DMB(); DSB(); }
    while (((*SYSTMR_CS) & 1) == 0) { }
    while ((*SYSTMR_CS) & 1) { *SYSTMR_CS = 1; DMB(); DSB(); }
    DMB();
}

uint32_t get_time()
{
    DSB();
    uint32_t val = *SYSTMR_CLO;
    DMB();
    return val;
}

void murmur(uint32_t num)
{
    DSB();
    for (uint32_t i = 0; i < num; i++) {
        *GPCLR1 = (1 << 15);
        wait(200000);
        *GPSET1 = (1 << 15);
        wait(200000);
    }
    DMB();
}

#define mbox_buf(__sz) \
    struct buf_t {              \
        uint32_t size;          \
        uint32_t code;          \
        struct tag_t {          \
            uint32_t id;        \
            uint32_t size;      \
            uint32_t code;      \
            uint32_t val[__sz]; \
        } tag;                  \
        uint32_t end_tag;       \
    } __attribute__((aligned(16)))

uint32_t get_pixel_order()
{
    mbox_buf(1) buf;

    buf.size = sizeof buf;
    buf.code = 0;           // Request
    buf.tag.id = 0x40006;   // Get pixel order
    buf.tag.size = 4;       // Request length
    buf.tag.code = 0;       // Request
    buf.tag.val[0] = 123;   // Don't care
    buf.end_tag = 0;

    send_mail(((uint32_t)&buf) >> 4, MAIL0_CH_PROP);
    recv_mail(MAIL0_CH_PROP);
    return buf.tag.val[0];
}

void set_virtual_offs(uint32_t x, uint32_t y)
{
    mbox_buf(2) buf;

    buf.size = sizeof buf;
    buf.code = 0;
    buf.tag.id = 0x48009;   // Set virtual offset
    buf.tag.size = 8;
    buf.tag.code = 0;
    buf.tag.val[0] = x;
    buf.tag.val[1] = y;
    buf.end_tag = 0;

    send_mail(((uint32_t)&buf) >> 4, MAIL0_CH_PROP);
    recv_mail(MAIL0_CH_PROP);
}

static volatile bool new_frame = false;

void __attribute__((interrupt("IRQ"))) _int_irq()
{
    DMB(); DSB();
    *SYSTMR_CS = 8;
    uint32_t t = *SYSTMR_CLO;
    t = t - t % 16666 + 16666;
    *SYSTMR_C3 = t;
    DMB(); DSB();
    new_frame = true;
}

void __attribute__((interrupt("IRQ"))) _int_uinstr()
{
    _set_domain_access((3 << 2) | 3);
    while (1) { murmur(2); wait(1000000); }
}

void __attribute__((interrupt("IRQ"))) _int_uhandler()
{
    _set_domain_access((3 << 2) | 3);
    while (1) { murmur(3); wait(1000000); }
}

void __attribute__((interrupt("IRQ"))) _int_swi()
{
    _set_domain_access((3 << 2) | 3);
    while (1) { murmur(4); wait(1000000); }
}

void __attribute__((interrupt("IRQ"))) _int_pfabort()
{
    _set_domain_access((3 << 2) | 3);
    while (1) { murmur(5); wait(1000000); }
}

void __attribute__((interrupt("IRQ"))) _int_dabort()
{
    _set_domain_access((3 << 2) | 3);
    while (1) { murmur(6); wait(1000000); }
}

void draw()
{
    static uint32_t r = 255, g = 255, b = 255;
    static uint32_t seed = 4481192 + 415092;
    static uint32_t frm = 0, t0 = UINT32_MAX, t;
    if (t0 == UINT32_MAX) t0 = get_time();

    for (uint32_t y = 0; y < f.pheight; y++)
    for (uint32_t x = 0; x < f.pwidth; x++) {
        gbuf[y * f.pitch + x * 3 + 2] =
        gbuf[y * f.pitch + x * 3 + 1] =
        gbuf[y * f.pitch + x * 3 + 0] = 0;
    }
    for (uint32_t y = 0; y < f.pheight; y++)
    for (uint32_t x = 0; x < f.pwidth; x++) {
        gbuf[y * f.pitch + x * 3 + 2] = r;
        gbuf[y * f.pitch + x * 3 + 1] = (x == (frm << 1) % f.pwidth ? 0 : g);
        gbuf[y * f.pitch + x * 3 + 0] = b;
    }
    seed = ((seed * 1103515245) + 12345) & 0x7fffffff;
    r = (r == 255 ? r - 1 : (r == 144 ? r + 1 : r + ((seed >> 0) & 2) - 1));
    g = (g == 255 ? g - 1 : (g == 144 ? g + 1 : g + ((seed >> 1) & 2) - 1));
    b = (b == 255 ? b - 1 : (b == 144 ? b + 1 : b + ((seed >> 2) & 2) - 1));
    t = get_time() - t0;
    frm++;
    print_setbuf(gbuf);
    print_putchar('\r');
    print_putchar('0' + frm / 10000 % 10);
    print_putchar('0' + frm / 1000 % 10);
    print_putchar('0' + frm / 100 % 10);
    print_putchar('0' + frm / 10 % 10);
    print_putchar('0' + frm % 10);
    print_putchar(' ');
    print_putchar('0' + frm * 1000000 / t / 100);
    print_putchar('0' + frm * 1000000 / t / 10 % 10);
    print_putchar('0' + frm * 1000000 / t % 10);
}

void kernel_main()
{
    DSB();
    *GPFSEL4 |= (1 << 21);
    DMB();

    // Enable interrupts from the system timer
    // https://github.com/dwelch67/raspberrypi/tree/master/blinker07
    DSB();
    *INT_IRQENAB1 = 8;
    DMB();

    _enable_int();

    // Prepare TLB
    // Enable MMU!
    for (uint32_t i = 0; i < 4096; i++) {
        mmu_table_section(i << 20, i << 20, (i == 0 ? (8 | 4) : 0));
    }
    _enable_mmu((uint32_t)mmu_table);

/*
    // Set up framebuffer
    volatile struct fb f_volatile __attribute__((aligned(16))) = { 0 };
    f_volatile.pwidth = 128;
    f_volatile.pheight = 128;
    f_volatile.vwidth = 128;
    f_volatile.vheight = 128 * 2;
    f_volatile.bpp = 24;
    send_mail(((uint32_t)&f_volatile + 0x40000000) >> 4, MAIL0_CH_FB);
    recv_mail(MAIL0_CH_FB);

    f = f_volatile;

    uint8_t *buf = (uint8_t *)(f.buf);
    for (uint32_t y = 0; y < f.vheight; y++)
    for (uint32_t x = 0; x < f.vwidth; x++) {
        buf[y * f.pitch + x * 3 + 2] =
        buf[y * f.pitch + x * 3 + 1] =
        buf[y * f.pitch + x * 3 + 0] = 255;
    }

    DMB();
    print_init(gbuf, f.vwidth, f.vheight, f.pitch);
    print("Hello world!\nHello MIKAN!\n");
    print("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\n\n");
    DSB();

    uint32_t pix_ord = get_pixel_order();
    printf("Pixel order %s\n", pix_ord ? "RGB" : "BGR");

    // Start system timer
    DSB();
    *SYSTMR_CS = 8;
    *SYSTMR_C3 = *SYSTMR_CLO + 16666;
    DMB();

    uint8_t buffer_id = 0;
    uint32_t last_time = get_time();
    for (uint32_t i = 0; i < 500; i++) {
        new_frame = false;
        uint32_t t = get_time();
        draw();
        uint32_t virt_y = (buffer_id == 0 ? 0 : f.pheight);
        uint8_t *scr = buf + virt_y * f.pitch;
        memcpy(scr, gbuf, f.pitch * f.pheight);
        set_virtual_offs(0, virt_y);
        buffer_id ^= 1;
        print_setbuf(scr);
        printf("|%d", _get_mode());
        last_time = t;
    }
*/

    *GPCLR1 = (1 << 15);
    wait(1000000);

    mmu_table_section(0x20000000, 0x20000000, 1 << 5);
    mmu_table_section(0x20200000, 0x20200000, 1 << 5);
    _set_domain_access((3 << 2) | 3);
    _flush_mmu_table();

    *GPSET1 = (1 << 15);
    wait(1000000);
    *GPCLR1 = (1 << 15);
    wait(1000000);

    // Uncommenting will result in a fault
    //_set_domain_access(3);
    _enter_user_mode();
    while (1) {
        //murmur(5);
        //wait(100000);
        for (uint32_t i = 0; i < 30000000; i++) __asm__ __volatile__ ("");
        *GPCLR1 = (1 << 15);
        for (uint32_t i = 0; i < 30000000; i++) __asm__ __volatile__ ("");
        *GPSET1 = (1 << 15);
    }
}
