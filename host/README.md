# host —— MCU 侧驱动（当前实现：HC32L196）

MCU 侧只做三件事：把 ESP8266 复位到已知状态、收发文本行协议、把结果翻译成事件回调。
它不关心网页、DNS、热点这些细节 —— 那些都在 ESP8266 里。

> 想直接跑起来：`platform/hc32l196/` 里有一个**打开就能编译下载**的 Keil MDK 工程
> （含官方 DDL 子集和示例 `main.c`），本文件下面是把它集成进你自己工程的说明。

## 分层

```
你的业务
   ↑ 事件回调（on_prov_start / on_got_ip / on_state_changed ...）
esp_client.c    复位握手、心跳看门狗、命令 API、状态机
   ↑
esp_link.c      行协议解析（+OK / -ERR / @EVT），带启动噪声过滤
   ↑
esp_port.c      函数指针绑定
   ↑
port/hc32l19x/esp_port_hc32l19x.c    UART1 + SysTick + RST 脚
```

移植到别的 MCU 只需重写 `port/*` 那一层（4 个函数）。

## 接线（可插拔模块）

| ESP8266 | 方向 | HC32L196 | 说明 |
| --- | --- | --- | --- |
| GPIO1 (TX) | ESP → MCU | PA03 (UART1_RX) | 协议下行 |
| GPIO3 (RX) | MCU → ESP | PA02 (UART1_TX) | 协议上行 |
| RST | MCU → ESP | PB00（可改） | 低电平复位，**建议 RST 上再并一个 10k 上拉** |
| GND | — | GND | 必须共地 |
| VCC | — | 3.3V | 模块电流峰值约 300mA，电源要够 |
| GPIO2 | ESP → 调试 | 可接 USB-TTL | 调试日志（115200，只发不收） |

> 需要注意：ESP8266 只有 UART0 能收发，UART1 只有 TX。所以"协议"和"下载口"是同一对脚，
> 可插拔时 USB-TTL 和 MCU 用同一对脚，不用改线；调试日志单独从 GPIO2 出。

## 集成步骤

1. 把 `host/*.c|h`、`host/port/hc32l19x/*` 加进你的 Keil/IAR 工程。
2. 包含路径加：`host`、`host/port/hc32l19x`（DDL 的 `driver/inc`、`mcu/common` 你工程里本来就有）。
3. 确认工程里编了 `mcu/common/interrupts_hc32l19x.c`（它把 `UART1_3_IRQHandler` 转发到 `Uart1_IRQHandler`）。
4. `main()` 里按这个顺序调用：`Esp_HwInit()` → 注册回调 → `Esp_Init(&cbs)` → 循环 `Esp_Poll()`。
   可直接参考 `platform/hc32l196/example/source/main.c`。
5. 主循环周期别超过 20ms，否则可能丢数据（接收是中断 + 256 字节环形缓冲，短暂延迟没问题）。
6. **GPIO 配置必须显式给 `enCtrlMode` 赋值**（`GpioAHB`）。
   `DDL_ZERO_STRUCT` 会把 `stc_gpio_cfg_t` 清成 `GpioFastIO(0)`，
   `Gpio_Init()` 会把它写进 `CTRL2.AHB_SEL`，端口就不走 AHB 寄存器了 —— 表现为
   "引脚配好了但读写没反应"。`esp_port_hc32l19x.c` 里已经处理，新加 GPIO 时别忘了。

## 编码提醒（Keil）

这些文件是 **UTF-8** 编码。如果你的 Keil 显示中文注释乱码，
把 `Edit → Configuration → Encoding` 改成 `UTF-8`，或执行下面这行把它转成 GBK：

```powershell
Get-ChildItem .\host -Recurse -Include *.c,*.h | ForEach-Object {
  $t = [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8)
  [System.IO.File]::WriteAllText($_.FullName, $t, [System.Text.Encoding]::GetEncoding('GB18030'))
}
```

## 能拿到的信息

```c
const esp_status_t *st = Esp_Status();
st->state        /* ESP_ST_OFFLINE / ESP_ST_PROV / ESP_ST_ONLINE / ESP_ST_ERROR */
st->ip           /* "192.168.1.23" */
st->ssid         /* 已连上的路由器 */
st->rssi         /* 信号强度 dBm */
st->prov_active  /* 1 = 正在配网，可以考虑显示提示 */
st->ap_ssid / st->ap_pwd   /* 让用户去连的热点名和密码 */
st->degraded     /* 1 = 连上路由器但外网不通 */
st->fail_msg     /* 最近一次配网失败原因（中文） */
```

## 主动操作

```c
Esp_Reprov();                              /* 重新配网（模块自己重启） */
Esp_Connect("MyWifi", "12345678");         /* 直接指定 SSID/密码 */
Esp_QueryStatus();                         /* 主动查一次（结果也会更新到 Esp_Status） */
Esp_HardReset();                           /* 拉低 RST 重新握手 */
Esp_SendLine("#SYS.INFO?");                /* 裸命令，用来抓诊断信息 */
```
