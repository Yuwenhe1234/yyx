# 项目功能与文件映射蓝皮书

---

## 1. 项目一句话定位

本项目是一个**手持式 BLE 无线信号刺激器**，面向生物医学/神经电生理实验场景。用户通过手机 APP 经蓝牙向 ESP32 设备发送指令，ESP32 控制外部精密 DAC 芯片输出直流、正弦波或脉冲波等模拟刺激信号，同时通过外部 ADC 芯片采集实验回路的电压数据并通过蓝牙实时回传至手机。一句话总结：**用手机无线控制一个可编程波形发生器，并实时监控输出与反馈电压**。

---

## 2. 完整目录树与文件职责清单

```
stimulate/                                          ← 项目根目录
├── CMakeLists.txt                                  ← 顶层构建入口（定义项目名 DAC）
├── README.md                                       ← 项目备注（开发时间线）
├── sdkconfig                                       ← ESP-IDF 功能裁剪配置（蓝牙、Flash 等选项）
├── ESP32_DEVKIT_PINOUT_MAP.txt                     ← ESP32 开发板引脚功能速查表
│
├── main/                                           ← 应用程序层（入口 + 主循环）
│   ├── CMakeLists.txt                              ← 注册 main.c，声明依赖 BSP 组件
│   └── main.c                                      ← 程序入口，统筹全部外设和业务逻辑
│
├── components/                                     ← 板级支持包（BSP），所有硬件驱动集中管理
│   └── BSP/
│       ├── CMakeLists.txt                          ← BSP 组件注册（汇总 5 个子模块源文件目录）
│       │
│       ├── BLUETOOTH/                              ← BLE 无线通信驱动
│       │   ├── bluetooth.h                         ← BLE 模块对外接口声明（数据类型、回调签名、公开函数）
│       │   └── bluetooth.c                         ← NimBLE 协议栈初始化和全部蓝牙业务逻辑
│       │
│       ├── ADC/                                    ← ESP32 内置 ADC 驱动（备用，当前未被 main.c 调用）
│       │   ├── adc.h                               ← ADC 模块对外接口声明
│       │   └── adc.c                               ← 片内 ADC1_CH4(GPIO32) 初始化与电压读取
│       │
│       ├── DAC/                                    ← ESP32 内置 DAC 驱动（备用，当前未被 main.c 调用）
│       │   ├── dac.h                               ← DAC 模块对外接口声明
│       │   └── dac.c                               ← 片内 DAC1(GPIO25) 定时器波形输出
│       │
│       ├── KEY/                                    ← 物理按键驱动
│       │   ├── key.h                               ← 按键模块对外接口声明
│       │   └── key.c                               ← BOOT 按键(GPIO0)初始化、消抖读取、长等待
│       │
│       ├── LED/                                    ← 板载指示灯驱动
│       │   ├── led.h                               ← LED 模块对外接口声明
│       │   └── led.c                               ← LED(GPIO2)初始化、亮灭控制、状态翻转
│       │
│       ├── IIC/                                    ← I2C 外部 ADC 芯片驱动
│       │   ├── CMakeLists.txt                      ← 占位文件（源文件由父级 BSP 的 SRC_DIRS 统一管理）
│       │   ├── ads1013.h                           ← ADS1013 驱动对外接口声明 + I2C 引脚和寄存器位定义
│       │   └── ads1013.c                           ← ADS1013 芯片 I2C 通信驱动全部实现
│       │
│       └── SPI/                                    ← SPI 外部 DAC 芯片驱动
│           ├── CMakeLists.txt                      ← 占位文件（源文件由父级 BSP 的 SRC_DIRS 统一管理）
│           ├── dac7311.h                           ← DAC7311 驱动对外接口声明 + SPI 引脚和波形枚举定义
│           └── dac7311.c                           ← DAC7311 芯片 SPI 通信驱动 + GPTimer 波形引擎
│
└── build/                                          ← 编译产物（固件镜像、中间文件等）
```

---

### 2.1 顶层工程文件

#### 文件路径：CMakeLists.txt（项目根目录）

核心职责：声明 ESP-IDF 项目名称（"DAC"），引入 IDF 构建框架，是整个工程的 CMake 入口文件。

关键依赖：被 ESP-IDF 工具链 `idf.py` 在执行 `build` 命令时首先解析；内部通过 `include(project.cmake)` 加载所有组件和目标芯片配置。

---

#### 文件路径：README.md

核心职责：保存开发起始日期和开发意向（"开始 GitHub"、"一个月做小程序"），无技术内容，属于开发者个人笔记。

关键依赖：无。

---

#### 文件路径：sdkconfig

核心职责：ESP-IDF 项目的功能开关配置表，定义哪些子系统启用（如蓝牙 BLE、NVS Flash 存储、WiFi 等），以及各项参数值（Flash 大小、主频、FreeRTOS Tick 频率）。由 `menuconfig` 图形化工具生成和维护。

关键依赖：被 CMake 构建系统在编译期读取，生成条件编译宏（`CONFIG_xxx`），影响 `bluetooth.c` 中 NimBLE 是否编译。

---

#### 文件路径：ESP32_DEVKIT_PINOUT_MAP.txt

核心职责：ESP32 开发板全部 39 个引脚的硬件功能映射速查手册，包括电源、编程接口、Flash 占用、ADC/DAC/I2C/SPI/Touch 等外设信号归属。

关键依赖：被开发者查阅，用于确定各驱动模块的 GPIO 编号配置。

---

### 2.2 应用层（main/）

#### 文件路径：main/CMakeLists.txt

核心职责：将 `main.c` 注册为 ESP-IDF 组件，声明组件公开的头文件路径为当前目录，声明本组件依赖 `driver`（GPIO/I2C/SPI 驱动层）和 `BSP`（板级支持包）。

关键依赖：被顶层 CMakeLists 通过 `project()` 递归加载。

---

#### 文件路径：main/main.c

核心职责：整个固件的业务逻辑总控中心。上电后按顺序执行——①初始化 LED、按键、蓝牙协议栈；②初始化 I2C 总线上的外部 ADC 芯片 ADS1013（4.096V 量程，3300 采样/秒）；③初始化 SPI 总线上的外部 DAC 芯片 DAC7311，并向蓝牙模块注册手机命令回调函数，将手机下发的 DC/SINE/PLU 指令转发给 DAC7311 驱动执行；④进入无限主循环，每 10ms 执行一次按键检测（按键按下则翻转 LED 并上报手机）、一次 I2C ADC 数据采集（将原始值与换算电压毫伏值写入传感器结构体）、一次蓝牙传感器数据推送（将最新 ADC/DAC 状态推送至手机 APP）。

关键依赖：依赖 `led.h`、`key.h`、`bluetooth.h`、`ads1013.h`、`dac7311.h` 共 5 个驱动头文件；被 FreeRTOS 调度器作为 `app_main` 任务运行。

---

### 2.3 BSP 组件（components/BSP/）

#### 文件路径：components/BSP/CMakeLists.txt

核心职责：BSP 组件的注册清单——将 LED、KEY、BLUETOOTH、IIC、SPI 五个子目录注册为源文件搜索路径，同时声明这五个目录为头文件搜索路径；声明组件依赖 ESP-IDF 的 `driver`、`nvs_flash`、`bt` 三个子系统；为所有源文件开启浮点快速数学运算和最高编译优化级别。

关键依赖：被 ESP-IDF 构建系统在扫描 components 目录时加载。

---

#### 2.3.1 蓝牙无线通信模块（BLUETOOTH/）

##### 文件路径：components/BSP/BLUETOOTH/bluetooth.h

核心职责：定义蓝牙模块向外部模块公开的全部数据结构和函数签名。包括：①传感器数据结构体 `sensor_data_t`（含 ADC 原始值、电压毫伏值、DAC 输出值、波形模式、数据有效标志位）；②手机命令回调函数指针类型定义 `bluetooth_command_cb_t`；③六个公开函数声明——蓝牙初始化、发送按键事件、查询连接状态、查询通知订阅状态、推送传感器数据缓存、注册命令回调、主动发送文本通知。

关键依赖：无依赖；被 `main.c` 引入以调用蓝牙服务，被 `bluetooth.c` 实现。

---

##### 文件路径：components/BSP/BLUETOOTH/bluetooth.c

核心职责：BLE 无线通信模块的全部硬件无关业务逻辑实现。分为两个编译路径：

当 `CONFIG_BT_ENABLED` 未开启时（NimBLE 未编译），所有函数退化为桩函数（空操作或返回不支持错误），保证工程在其他配置下仍能编译通过。

当 `CONFIG_BT_ENABLED` 开启时，完整实现以下功能：

- **蓝牙协议栈初始化**：初始化 NVS 非易失存储分区 → 初始化 NimBLE 控制器和主机栈 → 注册 BLE 重置/同步回调 → 初始化 GAP 通用访问配置文件（设置广播设备名为 "ESP32_STIM"）→ 初始化 GATT 通用属性配置文件 → 注册自定义 GATT 服务（UUID=0xFFF0，含 RX 命令通道 0xFFF1 和 TX 通知通道 0xFFF2 两个特性）→ 启动 FreeRTOS 主机任务。

- **GAP 事件处理**：管理 BLE 连接生命周期——手机连接成功时记录句柄并复位通知订阅状态，断开连接时清理通知状态和流式任务并立即恢复广播，订阅事件启用时自动创建传感器数据流式发送任务（每 25ms 推送一次）。

- **GATT 命令解析处理**：手机通过 RX 特性（0xFFF1）下发文本命令。解析到 "ADC" 前缀时从缓存的传感器数据中回报 ADC 原始值和电压毫伏值；解析到 "DC"/"SINE"/"PLU" 前缀时转发到 `main.c` 注册的命令回调函数（回调内部调用 DAC7311 驱动执行输出）；无法识别的命令回复 "ERROR"。

- **传感器数据流式推送**：后台任务每 25ms 执行一次，若通知已订阅且连接有效，将缓存的最新传感器数据（ADC 原始值、ADC 电压、DAC 数值、DAC 当前模式、DAC 参数）格式化为文本字符串，通过 TX 特性（0xFFF2）以 BLE 通知形式推送到手机。

- **传感器数据缓存接口**：提供 `bluetooth_send_sensor_data()` 供 `main.c` 在主循环中调用，将 ADS1013 的最新的 ADC 读数写入模块内部缓存，等待流式任务推送。

- **主动文本通知接口**：提供 `bluetooth_notify_text()` 供 `main.c` 在 BLE 命令处理回调中发送 ACK 确认消息。

关键依赖：依赖 NimBLE 协议栈（`host/ble_hs.h`、`services/gap/ble_svc_gap.h`、`services/gatt/ble_svc_gatt.h`）、FreeRTOS 任务管理、NVS Flash 存储；被 `main.c` 通过公开接口调用。

---

#### 2.3.2 片内 ADC 驱动（ADC/）——备用模块

##### 文件路径：components/BSP/ADC/adc.h

核心职责：定义 ESP32 片内 ADC1 的硬件配置宏（通道 4 即 GPIO32、11dB 衰减对应 0-3.9V 量程、12 位分辨率），声明初始化函数、原始值读取函数和电压换算函数的对外接口。

关键依赖：依赖 ESP-IDF `esp_err.h`；被 `adc.c` 实现。

---

##### 文件路径：components/BSP/ADC/adc.c

核心职责：封装 ESP32 片内 ADC1 通道 4（GPIO32）的底层操作——初始化时配置 12 位采样宽度和 11dB 衰减（0-3.9V 量程）；提供 `adc_read_raw()` 直接读取 0-4095 原始值，以及 `adc_read_voltage()` 将原始值换算为 0-3.9V 电压值。

当前状态：本模块已被外部 ADS1013 芯片替代，`main.c` 中未引用。

关键依赖：依赖 ESP-IDF `driver/adc.h`；暂未被任何模块调用。

---

#### 2.3.3 片内 DAC 驱动（DAC/）——备用模块

##### 文件路径：components/BSP/DAC/dac.h

核心职责：定义 ESP32 片内 DAC 通道 1（GPIO25）的硬件配置宏，声明输出波形枚举类型（直流、正弦、脉冲），公开初始化、设置输出模式、停止输出和内部任务函数的对外接口。

关键依赖：依赖 ESP-IDF `esp_err.h`；被 `dac.c` 实现。

---

##### 文件路径：components/BSP/DAC/dac.c

核心职责：封装 ESP32 片内 DAC1（GPIO25）的底层操作——使用 4000Hz 的硬件定时器周期回调，根据当前波形模式（直流/40Hz 正弦/40Hz 脉冲）在定时器中断服务程序中实时计算每个采样点的电压值，缩放为 0-255 的 8 位 DAC 码值后写入片内 DAC 输出寄存器。

当前状态：本模块已被外部 DAC7311 芯片替代，`main.c` 中未引用。

关键依赖：依赖 ESP-IDF `driver/dac.h`、`esp_timer.h` 和标准数学库 `<math.h>`；暂未被任何模块调用。

---

#### 2.3.4 按键驱动模块（KEY/）

##### 文件路径：components/BSP/KEY/key.h

核心职责：定义 BOOT 按键引脚号（GPIO0），声明四个按键操作函数签名——引脚初始化、原始状态读取、阻塞等待按下、带软件消抖的按下检测。

关键依赖：依赖 ESP-IDF `driver/gpio.h`；被 `key.c` 实现。

---

##### 文件路径：components/BSP/KEY/key.c

核心职责：实现物理按键（BOOT 按钮，GPIO0）的完整操作——初始化时将该引脚配置为内置上拉的输入模式；`key_read()` 直接返回引脚电平（按键按下时引脚被拉到低电平，返回 1）；`key_wait_for_press()` 阻塞直到按键按下（每 10ms 查询一次）；`key_is_pressed()` 执行 20ms 延时软件消抖后确认按键状态，防止机械抖动误判。

关键依赖：依赖 `freertos/FreeRTOS.h` 和 `freertos/task.h` 的延时函数；被 `main.c` 在主循环中每 10ms 调用一次进行按键检测。

---

#### 2.3.5 板载 LED 驱动模块（LED/）

##### 文件路径：components/BSP/LED/led.h

核心职责：定义 LED 引脚号（GPIO2），声明四个 LED 操作函数签名——初始化、点亮、熄灭、翻转状态。

关键依赖：依赖 ESP-IDF `driver/gpio.h`；被 `led.c` 实现。

---

##### 文件路径：components/BSP/LED/led.c

核心职责：实现板载 LED（GPIO2）的基础控制——初始化时将该引脚配置为推挽输出模式并默认熄灭；`led_on()` 输出高电平点亮；`led_off()` 输出低电平熄灭；`led_toggle()` 维护一个模块内静态布尔变量记录当前亮灭状态，翻转该变量后直接设置 GPIO 电平。

关键依赖：依赖 `freertos/FreeRTOS.h` 和 `freertos/task.h`（实际未使用延时，仅引入依赖）；被 `main.c` 在按键检测到按下时调用 `led_toggle()` 实现按键按一次亮再按一次灭的交互。

---

#### 2.3.6 I2C 外部 ADC 驱动模块（IIC/）

##### 文件路径：components/BSP/IIC/ads1013.h

核心职责：定义 ADS1013 芯片驱动对外接口和硬件参数常量。包括：①I2C 总线引脚分配（SCL=GPIO22、SDA=GPIO21，从机地址 0x48，速率 100kHz）；②四个内部寄存器地址（转换结果 0x00、配置 0x01、低阈值 0x02、高阈值 0x03）；③配置寄存器中各位的位置偏移宏和选项常量（满量程范围从 ±6.144V 到 ±0.256V 共 6 档、采样率从 128 SPS 到 3300 SPS 共 7 档）；④设备句柄结构体（封装 I2C 总线句柄和设备句柄）；⑤七个公开函数声明——初始化和反初始化、读原始转换值、原始值转毫伏换算、读写配置寄存器。

关键依赖：依赖 ESP-IDF `driver/i2c_master.h`；被 `ads1013.c` 实现，被 `main.c` 引入调用。

---

##### 文件路径：components/BSP/IIC/ads1013.c

核心职责：实现 ADS1013（12 位 I2C 模数转换器）的完整驱动逻辑：

- **初始化流程（ads1013_init）**：分配句柄内存 → 初始化 I2C0 总线硬件（开启内部弱上拉，7 个时钟周期的毛刺过滤）→ 将 ADS1013 设备挂载到 I2C 总线（7 位地址 0x48，100kHz 速率）→ 写入配置寄存器（单端输入 AIN0 vs GND，±4.096V 满量程，连续转换模式，3300 SPS 采样率，禁用比较器）→ 回读配置寄存器并校验（掩码忽略 OS 位差异）→ 返回句柄。

- **运行时读取（ads1013_read_raw）**：通过 I2C 组合读写操作从转换结果寄存器（0x00）读取 2 字节 16 位原始数据，右移 4 位并掩码取出 12 位有效值（0-4095）。

- **电压换算（ads1013_raw_to_voltage_mv）**：纯计算函数，将 12 位二进制补码格式的原始值解码为有符号整数（bit11 为符号位，负值执行补码转原码），根据满量程 PGA 参数按比例换算为毫伏值（公式：signed_value × PGA_range ÷ 2048），使用 32 位中间整数防溢出。

- **配置读写（ads1013_write_config / ads1013_read_config）**：对配置寄存器（0x01）的底层 I2C 读写封装，调试时用于校验配置写入的正确性。

- **资源释放（ads1013_deinit）**：从 I2C 总线移除设备 → 释放 I2C 总线硬件 → 释放句柄堆内存。

关键依赖：依赖 ESP-IDF `driver/i2c_master.h` 的 I2C 主机驱动 API；被 `main.c` 调用初始化、读取和释放。

---

#### 2.3.7 SPI 外部 DAC 驱动模块（SPI/）

##### 文件路径：components/BSP/SPI/dac7311.h

核心职责：定义 DAC7311 芯片驱动对外接口和硬件参数常量。包括：①SPI 总线引脚分配（SCLK=GPIO18、MOSI=GPIO23、MISO=GPIO19、CS=GPIO5，主机 SPI2，时钟 10MHz）；②芯片参数（12 位分辨率，最大码值 4095，参考电压 3.3V）；③波形模式枚举（无波形、正弦波、脉冲波）；④设备句柄结构体（封装 SPI 设备句柄、GPTimer 句柄、波形管理任务句柄、512 点正弦查找表、脉冲高低码值等）；⑤十个公开函数声明——生命周期管理（初始化、反初始化）、底层写入（原始值写入、电压值写入）、直流输出设定、正弦波启动、脉冲波启动、波形停止、芯片上下电。

关键依赖：依赖 ESP-IDF `driver/spi_master.h`、`driver/gptimer.h` 和 FreeRTOS 任务管理；被 `dac7311.c` 实现。

---

##### 文件路径：components/BSP/SPI/dac7311.c

核心职责：实现 DAC7311（12 位 SPI 数模转换器）的完整驱动逻辑，是本项目最复杂的硬件驱动模块。采用**双核架构**确保 BLE 无线通信不受波形生成中断的干扰：

- **双核隔离策略**：Core 0 专用于 BLE NimBLE 协议栈运行（由 ESP-IDF 蓝牙子系统管理）；Core 1 专用于波形生成的 GPTimer 中断服务程序（ISR）和对 SPI 总线的直接轮询传输。由于 ESP32 的 GPTimer ISR 自动绑定到创建定时器的任务所在核心上，驱动在 Core 1 上创建波形管理任务（`waveform_mgr_task`），该任务再创建 GPTimer，从而将 ISR 固定在 Core 1，确保 Core 0 的中断响应零干扰。

- **初始化流程（dac7311_init）**：分配并清零句柄内存 → 初始化 SPI2 总线（MOSI+SCLK+MISO 三线）→ 注册 SPI 设备（SPI Mode 0，10MHz，CS 引脚由硬件自动控制）→ 在 Core 1 上创建波形管理任务 → 预计算 2000mVpp 的 512 点正弦波查找表 → 写入 0 码值确保 DAC 上电后默认为 0V 输出 → 返回句柄。

- **直流设定（dac7311_set_dc）**：先停止当前波形输出，将用户输入的目标电压毫伏值等比换算为 0-4095 的 12 位 DAC 码值（公式：mV × 4096 ÷ 3300），通过 SPI 单次轮询传输发送到 DAC7311 芯片。

- **正弦波输出（dac7311_start_sine）**：停止当前波形 → 若振幅变化则重建 512 点正弦查找表（以电压中心点为基准，叠加振幅正弦值，夹逼至 0-4095 范围）→ 设置定时器周期为 1 ÷（频率 × 512）微秒 → 设置波形模式为 SINE → 通知 Core 1 管理任务启动 GPTimer。ISR 运行时每次触发从查找表中取下一个预格式化的 16 位 SPI 帧直接发送，相位递增并循环。

- **脉冲波输出（dac7311_start_pulse）**：停止当前波形 → 计算高电平码值（振幅 × 4096 ÷ 3300）和低电平码值（0）→ 设置定时器周期为 1 ÷（频率 × 2）微秒 → 设置波形模式为 PULSE → 通知 Core 1 管理任务启动 GPTimer。ISR 运行时交替发送高码值和低码值。

- **波形管理任务（waveform_mgr_task）**：运行在 Core 1 上的循环任务，收到 FreeRTOS 任务通知后——①创建 GPTimer 定时器（1MHz 分辨率基准）→ ②配置为自动重载的周期报警模式 → ③注册 ISR 回调 → ④使能并启动定时器 → ⑤进入阻塞等待，直到收到停止或模式变更通知 → ⑥停止、禁用、删除定时器，回到循环起点等待下一个命令。

- **GPTimer ISR 回调（waveform_timer_isr）**：运行在 Core 1 上，标记为可放入 IRAM 以加速执行。根据波形模式直接从句柄中取预格式化的 SPI 帧数据，调用 `spi_device_polling_transmit()` 直接操作 SPI 硬件寄存器发送；正弦模式下顺序遍历查找表，脉冲模式下交替翻转高低状态。不涉及任何 FreeRTOS API 调用，确保中断退出延迟极低。

- **资源释放（dac7311_deinit）**：停止波形 → 删除 Core 1 管理任务 → 从 SPI 总线移除设备 → 释放 SPI 总线硬件 → 释放句柄。

关键依赖：依赖 ESP-IDF `driver/spi_master.h`（SPI 轮询传输）、`driver/gptimer.h`（硬件定时器报警回调）、FreeRTOS 任务通知和核心绑定机制、标准数学库 `<math.h>`；被 `main.c` 调用初始化，并通过 BLE 命令回调函数间接调用 DC/SINE/PLU 启动函数。

---

## 3. 核心业务数据流向文字推演

### 3.1 场景一：用户用手机 App 下发 "SINE" 指令启动正弦波输出

第一步——手机 App 通过 BLE 连接到 ESP32 设备（设备名 "ESP32_STIM"），连接成功后 ESP32 端触发 `BLE_GAP_EVENT_CONNECT` 事件，记录连接句柄，复位通知订阅状态。

第二步——用户在手机 App 中输入并发送文本指令 "SINE"。该指令作为 GATT 写操作到达 ESP32 的 RX 特性（UUID 0xFFF1），NimBLE 主机栈触发 `gatt_svr_chr_access_cb` 回调，识别操作类型为写特征值，从 BLE 接收缓冲区取出 "SINE" 字符串，转发给 `handle_text_command` 函数。

第三步——`handle_text_command` 函数去除尾部空白字符后，检查命令前缀。"SINE" 匹配 `strcasecmp` 判断，确认命中 DAC 命令类型（前缀为 DC/SINE/PLU 之一），调用 `main.c` 预先注册的回调函数 `ble_command_handler`，同时传入命令文本和 DAC 驱动句柄。

第四步——`ble_command_handler`（位于 `main.c`）用 `strcasecmp` 匹配 "SINE"，调用 `dac7311_start_sine(dac, 40, 2000)`，启动频率为 40Hz、振幅为 2000mVpp 的正弦波输出。

第五步——`dac7311_start_sine` 先停止当前波形输出，检查查找表是否需要重建，将定时器周期设为 1 微秒 ÷（40Hz × 512 点）≈ 48.8 微秒/点，将波形模式设为 SINE，然后向 Core 1 上的波形管理任务发送 FreeRTOS 任务通知。

第六步——Core 1 上的 `waveform_mgr_task` 被唤醒，创建 1MHz 分辨率的 GPTimer 定时器并设置自动重载周期报警，注册 ISR 回调 `waveform_timer_isr`，启动定时器。每约 48.8 微秒触发一次 ISR，ISR 从预格式化的 512 点正弦查找表中取下一个 SPI 帧，调用 `spi_device_polling_transmit` 直接操作 SPI2 硬件发送到 DAC7311 芯片，DAC7311 芯片收到 SPI 帧后立即更新模拟输出电压。

第七步——正弦波启动成功后控制返回到 `ble_command_handler`，回调函数通过 `bluetooth_notify_text("ACK SINE 40Hz 2Vpp")` 发送确认消息到手机。流式任务 `streaming_task` 在下一轮 25ms 周期到来时将最新缓存数据（含 DAC 模式=SINE 和参数=2000mVpp）通过 TX 特性以 BLE 通知推送至手机。

### 3.2 场景二：主循环持续采集 ADC 数据并推送至手机

第一步——`app_main` 在完成所有外设初始化后进入 `while(1)` 无限循环，每 10ms（`vTaskDelay(1)`，FreeRTOS Tick 10ms）执行一次全部操作。

第二步——循环体首先调用 `key_is_pressed()` 检测 BOOT 按钮（GPIO0）是否按下。函数内部先读取 GPIO0 电平（按键按下时低电平），然后等待 20ms 消抖后再次读取，若两次均为低电平则确认按下列有效，返回 1。

第三步——若确认按下列有效，`main.c` 调用 `led_toggle()` 翻转板载 LED（GPIO2）的亮灭状态，同时调用 `bluetooth_send_key_event(1)` 尝试通过 BLE 向手机发送按键事件通知（仅当 BLE 已连接且通知已订阅且流媒体已开启时才实际发送）。

第四步——随后执行 I2C ADC 采集。`main.c` 检查 `ads1013` 句柄是否有效，若有效则依次调用 `ads1013_read_raw()` 从 ADS1013 芯片读取 12 位原始 ADC 值和 `ads1013_raw_to_voltage_mv()` 将原始值换算为毫伏电压值，填充 `sensor_data_t` 结构体的 adc_raw、adc_voltage_mv 字段并置 adc_valid 为真。

第五步——检查 adc_valid 标志，若为真则调用 `bluetooth_send_sensor_data(&sensor)`，将传感器结构体数据 memcpy 到蓝牙模块内部缓存 `g_latest_sensor_data` 中。

第六步——蓝牙模块的 `streaming_task` 后台任务在自己的 25ms 周期到达时，检查 BLE 通知订阅状态和连接状态，若满足条件则从 `g_latest_sensor_data` 中取出各字段值，格式化为 "SENSOR ADC_RAW=xxx ADC_MV=xxx DAC=xxx MODE=xxx PARAM=xxx" 字符串，调用 NimBLE 的 `ble_gatts_notify_custom` 通过 TX 特性推送至手机。

第七步——每轮循环以 `vTaskDelay(1)` 结束，FreeRTOS 调度器将 CPU 让给其他任务（BLE 主机任务、流式任务、空闲任务等），10ms 后重新唤醒主任务开始下一轮循环。

---

## 4. 核心数据实体（字段定义）

### 4.1 传感器数据结构体（sensor_data_t）

该结构体定义在 [bluetooth.h](components/BSP/BLUETOOTH/bluetooth.h)，是 `main.c` 与蓝牙模块之间传递传感器读数的唯一数据载体。

| 字段名 | 业务含义 |
|---|---|
| adc_raw | ADS1013 外部 ADC 芯片最新一次采样的 12 位原始二进制值，范围 0 至 4095，正满量程值 2047 对应 +4.096V，0 对应 0V（单端模式下负值在补码范围内不出现） |
| adc_voltage_mv | 由原始值经二进制补码解码和 PGA 量程换算后的电压值，单位毫伏，可为负值（在差分模式下），用于手机 App 直接显示物理量 |
| dac_value | DAC7311 外部 DAC 芯片当前的输出码值，范围 0 至 4095，对应 0V 至 3.3V 模拟输出电压 |
| dac_mode | DAC 当前波形模式编号：0 表示直流模式，1 表示正弦波模式，2 表示脉冲波模式 |
| dac_param | DAC 当前输出参数：直流模式下存放目标电压毫伏值，正弦波和脉冲波模式下存放峰-峰电压值（单位毫伏） |
| adc_valid | 数据新鲜标志：为真时表示 adc_raw 和 adc_voltage_mv 是本轮主循环最新读取的有效数据，蓝牙模块可据此判断是否需要推送 |
| dac_valid | 数据新鲜标志：为真时表示 dac_value 和 dac_mode 和 dac_param 字段已被更新，留给未来扩展，当前版本主要由 streaming_task 在每 25ms 推送周期内主动从 DAC7311 驱动句柄读取 |

### 4.2 ADS1013 芯片配置寄存器（16 位）

该配置寄存器的各位定义在 [ads1013.h](components/BSP/IIC/ads1013.h) 中以宏常量表示，由 `ads1013_init` 写入芯片后决定其全部工作行为。

| 位段 | 位宽 | 业务含义 |
|---|---|---|
| OS（bit 15） | 1 位 | 操作状态与启动位：写 1 启动单次转换，读 1 表示转换完成数据就绪，读 0 表示正在转换中；在连续转换模式下该位无实际作用 |
| MUX（bit 14-12） | 3 位 | 输入通道多路选择器配置：000 选择模拟输入 0 对地单端输入，001-011 分别选择模拟输入 1/2/3 对地单端，100-111 为差分通道组合；本项目固定为 000（AIN0 vs GND） |
| PGA（bit 11-9） | 3 位 | 可编程增益放大器设置，决定满量程电压范围：000=±6.144V，001=±4.096V，010=±2.048V，依次减半至 101=±0.256V；本项目使用 001（±4.096V） |
| MODE（bit 8） | 1 位 | 转换模式：0 为连续转换（芯片自动不间断采样），1 为单次转换（写 OS 位 1 后执行一次即停止）；本项目设为 0（连续模式） |
| DR（bit 7-5） | 3 位 | 数据输出速率即采样频率：000=128 SPS，依次递增至 110=3300 SPS（每秒 3300 次）；本项目设为 110（3300 SPS） |
| COMP（bit 4-0） | 5 位 | 内部比较器功能配置（模式、极性、是否锁存、触发队列长度）：本项目全部置为禁用状态（比较器队列=11），不使用 ADS1013 的硬件比较器报警功能 |

### 4.3 DAC7311 设备句柄（dac7311_handle_t）

该结构体定义在 [dac7311.h](components/BSP/SPI/dac7311.h)，封装了外部 DAC 芯片运行时的全部状态，是 SPI 驱动层和波形引擎层的共享上下文。

| 字段名 | 业务含义 |
|---|---|
| spi_handle | ESP-IDF SPI 主模式设备句柄，指向挂载在 SPI2 总线上的 DAC7311 从设备，用于后续所有 SPI 数据收发操作 |
| waveform_mode | 当前波形输出模式：DAC7311_MODE_NONE（无波形，即停止状态）、DAC7311_MODE_SINE（正弦波输出中）、DAC7311_MODE_PULSE（脉冲波输出中） |
| waveform_period_us | 当前波形输出所对应硬件定时器的报警周期，单位为微秒；正弦波时为 1÷(频率×512点)，脉冲波时为 1÷(频率×2个半周期) |
| waveform_timer | ESP-IDF GPTimer 通用定时器句柄，运行在 Core 1 上，提供周期报警以驱动波形 ISR 以精确时间间隔发送 SPI 数据 |
| waveform_task | 波形管理 FreeRTOS 任务句柄，运行在 Core 1 上，负责按需创建/回收 GPTimer 定时器，通过任务通知机制与外部调用者同步 |
| sine_lut[512][2] | 正弦波 512 点预格式化查找表，每个点已预先编码为完整的 16 位 SPI 帧（含 2 位命令 + 12 位 DAC 码值的 MOSI 线上数据），存储为 2 字节高位在前格式，供 ISR 零计算量直接发送 |
| sine_phase | 正弦查找表当前相位索引，范围 0 至 511，每次 ISR 触发后递增 1 并取模，实现波形连续循环输出 |
| sine_freq_hz | 当前正弦波输出频率，单位赫兹，由手机 App 下发指定（当前默认固定为 40Hz） |
| sine_amplitude_mv | 当前正弦波振幅峰-峰值，单位毫伏，用于判断查找表是否需要重建（振幅改变时触发 build_sine_lut 重新计算） |
| pulse_high_code | 脉冲波高电平期间对应的 12 位 DAC 码值，单位毫伏换算为码值（公式 mV × 4096 ÷ 3300） |
| pulse_low_code | 脉冲波低电平期间对应的 12 位 DAC 码值，通常为 0 对应 0V 输出 |
| pulse_state | 脉冲波 ISR 内部状态标志位：真表示当前输出为高电平半周期，假表示低电平半周期，每次 ISR 触发后翻转一次 |

### 4.4 蓝牙 GATT 服务与特性定义

该服务表定义在 [bluetooth.c](components/BSP/BLUETOOTH/bluetooth.c) 中以静态结构体数组描述，编译期固定不变。

| 字段/条目 | 业务含义 |
|---|---|
| 主服务 UUID（0xFFF0） | 本项目自定义蓝牙 GATT 主服务标识，手机端 App 据此发现和识别 ESP32 刺激器设备 |
| RX 特性 UUID（0xFFF1） | 手机向 ESP32 写入命令的数据通道：支持读操作（读取最近一次通知内容缓存）和写操作（下发 DC/SINE/PLU/ADC 文本指令），无需写入响应 |
| TX 特性 UUID（0xFFF2） | ESP32 向手机推送数据的通知通道：支持读操作（读取最近一次缓存文本）和通知订阅（手机订阅后 ESP32 可主动推送传感器数据流和状态确认消息） |
| 设备广播名称（"ESP32_STIM"） | BLE 广播包中携带的设备人类可读名称，手机 App 扫描时据此筛选并显示给用户 |
