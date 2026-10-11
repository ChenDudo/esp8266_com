# platform/hc32l196 —— HC32L196 的 Keil MDK 工程

**打开就能编译、能下载**的参考工程：把根目录 `host/` 里的驱动接上，
主循环基本只做两件事 —— `Esp_Poll()` + 看 `Esp_Status()`。
官方 DDL 已经一起放进仓库（`ddl/`），换台机器、别人克隆下来也能直接编。

## 一、打开与编译

用 Keil uVision5 打开 **`example/MDK/esp_hc32l196.uvprojx`**，然后检查三处：

| 位置 | 应该是什么 | 不对怎么办 |
| --- | --- | --- |
| Options → Device | `HC32L196PCTA`（`HDSC.HC32L19x` 器件包） | 用 Pack Installer 装包，源：`https://raw.githubusercontent.com/hdscmcu/pack/master/` |
| Options → Target | IROM `0x0000`-`0x40000`（256KB），IRAM `0x20000000`-`0x8000`（32KB） | 已配好；换封装（非 PCTA）时这里会跟着器件自动更新 |
| Options → Debug | **你自己用的仿真器** | 工程沿用了官方例程的默认设置，不一定是你的硬件，改成 J-Link / 板载 DAP 等 |

然后 F7 编译、F8 下载。

> 编译时会有一条 `warning: return type of 'main' is not 'int' [-Wmain]`。
> 这是官方 DDL 例程统一写 `int32_t main(void)` 带来的，**无害**，可以忽略。

## 二、硬件接线

| ESP8266 | 方向 | HC32L196 | 说明 |
| --- | --- | --- | --- |
| GPIO1 (UART0 TX) | ESP → MCU | **PA03**（UART1_RX） | 模块发来的应答 / 事件 |
| GPIO3 (UART0 RX) | MCU → ESP | **PA02**（UART1_TX） | MCU 发出去的命令 |
| RST | MCU → ESP | **PB00** | 低电平复位；**建议该脚再并 10k 上拉到 3.3V** |
| GPIO2 | ESP → 调试 | 可选，接 USB-TTL 的 RX | 模块自己的日志，115200，只发不收 |
| GND / VCC | — | 共地 / 3.3V | 模块峰值电流约 300mA，电源要有余量 |
| EN / CH_PD | — | 10k 上拉 3.3V | |
| GPIO0 | — | 10k 上拉 3.3V | 下拉才进烧录模式 |
| GPIO15 | — | 10k 下拉 GND | **必须** |

引脚要改的话，改 `../../host/port/hc32l19x/esp_port_hc32l19x.c` 顶部那 4 个宏
（`ESP_TX_PORT/PIN`、`ESP_RX_PORT/PIN`、`ESP_RST_PORT/PIN`）。

## 三、快速验证（不看代码也能确认通没通）

1. 烧录后上电，板上 LED 应该先**超快闪**（正在复位 / 等模块握手），随后变**慢闪**
   —— 慢闪表示模块已经开了配网热点在等人。
2. 手机连热点 `MyDev-xxxx`（密码 `12345678`），浏览器打开 `192.168.4.1`。
3. 选你的 WiFi、填密码、提交。几秒后 LED 变**常亮** = 已联网。
4. 断电重上电，灯应该直接常亮（凭据已存在模块 Flash 里）。

不想用手机，也可以在 Keil 里加两个 watch：

| 变量 | 看什么 |
| --- | --- |
| `Esp_Status()->state` | `RESET / WAIT_READY / OFFLINE / PROV / ONLINE / ERROR` |
| `s_last_event` | 最近一条事件文本，例如 `esp: ONLINE` |

## 四、灯语

| 灯的闪法 | 对应状态 | 含义 |
| --- | --- | --- |
| 常亮 | ONLINE | 已联网（外网不通时内部会置 `degraded`，灯不变） |
| 慢闪 1s | PROV | 配网模式：让用户连 `st->ap_ssid` 这个热点 |
| 快闪 200ms | OFFLINE | 正在连路由器 / 掉线重连 |
| 超快闪 100ms | RESET、WAIT_READY | 正在复位模块 / 等 `@EVT READY` |
| 熄灭 | ERROR | 连续 3 次复位都收不到模块响应 |

## 五、工程结构

```
platform/hc32l196/
├─ ddl/                          # 官方 HC32L19x DDL 子集（BSD-3，见 ddl/README.md）
│  ├─ driver/inc, driver/src     # 外设驱动（32+32 个文件都在，方便以后加 ADC/SPI）
│  └─ mcu/common                 # 器件头文件、system/interrupts、STK 板定义
├─ example/
│  ├─ MDK/
│  │  ├─ esp_hc32l196.uvprojx    # ← 双击这个打开
│  │  └─ startup_hc32l19x.s      # 中断向量表 / 启动代码
│  └─ source/
│     ├─ main.c                  # 示例应用（状态灯 + 按键重新配网 + 业务占位）
│     └─ ddl_device.h            # 器件系列选择（DDL 必须）
└─ README.md
```

Keil 工程里的分组：`startup` / `mcu_common` / `ddl_driver` / `host_esp` / `app` / `CMSIS` / `docs`。

## 六、常见问题

**灯一直超快闪（在 RESET / WAIT_READY 之间循环）**
MCU 收不到模块的 `@EVT READY`。按顺序查：

1. RST 接线与极性：PB00 → ESP 的 RST，**低电平复位**，且 RST 上要有 10k 上拉到 3.3V。
2. 串口是否交叉：ESP 的 TX(GPIO1) → PA03(RX)，ESP 的 RX(GPIO3) → PA02(TX)。
3. 是否共地；ESP 的 EN / CH_PD 是否拉高；GPIO15 是否下拉。
4. 用 USB-TTL 接 ESP 的 GPIO2（115200，只收）看模块自己的日志。

**灯超快闪之后熄灭（ERROR）**
连续 3 次复位都没有任何响应，原因同上。也可以先只给 ESP 上电（不接 MCU），
用 USB-TTL 看它是否正常启动并打印日志。

**慢闪，但手机连上热点打不开 192.168.4.1**
先在 watch 里看 `Esp_Status()->ap_ssid` 有没有值。部分手机会提示"此网络无互联网连接"，
选择"继续使用"即可；也可以直接在浏览器地址栏手输 `192.168.4.1`。

**想打开调试打印**
`example/source/main.c` 顶部把 `APP_DEBUG_UART` 改成 `1`（或编译时加 `-DAPP_DEBUG_UART=1`），
并把 `APP_DBG_TX_PIN` 改成你板上 UART0 TX 实际引出的脚（L196 上可选 PA08 / PA09 是 AF1、
PA14 是 AF2），接 USB-TTL 就能看到状态日志，115200 8N1。

**想加别的外设（ADC / SPI / 定时器…）**
DDL 源码都在 `ddl/driver/src/`，在 Keil 里右键分组 → Add Existing Files 加进来即可，
头文件路径已经配好（`ddl/driver/inc`）。

**改了代码但下载后没变化**
检查 Options → Utilities → Settings 里的 Flash 算法是不是 `HC32L19x_256KB`，
以及是否勾了 Reset and Run。

## 七、这个工程改了哪些默认设置

工程文件是在官方 `example/uart/uart_int` 的工程基础上改的。除了下面这些，
器件型号、Flash 算法、AC6 编译器设置、`--keep=*Handler` 链接选项都和原例程一致：

| 项 | 原例程 | 本工程 |
| --- | --- | --- |
| Target / 输出名 | `uart_int_Release` / `uart_int` | `esp_hc32l196_Release` / `esp_hc32l196` |
| 输出目录 | `.\output\release\` | `.\output\` |
| 包含路径 | `mcu\common; source; driver\inc` | `source; ddl\mcu\common; ddl\driver\inc; host; host\port\hc32l19x` |
| 分组 | common / source / driver / README | startup / mcu_common / ddl_driver / host_esp / app / CMSIS / docs |
