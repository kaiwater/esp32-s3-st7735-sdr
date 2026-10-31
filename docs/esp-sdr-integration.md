# ESP-SDR S3 接入方案

目标：在 ESP32-S3 上用 ESP-SDR 的 raw I/Q / 片内 FFT，把 256 bin 频谱直接画到
ST7735 160x128，不依赖电脑。

## 基座

- 上游：https://github.com/ESPARGOS/esp-sdr
- 参考 commit：`25fe69f946311abdaf9ad56591f25fedbc20ac98`
- License：GPL-3.0-or-later
- S3 相关代码：
  - `main/targets/esp32s3/receiver.c`
  - `main/common/spectrum.c`（上游默认不编进 S3）
  - `main/common/burst_serial.c`
  - `main/targets/esp32s3/s3_unpack.S` / `s3_fft_rnd.S`

## 1. 把 S3 的 capture 拆成“只采集”和“发送”

现在 `receiver.c` 里的 `capture()` 一边采集一边通过 `burst_send_capture()` 发给主机。
显示需要“只采集、拿 IQ 指针”，所以拆开：

```c
/* 只采集，不发送。成功时 IQ_BUFFER 里是 IQ10:
 * I 在 bits 0..9，Q 在 bits 10..19。 */
static bool acquire_iq(unsigned n, unsigned divider, unsigned *elapsed)
{
    if (divider != 0 && divider != 1 && divider != 6) return false;

    prepare_rx();
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = 0xa5a0055au;

    rx_filter_apply();
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    int64_t start = esp_timer_get_time();

    /* 与上游 capture() 相同的寄存器序列 */
    REG_WRITE(0x60033d5c, 0);
    REG_WRITE(0x60033d90, rx_pack | ((rx_pack + 1) << 6) |
                          ((rx_pack + 2) << 12) | ((rx_pack + 3) << 18) |
                          (rx_agc << 24));
    REG_WRITE(SRAM_OWNER_REG, (owner & ~15u) | 4u);

    uint32_t rate_bits = divider == 1 ? (1u << 15) : divider == 6 ? (1u << 16) : 0;
    uint32_t ctrl = 0x80000000u | rate_bits | (rx_wide << 17) |
                    (rx_flag << 16) | (rx_source << 20) | (rx_mode << 28) | n;
    REG_WRITE(0x60033d5c, ctrl);
    REG_WRITE(0x60033d5c, ctrl | (1u << 19));
    REG_WRITE(0x60033d5c, ctrl);

    while (!(REG_READ(0x60033d5c) & (1u << 18)) &&
           esp_timer_get_time() - start < 20000) {}

    bool done = (REG_READ(0x60033d5c) & (1u << 18)) != 0;
    if (elapsed) *elapsed = esp_timer_get_time() - start;

    REG_WRITE(0x60033d5c, 0);
    REG_WRITE(SRAM_OWNER_REG, owner);
    rx_filter_restore();

    if (!done) return false;
    for (unsigned j = 0; j < n; j++) {
        if (IQ_BUFFER[j] == 0xa5a0055au) return false;
    }
    return true;
}

static bool capture(unsigned n, unsigned divider, unsigned format)
{
    unsigned elapsed;
    if (!acquire_iq(n, divider, &elapsed)) {
        reply("ERR capture_timeout\n");
        return false;
    }
    return burst_send_capture(n, format, elapsed);
}
```

## 2. 给 `spectrum_snapshot()` 提供 acquire 回调

`main/common/spectrum.c` 里已有可移植的快照 FFT：

```c
typedef bool (*spectrum_acquire_fn)(unsigned n, unsigned rate,
                                    const uint32_t **words,
                                    unsigned *elapsed_us);
bool spectrum_snapshot(unsigned n, unsigned rate,
                       spectrum_acquire_fn acquire, uint8_t *bins);
```

在 `receiver.c` 里加：

```c
#include "spectrum.h"

static bool s3_spectrum_acquire(unsigned n, unsigned rate,
                                const uint32_t **words,
                                unsigned *elapsed_us)
{
    if (!acquire_iq(n, rate, elapsed_us)) return false;
    *words = IQ_BUFFER;   /* IQ10 words */
    return true;
}
```

### S3 采样率映射

`spectrum.c` 默认的 rate 表对 S3 不匹配。S3 的 divider 是：

- 0 = 80 MS/s
- 1 = 40 MS/s
- 6 = 16 MS/s

在 `spectrum.c` 的 `rates[]` 增加：

```c
#elif CONFIG_IDF_TARGET_ESP32S3
    80000000, 40000000, 0, 0, 0, 0, 16000000
```

然后：

```c
uint8_t bins[256];
spectrum_snapshot(256, 6, s3_spectrum_acquire, bins);  // 16 MS/s
```

## 3. 把 `spectrum.c` 编进 S3

上游 `main/CMakeLists.txt` 对 S3 排除了 `spectrum.c`：

```cmake
if(NOT IDF_TARGET STREQUAL "esp32s3")
    list(APPEND sources "common/spectrum.c")
endif()
```

改成 S3 也编译（或复制一份 `s3_spectrum.c`，避免影响其他 target）：

```cmake
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND sources "common/spectrum.c")
    list(APPEND dependencies esp_lcd esp_driver_spi esp_driver_gpio)
    list(APPEND sources "targets/esp32s3/st7735.c")
endif()
```

依赖里加：

```cmake
idf_component_register(... REQUIRES ... esp_lcd esp_driver_spi esp_driver_gpio)
```

`main/idf_component.yml`：

```yaml
dependencies:
  waveshare/esp_lcd_st7735: "^1.0.1"
```

## 4. ST7735 显示驱动

阶段 1 的 `main/main.c` 已经有一份可跑通的 `esp_lcd` 初始化：

- SPI2/FSPI：SCK12、MOSI11、CS10、DC9、RST8、BL7
- 160x128，RGB565
- `esp_lcd_panel_set_gap(panel, 4, 0)`
- 横屏：`swap_xy=true`，`mirror=true,false`

显示模块只需要：

```c
void st7735_draw_bins(const uint8_t *bins, unsigned n,
                      unsigned mhz, unsigned span_mhz,
                      const char *footer);
```

- 256 bin 压到 160 列；
- 上半屏画柱状频谱；
- 下半屏画瀑布（每帧下移一行）；
- 顶部/底部画频率、span、状态；
- 只做相对强度，不标 dBm。

## 5. 显示任务与 RF 互斥

`spectrum_snapshot()` 内部会调用 `acquire_iq()`，会和串口 `CAP`/`SPEC` 命令抢射频。
加一个互斥锁：

```c
static SemaphoreHandle_t rf_lock;

/* 串口 capture / ring 命令也拿这把锁 */
xSemaphoreTake(rf_lock, portMAX_DELAY);
... acquire_iq / spectrum_snapshot ...
xSemaphoreGive(rf_lock);
```

显示任务（建议 pin 到 core 0）：

```c
static void lcd_task(void *arg)
{
    uint8_t bins[256];
    for (;;) {
        if (xSemaphoreTake(rf_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (spectrum_snapshot(256, 6, s3_spectrum_acquire, bins)) {
                st7735_draw_bins(bins, 256, frequency_mhz, 16, "ESP-SDR");
            }
            xSemaphoreGive(rf_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(200));  // 5 fps
    }
}
```

- 20ms 超时避免串口大传输时显示任务卡死；
- 显示刷新 5 fps 足够；
- 串口主机占用射频时，显示可以暂停/显示 `BUSY`。

## 6. 按键与调谐

- K1：中心频率 +5 MHz
- K2：中心频率 -5 MHz
- K3：span / 模式（16 / 40 / 80 MHz，步进）
- K4：自动扫描 / 返回

调谐必须和采集在同一把锁里：

```c
frequency_mhz = new_mhz;
rx_ready = false;
prepare_rx();   // 下一次 acquire_iq 会重新 tune
```

频率范围受 PLL 限制：S3 主要可用在 2.4GHz 附近，向下约 1.85GHz；
不是 100–6000 MHz 全段都能锁。

## 7. 内存/时序预算

- 帧缓冲：160×128×2 = 40,960 B（内部 RAM，不用 PSRAM）
- `spectrum.c` 静态 FFT/窗/功率/帧缓冲约 20KB
- RF 采集 aperture 单独保留（S3 `SOC_RESERVE_MEMORY_REGION`）
- 256 点 FFT + 160 列绘图在 S3 上很轻；主要成本是 SPI 刷屏
- 27MHz SPI，全屏 40KB 约 12ms；5 fps 没问题
- 不要每帧 `malloc/free`；帧缓冲和 bins 都静态/一次性分配

## 8. 验证顺序

1. 只跑阶段 1：屏幕颜色条、方向、偏移、按键；
2. 接入 `acquire_iq`，串口打印 IQ 非 0、无 timeout；
3. 跑 `spectrum_snapshot`，串口打印 256 bin 的最大/最小；
4. 屏幕画静态 bin；
5. 接实时采集，5 fps；
6. 加调谐/按键/瀑布；
7. 对比电脑版 ESP-SDR viewer，确认频率和相对强度趋势一致。

## 9. 已知坑

- ST7735 的 offset/rotation 因面板批次不同，必须实测；
- S3 无 PSRAM，别照搬 S2 Feather 的 PSRAM 帧缓冲；
- `spectrum.c` 的 rate 表必须改，否则 16 MS/s 会被当成 4 MS/s；
- `capture()` 原函数会发送数据，显示路径必须走 `acquire_iq()`，不要重复发送；
- 显示任务和串口命令必须互斥，否则 RF 状态会乱；
- 跑 SDR 时不能同时跑 RID 扫描；
- 未公开的 raw I/Q 机制可能随 IDF/芯片版本变化，升级后要重新验证。

## 参考

- ESP-SDR：https://github.com/ESPARGOS/esp-sdr
- issue #7（板载显示）：https://github.com/ESPARGOS/esp-sdr/issues/7
- S2 TFT Feather 显示 fork：https://github.com/Kelsidavis/esp-sdr-s2-range
- `esp_lcd_st7735`：https://components.espressif.com/components/waveshare/esp_lcd_st7735
