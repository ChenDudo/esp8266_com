# ESP8266 通用 WiFi 配网模块 —— 设计方案

版本 v0.2 · 2026-10-11

> 说明：本文是设计阶段文档，用于对齐需求与架构。文中库/固件版本号基于既有经验，
> 落地前建议联网核对一次最新版本（当前开发环境无外网）。

## 已确认决策（2026-10-11）

| 编号 | 决策 | 影响 |
| --- | --- | --- |
| D1 | ESP8266 **刷自定义固件**（Arduino core + PlatformIO），不用原厂 AT 固件 | 门户/存储/协议全部自研，见 §3.3 |
| D2 | 主控 **HC32L196**（HC32L19x DDL Rev1.3.0） | MCU 侧驱动放在 `host/`，UART1 PA02/PA03 |
| D3 | ESP8266 是**可插拔模块**，MCU **可以控制 RST** | 上电时序与握手见 §2.4，模块死机可硬复位 |

> 本章对应的代码已经落地：`firmware/`（ESP 侧）+ `host/`（HC32L196 侧），
> 协议规范见 [uart_protocol.md](uart_protocol.md)。

---

## 0. 结论速览（TL;DR）

- 你要的功能在业界叫 **无头设备 WiFi 凭据分发（WiFi provisioning / onboarding）**，
  标准做法是 **AP 配网 + Captive Portal（强制门户）**：
  连不上网 → 自己开热点 → 手机连热点 → 手机上自动/手动弹出配网网页 → 提交 SSID/密码 → 设备联网。
- **最主流的实现**：ESP8266 跑自定义固件（Arduino core / PlatformIO），
  用 `SoftAP + DNSServer(DNS 劫持) + HTTP Server` 组成门户；最省事的现成方案是
  **WiFiManager（tzapu/WiFiManager）**，它本身就是"零配置自动配网 + 强制门户"。
- 你 readme 里写的 **"MCU 通过串口用 AT 命令控制 ESP8266"** 这条路**也能做通**，
  但原厂 AT 固件**没有 DNS 服务 / 门户能力**：手机不会自动弹页面，只能手动访问
  `http://192.168.4.1`，而且网页 HTML 要由 MCU 自己拼 HTTP 报文返回（`+IPD` / `AT+CIPSEND`）。
  体验和工程量都明显更差。
- **建议**：**ESP8266 刷自定义固件当"联网模块"**，对 MCU 仍然暴露一套"类 AT"的行协议，
  保留你原设计里"MCU 只发命令、不碰网络细节"的分工。两边都干净。

---

## 1. 需求拆解

### 1.1 用户故事

> 设备上电后自己尝试联网。连不上，它就开一个热点，我用手机连上这个热点，
> 网页里选一下我要它连的 WiFi、填密码、点提交，设备马上用这组凭据连上去。
> 之后断电重启，它自己就连上了，我不用再管。

### 1.2 功能清单

| 编号 | 功能 | 说明 |
| --- | --- | --- |
| F1 | 上电联网判定 | 读取 Flash 里的凭据 → STA 连接 → 判定"是否在线" |
| F2 | 失败自动进配网模式 | 无凭据 / 连接失败 / 密码错误 → 开 SoftAP + 网页门户 |
| F3 | 网页配网 | 扫描附近 AP 列表、选 SSID、填密码、提交、显示连接进度与结果 |
| F4 | 凭据持久化 | 写入 Flash，带校验；密码错误/连续失败要能清除、能重配 |
| F5 | 运行时保活 | 掉线自动重连、状态变化上报 MCU、可选外网可达性探测 |
| F6 | 重新配网入口 | 按键长按 / 网页"重新配网"按钮 / 串口命令，三种都要有 |
| F7 | 主控接口（可选层） | MCU ↔ ESP8266 串口行协议：命令、应答、事件 |

### 1.3 非目标（先不做，避免一次摊太大）

- 不涉及云端/App、不做 BLE 配网（**ESP8266 硬件上没有 BLE**，这条路直接排除）。
- 不做 OTA 升级（后续可加，架构上留口子）。
- 不做多协议业务（MQTT / HTTP 上报等属于业务层，本模块只负责"把网连上"）。

---

## 2. 上电流程（核心状态机）

```
        ┌──────────────┐
 上电 → │  BOOT        │ 读 Flash 凭据(带 CRC)
        └──────┬───────┘
               │
      ┌────────┴─────────┐
   有凭据              无凭据/CRC 错
      │                   │
      ▼                   │
 ┌─────────┐              │
 │ STA_CONN│  超时 15s ×2 │
 └────┬────┘              │
  成功 │   失败/密码错      │
      │      │             │
      ▼      └──────┬──────┘
 ┌─────────┐        ▼
 │ ONLINE  │  ┌───────────────┐
 │ 业务运行 │  │ PROV (配网)   │
 └────┬────┘  │ SoftAP+Web+DNS│
  掉线 │      └──────┬────────┘
      │             │ 手机提交凭据
      └────→ STA_CONN ◄──┘  (配网中提交即试连)
                 │
              成功 → 保存 + 返回成功页 + 切 STA
              失败 → 返回错误页，继续留在配网模式
                 超时 5min 无人提交 → 重启重试
```

### 2.1 状态定义

| 状态 | 进入条件 | 主要动作 | 退出条件 |
| --- | --- | --- | --- |
| `BOOT` | 上电 / 复位 | 读 Flash 凭据、判断重启原因 | 有凭据 → `STA_CONN`；否则 → `PROV` |
| `STA_CONN` | 有凭据 | `WiFi.begin()` + 超时等待，最多 2~3 次 | 成功 → `ONLINE`；失败 → `PROV` |
| `ONLINE` | 拿到 IP | 业务运行 + 保活心跳 | 掉线超阈值 → `STA_CONN` |
| `PROV` | 无凭据 / 连不上 / 用户主动触发 | 开 SoftAP、启动 HTTP + DNS、等待提交 | 收到提交 → 试连；超时 → 重启 |
| `REPROV` | 用户长按按键 / 串口命令 / 网页按钮 | 清凭据 → `PROV` | 同 `PROV` |

### 2.2 "是否能联网"分三层判定（重要）

只说"连上了"容易误判，建议分层：

1. **L1 关联成功**：`WiFi.status() == WL_CONNECTED`（拿到 IP）。
2. **L2 局域网可达**：能 ping/访问默认网关（可选，ESP8266 无原生 ping，可用 UDP 或 `WiFi.gatewayIP()` 比对）。
3. **L3 外网/服务器可达**：向已知服务器发一次 HTTP HEAD 或 TCP 连接（如 `http://connectivitycheck.gstatic.com/generate_204`，
   或你自己的服务器）。**只有 L3 失败才算"不能联网"才进配网**，否则家里路由器没外网时会一直弹配网。

建议默认策略：**L1 + L3（带 3 秒超时）**；L3 连续失败 N 次（例如 3 次，跨若干分钟）才降级进 `PROV`。

### 2.3 失败原因要区分对待

`WiFi.waitForConnectResult()` 的返回码：

| 返回码 | 含义 | 处理策略 |
| --- | --- | --- |
| `WL_CONNECTED` | 成功 | 保存/进入业务 |
| `WL_WRONG_PASSWORD` | 密码错 | **快速失败**，直接进 `PROV` 并在网页提示"密码错误" |
| `WL_NO_SSID_AVAIL` | 找不到 AP | 重试 1 次后进 `PROV`（可能路由器没开或 5G-only） |
| `WL_CONNECT_FAILED` | 其他失败 | 重试后进 `PROV` |
| 超时（`WL_IDLE_STATUS`） | 信号弱/慢 | 重试 2 次后进 `PROV` |

> 密码错误**不要**无脑重试 3 次再进配网——用户等得久，且体验上"错就是错"。

### 2.4 硬件接口与上电时序（已确认：可插拔模块 + MCU 控制 RST）

**接线（MCU = HC32L196）**

| ESP8266 | 方向 | MCU / 其它 | 说明 |
| --- | --- | --- | --- |
| GPIO1 (UART0 TX) | ESP → MCU | PA03（UART1_RX） | 协议下行 |
| GPIO3 (UART0 RX) | MCU → ESP | PA02（UART1_TX） | 协议上行 |
| RST | MCU → ESP | PB00（可改） | 低电平复位；**建议该脚再并 10k 上拉到 3.3V** |
| GPIO2 (UART1 TX) | ESP → 调试口 | USB-TTL RX（可选） | 调试日志 115200，只发不收 |
| EN / CH_PD | — | 10k 上拉 3.3V | 建议常开 |
| GPIO0 | — | 10k 上拉 3.3V | 运行模式；下拉才进烧录模式 |
| GPIO15 | — | 10k 下拉 GND | **必须** |
| GND / VCC | — | 共地 / 3.3V | 峰值电流约 300mA，电源要有余量 |

**为什么协议口就是下载口**：ESP8266 只有 UART0 能同时收发（UART1 只有 TX）。
做成可插拔后，USB-TTL 与 MCU 用的是同一对脚，不用改线；调试日志单独从 GPIO2 出。

**上电时序（两边都已知情，不会互相等死）**

```
① MCU 上电 → Esp_HwInit()：RST 脚配置为输出且默认高（释放）
② Esp_Init() → RST 拉低 200ms → 释放 → 等 @EVT READY（超时 6s）
③ ESP 侧：读 Flash → 有凭据连路由器 / 无凭据开热点，就绪后发 @EVT READY
④ MCU 收到 READY → 回 #HELLO → 之后靠 @EVT HB（5s 一次）判断模块是否活着
⑤ MCU 连续 20s 收不到任何消息 → 拉低 RST 重新握手（最多 3 次，之后报错）
```

关键点：

1. **RST 上必须有 10k 上拉**。否则 MCU 复位期间（GPIO 处于输入态）ESP 的 RST 状态不定，
   可能出现"MCU 重启 → ESP 也被拖住"的互相等待。
2. **不要用 GPIO0 做"长按配网"按键**，它是启动模式脚，上电被拉低会进烧录模式。
   固件默认用 GPIO14（D5），见 `firmware/src/board_pins.h`。
3. 可插拔模块拔下来单独用 USB-TTL 刷固件时，MCU 侧 RX 悬空即可，
   USB-TTL **必须是 3.3V 电平**，不要用 5V 的。

---

## 3. 主流做法调研与对比

### 3.1 六种可选项

| 方案 | 载体 | 手机端操作 | 体验 | 工作量 | 关键点 |
| --- | --- | --- | --- | --- | --- |
| **WiFiManager**（tzapu） | ESP8266 自定义固件 | 连 AP → 自动弹网页 → 选 SSID 填密码 | 好 | **低** | 事实标准，自带门户/超时/自定义参数 |
| **自研 AP+Web 门户** | ESP8266 自定义固件 | 同上 | 好，可完全定制 | 中 | `DNSServer` + `ESP8266WebServer`，可控性最高 |
| **ESP-AT + MCU 驱动** | ESP8266 原厂 AT 固件 + MCU | 连 AP → **手动**访问 `192.168.4.1` | 一般 | **中高** | 无 DNS 劫持、无自动弹窗、HTTP 报文手拼 |
| **SmartConfig (ESP-Touch)** | 固件 + 专用 App | 打开 App 一键配网 | 好 | 中（要 App/要维护） | 需厂商 App，ESP8266 AT 部分版本支持 `AT+CWSTARTSMART` |
| **Improv WiFi** | 固件 + 浏览器 Web Serial | USB 插入 + Chrome 网页配网 | 好（但必须有 USB） | 中 | 开放标准，ESP8266 只支持串口传输，不支持 BLE |
| **ESP-IDF wifi_provisioning** | ESP32 为主 | 官方 App | 好 | - | **ESP8266 用不了新版 IDF**，直接排除 |

### 3.2 为什么推荐"自定义固件"而不是"原厂 AT 固件"

原厂 AT 固件（ESP-AT for ESP8266，v2.x）能做的事其实不少：

```
AT+CWMODE=2                      # 设为 SoftAP
AT+CWSAP="MyDev-1A2B","12345678",5,3   # 配 AP 名称/密码/信道/加密
AT+CIPMUX=1 ; AT+CIPSERVER=1,80        # 开 TCP 服务器 80 端口
+IPD,0,180:GET / HTTP/1.1 ...          # 收到手机 HTTP 请求（透传上行）
AT+CIPSEND=0,320 → ">" → 发送 HTTP 响应 # 手工回网页
AT+CWMODE=3 ; AT+CWJAP="ssid","pass"   # 切 STA 去连目标路由器
AT+CWSTATE?；AT+CIPSTA?                 # 查连接状态 / IP
```

**但缺口很致命**：

1. **没有 DNS 服务器**：手机连上 AP 后，浏览器访问任何域名都失败；不劫持 DNS 就没有
   "连上就自动弹配网页"的体验（Android/iOS 的强制门户探测依赖 DNS + HTTP 探测响应）。
2. **网页 HTML 必须由 MCU 拼 HTTP 报文返回**：MCU 要缓存 2~4 KB 的 HTML，
   还要按 `+IPD` 长度解析、分片 `AT+CIPSEND`，任何一处错位就白屏。
3. **AT 命令时序脆弱**：`AT+CWJAP` 阻塞可能 15 s，期间 MCU 侧业务要卡住，
   还得处理 `busy p...`、`ERROR`、超时重发。
4. ESP8266 的 ESP-AT 已基本停止新功能维护（官方主推 ESP32 系列）。

自定义固件里这些都是一行调用：`DNSServer.start(53, "*", apIP)`。**结论：AT 固件方案可行但性价比低。**

### 3.3 推荐的库与栈

| 组件 | 选择 | 备注 |
| --- | --- | --- |
| 框架 | Arduino ESP8266 core 3.1.x + PlatformIO | 比 RTOS/NONOS SDK 上手快得多 |
| 配网（快） | `tzapu/WiFiManager` v2.0.x | 一天就能出原型，先验证流程 |
| 配网（可控） | 自研：`DNSServer` + `ESP8266WebServer` | 产品化推荐，本节 4 章展开 |
| 异步替代 | `ESPAsyncWebServer` + `ESPAsyncTCP` | 并发好，但 ESP8266 上游基本停更，小众分支需自评风险 |
| 存储 | `LittleFS` 或 `EEPROM`(Flash 扇区模拟) | 只在内容变化时写，注意 Flash 寿命 |
| 设备发现 | `ESP8266mDNS` | 配网成功后可用 `http://mydev.local/` 访问 |

> 建议路径：**M1 先用 WiFiManager 跑通全流程建立信心，M2 再换成自研门户模块**
> （网页、字段、超时、UI 都要按自己产品定制的，WiFiManager 的默认页面不够用）。
>
> **已确认（D1）**：本项目直接走"自研门户模块"路线，不引入 WiFiManager 依赖。
> 下面 §4 的每个细节都已经在 `firmware/src/provisioning.cpp` 里实现。

---

## 4. 配网门户的关键实现细节

### 4.1 SoftAP 参数

```cpp
const char* AP_PWD = "12345678";           // 建议固定密码，印在设备标签上
String apSsid = "MyDev-" + String(ESP.getChipId(), HEX);  // 唯一名，便于多台设备区分
WiFi.mode(WIFI_AP);
WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
WiFi.softAP(apSsid.c_str(), AP_PWD, /*channel*/1, /*hidden*/false, /*maxConn*/4);
```

- 默认 AP IP 就是 `192.168.4.1`。**保持这个地址**，它是所有教程/贴纸的通用约定。
- 不要设 AP 隐藏，否则用户找不到热点。
- `maxConn` 给 2~4 即可，门户场景不会很多台。

### 4.2 强制门户（Captive Portal）三件套

1. **DNS 劫持**：把所有域名解析到 `192.168.4.1`
   ```cpp
   DNSServer dnsServer;
   dnsServer.start(53, "*", WiFi.softAPIP());   // loop() 里 dnsServer.processNextRequest()
   ```
2. **HTTP 302 重定向**：把系统探测 URL 全部 302 到门户首页，并在门户首页对所有未知路径也返回 302/门户页
   ```cpp
   server.onNotFound([](){ server.sendHeader("Location","http://192.168.4.1/",true);
                            server.send(302,"text/plain",""); });
   ```
   已知探测 URL（覆盖 Android/iOS/Windows）：
   `/generate_204`、`/gen_204`、`/hotspot-detect.html`、`/library/test/success.html`、
   `/connecttest.txt`、`/ncsi.txt`、`/redirect`、`/canonical.html`、`/success.txt`。
3. **DHCP option 114（RFC 7710）**：直接告诉手机"门户在哪"，部分 Android/iOS 会**自动弹出**配网页
   ```cpp
   #include <user_interface.h>
   dhcp_set_captiveportal_url("http://192.168.4.1/");   // NONOS SDK 提供，需验证当前内核是否暴露
   ```

> 兜底：即便自动弹窗不生效，用户手动访问 `192.168.4.1` 或点通知栏"登录网络"也能进页面。

### 4.3 配网网页（HTML 由固件内嵌，放 PROGMEM）

页面元素：

- 设备名 + MAC + 当前固件版本（方便售后定位）。
- **WiFi 扫描下拉列表**：`WiFi.scanNetworks()` → SSID + RSSI（信号格）+ 加密类型，
  加密为 `AUTH_OPEN` 时不显示密码框；列表为空时允许手输 SSID。
- 密码输入框 + "显示密码"勾选 + 中文字符提示（ESP8266 对非 ASCII SSID 支持有限，须在页面提示）。
- 提交按钮 → `POST /connect`（表单 `application/x-www-form-urlencoded`，
  密码里的 `+ / % & =` 必须正确 URL 编码/解码）。
- **进度反馈**：提交后 JS 每 1 s 轮询 `GET /status`，返回 JSON
  `{"state":"connecting|ok|fail","msg":"密码错误"}`；成功页显示
  "配网成功，设备 IP：192.168.1.23，正在重启…"。
- **可选高级字段**（对应 WiFiManager 的 `WiFiManagerParameter`）：
  设备名、静态 IP、业务参数（服务器地址/端口/MQTT 等）——通用项目建议预留 2~3 个自定义字段。
- 页面**内嵌 CSS/JS，不引外部 CDN**（配网时没有外网，引 CDN 必白屏）。

提交后两种实现风格：

- **同步阻塞**：`POST /connect` 里直接 `WiFi.begin()` 等 10 s，再返回结果页。
  简单，但页面卡 10 s；期间 HTTP 无响应，部分浏览器会重发请求。
- **异步（推荐）**：`POST /connect` 只存参数并置 `connecting` 状态立即返回，
  主循环里做连接，页面轮询 `/status`。UI 流畅，超时/重试好控制。

### 4.4 凭据存储

| 项 | 建议 |
| --- | --- |
| 介质 | `LittleFS`（放 `/wifi.json`）或 `EEPROM`（固定结构体） |
| 校验 | 结构体带 `magic + version + crc16/32`，校验失败视为"无凭据" |
| 写入时机 | **连接验证成功后再提交**（避免存了错密码，导致以后每次上电都白等 15 s） |
| 磨损 | 只在凭据变化时 `commit()`；不要每次上电都写 |
| 与 SDK 冲突 | 调 `WiFi.persistent(false)`，避免 SDK 自己也往 Flash 写一套，闪存写冲突 |
| 多 SSID | 可选，存 2~3 组，按顺序尝试（WiFiMulti 风格），适合"家里/公司都带" |

配套失败保护：**连续 N 次（如 3 次）上电连接失败 → 自动清除凭据进配网**，
防止设备永久卡在尝试一个已失效的 AP。

### 4.5 超时、重试与看门狗

| 参数 | 建议默认值 | 说明 |
| --- | --- | --- |
| STA 单次连接超时 | 10~15 s | `WiFi.waitForConnectResult(timeout)` |
| STA 重试次数 | 2 | 密码错不重试 |
| 配网门户超时 | 5 min | 无人提交则 `ESP.restart()`，形成"再试一次联网"的循环 |
| 掉线判定 | 30 s 内未恢复即重连 | `WiFi.setAutoReconnect(true)` + 定期自检 |
| 连接期喂狗 | 每次 `delay/yield` | 长循环里必须 `ESP.wdtFeed()`/`yield()`，否则复位 |
| 冷启动延时 | 可选 500 ms | 等电源稳定，尤其和 MCU 共板时 |

### 4.6 重新配网入口（F6）

| 入口 | 实现 |
| --- | --- |
| 按键长按 | 上电时检测 GPIO（如 GPIO0 拉低 3 s）或运行时长按 → 清凭据 → 进 `PROV` |
| 网页按钮 | 门户页放"重新配网/清除设置"，配网成功后也可常驻一个 `/config` 页 |
| 串口命令 | 来自 MCU 的 `WIFI.REPROV` 命令（见第 5 章） |
| 固件后门 | 连续 5 次快速断电重启 → 进配网（可选，救砖用） |

### 4.7 安全注意

- 配网期间 AP 是开放的入口，**必须有密码**（默认 `12345678` 或设备型号相关），并印在标签上。
- **不要把 WiFi 密码打到串口日志**，也不要明文回显到网页（密码框用 `type=password`，不回填真值）。
- 配网成功后建议**限制 `/config` 访问**，或 30 min 后自动关闭门户。
- AP 名不要包含个人信息（别用用户手机热点名派生）。

---

## 5. MCU ↔ ESP8266 串口协议（已实现；完整规范见 [uart_protocol.md](uart_protocol.md)）

即使换成自定义固件，你和 MCU 的对接方式可以**继续保持"AT 风格"**，
这样 MCU 侧代码几乎不用重写，只是 ESP 侧从"原厂 AT"换成"你的协议"。

### 5.1 帧格式（文本行协议，最易调试）

```
↓ MCU → ESP   # <VERB> [key=value ...]\r\n
↑ ESP → MCU   +OK <VERB> [key=value ...]\r\n
              -ERR <code> <text>\r\n
              @EVT <NAME> [key=value ...]\r\n     ← 异步事件
```

- 全部 ASCII + `\r\n` 结束，人肉用串口助手就能调。
- 需要透传大块数据（HTTP 上报/MQTT）时再上二进制帧：`AA 55 | len | payload | CRC16`。
- 波特率建议 115200，8N1，无流控。

### 5.2 命令表（初稿）

| 命令 | 方向 | 说明 |
| --- | --- | --- |
| `#WIFI.STATUS?` | MCU→ESP | 查询状态：`state=online,ip=192.168.1.23,rssi=-62` |
| `#WIFI.CONN ssid=xxx pass=yyy` | MCU→ESP | 主动指定凭据联网（可跳过网页） |
| `#WIFI.REPROV` | MCU→ESP | 清除凭据并进入配网模式 |
| `#WIFI.RESET` | MCU→ESP | 重启模块 |
| `#PROV.STATUS?` | MCU→ESP | 配网模式是否激活、已连入几台手机 |
| `#NET.PING host=1.1.1.1` | MCU→ESP | 可选连通性检查 |
| `#NET.TCP.SEND id=0,data=...` | MCU→ESP | 业务数据发送 |
| `@EVT PROV_START ap=MyDev-1A2B` | ESP→MCU | 进入配网模式 |
| `@EVT PROV_DONE ssid=Home-2G` | ESP→MCU | 配网成功 |
| `@EVT GOT_IP ip=192.168.1.23` | ESP→MCU | 拿到 IP |
| `@EVT WIFI_LOST` | ESP→MCU | 掉线 |
| `@EVT READY ver=0.1` | ESP→MCU 上电首发 | 模块就绪，MCU 据此开始握手 |

### 5.3 建议的 MCU 侧状态映射

MCU 不关心"热点/网页"这些细节，只关心：
`OFFLINE → PROVISIONING(可显示"配网中,请连热点") → ONLINE(ip, rssi)`。
所以 MCU 侧只需实现上面 3 个事件 + 2 个查询命令即可跑通，
**建议 M3 阶段只实现 `READY / PROV_START / PROV_DONE / GOT_IP / WIFI_LOST` + `WIFI.STATUS?`**。

### 5.4 如果坚持用原厂 AT 固件（方案 B）的 MCU 侧要点

- 上电握手：`AT` → `AT+RST` → `AT+GMR`（记录版本）→ `AT+CWMODE=3`。
- 进入配网：`AT+CWSAP="MyDev","12345678",5,3` + `AT+CIPMUX=1` + `AT+CIPSERVER=1,80`。
- 收包：解析 `+IPD,<link>,<len>:` 之后的 `<len>` 字节，剥出 HTTP 请求行与 body。
- 回包：预生成完整 HTTP 响应（含 `Content-Length`）→ `AT+CIPSEND=<link>,<len>` → 等 `>` → 发送。
- 转 STA：`AT+CIPSERVER=0` → `AT+CWMODE=1` → `AT+CWJAP="ssid","pass"`（阻塞，超时可达 15 s）
  → `AT+CWSTATE?` / `AT+CIPSTA?` 确认，失败时回退到 `AT+CWMODE=2` 重开门户。
- **务必**：所有 AT 命令带重试与超时；`busy p...` 视为"忙"要等；把网页 HTML 放 MCU 的 Flash 常量区。

---

## 6. 建议的目录结构

```
esp8266_com/
├─ docs/
│  ├─ provisioning_design.md      # 本文：方案设计 + 决策记录
│  ├─ uart_protocol.md            # 串口协议规范（唯一权威）
│  └─ test_plan.md                # 测试用例：弱信号/错密码/无外网（待写）
├─ firmware/                      # ESP8266 侧（PlatformIO + Arduino core）
│  ├─ platformio.ini              # board=esp12e（换模组改这一行）
│  ├─ README.md                   # 编译 / 接线 / 文件说明
│  └─ src/
│     ├─ main.cpp                 # setup/loop：串起来 + MCU 命令表
│     ├─ app_cfg.h                # 全部可调参数（可用 -D 覆盖）
│     ├─ board_pins.h             # 接线约定、状态灯 / 按键
│     ├─ config_store.*           # Flash 凭据读写（双槽 A/B + CRC16）
│     ├─ net_check.*              # 外网可达性探测
│     ├─ wifi_conn.*              # 上电联网状态机（非阻塞、重试、保活）
│     ├─ provisioning.*           # SoftAP + DNSServer + 网页 + 强制门户
│     ├─ portal_html.h            # 配网页（纯 HTML，不依赖 JS）
│     └─ host_link.*              # 与 MCU 的行协议
└─ host/                          # MCU 侧（HC32L196，纯 C）
   ├─ README.md                   # 接线 / 集成步骤 / 编码提醒
   ├─ esp_port.h / esp_port.c     # 移植层接口 + 绑定
   ├─ esp_link.h / esp_link.c     # 行协议解析（含启动噪声过滤）
   ├─ esp_client.h / esp_client.c # 复位握手、心跳看门狗、状态机、命令 API
   └─ port/hc32l19x/
      ├─ esp_hw.h
      └─ esp_port_hc32l19x.c      # UART1(PA02/PA03) + SysTick + RST
```

**可运行的 MCU 工程**（含官方 DDL 子集与示例 `main.c`）单独放一层：

```
platform/hc32l196/
├─ ddl/                           # 官方 HC32L19x DDL 子集（BSD-3，自包含）
│  ├─ driver/inc, driver/src
│  └─ mcu/common
├─ example/
│  ├─ MDK/esp_hc32l196.uvprojx    # ← 双击打开，F7 编译 F8 下载
│  ├─ MDK/startup_hc32l19x.s
│  └─ source/{main.c, ddl_device.h}
└─ README.md                      # 接线 / 灯语 / 快速验证 / 常见问题
```

## 7. 里程碑

| 阶段 | 内容 | 产出 | 估时 |
| --- | --- | --- | --- |
| M1 | ~~用 WiFiManager 跑通全流程~~ | 已跳过（直接自研，见 D1） | — |
| M2 | 自研门户模块（AP+DNS+网页+存储） | `firmware/` 代码 | ✅ 代码就绪 |
| M3 | 串口协议 + MCU 侧驱动 | `host/` + `docs/uart_protocol.md` | ✅ 代码就绪 |
| M4 | **上板联调**：编译、烧录、真机配网走通 | 能配网、能联网、重启保持 | ⬅ 下一步 |
| M5 | 健壮性：弱信号 / 错密码 / 无外网 / 断电 / 反复插拔 | 测试报告 + `test_plan.md` | 2 天 |
| M6 | 通用化：参数宏化、抽成可复用组件、使用说明 | 可复制到新项目 | 1~2 天 |

## 8. 决策记录与遗留问题

### 8.1 已拍板

| 编号 | 决策 | 落地情况 |
| --- | --- | --- |
| D1 | ESP8266 刷自定义固件（Arduino core + PlatformIO） | `firmware/` 已实现 |
| D2 | 主控 HC32L196（HC32L19x DDL Rev1.3.0） | `host/port/hc32l19x/` 已实现并做过语法校验 |
| D3 | 可插拔模块 + MCU 控制 RST | 时序与看门狗见 §2.4、`host/esp_client.c` |
| D4 | 配网门户 5 分钟无提交自动重启 | `CFG_PROV_TIMEOUT_MS`，可改 |
| D5 | 默认参数：AP 名 `MyDev-xxxx`、密码 `12345678`、记住 2 组 SSID | `firmware/src/app_cfg.h`，全部可用 `-D` 覆盖 |

### 8.2 仍待你确认（不影响现在上板联调，但影响最终产品形态）

1. **目标模组型号**：现在按 4MB 的 ESP-12F 配（`board = esp12e`）。若实际用 ESP-01S（1MB），
   需要 `-DCFG_MINIMAL_UI=1`，且要放弃 OTA。
2. **AP 密码策略**：现在固定 `12345678`。是否改成每台设备独立密码（如芯片 ID 派生）印在标签上？
   更安全，但售后沟通成本更高。
3. **业务参数字段**：门户页现在只有 SSID/密码。是否要加"服务器地址/端口/MQTT 参数/静态 IP"？
   架构上留了口子：`AppCfg` 的 `reserved` 和 `flags` 都能扩。
4. **局域网发现**：现在用 mDNS（`espnode.local`）。是否需要 UDP 广播 / 固定端口发现？
5. **配网入口**：现有"上电长按 GPIO14 3 秒"和串口 `#WIFI.REPROV` 两条路。
   产品上还需要常驻的网页"重新配网"按钮吗？（配网成功后门户会关闭，需要的话要另开 `/config`。）
6. **提示规范**：现在只定了状态灯闪法（快闪=连接中、慢闪=配网、常亮=在线）。
   若有屏幕/蜂鸣器，希望怎么提示？

## 9. 风险清单

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 中文/特殊字符 SSID、密码含特殊符号 | 提交失败或连接失败 | 网页提示 + 正确 URL 编解码 + 失败时给出明确原因 |
| 路由器只开 5 GHz | 找不到 AP | 网页提示"请连接 2.4 GHz 频段" |
| 家里路由器正常但无外网 | 误判为"不能联网"反复进配网 | 采用 2.2 的 L1+L3 分层判定 + 失败计数 |
| 手机连上 AP 但不弹页面 | 用户以为坏了 | DNS 劫持 + 302 + DHCP option 114 三重兜底 + 贴纸说明 |
| 长时间阻塞导致看门狗复位 | 配网循环重启 | 非阻塞状态机 + 定期 `yield()`/喂狗 |
| Flash 频繁写入 | 闪存磨损 | 只在变化时 commit，`WiFi.persistent(false)` |

## 10. 参考

- tzapu/WiFiManager（ESP8266/ESP32 配网库，事实标准）
- ESP8266 Arduino core 文档：WiFi / DNSServer / ESP8266WebServer / LittleFS / ESP8266mDNS
- ESP8266 NONOS SDK API：`user_interface.h`（`dhcp_set_captiveportal_url`、`wifi_softap_get_station_num` 等）
- ESP-AT 用户指南（ESP8266 版）：`AT+CWSAP` / `AT+CIPSERVER` / `AT+CWJAP` / `AT+CWSTATE`
- RFC 7710：Captive-Portal Identification Using DHCP
- Improv WiFi 标准（improv-wifi.com）
