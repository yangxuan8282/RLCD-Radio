# 无界面构建与烧录

## 构建

项目固定使用 PioArduino 55.03.311 和 Arduino-ESP32 3.3.11，不需要图形界面。

```sh
python3 -m venv .venv
.venv/bin/pip install platformio
.venv/bin/pio run -e waveshare_esp32_s3_rlcd
```

主要产物位于 `.pio/build/waveshare_esp32_s3_rlcd/`：

- `firmware.factory.bin`：包含 bootloader、分区表和应用，推荐传到烧录机使用。
- `firmware.bin`：仅应用固件，写入偏移为 `0x10000`。
- `bootloader.bin`、`partitions.bin`、`boot_app0.bin`：分离烧录时使用。

## 单文件烧录

将 `firmware.factory.bin` 传到烧录机，然后执行：

```sh
esptool.py --chip esp32s3 write-flash 0x0 firmware.factory.bin
```

## Wi-Fi 配置

首次启动或保存的 Wi-Fi 无法连接时，设备会创建以下热点：

```text
名称: RLCD-Radio-Setup
密码: 11111111
配置地址: http://192.168.4.1
```

配置页面支持填写 Wi-Fi、NTP、音量和电台列表。电台格式为 `名称|URL`，每行一个。
