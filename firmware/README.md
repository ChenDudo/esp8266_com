# firmware —— ESP8266 联网 / 配网模块

## 它做什么

上电先按 Flash 里存的凭据联网；连不上就自己开热点 + 配网页；
手机提交 WiFi 密码后立刻联网，成功才写 Flash，之后掉电重启自动联网。
全程通过 UART0 与 MCU 用文本行协议通信（见 `../docs/uart_protocol.md`）。

## 编译烧录

```bash
pip install platformio
pio run -d firmware                 # 编译
pio run -d firmware -t upload       # 烧录（USB-TTL 接 GPIO1/GPIO3 与 RST/GND）
pio device monitor -b 115200        # 看调试日志（UART1 TX = GPIO2）
```

换模组只改 `platformio.ini` 里的 `board`：`esp12e` / `nodemcuv2` / `d1_mini` / `esp01_1m`。

## 接线

见 `src/board_pins.h` 的表。两个要点：

1. 与 MCU 的协议走 **UART0（GPIO1 TX / GPIO3 RX）** —— 这也正是模块的下载口，
   所以做成可插拔后，USB-TTL 和 MCU 用的是同一对脚，不用改线。
2. 调试日志从 **GPIO2（UART1，只有 TX）** 出来，115200。所以调试和通信互不干扰。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `src/app_cfg.h` | 所有可调参数（超时、热点名、心跳周期……），可用 `-D` 覆盖 |
| `src/board_pins.h` | 接线约定、状态灯/按键引脚 |
| `src/config_store.*` | 凭据存 Flash（双槽 A/B + CRC16，掉电不丢） |
| `src/wifi_conn.*` | 上电联网状态机：连接、重试、密码错快速失败、保活、外网探测 |
| `src/net_check.*` | 外网可达性探测（TCP 连已知 DNS，不走 DNS 解析） |
| `src/provisioning.*` | 配网门户：SoftAP + DNS 劫持 + 网页 + 强制门户 |
| `src/portal_html.h` | 配网页模板（纯 HTML，不依赖 JS，无外链） |
| `src/host_link.*` | 与 MCU 的行协议收发与解析 |
| `src/main.cpp` | 把上面几块串起来 + 命令表实现 |

## 上电流程（对照 `docs/provisioning_design.md`）

```
setup:  调试口 → 读配置 → 长按按键检查 → 串口就绪 → 发 @EVT READY → 联网判定
loop:   HostLink 收命令 → WifiConn 状态机 → Provisioning 门户 → LED → 心跳
```

## 已知限制

- 配网时用 `WIFI_AP_STA`，STA 一旦连上路由器，SoftAP 会跟着切到路由器信道，
  手机可能瞬断一下（页面会自动重连），属正常现象。
- `dhcp_set_captiveportal_url()`（DHCP option 114）依赖 ESP8266 内核里 NONOS SDK 的
  接口；若某版本编译报"未声明"，把 `platformio.ini` 里的 `-DCFG_USE_DHCP_OPT114=1`
  改成 `0`，其余功能不受影响（只是少了自动弹窗，用户手动访问 192.168.4.1 仍然可用）。
- 扫描 WiFi 时是阻塞的（约 1.5~2.5 秒），门户页有 30 秒缓存，避免每次刷新都卡。
- **外网探测会阻塞最多 3 秒**（`CFG_PROBE_TIMEOUT_MS`），每 60 秒才做一次。
  这 3 秒里主循环不转，MCU 的命令要等一等才回（UART 有 256 字节缓冲，不会丢数据）。
  如果 MCU 对响应时延敏感，把 `CFG_PROBE_TIMEOUT_MS` 调小或把 `CFG_PROBE_INTERVAL_MS` 调大。

## 当前验证状态

- ✅ 代码完整、`host/` 侧已用 arm-none-eabi-gcc + 真实 DDL 头文件做过语义检查。
- ⚠️ **ESP8266 固件尚未编译过**：本机没有 ESP8266 工具链（PlatformIO / Arduino core）。
  第一次 `pio run` 时如果某个 API 名字对不上（例如 `dhcp_set_captiveportal_url`），
  按上面"已知限制"里的办法降级即可。建议第一次编译时先不加任何 `-D`，用默认配置。
