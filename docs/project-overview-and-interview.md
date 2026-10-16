# Ingaway 项目全方位介绍与面试问答

> 项目定位：基于 ESP32-S3 的联网采集与上报终端。设备通过 BLUFI 完成 Wi-Fi 配网，使用 Modbus TCP 采集传感器/从站数据，通过 MQTT over TLS 上报；本地使用 ST7789 LCD + 编码器进行交互；外接 W25Q64 Flash 做历史记录、日志和 MQTT 离线缓存；支持基于 S3/CloudFront HTTPS 地址的 OTA、双分区回滚、自检确认有效。

---

## 1. 项目概览

### 1.1 硬件平台

当前主控目标：

- MCU：ESP32-S3
- Flash：16MiB，配置为 ESP32-S3 N16R8
- PSRAM：8MiB，LCD framebuffer 优先放 PSRAM
- 外部存储：W25Q64 SPI Flash
- 显示：ST7789，240x320，SPI
- 输入：旋转编码器 + 按键
- 网络：Wi-Fi STA
- 协议：BLUFI、Modbus TCP、MQTT TLS、HTTPS OTA

后续扩展记录：

- STM32F103C8T6 作为后续接入节点/从设备方向
- 资源约束：64KB Flash / 20KB RAM
- 初期通过 ST-Link + SWD 烧录
- STM32 Bootloader 与远程升级列为后续扩展

---

## 2. 主要功能

### 2.1 Wi-Fi 配网与连接恢复

相关文件：

- `components/wifi/wifi.c`
- `components/wifi/include/wifi.h`
- `components/blufi/`

主要功能：

1. 使用 ESP-IDF BLUFI 进行 BLE 配网。
2. 启动时优先读取 NVS 中已保存的 Wi-Fi 配置。
3. 如果存在历史 Wi-Fi 配置，则先尝试自动连接。
4. 如果 30 秒内未连接成功，则启动 BLUFI 配网。
5. 配网成功并获取 IP 后，关闭 BLE host/controller，释放资源。
6. Wi-Fi 断线后通过 `wifi_recovery_task` 做周期性恢复。
7. Wi-Fi 断线时通知 MQTT 和 Modbus 进入网络不可用状态。

关键设计点：

- 只在拿到 `IP_EVENT_STA_GOT_IP` 后认为网络真正可用。
- 断线时区分 AP 关联状态和 IP 状态。
- BLUFI 只用于配网，配网成功后关闭 BLE，减少内存和功耗占用。
- 本地 UI 可触发重新配网：写入 `wifi_boot/force_blufi`，然后重启进入 BLUFI。

---

### 2.2 Modbus TCP 数据采集

相关文件：

- `components/modbusTCP/modbusTCP.c`
- `components/modbusTCP/include/modbusTCP.h`
- `components/modbusTCP/Kconfig`

主要功能：

1. 通过 Modbus TCP 连接从站。
2. 周期性读取温度寄存器。
3. 维护最新采样快照。
4. 提供非阻塞读取接口：
   - `modbusTCP_get_temp_sample()`
   - `read_temp()`
5. 支持网络断线标记 stale。
6. 支持连接失败、读失败、服务器关闭后的重连。
7. 异常后释放 Modbus master/controller，避免复用坏 socket。
8. 支持有限重试、指数退避和恢复日志。

采集状态：

```c
typedef enum {
    MODBUS_WAIT_NETWORK,
    MODBUS_CONNECTING,
    MODBUS_POLLING,
    MODBUS_BACKOFF,
    MODBUS_FAULT,
} modbus_acquisition_state_t;
```

关键设计点：

- 使用 `network_generation` 防止旧网络会话返回的数据污染新网络状态。
- Wi-Fi 断线时立即将缓存数据标记为 stale。
- 读失败后释放 controller/socket，下一次重试重新连接。
- backoff 前释放资源，避免隐藏 socket 泄漏。
- 对 task watchdog 做喂狗，长 backoff 被拆成短 slice。
- 周期记录 internal heap 与任务栈高水位，便于定位内存泄漏或栈不足。

---

### 2.3 MQTT TLS 上报与离线缓存

相关文件：

- `components/mymqtt/mymqtt.c`
- `components/mymqtt/include/mymqtt.h`
- `components/mymqtt/Kconfig`
- `components/mymqtt/certs/`

主要功能：

1. MQTT over TLS 连接服务器。
2. 使用 CA + 客户端证书 + 客户端私钥进行 TLS 双向认证。
3. 上报：
   - telemetry
   - temperature
   - heartbeat
4. 网络断线时停止旧 MQTT session。
5. 数据先进入缓存，再由 worker 发送。
6. 支持 Flash 离线缓存和 RAM fallback。
7. 使用 QoS/PUBACK 确认后释放缓存记录。
8. 每条数据带 `record_id`，用于去重与追踪。

关键设计点：

- 不跳过 TLS 主机名校验：

```c
.verification.skip_cert_common_name_check = false
```

- MQTT 自动重连关闭，由项目 worker 自己控制重连节奏。
- Flash FIFO 中的记录只有收到 ACK 后才释放。
- RAM fallback 防止外部 Flash 暂时不可用时完全丢数据。
- `record_id` 由随机 boot nonce + 单调序列生成，避免重启后重复。

---

### 2.4 外部 W25Q64 存储

相关文件：

- `components/w25q64/w25q64.c`
- `components/w25q64/flash_fifo.c`
- `components/w25q64/w25q64_cache.c`
- `components/w25q64/storage_archive.c`
- `components/w25q64/include/`

主要功能：

1. 初始化外部 W25Q64 SPI Flash。
2. 提供 Flash FIFO 抽象。
3. 提供 MQTT 离线缓存。
4. 提供历史采样归档。
5. 提供运行日志归档。
6. 写入采用后台任务异步处理，业务线程不直接做长时间 Flash IO。

存储类型：

- MQTT cache：用于可靠上报，收到 PUBACK 后才 ack。
- History archive：保存采样历史，不被 MQTT ACK 消费。
- Logs archive：保存运行日志、错误日志、状态变化。

关键设计点：

- MQTT 缓存与历史归档分离，避免上报 ACK 删除历史记录。
- 存储提交函数非阻塞，只负责复制数据到队列。
- 队列溢出时记录 drop 统计。
- 外部 Flash 不可用时，不自动格式化，避免误擦数据。

---

### 2.5 LCD UI、息屏唤醒与本地交互

相关文件：

- `components/interaction/interaction.c`
- `components/interaction/lcd.c`
- `components/interaction/input_logic.h`
- `components/interaction/ui_logic.h`
- `components/interaction/include/interaction_config.h`

主要功能：

1. ST7789 LCD 显示菜单。
2. 编码器旋转选择菜单。
3. 按键点击进入/确认。
4. 长按返回主菜单。
5. 可触发 Wi-Fi 重新配网。
6. 空闲自动息屏。
7. 输入或外部状态变化唤醒屏幕。

息屏唤醒策略：

- `LCD_IDLE_SLEEP_MS` 默认 30 秒。
- 空闲后调用：

```c
esp_lcd_panel_disp_on_off(panel, false)
```

并关闭背光 GPIO。

- 息屏后第一下编码器/按键输入只负责唤醒，不执行菜单动作，避免黑屏误触确认。
- 外部状态变化可调用：

```c
interaction_notify_status_change();
```

关键设计点：

- LCD framebuffer 只允许分配到 PSRAM，避免占用内部 RAM。
- 输入采样在 esp_timer 回调中完成，不做绘制和阻塞操作。
- UI task 独占 LCD 绘制，避免并发绘图。

---

### 2.6 S3 HTTPS OTA 与自检回滚

相关文件：

- `components/ota_service/ota_service.c`
- `components/ota_service/include/ota_service.h`
- `components/ota_service/Kconfig`
- `docs/s3-https-ota.md`
- `partitions.csv`

主要功能：

1. 从 HTTPS URL 下载固件。
2. URL 可指向 S3 或 CloudFront。
3. 支持双 OTA 分区。
4. OTA 前检查 Flash 容量和 OTA 分区。
5. 校验候选固件 project name。
6. 比较当前版本与候选版本，相同则跳过。
7. 下载完成后切换启动分区并重启。
8. 新固件通过自检后才标记 valid。
9. 自检失败则回滚旧分区。

分区表：

```csv
ota_0, app, ota_0, 0x20000,  0x400000,
ota_1, app, ota_1, 0x420000, 0x400000,
```

启用回滚：

```ini
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

OTA 自检内容：

1. 当前目标必须为 ESP32-S3。
2. Flash 容量不少于配置值，默认 16MiB。
3. 至少存在两个 OTA app 分区。
4. running partition 和 next update partition 有效且不同。
5. 当前运行固件 `project_name` 与配置值匹配。
6. 已经通过 Wi-Fi got IP 路径，作为首期网络可用门槛。

关键设计点：

- 新固件首次启动处于 `ESP_OTA_IMG_PENDING_VERIFY`。
- 只有自检通过才调用：

```c
esp_ota_mark_app_valid_cancel_rollback();
```

- 自检失败调用：

```c
esp_ota_mark_app_invalid_rollback_and_reboot();
```

- 自检阶段不做 NVS schema 迁移，保证回滚后旧固件仍能读懂 NVS。

---

## 3. 系统启动流程

```text
app_main
  ↓
初始化 NVS
  ↓
创建默认事件循环
  ↓
初始化 W25Q64 外部存储
  ↓
启动历史/日志归档服务
  ↓
初始化外部 MQTT cache
  ↓
初始化 Wi-Fi
  ↓
启动 LCD/UI
```

Wi-Fi 成功后：

```text
IP_EVENT_STA_GOT_IP
  ↓
设置 CONNECTED_BIT / AUTO_RECONNECT_BIT
  ↓
Modbus TCP init
  ↓
MQTT start
  ↓
OTA start once
  ↓
如处于 PENDING_VERIFY，执行 OTA 自检并标记 valid 或回滚
```

断网后：

```text
WIFI_EVENT_STA_DISCONNECTED / IP_EVENT_STA_LOST_IP
  ↓
Modbus network_down
  ↓
MQTT network_down
  ↓
样本 stale
  ↓
等待 Wi-Fi 恢复
```

---

## 4. 关键可靠性设计

### 4.1 网络 epoch / generation

Modbus 使用 `network_generation` 表示网络代际。

- GOT_IP 时递增 generation。
- network_down 时递增 generation。
- 请求发起时记录 generation。
- 响应回来时只有 generation 仍然一致才发布数据。

解决的问题：

```text
旧 TCP 请求超时或断网后才返回
如果不判断 generation，可能把旧数据当成新网络数据发布
```

---

### 4.2 stale 缓存语义

Modbus 最新采样结构中包含：

- `has_value`
- `stale`
- `last_success_ms`
- `last_error`
- `state`

这区分了：

1. 从未采样成功。
2. 曾经采样成功，但现在过期。
3. 当前采样新鲜。
4. 网络或设备异常。

`read_temp()` 约定：

- 无样本：`ESP_ERR_INVALID_STATE`
- 样本过期：`ESP_ERR_TIMEOUT`
- 成功：`ESP_OK`
- 过期时不改调用者输出 buffer

---

### 4.3 MQTT exactly-once 风格的应用层去重

MQTT 本身使用 QoS，但设备端仍给每条记录加入 `record_id`。

目的：

- 网络重连后可能重复发布。
- broker 或后端可能收到重复消息。
- 后端可使用 `record_id` 去重。

record_id 由：

```text
boot_nonce + sequence
```

构成，降低重启后 ID 冲突概率。

---

### 4.4 OTA 回滚和 NVS 兼容

OTA 的最大风险不是下载失败，而是：

```text
新固件启动后修改了持久化数据
随后自检失败回滚旧固件
旧固件无法理解新格式 NVS
```

因此本项目约束：

- 标记 app valid 前不做破坏性 NVS 迁移。
- 新字段要向后兼容。
- 必须保留 Wi-Fi 凭据和 `wifi_boot` namespace 兼容。

---

### 4.5 任务看门狗与资源监控

Modbus worker：

- 注册 task watchdog。
- 循环、等待、backoff 中喂狗。
- 周期记录：
  - internal heap free
  - internal heap minimum free
  - task stack high-water mark

目的：

- 发现死循环或长时间卡死。
- 定位内存泄漏。
- 定位栈溢出风险。

---

## 5. ESP32 OTA 与 STM32 Bootloader 跳转的区别

ESP32 OTA：

```text
ESP-IDF Bootloader
  ↓
读取 partition table 和 otadata
  ↓
选择 ota_0 或 ota_1
  ↓
启动对应 app
```

STM32 Bootloader 跳转：

```text
自写 Bootloader
  ↓
检查 App 地址和合法性
  ↓
设置 MSP
  ↓
设置 VTOR
  ↓
跳转到 App Reset_Handler
```

核心区别：

| 项目 | ESP32 OTA | STM32 Bootloader |
|---|---|---|
| 启动选择 | 官方 Bootloader + otadata | 用户 Bootloader 自己判断 |
| 固件位置 | ota_0 / ota_1 分区 | 固定 Flash 地址 |
| 跳转细节 | ESP-IDF 内部完成 | 用户设置 MSP/VTOR 并跳转 |
| 回滚 | ESP-IDF 原生支持 | 需要自己实现 |
| 资源 | 当前每个 OTA 分区 4MiB | F103C8T6 总 Flash 64KB |
| 远程升级 | HTTPS OTA API | 需自写下载、校验、写 Flash |

STM32F103C8T6 当前策略：

- 先完成网络模块驱动、TCP 服务端、传感器驱动。
- 核对 64KB Flash / 20KB RAM 占用。
- 初期用 ST-Link + SWD 烧录。
- Bootloader 与远程升级后续再做。

---

## 6. 模块清单

| 模块 | 路径 | 作用 |
|---|---|---|
| main | `main/ingaway.c` | 系统入口，初始化 NVS、事件循环、存储、Wi-Fi、UI |
| Wi-Fi/BLUFI | `components/wifi`, `components/blufi` | 配网、连接、断线恢复、BLE 关闭 |
| Modbus TCP | `components/modbusTCP` | 采集温度寄存器、状态缓存、异常恢复 |
| MQTT | `components/mymqtt` | TLS MQTT 上报、心跳、离线缓存、ACK 释放 |
| 外部 Flash | `components/w25q64` | W25Q64 驱动、FIFO、缓存、历史和日志 |
| UI | `components/interaction` | ST7789、编码器菜单、息屏唤醒 |
| OTA | `components/ota_service` | HTTPS OTA、版本检查、型号校验、自检回滚 |
| 文档 | `docs/` | 分区、MQTT、OTA、外部存储、STM32 接入等说明 |

---

## 7. 构建与测试

构建：

```bash
ninja -C build
```

Modbus host 单元测试：

```bash
python3 components/modbusTCP/tests/run_tests.py
```

已覆盖场景：

- 连接失败有限重试
- 读取超时后释放并重连
- Wi-Fi flap 后旧响应丢弃
- Wi-Fi 恢复后继续轮询
- controller cleanup fail 进入 fault
- backoff capped/reset
- singleton worker 防重复创建

---

# 8. 面试常问问题与参考答案

## 8.1 项目整体架构怎么介绍？

**答：**

这个项目是一个 ESP32-S3 边缘采集与上报终端。底层通过 Wi-Fi 接入网络，使用 BLUFI 做首次配网；采集侧通过 Modbus TCP 周期读取从站寄存器；数据侧通过 MQTT over TLS 上报云端；可靠性上使用 W25Q64 外部 Flash 做 MQTT 离线缓存、历史采样和运行日志；维护侧支持 S3/CloudFront HTTPS OTA，并使用 ESP-IDF 双 OTA 分区和回滚机制确保升级失败可恢复；本地交互使用 ST7789 LCD 和编码器，并支持空闲息屏唤醒。

---

## 8.2 Wi-Fi 配网为什么用 BLUFI？

**答：**

BLUFI 是 Espressif 官方提供的 BLE 配网方案，适合无键盘、无屏幕或小屏设备。它可以通过手机把 SSID 和密码安全传给 ESP32。项目中 BLUFI 只在首次配网或重新配网时启用，配网成功并拿到 IP 后会关闭 BLE host/controller，以释放内存和降低功耗。

---

## 8.3 为什么 Wi-Fi connected 不等于网络可用？

**答：**

`WIFI_EVENT_STA_CONNECTED` 只表示设备已关联到 AP，但还没有 DHCP IP，也不代表网络栈可用。本项目只有在 `IP_EVENT_STA_GOT_IP` 后才启动 MQTT、Modbus 和 OTA，因为这时 TCP/IP 通信才具备基本条件。

---

## 8.4 Modbus TCP 为什么要维护 network_generation？

**答：**

因为 TCP 请求可能跨越网络状态变化。比如请求发出后 Wi-Fi 断开又恢复，旧 socket 上的请求可能晚到或返回。如果不区分网络代际，就可能把旧网络会话的数据发布到新状态里。`network_generation` 在断线和重新 got IP 时递增，请求开始时记录 generation，响应回来时只有 generation 一致才接受数据。

---

## 8.5 Modbus 读失败为什么要释放 controller/socket？

**答：**

读超时、服务器关闭、半开 TCP 连接等异常后，底层 socket 状态可能已经不可预测。继续复用同一个 controller 可能导致后续请求持续失败或资源泄漏。因此读失败后主动 `mbc_master_delete()`，下一轮重新创建 controller 和 socket，能覆盖服务器重启、连接被 NAT/AP 清理等情况。

---

## 8.6 如何避免 Modbus worker 被看门狗复位？

**答：**

项目中 worker 注册了 task watchdog，并在主循环、网络等待和 backoff 分片中调用 `esp_task_wdt_reset()`。长时间 backoff 不使用一次性长 delay，而是拆成 `WAIT_SLICE_MS` 小片段，这样既能及时响应网络变化，也能定期喂狗。

---

## 8.7 MQTT 为什么需要离线缓存？

**答：**

现场网络不稳定时，采集数据不能因为 MQTT 断线就直接丢失。项目中先把上报数据写入缓存，然后再发布。Flash 可用时写入 W25Q64 FIFO；Flash 不可用时使用有限 RAM fallback。收到 PUBACK 后才释放缓存记录，保证至少一次投递。

---

## 8.8 MQTT 已经 QoS1 了，为什么还要 record_id？

**答：**

QoS1 保证消息至少到达一次，但不能保证后端只收到一次。断线重连、PUBACK 丢失、客户端重发都可能导致重复消息。`record_id` 提供应用层幂等键，后端可以据此去重。

---

## 8.9 TLS 连接有哪些安全点？

**答：**

项目使用 MQTT over TLS，配置 CA 证书、客户端证书和客户端私钥。证书校验不跳过 CN/hostname 检查：

```c
.verification.skip_cert_common_name_check = false
```

HTTPS OTA 也使用证书 bundle 校验服务器证书，不使用明文 HTTP，也不关闭证书校验。

---

## 8.10 OTA 为什么需要双分区？

**答：**

如果只有一个 app 分区，下载新固件时会覆盖当前正在运行的固件，一旦中途断电就可能变砖。双 OTA 分区允许当前 app 继续运行，同时把新固件写入另一个分区。写入成功后只更新 `otadata`，重启后 Bootloader 再切换到新分区。

---

## 8.11 ESP32 OTA 回滚是怎么实现的？

**答：**

项目启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`。OTA 成功后，新固件首次启动处于 `ESP_OTA_IMG_PENDING_VERIFY`。应用必须通过自检后调用：

```c
esp_ota_mark_app_valid_cancel_rollback();
```

否则如果崩溃、看门狗复位或主动调用 invalid rollback，Bootloader 下次会回滚到旧分区。

---

## 8.12 OTA 自检为什么放在 Wi-Fi got IP 后？

**答：**

该设备的核心能力依赖联网，包括 MQTT、Modbus TCP 和后续 OTA。如果新固件连基本网络都无法恢复，直接标记 valid 风险较大。因此项目把 Wi-Fi got IP 作为首期网络可用门槛，只有进入该路径后才进行自检并标记新固件有效。

---

## 8.13 OTA 为什么要校验 project_name？

**答：**

避免把错误型号或错误项目的固件刷入设备。例如同样是 ESP32-S3 的其他项目固件，芯片类型可能匹配，但业务逻辑和硬件引脚完全不同。项目读取候选镜像的 `esp_app_desc.project_name`，必须匹配 `CONFIG_INGAWAY_OTA_PROJECT_NAME`，默认 `ingaway`。

---

## 8.14 OTA 和 NVS 兼容有什么关系？

**答：**

如果新固件在 pending verify 阶段修改了 NVS 格式，然后自检失败回滚旧固件，旧固件可能读不懂新 NVS 格式，导致无法联网或异常。因此项目规定：标记 app valid 前不做破坏性 NVS schema 迁移，Wi-Fi 配置和 `wifi_boot` namespace 保持向后兼容。

---

## 8.15 外部 W25Q64 Flash 为什么不能随便自动格式化？

**答：**

外部 Flash 中可能有未上报的 MQTT 缓存、历史采样和故障日志。如果初始化失败就自动格式化，可能造成数据丢失。项目策略是：外部 Flash 不可用时记录 warning，并降级运行，而不是自动擦除。

---

## 8.16 LCD framebuffer 为什么放 PSRAM？

**答：**

240x320 RGB565 framebuffer 大小约：

```text
240 * 320 * 2 = 153600 bytes
```

如果放内部 RAM，会挤压 Wi-Fi、TLS、MQTT、Modbus 和 FreeRTOS 任务栈空间。项目要求 framebuffer 只分配到 PSRAM；如果 PSRAM 不可用，则 UI 初始化失败，但主业务继续运行。

---

## 8.17 为什么息屏后第一下输入只唤醒不执行动作？

**答：**

黑屏时用户不知道当前菜单位置，如果第一下点击直接执行确认，可能误触发 Wi-Fi 重置或其他危险操作。因此项目设计为：息屏状态下第一个输入事件只点亮屏幕，不传给 UI 状态机。

---

## 8.18 为什么要缩短临界区持锁时间？

**答：**

ESP32 上 Wi-Fi、TCP/IP、MQTT、Flash IO 都可能涉及较长耗时。如果在锁内执行这些操作，会阻塞其他任务访问共享状态，甚至增加死锁和看门狗风险。本项目临界区只保护简单状态读写，例如 `latest`、`network_ready` 和 `network_generation`，网络请求和日志写入都在锁外执行。

---

## 8.19 如何检查堆和栈余量？

**答：**

Modbus worker 周期调用：

```c
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
uxTaskGetStackHighWaterMark(NULL)
```

记录当前 internal heap、历史最低 internal heap 和任务栈高水位。低于阈值时按 warning 记录，便于排查内存泄漏和栈不足。

---

## 8.20 ESP32 OTA 和 STM32 Bootloader 跳转有什么区别？

**答：**

ESP32 OTA 依赖 ESP-IDF Bootloader、分区表和 otadata。应用下载新固件到另一个 OTA 分区后重启，Bootloader 根据 otadata 选择启动分区。

STM32 Bootloader 则通常需要自己实现：检查 App 地址、设置 MSP、设置 VTOR，然后跳到 App 的 Reset_Handler。回滚、校验、状态区、下载协议也都要自己设计。

---

## 8.21 STM32F103C8T6 为什么暂时不做远程升级？

**答：**

F103C8T6 资源很小：64KB Flash / 20KB RAM。网络模块驱动、TCP 服务端、传感器驱动还未完成前，无法准确评估 Bootloader 和 OTA 的空间预算。双镜像远程升级尤其困难，因为两个 App 加 Bootloader 很可能超过 64KB。因此初期采用 ST-Link + SWD 烧录，Bootloader 和远程升级列为后续扩展。

---

## 8.22 如何验证异常后系统能恢复轮询？

**答：**

项目有 host 单元测试：

```bash
python3 components/modbusTCP/tests/run_tests.py
```

它模拟：连接失败、读超时、Wi-Fi flap、Wi-Fi 恢复、cleanup 失败、backoff reset 等场景，验证 worker 不会无限阻塞，异常后能释放旧资源并恢复轮询。

---

## 8.23 这个项目最有技术含量的点是什么？

**答：**

主要在可靠性工程，而不是单一协议调用：

1. Wi-Fi/Modbus/MQTT/OTA 多状态机协同。
2. 断网、服务器关闭、读超时后的资源释放和恢复。
3. 外部 Flash 离线缓存与 MQTT ACK 一致性。
4. OTA 自检回滚和 NVS 兼容策略。
5. LCD/UI 内存受限设计和息屏误触保护。
6. 看门狗、堆、栈余量监控。
7. host mock 测试覆盖异常路径。

---

## 8.24 如果让你继续优化，会做什么？

**答：**

可以从以下方向继续优化：

1. OTA 增加 manifest：包含版本、sha256、大小、型号、强制升级策略。
2. 增加固件签名校验或 Secure Boot / Flash Encryption。
3. MQTT 后端增加 record_id 去重表和设备状态页面。
4. Modbus 支持多个从站、多寄存器描述和动态配置。
5. LCD UI 增加网络状态、MQTT 状态、OTA 状态页面。
6. 外部 Flash 增加磨损统计和坏块/坏页处理策略。
7. STM32 侧完成最小 TCP server 和传感器驱动后，再评估 Bootloader。

---

## 9. 深度细节问题

### 9.1 如果 MQTT PUBACK 收到了，但 Flash ack 写失败怎么办？

**答：**

项目中将网络 ACK 和 Flash release 分离。PUBACK 表示消息已经被 broker 接收，但 Flash FIFO 的 ack 仍可能失败。设计上应保留 active record 的 ack_received 状态，并允许后续重试 Flash release。这样避免因为 Flash 写失败而重复发布已经 ACK 的数据，也避免直接丢失 FIFO 状态。

---

### 9.2 如果 OTA 下载完成但 `esp_https_ota_finish()` 失败怎么办？

**答：**

`esp_https_ota_finish()` 会做镜像校验和启动分区更新等工作。如果失败，项目不会重启到新固件，而是将 OTA 状态置为 failed，记录 `finish_failed`。当前运行分区继续运行，不会切换。

---

### 9.3 如果新固件自检期间设备断电会怎样？

**答：**

因为启用了 rollback，新固件首次启动处于 `ESP_OTA_IMG_PENDING_VERIFY`。如果还没调用 `esp_ota_mark_app_valid_cancel_rollback()` 就断电或 WDT 复位，Bootloader 下次会认为新固件没有确认成功，从而回滚旧分区。

---

### 9.4 为什么 Modbus cleanup 失败后不再重启 worker？

**答：**

`mbc_master_delete()` 失败可能表示 controller 内部已经部分释放或状态不一致。此时如果继续创建新的 controller，可能造成双重释放、句柄悬挂或底层任务冲突。因此进入 `MODBUS_FAULT`，保留 `worker_requested`，避免重复创建一个更坏的协议栈。生产环境可进一步设计为整机重启或延迟冷重启协议栈。

---

### 9.5 为什么 Wi-Fi 恢复时也要递增 generation？

**答：**

断线时递增 generation 可以让旧请求失效；恢复时再递增可以区分多个快速 GOT_IP 事件或网络重建后的新会话。这样 worker 只接受在同一网络周期内发起并返回的数据，避免状态混淆。

---

### 9.6 为什么 LCD 不使用局部刷新？

**答：**

当前 UI 简单，全屏 framebuffer + 分块 DMA 刷新实现更直接，代码复杂度低。由于 framebuffer 放 PSRAM，DMA buffer 较小，内存可控。后续如果 UI 页面复杂或刷新频繁，可以改成 dirty rectangle 局部刷新，进一步降低 SPI 带宽和功耗。

---

### 9.7 Flash FIFO 和普通文件系统相比有什么优势？

**答：**

FIFO 更适合日志和离线消息这种顺序写、顺序读、按 ACK 删除的场景。相比文件系统，它的元数据更少，恢复逻辑更简单，可控性更高，也更容易保证掉电后从头部继续处理。但缺点是随机访问、复杂查询和空间管理能力弱。

---

### 9.8 为什么 OTA 不直接放在 MQTT 里做？

**答：**

MQTT 可以传升级命令或 manifest，但大固件二进制通过 HTTPS 下载更合适：

- HTTPS/S3/CloudFront 对大文件分发成熟。
- 可利用 HTTP range、CDN、缓存、证书体系。
- MQTT broker 不需要承载大二进制流。
- 设备端可用 ESP-IDF `esp_https_ota` API，减少自定义协议风险。

---

### 9.9 如何判断这个项目是否适合量产？

**答：**

还需要补充：

1. 设备证书批量生产与烧录流程。
2. OTA manifest 与签名校验。
3. 更完整的异常遥测和云端设备状态管理。
4. 外部 Flash 长时间老化测试。
5. Wi-Fi 弱网测试和断电恢复测试。
6. 工厂测试模式。
7. 安全启动和 Flash 加密评估。
8. 版本发布、灰度和回滚策略。

---

### 9.10 如果现场反馈“偶尔不上报”，你怎么排查？

**答：**

排查路径：

1. 看 Wi-Fi 日志：是否频繁 disconnected/lost_ip。
2. 看 MQTT 日志：是否 TLS 失败、broker 断开、PUBACK 超时。
3. 看 Modbus 状态：是否 `MODBUS_BACKOFF` 或 `MODBUS_FAULT`。
4. 看 sample stale：是采集失败还是上报失败。
5. 看 W25Q64 cache：是否 cache full 或 Flash unavailable。
6. 看 heap/stack：是否低内存导致 TLS/MQTT 失败。
7. 看 record_id：云端是否收到但去重/处理失败。
8. 使用历史归档和日志归档恢复现场时间线。

---

## 10. 总结

这个项目不是简单的 ESP32 联网 demo，而是围绕现场可靠性做了系统设计：

- 配网：BLUFI + Wi-Fi 自动恢复
- 采集：Modbus TCP + stale 语义 + 异常重连
- 上报：MQTT TLS + Flash/RAM 离线缓存 + ACK 释放
- 存储：W25Q64 FIFO + 历史与日志归档
- 运维：S3 HTTPS OTA + 双分区 + 自检回滚
- UI：LCD 菜单 + 编码器 + 息屏唤醒
- 可靠性：看门狗、资源监控、NVS 兼容、host 异常测试

面试介绍时可以重点突出：**你不是只会调库，而是围绕网络不稳定、断电、服务器关闭、Flash 不可用、OTA 失败、内存受限这些真实工程问题做了完整闭环设计。**

---

## 11. FreeRTOS 与 ESP-IDF 并发机制补充

本项目大量使用 FreeRTOS/ESP-IDF 的并发能力，包括任务、队列、事件组、临界区、原子变量、软件定时器和任务看门狗。面试时这部分非常容易被追问，因为它体现了嵌入式工程的稳定性设计能力。

### 11.1 任务 Task

项目中的典型任务：

| 任务 | 位置 | 作用 |
|---|---|---|
| `wifi_recovery_task` | `components/wifi/wifi.c` | Wi-Fi 断线后的周期性恢复 |
| `blufi_boot_task` | `components/wifi/wifi.c` | 判断是否启动 BLUFI 配网 |
| `ble_shutdown_task` | `components/wifi/wifi.c` | 配网成功后异步关闭 BLE |
| `modbus_poll_task` | `components/modbusTCP/modbusTCP.c` | Modbus TCP 采集和异常恢复 |
| MQTT worker | `components/mymqtt/mymqtt.c` | MQTT 连接、重连、发布、ACK 处理 |
| storage archive task | `components/w25q64/storage_archive.c` | 异步写历史和日志到外部 Flash |
| `ui_task` | `components/interaction/interaction.c` | LCD 绘制和 UI 状态机 |

设计原则：

1. **耗时操作放任务，不放回调/ISR。**
   - BLE/Wi-Fi/IP 回调只改状态或投递事件。
   - LCD 绘制只在 UI task 中做。
   - Flash 写入由 storage task 做。

2. **网络协议栈和业务 worker 解耦。**
   - Wi-Fi 事件只通知 MQTT/Modbus 网络状态变化。
   - Modbus/MQTT 自己决定重连节奏。

3. **任务只负责一个明确职责。**
   - UI 不做网络。
   - Modbus 不做 MQTT 发布。
   - Storage 不理解业务协议，只写记录。

---

### 11.2 队列 Queue

项目中使用队列的场景：

| 队列 | 作用 |
|---|---|
| UI 输入事件队列 | 编码器采样回调向 UI task 投递旋转/点击/长按事件 |
| MQTT 事件队列 | MQTT event handler 向 worker 投递连接、断开、PUBACK 等事件 |
| Storage 队列 | 业务线程非阻塞提交日志/历史，storage task 异步落盘 |

典型特点：

- 队列用于 **跨上下文传递事件**。
- 生产者不做耗时处理，只投递轻量事件。
- 消费者在 task 中串行处理，避免复杂锁。

UI 中的例子：

```c
if (rotate && xQueueSend(events, &rotate, 0) != pdTRUE) {
    atomic_store(&input_overflow, true);
}
```

这里 `xQueueSend(..., 0)` 表示不阻塞。如果队列满，设置 overflow 标志，UI task 后续清空队列并返回安全菜单，避免输入乱序导致误确认。

队列设计的关键点：

1. **队列长度不是越大越好。**
   - UI 队列从 32 缩小到 16，减少常驻内存。
   - 输入事件应该快速消费，队列过大反而可能积累过期操作。

2. **队列满要有降级策略。**
   - UI：丢弃并回主菜单。
   - Storage：记录 dropped 统计。
   - MQTT：记录 overflow，worker 做状态修正。

3. **不要在 ISR 或 timer callback 里做复杂逻辑。**
   - 编码器采样 timer 只计算事件并投队列，不画屏、不写 Flash。

---

### 11.3 事件组 EventGroup

Wi-Fi/BLUFI 中使用 EventGroup 表示多个异步状态位：

```c
CONNECTED_BIT
BLE_SHUTDOWN_REQUESTED_BIT
BLE_DISCONNECTED_BIT
BLUFI_DEINITIALIZED_BIT
WIFI_AUTO_RECONNECT_BIT
WIFI_ASSOCIATED_BIT
```

适合 EventGroup 的场景：

- 多个 bool 状态组合。
- 一个任务等待某个状态位出现。
- 多个事件来源共同改变状态。

例如 BLUFI 关闭流程：

```text
GOT_IP
  ↓
设置 BLE_SHUTDOWN_REQUESTED_BIT
  ↓
ble_shutdown_task 等待该 bit
  ↓
停止广播、断开连接、deinit profile、deinit host/controller
```

为什么这里适合事件组：

- BLE 关闭涉及多个异步回调。
- 任务需要等待状态完成，例如 `BLUFI_DEINITIALIZED_BIT`。
- 使用 bit 可以避免多个全局 bool 的竞态混乱。

---

### 11.4 优先级 Priority

项目中任务优先级设计思路：

- Wi-Fi/IP 协议栈任务由 ESP-IDF 管理，优先保证系统网络栈。
- Modbus worker 优先级低于 esp-modbus 内部任务：

```c
#define MB_WORKER_PRIORITY (CONFIG_FMB_PORT_TASK_PRIO - 1)
```

并通过静态断言保证：

```c
_Static_assert(CONFIG_FMB_PORT_TASK_PRIO > 1, ...);
```

目的：

1. 防止业务 worker 抢占底层 Modbus TCP 任务。
2. 避免 stop/delete 时与底层任务调度产生死锁风险。
3. 保证网络协议栈有机会及时处理 socket 事件。

UI task 优先级较低，因为 UI 不是实时关键路径。Storage task 也不应抢占网络关键路径，否则可能因为 Flash IO 影响 MQTT/TCP。

---

### 11.5 临界区与锁

项目中使用：

```c
portENTER_CRITICAL(&sample_lock);
portEXIT_CRITICAL(&sample_lock);
```

保护共享状态，例如：

- `network_ready`
- `network_generation`
- `latest` 采样快照

设计重点：**锁内只做简单内存读写，不做耗时操作。**

错误示例：

```c
portENTER_CRITICAL(&lock);
mbc_master_get_parameter(...);   // 错：网络 IO 不应在锁内
storage_log(...);                // 错：可能排队或触发复杂逻辑
portEXIT_CRITICAL(&lock);
```

正确做法：

```c
portENTER_CRITICAL(&lock);
copy = latest;
portEXIT_CRITICAL(&lock);

storage_log(...); // 锁外做
```

这样能降低：

- 死锁概率
- 中断延迟
- 任务阻塞时间
- 看门狗风险

---

### 11.6 原子变量 Atomic

项目中使用 C11 atomic，例如：

```c
static atomic_bool worker_requested;
static atomic_bool input_overflow;
```

适用场景：

- 单个布尔/计数状态。
- 不需要保护复杂结构。
- 避免为了一个 flag 使用互斥锁。

例如 Modbus worker 防重复创建：

```c
bool expected = false;
if (!atomic_compare_exchange_strong(&worker_requested, &expected, true)) {
    return ESP_OK;
}
```

这保证即使多次 got IP 或重复 init，也只创建一个 worker。

---

### 11.7 软件定时器 esp_timer

编码器使用 `esp_timer` 1ms 周期采样：

```c
esp_timer_start_periodic(input_timer, 1000);
```

注意：`esp_timer` 回调运行在 timer task，不是普通业务 task，也不是 ISR。项目中仍然遵循：

- 不画屏
- 不写 Flash
- 不阻塞
- 只读 GPIO、计算输入状态、投递队列

---

### 11.8 Task Watchdog

Modbus worker 注册任务看门狗：

```c
esp_task_wdt_add(NULL);
```

循环和等待中定期：

```c
esp_task_wdt_reset();
```

为什么 backoff 不能直接 `vTaskDelay(30000)`？

如果任务已加入 WDT，长时间不 reset 可能导致 WDT 报警。因此项目把长 backoff 拆成短 slice，既能喂狗，也能更快响应网络状态变化。

---

## 12. 技术栈面试题补充

### 12.1 FreeRTOS 队列和事件组有什么区别？

**答：**

队列用于传递数据或事件对象，强调“一个个消息”；事件组用于表示状态位，强调“状态是否发生”。

例如：

- 编码器点击、旋转事件适合队列，因为每个动作都要处理。
- Wi-Fi connected、BLE shutdown requested 适合事件组，因为它们是状态。

队列保留顺序，事件组不保留发生次数。连续设置同一个 bit，消费者只知道 bit 为 1，不知道发生了几次。

---

### 12.2 为什么 UI 输入用队列而不是事件组？

**答：**

UI 输入是有顺序和次数的：旋转三格、点击一次、长按一次，每个事件都要被状态机处理。如果用事件组，多个旋转事件会合并成一个 bit，无法表示次数和顺序。因此使用队列。

---

### 12.3 为什么 Wi-Fi 状态用事件组？

**答：**

Wi-Fi/BLUFI 状态更像状态位，例如是否 connected、是否请求关闭 BLE、BLE 是否已断开。这些状态不关心发生次数，而关心当前是否成立。事件组可以让任务等待多个条件组合，代码更清晰。

---

### 12.4 什么情况下队列会导致内存问题？

**答：**

队列创建时会一次性分配存储空间，长度和元素大小越大，占用越多。如果队列里放大结构体或大 buffer，会浪费 RAM。项目中队列只传递小事件或拷贝后的紧凑记录，大数据如 LCD framebuffer 不通过队列传输。

---

### 12.5 队列满了怎么办？

**答：**

取决于业务重要性：

- UI 输入：可以丢弃，回到安全菜单，避免误操作。
- 日志：可以统计 dropped，保证主业务不阻塞。
- MQTT ACK：不能随便丢，需要 worker 检测 overflow 并重建状态。

项目里 UI 队列满时设置 `input_overflow`，UI task 清队列并回主菜单。

---

### 12.6 FreeRTOS 任务优先级如何设计？

**答：**

原则：实时性越高、阻塞越短的任务优先级越高；耗时 IO 和非关键 UI 优先级较低。项目中 Modbus worker 优先级低于 esp-modbus 内部任务，避免业务任务抢占底层协议处理。UI 优先级低，避免影响 Wi-Fi/MQTT/TCP。

---

### 12.7 什么是优先级反转？这个项目如何避免？

**答：**

优先级反转是高优先级任务等待低优先级任务持有的锁，而中优先级任务又抢占低优先级任务，导致高优先级任务长时间得不到执行。

项目避免方式：

1. 临界区极短。
2. 锁内不做网络 IO、Flash IO、日志写入。
3. 尽量用队列把工作转移到单 owner task，减少多任务争锁。

---

### 12.8 为什么不能在 Wi-Fi 事件回调里直接做 OTA 或 Flash 写？

**答：**

Wi-Fi/IP 事件回调运行在系统事件循环上下文。如果在里面做 HTTPS OTA、Flash 写入或长时间阻塞，会阻塞事件循环，影响 Wi-Fi、IP、MQTT 等其他事件处理。项目只在回调中启动任务或通知模块，耗时逻辑放 worker task。

---

### 12.9 `vTaskDelay()` 和阻塞式 socket read 有什么区别？

**答：**

`vTaskDelay()` 会让出 CPU，其他任务可以运行；阻塞式 socket read 如果超时时间过长，虽然通常也会阻塞当前任务，但可能导致业务状态迟迟不能响应网络变化。项目通过配置连接超时、响应超时，并在业务层用 generation 和分片等待降低影响。

---

### 12.10 为什么 storage 使用异步队列写 Flash？

**答：**

Flash 写入可能耗时较长，并可能涉及擦除。如果业务任务直接写 Flash，会阻塞 MQTT/Modbus/Wi-Fi 关键路径。异步队列可以让业务任务快速返回，由 storage task 串行写 Flash，同时也避免多任务同时访问外部 Flash。

---

### 12.11 为什么 LCD 绘制必须单任务 owner？

**答：**

LCD panel、SPI DMA buffer 和 framebuffer 都是共享资源。如果多个任务同时画屏或 present，可能导致 DMA buffer 被覆盖、SPI transaction 交叉、画面撕裂甚至崩溃。项目让 UI task 独占所有绘制操作，其他模块只能通过 `interaction_notify_status_change()` 发轻量事件。

---

### 12.12 ESP-IDF event loop 和 FreeRTOS queue 有什么关系？

**答：**

ESP-IDF event loop 本质上也是基于任务和事件队列的事件派发机制。Wi-Fi/IP/MQTT 等组件通过 event loop 回调通知应用。应用如果要做耗时处理，应从 event handler 里投递到自己的 FreeRTOS queue 或启动 worker，而不是直接在 event handler 中执行。

---

### 12.13 如何判断任务栈是否够？

**答：**

使用：

```c
uxTaskGetStackHighWaterMark(NULL)
```

它返回任务运行以来剩余栈空间的最低水位。若该值长期接近 0，说明栈可能溢出。项目中 Modbus worker 周期记录 stack high-water mark，低于阈值按 WARN 记录。

---

### 12.14 internal heap 和 PSRAM 的区别是什么？

**答：**

internal heap 在芯片内部 SRAM，速度快，很多 DMA、Wi-Fi、TLS 结构需要内部 RAM。PSRAM 容量大但速度较慢，也不一定适合所有 DMA。项目把大 framebuffer 放 PSRAM，把 DMA buffer 放内部 DMA-capable memory，避免挤占关键内部 RAM。

---

### 12.15 为什么 DMA buffer 不能放普通 malloc 的内存？

**答：**

SPI DMA 需要满足地址和 capability 要求，必须分配 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` 的内存。普通 malloc 可能返回 PSRAM 或不满足 DMA 对齐/可访问要求，导致 SPI DMA 失败或数据异常。

---

### 12.16 MQTT 和 Modbus 为什么不放在同一个任务里？

**答：**

两者生命周期和阻塞特性不同。Modbus 是采集侧，有连接/读取/退避；MQTT 是上报侧，有 TLS、broker、PUBACK、离线缓存。如果放同一个任务，一个协议阻塞会影响另一个协议。分任务后可以独立恢复、独立监控、独立限速。

---

### 12.17 为什么 Modbus 的最新值缓存要用快照而不是直接每次读设备？

**答：**

直接每次读设备会让调用者阻塞在网络 IO 上，并可能造成多处并发访问同一个 Modbus controller。项目使用采集 worker 周期更新缓存，其他模块只读快照，保证非阻塞、可控、线程安全。

---

### 12.18 为什么 OTA 状态要独立维护？

**答：**

OTA 是长流程，包含 checking、downloading、updated、failed 等阶段。独立状态便于 UI、日志或云端查询，也方便失败时定位是 URL、证书、版本、型号、下载还是 finish 阶段出错。

---

## 13. 场景题：现场设备运行一周后偶发停止上报，但 LCD 还能操作，如何定位？

### 题目

现场反馈：设备运行一周后，LCD 菜单还能响应，Wi-Fi 图标偶尔正常，但云端收不到数据。重启后恢复。请你基于本项目设计，给出排查思路和可能修复方向。

### 参考答案

首先要区分是 **采集失败**、**上报失败**、**网络失败** 还是 **存储/资源问题**。

#### 第一步：看 Wi-Fi 状态

检查历史日志中是否有：

- `wifi/disconnected`
- `wifi/lost_ip`
- `wifi/reconnect_attempt`
- `wifi/got_ip`

如果频繁断线，说明是网络侧问题。继续看 Wi-Fi 恢复后是否重新触发：

- `modbusTCP_init()`
- `mqtt_start()`

如果 got IP 后没有恢复，可能是状态机卡住或 worker 已进入 fault。

#### 第二步：看 Modbus 状态

检查日志：

- `modbus/connect_failed`
- `modbus/read_failed_releasing`
- `modbus/acquisition_failed`
- `modbus/acquisition_recovered`
- `modbus/controller_cleanup_failed`

如果 Modbus 一直在 BACKOFF，说明采集侧有问题，比如从站关闭、IP 变了、服务器端口不可达。项目会释放旧 socket 并重连，如果一直不能恢复，应检查从站和网络链路。

如果出现 `controller_cleanup_failed`，说明 esp-modbus controller 清理失败，当前设计会进入 `MODBUS_FAULT`，避免继续复用坏状态。修复方向可以是：在 fault 后触发整机软重启，或实现更强的协议栈冷重启流程。

#### 第三步：看 MQTT 状态

检查：

- TLS 是否失败
- broker 是否断开
- PUBACK 是否超时
- MQTT outbox 是否满
- `cache_full`
- `flash_cache_failed`

如果 Modbus 有新数据但 MQTT 不上报，多半是 MQTT/TLS/broker 或缓存满。

#### 第四步：看 W25Q64 外部 Flash

如果网络长期不可用，MQTT 缓存会增长。检查：

- Flash cache 是否 ready
- FIFO 是否 full/exhausted
- RAM fallback 是否用尽
- storage archive 是否有 write error

如果 Flash 不可用，设备会降级到 RAM cache，但 RAM cache 容量有限，最终会拒绝新记录。

#### 第五步：看资源余量

检查 Modbus 资源日志：

```text
heap=xxx min=xxx stack_hwm=xxx
```

如果 internal heap minimum 持续下降，可能存在内存泄漏。如果 stack high-water mark 很低，可能任务栈不足。

#### 第六步：判断为什么重启能恢复

重启能恢复通常说明：

1. 某个协议栈进入不可恢复状态。
2. socket/controller 资源泄漏。
3. heap 碎片或泄漏。
4. 外部设备/服务器断开后客户端状态机没有完全重建。

本项目已经针对 Modbus 读失败释放 controller/socket，并周期记录资源。如果仍出现该问题，下一步可以：

- 在 `MODBUS_FAULT` 时请求系统重启。
- MQTT 多次重连失败后重建 client handle。
- 增加云端心跳状态和本地故障码。
- 增加外部 Flash cache 使用率上报。
- 做 72 小时弱网压测，包括 AP 重启、服务器重启、broker 断连、Flash 拔除/异常模拟。

#### 总结

面试回答时重点强调：不要直接说“重启解决”，而是先分层定位：

```text
Wi-Fi → Modbus 采集 → MQTT 上报 → Flash 缓存 → heap/stack 资源 → 状态机恢复
```

然后用项目已有日志和状态字段逐步缩小范围，最后给出工程修复策略。

