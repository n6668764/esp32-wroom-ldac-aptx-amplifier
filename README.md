# ESP32 LDAC/aptX HD/Classic 蓝牙功放（ESP-IDF 6.2）

本版本保留蓝牙音频接收、解码和 I2S 输出，并加入独立低优先级的 SSD1306 FFT 频谱显示。

## 硬件兼容性

- 支持普通 `ESP32-WROOM-32`、`ESP32-WROOM-32D`、`ESP32-WROOM-32E` 模组
- 4 MB Flash 即可，使用工程内的 3 MB 应用分区表
- 不需要 PSRAM；当前实机为双核 240 MHz 原版 ESP32
- 必须具备 Bluetooth Classic/BR-EDR。ESP32-C3、ESP32-S3、ESP32-C6 等仅支持 BLE 的型号不能运行本 A2DP Sink

## 音频链路

- 编解码器优先级：LDAC、aptX HD、aptX Classic、AAC、SBC
- 输出：44.1/48/88.2/96 kHz、32-bit I2S 槽；当前为左右声道混合后复制到 L/R 的单声道输出
- LDAC 解码输出为 16-bit PCM；aptX HD 保留 24 位有效 PCM
- DAC：PCM5102
- BCK：GPIO 26
- WS/LRCK：GPIO 25
- DATA：GPIO 23
- SSD1306：128×64 I²C，SDA GPIO 21、SCL GPIO 22、地址 `0x3C`
- 频谱：256 点单精度 FFT、32 柱、约 20 FPS；单帧覆盖不及时时自动丢弃旧显示数据，不阻塞音频
- 蓝牙名称：`ESP32 aptX Amplifier`

aptX Classic/HD 解码使用 `components/freeaptx`，其许可证见该目录中的 `COPYING`。
LDAC 解码使用实验性的 `components/libldacdec`，其许可证见该目录中的 `LICENSE`。本工程已将其 IMDCT 热路径从软件模拟的双精度改为 ESP32 可硬件执行的单精度浮点；能否持续实时解码仍须以实际播放测试为准，建议依次测试 LDAC 330/660/990 kbps，并观察 `LDAC overrun` 日志。



## ESP-IDF 6.2 补丁

ESP-IDF 6.2 的 A2DP Sink 公共接口尚未直接提供这些 vendor codec 解码路径。本工程还针对 aptX 媒体封装、aptX HD CIE/RTP、LDAC 载荷和高采样率进行了适配，因此构建前需要依次应用以下补丁：

```powershell
git -C $env:IDF_PATH apply C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-classic.patch
git -C $env:IDF_PATH apply C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-raw-media.patch
git -C $env:IDF_PATH apply C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-hd.patch
git -C $env:IDF_PATH apply C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-48k.patch
git -C $env:IDF_PATH apply C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-hd-cie13.patch
git -C $env:IDF_PATH apply C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-hd-rtp.patch
git -C $env:IDF_PATH apply --unidiff-zero C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-ldac-sink.patch
git -C $env:IDF_PATH apply C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-ldac-hires.patch
```

补丁已经应用过时不要重复执行。若需要撤销：

```powershell
git -C $env:IDF_PATH apply -R C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-ldac-hires.patch
git -C $env:IDF_PATH apply -R --unidiff-zero C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-ldac-sink.patch
git -C $env:IDF_PATH apply -R C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-hd-rtp.patch
git -C $env:IDF_PATH apply -R C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-hd-cie13.patch
git -C $env:IDF_PATH apply -R C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-48k.patch
git -C $env:IDF_PATH apply -R C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-hd.patch
git -C $env:IDF_PATH apply -R C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-raw-media.patch
git -C $env:IDF_PATH apply -R C:\path\to\sketch_aug6a_idf_gf\idf_patches\esp-idf-6.2-aptx-classic.patch
```

## 构建与烧录

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COM4 -b 96000 flash
idf.py -p COM4 -b 115200 monitor
```

针对当前芯片/串口链路，固定使用 96000 波特率烧录；串口日志仍使用 115200。若烧录出现串口中断或读取 Flash 失败，请先关闭占用 COM4 的监视器，并检查 GPIO0/EN 自动下载电路。

## 开源许可

工程自身代码采用 [MIT License](LICENSE)。内置解码器仍使用各自的上游许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。本工程为独立社区项目，LDAC、aptX 等名称及商标归其各自权利人所有。

## 鸣谢

感谢以下开源项目及其贡献者。本工程能够在普通 ESP32-WROOM 上实现多格式蓝牙音频接收，离不开他们公开的代码、文档和研究成果：

- [hegdi/libldacdec](https://github.com/hegdi/libldacdec)：本工程 LDAC 解码器的直接基础；在此基础上完成了 ESP-IDF 组件化、编译兼容修复和适合 ESP32 FPU 的单精度优化。
- [pali/libfreeaptx](https://github.com/pali/libfreeaptx)：提供 aptX Classic/HD 开源解码实现。
- [cfint/libfreeaptx-esp](https://github.com/cfint/libfreeaptx-esp)：为 freeaptX 在 ESP 平台上的组件封装提供参考。
- [Espressif ESP-IDF](https://github.com/espressif/esp-idf)：提供 ESP32、Bluetooth Classic A2DP、FreeRTOS、I2S 和 I2C 基础平台。
- [Espressif ESP Audio Codec](https://components.espressif.com/components/espressif/esp_audio_codec)：提供 SBC/AAC 解码组件。
- [pschatzmann/ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP)：项目早期 A2DP Sink 数据通路和音频处理方式的重要参考。
- [Thealexbarney/LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)：其音频编解码研究成果也被 `libldacdec` 上游特别致谢。

也感谢所有参与实机测试、协议栈排错和音质验证的贡献者。
