#pragma once
/* ============================================================================
 * wifi_conn.h —— 上电联网状态机（非阻塞）
 *
 *   INIT ──有凭据──> CONNECTING ──成功──> ONLINE
 *                       │                  │
 *                       └──失败/无凭据──┐  └──掉线超时──> CONNECTING
 *                                       ↓
 *                                 PROVISIONING（由 main 启动门户）
 *
 * 关键点：
 *  - 全程非阻塞（不用 waitForConnectResult），所以配网门户的 HTTP 服务不会被卡住。
 *  - 密码错误 → 立刻判失败，不无脑重试。
 *  - 连接成功后先验证再落盘（remember=true 时）；连续失败 N 次自动清凭据。
 * ==========================================================================*/

#include <Arduino.h>
#include "config_store.h"
#include "net_check.h"

enum WifiConnState : uint8_t {
    WC_INIT = 0,
    WC_CONNECTING,
    WC_ONLINE,
    WC_PROVISIONING,
};

enum WifiFailReason : uint8_t {
    WF_NONE = 0,
    WF_WRONG_PASSWORD,
    WF_NO_AP,
    WF_CONNECT_FAILED,
    WF_TIMEOUT,
    WF_NO_CREDENTIAL,
};

class WifiConn {
public:
    typedef void (*StateCb)(uint8_t state);
    typedef void (*ResultCb)(bool ok, const char* detail);

    void begin(ConfigStore* store, NetCheck* net);
    void loop();

    /* 用指定凭据连接（非阻塞）。remember=true 时连接成功才写 Flash。
     * 结果通过 setResultCb 回调（配网门户用它更新 /status 页面）。 */
    bool connectWith(const char* ssid, const char* pass, bool remember);

    /* 清掉当前凭据并进入配网模式（门户由 main 在状态回调里启动） */
    void startProvisioning();
    /* 忘记所有凭据 */
    void forgetAll();

    uint8_t     state() const      { return _state; }
    bool        online() const     { return _state == WC_ONLINE; }
    bool        degraded() const   { return _degraded; }   /* 链路在但外网不通 */
    const char* ipStr() const      { return _ip; }
    const char* ssidStr() const    { return _ssid; }
    int16_t     rssi() const;
    const char* failText() const;
    const char* probeDetail() const { return _net ? _net->detail() : ""; }
    uint32_t    connectedAt() const  { return _connectedAt; }

    void setStateCb(StateCb cb)   { _stateCb = cb; }
    void setResultCb(ResultCb cb) { _resultCb = cb; }

private:
    enum Phase : uint8_t { PH_IDLE = 0, PH_START, PH_WAIT };

    ConfigStore* _store;
    NetCheck*    _net;

    uint8_t  _state;
    uint8_t  _phase;
    uint8_t  _credIdx;
    uint8_t  _retry;
    uint8_t  _failReason;
    bool     _saveOnSuccess;
    bool     _degraded;
    bool     _pendingResult;

    uint32_t _t0;
    uint32_t _connectedAt;
    uint32_t _lastCheck;
    uint32_t _lastProbe;
    uint32_t _lostAt;

    uint8_t  _probeFail;
    uint16_t _bootFailCount;

    char _ssid[CFG_SSID_LEN];
    char _pass[CFG_PASS_LEN];
    char _ip[16];

    StateCb  _stateCb;
    ResultCb _resultCb;

    void setState(uint8_t s);
    void beginAttempt(uint8_t credIdx, bool saveOnSuccess);
    void beginAttemptRaw(const char* ssid, const char* pass);
    void onConnected();
    void onFailed(uint8_t reason);
    void doProbe();
    void refreshIp();
};
