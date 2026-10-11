#include "provisioning.h"
#include "portal_html.h"
#include "board_pins.h"
#include <ESP8266WiFi.h>
#if CFG_USE_DHCP_OPT114
#include <user_interface.h>
#endif

/* 手机 / 各系统做"强制门户"探测时会访问这些地址，全部 302 到门户首页 */
static const char* kCaptivePaths[] = {
    "/generate_204",                 /* Android */
    "/gen_204",                      /* Android(旧) */
    "/hotspot-detect.html",          /* iOS / macOS */
    "/library/test/success.html",    /* iOS(旧) */
    "/connecttest.txt",              /* Windows */
    "/ncsi.txt",                     /* Windows */
    "/redirect",                     /* Windows Phone */
    "/canonical.html",               /* Firefox */
    "/success.txt",                  /* Firefox */
    "/kindle-wifi/wifistub.html",    /* Kindle */
};
static const uint8_t kCaptiveCount = (uint8_t)(sizeof(kCaptivePaths) / sizeof(kCaptivePaths[0]));

static const uint32_t kScanTtlMs = 30000UL;

/* ------------------------------------------------------------ 工具函数 --- */
static String htmlEscape(const String& in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
        case '&':  out += F("&amp;");  break;
        case '<':  out += F("&lt;");   break;
        case '>':  out += F("&gt;");   break;
        case '"':  out += F("&quot;"); break;
        case '\'': out += F("&#39;");  break;
        default:   out += c;           break;
        }
    }
    return out;
}

static String jsonEscape(const String& in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 0x20) { out += ' '; }
        else out += c;
    }
    return out;
}

/* -------------------------------------------------------------- 启动 ---- */
bool Provisioning::begin(const char* devName)
{
    if (_active) return true;

    strncpy(_devName, (devName != NULL && devName[0] != '\0') ? devName : PROV_DEV_NAME,
            sizeof(_devName) - 1);
    _devName[sizeof(_devName) - 1] = '\0';

    snprintf(_apSsid, sizeof(_apSsid), "%s%04X",
             PROV_AP_SSID_PREFIX, (unsigned)(ESP.getChipId() & 0xFFFF));

    WiFi.persistent(false);
    /* AP_STA：既能当热点（手机连进来），又能在 STA 侧扫描/连接路由器 */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(AP_IP_0, AP_IP_1, AP_IP_2, AP_IP_3),
                      IPAddress(AP_IP_0, AP_IP_1, AP_IP_2, AP_IP_3),
                      IPAddress(255, 255, 255, 0));
    WiFi.softAP(_apSsid, PROV_AP_PWD, PROV_AP_CHANNEL, 0, PROV_AP_MAX_CONN);
    delay(150);
    WiFi.hostname(_devName);

#if CFG_USE_DHCP_OPT114
    /* RFC7710：DHCP option 114 告诉手机"门户在 192.168.4.1"，部分系统会自动弹窗 */
    dhcp_set_captiveportal_url("http://192.168.4.1/");
#endif

    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", WiFi.softAPIP());

    setupRoutes();
    _server.begin();

    _page        = PG_IDLE;
    _stopAt      = 0;
    _scanAt      = 0;
    _scanCount   = 0;
    _lastSsid[0] = '\0';
    _resultMsg[0] = '\0';
    _t0          = millis();
    _active      = true;

    return true;
}

/* 路由只注册一次。
 * ESP8266WebServer::stop() 不会释放已注册的 handler（也没有公开的清除接口），
 * 每次 begin() 都注册一遍会让 handler 链表随配网次数不断变长。
 * lambda 捕获的是 this（全局对象），跨 stop/begin 一直有效。 */
void Provisioning::setupRoutes()
{
    if (_routesReady) return;
    _routesReady = true;

    _server.on("/",                   [this]() { handleRoot(); });
    _server.on("/connect", HTTP_POST,  [this]() { handleConnect(); });
    _server.on("/status",             [this]() { handleStatus(); });
    _server.on("/scan",               [this]() { handleScanJson(); });
    _server.on("/reprov", HTTP_POST,   [this]() { handleReprov(); });
    _server.on("/info",               [this]() { handleInfo(); });
    for (uint8_t i = 0; i < kCaptiveCount; i++) {
        _server.on(kCaptivePaths[i], [this]() { handleCaptive(); });
    }
    _server.onNotFound([this]() { handleCaptive(); });
}

void Provisioning::loop()
{
    if (!_active) return;

    _dns.processNextRequest();
    _server.handleClient();

    if (_stopAt != 0 && (int32_t)(millis() - _stopAt) >= 0) {
        stop();
    }
}

void Provisioning::stop()
{
    if (!_active) return;

    _server.stop();
    _dns.stop();
#if CFG_USE_DHCP_OPT114
    dhcp_set_captiveportal_url("");
#endif
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);

    _active      = false;
    _stopAt      = 0;
    _page        = PG_IDLE;
    _scanAt      = 0;
    _scanCount   = 0;
}

void Provisioning::scheduleStop()
{
    if (!_active) return;
    _stopAt = millis() + CFG_PROV_TEARDOWN_DELAY_MS;
}

uint8_t Provisioning::staCount() const
{
    return (uint8_t)WiFi.softAPgetStationNum();
}

uint32_t Provisioning::elapsedMs() const
{
    return _active ? (uint32_t)(millis() - _t0) : 0;
}

bool Provisioning::timedOut() const
{
    return _active && ((uint32_t)(millis() - _t0) >= CFG_PROV_TIMEOUT_MS);
}

/* ---------------------------------------------------------- 扫描 / 列表 -- */
void Provisioning::refreshScan(bool force)
{
    if (!force && _scanAt != 0 && (uint32_t)(millis() - _scanAt) < kScanTtlMs) {
        return;
    }

    int n = WiFi.scanNetworks();     /* 阻塞约 1.5~2.5s，只在明确需要时做 */
    _scanCount = 0;
    _scanAt    = millis();

    if (n > 0) {
        /* 先按 RSSI 从强到弱做一次插入排序 */
        int idx[CFG_SCAN_MAX * 2];
        int cnt = 0;
        int lim = (n < (int)(CFG_SCAN_MAX * 2)) ? n : (int)(CFG_SCAN_MAX * 2);
        for (int i = 0; i < lim; i++) {
            if (WiFi.SSID(i).length() == 0) continue;   /* 隐藏网络跳过 */
            int key = i;
            int j   = cnt - 1;
            while (j >= 0 && WiFi.RSSI(idx[j]) < WiFi.RSSI(key)) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = key;
            cnt++;
        }
        /* 去重后填进缓存 */
        for (int i = 0; i < cnt && _scanCount < CFG_SCAN_MAX; i++) {
            String s = WiFi.SSID(idx[i]);
            bool dup = false;
            for (uint8_t k = 0; k < _scanCount; k++) {
                if (strcmp(_scan[k].ssid, s.c_str()) == 0) { dup = true; break; }
            }
            if (dup) continue;
            strncpy(_scan[_scanCount].ssid, s.c_str(), sizeof(_scan[0].ssid) - 1);
            _scan[_scanCount].ssid[sizeof(_scan[0].ssid) - 1] = '\0';
            _scan[_scanCount].rssi = (int8_t)WiFi.RSSI(idx[i]);
            _scan[_scanCount].open = (WiFi.encryptionType(idx[i]) == ENC_TYPE_NONE) ? 1 : 0;
            _scanCount++;
        }
    }
    WiFi.scanDelete();
}

String Provisioning::buildOptions() const
{
    String opts;
    if (_scanCount == 0) {
        return String(F("<option value=\"\">（没扫到网络，请在下面手动输入）</option>"));
    }
    for (uint8_t i = 0; i < _scanCount; i++) {
        String label = htmlEscape(String(_scan[i].ssid));
        opts += F("<option value=\"");
        opts += label;
        opts += F("\">");
        opts += label;
        opts += F("　");
        opts += String((int)_scan[i].rssi);
        opts += F("dBm");
        opts += _scan[i].open ? F("　开放") : F("");
        opts += F("</option>");
    }
    return opts;
}

/* ------------------------------------------------------------ 页面 ------ */
void Provisioning::sendWrapped(int code, const String& body, uint32_t refreshSec)
{
    String page = FPSTR(PROV_PAGE_HEAD);
    String refresh;
    if (refreshSec > 0) {
        refresh  = F("<meta http-equiv=\"refresh\" content=\"");
        refresh += String((unsigned long)refreshSec);
        refresh += F(";url=/status\">");
    }
    page.replace(F("%REFRESH%"), refresh);
    page.replace(F("%TITLE%"), F("设备配网"));
    page += body;
    page += FPSTR(PROV_PAGE_TAIL);

    _server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate"));
    _server.send(code, F("text/html; charset=utf-8"), page);
}

String Provisioning::pageBody(uint8_t which) const
{
    String body;
    const char* apName = _apSsid;
    const char* devName = _devName;

    switch (which) {
    case PG_CONNECTING: {
        body = FPSTR(PROV_CONNECTING_BODY);
        body.replace(F("%SSID%"), htmlEscape(String(_lastSsid)));
        body.replace(F("%APNAME%"), apName);
        break;
    }
    case PG_OK: {
        body = FPSTR(PROV_OK_BODY);
        body.replace(F("%SSID%"), htmlEscape(String(_lastSsid)));
        body.replace(F("%IP%"), htmlEscape(String(_resultMsg)));
        break;
    }
    case PG_FAIL: {
        body = FPSTR(PROV_FAIL_BODY);
        body.replace(F("%MSG%"), htmlEscape(String(_resultMsg)));
        break;
    }
    default: {
        body = FPSTR(PROV_FORM_BODY);
        body.replace(F("%DEVNAME%"), htmlEscape(String(devName)));
        body.replace(F("%APNAME%"), apName);
        body.replace(F("%OPTIONS%"), buildOptions());
        body.replace(F("%TIMEOUT%"), String((unsigned long)(CFG_PROV_TIMEOUT_MS / 60000UL)));
        body.replace(F("%MSG%"), F(""));
        break;
    }
    }
    return body;
}

void Provisioning::handleRoot()
{
    refreshScan(_server.hasArg("r"));
    sendWrapped(200, pageBody(PG_IDLE), 0);
}

void Provisioning::handleScanJson()
{
    refreshScan(true);
    String j = F("{\"n\":");
    j += String((int)_scanCount);
    j += F(",\"ap\":[");
    for (uint8_t i = 0; i < _scanCount; i++) {
        if (i) j += ',';
        j += F("{\"s\":\"");
        j += jsonEscape(String(_scan[i].ssid));
        j += F("\",\"r\":");
        j += String((int)_scan[i].rssi);
        j += F(",\"o\":");
        j += String((int)_scan[i].open);
        j += F("}");
    }
    j += F("]}");
    _server.sendHeader(F("Cache-Control"), F("no-store"));
    _server.send(200, F("application/json; charset=utf-8"), j);
}

void Provisioning::handleInfo()
{
    String j = F("{\"dev\":\"");
    j += jsonEscape(String(_devName));
    j += F("\",\"ap\":\"");
    j += jsonEscape(String(_apSsid));
    j += F("\",\"fw\":\"");
    j += CFG_FW_VERSION;
    j += F("\",\"sta\":");
    j += String((int)staCount());
    j += F(",\"up\":");
    j += String((unsigned long)(millis() / 1000UL));
    j += F("}");
    _server.sendHeader(F("Cache-Control"), F("no-store"));
    _server.send(200, F("application/json; charset=utf-8"), j);
}

void Provisioning::handleConnect()
{
    String ssid = _server.arg("ssid_manual");
    ssid.trim();
    if (ssid.length() == 0) {
        ssid = _server.arg("ssid");
        ssid.trim();
    }
    String pass = _server.arg("pass");

    if (ssid.length() == 0 || ssid.length() > 32) {
        strncpy(_resultMsg, "请选择或输入 WiFi 名称", sizeof(_resultMsg) - 1);
        _resultMsg[sizeof(_resultMsg) - 1] = '\0';
        _page = PG_FAIL;
        sendWrapped(200, pageBody(PG_FAIL), 0);
        return;
    }

    strncpy(_lastSsid, ssid.c_str(), sizeof(_lastSsid) - 1);
    _lastSsid[sizeof(_lastSsid) - 1] = '\0';
    _resultMsg[0] = '\0';
    _page = PG_CONNECTING;

    if (_credsCb) {
        _credsCb(ssid.c_str(), pass.c_str());
    }

    sendWrapped(200, pageBody(PG_CONNECTING), 3);
}

void Provisioning::handleStatus()
{
    uint32_t refresh = (uint8_t)_page == PG_CONNECTING ? 3UL : 0UL;
    sendWrapped(200, pageBody(_page), refresh);
}

void Provisioning::handleReprov()
{
    _page = PG_IDLE;
    String body = F("<h1>已清除</h1><div class=\"msg ok\">"
                    "设备正在重启并重新进入配网模式，请稍后重新连接热点。</div>");
    sendWrapped(200, body, 0);
    delay(200);                       /* 让响应先发出去 */
    if (_reprovCb) _reprovCb();
}

void Provisioning::handleCaptive()
{
    _server.sendHeader(F("Location"), F("http://192.168.4.1/"), true);
    _server.sendHeader(F("Cache-Control"), F("no-store"));
    _server.send(302, F("text/plain; charset=utf-8"), F(""));
}

/* ------------------------------------------------------ 结果回写 -------- */
void Provisioning::notifyConnecting(const char* ssid)
{
    if (!_active) return;
    if (ssid != NULL) {
        strncpy(_lastSsid, ssid, sizeof(_lastSsid) - 1);
        _lastSsid[sizeof(_lastSsid) - 1] = '\0';
    }
    _resultMsg[0] = '\0';
    _page = PG_CONNECTING;
}

void Provisioning::notifyResult(bool ok, const char* detail)
{
    if (!_active) return;
    strncpy(_resultMsg, (detail != NULL) ? detail : "", sizeof(_resultMsg) - 1);
    _resultMsg[sizeof(_resultMsg) - 1] = '\0';
    _page = ok ? PG_OK : PG_FAIL;
}
