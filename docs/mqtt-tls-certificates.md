# ESP32 与 Broker 双向 TLS：配置、证书生成与排错记录

本文整理本工程 MQTT 从明文连接迁移到双向 TLS（mTLS）的过程。目标设备为 ESP32-S3 N16R8，使用 ESP-IDF 6.1；Docker 容器名为 `emqx-edge`，镜像为 `emqx/emqx-edge:1.5.0`。实际提供的配置是 NanoMQ 风格，因此不能直接套用其他 EMQX 版本的配置路径和语法。

## 1. 当前方案与验证状态

- Broker 宿主机地址：`192.168.31.73`。
- 明文 MQTT：`mqtt://192.168.31.73:1883`。
- TLS MQTT：`mqtts://192.168.31.73:8883`。
- Broker 证书目录：`/opt/emqx-edge/etc/certs/`。
- ESP32 证书目录：`components/mymqtt/certs/`。
- 推荐最终方案：使用自己的私有 CA，分别签发服务器证书与设备客户端证书。
- 本工程已修复证书嵌入符号和 TLS 端口，并验证构建通过。
- 已确认容器原服务器证书不匹配连接使用的 IP。
- 下文重新签发、替换服务器证书和校时是解决该问题的步骤；目前没有证据确认全部部署后已完成 mTLS 连接，不应把构建成功当作 TLS 连接成功。

## 2. 证书、私钥、CA 分别是什么

| 文件 | 内容与作用 | 是否保密 |
| --- | --- | --- |
| CA 证书 | CA 的公钥、身份与有效期；作为信任锚验证其签发的证书 | 可公开，但分发来源必须可信 |
| CA 私钥 | 签发服务器或客户端证书 | 严格保密，只留在签发环境 |
| 服务器证书 | Broker 的公钥、身份、有效期、SAN 和用途等 | 可公开 |
| 服务器私钥 | Broker 在握手中证明自己持有证书对应的私钥 | 严格保密 |
| 客户端证书 | ESP32 的公钥、设备身份和客户端用途等 | 可公开 |
| 客户端私钥 | ESP32 在握手中证明设备身份 | 严格保密 |
| CSR | 证书签名申请，包含申请者的公钥和申请信息 | 不包含私钥 |

CA 对证书内容签名。验证者使用可信 CA 的公钥验证签名，并检查证书用途、有效期及身份等条件。证书文件与私钥必须配对；有 CA 证书但没有对应 CA 私钥，不能签发该 CA 名下的新证书。

服务器 CA 与客户端 CA 可以是两个不同机构。本项目为简化管理，采用一个私有 CA 分别签发两类证书。虽然文件名沿用 `client-ca`，它同时签发服务器证书也是可以的，实际用途由证书扩展决定。

单向 TLS 已经加密双向数据流：客户端验证服务器，通常通过账号等方式完成客户端认证。双向 TLS 额外要求服务器验证客户端证书，并不是“单向 TLS 只加密一个方向”。

## 3. 实际遇到的错误及处理

### 3.1 ESP32 使用 localhost

原配置：

```c
#define MQTT_BROKER_URI "mqtt://localhost"
#define MQTT_BROKER_PORT 8086
```

`localhost` 在 ESP32 上指向 ESP32 自己，不是 Docker 宿主机。应使用可访问的宿主机 IP；普通 MQTT 使用实际 TCP 监听端口 1883。8086 在提供的配置中用于 WSS，而非普通 MQTT TCP。

```c
#define MQTT_BROKER_URI "mqtt://192.168.31.73"
#define MQTT_BROKER_PORT 1883
```

宿主机地址由 DHCP 分配，建议在路由器设置地址保留。证书绑定 IP 时，IP 改变还会导致证书身份不匹配。

### 3.2 MQTTX 主机参数写入端口

错误：

```sh
mqttx sub -h localhost:1883 -t 'env/#'
# getaddrinfo ENOTFOUND localhost:1883
```

`-h` 是主机名，不应包含端口：

```sh
mqttx sub -h localhost -p 1883 -t 'env/#'
# 从其他机器访问时：
mqttx sub -h 192.168.31.73 -p 1883 -t 'env/#'
```

### 3.3 映射了 8883，但没有 TLS 监听

测试：

```sh
openssl s_client -connect 192.168.31.73:8883 -showcerts </dev/null
```

错误：

```text
Connection refused
connect:errno=111
```

TCP 连接被拒绝，TLS 握手还没有开始。Docker 的 `8883:8883` 只配置端口转发，不会创建 Broker 监听器。实际配置中：

```hocon
listeners.ssl {
    enable = false
    bind = "0.0.0.0:8883"
}
```

需要启用监听、配置可读取的证书和私钥，并应用配置或重启服务。WebUI 保存配置不一定代表当前监听器已成功启动。

几个开关的用途不同：

| 配置 | 用途 |
| --- | --- |
| `listeners.ssl` | MQTT over TLS，通常 8883 |
| `listeners.wss` | MQTT over 安全 WebSocket，配置中为 8086 |
| `http_server.ssl` | Web 管理/API 的 HTTPS |
| `bridges...ssl` | Broker 作为客户端连接其他 Broker 时的 TLS |

启用 WebUI HTTPS 或 WSS，不代表启用了 8883。开启 TLS 也不自动关闭 1883 明文监听。

### 3.4 按错误的 Broker 目录查找证书

早先使用 `/opt/emqx/etc` 未找到文件。后来找到真实目录：

```text
/opt/emqx-edge/etc/certs/client-key.pem
/opt/emqx-edge/etc/certs/client-cert2.pem
/opt/emqx-edge/etc/certs/cert.pem
/opt/emqx-edge/etc/certs/key.pem
/opt/emqx-edge/etc/certs/cacert.pem
/opt/emqx-edge/etc/certs/client-cert.pem
/opt/emqx-edge/etc/certs/jwt_key/jwtRS256.key
```

查找命令：

```sh
docker exec -u 0 emqx-edge sh -c \
'find / -type f \( -name "*.pem" -o -name "*.crt" -o -name "*.key" \) 2>/dev/null'
```

`client-cert.pem` 和 `client-cert2.pem` 是两份证书候选，不是 mTLS 必须同时使用的两个证书。必须检查哪份与客户端私钥配对。JWT 密钥不是 MQTT TLS 私钥。

配置中的 `./etc/certs/...` 相对于服务进程工作目录，不一定相对于配置文件。建议使用绝对路径。

### 3.5 嵌入符号导致链接失败

出现：

```text
undefined reference to `_binary_certs_ca_crt_start'
undefined reference to `_binary_certs_client_crt_start'
undefined reference to `_binary_certs_client_key_start'
```

文件已经通过 `EMBED_TXTFILES` 嵌入，但本工程 ESP-IDF 实际生成的符号不带 `certs_` 前缀。已修复为：

```text
_binary_ca_crt_start
_binary_client_crt_start
_binary_client_key_start
```

之后完整构建通过。遇到类似问题应检查生成对象的实际符号，而不是只推测路径转换规则：

```sh
/home/px/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm \
  -g build/esp-idf/mymqtt/libmymqtt.a | grep _binary_
```

工具链路径可能随版本变化。

### 3.6 TLS 握手 -0x7280

日志：

```text
mbedtls_ssl_handshake returned -0x7280
```

当前 mbedTLS 定义为 `MBEDTLS_ERR_SSL_CONN_EOF`：握手期间连接结束。这个错误不直接表示某种证书错误。

检查发现代码使用 `mqtts://192.168.31.73`，但显式端口仍是 1883。向明文端口发送 TLS 握手，服务器可能直接关闭连接。已修复为 8883，并通过构建验证。

若 URI 中带端口，同时又设置 `.address.port`，应保证一致；本项目使用显式 8883，避免保留旧 1883 配置。

### 3.7 TLS 握手 -0x2700

日志：

```text
mbedtls_ssl_handshake returned -0x2700
mqtt: type=1, tls_err=0x801a, mbedtls_err=9984, cert_flags=0x0, socket_errno=0
```

`0x2700` 为 `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`；9984 是 0x2700 的十进制值，MQTT 错误结构中记录了该值。服务器证书验证失败，可能涉及 CA、有效期、用途或身份匹配。

本次 `cert_flags=0x0` 没给出细分原因，不能据此判断证书验证成功。已在 MQTT 错误事件中添加错误类型、TLS 错误、证书标志与 socket errno 日志。

进一步在宿主机检查服务器证书：

```sh
docker cp emqx-edge:/opt/emqx-edge/etc/certs/cert.pem /tmp/server.pem
docker cp emqx-edge:/opt/emqx-edge/etc/certs/cacert.pem /tmp/ca.pem

openssl x509 -in /tmp/server.pem -noout \
  -subject -issuer -dates -ext subjectAltName

openssl verify -CAfile /tmp/ca.pem \
  -purpose sslserver -verify_ip 192.168.31.73 /tmp/server.pem
```

实际结果：

```text
subject=C=CN, ST=hangzhou, O=EMQ, CN=Server
issuer=C=CN, ST=hangzhou, O=EMQ, CN=RootCA
notBefore=May 8 08:07:05 2020 GMT
notAfter=May 6 08:07:05 2030 GMT
No extensions in certificate
error 64 at 0 depth lookup: IP address mismatch
```

确认的问题是：原证书没有 IP SAN，也不对应 `192.168.31.73`。解决方法是重新签发包含该 IP 的服务器证书，或使用可解析且确实包含在证书身份中的域名。不应关闭名称验证来绕过。

ESP32 还需要正确系统时间。当前排查时未发现 SNTP 校时代码；即使修正 IP SAN，时间停留在 1970 年仍可能导致证书未生效错误。这是另一个需要落实的前置条件，不是已经证实唯一的错误原因。

## 4. 完整证书生成流程

下面命令在 Docker 宿主机或专门的签发机器执行，需要 OpenSSL。为局域网设备使用私有 CA，不需要向公共网站申请。公共 CA 通常不会签发私网 IP 地址证书。

如果已经生成 `client-ca.crt`、`client-ca.key` 及设备证书，不要重新覆盖 CA；直接用已有 CA 签发服务器证书。以下是一套从零创建的流程。

### 4.1 建立私有 CA

```sh
mkdir -p ~/mqtt-client-pki
cd ~/mqtt-client-pki
umask 077

openssl genpkey -algorithm RSA \
  -pkeyopt rsa_keygen_bits:2048 \
  -out client-ca.key

openssl req -x509 -new -sha256 \
  -key client-ca.key \
  -days 3650 \
  -subj "/CN=My MQTT Client CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out client-ca.crt
```

`umask 077` 限制新建文件权限。示例 CA 私钥没有密码，应严格保护；生产签发环境应考虑加密私钥、离线保存和备份。CA 私钥绝不能嵌入固件或复制进 Broker 容器。

### 4.2 签发 ESP32 客户端证书

```sh
openssl genpkey -algorithm RSA \
  -pkeyopt rsa_keygen_bits:2048 \
  -out client.key

openssl req -new -key client.key \
  -subj "/CN=esp32-device01" \
  -out client.csr

printf '%s\n' \
  'basicConstraints=critical,CA:FALSE' \
  'keyUsage=critical,digitalSignature' \
  'extendedKeyUsage=clientAuth' \
  'subjectKeyIdentifier=hash' \
  'authorityKeyIdentifier=keyid,issuer' \
  > client.ext

openssl x509 -req \
  -in client.csr \
  -CA client-ca.crt \
  -CAkey client-ca.key \
  -CAcreateserial \
  -days 365 -sha256 \
  -extfile client.ext \
  -out client.crt

openssl verify -CAfile client-ca.crt \
  -purpose sslclient client.crt
```

预期：`client.crt: OK`。每台设备应使用不同的私钥、证书身份和证书序列号。不要将所有设备共用同一个私钥作为正式部署方案。

### 4.3 签发带 IP SAN 的服务器证书

```sh
openssl genpkey -algorithm RSA \
  -pkeyopt rsa_keygen_bits:2048 \
  -out server.key

openssl req -new -key server.key \
  -subj "/CN=mqtt-broker" \
  -out server.csr

printf '%s\n' \
  'basicConstraints=critical,CA:FALSE' \
  'keyUsage=critical,digitalSignature,keyEncipherment' \
  'extendedKeyUsage=serverAuth' \
  'subjectAltName=IP:192.168.31.73' \
  > server.ext

openssl x509 -req \
  -in server.csr \
  -CA client-ca.crt \
  -CAkey client-ca.key \
  -CAcreateserial \
  -days 365 -sha256 \
  -extfile server.ext \
  -out server.crt

openssl verify -CAfile client-ca.crt \
  -purpose sslserver -verify_ip 192.168.31.73 server.crt
```

预期：`server.crt: OK`。CA 序列号文件应保留，签发命令不要并发执行。

如果采用域名，应将 SAN 改为实际域名，如 `DNS:mqtt.example.com`，保证 ESP32 能解析这个域名，并在 URI 中使用它。不要把示例域名直接作为实际配置。

### 4.4 检查证书与私钥是否配对

以下两条命令的摘要必须相同：

```sh
openssl x509 -in client.crt -pubkey -noout |
  openssl pkey -pubin -outform DER |
  openssl dgst -sha256

openssl pkey -in client.key -pubout -outform DER |
  openssl dgst -sha256
```

服务器证书也可将文件换成 `server.crt` 和 `server.key` 检查。验证私钥结构但不输出内容：

```sh
openssl pkey -in client.key -check -noout
```

## 5. Broker 容器部署

### 5.1 将运行所需文件放入容器

```sh
docker cp server.crt emqx-edge:/opt/emqx-edge/etc/certs/server.crt
docker cp server.key emqx-edge:/opt/emqx-edge/etc/certs/server.key
docker cp client-ca.crt emqx-edge:/opt/emqx-edge/etc/certs/client-ca.crt
```

只复制服务器私钥，不复制 CA 私钥，也不需要把客户端私钥交给 Broker。确保 Broker 服务用户可读取证书与服务器私钥，私钥不要设为全员可读。

上面的 `docker cp` 便于验证，但容器删除重建后不保证保留文件。正式使用应将配置和证书通过 bind mount 或 volume 持久化，建议只读挂载证书，并正确设置宿主机文件权限。不要因重建容器丢失原配置。

### 5.2 配置双向 TLS

在服务实际加载的配置文件中设置：

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

- `certfile`/`keyfile`：服务器提供自己的身份与私钥证明。
- `cacertfile`：Broker 信任的客户端 CA。
- `verify_peer`：验证客户端证书。
- `fail_if_no_peer_cert`：不接受缺少客户端证书的连接。

若当前版本不支持相应功能、文件加载失败或配置未实际生效，应以启动日志和端口验证为准。

应用后：

```sh
docker restart emqx-edge
docker logs --tail 100 emqx-edge
```

重启会暂时断开客户端。日志应检查证书读取、私钥配对、TLS 支持和监听器启动是否出错。

如果目标是强制安全连接，还需要按当前版本支持的方法停用明文 TCP 1883 和明文 WS 8083。WSS 等其他入口同样要配置认证策略，避免绕过；仅删除一个端口映射不能替代完整的访问控制。建议先验证 TLS 成功，再停用明文入口。

## 6. ESP32 嵌入与 MQTT 配置

### 6.1 文件分配

最终统一使用私有 CA 时：

```text
components/mymqtt/certs/
├── ca.crt        ← client-ca.crt，验证服务器
├── client.crt    ← 设备证书
└── client.key    ← 设备私钥
```

例如从签发目录复制：

```sh
cp client-ca.crt /home/px/ingaway/components/mymqtt/certs/ca.crt
cp client.crt /home/px/ingaway/components/mymqtt/certs/client.crt
cp client.key /home/px/ingaway/components/mymqtt/certs/client.key
```

原先“保留镜像服务器证书”的方案中 ESP32 信任的是镜像的 `cacert.pem`。替换服务器证书后必须同步更新 ESP32 的 CA，否则仍会验证失败。

### 6.2 CMake 嵌入

在 `components/mymqtt/CMakeLists.txt` 的现有 `idf_component_register()` 内加入，保留已有源文件、头文件路径和依赖：

```cmake
EMBED_TXTFILES
    "certs/ca.crt"
    "certs/client.crt"
    "certs/client.key"
```

文件会嵌入固件，并作为文本在结尾添加 NUL。PEM 必须保持完整的 BEGIN/END 标记和换行。不要在 C 字符串中手动复制私钥内容。

### 6.3 声明嵌入符号

本工程实际符号：

```c
extern const char mqtt_ca[]
    asm("_binary_ca_crt_start");
extern const char mqtt_cert[]
    asm("_binary_client_crt_start");
extern const char mqtt_key[]
    asm("_binary_client_key_start");
```

### 6.4 客户端配置

```c
const esp_mqtt_client_config_t mqtt_cfg = {
    .broker = {
        .address.uri = "mqtts://192.168.31.73:8883",
        .verification.certificate = mqtt_ca,
    },
    .credentials = {
        .authentication = {
            .certificate = mqtt_cert,
            .key = mqtt_key,
        },
    },
};
```

此示例把端口放入 URI；若项目继续使用单独的 `.address.port`，必须设置为 8883。不要保留 1883。不启用 `skip_cert_common_name_check` 等绕过身份检查的选项。

示例客户端私钥是未加密 PEM。若使用带密码的私钥，需另外配置对应密码字段及其长度，并考虑密码的保存方式。

### 6.5 时间同步与启动顺序

推荐顺序：

```text
Wi-Fi 获取 IP
→ SNTP 校时成功
→ 创建并启动 MQTT 客户端
→ TLS 认证成功
→ MQTT CONNECT/CONNACK 成功
→ 订阅/发布业务数据
```

不要在 IP 事件回调中阻塞等待 SNTP；可交给独立的启动任务，配置同步超时，失败时重试。使用 ESP-IDF 的 `esp_netif_sntp` 等接口完成实际校时。

排查设备时间：

```c
#include <time.h>

time_t now = time(NULL);
ESP_LOGI(TAG, "System time: %lld", (long long)now);
```

若值接近 0，表示仍接近 1970 年。证书有效期按真实时间检查；应保证签发机器时间也正确。

构建与烧录：

```sh
idf.py reconfigure
idf.py build flash monitor
```

构建成功只说明编译、链接通过，不保证 Broker 配置、证书和网络正确。

## 7. 用宿主机先验证 mTLS

在签发目录执行：

```sh
openssl s_client \
  -connect 192.168.31.73:8883 \
  -CAfile client-ca.crt \
  -cert client.crt \
  -key client.key \
  -verify_ip 192.168.31.73 \
  -verify_return_error \
  -showcerts </dev/null
```

检查服务器验证结果、协商出的协议和加密套件，以及 Broker 端是否报错。`s_client` 不会发送有效的 MQTT CONNECT，因此随后被 Broker 断开或关闭输入不等同于 TLS 验证失败。

域名连接时改用：

```sh
-servername 实际域名 -verify_hostname 实际域名
```

MQTTX 双向 TLS 订阅（具体选项可用 `mqttx sub --help` 核对）：

```sh
mqttx sub \
  -h 192.168.31.73 -p 8883 -l mqtts \
  --ca client-ca.crt \
  --cert client.crt \
  --key client.key \
  -t 'env/#'
```

验证强制客户端认证时，应另外测试不带客户端证书的连接确实被拒绝，并确认有效证书能完成 MQTT 连接。只有 `Verify return code: 0` 不足以独立证明服务器已要求客户端证书。

## 8. TLS 如何通过证书和密钥保护通信

### 8.1 身份认证

简化流程：

```text
ESP32                                       Broker
  | -- ClientHello（协议、套件、随机数等） --> |
  | <-- 服务器握手信息、证书、密钥交换信息 --- |
  | 验证服务器证书链、时间、SAN、用途          |
  | -- 客户端证书及握手证明 ----------------> |
  |                         验证客户端证书链  |
  | <-- 双方验证握手 Finished，建立安全会话 --> |
  | <---------- 加密 MQTT 数据 ------------> |
```

精确消息顺序和签名方式随 TLS 版本、密码套件变化。现代 TLS 使用握手签名等机制证明对方持有证书公钥对应的私钥，不能只复制一张公开证书就冒充设备。服务器/客户端私钥不通过网络发送。

### 8.2 会话密钥与数据加密

证书通常用于身份验证，而不是直接拿 RSA 私钥逐条加密 MQTT 数据。使用现代的 ECDHE 密钥交换套件时：

1. 双方生成临时密钥交换参数，交换公开部分。
2. 双方各自计算共享秘密，不在网络上直接发送它。
3. 使用协议规定的密钥派生机制，结合握手信息和随机数生成会话密钥。
4. 客户端到服务器、服务器到客户端使用各自方向的流量密钥。
5. MQTT 数据通过协商的对称加密算法处理，例如 AES-GCM 或 ChaCha20-Poly1305。

TLS 1.3 采用 HKDF 等派生机制，TLS 1.2 的派生机制不同；具体使用何种协议和套件，应以实际握手结果为准，不能默认本项目已经使用 TLS 1.3。

AEAD 套件同时提供机密性和完整性：旁观者不能直接读取内容，篡改密文会导致认证失败。临时 ECDHE 在正确实现并销毁临时秘密时可提供前向保密，日后泄露长期证书私钥通常不能解密此前录制的会话。

### 8.3 TLS 不解决所有安全问题

- TLS 保护 ESP32 到 Broker 这段链路；Broker 解密后可以读取消息，不等于业务端到端加密。
- 证书认证身份，ACL 决定该身份允许发布或订阅哪些主题。
- 只检查客户端证书可信，不自动把 CN 映射成 MQTT 用户名；需按 Broker 能力配置身份映射和 ACL。
- 客户端私钥被复制后，攻击者可能冒充该设备，因此应按设备分配密钥并设计撤销/轮换机制。
- 私钥嵌入固件不代表私钥已加密。正式部署应考虑 Flash 加密、安全启动、调试口保护，必要时使用安全元件。
- 原镜像附带的示例私钥不适合作为生产秘密。

## 9. 部署核对清单

- [ ] Docker 宿主机 IP/域名稳定且 ESP32 可达。
- [ ] 8883 映射且 Broker 实际开启 TLS 监听。
- [ ] 服务器证书具有正确 IP SAN/域名 SAN 和 serverAuth 用途。
- [ ] 客户端证书具有 clientAuth 用途，并与客户端私钥匹配。
- [ ] Broker 信任签发客户端证书的 CA，要求客户端证书。
- [ ] ESP32 信任签发当前服务器证书的 CA。
- [ ] 签发机器和 ESP32 时间正确，证书在有效期内。
- [ ] 证书路径、权限正确，配置和证书持久化保存。
- [ ] 先通过 OpenSSL/MQTTX 验证，再验证 ESP32。
- [ ] 业务身份与 ACL 完成配置，明文及其他入口不绕过安全策略。
- [ ] 私钥不提交 Git、不贴日志；CA 私钥只留签发环境。
- [ ] 已规划证书续期、设备撤销及配置迁移。

建议给项目 `.gitignore` 增加针对私钥和本地 PKI 目录的规则；已被 Git 跟踪的文件需要额外处理，单独添加忽略规则不能移除历史中的秘密。若私钥曾公开，应重新生成并替换，不能只删除文件。
