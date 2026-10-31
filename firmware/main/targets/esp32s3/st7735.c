/* ST7735 160x128 display for the ESP-SDR S3 standalone UI.
 *
 * Internal RAM is tight once WiFi/PHY are up, so there is no full-screen
 * framebuffer. Each 8-row strip is composed on the fly into a small DMA
 * buffer, pushed to the panel, and reused after the transfer completes.
 *
 * Layout (160x128):
 *   0..17   large frequency readout + MHz + span
 *   19..24  control hint / paused status
 *   27..92  live spectrum (filled area with a bright leading edge)
 *   93      baseline
 *   94..98  frequency axis
 *   100..127 waterfall (magma palette)
 */
#include "st7735.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WIDTH 160
#define HEIGHT 128
#define STRIP_H 8
#define STRIP_PIXELS (WIDTH * STRIP_H)

#define PIN_SCK 12
#define PIN_MOSI 11
#define PIN_CS 10
#define PIN_DC 9
#define PIN_RST 8
#define PIN_BL 7

#define PIN_BTN_UP 4
#define PIN_BTN_DOWN 5
#define PIN_BTN_OK 6
#define PIN_BTN_BACK 14

/* Dark, low-glare theme: near-black background, mint spectrum, magma falls. */
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define COL_BG     RGB565(0x0A, 0x0E, 0x14)
#define COL_TEXT   RGB565(0xE8, 0xF6, 0xF3)
#define COL_ACCENT RGB565(0x4F, 0xE3, 0xD0)
#define COL_DIM    RGB565(0x5A, 0x7A, 0x80)
#define COL_GRID   RGB565(0x17, 0x31, 0x3A)
#define COL_FILL   RGB565(0x0B, 0x5E, 0x57)
#define COL_EDGE   RGB565(0x5F, 0xFF, 0xE8)

#define SPEC_TOP 27
#define SPEC_BOTTOM 92
#define AXIS_Y 94
#define WF_TOP 100
#define WF_ROWS 28

static const char *TAG = "st7735";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_strip;
static int s_strip_y;
static uint8_t s_wf[WF_ROWS][WIDTH];
static uint16_t s_heat[256];

/* 8x8 font, public domain (font8x8 / VGA-derived). Uppercase and digits. */
static const uint8_t s_font8[64][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* ! */
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* quote */
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, /* # */
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, /* $ */
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, /* % */
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, /* & */
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' */
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, /* ( */
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, /* ) */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* * */
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* , */
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* . */
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, /* / */
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, /* 0 */
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, /* 1 */
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, /* 2 */
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, /* 3 */
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, /* 4 */
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, /* 5 */
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, /* 6 */
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00}, /* 7 */
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, /* 8 */
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00}, /* 9 */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* : */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* ; */
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, /* < */
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00}, /* = */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, /* > */
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00}, /* ? */
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, /* @ */
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, /* A */
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, /* B */
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, /* C */
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, /* D */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00}, /* E */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, /* F */
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, /* G */
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, /* H */
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* I */
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, /* J */
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, /* K */
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, /* L */
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00}, /* M */
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, /* N */
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, /* O */
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, /* P */
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, /* Q */
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, /* R */
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, /* S */
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* T */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, /* U */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, /* V */
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* W */
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, /* X */
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, /* Y */
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, /* Z */
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, /* [ */
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, /* backslash */
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00}, /* ] */
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, /* ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* _ */
};


static unsigned glyph3x5(char c)
{
    switch (c) {
    case '0': return 075557; case '1': return 026227; case '2': return 071747;
    case '3': return 071717; case '4': return 055711; case '5': return 074717;
    case '6': return 074757; case '7': return 071111; case '8': return 075757;
    case '9': return 075717; case 'A': return 025755; case 'B': return 065656;
    case 'C': return 074447; case 'D': return 065556; case 'E': return 074647;
    case 'F': return 074644; case 'G': return 074557; case 'H': return 055755;
    case 'I': return 072227; case 'J': return 011157; case 'K': return 055655;
    case 'L': return 044447; case 'M': return 057555; case 'N': return 057755;
    case 'O': return 025552; case 'P': return 075744; case 'Q': return 075573;
    case 'R': return 075765; case 'S': return 074717; case 'T': return 072222;
    case 'U': return 055557; case 'V': return 055552; case 'W': return 055575;
    case 'X': return 055255; case 'Y': return 055222; case 'Z': return 071247;
    case '-': return 000700; case '.': return 000002; case '/': return 011244;
    case ':': return 002020; case '+': return 002720; case '!': return 022200;
    default: return 0;
    }
}

/* All drawing goes through the current strip; rows outside it are dropped. */
static void px(int x, int y, uint16_t color)
{
    int row = y - s_strip_y;
    if (x < 0 || x >= WIDTH || row < 0 || row >= STRIP_H) return;
    s_strip[row * WIDTH + x] = color;
}

static void rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (x + w > WIDTH) w = WIDTH - x;
    if (w <= 0) return;
    int y0 = y < s_strip_y ? s_strip_y : y;
    int y1 = y + h > s_strip_y + STRIP_H ? s_strip_y + STRIP_H : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x; xx < x + w; xx++) s_strip[(yy - s_strip_y) * WIDTH + xx] = color;
}

static void text3x5(int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; s++, x += 4 * scale) {
        unsigned bits = glyph3x5(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (bits & (1u << (14 - row * 3 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}

static void text8(int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; s++, x += 8 * scale) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 32 || c > 95) c = '?';
        const uint8_t *g = s_font8[c - 32];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (1u << col))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}

static uint16_t mix565(uint16_t a, uint16_t b, unsigned num, unsigned den)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + ((br - ar) * (int)num) / (int)den;
    int g = ag + ((bg - ag) * (int)num) / (int)den;
    int bl = ab + ((bb - ab) * (int)num) / (int)den;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

typedef struct {
    uint8_t at;
    uint16_t color;
} ramp_stop_t;

static uint16_t ramp(const ramp_stop_t *stop, int n, unsigned v)
{
    if (v <= stop[0].at) return stop[0].color;
    for (int i = 1; i < n; i++) {
        if (v <= stop[i].at) {
            unsigned span = stop[i].at - stop[i - 1].at;
            return mix565(stop[i - 1].color, stop[i].color, v - stop[i - 1].at, span);
        }
    }
    return stop[n - 1].color;
}

static void build_waterfall_palette(void)
{
    /* Magma: near-black -> deep purple -> magenta -> orange -> pale yellow. */
    const ramp_stop_t heat[] = {
        {0, COL_BG},
        {36, RGB565(0x1A, 0x0B, 0x2E)},
        {90, RGB565(0x4B, 0x1D, 0x6B)},
        {140, RGB565(0x9C, 0x2F, 0x63)},
        {190, RGB565(0xE0, 0x60, 0x2C)},
        {232, RGB565(0xF7, 0xC5, 0x48)},
        {255, RGB565(0xFF, 0xF4, 0xD6)},
    };
    for (unsigned v = 0; v < 256; v++) s_heat[v] = ramp(heat, 7, v);
}

static void clear_strip(void)
{
    uint32_t pair = ((uint32_t)COL_BG << 16) | COL_BG;
    uint32_t *p = (uint32_t *)s_strip;
    for (int i = 0; i < STRIP_PIXELS / 2; i++) p[i] = pair;
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

void st7735_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STRIP_PIXELS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 27 * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) abort();
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_color_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    ESP_ERROR_CHECK(gpio_set_level(PIN_BL, 1));

    s_strip = heap_caps_malloc(STRIP_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_strip) {
        ESP_LOGE(TAG, "no memory for strip buffer");
        abort();
    }
    memset(s_strip, 0, STRIP_PIXELS * sizeof(uint16_t));
    memset(s_wf, 0, sizeof(s_wf));
    build_waterfall_palette();

    const gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN) |
                        (1ULL << PIN_BTN_OK) | (1ULL << PIN_BTN_BACK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));
    ESP_LOGI(TAG, "ST7735 ready");
}

void st7735_draw_bins(const uint8_t *bins, unsigned n,
                      unsigned mhz, unsigned span_mhz,
                      const char *status)
{
    if (!s_panel || !s_strip || !bins || !n) return;

    uint8_t vcol[WIDTH];
    for (int x = 0; x < WIDTH; x++) {
        unsigned lo = (unsigned)x * n / WIDTH;
        unsigned hi = (unsigned)(x + 1) * n / WIDTH;
        if (hi <= lo) hi = lo + 1;
        unsigned v = 0;
        for (unsigned j = lo; j < hi && j < n; j++) {
            unsigned code = bins[j];
            if (code > v) v = code;
        }
        vcol[x] = (uint8_t)v;
        s_wf[WF_ROWS - 1][x] = (uint8_t)v;
    }
    memmove(&s_wf[0][0], &s_wf[1][0], (WF_ROWS - 1) * WIDTH);

    char freq[12], span[16], left[12], right[12];
    snprintf(freq, sizeof(freq), "%u", mhz);
    snprintf(span, sizeof(span), "SPAN %u", span_mhz);
    int lo_mhz = (int)mhz - (int)span_mhz / 2;
    if (lo_mhz < 0) lo_mhz = 0;
    snprintf(left, sizeof(left), "%d", lo_mhz);
    snprintf(right, sizeof(right), "%d", (int)mhz + (int)span_mhz / 2);

    for (int y0 = 0; y0 < HEIGHT; y0 += STRIP_H) {
        s_strip_y = y0;
        clear_strip();

        if (y0 < SPEC_TOP) {
            text8(4, 1, freq, 2, COL_TEXT);
            text8(70, 8, "MHz", 1, COL_ACCENT);
            text8(100, 3, span, 1, COL_ACCENT);
        }

        if (status && y0 + STRIP_H > 19 && y0 <= 19 + 5)
            text3x5(4, 19, status, 1, COL_DIM);

        if (y0 < SPEC_BOTTOM + 1 && y0 + STRIP_H > SPEC_TOP) {
            const int range = SPEC_BOTTOM - SPEC_TOP;
            for (int x = 0; x < WIDTH; x++) {
                int h = (int)vcol[x] * range / 255;
                int ytop = SPEC_BOTTOM - h + 1;
                if (h > 0) {
                    for (int y = ytop; y <= SPEC_BOTTOM; y++) px(x, y, COL_FILL);
                    px(x, ytop, COL_EDGE);
                    if (ytop + 1 <= SPEC_BOTTOM) px(x, ytop + 1, COL_EDGE);
                }
            }
            for (int gx = 40; gx < WIDTH; gx += 40)
                for (int y = SPEC_TOP; y <= SPEC_BOTTOM; y++) px(gx, y, COL_GRID);
        }

        if (y0 + STRIP_H > SPEC_BOTTOM + 1 && y0 <= SPEC_BOTTOM + 1)
            for (int x = 0; x < WIDTH; x++) px(x, SPEC_BOTTOM + 1, COL_GRID);

        if (y0 + STRIP_H > AXIS_Y && y0 <= AXIS_Y + 5) {
            text3x5(2, AXIS_Y, left, 1, COL_DIM);
            text3x5(72, AXIS_Y, freq, 1, COL_DIM);
            text3x5(158 - (int)strlen(right) * 4, AXIS_Y, right, 1, COL_DIM);
        }

        if (y0 + STRIP_H > WF_TOP) {
            for (int row = 0; row < WF_ROWS; row++) {
                int y = WF_TOP + row;
                if (y < y0 || y >= y0 + STRIP_H) continue;
                for (int x = 0; x < WIDTH; x++) px(x, y, s_heat[s_wf[row][x]]);
            }
        }

        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, y0, WIDTH, y0 + STRIP_H, s_strip));
        if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
    }
}

int st7735_button(void)
{
    static uint8_t stable;
    static uint32_t last;
    static uint32_t down_at[4];
    static bool was_down[4];

    uint8_t raw = 0;
    if (gpio_get_level(PIN_BTN_UP) == 0) raw |= 1;
    if (gpio_get_level(PIN_BTN_DOWN) == 0) raw |= 2;
    if (gpio_get_level(PIN_BTN_OK) == 0) raw |= 4;
    if (gpio_get_level(PIN_BTN_BACK) == 0) raw |= 8;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - last < 25) return 0;
    last = now;
    stable = raw;

    for (int i = 0; i < 4; i++) {
        bool down = (stable >> i) & 1;
        if (down && !was_down[i]) down_at[i] = now;
        if (!down && was_down[i]) {
            was_down[i] = false;
            if (now - down_at[i] >= 30) return i + 1;
        }
        was_down[i] = down;
    }
    return 0;
}
