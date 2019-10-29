#include "common.h"
#include "user/elf/elf.h"

#include "user_c_a_out.h"

extern unsigned char _bss_dmem_begin;
extern unsigned char _bss_dmem_end;

uint32_t mm_sys[4096] __attribute__((aligned(1 << 14)));
uint32_t mm_user[4096] __attribute__((aligned(1 << 14)));

void mmu_table_section(uint32_t *table, uint32_t vaddr, uint32_t paddr, uint32_t flags)
{
    uint32_t *table_addr = (uint32_t *)((uint8_t *)table + (vaddr >> 18));
    uint32_t table_val = paddr | flags | 2;
    // 2 = Section; see ARM ARM B4-27
    *table_addr = table_val;
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
static uint8_t gbuf[256 * 256 * 8];

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

uint32_t get_pixel_order()
{
    static mbox_buf(4) buf __attribute__((section(".bss.dmem"), aligned(16)));
    mbox_init(buf);
    buf.tag.id = 0x40006;   // Get pixel order
    buf.tag.u32[0] = 123;
    mbox_emit(buf);
    return buf.tag.u32[0];
}

void set_virtual_offs(uint32_t x, uint32_t y)
{
    static mbox_buf(8) buf __attribute__((section(".bss.dmem"), aligned(16)));
    mbox_init(buf);
    buf.tag.id = 0x48009;   // Set virtual offset
    buf.tag.u32[0] = x;
    buf.tag.u32[1] = y;
    mbox_emit(buf);
}

uint32_t get_clock_rate(uint8_t id)
{
    static mbox_buf(8) buf __attribute__((section(".bss.dmem"), aligned(16)));
    mbox_init(buf);
    buf.tag.id = 0x30002;   // Get clock rate
    buf.tag.u32[0] = id;
    mbox_emit(buf);
    return buf.tag.u32[1];
}

uint32_t set_clock_rate(uint8_t id, uint32_t hz)
{
    static mbox_buf(12) buf __attribute__((section(".bss.dmem"), aligned(16)));
    mbox_init(buf);
    buf.tag.id = 0x38002;   // Set clock rate
    buf.tag.u32[0] = id;
    buf.tag.u32[1] = hz;
    buf.tag.u32[2] = 0;
    mbox_emit(buf);
    return buf.tag.u32[1];
}

uint32_t get_min_clock_rate(uint8_t id)
{
    static mbox_buf(8) buf __attribute__((section(".bss.dmem"), aligned(16)));
    mbox_init(buf);
    buf.tag.id = 0x30007;   // Get min clock rate
    buf.tag.u32[0] = id;
    mbox_emit(buf);
    return buf.tag.u32[1];
}

uint32_t get_max_clock_rate()
{
    static mbox_buf(8) buf __attribute__((section(".bss.dmem"), aligned(16)));
    mbox_init(buf);
    buf.tag.id = 0x30004;   // Get max clock rate
    buf.tag.u32[0] = 3;     // ARM
    mbox_emit(buf);
    return buf.tag.u32[1];
}

void __attribute__((interrupt("UNDEFINED"))) _int_uinstr()
{
    _set_domain_access((3 << 2) | 3);
    DSB();
    printf("Undefined Instruction\n");
    DMB();
    while (1) { murmur(2); wait(1000000); }
}

void __attribute__((interrupt("UNDEFINED"))) _int_uhandler()
{
    _set_domain_access((3 << 2) | 3);
    DSB();
    printf("Undefined Handler\n");
    DMB();
    while (1) { murmur(3); wait(1000000); }
}

typedef void (*update_func_t)();
typedef void *(*draw_func_t)();

static update_func_t _update = NULL;
static draw_func_t _draw = NULL;

uint32_t _int_swi(uint32_t r0, uint32_t r1, uint32_t r2)
{
    _set_domain_access((3 << 2) | 3);
    DMB(); DSB();
    //printf("SVC %d\n", r0);
    //DMB(); DSB();
    uint32_t ret = 0;
    if (r0 == 1) {
        _update = (update_func_t)r1;
        _draw = (draw_func_t)r2;
    } else if (r0 == 2) {
        if (get_time() & 1048576) ret = 1;
    } else if (r0 == 42) {
        *GPCLR1 = r1;
    } else if (r0 == 43) {
        *GPSET1 = r1;
    }
    DMB(); DSB();
    _set_domain_access((1 << 2) | 3);
    return ret;
}

void __attribute__((interrupt("ABORT"))) _int_pfabort()
{
    _set_domain_access((3 << 2) | 3);
    DSB();
    printf("Prefetch Abort\n");
    DMB();
    while (1) { murmur(5); wait(1000000); }
}

void __attribute__((interrupt("ABORT"))) _int_dabort()
{
    _set_domain_access((3 << 2) | 3);
    DSB();
    printf("Data Abort\n");
    DMB();
    while (1) {
        for (uint32_t i = 0; i < 10000000; i++) __asm__ __volatile__ ("");
        *GPCLR1 = (1 << 15);
        for (uint32_t i = 0; i < 10000000; i++) __asm__ __volatile__ ("");
        *GPSET1 = (1 << 15);
    }
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
    _putchar('\r');
    _putchar('0' + frm / 10000 % 10);
    _putchar('0' + frm / 1000 % 10);
    _putchar('0' + frm / 100 % 10);
    _putchar('0' + frm / 10 % 10);
    _putchar('0' + frm % 10);
    _putchar(' ');
    _putchar('0' + frm * 1000000 / t / 100);
    _putchar('0' + frm * 1000000 / t / 10 % 10);
    _putchar('0' + frm * 1000000 / t % 10);
}

void qwqwq(TKernelTimerHandle h, void *_u1, void *_u2)
{
    _putchar('>');
    _putchar('0' + h / 10);
    _putchar('0' + h % 10);
    _putchar('\n');
}

void status_handler(unsigned int index, const USPiGamePadState *state)
{
    const uint8_t *report = state->report;
    uint32_t report_len = state->report_len;
    for (int i = 0; i < report_len; i++) printf(" %02x", report[i]);
    _putchar('\r');
    _putchar('\b');
    _putchar('\b');
    return;

    printf("GP %u", index);
    uint32_t naxes = state->naxes;
    uint32_t nhats = state->nhats;
    uint32_t nbtns = state->nbuttons;
    for (uint32_t i = 0; i < naxes; i++) printf(" %3d", state->axes[i].value); _putchar('|');
    for (uint32_t i = 0; i < nhats; i++) printf(" %x", state->hats[i]); _putchar('|');
    printf(" %04x\r", state->buttons);
}

void load_program(const elf_ehdr *ehdr, const elf_phdr *program)
{
    const char *buf = (const char *)ehdr;
    uint32_t empty_len =
        (program->filesz < program->memsz ? program->memsz - program->filesz : 0);
    memcpy((void *)program->vaddr, buf + program->offs, program->filesz);
    memset((void *)program->vaddr + program->filesz, 0, empty_len);
}

void timer3_handler(void *_unused)
{
    _set_domain_access((3 << 2) | 3);

    do *SYSTMR_CS = 8; while (*SYSTMR_CS & 8);
    uint32_t t = *SYSTMR_CLO;
    t = t - t % 16667 + 16667;
    *SYSTMR_C3 = t;

    // TODO: Do not run these with high privileges!
    if (_update) (*_update)();
    if (_draw) {
        uint8_t *ret = (uint8_t *)(*_draw)();
        uint8_t *buf = (uint8_t *)(f.buf);
        for (uint32_t y = 0; y < f.pheight; y++)
        for (uint32_t x = 0; x < f.pwidth; x++) {
            buf[y * f.pitch + x * 3 + 2] = ret[0];
            buf[y * f.pitch + x * 3 + 1] = ret[1];
            buf[y * f.pitch + x * 3 + 0] = ret[2];
            ret += 3;
        }
    }
    _set_domain_access((1 << 2) | 3);
}

void kernel_main()
{
    DSB();
    *GPFSEL4 |= (1 << 21);
    DMB();

    _enable_int();

    // 60 FPS tick
    DSB();
    *SYSTMR_CS = 8;
    *SYSTMR_C3 = 3000000;
    DMB(); DSB();
    *INT_IRQENAB1 = 8;
    DMB(); DSB();
    set_irq_handler(3, timer3_handler, NULL);
    DMB();

    // Prepare TLB
    for (uint32_t i = 0; i < 4096; i++) {
        mmu_table_section(mm_sys, i << 20, i << 20, (i < 1 ? (8 | 4) : 0));
    }
    // Disable buffering/caching on .bss.dmem(qwq) sections
    uint32_t dmem_start = ((uint32_t)&_bss_dmem_begin) >> 20;
    uint32_t dmem_end = ((uint32_t)&_bss_dmem_end - 1) >> 20;
    for (uint32_t i = dmem_start; i < dmem_end; i++) {
        mmu_table_section(mm_sys, i << 20, i << 20, 0);
    }
    _enable_mmu((uint32_t)mm_sys);

    // Set up framebuffer
    static struct fb f_volatile __attribute__((section(".bss.dmem"), aligned(16))) = { 0 };
    f_volatile.pwidth = 256;
    f_volatile.pheight = 256;
    f_volatile.vwidth = 256;
    f_volatile.vheight = 256 * 2;
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

    uint32_t buf_p = (uint32_t)buf;
    buf_p = (buf_p >> 20) << 20;
    // Region attributes: B4-12
    // Descriptor: B4-27
    // AP = (3 bits << 12), C = 8, B = 4
    for (uint32_t i = 0; i < 4; i++)
        mmu_table_section(mm_sys, buf_p + (i << 20), buf_p + (i << 20), 12);
    _flush_mmu_table();

    DMB();
    print_init(buf, f.pwidth, f.pheight, f.pitch);
    printf("Hello world!\nHello MIKAN!\n");
    printf("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\n\n");
    printf("%d %d\n", dmem_start, dmem_end);
    DSB();

    uint32_t pix_ord = get_pixel_order();
    printf("Pixel order %s\n", pix_ord ? "RGB" : "BGR");

    for (uint8_t i = 1; i <= 9; i++) {
        printf("Clock %u rate range %u - %u\n", i, get_min_clock_rate(i), get_max_clock_rate(i));
        printf("Current clock rate %u\n", get_clock_rate(i));
    }
    set_clock_rate(3, (get_min_clock_rate(3) + get_max_clock_rate(3)) / 2);

    uspios_init();

    uint8_t mac_addr[6];
    GetMACAddress(mac_addr);
    printf("MAC address:\n");
    DebugHexdump(mac_addr, 6, NULL);

/*
    USPiInitialize();
    printf("!!!!!!!\n");

    uint32_t count = USPiGamePadAvailable();
    printf("%d gamepad(s)\n", count);
    USPiGamePadRegisterStatusHandler(status_handler);
*/

    // Set domain to 1
    // Set AP = 0b01 (privileged access only) (ARM ARM p. B4-9/B4-27)
    memcpy(mm_user, mm_sys, sizeof mm_user);
    mmu_table_section(mm_user, 0x20000000, 0x20000000, (1 << 5) | (1 << 10));
    mmu_table_section(mm_user, 0x20200000, 0x20200000, (1 << 5) | (1 << 10));
    // Open up enough space for user code
    for (uint32_t i = 0x80000000; i < 0x90000000; i += 0x100000) {
        mmu_table_section(mm_user, i, i - 0x80000000 + 0x1000000, 8 | 4);
    }
    _enable_mmu((uint32_t)mm_user);
    // Client for domain 1, Manager for domain 0
    _set_domain_access((1 << 2) | 3);

    load_elf(user_c_a_out);
    const elf_ehdr *ehdr = (const elf_ehdr *)user_c_a_out;

    _enter_user_mode();
    _enter_user_code(ehdr->entry);

    // Should not reach here
    while (1) { }
}
