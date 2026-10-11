# MCU ↔ ESP8266 串口协议规范

版本 v1 · 2026-10-11 · 对应固件 `firmware/` v0.1.0 与 `host/` v0.1.0

本文是**唯一权威**：`firmware/src/host_link.cpp`（ESP 侧）和 `host/esp_link.c`（MCU 侧）
都必须与本文一致。改协议先改本文。

---

## 1. 物理层

| 项 | 值 |
| --- | --- |
| 接口 | ESP8266 **UART0**（GPIO1 = TX，GPIO3 = RX） |
| 电平 | 3.3V TTL |
| 参数 | 115200，8N1，无流控，无校验 |
| 复位 | MCU 用一根 GPIO 控制 ESP 的 **RST**（低电平复位），建议 RST 上并 10k 上拉到 3.3V |
| 调试口 | ESP 的 **GPIO2**（UART1，仅 TX，115200）单向输出日志，与协议互不干扰 |

### 1.1 上电噪声（必须容忍）

ESP8266 上电时会在 UART0 吐出一段 ROM 启动信息，波特率是 **74880**，
在 115200 下收到的是一串乱码字节；`Serial.begin()` 之后才恢复干净。

因此 MCU 侧解析规则是：**按 `\n` 切行 → 丢弃含不可打印字符的行 → 只认行首为
`+OK` / `-ERR` / `@EVT` 的行**。靠这个方法重新同步，不需要任何特殊的上电时序
（`host/esp_link.c` 已实现，见 `line_is_clean()` 与 `parse_line()`）。

---

## 2. 帧格式

全部 ASCII 文本，`\r\n` 结尾（MCU 只发 `\r\n`，解析时 `\r` 可忽略）。

```
MCU → ESP      # <VERB> [key=value ...]
ESP → MCU      +OK <VERB> [key=value ...]      命令成功
               -ERR <code> <text>              命令失败
               @EVT <NAME> [key=value ...]     异步事件
```

- `<VERB>` / `<NAME>` 区分大小写不敏感，但约定全部大写；含 `.` 和 `?` 的写法是习惯用法。
- 一行最长 160 字节（含 `\n`）。超长行接收方丢弃并重新同步。

### 2.1 值的转义

值里不允许出现空格（空格是分隔符），也不允许出现 `=`（会破坏 `k=v` 切分）。
双方统一按下面规则转义，**只在值里转义，key 不转义**：

| 原字符 | 转义为 |
| --- | --- |
| 空格 | `%20` |
| `=` | `%3D` |
| `%` | `%25` |

例：SSID 是 `My Home`，则下发 `#WIFI.CONN ssid=My%20Home pass=...`。
MCU 侧由 `esc_to()` 自动完成，ESP 侧由 `HostLink::val()` / `unval()` 完成。

> 注意：只支持 ASCII 与 UTF-8 原样透传，不做中文转码；SSID 里的中文在 802.11
> 层面能用，但 ESP8266 的 `WiFi.begin()` 对非 ASCII SSID 支持有限，页面上已提示用户。

---

## 3. 命令表（MCU → ESP）

| 命令 | 参数 | 应答 | 说明 |
| --- | --- | --- | --- |
| `#PING` | — | `+OK PING` | 存活探测，兼作链路延迟测量 |
| `#HELLO` | `proto=1` | `+OK HELLO` | 收到 `@EVT READY` 后的握手确认 |
| `#WIFI.STATUS?` | — | `+OK WIFI.STATUS ...` | 查询当前状态（见下） |
| `#WIFI.CONN` | `ssid=x` `pass=y` | `+OK WIFI.CONN` | 直接指定凭据联网；成功才写 Flash |
| `#WIFI.REPROV` | — | `+OK WIFI.REPROV` | 清凭据 → 重启 → 进配网 |
| `#PROV.STATUS?` | — | `+OK PROV.STATUS ...` | 配网模式是否激活、连了几台手机 |
| `#PROV.STOP` | — | `+OK PROV.STOP` | 立即关闭配网热点（应急用） |
| `#CFG.CLEAR` | — | `+OK CFG.CLEAR saved=1` | 只清 WiFi 记录，不重启 |
| `#SYS.SETNAME` | `name=x` | `+OK SYS.SETNAME name=x` | 改设备名（mDNS / 页面显示） |
| `#SYS.INFO?` | — | `+OK SYS.INFO ...` | 固件版本、Flash、内存、复位原因、统计 |
| `#SYS.REBOOT` | — | `+OK SYS.REBOOT` | 重启模块 |

未知命令回 `-ERR 1 unknown_cmd`。

### 3.1 `+OK WIFI.STATUS` 字段

| key | 取值 | 说明 |
| --- | --- | --- |
| `state` | `INIT` / `CONNECTING` / `ONLINE` / `PROV` | 当前状态 |
| `ip` | `192.168.1.23` 或 `-` | 未联网时是 `-` |
| `ssid` | 已转义的字符串或 `-` | 目标/已连上的路由器 |
| `rssi` | 例如 `-62` | dBm |
| `degraded` | `0` / `1` | 1 = 连上路由器但外网不通 |
| `prov` | `0` / `1` | 1 = 配网热点已开启 |
| `ap` | `MyDev-1A2B` 或 `-` | 配网热点名 |
| `up` | 秒 | 模块本次运行时长 |

### 3.2 `+OK PROV.STATUS` 字段

`active=1 sta=1 elapsed=42 ap=MyDev-1A2B pwd=12345678`

### 3.3 `+OK SYS.INFO` 字段

`fw=0.1.0 proto=1 chip=0x1A2B3C flash=4194304 heap=31280 rst=Power%20on creds=1 fails=0 rxlines=87 drop=0`

---

## 4. 事件表（ESP → MCU）

| 事件 | 参数 | 触发时机 | MCU 建议动作 |
| --- | --- | --- | --- |
| `@EVT READY` | `ver=0.1.0 proto=1` | 模块启动完成，**每个上电周期第一条** | 发 `#HELLO`，认为模块已就绪 |
| `@EVT HB` | `state= ip= rssi= up=` | 每 5 秒 | 刷新状态；**4 次没收到就复位模块** |
| `@EVT WIFI_CONNECTING` | `ssid=` | 开始尝试连接某个路由器 | 界面显示"连接中" |
| `@EVT GOT_IP` | `ip= rssi=` | 拿到 IP | 置 ONLINE，开始业务 |
| `@EVT PROV_START` | `ap=` `pwd=` | 配网热点已开启 | 提示用户连热点、开网页；可显示密码 |
| `@EVT PROV_DONE` | `ssid=` `ip=` | 用户在网页提交后连接成功 | 置 ONLINE；提示"配网成功" |
| `@EVT PROV_FAIL` | `msg=密码错误` | 网页提交的凭据连不上 | 显示 `msg`（中文，直接上屏） |
| `@EVT PROV_TIMEOUT` | — | 5 分钟没人配网，模块即将重启 | 提前告警，然后按模块重启处理 |

> `WIFI_LOST` 事件当前固件**不发**：链路掉线时模块会直接发 `@EVT WIFI_CONNECTING`
> （重连中）或 `@EVT PROV_START`（进配网）。MCU 侧保留了对 `WIFI_LOST` 的处理，
> 便于以后固件增加。

---

## 5. 时序

### 5.1 上电握手

```
MCU                                   ESP8266
 │  Esp_HwInit()  配好 UART/时钟/RST
 │  Esp_Init()  → RST 拉低 200ms
 │                              ── 启动(74880bps 噪声，被忽略) ──►
 │  RST 释放，等 READY                                              │
 │                                   setup(): 读配置 → 联网判定 ──► │
 │  ◄── @EVT READY ver=0.1.0 proto=1 ────────────────────────────── │
 │  ── #HELLO proto=1 ────────────────────────────────────────────► │
 │  ◄── +OK HELLO ────────────────────────────────────────────────  │
 │  ── #WIFI.STATUS? ─────────────────────────────────────────────► │
 │  ◄── +OK WIFI.STATUS state=CONNECTING ... ─────────────────────  │
 │  ◄── @EVT PROV_START ap=MyDev-1A2B pwd=12345678 ───────────────  │
```

握手超时：`.h` 里 `ESP_WAIT_READY_MS`（默认 6 秒）内没等到 `READY` 就重新拉 RST，
连续 `ESP_MAX_RESET`（默认 3）次失败 → 进入 `ESP_ST_ERROR`，MCU 可以点灯/告警。

### 5.2 网页配网

```
手机 ─连接 MyDev-1A2B─► ESP(SoftAP) ─打开 192.168.4.1─► 配网页
手机 提交 SSID/密码 ─► ESP: WIFI.CONN 内部执行
                        ├─ 成功 → 写 Flash → @EVT PROV_DONE ssid= ip=
                        │         8 秒后关掉热点（手机自动回到原 WiFi）
                        └─ 失败 → @EVT PROV_FAIL msg=密码错误
                                  网页显示原因，用户可以再试（热点不关）
```

MCU 侧对应回调：`on_prov_start` / `on_prov_done` / `on_fail`。

### 5.3 掉线重连（MCU 什么都不用做）

```
ESP: WiFi.status() != CONNECTED ─(超 30s)─► 重新 begin() ─► @EVT WIFI_CONNECTING
     └─ 再失败 ─► @EVT PROV_START（回到配网）
```

---

## 6. 错误码

| code | text | 含义 |
| --- | --- | --- |
| 1 | `unknown_cmd` | 命令不认识 |
| 2 | `missing_ssid` / `missing_name` | 参数缺失 |

> 目前错误码很少。以后新增时只在尾部追加，不要改已有编号。

---

## 7. 两侧实现要点（排查问题时看这里）

**MCU 侧**（`host/esp_link.c`）

- 接收在 UART 中断里只做一件事：`Esp_LinkFeedByte()` 塞进 256 字节环形缓冲；
  解析全在主循环的 `Esp_LinkPoll()` 里做，中断里绝不解析。
- 环形缓冲溢出只记 `overflow` 计数，不阻塞中断。
- 主循环调用 `Esp_Poll()` 的间隔建议 ≤ 20ms。

**ESP 侧**（`firmware/src/host_link.cpp`）

- `Serial.begin()` 后必须 `Serial.setDebugOutput(false)`，否则 SDK 日志会插进协议流。
- 调试日志一律走 `Serial1`（UART1 TX = GPIO2），不要走 `Serial`。
- 所有事件都是"状态变化时发一次"，不是轮询式重复发；MCU 不要依赖事件次数。

---

## 8. 版本与兼容

| 协议版本 | 变化 |
| --- | --- |
| 1 | 首版：READY / HB / WIFI_CONNECTING / GOT_IP / PROV_* + 上述命令表 |

`@EVT READY` 里带 `proto=1`，MCU 应据此判断兼容性；未知事件和未知 `key=value`
一律忽略（向前兼容），不要报错。
