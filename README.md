# ESP32-S3 + ST7735 手持 SDR

把 ESP32-S3 的 WiFi 射频变成一台**不依赖电脑**的手持 SDR：
原始 I/Q → 片内 FFT → ST7735 160x128 实时频谱 + 瀑布。

> 这是一个独立项目，和 RID 扫描器分开。跑 SDR 时不能同时做 Remote ID 扫描，
> 因为两者都要占用同一个 WiFi 射频。

## 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | ST7735 160x128 + 4 按键 bring-up | 已实机验证：屏幕无偏移/变形，K1~K4 全部正常 |
| 2 | ESP-SDR S3 接入（raw IQ / 片内 FFT） | 已实机验证：256-bin 频谱连续刷新 |
| 3 | 频谱 + 瀑布 + 调谐 UI | 已实机验证：调谐、span、暂停均正常 |
| 4 | 电池 / 外壳 / 长期稳定性 | 未开始 |

**现在是一台能独立工作的 SDR**，不接电脑即可显示实时频谱和瀑布。
阶段 4（电池、外壳、长时间稳定性）还没做。

## 功能与操作

- 默认中心频率 **2412 MHz**，UI 范围 **1850–2500 MHz**
- **短按 K1 / K2**：中心频率 ±5 MHz
- **长按 K1 / K2**：加速调谐，步进 5 → 10 → 25 → 50 → 100 MHz
- **短按 K3**：切换 span **16 / 40 / 80 MHz**（对应 16 / 40 / 80 MS/s）
- **长按 K3**：在 **2500 MHz（最高）** 和 **1850 MHz（最低）** 之间跳转
- **短按 K4**：暂停 / 继续；暂停时状态行显示 `PAUSED  K4 RESUME`
- **长按 K4**：回到初始频率 **2412 MHz** 并恢复刷新
- 256-bin 片内 FFT，160x128 屏，28 行瀑布
- CRT 荧光绿 UI：纯黑背景、大号频率、实心频谱 + 余辉、单色绿瀑布、示波器刻度

## 目录

```
firmware/   完整固件：ESP-SDR fork + S3 独立显示 UI
bringup/    阶段 1 的屏幕/按键 bring-up demo（可单独编译）
docs/       硬件接线、固件实现说明
changelogs/ 版本记录
```

`firmware/` 里已经包含：

- ESP-SDR（S3 raw I/Q / 片内 FFT）源码
- 内置 `esp-dsp`（无需联网拉取）
- 内置 `waveshare/esp_lcd_st7735` 组件

## 构建固件

需要 **ESP-IDF v5.4.2**（更新版本可能也能编，但只验证了 5.4.2）：

```bash
cd firmware
idf.py -B build-s3 -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-s3/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 build
idf.py -B build-s3 -p /dev/cu.usbserial-XXXX flash
```

说明：

- 组件管理器在上游固件里是关闭的，依赖全部在 `firmware/components/` 内，编译不需要联网。
- 应用日志默认走 UART0（2 Mbaud），不是 USB-Serial-JTAG；`idf.py monitor`
  默认连 USB 口时可能看不到应用日志，但不影响屏幕和射频。
- 如果换 ESP-IDF 版本后编译报错，先看 `firmware/docs/` 和根目录
  [`docs/esp-sdr-integration.md`](docs/esp-sdr-integration.md) 里的兼容性说明。

## 构建阶段 1 demo

```bash
cd bringup
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

这个 demo 只画颜色条/渐变/按键状态，用来验证屏幕方向和接线。
首次构建会从 Espressif Component Registry 拉取 `waveshare/esp_lcd_st7735`。

## 支持的硬件

- **主控**：ESP32-S3-DevKitC-1 公版，双 USB-C，16MB flash，无 PSRAM
- **屏幕**：ST7735S 1.8" 128x160 SPI，8 针，3.3V
- **按键**：4 个独立轻触按键，K1~K4
- **供电**：USB 5V；屏幕逻辑 3.3V

### 接线

| ST7735S | ESP32-S3 | J1 脚位 |
|---|---|---|
| GND | GND | J1-22 |
| VCC | 3V3 | J1-1 |
| SCL/SCK | GPIO12 | J1-18 |
| SDA/MOSI | GPIO11 | J1-17 |
| CS | GPIO10 | J1-16 |
| DC/A0 | GPIO9 | J1-15 |
| RES/RST | GPIO8 | J1-12 |
| BLK | GPIO7（或 3V3） | J1-7 |

| 按键 | ESP32-S3 | J1 脚位 |
|---|---|---|
| K1 / UP | GPIO4 | J1-4 |
| K2 / DOWN | GPIO5 | J1-5 |
| K3 / OK | GPIO6 | J1-6 |
| K4 / BACK | GPIO14 | J1-20 |
| 公共端 | GND | J1-22 |

- 屏幕的 `SDA` 是 SPI MOSI，不是 I2C。
- 本机在 ESP-IDF `esp_lcd` 下使用 `set_gap(0, 0)`；不要套用 LovyanGFX 的 `(4,0)`。
- 按键一端接 GPIO、一端接 GND，内部上拉。
- 详细说明见 [`docs/hardware.md`](docs/hardware.md)。

## 软件架构

```
ESP32-S3 WiFi RX
   ↓ 未公开 raw I/Q bypass（ESP-SDR）
IQ10 samples (I[9:0], Q[19:10])
   ↓ spectrum_snapshot() / 片内 256-bin FFT
256 bins (8-bit relative power)
   ↓ 条带渲染（8 行，2.5KB DMA 缓冲）
ST7735 160x128：大号频率 + 频谱 + 瀑布
```

- 基座：ESP-SDR（GPL-3.0），S3 支持 raw IQ 和片内 FFT
- 显示：ST7735，SPI2/FSPI，160x128，16 位色
- 按键：K1/K2 调中心频率，K3 切 span，K4 暂停
- 不依赖电脑；保留 UART0 调试/命令通道

### 为什么不用 40KB 全屏帧缓冲

WiFi/PHY 启动后，S3 内部 DMA 内存只剩约 24KB，最大连续块约 15KB，
40KB 全屏缓冲会直接分配失败。固件改成 **8 行条带缓冲（160x8x2 = 2.5KB）**，
每帧分 16 次刷屏；同时把 ESP-SDR ring 的大块 BSS 改成堆分配，
避免 RF ring（`0x3fcb0000`）和 BSS 重叠导致链接失败。

## 已知限制

- 上游 ESP-SDR 的 raw I/Q 机制是**未公开**的，部分代码由 AI 生成；
- S3 只能覆盖 2.4GHz 附近；5GHz 需要 ESP32-C5；
- 快照之间有 gaps；显示的是相对强度，不是校准 dBm；
- 没有 PSRAM：瀑布 28 行，频谱 256 bin；
- 80 MHz span 最外侧受 S3 RX 滤波器（约 69MHz）限制会有滚降；
- 跑 SDR 时不能同时做 RID 扫描；
- 长时间稳定性、电池和外壳还没验证；
- 本项目是 GPL-3.0，和 Apache-2.0 的 RID 扫描器分开维护。

## 参考

- ESP-SDR：https://github.com/ESPARGOS/esp-sdr
- 项目介绍：https://espargos.net/espsdr/
- 板载显示 issue：https://github.com/ESPARGOS/esp-sdr/issues/7
- S2 TFT Feather 显示 fork：https://github.com/Kelsidavis/esp-sdr-s2-range
- esp_lcd_st7735 组件：https://components.espressif.com/components/waveshare/esp_lcd_st7735
- 8x8 像素字体：font8x8（public domain）
