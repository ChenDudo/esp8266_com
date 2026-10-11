#pragma once
/* ============================================================================
 * provisioning.h —— 配网门户（SoftAP + DNS 劫持 + HTTP 配置页 + 强制门户）
 *
 * 对外只有三件事：
 *   begin()  开热点和网页      loop()  驱动 DNS/HTTP
 *   notifyResult()  把"连上了/失败了"的结果回写到 /status 页面
 * 真正的连接动作由 WifiConn 完成，本模块不含业务逻辑。
 * ==========================================================================*/

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include "app_cfg.h"

class Provisioning {
public:
    typedef void (*CredsCb)(const char* ssid, const char* pass);
    typedef void (*VoidCb)(void);

    /* 幂等：已经在跑就直接返回 true（重复调用不会重置计时/断掉手机） */
    bool begin(const char* devName);
    void loop();
    void stop();
    void scheduleStop();                       /* 延迟 CFG_PROV_TEARDOWN_DELAY_MS 后关热点 */

    void notifyConnecting(const char* ssid);   /* MCU 侧发起连接时同步页面状态 */
    void notifyResult(bool ok, const char* detail);

    bool        active() const { return _active; }
    uint8_t     staCount() const;
    uint32_t    elapsedMs() const;
    bool        timedOut() const;
    const char* apSsid() const { return _apSsid; }
    const char* devName() const { return _devName; }
    uint8_t     lastScanCount() const { return _scanCount; }

    void setCredsCb(CredsCb cb) { _credsCb = cb; }
    void setReprovCb(VoidCb cb) { _reprovCb = cb; }

private:
    enum PageState : uint8_t { PG_IDLE = 0, PG_CONNECTING, PG_OK, PG_FAIL };

    struct ScanItem {
        char    ssid[33];
        int8_t  rssi;
        uint8_t open;
    };

    ESP8266WebServer _server;
    DNSServer        _dns;

    bool      _active;
    bool      _routesReady;   /* 路由只注册一次：ESP8266WebServer 的 stop() 不会释放 handler */
    uint8_t   _page;
    char      _apSsid[40];
    char      _devName[24];
    char      _lastSsid[33];
    char      _resultMsg[96];
    uint32_t  _t0;
    uint32_t  _stopAt;
    CredsCb   _credsCb;
    VoidCb    _reprovCb;

    ScanItem  _scan[CFG_SCAN_MAX];
    uint8_t   _scanCount;
    uint32_t  _scanAt;

    void   handleRoot();
    void   handleConnect();
    void   handleStatus();
    void   handleScanJson();
    void   handleReprov();
    void   handleInfo();
    void   handleCaptive();

    void   refreshScan(bool force);
    void   setupRoutes();
    String buildOptions() const;
    void   sendWrapped(int code, const String& body, uint32_t refreshSec);
    String pageBody(uint8_t which) const;
};
