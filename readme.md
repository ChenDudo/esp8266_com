# esp8266_com

ESP8266 通用联网 / 配网模块：上电自动联网；连不上就自己开热点 + 配网网页，
手机提交 WiFi 密码后立刻联网，成功才写 Flash，之后掉电重启自动联网。
对外通过串口与 MCU 交互。

## 设计需求

设备每次重新上电时检查是否能联网；如果不能联网，就自己创建一个热点，
让手机连接这个热点，然后在网页上提交 WiFi 密码，设备根据提交的密码联网。

## 硬件连接

MCU 通过串口连接至 ESP8266，使用**自定义文本行协议**交互。

> 原始记录写的是"AT 命令交互"。按决策 D1（改走自定义固件），这里已经不再是原厂 AT 固件，
> 而是一套自定义的文本行协议。接线表见 `docs/provisioning_design.md` §2.4，
> 协议规范见 `docs/uart_protocol.md`。

## 文档与代码

| 位置 | 内容 |
| --- | --- |
| [docs/provisioning_design.md](docs/provisioning_design.md) | 配网方案设计、主流做法对比、决策记录（**先看这个**） |
| [docs/uart_protocol.md](docs/uart_protocol.md) | MCU ↔ ESP8266 串口协议规范（唯一权威） |
| [firmware/](firmware/) | ESP8266 侧固件（PlatformIO + Arduino core）：配网门户、凭据存储、联网状态机、串口协议 |
| [host/](host/) | MCU 侧驱动（HC32L196，纯 C）：移植层 / 协议解析 / 复位握手与状态机 |
| [platform/hc32l196/](platform/hc32l196/) | HC32L196 的 Keil MDK 工程（含官方 DDL 子集）：**打开就能编译下载** |

## 快速上手

ESP8266 侧（配网 + 联网逻辑，跑在模块里）：

```bash
pip install platformio
pio run -d firmware -t upload      # 编译 + 烧录 ESP8266
pio device monitor -b 115200       # 看调试日志（UART1 TX = GPIO2）
```

MCU 侧（HC32L196）：用 Keil uVision5 打开
`platform/hc32l196/example/MDK/esp_hc32l196.uvprojx`，F7 编译、F8 下载。
更多接线 / 灯语 / 排查见 [platform/hc32l196/README.md](platform/hc32l196/README.md)。

有一个必须记住的硬件前提：**ESP8266 只有 UART0 能同时收发**，所以与 MCU 的协议口
就是模块的下载口（GPIO1 TX / GPIO3 RX），调试日志单独从 GPIO2 出，RST 由 MCU 控制。

## 当前状态

- ✅ `firmware/`（ESP8266 固件）与 `host/`（HC32L196 驱动）代码已完成
- ✅ `platform/hc32l196/` Keil 工程（含 DDL 子集与示例 `main.c`），打开即可编译下载
- ✅ `host/` + 示例 `main.c` 已用 arm-none-eabi-gcc + 真实 DDL 头文件通过语义检查（两种调试串口配置都测过）
- ⚠️ `firmware/` 尚未编译过（本机没有 PlatformIO），第一次编译的注意事项见 `firmware/README.md`
- ⬜ 上板联调；测试用例 `docs/test_plan.md` 待写
