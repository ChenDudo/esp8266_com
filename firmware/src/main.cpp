/* ============================================================================
 * ESP8266 通用联网 / 配网模块 —— 主程序
 *
 * 职责分工：
 *   WifiConn     上电联网判定 / 重试 / 保活（不含网页）
 *   Provisioning 配网门户（SoftAP + DNS 劫持 + 网页 + 强制门户）
 *   HostLink     与 MCU 的行协议（命令 / 应答 / 事件）
 *   main.cpp     只做"把这些串起来"，不放业务逻辑
 * ==========================================================================*/

#include <Arduino.h>
#include "app_cfg.h"
#include "board_pins.h"
#include "config_store.h"
#include "net_check.h"
#include "wifi_conn.h"
#include "provisioning.h"
#include "host_link.h"

#if CFG_ENABLE_MDNS
#include <ESP8266mDNS.h>
#endif

/* 调试日志口：UART1 只有 TX（GPIO2），所以只能单向输出。
 * 协议口是 UART0 —— 也就是 Arduino 里的 Serial。 */
#define DBG Serial1

static ConfigStore  g_store;
static NetCheck     g_net;
static WifiConn     g_wifi;
static Provisioning g_prov;
static HostLink     g_link;

/* --------------------------------------------------------------- LED ----- */
#if PIN_LED_STATUS >= 0
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif
static void ledWrite(bool on)
{
#if LED_ACTIVE_LOW
    digitalWrite(PIN_LED_STATUS, on ? LOW : HIGH);
#else
    digitalWrite(PIN_LED_STATUS, on ? HIGH : LOW);
#endif
}
static void ledSetup()
{
    pinMode(PIN_LED_STATUS, OUTPUT);
    ledWrite(false);
}
static void ledTask()
{
    static uint32_t next = 0;
    static bool     on   = false;
    uint32_t now = millis();
    uint32_t interval;

    switch (g_wifi.state()) {
    case WC_ONLINE:       interval = 0;    on = true; break;   /* 常亮 */
    case WC_CONNECTING:   interval = 200;  break;              /* 快闪 */
    case WC_PROVISIONING: interval = 800;  break;              /* 慢闪 */
    default:              interval = 1600; break;
    }
    if (interval == 0) {
        ledWrite(true);
        return;
    }
    if ((int32_t)(now - next) >= 0) {
        on   = !on;
        ledWrite(on);
        next = now + interval;
    }
}
#else
#define ledSetup() do { } while (0)
#define ledTask()  do { } while (0)
#endif

/* -------------------------------------------------------- 配网按键 ------- */
/* 上电时若按键一直被按住 ms 毫秒 → 清凭据进配网。
 * 只在 setup 里调用，返回前不阻塞（没按就立刻返回）。 */
static bool bootButtonHeld(uint32_t ms)
{
#if PIN_BTN_PROV >= 0
    pinMode(PIN_BTN_PROV, INPUT_PULLUP);
    if (digitalRead(PIN_BTN_PROV) != LOW) return false;
    uint32_t t0 = millis();
    while (digitalRead(PIN_BTN_PROV) == LOW) {
        if ((uint32_t)(millis() - t0) >= ms) return true;
        delay(10);
        yield();
    }
#else
    (void)ms;
#endif
    return false;
}

/* --------------------------------------------------------- 状态文本 ----- */
static const char* stateName(uint8_t st)
{
    switch (st) {
    case WC_ONLINE:       return "ONLINE";
    case WC_CONNECTING:   return "CONNECTING";
    case WC_PROVISIONING: return "PROV";
    default:              return "INIT";
    }
}

/* 不依赖 strcasecmp（新 libc 里它的可见性随宏变化），自己写一个大小写无关比较 */
static bool ciEq(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return (*a == '\0' && *b == '\0');
}

/* ------------------------------------------------- WifiConn 回调 -------- */
static void onWifiState(uint8_t st)
{
    switch (st) {
    case WC_CONNECTING:
        DBG.print(F("[wifi] connecting: "));
        DBG.println(g_wifi.ssidStr());
        g_link.event1("WIFI_CONNECTING", "ssid", String(g_wifi.ssidStr()));
        break;

    case WC_ONLINE:
        DBG.print(F("[wifi] ONLINE ip="));
        DBG.println(g_wifi.ipStr());
        g_link.event2("GOT_IP", "ip", String(g_wifi.ipStr()),
                               "rssi", String((int)g_wifi.rssi()));
        if (g_prov.active()) {
            g_prov.scheduleStop();      /* 手机看完结果页后再关热点 */
        }
#if CFG_ENABLE_MDNS
        MDNS.begin(g_store.devName());
#endif
        break;

    case WC_PROVISIONING:
        DBG.println(F("[wifi] enter provisioning"));
        g_prov.begin(g_store.devName());
        g_link.event2("PROV_START", "ap", String(g_prov.apSsid()),
                                    "pwd", String(PROV_AP_PWD));
        break;

    default:
        break;
    }
}

static void onWifiResult(bool ok, const char* detail)
{
    DBG.print(F("[wifi] result "));
    DBG.print(ok ? "OK " : "FAIL ");
    DBG.println(detail);

    if (g_prov.active()) {
        g_prov.notifyResult(ok, detail);
    }
    if (ok) {
        g_link.event2("PROV_DONE", "ssid", String(g_wifi.ssidStr()), "ip", String(detail));
    } else {
        g_link.event1("PROV_FAIL", "msg", String(detail));
    }
}

/* ------------------------------------------------- 门户回调 ------------- */
static void onProvCreds(const char* ssid, const char* pass)
{
    DBG.print(F("[prov] submit ssid="));
    DBG.println(ssid);
    g_wifi.connectWith(ssid, pass, true);   /* 连上了才写 Flash */
}

static void onProvReprov(void)
{
    DBG.println(F("[prov] reprov -> clear & reboot"));
    g_store.clearCredentials();
    g_store.save();
    delay(50);
    ESP.restart();
}

/* ------------------------------------------------- MCU 命令 ------------- */
static bool onCommand(const char* verb, const char* args)
{
    char tmp[80];

    if (ciEq(verb, "PING")) {
        g_link.ok("PING");
        return true;
    }

    if (ciEq(verb, "HELLO")) {
        g_link.ok("HELLO");
        return true;
    }

    if (ciEq(verb, "WIFI.STATUS?")) {
        String s = F("+OK WIFI.STATUS state=");
        s += stateName(g_wifi.state());
        s += F(" ip=");
        s += (g_wifi.ipStr()[0] != '\0') ? g_wifi.ipStr() : "-";
        s += F(" ssid=");
        s += (g_wifi.ssidStr()[0] != '\0') ? HostLink::val(String(g_wifi.ssidStr())) : "-";
        s += F(" rssi=");
        s += String((int)g_wifi.rssi());
        s += F(" degraded=");
        s += g_wifi.degraded() ? "1" : "0";
        s += F(" prov=");
        s += g_prov.active() ? "1" : "0";
        s += F(" ap=");
        s += g_prov.active() ? g_prov.apSsid() : "-";
        s += F(" up=");
        s += String((unsigned long)(millis() / 1000UL));
        g_link.raw(s);
        return true;
    }

    if (ciEq(verb, "WIFI.CONN")) {
        char ssid[36];
        if (!HostLink::argGet(args, "ssid", ssid, sizeof(ssid))) {
            g_link.err(2, "missing_ssid");
            return true;
        }
        String pass = F("");
        if (HostLink::argGet(args, "pass", tmp, sizeof(tmp))) {
            pass = HostLink::unval(String(tmp));
        }
        String ssidStr = HostLink::unval(String(ssid));
        g_link.ok("WIFI.CONN");
        if (g_prov.active()) g_prov.notifyConnecting(ssidStr.c_str());
        g_wifi.connectWith(ssidStr.c_str(), pass.c_str(), true);
        return true;
    }

    if (ciEq(verb, "WIFI.REPROV")) {
        g_link.ok("WIFI.REPROV");
        Serial.flush();
        delay(50);
        onProvReprov();
        return true;
    }

    if (ciEq(verb, "CFG.CLEAR")) {
        g_store.clearCredentials();
        bool okSave = g_store.save();
        g_link.ok("CFG.CLEAR", "saved", okSave ? "1" : "0");
        return true;
    }

    if (ciEq(verb, "SYS.SETNAME")) {
        if (!HostLink::argGet(args, "name", tmp, sizeof(tmp))) {
            g_link.err(2, "missing_name");
            return true;
        }
        g_store.setDevName(HostLink::unval(String(tmp)).c_str());
        g_store.save();
#if CFG_ENABLE_MDNS
        if (g_wifi.online()) MDNS.begin(g_store.devName());
#endif
        g_link.ok("SYS.SETNAME", "name", String(g_store.devName()));
        return true;
    }

    if (ciEq(verb, "PROV.STATUS?")) {
        String s = F("+OK PROV.STATUS active=");
        s += g_prov.active() ? "1" : "0";
        s += F(" sta=");
        s += String((int)g_prov.staCount());
        s += F(" elapsed=");
        s += String((unsigned long)(g_prov.elapsedMs() / 1000UL));
        s += F(" ap=");
        s += g_prov.active() ? g_prov.apSsid() : "-";
        s += F(" pwd=");
        s += PROV_AP_PWD;
        g_link.raw(s);
        return true;
    }

    if (ciEq(verb, "PROV.STOP")) {
        g_prov.stop();
        g_link.ok("PROV.STOP");
        return true;
    }

    if (ciEq(verb, "SYS.INFO?")) {
        String s = F("+OK SYS.INFO fw=");
        s += CFG_FW_VERSION;
        s += F(" proto=");
        s += String((int)CFG_PROTO_VERSION);
        s += F(" chip=0x");
        s += String(ESP.getChipId(), HEX);
        s += F(" flash=");
        s += String((unsigned long)ESP.getFlashChipRealSize());
        s += F(" heap=");
        s += String((unsigned long)ESP.getFreeHeap());
        s += F(" rst=");
        s += HostLink::val(String(ESP.getResetReason()));
        s += F(" creds=");
        s += String((int)g_store.credCount());
        s += F(" fails=");
        s += String((int)g_store.failCount());
        s += F(" rxlines=");
        s += String((unsigned long)g_link.rxLines());
        s += F(" drop=");
        s += String((unsigned long)g_link.droppedLines());
        g_link.raw(s);
        return true;
    }

    if (ciEq(verb, "SYS.REBOOT")) {
        g_link.ok("SYS.REBOOT");
        Serial.flush();
        delay(50);
        ESP.restart();
        return true;
    }

    return false;   /* 交给 HostLink 回 -ERR unknown_cmd */
}

/* --------------------------------------------------------- setup -------- */
void setup()
{
    /* 调试口：UART1 只有 TX（默认 GPIO2），只发不收 */
    DBG.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
    DBG.println();
    DBG.print(F("[boot] fw="));
    DBG.print(CFG_FW_VERSION);
    DBG.print(F(" proto="));
    DBG.print((int)CFG_PROTO_VERSION);
    DBG.print(F(" chip=0x"));
    DBG.println(ESP.getChipId(), HEX);
    DBG.print(F("[boot] reset="));
    DBG.println(ESP.getResetReason());

    ledSetup();

    /* 1) 本地配置 */
    bool cfgOk = g_store.begin();
    DBG.print(F("[cfg] valid="));
    DBG.print(cfgOk ? 1 : 0);
    DBG.print(F(" creds="));
    DBG.print((int)g_store.credCount());
    DBG.print(F(" fails="));
    DBG.println((int)g_store.failCount());

    /* 2) 上电时长按按键 → 清凭据 */
    if (bootButtonHeld(3000)) {
        DBG.println(F("[cfg] button held -> clear credentials"));
        g_store.clearCredentials();
        g_store.save();
    }

    g_net.begin();

    /* 3) 串口协议要先就绪：后面的回调会往 MCU 发事件 */
    g_link.begin(CFG_HOST_BAUD);
    g_link.setHandler(onCommand);

    /* 4) 回调注册必须在 begin 之前，begin 里可能立刻进入配网 */
    g_wifi.setStateCb(onWifiState);
    g_wifi.setResultCb(onWifiResult);
    g_prov.setCredsCb(onProvCreds);
    g_prov.setReprovCb(onProvReprov);

    g_link.event2("READY", "ver", String(CFG_FW_VERSION),
                           "proto", String((int)CFG_PROTO_VERSION));

    /* 5) 上电联网判定 */
    g_wifi.begin(&g_store, &g_net);
}

/* ---------------------------------------------------------- loop -------- */
void loop()
{
    g_link.loop();
    g_wifi.loop();
    g_prov.loop();
    ledTask();

    /* 配网超时：重启，回到"再试一次联网" */
    if (g_prov.timedOut()) {
        DBG.println(F("[prov] timeout -> reboot"));
        g_link.event("PROV_TIMEOUT");
        Serial.flush();
        delay(50);
        ESP.restart();
    }

    /* 心跳：让 MCU 知道模块还活着 */
    static uint32_t hbAt = 0;
    if ((uint32_t)(millis() - hbAt) >= CFG_HOST_HB_MS) {
        hbAt = millis();
        String kvs = F("state=");
        kvs += stateName(g_wifi.state());
        kvs += F(" ip=");
        kvs += (g_wifi.ipStr()[0] != '\0') ? g_wifi.ipStr() : "-";
        kvs += F(" rssi=");
        kvs += String((int)g_wifi.rssi());
        kvs += F(" up=");
        kvs += String((unsigned long)(millis() / 1000UL));
        g_link.eventLine("HB", kvs);
    }

#if CFG_HOST_SILENCE_RESET_MS > 0
    if ((uint32_t)(millis() - g_link.lastCmdMs()) >= CFG_HOST_SILENCE_RESET_MS) {
        DBG.println(F("[link] MCU silent -> reboot"));
        ESP.restart();
    }
#endif

#if CFG_ENABLE_MDNS
    if (g_wifi.online()) MDNS.update();
#endif

    yield();
}
