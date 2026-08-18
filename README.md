# RLCD Radio 中文版

适用于微雪 ESP32-S3 400x300 反射式 LCD 开发板的网络收音机固件。

本项目基于 Elektroda 用户 **dktr** 发布的原始工程继续开发。原项目完成了硬件驱动、反射式 LCD 显示、网络音频播放和频谱显示等核心功能：

- 原作者：dktr
- 原项目文章：[Elektroda - ESP32-S3 RLCD Internet Radio](https://www.elektroda.com/news/news4174232.html)

本仓库不是微雪官方项目。基础工程及其原有实现归原作者所有，仓库中包含的第三方库遵循各自许可证。

## 本仓库的主要改动

- 增加简体中文 LVGL 字体，汉化设备界面和 Web 配置页面。
- 内置中国之声、国际新闻广播等中国网络电台预设。
- 支持以 `名称|地址` 格式配置电台列表。
- 增加 `RLCD-Radio-Setup` Wi-Fi 配置热点。
- 固定 PioArduino、Arduino-ESP32 和 C++ 版本，支持无图形界面的可重复构建。
- 生成可直接转移到其他电脑烧录的单文件 factory 镜像。
- 修复 Arduino-ESP32 3.x 构建兼容问题。
- 修复 NTP 初始化导致的启动崩溃。
- 修复音频数据仍正常时看门狗误重连的问题。
- 修复音频解码后 SPI DMA 内存不足导致的重启。
- 修正 ES8311 音量范围和默认音量处理。

## 硬件

- ESP32-S3，240 MHz
- 400x300 反射式 LCD
- ES8311 音频编解码器
- 8 MB Flash
- PSRAM

显示、音频和外设引脚已经按照原项目使用的微雪开发板配置。不要直接烧录到只有 ESP32-S3 芯片相同、但屏幕和引脚不同的其他开发板。

## 无界面构建

项目使用 PlatformIO Core，不要求安装 VS Code 或 PlatformIO IDE。

### Linux / macOS

```sh
git clone https://github.com/yangxuan/RLCD-Radio.git
cd RLCD-Radio

python3 -m venv .venv
. .venv/bin/activate
python -m pip install "platformio==6.1.19"

pio run -e waveshare_esp32_s3_rlcd
```

也可以不激活虚拟环境：

```sh
.venv/bin/pio run -e waveshare_esp32_s3_rlcd
```

首次构建需要联网下载固定版本的 PioArduino 平台和少量 PlatformIO 依赖。

## 构建产物

构建成功后，文件位于：

```text
.pio/build/waveshare_esp32_s3_rlcd/
```

主要文件：

| 文件 | 用途 | 烧录偏移 |
| --- | --- | --- |
| `firmware.factory.bin` | bootloader、分区表和应用合并镜像，推荐用于首次烧录 | `0x0` |
| `firmware.bin` | 仅应用固件 | `0x10000` |
| `bootloader.bin` | 分离烧录使用 | `0x0` |
| `partitions.bin` | 分离烧录使用 | `0x8000` |
| `boot_app0.bin` | 分离烧录使用 | `0xe000` |

## 烧录

推荐将 `firmware.factory.bin` 复制到烧录电脑，然后执行：

```sh
esptool.py --chip esp32s3 write-flash 0x0 firmware.factory.bin
```

烧录前请确认串口对应目标开发板。首次安装其他固件后出现异常时，可以先擦除 Flash：

```sh
esptool.py --chip esp32s3 erase-flash
```

## Wi-Fi 和电台配置

首次启动或保存的 Wi-Fi 无法连接时，设备会创建配置热点：

```text
热点名称: RLCD-Radio-Setup
热点密码: 11111111
配置地址: http://192.168.4.1
```

连接热点后，在浏览器打开配置地址，可以设置：

- Wi-Fi 名称和密码
- NTP 服务器
- 开机音量
- 网络电台列表

电台列表每行一个，格式如下：

```text
中国之声|https://example.com/live.mp3
电台名称|音频流地址
```

设备连接家庭 Wi-Fi 后，可以通过屏幕显示的局域网 IP 再次进入 Web 配置页面。开发板上的实体按键用于切换电台，当前音量通过 Web 页面调整。

## GitHub Actions

仓库中的 GitHub Actions 会在 push、pull request 和手动触发时执行 headless 构建。

推送 `v*` 标签时，CI 还会创建对应 GitHub Release，并上传：

```text
firmware.factory.bin
firmware.bin
SHA256SUMS.txt
```

发布示例：

```sh
git tag v0.2.3
git push origin v0.2.3
```

## 更新记录

详细变更见 [CHANGELOG.md](CHANGELOG.md)。
