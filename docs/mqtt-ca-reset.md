# 从头重建 MQTT 双向 TLS

本次目标：EMQX Edge 容器 `emqx-edge`，Broker `10.30.192.176:8883`，ESP32 使用 `mqtts://10.30.192.176`。下面命令都在 WSL/Linux Bash、项目根目录执行，不要把 Bash 的换行反斜线直接粘贴到 PowerShell。

本次已完成：备份 ESP32 原证书，生成全新 CA/服务器/客户端证书，更新本地固件证书，检查签名链、用途、IP SAN 和私钥配对；本地 TLS 1.2 正反向测试通过。Broker 部署和设备烧录尚未执行。本次生成的材料放在 `.local/mqtt-pki/current/`，不需要再次生成。

## 1. 文件关系与已生成的目录

```text
.local/mqtt-pki/current/
  ca/
    ca.crt                 根 CA 证书
    ca.key                 CA 签发私钥，仅保存在签发环境
    ca.cnf                 CA 扩展配置
  broker/
    server.crt             服务器证书：serverAuth、IP SAN 10.30.192.176
    server.key             服务器私钥
    client-ca.crt          Broker 信任的客户端 CA（与 ca/ca.crt 相同）
    listeners-ssl.hocon    服务端配置片段
  device/
    ca.crt                 ESP32 信任的服务器 CA（同一根 CA）
    client.crt             客户端证书：clientAuth、CN=esp32-device01
    client.key             客户端私钥
  previous-device/         更换前的三个设备证书文件
```

CA 是 RSA 3072 位，有效期 10 年；服务器和客户端密钥是 RSA 2048 位，证书有效期 1 年，签名为 SHA-256。叶证书为 `CA:false`，具备 digitalSignature/keyEncipherment；服务器有 serverAuth 和指定 IP SAN，客户端有 clientAuth。配置参考 [OpenSSL X.509 扩展文档](https://docs.openssl.org/3.0/man5/x509v3_config/)。

CA 签发私钥不复制到 Broker 或 ESP32。生成目录权限为 700，私钥权限为 600。目录已加入 `.gitignore`；设备私钥原本已被 Git 跟踪，忽略规则不会自动取消跟踪，提交代码时不要提交其私钥内容。

## 2. 从零生成命令（本次已经执行，不要重复覆盖）

```bash
cd /home/px/ingaway
python3 tools/mqtt_pki.py \
  --server-ip 10.30.192.176 \
  --out .local/mqtt-pki/current \
  --install-device
```

脚本按顺序创建 CA → 生成服务器密钥/CSR → 签发服务器证书 → 生成客户端密钥/CSR → 签发客户端证书 → 验证 → 备份和替换固件证书。它拒绝覆盖已有输出目录，避免误覆盖当前 CA。未来确实需要重建时，换一个新的 `--out` 目录，并同步更换两端。

## 3. 本地验证

```bash
openssl verify -x509_strict \
  -CAfile .local/mqtt-pki/current/ca/ca.crt \
  -purpose sslserver -verify_ip 10.30.192.176 \
  .local/mqtt-pki/current/broker/server.crt

openssl verify -x509_strict \
  -CAfile .local/mqtt-pki/current/ca/ca.crt \
  -purpose sslclient \
  .local/mqtt-pki/current/device/client.crt

openssl x509 -in .local/mqtt-pki/current/broker/server.crt \
  -noout -subject -issuer -dates -ext subjectAltName

python3 tools/verify_mqtt_pki.py --pki .local/mqtt-pki/current
```

前两条应为 `OK`，SAN 应为 `IP Address:10.30.192.176`。最后一条仅启动本机临时 TLS 1.2 服务器，验证合法客户端成功、未提供证书和非受信客户端证书失败、错误服务器 IP 被拒绝；它不是对真实 EMQX 的测试。

本次 CA 指纹：

```text
85:9C:73:25:45:A4:A7:66:56:38:F6:BB:7C:BC:CD:CD:9B:C6:F5:7B:AC:5F:E3:D8:09:6F:B4:36:AA:BC:5C:A8
```

## 4. 备份并部署 Broker 文件（需要你在 Docker 所在环境执行）

先备份容器证书目录：

```bash
docker cp emqx-edge:/opt/emqx-edge/etc/certs \
  .local/mqtt-pki/current/previous-broker-certs
```

同样备份你之前编辑的实际生效配置文件；不要只保存下面的片段而丢掉其余 MQTT/授权配置。用 `docker inspect emqx-edge --format '{{json .Mounts}}'` 确认配置和证书是否来自宿主机挂载：如果是只读挂载，应更新对应宿主机源目录，不能依赖 `docker cp` 写入容器。

对于普通可写的容器目录：

```bash
docker cp .local/mqtt-pki/current/broker/server.crt \
  emqx-edge:/opt/emqx-edge/etc/certs/server.crt

docker cp .local/mqtt-pki/current/broker/server.key \
  emqx-edge:/opt/emqx-edge/etc/certs/server.key

docker cp .local/mqtt-pki/current/broker/client-ca.crt \
  emqx-edge:/opt/emqx-edge/etc/certs/client-ca.crt
```

确认 Broker 进程用户能读取三个文件；服务器私钥应只给服务用户读取，不要用全局可读权限解决权限问题。使用卷/绑定挂载保存正式证书，避免容器重建丢失。不会复制 `ca.key` 或 `client.key` 到 Broker。

## 5. 服务端实际配置

在 Broker **实际加载的配置文件**中替换 SSL listener 块，其余配置保持：

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

这是 EMQX Edge 的语法。`verify_peer` 检查客户端证书，`fail_if_no_peer_cert` 拒绝缺少证书的客户端，两项都要为 true。[官方说明](https://docs.emqx.com/en/emqx-edge/latest/config-description/listener.html)

确认宿主机映射 `8883:8883`（你此前输出中已存在）。配置和文件都就绪后再重启：

```bash
docker restart emqx-edge
docker logs --since 2m emqx-edge
```

检查是否有证书解析、私钥加载或文件权限错误。若容器包含 OpenSSL，可以检查真正部署的证书：

```bash
docker exec emqx-edge openssl x509 \
  -in /opt/emqx-edge/etc/certs/server.crt -noout -subject -issuer -ext subjectAltName

docker exec emqx-edge openssl x509 \
  -in /opt/emqx-edge/etc/certs/client-ca.crt -noout -fingerprint -sha256
```

没有 OpenSSL 时，通过 `docker cp` 导出公开证书并在宿主机检查。CA 指纹应与第 3 步一致。

## 6. 先用电脑验证真实 Broker

合法客户端：

```bash
openssl s_client \
  -connect 10.30.192.176:8883 \
  -CAfile .local/mqtt-pki/current/device/ca.crt \
  -cert .local/mqtt-pki/current/device/client.crt \
  -key .local/mqtt-pki/current/device/client.key \
  -verify_ip 10.30.192.176 \
  -verify_return_error -tls1_2 -brief </dev/null
```

应看到完成握手、TLSv1.2、非空密码套件、对端证书及验证成功。**单独一个 `Verify return code: 0` 不够**；若仍显示 `no peer certificate` 或 `Cipher (NONE)`，没有成功。

反向测试：故意不带客户端证书。

```bash
openssl s_client \
  -connect 10.30.192.176:8883 \
  -CAfile .local/mqtt-pki/current/device/ca.crt \
  -verify_ip 10.30.192.176 \
  -verify_return_error -tls1_2 -brief </dev/null
```

这条应收到握手拒绝。若仍成功，服务端未强制客户端证书，需检查实际 listener 或配置是否加载。

非受信证书测试（仅用于测试的临时证书，保存在私有目录）：

```bash
umask 077
mkdir -p .local/mqtt-pki/current/negative-test
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
  -subj '/CN=untrusted-test-only' \
  -keyout .local/mqtt-pki/current/negative-test/untrusted.key \
  -out .local/mqtt-pki/current/negative-test/untrusted.crt

openssl s_client \
  -connect 10.30.192.176:8883 \
  -CAfile .local/mqtt-pki/current/device/ca.crt \
  -cert .local/mqtt-pki/current/negative-test/untrusted.crt \
  -key .local/mqtt-pki/current/negative-test/untrusted.key \
  -verify_ip 10.30.192.176 \
  -verify_return_error -tls1_2 -brief </dev/null
```

此连接也应因证书不受信而被拒绝。端口不通不能算反向证书测试通过，必须先确认同地址的合法客户端成功。OpenSSL 测试仅验证 TLS，不发送 MQTT CONNECT；TLS 成功后 Broker 记录 EOF/没有 CONNECT 属正常测试收尾，不应误判为 TLS 失败。

## 7. 烧录 ESP32

脚本已把新设备材料放进：

```text
components/mymqtt/certs/ca.crt
components/mymqtt/certs/client.crt
components/mymqtt/certs/client.key
```

在已激活 ESP-IDF 的终端中：

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

将端口替换为实际串口；Windows 下可能是 `COM5`。当前 URI 保持 `mqtts://10.30.192.176`、端口 8883，CA/客户端证书/私钥按原嵌入方式加载。设备需要正确系统时间；已有 SNTP 逻辑，隔离网络可将 NTP 服务器改成局域网可达地址。不能通过关闭证书检查来解决时间问题。

新固件与新 Broker 证书要同步部署。仅烧录固件而 Broker 仍使用旧证书，会因信任链变化失败；仅更新 Broker 而设备仍用旧客户端证书同样会失败。

## 8. MQTT 与恢复验收

先确认 ESP32 日志出现 `MQTT connected; upload every 5 seconds`。用 MQTTX 等客户端，填写当前 Broker 地址、8883、TLS、新 CA、新客户端证书/私钥，订阅 `env/telemetry` 和 `env/heartbeat`。测试客户端使用不同 MQTT client ID；若 Broker 配置按证书 CN 强制映射 client ID，需要另签测试客户端证书，避免挤掉 ESP32。

每约 5 秒收到温度快照，其中含 `stale` 和数据年龄；心跳约 10 秒一次。停止/恢复 Broker 或 AP 后，设备应自动恢复。断开 Modbus 从站时，MQTT 仍上报过期状态；恢复后重新上报新值。

如果第 6 步仍在发送服务器证书之前收到 alert 40，即使新证书链本地验证通过，也不能宣布问题已解决。需继续查 Broker 是否实际加载新文件、私钥权限、密码套件和签名算法兼容性，结合重启日志判断。

## 回退

固件旧证书在 `previous-device/`，部署前备份的服务器证书在 `previous-broker-certs/`。若需要回退，应同步恢复两端证书与服务端配置，再重新构建烧录；不要新旧 CA 混用。
