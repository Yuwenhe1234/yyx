# ESP32 BLE 无线信号刺激器

## 项目简介

基于 ESP32 + NimBLE 的便携式无线可编程波形发生器。手机 APP 通过 BLE 下发指令，ESP32 控制外部精密 DAC 芯片（DAC7311）输出直流/正弦波/脉冲波等模拟刺激信号，同时通过外部 ADC 芯片（ADS1013）实时采集回路电压并通过蓝牙回传至手机，适用于生物医学电生理实验场景。

## 技术栈

| 项目 | 版本/型号 |
|---|---|
| 芯片 | ESP32 (Xtensa 双核, 160MHz) |
| 框架 | ESP-IDF v5.2.6 |
| BLE 协议栈 | NimBLE (Controller + Host, 绑定 Core 0) |
| 编译工具链 | xtensa-esp-elf |
| Flash | 2MB, DIO 模式, 40MHz |

## 硬件接线

### I2C — ADS1013 外部 ADC（12 位, 12-bit, 3300 SPS, ±4.096V）

| ESP32 引脚 | ADS1013 引脚 | 说明 |
|---|---|---|
| GPIO21 (SDA) | 4 脚 SDA | 数据线 |
| GPIO22 (SCL) | 3 脚 SCL | 时钟线, 100kHz |
| GND | 2 脚 ADDR | 地址选择（接地 = 0x48） |
| 3.3V | 5 脚 VDD | 供电 |

### SPI — DAC7311 外部 DAC（12 位, 12-bit, Vref=3.3V）

| ESP32 引脚 | DAC7311 引脚 | 说明 |
|---|---|---|
| GPIO5 | CS / SYNC | 片选 |
| GPIO18 | SCLK | 时钟, 10MHz |
| GPIO23 | MOSI | 主机数据输出 |
| GPIO19 | MISO | 主机数据输入（未实际使用, unused） |

### 其他外设

| ESP32 引脚 | 外设 | 说明 |
|---|---|---|
| GPIO0 | BOOT 按键 | 低电平有效, 内置上拉, 20ms 消抖 |
| GPIO2 | LED 指示灯 | 高电平点亮, 按键按一次翻转一次 |
| GPIO25 | ESP32 片内 DAC1 | 备用（当前代码用 DAC7311 替代）|
| GPIO32 | ESP32 片内 ADC1_CH4 | 备用（当前代码用 ADS1013 替代）|

## 双核架构

| 核心 | 职责 | 关键任务 |
|---|---|---|
| **Core 0** | BLE 通信 | NimBLE Host 任务、app_main 主循环、streaming_task（25ms 周期传感器推送） |
| **Core 1** | 波形生成 | DAC7311 管理任务 → 创建 GPTimer → ISR 直接轮询 SPI 发送波形数据 |

Core 1 的 GPTimer ISR 直接操作 SPI 硬件寄存器，不经过 FreeRTOS API，最大程度降低延迟和对 Core 0 的干扰，确保 BLE 通信稳定。

## BLE GATT 服务定义

| 属性 | UUID | 操作 | 说明 |
|---|---|---|---|
| Primary Service | 0xFFF0 | — | 自定义主服务 |
| RX Characteristic | 0xFFF1 | Write / WriteNoRsp / Read | 手机下发命令通道 |
| TX Characteristic | 0xFFF2 | Notify / Read | ESP32 推送数据通道 |

广播设备名：**ESP32_STIM**

## 手机端 BLE 命令

| 命令格式 | 示例 | 功能 | 限制 |
|---|---|---|---|
| `DC<电压>` | `DC1.50` | 设置直流输出 1.50V | 0 ~ 2.0V |
| `SINE` | `SINE` | 启动正弦波输出 | 固定 40Hz, 2Vpp |
| `PLU` | `PLU` | 启动脉冲波输出 | 固定 40Hz, 50% 占空比, 2Vpp |
| `ADC` | `ADC` | 查询 ADS1013 当前 ADC 读数 | 返回原始值和电压毫伏值 |

ESP32 确认 (ACK) 和传感器数据通过 TX 特性 (0xFFF2) 以 Notify 方式推送。

## 传感器数据推送格式

流式任务每 25ms（40 SPS 有效更新率）推送一帧：

```text
SENSOR ADC_RAW=2047 ADC_MV=2048 DAC=2047 MODE=SINE PARAM=2000
```

- `ADC_RAW`：ADS1013 12 位原始值 (0-4095)
- `ADC_MV`：经补码解码 + PGA 换算的电压 (mV)
- `DAC`：DAC7311 当前输出码值 (0-4095)
- `MODE`：DC / SINE / PULSE
- `PARAM`：DC 模式为 mV 值，SINE/PULSE 模式为 mVpp

## 目录结构

```
stimulate/
├── CMakeLists.txt                 ← 顶层工程入口（项目名：DAC）
├── sdkconfig                      ← ESP-IDF 功能配置（BSP 组件用 -ffast-math -O3 编译）
├── main/
│   ├── CMakeLists.txt             ← 注册 main.c，依赖 BSP 组件
│   └── main.c                     ← 程序入口，全部业务逻辑总控
├── components/BSP/
│   ├── CMakeLists.txt             ← BSP 组件注册（汇总 5 个子模块，声明 driver/nvs_flash/bt 依赖）
│   ├── BLUETOOTH/
│   │   ├── bluetooth.h            ← 传感器结构体 + 回调接口 + 公开函数声明
│   │   └── bluetooth.c            ← NimBLE 完整实现（协议栈初始化/GAP 事件/GATT 命令解析/流式推送）
│   ├── IIC/
│   │   ├── ads1013.h              ← ADS1013 引脚/寄存器/句柄声明
│   │   └── ads1013.c              ← ADS1013 I2C 驱动（3300 SPS, ±4.096V, AIN0 vs GND 单端）
│   ├── SPI/
│   │   ├── dac7311.h              ← DAC7311 引脚/波形枚举/句柄声明
│   │   └── dac7311.c              ← DAC7311 SPI 驱动 + Core 1 GPTimer 波形引擎（正弦 LUT 512 点）
│   ├── KEY/
│   │   ├── key.h                  ← BOOT 按键接口声明
│   │   └── key.c                  ← GPIO0 消抖按键驱动
│   ├── LED/
│   │   ├── led.h                  ← LED 接口声明
│   │   └── led.c                  ← GPIO2 亮灭/翻转控制
│   ├── ADC/                       ← 备用（ESP32 片内 ADC，已被 ADS1013 替代）
│   │   ├── adc.h
│   │   └── adc.c
│   └── DAC/                       ← 备用（ESP32 片内 DAC，已被 DAC7311 替代）
│       ├── dac.h
│       └── dac.c
└── build/                         ← 编译产物（idf.py build 自动生成）
```

## 快速开始

### 编译

```bash
idf.py build
```

### 烧录并查看日志

```bash
idf.py -p COM3 flash monitor
```

BSP 组件已配置 `-ffast-math -O3` 优化选项，确保波形查找表计算和浮点运算效率最高。

## 关键参数速查

| 参数 | 值 |
|---|---|
| FreeRTOS Tick | 100Hz (10ms/Tick) |
| 主循环周期 | ~10ms (vTaskDelay 1 Tick) |
| BLE 传感器推送周期 | 25ms (40 SPS) |
| ADS1013 采样率 | 3300 SPS |
| DAC7311 正弦波 LUT 点数 | 512 |
| 正弦/脉冲波频率 | 40Hz |
| BLE 连接间隔 (PPCP) | 20ms ~ 50ms |
| NimBLE MTU | 256 字节 |
| CPU 主频 | 160MHz |
