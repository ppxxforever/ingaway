# 内置 Flash + W25Q64 存储方案

## 分工

内置 16 MiB Flash 保持 `partitions.csv` 的布局不变，负责程序、4 MiB × 2 OTA 槽、NVS、备用文件空间与内部崩溃转储。外置 W25Q64 的 64 Mbit 等于 8 MiB，不是 64 MB，不用作启动固件或 OTA 槽。

外置分区由 `w25q64_init()` 在运行时通过 `esp_partition_register_external()` 注册，不写进内置 Flash 分区表：

| label | 外置芯片内偏移 | 大小 | data subtype | 用途 |
| --- | --- | --- | --- | --- |
| ext_history | 0x000000 | 6 MiB | 0x41 | Modbus 采集历史环 |
| ext_cache | 0x600000 | 1 MiB | 0x40 | MQTT 待确认记录 |
| ext_logs | 0x700000 | 1 MiB | 0x42 | 分级事件日志环 |

已实现芯片检测、容量校验、分区注册和三个分区的原始 Flash 记录格式，均不能挂载 SPIFFS。历史与日志由独立存储任务处理，满容量时循环淘汰最旧记录，详见 `history-logs.md`。MQTT 工作任务已接入持久化缓存补发：Flash 优先、RAM 有界兜底、匹配 PUBACK 后释放；其未确认记录不淘汰，详见 `mqtt-reconnect.md`。

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

确认具体开发板 GPIO10～13 已引出且未被其他外设占用。不要连接到模块内部 Flash/PSRAM 使用的引脚。若外设使用 I²C，须与 SPI 引脚分开。

必须确认完整型号：常见 W25Q64JV/FV 为 3 V 系列，可使用 3.3 V；W25Q64FW 等 1.8 V 型号不能直接接 3.3 V，应使用合适电源和电平转换。裸芯片旁建议放置 0.1 µF 去耦电容，线短并共地。JEDEC ID/容量检测无法替代电压确认。

`idf.py menuconfig` → `External W25Q64 storage` 可以修改引脚或关闭启动检测。外置芯片缺失或缓存恢复失败时只打印警告，继续启动其他功能与 Wi-Fi。芯片初始化及缓存扫描均不写入、擦除或自动格式化。

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

## 缓存格式与恢复（已实现）

实现位于 `components/w25q64/flash_fifo.c`，ESP-IDF 分区适配器位于 `w25q64_cache.c`。启动时先 `w25q64_init()`，再 `w25q64_cache_init()`；`main/ingaway.c` 已加入此流程。

- 一个 4 KiB 扇区存一条记录，1 MiB 分区最多 256 条，单条负载 1～4056 字节。该保守格式避免不同消息共用擦除扇区造成未确认数据丢失，代价是空间利用率较低。
- 40 字节小端头：magic、版本、64 位 ID、32 位长度、负载 IEEE CRC32、静态头 IEEE CRC32、提交标记、确认标记及保留字段。CRC 覆盖 ID、长度等不可变元数据，不仅校验负载。
- 按顺序写入并回读校验静态头、写负载并回读校验 CRC，最后写四字节全零提交标记。只有完整提交且 CRC 正确的记录才可读。
- `peek` 不消费；`ack(id)` 仅确认最旧记录。四字节确认标记全部写完才生效，确认中断则恢复后再次发送，允许重复、不保证 exactly-once。
- 开机扫描全部扇区：按最小未确认 ID 恢复读位置，按最大有效预留 ID 的下一扇区恢复写位置。头已经完成而负载或提交中断的记录不发送，其 ID 仍被占用；允许 ID 有间隙。
- ID 从 1 递增，最新有效 ID 的扇区作为持久化高水位保留，不擦除，即使已经确认。回收更旧的已确认或中断记录，确保擦除中断不会导致成功记录 ID 重用。全部有效空间只有高水位保留扇区时返回满；64 位 ID 耗尽返回 `FF_EXHAUSTED`，不回绕。
- 未确认记录绝不覆盖。没有可回收扇区时返回 `FF_FULL`；调用方决定拒绝新采样、限速或报警，不能把该错误当成写入成功。
- 未知格式、撕裂头或非全空白扇区隔离保留，不自动擦除；反复在写头时掉电可能使可用容量减少，最终返回满，需显式维护。已提交未确认记录的头/负载损坏返回 `FF_CORRUPT`，停止使用，不静默丢弃或格式化。
- 任一读写/擦除 IO 错误使句柄失效，必须重新初始化扫描后再操作，不能在部分写入的 NOR 地址上直接重试。当前接口为单任务所有者设计，不可并发调用；不要在 MQTT 回调中执行 Flash 操作。
- ID 在当前芯片/格式生命周期内唯一，跨设备应组合设备 ID；人为全分区擦除会重置序号，不提供跨格式化的唯一性保证。

### 调用示例

其他组件声明 `PRIV_REQUIRES w25q64`：

```c
#include "w25q64_cache.h"

uint64_t id;
int rc = w25q64_cache_append(payload, payload_len, &id);
if (rc == FF_FULL) {
    // 缓存满：拒绝新记录并记录告警，旧记录仍保留。
}

uint8_t buffer[FF_MAX_PAYLOAD];
size_t len;
rc = w25q64_cache_peek(buffer, sizeof(buffer), &len, &id);
if (rc == FF_OK) {
    // 将 (设备 ID, id) 一并上传以便服务端去重。
    // 等待与此记录对应的 QoS 1 PUBACK 后，才调用：
    // w25q64_cache_ack(id);
}
```

消息 ID 必须以返回的缓存 ID 关联网络确认，不能以 MQTT 会话内 packet ID 作为持久化唯一 ID。重连需丢弃旧会话映射，重新 peek 未确认记录。MQTT 已通过单条在途记录实现该映射，并将独立的 `record_id` 写入 JSON 负载，RAM→Flash 迁移时不改变它；服务端使用 `(device_id, record_id)` 去重，不能使用迁移时可能改变的 Flash 物理记录 ID。负载为任意二进制数据，peek 返回准确长度，不追加字符串终止符。

### 编译检查

```sh
python "$IDF_PATH/tools/idf.py" build
```

按要求不附带测试程序。实际 W25Q64 的页编程及扇区擦除掉电行为仍需上板验证；编译成功不能证明硬件断电恢复可靠性。

## 历史、日志与后续优化

历史与日志入口为 `storage_archive.h`，实现为 `storage_archive.c`。成功采集、最终采集失败和网络失效快照进入历史环；Wi-Fi/MQTT 连接事件、采集异常、PUBACK 与补传超时进入分级日志环。UTC 仅在显式 SNTP 同步后有效，未同步记录保留启动标识和单调时间。历史/日志读取不消费，MQTT PUBACK 不会删除它们。

当前仍是一扇区一条记录，后续可以兼容升级为批量写入、多记录扇区，以增加保留时长和降低擦写频率。NVS 仅每次启动更新一次启动序号，不随采样写入。内置 Flash 更新不会主动擦除外置芯片，新固件须兼容或迁移外置数据格式；未知旧格式保持隔离，不自动格式化。

软件编译验证不能代替上板读写与掉电测试。本次没有烧录、格式化或擦除任何硬件。
