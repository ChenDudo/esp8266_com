#ifndef ESP_CLIENT_H
#define ESP_CLIENT_H
/* ============================================================================
 * esp_client.h —— ESP8266 模块客户端（MCU 侧状态机）
 *
 * 职责：
 *   - 用 RST 脚把模块复位到已知状态，等 @EVT READY 完成握手；
 *   - 把模块发来的 +OK / -ERR / @EVT 翻译成 MCU 关心的事件回调；
 *   - 心跳看门狗：模块 20 秒不吭声就硬复位重来；
 *   - 提供发命令的 API（配网、重连、状态查询、裸命令）。
 *
 * 用法（见 example_esp_app.c）：
 *   Esp_HwInit(115200);          // 移植层：串口/时钟/RST 脚
 *   Esp_Init(&cbs);              // 注册回调，开始复位握手
 *   while (1) { Esp_Poll(); ... } // 主循环，建议 <=20ms 一次
 *
 * MCU 侧只关心 3 个状态：OFFLINE / PROV（配网中）/ ONLINE，其余细节都在回调里。
 * ==========================================================================*/

#include <stdint.h>
#include <stdbool.h>
#include "esp_link.h"
#include "esp_port.h"     /* Esp_Port() / esp_port_t：应用层取 tick 也要用 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_ST_RESET = 0,     /* 正在拉低 RST 复位模块 */
    ESP_ST_WAIT_READY,    /* 等 @EVT READY */
    ESP_ST_OFFLINE,       /* 模块在线，但没联网 */
    ESP_ST_PROV,          /* 模块正在配网（开了热点） */
    ESP_ST_ONLINE,        /* 已联网 */
    ESP_ST_ERROR          /* 复位若干次仍无响应，放弃 */
} esp_state_t;

typedef struct {
    esp_state_t state;
    uint8_t  prov_active;      /* 1 = 配网热点已开启 */
    uint8_t  degraded;         /* 1 = 连上路由器但外网不通 */
    uint8_t  reset_count;
    int16_t  rssi;
    uint32_t uptime_s;
    uint32_t online_since_ms;
    uint32_t last_msg_ms;      /* 最近一次收到模块消息的时间 */
    char     ip[16];
    char     ssid[33];         /* 正在连 / 已连上的 SSID */
    char     ap_ssid[40];      /* 配网热点名 */
    char     ap_pwd[16];       /* 配网热点密码 */
    char     fail_msg[48];     /* 最近一次配网失败原因（中文，来自模块） */
    char     fw[16];
    int32_t  proto;
} esp_status_t;

typedef struct {
    void (*on_ready)(const char *fw, int32_t proto);
    void (*on_state_changed)(esp_state_t old_st, esp_state_t new_st, const esp_status_t *st);
    void (*on_prov_start)(const char *ap_ssid, const char *ap_pwd);
    void (*on_prov_done)(const char *ssid, const char *ip);
    void (*on_got_ip)(const char *ip, int16_t rssi);
    void (*on_fail)(const char *msg);        /* 配网失败，模块会自动回到配网模式 */
    void (*on_timeout)(void);                /* 配网超时，模块即将重启 */
    void (*on_wifi_lost)(void);
    void (*on_error)(int32_t code, const char *text);
} esp_cbs_t;

/* ---- 生命周期 ---- */
void Esp_Init(const esp_cbs_t *cbs);          /* 注册回调并开始复位握手（非阻塞） */
void Esp_Poll(void);                          /* 主循环调用 */
bool Esp_WaitReady(uint32_t timeout_ms);      /* 阻塞版：等握手完成（初始化时方便） */

/* ---- 控制 ---- */
void Esp_SendLine(const char *line);          /* 直接下发一行（不用带 \r\n） */
void Esp_HardReset(void);                     /* 拉低 RST 重新握手 */
void Esp_Reprov(void);                        /* 清凭据并进入配网（模块会重启） */
void Esp_Connect(const char *ssid, const char *pass);  /* 指定凭据联网 */
void Esp_QueryStatus(void);                   /* 主动查询一次状态 */
void Esp_PingAsync(void);

/* ---- 同步式的简单探测（阻塞 <=timeout_ms，仅初始化时用） ---- */
bool Esp_Ping(uint32_t timeout_ms);

const esp_status_t *Esp_Status(void);
const char *Esp_StateName(esp_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* ESP_CLIENT_H */
