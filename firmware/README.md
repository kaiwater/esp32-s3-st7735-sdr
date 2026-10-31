# firmware — ESP-SDR + ST7735 独立显示

这是 ESP-SDR 的 S3 固件分支，额外接入了 ST7735 160x128 屏幕和 4 个按键，
让设备不接电脑也能显示实时频谱/瀑布。

- 上游：<https://github.com/ESPARGOS/esp-sdr>
- 基线 commit：`9b61e7465bee6da15209c5c09308043d08c46a1d`
- 上游原始 README：[`UPSTREAM-README.md`](UPSTREAM-README.md)
- License：GPL-3.0-or-later（见 [`LICENSE`](LICENSE)）

## 本分支的改动

- `main/targets/esp32s3/st7735.c/.h`：ST7735 驱动、条带渲染、UI、按键
- `main/targets/esp32s3/receiver.c`：`acquire_iq()`、`s3_spectrum_acquire()`、`lcd_task()`
- `main/common/spectrum.c/.h`：`spectrum_snapshot()`、S3 采样率映射、S3 `MAX_FFT=256`
- `main/common/ring_capture.c`：IDF 5.4 兼容 shim；S3 大块缓冲改堆分配
- `main/CMakeLists.txt`：加入显示源码和 `esp_lcd` / `esp_dsp` 依赖
- `components/esp_lcd_st7735/`：内置 Waveshare ST7735 组件（MIT）
- `components/esp-dsp/`：内置 esp-dsp（Apache-2.0），不依赖子模块/联网

## 构建

```bash
cd firmware
idf.py -B build-s3 -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-s3/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 build
```

烧录：

```bash
idf.py -B build-s3 -p /dev/cu.usbserial-XXXX flash
```

已验证环境：ESP-IDF **v5.4.2**。组件管理器已关闭，所有依赖都在
`components/` 内。

## 操作

| 按键 | 功能 |
|---|---|
| K1 | 中心频率 +5 MHz |
| K2 | 中心频率 −5 MHz |
| K3 | span 16 / 40 / 80 MHz |
| K4 | 暂停 / 继续 |

## UI

- 顶部：大号中心频率 + `MHz` + `SPAN`
- 中部：256-bin 实心频谱（深青填充 + 亮边），3 条竖向刻度
- 底部：magma 色带瀑布（28 行）
- 暂停时状态行显示 `PAUSED  K4 RESUME`

实现细节见根目录 [`docs/esp-sdr-integration.md`](../docs/esp-sdr-integration.md)。
