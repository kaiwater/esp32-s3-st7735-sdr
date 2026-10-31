# ESP-SDR S3 独立显示实现记录

目标：在 ESP32-S3 上用 ESP-SDR 的 raw I/Q + 片内 FFT，把 256-bin 频谱直接画到
ST7735 160x128，不依赖电脑。

本文记录**已经实现并实机验证**的方案，不再是待办设计稿。

## 基座

- 上游：<https://github.com/ESPARGOS/esp-sdr>
- 基线 commit：`9b61e7465bee6da15209c5c09308043d08c46a1d`
- License：GPL-3.0-or-later
- 固件目录：[`../firmware`](../firmware)
- 已验证 IDF：**v5.4.2**

## 改动的文件

| 文件 | 作用 |
|---|---|
| `main/targets/esp32s3/st7735.c/.h` | ST7735 初始化、条带渲染、UI、按键 |
| `main/targets/esp32s3/receiver.c` | `acquire_iq()`、`s3_spectrum_acquire()`、`lcd_task()`、调谐 |
| `main/common/spectrum.c/.h` | `spectrum_snapshot()`、S3 采样率、S3 `MAX_FFT=256` |
| `main/common/ring_capture.c` | IDF 5.4 兼容 shim；S3 大块缓冲改堆分配 |
| `main/CMakeLists.txt` | 加入 `spectrum.c` / `st7735.c` 和显示依赖 |
| `components/esp_lcd_st7735/` | 内置 Waveshare ST7735 组件（MIT） |
| `components/esp-dsp/` | 内置 esp-dsp（Apache-2.0），不用子模块 |

## 1. 只采集、不发送

上游 `receiver.c` 的 `capture()` 会一边采集一边通过 `burst_send_capture()` 发给主机。
显示路径需要“只采集、拿 IQ 指针”，因此新增 `acquire_iq()`：

- 与 `capture()` 相同的寄存器序列；
- 成功后 `IQ_BUFFER` 里是 IQ10：`I` 在 bits 0..9，`Q` 在 bits 10..19；
- 用 `0xa5a0055a` 哨兵检测采集超时；
- 用 `s_iq_lock` 互斥锁保护 `prepare_rx()` / 调谐 / 采集。

`capture()` 保留原样给串口主机使用。

## 2. 片内 FFT 快照

`main/common/spectrum.c` 增加：

```c
bool spectrum_snapshot(unsigned n, unsigned rate,
                       spectrum_acquire_fn acquire, uint8_t *bins);
```

流程：

1. 调用 `acquire(n, rate, &words, &elapsed)`；
2. 解包 IQ10，乘 Hann 窗；
3. 调用片内 Q15 FFT；
4. 去 DC；
5. 输出自然频率顺序的 8-bit 相对功率，共 256 bin。

S3 的采样率映射（`spectrum.c` 的 `rates[]`）：

| rate code | 采样率 | UI span |
|---|---|---|
| 0 | 80 MS/s | 80 MHz |
| 1 | 40 MS/s | 40 MHz |
| 6 | 16 MS/s | 16 MHz |

S3 使用 `MAX_FFT = 256`，只保留显示需要的分辨率，减少 BSS。

## 3. 内存：RF ring 与 BSS 的冲突

S3 的 RF ring 固定在：

```
RING_BANK_BASE = 0x3fcb0000
RING_BANK_END  = 0x3fce0000
```

`sram_guard.ld` 要求 `_bss_end <= 0x3fcb0000`。直接把显示和 spectrum 编进来后，
链接报错：

```
ld: S3 RF ring overlaps BSS
```

处理：

- `ring_capture.c` 的 `txq`、`win2`、`fft_buf`、`bin_of` 在 S3 上改成堆分配；
- 原有 twiddles 本来就在堆上；
- `spectrum.c` 在 S3 上 `MAX_FFT=256`；
- 最终 `_bss_end ≈ 0x3fca7380`，距离 ring 还有约 36KB 余量。

## 4. 显示：为什么不用 40KB 全屏帧缓冲

WiFi/PHY 启动后实测：

- DMA-capable 内部内存约 24KB；
- 最大连续块约 15KB；
- 40,960B 全屏 RGB565 帧缓冲分配失败，`abort()` 导致重启。

因此 `st7735.c` 改成 **8 行条带渲染**：

- 条带缓冲 `160 × 8 × 2 = 2,560B`；
- 每帧 16 条，逐条 compose → `esp_lcd_panel_draw_bitmap()` → 等 DMA 完成；
- 用 `on_color_trans_done` 回调和二值信号量复用同一条带；
- 全屏刷新约 12ms（27MHz SPI），5fps 界面没有压力。

`st7735_draw_bins()` 的签名保持不变：

```c
void st7735_draw_bins(const uint8_t *bins, unsigned n,
                      unsigned mhz, unsigned span_mhz,
                      const char *status);
```

## 5. UI 布局与配色

参考 S2 TFT Feather 的机载显示：

```
y=0..17    大号中心频率 + MHz + SPAN
y=19..24   操作提示 / PAUSED
y=27..92   实心频谱 + 亮边 + 3 条竖向刻度
y=93       基线
y=94..98   左/中/右频率刻度
y=100..127 28 行瀑布
```

配色：

- 背景：近黑 `#0A0E14`
- 文字：暖白 `#E8F6F3`
- 强调：薄荷青 `#4FE3D0`
- 频谱填充：深青 `#0B5E57`，顶边 `#5FFFE8`
- 瀑布：magma 色带（黑紫 → 品红 → 橙 → 淡黄）

大号频率使用 8x8 像素字体（public domain，font8x8 / VGA 派生），
小字继续用 3x5 字体。

## 6. 按键与调谐

`lcd_task()` pin 在 core 0，周期性执行：

- **K1/K2**：中心频率 ±5 MHz，限制在 `RX_FREQ_MIN..RX_FREQ_MAX`；
- **K3**：span 16 → 40 → 80 MHz 循环；
- **K4**：暂停/继续；暂停时保留最后一帧并显示 `PAUSED  K4 RESUME`；
- 未暂停时每 200ms 取一次 `spectrum_snapshot(256, rate_code, ...)`。

调谐和采集都在 `s_iq_lock` 内：

```c
frequency_mhz = new_mhz;
rx_ready = false;
prepare_rx();     /* 下一次 acquire_iq 重新 tune */
```

注意：固件允许输入 100–6000 MHz，但 S3 的 WiFi PLL 实际主要覆盖
**约 1.85–2.5GHz**；超出这个范围可能没有信号或失锁。spans/频率显示的是
请求值，不代表芯片一定锁得住。

## 7. IDF 5.4 兼容

上游使用较新的 IDF API：

```c
cpu_utility_ll_enable_clock_and_reset_app_cpu();
```

IDF 5.4.2 没有这个合并函数，`ring_capture.c` 加了 shim：

```c
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
static inline void cpu_utility_ll_enable_clock_and_reset_app_cpu(void)
{
    cpu_utility_ll_reset_cpu(1);
}
#endif
```

## 8. 并发限制

- 显示任务和串口 `CAP` / `SPEC` / `IQS` 都会操作同一套 RF 寄存器；
- 显示路径拿了 `s_iq_lock`，但上游串口 `capture()` / `ring_capture_run()`
  仍然直接操作射频；
- **不要同时用电脑串口抓 IQ 和让屏幕刷新**；独立手持使用没问题。

如果以后要支持“边看屏幕边推流”，需要把串口路径也纳入同一把锁，
并接受刷屏期间主机传输被阻塞。

## 9. 构建与验证

```bash
cd firmware
idf.py -B build-s3 -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-s3/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 build
idf.py -B build-s3 -p /dev/cu.usbserial-XXXX flash
```

实机验证项：

1. 屏幕无偏移/变形；
2. 频谱连续刷新（约 5fps），瀑布向下滚动；
3. K1/K2 以 5MHz 改变中心频率；
4. K3 在 16/40/80 MHz 间切换，刻度随之变化；
5. K4 暂停后画面保持，再按恢复；
6. 连续运行无重启/panic。

## 10. 已知坑

- ST7735 的 offset/rotation 因面板批次不同，必须实测；本机 `set_gap(0,0)`；
- S3 无 PSRAM，不能照搬 S2 Feather 的 40KB 帧缓冲；
- `spectrum.c` 的 `rates[]` 必须包含 S3，否则 16 MS/s 会被当成 4 MS/s；
- 显示路径不要调用 `capture()`，否则会重复向主机发送数据；
- 不要把大数组放回 BSS，否则会再次触发 RF ring 重叠；
- 跑 SDR 时不能同时跑 RID 扫描；
- 未公开的 raw I/Q 机制可能随 IDF/芯片版本变化，升级后要重新验证。

## 参考

- ESP-SDR：<https://github.com/ESPARGOS/esp-sdr>
- issue #7（板载显示）：<https://github.com/ESPARGOS/esp-sdr/issues/7>
- S2 TFT Feather 显示 fork：<https://github.com/Kelsidavis/esp-sdr-s2-range>
- `esp_lcd_st7735`：<https://components.espressif.com/components/waveshare/esp_lcd_st7735>
- font8x8：<https://github.com/dhepper/font8x8>
