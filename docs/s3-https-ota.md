# ESP32-S3 HTTPS OTA 自检回滚

## 当前策略

- 目标芯片：ESP32-S3。
- Flash 要求：默认 16MiB，运行时通过 `esp_flash_get_size()` 校验。
- 分区要求：必须存在两个 OTA app 分区，当前 `partitions.csv` 包含 `ota_0` 和 `ota_1`，每个 4MiB。
- 回滚：启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`。
- HTTPS 地址：通过 `CONFIG_INGAWAY_OTA_URL` 配置 S3/CloudFront 直链 `.bin`。
- 固件型号：候选镜像 `esp_app_desc.project_name` 必须匹配 `CONFIG_INGAWAY_OTA_PROJECT_NAME`，默认 `ingaway`。

## 自检与标记有效

新固件 OTA 写入完成后重启，Bootloader 会把新 app 置为 `ESP_OTA_IMG_PENDING_VERIFY`。

应用启动后不会立即标记有效。只有当 Wi-Fi 获取 IP 后触发 `ota_service_start_once()`，并通过以下自检，才调用：

```c
esp_ota_mark_app_valid_cancel_rollback();
```

自检项：

1. 当前目标为 ESP32-S3。
2. Flash 容量不少于配置值。
3. 存在双 OTA app 分区。
4. 当前运行分区和下一更新分区均为有效 OTA 分区。
5. 当前运行镜像 `project_name` 与配置型号一致。
6. 已进入 Wi-Fi got IP 路径，作为首期网络可用自检门槛。

## 失败恢复旧分区

如果 pending verify 状态下自检失败，会记录日志并调用：

```c
esp_ota_mark_app_invalid_rollback_and_reboot();
```

Bootloader 将恢复到上一个有效 OTA 分区。若 pending app 在首次启动期间掉电或 WDT 复位，Bootloader 也会回滚。

## NVS 格式兼容约束

为保证回滚可用：

- OTA 自检阶段不做 NVS schema 迁移。
- 新固件不得在标记 app valid 前写入与旧固件不兼容的 NVS 数据格式。
- 如后续确需修改 NVS schema，应采用向后兼容字段或延迟迁移：先通过自检并确认新固件稳定，再执行可回退的数据迁移策略。
- Wi-Fi 凭据、`wifi_boot` 等现有 namespace 需保持字段兼容，避免回滚旧固件后无法联网。

## 发布检查

1. 替换 `CONFIG_INGAWAY_OTA_URL` 为真实 S3/CloudFront HTTPS 固件 URL。
2. 确认固件版本号与线上版本不同。
3. 确认 `project_name=ingaway` 或同步更新 `CONFIG_INGAWAY_OTA_PROJECT_NAME`。
4. 构建后确认 `ingaway.bin` 小于最小 OTA app 分区大小。
5. 首次 OTA 后观察日志：`self_test` -> `app_valid`。
