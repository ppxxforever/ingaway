# ESP32-S3 N16R8 分区方案

N16=16 MiB Flash；R8=8 MiB PSRAM。PSRAM 是运行内存，不用于持久分区。

分区源文件为根目录 `partitions.csv`，`sdkconfig` 已选择自定义分区；`sdkconfig.defaults` 保留 Flash 大小与分区选择，避免重新生成配置时回到 1 MiB 单应用布局。

| 分区 | 起始地址 | 大小 | 用途 |
| --- | --- | --- | --- |
| nvs | 0x9000 | 52 KiB | Wi-Fi/MQTT 配置、Modbus 地址/波特率/映射参数、传感器校准 |
| otadata | 0x16000 | 8 KiB | OTA 启动槽选择，系统管理 |
| phy_init | 0x18000 | 4 KiB | 预留 PHY 初始化数据，是否使用取决于配置 |
| ota_0 | 0x20000 | 4 MiB | 第一个应用槽 |
| ota_1 | 0x420000 | 4 MiB | 第二个应用槽 |
| storage | 0x820000 | 6 MiB | 预留 SPIFFS 文件存储：配置文件、日志、离线数据等 |
| coredump | 0xE20000 | 128 KiB | 预留崩溃转储，须另行启用 Flash core dump |
| 未分配 | 0xE40000 | 1.75 MiB | 后续独立数据分区或其他扩展 |

应用分区必须 64 KiB 对齐。应用槽结束后分别衔接到下一分区，无重叠，末端不超过 16 MiB。

## OTA 规划

- 不单独保留 factory 分区，避免占用第三份固件空间。
- 全新部署、otadata 擦除时默认启动 ota_0。
- 后续 OTA 将固件写入非运行槽，校验成功后更新 otadata 并重启。
- 单份固件必须小于等于 4 MiB；若启用安全启动等特性，需计入相关开销。
- 当前仅完成分区布局，尚未添加 OTA 下载、鉴权、签名验证或回滚逻辑。
- 自动回滚需要单独开启 bootloader 回滚配置，并在首次启动自检成功后调用 `esp_ota_mark_app_valid_cancel_rollback()`；不能仅开启选项而不处理首次启动确认。
- 回滚自检应围绕关键系统能力设计，避免仅因暂时断网或可选传感器缺失误判失败。

## Modbus / 存储规划

Modbus 驱动和业务逻辑属于应用固件，不需要独立 Flash 分区。低频更新的参数使用 NVS。大量日志/历史采样放文件存储，不要每次测量都写 NVS。

采用内置 Flash + W25Q64 的组合后，大量历史数据、离线队列与日志优先放外置 W25Q64，详见 `docs/external-storage.md`。内置 storage 留作备用和配置文件，不改变已有 OTA 布局。

storage 当前只是预留，还未挂载或格式化。后续使用 SPIFFS 时按 label `storage` 挂载；数据存储应设置限额、循环淘汰和批量写入，减少 Flash 擦写。OTA 切换不会主动擦除这些数据，但新固件须兼容旧版配置格式。

## 首次迁移与烧录

从原来的 factory 单应用布局切换到本布局时，需要用有线方式烧录新的 bootloader、分区表和应用，不能只 OTA 或只烧应用文件。原 nvs 从 24 KiB 扩大到 52 KiB，旧数据不保证可直接沿用。

首次部署建议先导出需保留的配置，再执行：

```sh
idf.py erase-flash  # 警告：清除整个 Flash，包括配网、配置和用户数据
idf.py flash monitor
```

这些烧录/擦除命令没有在本次修改中执行。后续日常 OTA 更新只更换应用，不要再次 erase-flash。若已有部署设备，以后修改分区大小/地址也需专门迁移方案。
