# ESP32 LDAC/aptX 功放 — ST7735 + 4 按键版本

这是根目录 SSD1306 版本的独立硬件变体。蓝牙 LDAC、aptX HD、aptX Classic、AAC、SBC 解码、PCM5102 I2S 输出、音量曲线和扬声器空间扩展均保持一致；显示器改为 160×128 ST7735 SPI 彩屏，并增加四个低电平按键。

## 默认接线

### PCM5102

| 信号 | ESP32 GPIO |
| --- | ---: |
| BCK | 26 |
| LRCK/WS | 25 |
| DATA | 23 |

### ST7735（SPI，160×128）

| 屏幕信号 | ESP32 GPIO |
| --- | ---: |
| SCL/SCK | 18 |
| SDA/MOSI | 19 |
| CS | 5 |
| DC/A0 | 16 |
| RES/RST | 17 |
| BL/LED | 4 |

模块的 VCC/GND 按模块规格连接；本固件将 BL 输出高电平点亮。屏幕不需要连接 MISO。默认参数适合常见 ST7735S 1.8 英寸 160×128 模块；颜色或方向不正确时可在 `main/board_config.h` 修改 RGB/BGR、方向和偏移。

### 四个按键

按键一端接 GPIO，另一端接 GND；固件启用内部上拉并做软件消抖。

| 按键 | ESP32 GPIO | 短按 | 长按/持续按住 |
| --- | ---: | --- | --- |
| PREV | 32 | 上一曲 | 音量减 |
| PLAY | 33 | 播放/暂停 | — |
| NEXT | 27 | 下一曲 | 音量加 |
| REVERB | 14 | 混响强度切换（关/轻微/标准/强） | — |

上一曲、播放/暂停、下一曲通过 AVRCP 控制手机。音量按键直接修改本机绝对音量，并将变化通知手机。若要改引脚，只修改 `main/board_config.h`。

## 屏幕内容

- 顶部：蓝牙连接和播放状态、当前编码格式与采样率
- 中部：32 柱彩色 FFT 频谱
- 底部：音量条与四键操作提示
- 未播放时仍显示状态界面，不影响蓝牙音频任务

## 构建

本目录是独立 ESP-IDF 工程。ESP-IDF 6.2 需先应用本目录 `idf_patches` 中的 LDAC/aptX 补丁，然后执行：

```powershell
cd st7735_version
idf.py set-target esp32
idf.py build
idf.py -p COM4 -b 96000 flash
idf.py -p COM4 -b 115200 monitor
```

## 说明

- ST7735 刷屏和 FFT 在 Core 0 的低优先级任务运行；音频解码继续在 Core 1，显示写入不会阻塞解码回调。
- 频谱队列深度为 1，新帧覆盖旧帧，显示跟不上时会自动丢弃旧画面。
- GPIO 5 和 GPIO 4 是 ESP32 启动配置脚。多数 ST7735 模块可正常使用，但模块在复位期间不得强拉这些脚到错误电平；如你的模块带强上/下拉，请在配置文件换用其它空闲 GPIO。

工程代码采用 MIT License；解码器许可与致谢见根目录文档及本目录的第三方声明。
