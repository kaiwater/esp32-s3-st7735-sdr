#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_H_RES 160
#define LCD_V_RES 128
#define LCD_PIXEL_CLK_HZ (27 * 1000 * 1000)

#define PIN_LCD_SCK 12
#define PIN_LCD_MOSI 11
#define PIN_LCD_CS 10
#define PIN_LCD_DC 9
#define PIN_LCD_RST 8
#define PIN_LCD_BL 7

#define PIN_BTN_UP 4
#define PIN_BTN_DOWN 5
#define PIN_BTN_OK 6
#define PIN_BTN_BACK 14

static const char *TAG = "st7735-demo";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static int s_mode;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = s_fb + yy * LCD_H_RES + x;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
}

static void draw_demo(int tick)
{
    const uint16_t bars[] = {
        0xF800, 0xFD20, 0xFFE0, 0x07E0,
        0x07FF, 0x001F, 0xF81F, 0xFFFF,
    };

    memset(s_fb, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));

    if (s_mode == 0) {
        for (int i = 0; i < 8; i++) {
            fill_rect(i * 20, 20, 20, 88, bars[i]);
        }
        fill_rect(0, 0, 160, 2, 0xFFFF);
        fill_rect(0, 126, 160, 2, 0xFFFF);
    } else if (s_mode == 1) {
        for (int y = 0; y < LCD_V_RES; y++) {
            for (int x = 0; x < LCD_H_RES; x++) {
                s_fb[y * LCD_H_RES + x] = rgb565(x * 255 / 159, y * 255 / 127, 64);
            }
        }
    } else {
        int x = (tick * 4) % (LCD_H_RES - 24);
        fill_rect(x, 48, 24, 32, 0x07FF);
        fill_rect(0, 0, 160, 2, 0xFFFF);
        fill_rect(0, 126, 160, 2, 0xFFFF);
    }
}

static void buttons_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN) |
                        (1ULL << PIN_BTN_OK) | (1ULL << PIN_BTN_BACK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static int read_buttons(void)
{
    int mask = 0;
    if (gpio_get_level(PIN_BTN_UP) == 0) mask |= 1;
    if (gpio_get_level(PIN_BTN_DOWN) == 0) mask |= 2;
    if (gpio_get_level(PIN_BTN_OK) == 0) mask |= 4;
    if (gpio_get_level(PIN_BTN_BACK) == 0) mask |= 8;
    return mask;
}

static void lcd_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_cfg, &io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 4, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_BL, 1));

    /* 160x128 RGB565 帧缓冲，40KB，放内部 DMA 内存。 */
    s_fb = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(s_fb ? ESP_OK : ESP_ERR_NO_MEM);
    memset(s_fb, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
}

void app_main(void)
{
    ESP_LOGI(TAG, "ST7735 display bring-up");
    buttons_init();
    lcd_init();

    int tick = 0;
    int last_buttons = 0;
    for (;;) {
        int buttons = read_buttons();
        int pressed = buttons & ~last_buttons;
        last_buttons = buttons;

        if (pressed & 4) {           // K3: next demo mode
            s_mode = (s_mode + 1) % 3;
            ESP_LOGI(TAG, "mode %d", s_mode);
        }
        if (pressed & 8) {           // K4: back to color bars
            s_mode = 0;
        }

        draw_demo(tick++);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                                  LCD_H_RES, LCD_V_RES, s_fb));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
