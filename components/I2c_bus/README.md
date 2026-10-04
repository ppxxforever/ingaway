# 共享 I²C 总线：BMP280 + AHT20

适用于本工程 ESP-IDF 6.1 的新版 `driver/i2c_master.h`，不要与旧版 `driver/i2c.h` 混用。

## 默认采集方案

`main/ingaway.c` 已调用 `environment_start()`：I2C0 使用 SDA=GPIO8、SCL=GPIO9，100 kHz；独立任务每轮读取后等待 2 秒。AHT20 提供温湿度，BMP280 提供气压，目前仅输出日志，不发布 MQTT。传感器未连接时自动重试，不影响 Wi-Fi 启动。连续三次读取失败会尝试重新初始化。

在 `idf.py menuconfig` → `Environment sensors` 可以修改引脚、BMP280 地址及采集间隔。GPIO8/9 是 ESP32-S3 默认规划，请确认具体开发板引出这些引脚且未被板载外设占用。

## 接线

两个传感器并联到同一组 SDA/SCL，使用 3.3 V 电源和共地：

| 信号 | ESP32 | BMP280 | AHT20 |
| --- | --- | --- | --- |
| SDA | 自选可用 GPIO | SDA | SDA |
| SCL | 自选可用 GPIO | SCL | SCL |
| 电源 | 3.3 V | VCC | VCC |
| 地 | GND | GND | GND |

SDA、SCL 需要外部上拉到 3.3 V（常见 4.7 kΩ；模块可能自带）。内部弱上拉仅作辅助。裸 BMP280 的 CSB 接高电平选择 I²C，SDO 接低电平为 0x76，接高电平为 0x77；不要悬空。AHT20 地址固定为 0x38，两者地址不冲突。

## 使用示例

将下面函数加入应用，在合适位置调用。GPIO 参数由实际接线决定，不在驱动中写死。示例读取一次，持续采集可在独立 FreeRTOS 任务中循环读取，建议间隔至少 2 秒。

```c
#include "i2c_bus.h"
#include "bmp280.h"
#include "aht20.h"
#include "esp_log.h"

static esp_err_t environment_read_once(gpio_num_t sda, gpio_num_t scl)
{
    i2c_master_bus_handle_t bus = NULL;
    bmp280_handle_t bmp = NULL;
    aht20_handle_t aht = NULL;
    float bmp_temp, pressure, temperature, humidity;

    esp_err_t err = i2c_bus_init(I2C_NUM_0, sda, scl, &bus);
    if (err != ESP_OK) return err;
    err = bmp280_init(bus, 0x76, &bmp);
    if (err != ESP_OK) goto cleanup;
    err = aht20_init(bus, &aht);  // 使用同一个 bus！
    if (err != ESP_OK) goto cleanup;

    err = bmp280_read(bmp, &bmp_temp, &pressure);
    if (err != ESP_OK) goto cleanup;
    err = aht20_read(aht, &temperature, &humidity);
    if (err != ESP_OK) goto cleanup;

    ESP_LOGI("env", "Temperature: %.2f C, humidity: %.2f %%, pressure: %.2f hPa",
             temperature, humidity, pressure);
    // 对应 MQTT 主题：env/temp、env/humidity、env/pressure。
    // env/temp 选用 AHT20 的温度；bmp_temp 是 BMP280 的温度读数。
cleanup:
    if (aht) aht20_deinit(aht);
    if (bmp) bmp280_deinit(bmp);
    i2c_bus_deinit(bus);
    return err;
}
```

其他自定义组件调用这些接口时，在 CMakeLists.txt 中声明 `PRIV_REQUIRES I2c_bus bmp280 aht20`。本工程 main 使用默认组件依赖，不必额外声明。

## 接口与生命周期

- 创建一次总线，再分别初始化两个传感器。
- 总线频率为 100 kHz，每次 I²C 传输超时为 100 ms，采用同步阻塞调用。
- BMP280 校验芯片 ID、读取校准参数、触发强制测量并补偿温度/气压。
- AHT20 初始化校准状态、触发测量、等待忙状态结束并校验 CRC-8。
- 测量失败时返回 `esp_err_t`，不要发布旧值或未初始化值。
- 同一传感器句柄的读取调用必须串行；多任务使用时应由应用加互斥锁，或统一交给一个采集任务。
- 不再使用时先停止采集，再释放传感器，最后释放总线。正常持续运行应保留句柄，不必每次采集重新初始化。

已验证三个组件可独立编译；实际 GPIO、电气连接和传感器读数仍需上板验证。
