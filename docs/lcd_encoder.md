# ST7789 240×320 + 旋转编码器交互

目标：ESP32-S3 N16R8，ESP-IDF v6.1。不使用触摸屏，不接入触摸驱动。
实现位于 `components/interaction/`，引脚及屏幕参数统一放在
`components/interaction/include/interaction_config.h`。

## 接线

| ST7789 模块 | ESP32-S3 默认 GPIO |
|---|---|
| SCL / SCK | 14 |
| SDA / MOSI | 15 |
| CS | 16 |
| DC | 17 |
| RES | 18 |
| BLK | 21（仅背光逻辑控制） |
| GND | GND |
| VCC | 按模块额定供电要求 |

SPI3、20MHz、竖屏 240×320，不需要 MISO。SPI 屏幕的 SDA 是 MOSI，不是 I²C。
BLK 如果直接连接背光 LED，必须使用适当的驱动电路，不可由 GPIO 直接供电。
背光固定供电时将 `LCD_PIN_BL` 设为 -1。背光默认高电平开启。

| 编码器信号 | ESP32-S3 默认 GPIO |
|---|---|
| A / CLK | 4 |
| B / DT | 5 |
| SW（按压） | 6 |
| 公共地 / GND | GND |

代码假设 A/B 为正交数字信号，SW 默认按下接地；A/B 使用内部上拉，SW 会按
`ENCODER_SW_ACTIVE_LEVEL` 自动选择上拉/下拉。若你的模块按下时 SW 输出高电平，
将 `components/interaction/include/interaction_config.h` 中的
`ENCODER_SW_ACTIVE_LEVEL` 改为 `1`。有源模块输出须兼容 3.3V，不能向 ESP32-S3 输入 5V。
**不要仅凭 C 字样认定是按键**：裸编码器的 C 往往是 A/B 公共端；
只有模块资料明确 C 为 SW 时才接 GPIO6。按键需要独立触点。
实际板上这些 GPIO 必须已引出且未被其他外设占用，上电前核对接线。

## 交互

启动由独立 `lcd_ui` 任务初始化屏幕，不阻塞原 Wi-Fi/BLUFI 流程。
默认 `LCD_TEST_ENABLED=1`：先依次显示红、绿、蓝、白、黑（各 0.5 秒），
再显示白色边框及 TL/TR/BL/BR 四角标记（2 秒），随后启用编码器并进入菜单。
可将该配置设为 0 跳过开机测试。

- **旋转**：选择菜单项。
- **短按**：进入选中菜单。
- **长按 800ms**：返回主菜单；松开不触发短按。
- `ENCODER TEST`：旋转增减计数，短按清零，计数范围 -9999～9999。
- `WIFI SETUP`：进入重新配网确认页，默认选中 `CANCEL`。
  旋转选择 `YES`，短按才执行；长按取消。

### 重新配网行为

确认后显示 `RESTARTING`，调用 `wifi_reprovision()` 将一次性的强制配网标记
写入 NVS（`wifi_boot/force_blufi`）并提交，然后重启设备。
下次启动消费该标记、跳过旧 Wi-Fi 自动连接，广播 BLUFI 等待手机下发凭据。
因此配网成功、BLE 已关闭之后仍能通过菜单重新配网。
普通启动会优先连接已保存网络；没有凭据时立即启用 BLUFI，连接等待超时为 30 秒。
重启会中断 Wi-Fi、MQTT、Modbus；重新配网获取 IP 后按现有流程恢复业务。
不擦除整个 NVS、不删除证书，也不保证旧凭据已从 Flash 安全清除。
若标记写入或提交失败，不重启；屏幕显示 `SETUP FAILED / NVS WRITE ERROR`
两秒后返回主菜单，用户可以重试。

## 驱动与限制

- 使用 ESP-IDF 自带的 `esp_lcd` ST7789 驱动；RGB565 主机帧缓冲优先分配在 PSRAM，
  分成 16 行转换为高字节在前的内部 DMA 缓冲，收到传输完成回调后才复用。
- 帧缓冲约 150KiB，DMA 缓冲 7.5KiB。按输入事件重绘，适合低频菜单，不是动画 UI。
- 内置 5×7 英文字体，支持大写字母、数字、空格、减号和冒号，暂不含中文字库。
- 红蓝互换调整 `LCD_BGR`，负片效果调整 `LCD_INVERT_COLOR`，
  偏移调整 `LCD_X_GAP/Y_GAP`，镜像调整 `LCD_MIRROR_X/Y`。
- 编码器由独立 esp_timer 任务每 1ms 采样，不在绘图任务里等待采样。
  依赖任务调度，不保证极高速旋转下完全不漏计。
- 默认 4 次合法相位变化算一格；每格两步/半步可调整 `ENCODER_TRANSITIONS_PER_STEP`。
  方向相反可设置 `ENCODER_REVERSE=1` 或交换 A/B。
- 状态表抵消往返抖动；双位非法跳变丢弃未完成步进；按键消抖 20ms。
  输入通过有序队列传递，溢出时取消当前操作并返回菜单。
- 软件初始化/传输成功不代表屏幕实际接好：SPI 没有屏幕响应验证。
  初始化/绘图报错会记录日志并停止 UI，Wi-Fi 不因此被主动停止。

## 编译与测试

```sh
. /home/px/.espressif/tools/activate_idf_v6.1.sh
idf.py reconfigure build
# 核实板型、接线和端口后再烧录：
idf.py -p /dev/ttyACM2 flash monitor
```

主机逻辑测试：

```sh
PATH=/usr/bin:/bin cc -std=c11 -Wall -Wextra -Werror \
  -fsanitize=undefined,address components/interaction/tests/test_logic.c \
  -o /tmp/ingaway-test-interaction
ASAN_OPTIONS=detect_leaks=0 /tmp/ingaway-test-interaction
```

覆盖双向转动、方向反转、触点反跳、中途换向、非法跳变、按键消抖、
长短按互斥、时钟回绕、菜单返回、计数及重新配网二次确认。
关闭 LeakSanitizer 以适配沙箱环境，逻辑测试无动态内存分配。

实机验收尚待执行：

1. 五色正确，边框完整，四角文字无镜像、裁切或偏移。
2. 顺/逆时针各转 20 格，分别增减 20；静置一分钟无误计数。
3. 短按只触发一次，长按返回且释放不触发短按。
4. `WIFI SETUP` 默认取消，取消不影响网络；确认 YES 后重启并可重新发现 BLUFI。
5. BLE 关闭后以及路由器离线时分别验证重新配网，再检查新网络下 MQTT/Modbus 恢复。
6. Wi-Fi/MQTT/Modbus 并行运行时反复操作 10 分钟，检查漏计、卡顿、重启及看门狗。

串口 `interaction` 日志会在初始化时打印 A/B/SW 引脚和当前电平，并记录输入事件及处理前的 UI 状态：
输入 1/2 为正反转，3 为短按，4 为长按；页面 0/1/2 为主菜单/计数/配网确认。
如果按下 SW 时没有出现输入 3/4，先看初始化日志里的 `SW` 电平及实际按下时电平是否变化；
默认按下应从 1 变 0，若相反则把 `ENCODER_SW_ACTIVE_LEVEL` 改为 1。
可以配合屏幕观察判断是否收到编码器输入，但日志本身不能证明每格计数正确。

相关回归测试：

```sh
PATH=/usr/bin:/bin python3 components/wifi/tests/run_tests.py
PATH=/usr/bin:/bin python3 components/mymqtt/tests/run_tests.py
PATH=/usr/bin:/bin python3 components/modbusTCP/tests/run_tests.py
```

编译和主机测试不能替代上述硬件验证。
