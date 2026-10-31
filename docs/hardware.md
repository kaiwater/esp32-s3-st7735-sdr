# 硬件与接线

## 已验证/目标硬件

- ESP32-S3-DevKitC-1 公版，双 USB-C、BOOT/RST、板载天线，16MB flash，无 PSRAM
- ST7735S 1.8" 128x160 SPI 屏，8 针
- 4 个独立轻触按键
- USB 5V 供电

## 屏幕

| ST7735S | ESP32-S3 | J1 脚位 |
|---|---|---|
| GND | GND | J1-22 |
| VCC | 3V3 | J1-1 |
| SCL / SCK / CLK | GPIO12 | J1-18 |
| SDA / MOSI | GPIO11 | J1-17 |
| CS | GPIO10 | J1-16 |
| DC / A0 | GPIO9 | J1-15 |
| RES / RST | GPIO8 | J1-12 |
| BLK / LED | GPIO7 | J1-7 |

- 屏幕的 `SDA` 是 SPI MOSI，不是 I2C。
- VCC 用 3.3V，不要接 5V。
- BLK 可以接 GPIO7 做开关/PWM；先验证时也可以直接接 3V3。
- 本机面板在 160x128 横屏下需要偏移 `x=4, y=0`。
- 如果换屏后图像偏移/裁切，优先调 `set_gap` 和 rotation/mirror。

## 按键

| 按键 | 功能 | ESP32-S3 | J1 脚位 |
|---|---|---|---|
| K1 | UP / 频率 + | GPIO4 | J1-4 |
| K2 | DOWN / 频率 - | GPIO5 | J1-5 |
| K3 | OK / 模式 | GPIO6 | J1-6 |
| K4 | BACK / 扫描 | GPIO14 | J1-20 |
| 公共端 | GND | GND | J1-22 |

按键一端接 GPIO，一端接 GND；GPIO 开内部上拉，按下为低。

## 电源与地

- 屏幕 GND、按键公共端、ESP32 GND 必须共地。
- 屏幕和逻辑都用 3.3V；USB 5V 只给开发板。
- 上电前用万用表确认 3V3 和 GND 不短路。
- 背光电流由屏幕模块决定；如果 USB 供电不稳，先断开 BLK 排查。

## 不要用的引脚

- GPIO0 / 3 / 45 / 46：strapping
- GPIO19 / 20：USB
- GPIO26~37：flash/PSRAM（视模组而定）

本项目使用 GPIO4/5/6/14（按键）和 GPIO7~12（屏幕），与 ESP-SDR 的
UART0 GPIO43/44、原生 USB GPIO19/20 不冲突。

## 屏幕方向

阶段 1 的 demo 默认横屏 160x128：

- `esp_lcd_panel_swap_xy(panel, true)`
- `esp_lcd_panel_mirror(panel, true, false)`
- `esp_lcd_panel_set_gap(panel, 4, 0)`

如果方向不对，调整 `swap_xy` / `mirror` 的组合；如果图像有 1~2 像素偏移，
调 `set_gap`。
