# 五秒上传、TLS 与断线恢复

当前 Broker 配置为 `mqtts://10.30.192.176:8883`，保留用户修改后的地址。通过 menuconfig 的 **Ingaway MQTT upload** 设置 URI、端口、主题及 NTP 服务器；直接修改 `mymqtt.c` 不再是修改 Broker 地址的入口。URI 必须以 `mqtts://` 开头。

## 上传行为

首次获取 IP 后启动唯一 MQTT 工作任务，此后约每 5 秒生成 Modbus 快照并缓存，包括 MQTT/Wi-Fi 离线期间。重连优先补发未确认记录，QoS 1、不保留；有历史积压时到达服务端的节奏不等于采样节奏。上传不会再次执行 Modbus 网络读取。开机从未获取 IP 时工作任务尚未启动，不生成离线快照。示例：

```json
{"record_id":"0123456789abcdef0123456789abcdef-0000000000000001","device_id":"esp32-aabbccddeeff","uptime_ms":20000,"temperature_raw":250,"has_value":true,"stale":false,"sample_age_ms":1000,"sample_uptime_ms":19000,"modbus_state":2,"modbus_error":0}
```

- `temperature_raw` 是未经倍率换算的寄存器值；尚无采样时为 JSON `null`。
- `has_value=false` 表示从未成功采样；`stale=true` 表示历史值已过期。断开 Modbus 不影响 MQTT 状态上报。
- 时间为开机后的单调毫秒，不是 Unix 时间。数据年龄和过期标记描述生成消息时的快照；接收端还应考虑网络传输延迟，不能把迟到的 QoS 消息当成即时测量。
- 新记录优先写 W25Q64 Flash；Flash 满或不可用时使用最多 16 条 RAM 记录兜底。两者均满则拒绝新记录并告警，不覆盖旧记录，不因缓存满反复重连。RAM 中的记录尚未迁移到 Flash 时不具备断电持久性。
- 重连补发 Flash 最旧记录，再处理 RAM 最旧记录；有 RAM 积压时新记录不会绕过 RAM 写入 Flash。Flash 恢复容量后先持久化 RAM 最旧记录再发送。
- 设备标识由 Wi-Fi MAC 生成，多设备共用主题时可据此区分。

## 独立温度主题

新增 `env/temp`（menuconfig → Ingaway MQTT upload → Temperature publish topic）。与遥测使用同一份 Modbus 快照，连接后立即发送，此后每 5 秒发送，QoS 1、不保留，复用同一 TLS 连接。

- **协议变更**：为支持逐条去重，不再发送纯数字；现在发送 JSON，例如 `{"record_id":"0123456789abcdef0123456789abcdef-0000000000000002","device_id":"esp32-aabbccddeeff","temperature_raw":250}`。`temperature_raw` 是未经倍率换算的 U16 寄存器值，不应直接认定为 250°C。订阅端须更新解析逻辑。
- 尚无采样或采样过期：JSON 中的 `temperature_raw` 为 `null`，详细故障及数据年龄查看 `env/telemetry`。
- MQTT 主题不需要在 Broker 预先创建；设备发布后，订阅 `env/temp` 即可接收。若启用了 ACL，需要允许设备发布及客户端订阅该主题。
- MQTTX 使用原有 CA、客户端证书和私钥连接 `10.30.192.176:8883`，订阅 `env/temp`，等待下一次上报；非 retained 消息不会在订阅瞬间补发。

## MQTT 重连

所有 start/stop/destroy 和 Flash 操作都由唯一工作任务执行，事件回调只入队，不阻塞。重连创建新的客户端实例，重复 GOT_IP 不会重复创建工作任务。

- 连接尝试超过 15 秒、明确断开、网络代次变化、事件队列溢出或数据 PUBACK 等待超过 30 秒：停止并销毁旧客户端，至少等待 10 秒，Wi-Fi 可用时重试。数据入队背压不释放记录，也不重复入队已在途记录。
- 每 10 秒向 `env/heartbeat` 发送 `alive`（QoS 1）。必须收到对应 PUBACK 才算心跳成功；普通数据 ACK 和已超时心跳的迟到 ACK 不算。
- 连续三次心跳超时/入队失败，主动停止连接并重连。Keepalive 另设为 30 秒。
- 停止旧连接后清空旧事件和心跳状态，避免旧会话的 ACK 干扰新连接。
- 底层网络操作超时设为 5 秒；实际 stop/start 耗时受库和调度影响。停止失败进入故障状态，避免重复操作未停止的客户端，需要检查日志并重启设备。

## 消息身份、PUBACK 与去重

- `MQTT msg_id` 是会话内 packet ID；Flash 64 位 ID 是存储位置身份；JSON `record_id` 是消息的业务身份。RAM/Flash 使用同一个数据发送状态机，每次最多一条数据在途，记录来源、存储 ID、msg_id 和 ACK 截止时间。心跳单独处理，不释放数据缓存。
- 入队成功并不等于发送完成。只有匹配当前在途 msg_id 的 `MQTT_EVENT_PUBLISHED`（QoS 1 PUBACK）才释放对应 RAM 记录，或写入 Flash 确认标记。无关 ACK 不消费缓存。
- Flash 确认写入失败时，保留“已收到 ACK、待持久化释放”状态；定期重新扫描，并再次写确认标记，不在同一启动期间额外发布该记录。若扫描发现该 ID 已确认，则结束释放。断电后该状态丢失，未完成确认的记录会重发。
- 同一连接内只由 ESP-MQTT outbox 重传。重连先 stop/destroy 清掉旧 outbox，再清旧事件及会话映射，从缓存重新发送；不会同时使用旧 outbox 和 Flash 两条补发路径。
- 新记录的 `record_id` 为 128 位随机启动 nonce 加 64 位递增序号（十六进制字符串），生成一次后作为负载持久保存。序号不回绕，跨启动采用新的随机 nonce；跨启动唯一性依赖随机 nonce 不碰撞。重传、RAM→Flash 迁移及断电恢复均保持原 ID。MAC 派生的 `device_id` 区分设备。
- 老格式缓存没有业务 ID时，发送时使用稳定的 `flash-<64位Flash ID>`，无需擦除旧数据。人为擦除/格式化会重置旧格式 Flash 序号，服务端需要重置相应去重域或引入设备代次。
- 接收端应以 **`(device_id, record_id)`** 建立持久化唯一键，并在同一数据库事务中完成“去重判断 + 业务写入”。重复 ID 忽略业务写入；不要按 packet ID、消息到达时间或温度值去重。两个主题的记录各有独立 ID。
- PUBACK 表示 Broker 接收消息，不表示订阅者业务落库。此实现提供 QoS 1/至少一次发送与稳定去重键，不声称 exactly-once；如需业务落库后才释放，应另外实现应用层 ACK 协议。
- Flash IO/格式错误保留数据、每 5 秒尝试恢复，不自动格式化；RAM 兜底不能替代 Flash 的断电保证。确认释放失败会阻塞数据发送，避免错误确认下一条记录。两主题快照独立缓存，不保证整批原子接收。

按要求未新增测试程序；已完成 ESP-IDF 编译检查，仍需实机验证 PUBACK、断网及断电行为。历史主机测试脚本不代表本次新增持久化路径已获验证。

## Wi-Fi 恢复

开机显式使用 `WIFI_STORAGE_FLASH`，读取 NVS 中 STA 配置：有 SSID 时先连接历史 Wi-Fi，30 秒内获得 IP 则不初始化 BLE；无配置或超时才启动 BLUFI。保存网络的启动重试和运行中恢复均约每 10 秒尝试连接；已关联但持续 30 秒没有 IP 时重新关联。BLUFI 超时兜底期间仍保留旧网络恢复能力，手机明确提交新配置后由新连接流程接管；手机要求断开时关闭自动恢复。

BLUFI 的 SSID/密码/BSSID 先暂存在 RAM，收到“连接 AP”请求时一次性提交到 Wi-Fi NVS 配置，检查保存错误，不输出密码。配网成功后仍关闭 BLE。本地 `wifi_reprovision()` 通过 NVS 一次性标志重启进入 BLUFI，不删除旧凭据或其他 NVS 数据，下一次普通重启恢复历史网络优先策略。保存发生在提交连接请求时，不是验证密码成功之后；错误密码可通过下次启动的 30 秒兜底重新配网。

注册并处理 `IP_EVENT_STA_LOST_IP`，与 `WIFI_EVENT_STA_DISCONNECTED` 一起通知 MQTT 和 Modbus 离线。MQTT 工作任务停止旧会话；Modbus 缓存立即标记过期。重新 GOT_IP 后两个任务恢复工作，无需重启设备。这个流程需要实机验证 AP/DHCP 的实际行为。

## 复用 TLS 与 EMQX 复核

沿用 `certs/ca.crt`、`certs/client.crt`、`certs/client.key` 及原嵌入符号。服务器 CA、身份匹配和有效期验证均保留，没有使用跳过证书校验选项。已本地检查客户端证书通过 CA 链与客户端用途校验，证书公钥与私钥公钥匹配。设备在系统时间不合理时启动 SNTP，并在时间恢复前等待，不在 Wi-Fi 回调阻塞。默认 NTP 为 `pool.ntp.org`；隔离局域网可改用可达的内部 NTP。

本次复用上一轮已部署和验证的证书，不再生成 CA。服务器证书 SAN 包含 `10.30.192.176`。

已读取本机 Docker `emqx-edge` 的实际配置 `/opt/emqx-edge/nanomq.conf`，确认 `verify_peer=true` 和 `fail_if_no_peer_cert=true`。运行 `tools/verify_live_mqtt.py` 对真实 `10.30.192.176:8883` 验证：合法证书 TLS 1.2 和 MQTT CONNECT 成功（CONNACK=20020000）；无客户端证书、非受信客户端证书均被拒绝。每次反向测试后再次确认合法客户端可连接。没有为测试重启 Broker，也没有修改其他监听端口。

项目历史文档记录的是 **EMQX Edge/NanoMQ 风格**，对应：

```hocon
listeners.ssl {
    enable = true
    bind = "0.0.0.0:8883"
    keyfile = "/opt/emqx-edge/etc/certs/server.key"
    certfile = "/opt/emqx-edge/etc/certs/server.crt"
    cacertfile = "/opt/emqx-edge/etc/certs/client-ca.crt"
    verify_peer = true
    fail_if_no_peer_cert = true
}
```

如果实际运行的是标准 EMQX 5/6，需要对应 listener 的 `ssl_options`，不能直接照搬 Edge 语法：

```hocon
listeners.ssl.default {
    bind = "0.0.0.0:8883"
    ssl_options {
        keyfile = "/path/to/server.key"
        certfile = "/path/to/server.crt"
        cacertfile = "/path/to/client-ca.crt"
        verify = verify_peer
        fail_if_no_peer_cert = true
    }
}
```

`cacertfile` 必须是签发设备客户端证书的受信 CA。官方说明：
- [EMQX Edge listener 配置](https://docs.emqx.com/en/emqx-edge/latest/config-description/listener.html)
- [EMQX X.509 认证](https://docs.emqx.com/en/emqx/latest/guides/access-control/authn/x509.html)

修改后必须验证三组：正确客户端证书/私钥成功；不带客户端证书拒绝；非受信 CA 签发的客户端证书拒绝。连接拒绝、路由不通或 MQTT ACL 拒绝都不能代替 TLS 证书拒绝证据。仅验证合法客户端能连接也不能证明服务端强制 mTLS。服务端还需允许发布 `env/telemetry` 和 `env/heartbeat`。

## 验证

```sh
env PATH=/usr/bin:/bin python3 components/wifi/tests/run_tests.py
env PATH=/usr/bin:/bin python3 components/mymqtt/tests/run_tests.py
env PATH=/usr/bin:/bin python3 components/modbusTCP/tests/run_tests.py
python3 tools/verify_live_mqtt.py
```

本次两组主机测试及在线 mTLS 测试均通过。新增覆盖动态 Modbus 快照（避免重复发送固定旧值）、Broker 独立断线后恢复五秒上传、同一工作周期内 Wi-Fi 断开再恢复的网络代次检测。使用 ESP-IDF v6.1 的 Python 环境直接调用 `tools/idf.py build` 已成功，产物为 `build/ingaway.bin`；尚未烧录或完成真实 AP 断网恢复验收。

MQTT 主机测试编译实际工作任务，对 MQTT、FreeRTOS、时钟做 mock，覆盖五秒上传、TLS 配置保留、匹配/无关/迟到 PUBACK、三次心跳失败、Wi-Fi 断开与恢复、连接截止时间、SNTP 等待、队列溢出、outbox 满和过期数据序列化。测试不代表真实 Broker/TLS/无线网络验证。

上板验收：订阅两主题检查节奏；关闭/恢复 AP、停止/恢复 Broker；丢弃 Broker 返回流量验证无 PUBACK 重连；断开 Modbus 从站确认 `stale=true` 仍上传、恢复后转为 false。多轮恢复应保持任务数稳定。另执行上述 mTLS 正反向测试。
