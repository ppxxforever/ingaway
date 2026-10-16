# Modbus TCP 采集

Wi-Fi 获取 IP 后调用 `modbusTCP_init(netif)`，只创建一次独立的 `modbus_poll` 任务。回调立即返回，不等待从站连接或寄存器响应。重复 GOT_IP 只恢复网络可用状态，不重复创建任务。断开或丢失 IP 时调用 `modbusTCP_network_down()`，立即将缓存标为过期。

当前采集目标为 `10.30.192.176:502`、Unit ID 1、保持寄存器偏移 0、一个 `uint16_t`。HMI 和压力没有配置寄存器映射，不发送猜测的请求；`read_hmi()` 返回 `ESP_ERR_NOT_SUPPORTED`。温度是原始值，不隐含倍率换算。

## 超时与重试

| 项目 | 默认值 | 配置 |
|---|---|---|
| 成功后轮询等待 | 2000 ms | `CONFIG_INGAWAY_MB_POLL_INTERVAL_MS` |
| 单次连接等待 | 2 s | 库的 `CONFIG_FMB_TCP_CONNECTION_TOUT_SEC` |
| 单次响应超时 | 2000 ms | `CONFIG_INGAWAY_MB_RESPONSE_TIMEOUT_MS` |
| 每轮额外重试 | 2 次（总共最多 3 次尝试） | `CONFIG_INGAWAY_MB_MAX_RETRIES` |
| 轮内重试等待 | 250 ms | `CONFIG_INGAWAY_MB_RETRY_DELAY_MS` |
| 重连退避 | 1、2、4、8、16、30、30…秒 | `CONFIG_INGAWAY_MB_BACKOFF_MIN_MS/MAX_MS` |
| 数据最大年龄 | 5000 ms | `CONFIG_INGAWAY_MB_STALE_AFTER_MS` |

这些采集参数可在 menuconfig 的 **Ingaway Modbus acquisition** 中修改；连接超时使用官方 Modbus 配置，默认已写入 `sdkconfig.defaults`。响应超时必须小于库的 API 阻塞上限，编译时检查。实际调用耗时还包含库的排队、调度和资源清理时间，不是硬实时截止时间。

每轮尝试包含：必要时创建/连接控制器，然后读取一个寄存器。连接失败或读取失败均消耗一次尝试。读取成功后恢复正常采集并重置退避；失败不覆盖最后一次成功值。每轮耗尽后删除控制器再退避，退避期间没有活动控制器或隐藏的后台重连；轮内由库处理连接恢复。Wi-Fi 不可用时不创建新控制器，网络恢复后继续采集。正在执行的库调用由其超时结束，断网通知不从其他任务强行删除控制器。

控制器由轮询任务独占，业务调用不直接执行网络事务。释放失败时进入 `MODBUS_FAULT` 并要求重启，不重复使用可能部分释放的句柄。普通离线、超时和连接拒绝会继续按退避重试。

## 获取数据

```c
modbus_temp_sample_t sample;
if (modbusTCP_get_temp_sample(&sample) == ESP_OK) {
    if (!sample.has_value) {
        // 尚无成功采样。
    } else if (sample.stale) {
        // sample.value 是历史值，界面应显示“过期/离线”。
    } else {
        // sample.value 是当前可用的 uint16_t 原始值。
    }
}
```

快照包含 `value`、`type`、`has_value`、`stale`、`last_success_ms`（开机后的单调毫秒）、`last_error` 和采集状态。查询成功只表示拿到了快照，不表示数据有效。断网、读取失败或数据年龄超限都会使数据过期；只有新的有效响应才清除过期标记。每次断网递增网络代次，即使迅速恢复，断网前发起请求的迟到响应也不会刷新缓存。

`read_temp(uint8_t *temp, uint8_t *type)` 现在是非阻塞缓存接口，**不再触发一次网络读取**。调用方需提供至少两字节的 `temp` 缓冲区。没有数据返回 `ESP_ERR_INVALID_STATE`；数据过期返回 `ESP_ERR_TIMEOUT`，均不修改输出缓冲区。首次连接后允许暂时没有数据。

## 协议与调度

业务层只调用官方 `mbc_master_create_tcp/set_descriptor/start/get_parameter/delete` 接口。MBAP、事务 ID、寄存器字节序和 TCP 分段/合并由 esp-modbus 处理；没有手写 socket 或假设一次 recv 就是一个 Modbus 响应。

esp-modbus 2.1.4 的驱动先发出停止完成信号，随后才挂起自身。此前日志先显示 resume、后显示 suspended，与丢失唤醒竞态一致。轮询任务沿用应用层规避：固定到驱动所在 CPU，优先级低一级（当前 CPU0，驱动 10、轮询 9）。编译断言禁止无 CPU 绑定配置。创建、启动、删除均从该任务调用，不从高优先级 Wi-Fi 回调执行。升级库或改变调度配置后需复核该规避方式。

本版本改用 `start_disconnected=false`，利用库的连接等待超时；等待发生在采集任务内，不阻塞 Wi-Fi 配网。官方依赖文件未修改。

## 验证

主机测试编译实际 `modbusTCP.c`，用可控时钟和库/RTOS mock 验证逻辑：

```sh
env PATH=/usr/bin:/bin python3 components/modbusTCP/tests/run_tests.py
```

覆盖：每轮尝试上限、指数退避及上限、退避期间释放控制器、失败保留旧值、缓存年龄过期、重新采样恢复、网络代次隔离迟到响应、重复初始化、任务创建失败以及清理失败保护。主机模拟不验证真实 FreeRTOS 调度或官方库 TCP 实现。

实机验收应执行：

1. 从站正常运行：持续读取，与寄存器值一致。
2. 启动时关闭从站，再启动：连接按有限尝试和退避重试，随后恢复采集。
3. 采集中停止/恢复从站：旧值标为过期，恢复后更新数据。
4. 断开/恢复 Wi-Fi，包括快速断开重连：缓存立即过期，恢复后只有一个采集任务。
5. 使用从站测试程序延迟响应、发送异常响应、拆分 MBAP/数据段或连续发送帧：检查库的超时、帧处理和事务匹配行为。
6. 长时间循环断连：监测可用堆和任务数量，确认库的控制器反复释放/创建没有资源增长。

抓包可用 `arp || tcp.port == 502`。连接超时、响应超时和数据过期是不同事件，应结合状态快照及抓包判断。
