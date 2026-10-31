# ESP32-S3 + ST7735 手持 SDR

把 ESP32-S3 的 WiFi 射频变成一台**不依赖电脑**的手持 SDR：
原始 I/Q / 片内 FFT → ST7735 160x128 频谱 + 瀑布。

> 这是一个独立项目，和 RID 扫描器分开。跑 SDR 时不能同时做 Remote ID 扫描，
> 因为两者都要占用同一个 WiFi 射频。

## 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | ST7735 160x128 + 4 按键 bring-up | 代码已写，待实机验证 |
| 2 | ESP-SDR S3 接入（raw IQ / FFT） | 设计完成，待实现 |
| 3 | 频谱 + 瀑布 + 调谐 UI | 待实现 |
| 4 | 电池/外壳/长期稳定性 | 未开始 |

**目前不是一台能直接用的 SDR。** 这个仓库先解决屏幕、按键、
硬件接线和 ESP-SDR 的接入方案；后续按 `docs/esp-sdr-integration.md` 推进。

## 为什么不用电脑

ESP-SDR 上游把 I/Q 或频谱通过 USB/UART 送到电脑，用浏览器看。
ESP32-S3 本身支持片内 FFT（256~2048 bin），所以可以把 FFT 结果直接画到
ST7735 上，不需要电脑。

上游没有板载屏支持，但这不是硬件限制：

- 上游 issue #7 就是在要“ESP32 自己显示瀑布图”：
  https://github.com/ESPARGOS/esp-sdr/issues/7
- 社区已经有人在 **ESP32-S2 TFT Feather + ST7789** 上做出来了：
  https://github.com/Kelsidavis/esp-sdr-s2-range
- S3 比 S2 强，ST7735 只是普通 SPI 屏，完全带得动。

## 支持的硬件

- **主控**：ESP32-S3-DevKitC-1 公版，双 USB-C，16MB flash，无 PSRAM
- **屏幕**：ST7735S 1.8" 128x160 SPI，8 针，3.3V
- **按键**：4 个独立轻触按键，K1~K4
- **供电**：USB 5V；屏幕逻辑 3.3V
- 没有 PSRAM 也能做：帧缓冲 160x128x16 = 40KB，使用内部 RAM

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
- 本机面板偏移：`x=4, y=0`；换屏后可能要调整。
- 按键一端接 GPIO、一端接 GND，内部上拉。
- 详细说明见 [`docs/hardware.md`](docs/hardware.md)。

## 软件架构

```
ESP32-S3 WiFi RX
   ↓ 未公开 raw I/Q bypass（ESP-SDR）
IQ10 samples (I[9:0], Q[19:10])
   ↓ spectrum_snapshot() / 片内 FFT
256 bins (8-bit relative power)
   ↓
ST7735 160x128：频谱条 + 瀑布 + 频率/span/状态
```

- 基座：ESP-SDR（GPL-3.0），S3 支持 raw IQ 和片内 FFT
- 显示：ST7735，SPI2/FSPI，160x128，16 位色
- 按键：K1/K2 调中心频率，K3 切 span/模式，K4 返回/扫描
- 不依赖电脑；可选保留 UART 日志

## 构建

这是 ESP-IDF 项目，不是 Arduino/PlatformIO：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

阶段 1 的显示 demo 在 `main/`，使用 Espressif Component Registry 的
`waveshare/esp_lcd_st7735`。首次构建会自动拉取组件。

完整 SDR 接入步骤见 [`docs/esp-sdr-integration.md`](docs/esp-sdr-integration.md)。

## 已知限制

- 上游 ESP-SDR 的 raw I/Q 机制是**未公开**的，部分代码由 AI 生成；
- S3 只能覆盖 2.4GHz 附近；5GHz 需要 ESP32-C5；
- 快照之间有 gaps；continuous 模式也只分析一部分信号；
- 增益/功率未校准，显示的是相对强度，不是校准 dBm；
- 跑 SDR 时不能同时做 RID 扫描；
- 没有 PSRAM，瀑布/帧缓冲要省着用；
- 本项目是 GPL-3.0，和 Apache-2.0 的 RID 扫描器分开维护。

## 参考

- ESP-SDR：https://github.com/ESPARGOS/esp-sdr
- 项目介绍：https://espargos.net/espsdr/
- 板载显示 issue：https://github.com/ESPARGOS/esp-sdr/issues/7
- S2 TFT Feather 显示 fork：https://github.com/Kelsidavis/esp-sdr-s2-range
- esp_lcd_st7735 组件：https://components.espressif.com/components/waveshare/esp_lcd_st7735
- 上游 commit 参考：`25fe69f946311abdaf9ad56591f25fedbc20ac98`

## License

GPL-3.0-or-later。基于 ESP-SDR 的设计与代码，见 `LICENSE` 和 `NOTICE`。

## Changelog

版本与阶段记录见 [`CHANGELOG.md`](CHANGELOG.md) 和 [`changelogs/`](changelogs/)。
