# MQTT 部署状态（2026-10-12 UTC）

- 本机 Docker：`emqx-edge`，镜像 `emqx/emqx-edge:1.5.0`。
- Broker：`10.30.192.176:8883`。
- 新 PKI：`.local/mqtt-pki/current/`，已同步安装至 `components/mymqtt/certs/`。
- 实际配置：容器内 `/opt/emqx-edge/nanomq.conf`；已有 SSL listener 配置正确，未修改其他配置。
- 已部署 `server.crt`、`server.key`、`client-ca.crt` 至 `/opt/emqx-edge/etc/certs/`，并重启容器。
- Broker 以 root 运行，服务器私钥属 root，权限 600。
- 部署前备份：`.local/mqtt-deploy/backup-20261012T162536Z/`（配置及证书）。
- 当前 CA SHA-256：`C9:8E:FF:2E:42:16:46:F0:E3:33:3B:B9:A1:67:0C:4E:DE:F1:68:C0:75:A6:A3:08:C5:9F:D0:DD:5E:CC:11:7F`。

## 验证结果

- 本地证书链、IP SAN、用途、密钥配对及 TLS 正反向测试通过。
- 真实 Broker TLS 1.2 握手成功，服务器证书验证通过。
- 真实 Broker 不带客户端证书、携带非受信客户端证书的 TLS 1.2 连接均被拒绝；每次反向测试后合法客户端仍可连接。复核命令：`python3 tools/verify_live_mqtt.py`。
- 使用新客户端证书发送 MQTT 3.1.1 CONNECT 成功，CONNACK 为 `20020000`，随后正常 DISCONNECT。

## 尚待完成

- 固件编译已通过：直接使用 `/home/px/.espressif/tools/python/v6.1/venv/bin/python /home/px/.espressif/v6.1/esp-idf/tools/idf.py build`，产物为 `build/ingaway.bin`。此前 export.sh 激活失败不再阻塞此次编译。串口发现 `/dev/ttyACM2`，未烧录，实机恢复验收待完成。
- 容器目前无卷挂载：证书与配置仅存于容器可写层，重启可保留，但容器删除重建会丢失。正式部署需持久化配置和证书。
- 1883 等其他监听与原有认证、ACL 保持不变；8883 的双向 TLS 不代表其他端口已加固。
- 本次未验证真实设备上报，不应将电脑 MQTT CONNECT 成功等同于设备验收通过。

不要再次生成 CA。后续编译、烧录及客户端测试应使用上述同一套材料。
