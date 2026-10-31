# Changelog

## v0.2.1 — 2026-10-31

修复宽 span 下频谱异常：

- 显示前把自然 FFT 顺序的 bins 旋转 `n/2`，让 DC 落在屏幕中心
- 在时域先减 I/Q 均值，去掉 DC/LO 泄漏，避免 Hann 窗把中间抬高
- 新增长按：K1/K2 加速调谐，K3 跳最高/最低，K4 回 2412 MHz

详细记录：[`changelogs/2026-10-31-v0.2.1.md`](changelogs/2026-10-31-v0.2.1.md)

## v0.2.0 — 2026-10-31

阶段 2/3 完成：ESP-SDR S3 接入、实时频谱/瀑布、调谐 UI，全部实机验证。

- `firmware/`：完整 ESP-SDR fork，内置 esp-dsp 和 esp_lcd_st7735
- `acquire_iq()` + `spectrum_snapshot()`：256-bin 片内 FFT
- ST7735 条带渲染（2.5KB），解决 40KB 帧缓冲分配失败
- 上电先按住屏幕，RF 稳定后再初始化，避免白屏/花屏
- 解决 `S3 RF ring overlaps BSS` 链接错误
- IDF 5.4.2 兼容 shim
- 新 UI：CRT 荧光绿、大号频率、实心频谱、单色绿瀑布
- K1/K2 调谐、K3 span、K4 暂停
- `bringup/`：阶段 1 demo 独立子目录

详细记录：[`changelogs/2026-10-31-v0.2.0.md`](changelogs/2026-10-31-v0.2.0.md)

## v0.1.0 — 2026-10-31

项目立项与阶段 1 脚手架。

- ESP32-S3 + ST7735 128x160 + 4 按键硬件方案
- ESP-IDF 显示 bring-up demo（颜色条/渐变/移动块）
- `esp_lcd_st7735` 组件接入
- ESP-SDR S3 raw I/Q / 片内 FFT 接入方案文档
- 明确限制：这不是完整 SDR，阶段 2 才接频谱

详细记录：[`changelogs/2026-10-31-v0.1.0.md`](changelogs/2026-10-31-v0.1.0.md)
