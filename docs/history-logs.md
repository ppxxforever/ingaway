# 历史记录、分级日志与时间基准

## 分区与启动

`components/w25q64/storage_archive.c` 使用 `ext_history`（6 MiB，data subtype 0x41）与 `ext_logs`（1 MiB，data subtype 0x42）。两者为原始记录环，不是 SPIFFS，不能混用文件系统。内部 `partitions.csv` 不变；外置分区由 `w25q64_init()` 注册。

`main/ingaway.c` 在 Wi-Fi 启动前调用 `storage_archive_start()`。唯一 `archive` 任务负责两个环的扫描、写入及循环淘汰，采集任务与网络回调只复制数据到队列，不访问 Flash。MQTT 待确认缓存仍由 MQTT 任务管理，ESP-IDF Flash 驱动序列化同一芯片的底层操作。两个环的读取与写入使用同一 mutex；禁止绕过服务直接写这些分区。

历史、日志各有独立的 24 条 RAM 提交队列。提交接口返回 `ESP_OK` 只代表已入队，**不代表已落盘**；队列满拒绝新记录并累计丢弃计数，不阻塞采集或网络回调。写入失败时任务保留当前待写记录，每 5 秒尝试扫描恢复，其他队列可能继续积压。断电会丢失尚未落盘的队列记录；不声称每个采样都必定持久化。

## 历史内容

历史保存实际采集结果，而不是每轮 MQTT 上传时重复保存缓存：

- 每次成功且网络代次仍有效的 Modbus 响应：保存原始 U16 温度、寄存器类型、有效与过期标志、采集状态、错误码、最后成功采样的单调毫秒。
- 每轮有限重试全部失败：保存状态与错误快照。存在历史值时保留数值，但明确 `stale=true`；不能把它当作本轮成功采样。
- Wi-Fi 断开或丢失 IP：保存失效快照。
- 控制器清理失败：保存故障快照。

成功响应快照在更新采集缓存的同一临界区内复制，随后在临界区外提交，避免快速断网篡改已经接受的成功响应内容。记录头具有 64 位物理 ID、长度和头/负载 CRC32，提交标记最后写入，沿用现有 `flash_fifo` 的恢复规则。

JSON 公共字段为 `version`、`boot_id`、`event_seq`、`mono_us`、`utc_us`、`utc_valid`、`time_epoch`、`sync_age_us`；历史额外包含 `kind=modbus`、`temperature_raw`、`type`、`has_value`、`stale`、`sample_mono_ms`、`state`、`error`。`sample_mono_ms` 是此启动内最后成功采样时刻，没有成功值时为 `null`。

## 分级事件日志

等级：0 Debug、1 Info、2 Warn、3 Error。menuconfig → **External W25Q64 storage** → **Minimum persistent log level**，默认 1 Info。低于阈值的事件不入队、不写 Flash。这里记录的是显式结构化事件，不是全局拦截 `ESP_LOG`；避免日志递归以及误存证书、口令等敏感内容。

- **Debug**：连接尝试、RAM→Flash 迁移、发送入队及背压、Flash 释放完成/恢复。
- **Info**：启动、Wi-Fi 关联/获取 IP、MQTT 连接、采集恢复、SNTP 时间锚点、Flash/RAM 补传收到匹配 PUBACK。
- **Warn**：Wi-Fi/MQTT 断开、IP 丢失、采集失败及退避、心跳失败、连接超时、PUBACK 超时、缓存满、Flash 缓存故障与确认失败、提交队列溢出。
- **Error**：控制器清理失败、任务创建失败、MQTT TLS/传输或停止/销毁异常、历史写入失败、历史/日志扫描恢复失败。

日志额外包含 `kind=log`、`level`、`source`、`event`、`error`、`record_id`、`detail`。`record_id` 是事件关联值，不是日志自身的物理 ID：MQTT 补传事件中为 Flash 物理 ID 或 RAM 序号，采集退避事件中为退避毫秒（detail 明确说明）。PUBACK、发送入队、PUBACK 超时的 detail 包含 `msg_id` 和业务 `wire_id`，便于关联补传结果。

PUBACK 只表示 Broker 已接收。Flash 确认写入失败另记 Warn；Debug 级记录本地确认释放结果。日志本身不随 MQTT PUBACK 删除，不把“收到 PUBACK”当作订阅端业务落库成功。

## 时间基准（确定规则）

1. **启动身份**：Wi-Fi MAC 加 NVS `archive_meta/boot_seq` 的 64 位递增序号。启动服务时先提交 NVS 序号，再启用记录，每个启动仅写一次 NVS。序号耗尽、NVS 错误时不启动存档服务，不冒用旧启动 ID。人工清空 NVS 会重置序号，应视为设备记录代次重置。
2. **主时间轴**：`mono_us` 来自 `esp_timer_get_time()`，是开机后的单调微秒，在事件提交时捕获，不是晚些时候写 Flash 的时间。事件 ID 使用 `(boot_id, event_seq)`；事件序号跨历史和日志队列共享，入队失败可能留下间隙。
3. **尚未同步**：`utc_valid=false`、`utc_us=null`、`sync_age_us=null`、`time_epoch=0`。不能用上电默认时间或仅通过 TLS 的日期合理性检查推断真实 UTC。
4. **显式 SNTP 同步后**：MQTT 初始化 SNTP 时注册 `storage_clock_synced()`，回调保存 UTC 微秒与同时捕获的单调时间锚点。UTC 接受范围为 2024～2100 年；之后记录的 `utc_us = anchor_utc + (mono_us - anchor_mono)`，`sync_age_us` 表示距锚点时长，是最近同步值的单调外推，不是持续读取可能跳变的系统墙钟。
5. **再次校时**：每次合法同步递增 `time_epoch` 并写 Info 日志 `sntp_sync`。每条记录保存当时的代次；UTC 可能前后跳变，排序应优先使用启动序号、单调时间和事件序号。老记录不回填或改写 UTC；如需离线估算，可基于同一 boot_id 的时间锚点推算并标记为估算值。
6. **精度边界**：SNTP 为网络时间，不是受认证的高精度授时；存在网络、回调延迟以及两次同步间的振荡器漂移。`sync_age_us` 不表示误差界限。整机重启后重新同步；跨启动仅有 UTC 有效时可直接比较绝对日期。

即使保留的系统墙钟已通过 TLS 合理性检查，MQTT 仍初始化 SNTP，以获得显式存档时间锚点。若 SNTP 暂不可达，记录继续保留启动身份与单调时间。

## 容量、恢复与读取

- 一扇区一条记录：历史最多 1536 条，日志最多 256 条。保留时长取决于实际采样/事件频率，不保证固定天数。例如历史每 5 秒一条时约 128 分钟，异常记录会缩短此时间。日志每条都消耗一个扇区，频繁 Info/Debug 会缩短保留时长并增加擦写；后续可升级为批量格式。
- 与 MQTT 缓存不同，历史和日志是**有限保留窗口**。满时先持久化淘汰标记，再回收最旧记录，写入新记录；最新 ID 的高水位扇区仍保留，避免正常擦除中断造成 ID 重用。断电可能发生在淘汰完成但新记录尚未提交之间。
- 扫描只接受完整提交且 CRC 正确的记录，读写位置自动恢复。未知格式/撕裂头隔离，损坏记录停止对应环并告警，不自动格式化。反复断电可能减少可用扇区，需要明确维护。
- 不确定写入结果重试可能产生内容重复，但其 `(boot_id, event_seq)` 不变；导出端可据此去重。物理 ID 及事件序号允许有间隙，不用它们推断连续采样。
- `storage_archive_read(kind, after, buffer, capacity, &len, &id)` 返回 `FF_*`，读取保留窗口内最小的 `id > after`，不消费记录。`after=0` 从最旧记录开始；旧 cursor 已被淘汰时从当前最旧记录继续。历史和日志物理 ID 分别计数。读者需用返回 ID 更新 cursor。
- 缓冲区返回 JSON 字节及准确长度，不自动追加 NUL。服务仍在初始扫描或故障恢复时读取返回 `FF_IO`，参数/容量不足返回 `FF_ARGUMENT`，无更新返回 `FF_EMPTY`。该接口会等待 mutex，不能在网络/中断回调中调用。
- `storage_archive_get_stats()` 返回本次启动的队列丢弃数、淘汰数、写入失败数和两个环的 ready 状态。队列丢弃变化每 10 秒汇总到串口，并尝试写入 Warn 日志；日志分区不可写或队列满时，持久化告警不保证成功。

按要求未新增测试程序，仅进行固件编译检查；未烧录、格式化或擦除任何硬件。实际上板仍需确认历史轮转、日志阈值、初始无时间/SNTP 校准及各阶段断电行为。
