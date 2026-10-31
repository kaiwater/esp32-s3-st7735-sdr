# bringup — ST7735 + 4 按键验证

阶段 1 的显示/按键 bring-up demo，用来确认接线、屏幕方向和按键。

```bash
cd bringup
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

屏幕应显示颜色条、渐变、边框和按键状态：

- 160x128 横屏，无偏移、无变形
- K1~K4 对应屏幕上的按键提示
- 本机 `esp_lcd` 使用 `set_gap(0, 0)`

完整的 SDR 固件在 [`../firmware`](../firmware)。
