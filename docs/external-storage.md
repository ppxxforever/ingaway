# 内置 Flash + W25Q64 存储方案

## 分工

内置 16 MiB Flash 保持 `partitions.csv` 的布局不变，负责程序、4 MiB × 2 OTA 槽、NVS、备用文件空间与内部崩溃转储。外置 W25Q64 的 64 Mbit 等于 8 MiB，不是 64 MB，不用作启动固件或 OTA 槽。

外置分区由 `w25q64_init()` 在运行时通过 `esp_partition_register_external()` 注册，不写进内置 Flash 分区表：

| label | 外置芯片内偏移 | 大小 | 规划用途 |
| --- | --- | --- | --- |
| ext_history | 0x000000 | 6 MiB | 环境/Modbus 历史数据 |
| ext_cache | 0x600000 | 1 MiB | MQTT 断网待发送记录 |
| ext_logs | 0x700000 | 1 MiB | 循环运行日志 |

目前只实现芯片检测、容量校验与分区注册，没有挂载文件系统、擦除、格式化、记录采样或补发数据。分区 subtype 预留为 SPIFFS，后续可以分别显式挂载；若改用原始环形记录格式，应同步调整 subtype 和文档，不能混用两种格式。

## 默认接线

外置芯片独占 SPI2_HOST，普通单线 SPI，10 MHz：

| W25Q64 信号 | ESP32-S3 |
| --- | --- |
| /CS | GPIO10 |
| DI / IO0 / MOSI | GPIO11 |
| CLK | GPIO12 |
| DO / IO1 / MISO | GPIO13 |
| GND | GND |
| /WP / IO2 | 拉高到芯片电源电压 |
| /HOLD 或 /RESET / IO3 | 按芯片手册拉高到芯片电源电压 |

确认具体开发板 GPIO10～13 已引出且未被其他外设占用。不要连接到模块内部 Flash/PSRAM 使用的引脚。I²C 仍使用 GPIO8/9，与 SPI 分开。

必须确认完整型号：常见 W25Q64JV/FV 为 3 V 系列，可使用 3.3 V；W25Q64FW 等 1.8 V 型号不能直接接 3.3 V，应使用合适电源和电平转换。裸芯片旁建议放置 0.1 µF 去耦电容，线短并共地。JEDEC ID/容量检测无法替代电压确认。

`idf.py menuconfig` → `External W25Q64 storage` 可以修改引脚或关闭启动检测。外置芯片缺失时只打印警告，继续启动环境采集和 Wi-Fi。默认初始化不改动已有芯片内容。

## 后续使用接口

```c
#include "w25q64.h"

const esp_partition_t *part = w25q64_history_partition();
if (part != NULL) {
    // 使用 esp_partition_read(part, offset, buffer, size) 读取。
    // offset 是分区内偏移，不是整个芯片的绝对地址。
}
```

接口返回的分区句柄仅在初始化成功后有效，生命周期为整个应用运行期间。需要访问此组件的其他组件声明 `PRIV_REQUIRES w25q64`。

原始 Flash 写入只能将位从 1 改成 0；重写前需擦除，擦除偏移与大小必须以 4 KiB 扇区对齐。不要盲目擦除整片芯片，也不要直接覆写已使用的记录。不要通过外置 Flash 的 mmap 读取，使用分区读写接口。

## 数据层规划（尚未实现）

- 一个存储任务串行处理写入，传感器/Modbus 任务通过队列提交。
- 历史记录包含版本、序号、时间戳、有效字段、数值与 CRC。
- 批量写入，采用分扇区循环存储、掉电恢复和保留空间；不要每次采样写 NVS。
- 尚未同步时间时保存启动标识和单调时间，避免把无效时间当成真实日期。
- 离线消息具有唯一序号；QoS 1 收到 PUBACK 后再标记完成，允许重复而不随意丢失。
- 限制离线队列与日志占用，明确满存储时的淘汰策略。
- 若使用 SPIFFS，不要默认启用 `format_if_mount_failed`；首次格式化必须是明确的维护操作，避免误删数据。
- 内置 Flash 更新不会主动擦除外置芯片，但新固件须兼容或迁移外置数据格式。

软件编译验证不能代替上板读写与掉电测试。本次没有烧录、格式化或擦除任何硬件。
