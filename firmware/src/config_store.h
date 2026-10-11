#pragma once
/* ============================================================================
 * config_store.h —— 凭据 / 设备参数的 Flash 持久化
 *
 * 介质：ESP8266 Arduino core 的 EEPROM（本质是 Flash 里 1 个 4KB 扇区）。
 * 布局：两个 512 字节的槽 A/B 交替写入，掉电最多丢最后一次修改，不会全丢。
 *       每个槽 = 头部(魔数/版本/序号/长度/CRC16) + 负载(AppCfg)。
 *
 * 用法：
 *     ConfigStore store;
 *     store.begin();                     // 必须在 WiFi 相关操作之前
 *     if (store.credCount() == 0) ...
 *     store.addOrUpdateCredential(ssid, pass);
 *     store.save();
 * ==========================================================================*/

#include <Arduino.h>

#define CFG_MAX_CREDS   2       /* 记住 2 组 WiFi（家里 / 公司） */
#define CFG_SSID_LEN    33      /* 32 + '\0' */
#define CFG_PASS_LEN    65      /* 64 + '\0' */
#define CFG_NAME_LEN    24

struct WifiCred {
    char ssid[CFG_SSID_LEN];
    char pass[CFG_PASS_LEN];
};

struct AppCfg {
    uint16_t version;
    uint16_t flags;         /* bit0：曾经配网成功过 */
    uint16_t failCount;     /* 连续上电联网失败次数（用于自动清凭据） */
    uint8_t  credCount;
    uint8_t  reserved;
    char     devName[CFG_NAME_LEN];
    WifiCred creds[CFG_MAX_CREDS];
};

class ConfigStore {
public:
    bool begin();                       /* 初始化 EEPROM 并载入；返回是否载入到有效配置 */

    bool          valid() const { return _valid; }
    const AppCfg& cfg()   const { return _cfg; }

    /* ---- 凭据 ---- */
    uint8_t        credCount() const { return _valid ? _cfg.credCount : 0; }
    const WifiCred* credAt(uint8_t i) const;
    bool           addOrUpdateCredential(const char* ssid, const char* pass);
    void           clearCredentials();

    /* ---- 设备名 ---- */
    const char* devName() const { return _cfg.devName; }
    void        setDevName(const char* name);

    /* ---- 失败计数 ---- */
    uint16_t failCount() const { return _cfg.failCount; }
    void     setFailCount(uint16_t n) { _cfg.failCount = n; }

    /* ---- 落盘 / 复位 ---- */
    bool save();                        /* 写入非活动槽；成功返回 true */
    void factoryReset();                /* 两个槽全部清空 */

    uint16_t lastCrc() const { return _lastCrc; }

private:
    AppCfg   _cfg;
    bool     _valid;
    uint16_t _lastCrc;
    int8_t   _activeSlot;               /* -1 = 无有效槽 */

    void     loadDefaults();
    void     sanitize();
    int8_t   scanSlots(uint16_t* seqOut);
};
