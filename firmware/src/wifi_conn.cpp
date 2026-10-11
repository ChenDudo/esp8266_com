#include "wifi_conn.h"
#include "app_cfg.h"
#include <ESP8266WiFi.h>

/* ---------------------------------------------------------------- 初始化 --- */
void WifiConn::begin(ConfigStore* store, NetCheck* net)
{
    _store = store;
    _net   = net;

    _state         = WC_INIT;
    _phase         = PH_IDLE;
    _credIdx       = 0;
    _retry         = 0;
    _failReason    = WF_NONE;
    _saveOnSuccess = false;
    _degraded      = false;
    _pendingResult = false;
    _t0 = _connectedAt = _lastCheck = _lastProbe = _lostAt = 0;
    _probeFail     = 0;
    _bootFailCount = _store->failCount();
    _ip[0] = '\0';
    _ssid[0] = '\0';
    _pass[0] = '\0';

    WiFi.persistent(false);        /* 不写 SDK 自己的 Flash 记录，避免和 ConfigStore 打架 */
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(true);   /* 掉线由 SDK 先兜一层 */
#if CFG_WIFI_NO_SLEEP
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
    WiFi.mode(WIFI_STA);

    if (_store->credCount() == 0) {
        _failReason = WF_NO_CREDENTIAL;
        startProvisioning();
    } else {
        beginAttempt(0, false);
    }
}

/* ------------------------------------------------------------ 状态切换 --- */
void WifiConn::setState(uint8_t s)
{
    if (_state == s) return;
    _state = s;
    if (_stateCb) _stateCb(s);
}

void WifiConn::refreshIp()
{
    IPAddress ip = WiFi.localIP();
    snprintf(_ip, sizeof(_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

int16_t WifiConn::rssi() const
{
    return (int16_t)WiFi.RSSI();
}

const char* WifiConn::failText() const
{
    switch (_failReason) {
    case WF_WRONG_PASSWORD: return "密码错误";
    case WF_NO_AP:          return "找不到这个 WiFi（可能是 5GHz 或信号太远）";
    case WF_CONNECT_FAILED: return "路由器拒绝了连接";
    case WF_TIMEOUT:        return "连接超时";
    case WF_NO_CREDENTIAL:  return "设备里还没有 WiFi 信息";
    default:                return "未知错误";
    }
}

/* ------------------------------------------------------------ 发起连接 --- */
void WifiConn::beginAttemptRaw(const char* ssid, const char* pass)
{
    _failReason = WF_NONE;
    _t0         = millis();
    _phase      = PH_START;
    setState(WC_CONNECTING);
}

void WifiConn::beginAttempt(uint8_t credIdx, bool saveOnSuccess)
{
    const WifiCred* c = _store->credAt(credIdx);
    if (c == NULL) {
        onFailed(WF_NO_CREDENTIAL);
        return;
    }
    strncpy(_ssid, c->ssid, sizeof(_ssid) - 1);
    _ssid[sizeof(_ssid) - 1] = '\0';
    strncpy(_pass, c->pass, sizeof(_pass) - 1);
    _pass[sizeof(_pass) - 1] = '\0';

    _credIdx       = credIdx;
    _saveOnSuccess = saveOnSuccess;
    beginAttemptRaw(_ssid, _pass);
}

bool WifiConn::connectWith(const char* ssid, const char* pass, bool remember)
{
    if (ssid == NULL || ssid[0] == '\0') return false;

    strncpy(_ssid, ssid, sizeof(_ssid) - 1);
    _ssid[sizeof(_ssid) - 1] = '\0';
    strncpy(_pass, (pass != NULL) ? pass : "", sizeof(_pass) - 1);
    _pass[sizeof(_pass) - 1] = '\0';

    /* 让 _credIdx 指向"无处可退"，失败的兜底逻辑就不会拿旧的凭据去试 */
    _credIdx       = _store->credCount();
    _retry         = 0;
    _saveOnSuccess = remember;
    _pendingResult = (_resultCb != NULL);
    beginAttemptRaw(_ssid, _pass);
    return true;
}

/* ------------------------------------------------------------ 连接成功 --- */
void WifiConn::onConnected()
{
    _phase       = PH_IDLE;
    _retry       = 0;
    _probeFail   = 0;
    _degraded    = false;
    _lostAt      = 0;
    _connectedAt = millis();
    _lastCheck   = _connectedAt;
    _lastProbe   = _connectedAt;
    refreshIp();

    if (_saveOnSuccess) {
        _store->addOrUpdateCredential(_ssid, _pass);
        _store->setFailCount(0);
        _store->save();
        _saveOnSuccess = false;
    } else if (_store->failCount() != 0) {
        _store->setFailCount(0);
        _store->save();
    }
    _bootFailCount = 0;

    setState(WC_ONLINE);

    if (_pendingResult && _resultCb) {
        _pendingResult = false;
        _resultCb(true, _ip);
    }
}

/* ------------------------------------------------------------ 连接失败 --- */
void WifiConn::onFailed(uint8_t reason)
{
    _failReason = reason;
    _phase      = PH_IDLE;

    /* 密码错直接放弃这个凭据，不重试（用户等得久，体验上"错就是错"） */
    if (reason != WF_WRONG_PASSWORD && reason != WF_NO_CREDENTIAL) {
        _retry++;
        if (_retry < CFG_STA_RETRY_PER_CRED) {
            _t0    = millis();
            _phase = PH_START;
            return;
        }
        _retry = 0;
        if ((uint8_t)(_credIdx + 1) < _store->credCount()) {
            beginAttempt((uint8_t)(_credIdx + 1), false);
            return;
        }
    }

    _bootFailCount++;
    _store->setFailCount(_bootFailCount);
    _store->save();
    if (_bootFailCount >= CFG_REPROV_AFTER_FAILS) {
        _store->clearCredentials();     /* 防止设备永久死磕一个已经作废的 AP */
        _store->save();
        _bootFailCount = 0;
    }

    if (_pendingResult && _resultCb) {
        _pendingResult = false;
        _resultCb(false, failText());
    }
    startProvisioning();
}

/* -------------------------------------------------------------- 探测 ---- */
void WifiConn::doProbe()
{
    if (_net == NULL) return;
    if (_net->probe(CFG_PROBE_TIMEOUT_MS)) {
        _probeFail = 0;
        _degraded  = false;
    } else if (_probeFail < 255) {
        _probeFail++;
        if (_probeFail >= CFG_PROBE_FAIL_LIMIT) _degraded = true;
    }
}

/* ------------------------------------------------------------ 主循环 ---- */
void WifiConn::loop()
{
    uint32_t now = millis();

    if (_phase == PH_START) {
        WiFi.disconnect(false);
        delay(20);
        WiFi.begin(_ssid, (_pass[0] != '\0') ? _pass : NULL);
        _phase = PH_WAIT;
        _t0    = now;
        return;
    }

    if (_phase == PH_WAIT) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED)      { onConnected();            return; }
        if (st == WL_WRONG_PASSWORD) { onFailed(WF_WRONG_PASSWORD); return; }
        if (st == WL_NO_SSID_AVAIL)  { onFailed(WF_NO_AP);       return; }
        if (st == WL_CONNECT_FAILED) { onFailed(WF_CONNECT_FAILED); return; }
        if ((uint32_t)(now - _t0) >= CFG_STA_CONNECT_TIMEOUT_MS) {
            onFailed(WF_TIMEOUT);
        }
        return;
    }

    /* ---- PH_IDLE ---- */
    if (_state != WC_ONLINE) return;

    if ((uint32_t)(now - _lastCheck) >= 5000UL) {
        _lastCheck = now;
        if (WiFi.status() != WL_CONNECTED) {
            if (_lostAt == 0) _lostAt = now;
            if ((uint32_t)(now - _lostAt) >= CFG_LINK_LOST_TIMEOUT_MS) {
                _lostAt = 0;
                _ip[0]  = '\0';
                beginAttempt(0, false);
                return;
            }
        } else {
            _lostAt = 0;
            refreshIp();
        }
    }

    if (WiFi.status() == WL_CONNECTED &&
        (uint32_t)(now - _lastProbe) >= CFG_PROBE_INTERVAL_MS) {
        _lastProbe = now;
        doProbe();
    }
}

/* ---------------------------------------------------------- 配网入口 ---- */
void WifiConn::startProvisioning()
{
    _phase = PH_IDLE;
    WiFi.disconnect(false);
    setState(WC_PROVISIONING);
}

void WifiConn::forgetAll()
{
    _store->clearCredentials();
    _store->save();
    _bootFailCount = 0;
    startProvisioning();
}
