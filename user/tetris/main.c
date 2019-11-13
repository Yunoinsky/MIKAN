#include "api.h"
#include "tetris.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI  3.1415926535897932384626433832795
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2   0.7071067811865476
#endif

#define MINO_W      11

#define MATRIX_X1   ((400 - MATRIX_W * MINO_W) / 2)
#define MATRIX_Y1   (240 - (240 - MATRIX_HV * MINO_W) / 2)
#define MATRIX_X2   (MATRIX_X1 + MATRIX_W * MINO_W)
#define MATRIX_Y2   (MATRIX_Y1 - MATRIX_HV * MINO_W)

#define CHAR_W  7
#define CHAR_H  14
static uint8_t font_data[CHAR_W * CHAR_H * 16 * 6];

static uint8_t buf[240][400][3];

#define SCR_MENU    0
#define SCR_GAME    1
#define SCR_WIN     2
#define SCR_LOSE    3
static uint8_t screen;

#define MODE_MARATHON   0
#define MODE_SPRINT     1
#define MODE_ULTRA      2
static uint8_t mode;
static uint8_t level;

#define ULTRA_DURATION  120

static uint32_t T;
static uint32_t b0, b1;

static void game_init();
static void game_draw();
static void overlay_init();
static void overlay_draw();

static inline void pix(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b)
{
    buf[y][x][2] = r;
    buf[y][x][1] = g;
    buf[y][x][0] = b;
}

static inline void pix_semi(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b)
{
    buf[y][x][2] += ((r - buf[y][x][2]) >> 2) * 3;
    buf[y][x][1] += ((g - buf[y][x][1]) >> 2) * 3;
    buf[y][x][0] += ((b - buf[y][x][0]) >> 2) * 3;
}

static inline void pix_alpha(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    buf[y][x][2] += ((uint16_t)(r - buf[y][x][2]) * a) >> 8;
    buf[y][x][1] += ((uint16_t)(g - buf[y][x][1]) * a) >> 8;
    buf[y][x][0] += ((uint16_t)(b - buf[y][x][0]) * a) >> 8;
}

static inline void lineh(
    uint16_t x1, uint16_t x2, uint16_t y, uint8_t r, uint8_t g, uint8_t b)
{
    for (; x1 <= x2; x1++) pix(x1, y, r, g, b);
}

static inline void linev(
    uint16_t x, uint16_t y1, uint16_t y2, uint8_t r, uint8_t g, uint8_t b)
{
    for (; y1 <= y2; y1++) pix(x, y1, r, g, b);
}

static inline void text_char(uint16_t x, uint16_t y, signed char ch)
{
    if (ch < 32) return;
    uint8_t row = (ch - 32) / 16;
    uint8_t col = (ch - 32) % 16;
    uint16_t ptr = row * CHAR_W * 16 * CHAR_H + col * CHAR_W;
    for (uint8_t j = 0; j < CHAR_H; j++)
    for (uint8_t i = 0; i < CHAR_W; i++)
        if (font_data[ptr + (j * CHAR_W * 16 + i)]) {
            pix(x + i, y + j, 255, 255, 255);
            pix(x + i + 1, y + j + 1, 108, 108, 108);
        }
}

static inline void text_str(uint16_t x, uint16_t y, const char *str)
{
    while (*str != 0) {
        text_char(x, y, *(str++));
        x += CHAR_W;
    }
}

static inline void text_xcen(uint16_t x, uint16_t y, const char *str)
{
    size_t n = strlen(str);
    if (n > 0) text_str(x - (n * CHAR_W / 2), y, str);
}


static const uint8_t bg[] = {
    // ffmpeg -f rawvideo -pix_fmt rgb24 - -i background01.png | xxd -i > bg.h
    #include "bg.h"
};


#define PARTICLES_MAX   4096
#define PARTICLE_LIFE   60
#define PARTICLE_RADIUS 8

static uint16_t particle_count = 0;
static struct particle {
    uint16_t x, y;
    uint8_t r, g, b;
    float vx, vy;
    uint32_t t;
    uint16_t life;
} particles[PARTICLES_MAX];

// XXX: Duplication?
static inline uint32_t mrand()
{
    static uint32_t seed = 20191103;
    return (seed = ((seed * 1103515245) + 12345) & 0x7fffffff);
}

static inline void add_particle(uint32_t T, uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b)
{
    if (particle_count >= PARTICLES_MAX) return;
    float a = (float)mrand() / 0x80000000u * M_PI * 2;
    uint16_t t = mrand() % PARTICLE_LIFE;
    float l = ((float)mrand() / 0x80000000u * 0.4f + 0.8f);
    particles[particle_count++] = (struct particle) {
        x, y, r, g, b, PARTICLE_RADIUS * l * cosf(a), PARTICLE_RADIUS * l * sinf(a), T, t
    };
}

static inline void update_and_draw_particles()
{
    for (uint16_t i = 0; i < particle_count; i++) {
        if (T >= particles[i].t + particles[i].life) {
            // To be removed
            particles[i] = particles[--particle_count];
            i--;
        } else {
            float r0 = (float)(T - particles[i].t) / particles[i].life;
            float r = 1 - expf(-r0 * 5);
            pix_alpha(
                (uint16_t)(particles[i].x + particles[i].vx * r),
                (uint16_t)(particles[i].y + particles[i].vy * r),
                particles[i].r, particles[i].g, particles[i].b,
                (uint8_t)(255 * (1 - r0) + 0.5f));
        }
    }
}


////// MENU //////

static uint8_t menu_sel = 0;
static uint8_t last_menu_sel = 0;
static uint32_t menu_sel_time = 0;
#define MENU_TRANSITION_DUR 15

static void menu_update()
{
    T++;

    uint8_t m0 = menu_sel;

    if ((b0 & BUTTON_DOWN) && !(b1 & BUTTON_DOWN)) menu_sel = (menu_sel + 1) % 3;
    if ((b0 & BUTTON_UP) && !(b1 & BUTTON_UP)) menu_sel = (menu_sel + 2) % 3;
    if ((b0 & BUTTON_CRO) && !(b1 & BUTTON_CRO)) {
        screen = SCR_GAME;
        mode = menu_sel;
        game_init();
    }
    if ((b0 & BUTTON_CIR) && !(b1 & BUTTON_CIR)) {
        uint16_t x = mrand() % 128 + 64;
        uint16_t y = mrand() % 128 + 64;
        for (uint8_t i = 0; i < 64; i++)
            add_particle(T, x, y,
                mrand() % 128 + 128, mrand() % 128 + 128, mrand() % 128 + 128);
    }

    if (menu_sel != m0) {
        last_menu_sel = m0;
        menu_sel_time = T;
    }
}

static void menu_draw()
{
    text_xcen(200, 60, "= T E T R I S =");
    text_xcen(200, 120, "Marathon");
    text_xcen(200, 120 + 24, "Sprint");
    text_xcen(200, 120 + 48, "Ultra");

    int16_t x = sinf((float)T * 0.06f) * 1.9;
    uint16_t y = menu_sel * 24;
    if (menu_sel_time + MENU_TRANSITION_DUR >= T) {
        float rate = expf((float)(T - menu_sel_time) / MENU_TRANSITION_DUR * -4);
        y -= (menu_sel - last_menu_sel) * rate * 24;
    }

    text_char(144 - CHAR_W / 2 + x, 123 + y, '~');
    text_char(256 - CHAR_W / 2 - x, 123 + y, '~');

    update_and_draw_particles();
}


////// GAME //////

static int8_t last_hor = 0;
static uint32_t hor_hold = 0;
static uint32_t drop_hold = 0;

static const uint8_t MINO_COLOURS[7][3] = {
    {254, 203, 0},
    {0, 159, 218},
    {149, 45, 152},
    {255, 121, 0},
    {0, 101, 189},
    {105, 190, 40},
    {237, 41, 57}
};

static void game_init()
{
    T = 0;
    level = 1;

    tetro_init();
    tetris_spawn();
}

static void game_update()
{
    T++;

    int32_t hor = 0;
    if (b0 & BUTTON_LEFT) hor -= 1;
    if (b0 & BUTTON_RIGHT) hor += 1;
    if (hor != 0) {
        if (hor_hold == 0 || (hor_hold >= 20 && hor_hold % 2 == 0))
            tetris_hor(hor);
        hor_hold++;
    } else {
        hor_hold = 0;
    }
    last_hor = hor;

    if (b0 & BUTTON_DOWN) {
        if (drop_hold++ % 2 == 0) tetris_drop();
    } else {
        drop_hold = 0;
    }

    if (((b0 & BUTTON_CIR) && !(b1 & BUTTON_CIR)) ||
        ((b0 & BUTTON_UP) && !(b1 & BUTTON_UP)))
        tetris_rotate(+1);
    if ((b0 & BUTTON_CRO) && !(b1 & BUTTON_CRO)) tetris_rotate(-1);
    if ((b0 & BUTTON_SQR) && !(b1 & BUTTON_SQR)) tetris_harddrop();
    if ((b0 & BUTTON_TRI) && !(b1 & BUTTON_TRI)) tetris_hold();

    uint32_t action = tetris_tick();
    if (action & TETRIS_LOCKDOWN) {
        tetris_spawn();
        if (action ^ TETRIS_LOCKDOWN) {
            for (uint8_t i = 0, k = 0; i < MATRIX_H; i++) if (action & (1 << i)) {
                for (uint8_t c = 0; c < MATRIX_W; c++) {
                    // Generate a block of particles
                    uint16_t x0 = MATRIX_X1 + c * MINO_W;
                    uint16_t y0 = MATRIX_Y1 - (i + 1) * MINO_W;
                    uint8_t t = recent_clear[k][c];
                    uint8_t r = MINO_COLOURS[t][0] + ((255 - MINO_COLOURS[t][0]) >> 1);
                    uint8_t g = MINO_COLOURS[t][1] + ((255 - MINO_COLOURS[t][1]) >> 1);
                    uint8_t b = MINO_COLOURS[t][2] + ((255 - MINO_COLOURS[t][2]) >> 1);
                    for (uint8_t x = 0; x < MINO_W; x += 4)
                    for (uint8_t y = 0; y < MINO_W; y += 4)
                        add_particle(T, x0 + x, y0 + y, r, g, b);
                }
                k++;
            }
        }
    }
    if (action & TETRIS_GAMEOVER) {
        screen = SCR_LOSE;
    } else if (
        (mode == MODE_SPRINT && clear_count >= 40) ||
        (mode == MODE_ULTRA && T >= ULTRA_DURATION * 60)
    ) {
        screen = SCR_WIN;
    }

    if (mode == MODE_MARATHON) {
        level = 1 + (clear_count / 10);
        drop_interval = 120 / (level + 3);
    }
}

// Top-left corner
static inline void draw_mino(int8_t row, int8_t col, uint8_t t)
{
    if (row >= MATRIX_HV) return;
    uint16_t x = MATRIX_X1 + col * MINO_W;
    uint16_t y = MATRIX_Y1 - (row + 1) * MINO_W;
    for (int i = 0; i < MINO_W; i++)
    for (int j = 0; j < MINO_W; j++) {
        uint8_t r = MINO_COLOURS[t][0];
        uint8_t g = MINO_COLOURS[t][1];
        uint8_t b = MINO_COLOURS[t][2];
        if (i == MINO_W - 1 || j == MINO_W - 1) {
            r = (r >> 2) * 3;
            g = (g >> 2) * 3;
            b = (b >> 2) * 3;
        } else if (i == 0 || j == 0) {
            r = 255 - ((255 - r) >> 2) * 3;
            g = 255 - ((255 - g) >> 2) * 3;
            b = 255 - ((255 - b) >> 2) * 3;
        }
        pix(x + i, y + j, r, g, b);
    }
}

static inline void draw_mino_ghost(uint8_t row, uint8_t col, uint8_t t)
{
    if (row >= MATRIX_HV) return;
    uint16_t x = MATRIX_X1 + col * MINO_W;
    uint16_t y = MATRIX_Y1 - (row + 1) * MINO_W;
    for (int i = 0; i < MINO_W; i++)
    for (int j = 0; j < MINO_W; j++)
        pix_semi(x + i, y + j,
            MINO_COLOURS[t][0] / 2, MINO_COLOURS[t][1] / 2, MINO_COLOURS[t][2] / 2);
}

static inline void draw_matrix()
{
    for (int i = 0; i <= MATRIX_HV; i++)
        lineh(MATRIX_X1, MATRIX_X2, MATRIX_Y1 - i * MINO_W, 96, 96, 96);
    for (int j = 0; j <= MATRIX_W; j++)
        linev(MATRIX_X1 + j * MINO_W, MATRIX_Y2, MATRIX_Y1, 96, 96, 96);

    for (int i = 0; i < MATRIX_HV; i++)
    for (int j = 0; j < MATRIX_W; j++) {
        if (matrix[i][j] != MINO_NONE)
            draw_mino(i, j, matrix[i][j]);
    }

    // Ghost
    uint8_t ghost_row = tetris_ghost();
    for (int i = 0; i < 4; i++)
        draw_mino_ghost(
            ghost_row + TETRO[drop_type].mino[drop_ori][i][0],
            drop_pos[1] + TETRO[drop_type].mino[drop_ori][i][1],
            drop_type);
    // Dropping
    for (int i = 0; i < 4; i++)
        draw_mino(
            drop_pos[0] + TETRO[drop_type].mino[drop_ori][i][0],
            drop_pos[1] + TETRO[drop_type].mino[drop_ori][i][1],
            drop_type);

    // Hold
    text_str(82, 10, "Hold");
    if (hold_type != MINO_NONE)
        for (int i = 0; i < 4; i++)
            draw_mino(
                MATRIX_HV - 2 - TETRO[hold_type].bbsize + TETRO[hold_type].mino[0][i][0],
                -5 + TETRO[hold_type].mino[0][i][1],
                hold_type);

    // Preview
    text_str(269, 10, "Next");
    for (int i = 0; i < 4; i++)     // Preview index
    for (int j = 0; j < 4; j++) {   // Mino index
        uint8_t t = drop_next[(drop_pointer + i) % 14];
        draw_mino(
            MATRIX_HV - 2 - i * 3 - TETRO[t].bbsize + TETRO[t].mino[0][j][0],
            MATRIX_W + 2 + TETRO[t].mino[0][j][1],
            t);
    }

    update_and_draw_particles();

    char s[4] = { 0 };

    text_str(82, 120, "Clear");
    s[0] = '0' + clear_count / 100;
    s[1] = '0' + clear_count / 10 % 10;
    s[2] = '0' + clear_count % 10;
    text_str(104, 136, s);

    if (mode == MODE_MARATHON) {
        text_str(82, 172, "Level");
        s[0] = '0' + level / 100;
        s[1] = '0' + level / 10 % 10;
        s[2] = '0' + level % 10;
        if (s[0] == '0') {
            s[0] = ' ';
            if (s[1] == '0') s[1] = ' ';
        }
        text_str(104, 188, s);
    } else {
        text_str(82, 172, "Time");
        uint32_t time = (mode == MODE_SPRINT ? T / 60 : ULTRA_DURATION - (T / 60));
        s[0] = '0' + time / 100;
        s[1] = '0' + time / 10 % 10;
        s[2] = '0' + time % 10;
        text_str(104, 188, s);
    }
}

static void game_draw()
{
    draw_matrix();
}


////// OVERLAY //////

void overlay_update()
{
    if ((b0 & BUTTON_CRO) && !(b1 & BUTTON_CRO)) {
        // Restart
        screen = SCR_GAME;
        game_init();
    } else if ((b0 & BUTTON_CIR) && !(b1 & BUTTON_CIR)) {
        // Back
        last_menu_sel = menu_sel;
        menu_sel_time = T = 0;
        screen = SCR_MENU;
        particle_count = 0; // Clear particles
    }
}

void overlay_draw()
{
    uint8_t *_buf = &buf[0][0][0];
    for (size_t i = 0; i < sizeof buf; i++) _buf[i] = ((uint16_t)_buf[i] * 3) >> 3;
    //uint8_t *start = &buf[64][0][0], *end = &buf[112][0][0];
    //for (; start < end; start++) *start = ((uint16_t)*start * 3) >> 3;

    const char *msg = (screen == SCR_WIN ?
        (mode == MODE_SPRINT ? "Supercalifragilisticexpialidocious" : "It's been great work!") :
        (mode == MODE_MARATHON ? "It's been great work!" : "Oops! Try again"));
    text_xcen(200, 88, msg);
    text_str(155, 120, "[X] - Restart");
    text_str(155, 136, "[O] - Back");
}


////// MAIN //////

void bg_draw()
{
    for (uint16_t y = 0; y < 240; y++)
    for (uint16_t x = 0; x < 400; x++)
        memcpy(buf[y][x], bg + ((x * 256 / 400) + ((y + 80) * 256 / 400) * 256) * 3, 3);
}

void init()
{
    b0 = b1 = 0;
    screen = SCR_MENU;
}

void update()
{
    b1 = b0;
    b0 = buttons();
    switch (screen) {
        case SCR_MENU: menu_update(); break;
        case SCR_GAME: game_update(); break;
        case SCR_WIN: case SCR_LOSE: overlay_update(); break;
        default: break;
    }
}

void *draw()
{
    bg_draw();
    switch (screen) {
        case SCR_MENU: menu_draw(); break;
        case SCR_GAME: game_draw(); break;
        case SCR_WIN: case SCR_LOSE:
            game_draw();
            overlay_draw();
        default: break;
    }
    return (void *)buf;
}

// print('\n'.join(map(lambda x: ''.join(map(chr, range(32 + 16 * x, 48 + 16 * x))), range(6))))
// ffmpeg -f rawvideo -pix_fmt gray - -i tamzen14.png | hexdump -ve '1/1 "%.2x"' | fold -w96 | sed -e 's/00/0,/g' | sed -e 's/ff/1,/g'

static uint8_t font_data[CHAR_W * CHAR_H * 16 * 6] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,
    0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,1,0,0,0,0,1,0,1,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,1,0,1,0,0,
    0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,1,0,0,0,0,1,0,1,0,0,0,0,1,1,
    1,1,0,1,0,1,0,0,1,0,0,1,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,
    1,0,1,0,0,0,1,1,1,1,1,0,0,1,0,0,0,0,0,1,0,1,0,1,0,0,0,1,0,1,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,
    0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,
    0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,1,0,0,0,0,0,0,1,0,1,0,0,0,0,0,1,0,0,1,
    0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,1,0,1,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,0,1,1,
    1,0,0,0,0,1,0,1,0,0,0,1,0,1,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,1,1,1,0,0,0,1,1,
    1,1,1,0,0,0,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,1,0,0,1,0,1,0,1,0,0,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,
    0,0,0,1,0,0,0,1,0,1,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,0,0,0,0,1,0,1,0,0,1,0,1,0,0,1,0,0,1,1,
    0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,
    0,0,0,0,1,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,1,1,1,
    1,0,0,0,0,0,0,1,0,0,0,0,1,1,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0,1,0,0,0,0,0,
    1,1,1,0,0,0,1,1,1,1,1,0,0,0,0,0,1,0,0,0,1,1,1,1,1,0,0,0,0,1,1,0,0,0,1,1,1,1,1,0,0,0,1,1,1,0,0,0,
    0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,
    0,1,0,0,0,1,0,0,0,1,1,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,1,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,
    0,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,1,0,0,1,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,1,0,
    1,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,1,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,1,0,0,0,1,1,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,1,0,1,0,1,0,0,0,0,1,0,0,0,0,0,
    0,0,1,0,0,0,0,0,1,1,0,0,0,1,0,0,1,0,0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0,0,0,0,1,0,0,0,0,1,1,1,0,0,0,
    1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,
    0,1,1,0,0,1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,1,1,1,1,1,0,0,0,0,0,0,1,0,0,1,0,0,0,1,
    0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,
    1,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,1,1,1,1,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,
    0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,
    0,0,0,1,0,0,0,0,1,1,0,0,0,0,0,1,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,
    0,0,1,1,1,0,0,0,1,1,1,1,1,0,0,1,1,1,1,1,0,0,0,1,1,1,0,0,0,0,0,0,1,0,0,0,0,1,1,1,0,0,0,0,1,1,1,0,
    0,0,0,1,0,0,0,0,0,0,1,1,1,0,0,0,0,1,1,0,0,0,0,0,1,1,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,
    0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,1,0,0,0,0,1,1,1,1,0,0,0,0,0,1,1,1,0,0,1,1,1,
    1,0,0,0,1,1,1,1,1,0,0,1,1,1,1,1,0,0,0,0,1,1,1,0,0,1,0,0,0,1,0,0,1,1,1,1,1,0,0,0,0,0,0,1,0,0,1,0,
    0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,0,1,0,0,0,1,0,1,0,0,0,1,
    0,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,
    0,0,1,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,1,0,0,0,0,0,0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1,0,0,0,1,0,
    0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,0,
    0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,0,0,1,0,1,0,
    1,0,0,1,0,1,0,1,0,0,1,0,0,0,1,0,0,1,0,0,1,1,0,0,1,0,0,0,1,0,0,1,1,1,1,0,0,0,1,0,0,0,0,0,0,1,0,0,
    0,1,0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0,1,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,1,1,
    0,0,0,0,0,1,0,0,0,0,0,0,1,0,1,0,1,0,0,1,0,0,1,1,0,0,1,0,0,0,1,0,0,1,0,1,0,1,0,0,1,1,1,1,1,0,0,1,
    0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,1,1,0,0,1,0,0,0,1,0,0,
    0,0,1,0,0,0,0,0,0,0,0,1,0,0,1,1,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,
    0,1,0,1,1,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,0,
    0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,
    1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,
    1,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,
    0,1,0,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,
    1,1,1,0,0,0,0,0,1,1,1,0,0,1,1,1,0,0,0,0,1,1,1,1,1,0,0,1,0,0,0,0,0,0,0,0,1,1,1,0,0,1,0,0,0,1,0,0,
    1,1,1,1,1,0,0,0,1,1,1,0,0,0,1,0,0,0,1,0,0,1,1,1,1,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,1,1,0,0,
    0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,1,1,1,1,0,0,0,0,1,1,1,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0,1,1,1,1,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,
    0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,1,1,1,1,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,
    0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,0,0,1,
    0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,0,0,1,
    0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,
    0,0,0,1,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,0,1,0,0,0,
    0,1,0,1,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,
    0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,1,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,
    0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,0,1,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,1,0,0,0,1,0,0,1,1,1,1,0,0,0,0,0,0,1,0,0,0,0,0,1,
    0,0,0,0,1,0,0,0,1,0,0,0,1,0,1,0,0,0,1,0,1,0,1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,1,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,
    0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,0,1,0,1,0,0,0,1,0,1,0,1,0,0,0,1,0,1,0,0,0,
    0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,0,0,1,0,0,
    0,0,1,0,1,0,1,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,1,1,0,0,0,1,0,0,0,1,0,0,1,1,1,1,0,0,0,0,0,1,
    0,0,0,0,0,1,1,1,0,0,0,0,0,1,0,0,0,0,1,1,0,1,1,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,1,1,1,1,0,0,0,1,
    0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,1,1,1,
    0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,
    0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,
    0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,1,0,1,1,0,0,0,0,1,1,1,1,0,0,0,1,1,
    1,1,0,0,0,1,1,1,0,0,0,1,1,1,1,1,0,0,0,1,1,1,1,0,0,1,0,1,1,0,0,0,1,1,1,0,0,0,0,0,1,1,1,0,0,0,1,0,
    0,1,0,0,0,0,0,1,0,0,0,0,1,1,1,1,0,0,0,1,0,1,1,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,
    1,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,1,1,0,0,1,0,0,
    0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,1,0,0,0,0,0,0,1,0,0,0,0,1,0,1,0,1,0,0,1,1,0,0,1,0,0,1,0,0,0,1,0,
    0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1,1,1,1,0,0,0,1,0,0,0,
    0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0,0,0,0,0,0,1,0,0,0,0,1,0,1,0,
    1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,
    0,1,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,
    1,0,0,0,0,0,0,1,0,0,0,0,1,0,1,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,
    0,0,0,1,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,
    0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,1,0,0,0,0,1,0,1,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,
    0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0,1,0,0,0,
    0,0,0,1,1,1,1,0,0,1,0,0,0,1,0,0,1,1,1,1,1,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,0,1,1,0,0,1,0,1,0,
    1,0,0,1,0,0,0,1,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,1,0,0,0,0,1,1,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,1,0,1,0,1,0,0,0,0,0,0,0,0,
    0,1,0,1,1,0,0,0,0,1,1,1,1,0,0,1,0,1,1,1,0,0,0,1,1,1,1,0,0,1,1,1,1,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,
    0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,1,1,1,1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,
    0,0,0,1,0,0,1,0,0,0,0,0,0,0,0,0,0,1,1,0,0,1,0,0,1,0,0,0,1,0,0,1,1,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,
    0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,0,1,0,1,0,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,
    1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,
    0,0,0,0,0,0,0,1,1,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,1,0,0,0,1,0,1,0,1,0,0,0,0,1,0,0,0,0,
    1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,1,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,1,0,
    0,0,1,0,1,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,1,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,
    0,0,0,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,1,0,1,0,0,0,1,0,1,0,0,0,1,0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,
    1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,1,1,0,1,0,0,1,
    0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,1,1,1,0,0,0,1,1,1,1,0,0,0,0,1,0,0,0,0,1,1,0,1,1,0,0,1,0,0,0,1,0,0,
    0,1,1,0,1,0,0,1,1,1,1,1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,
    0,1,1,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
