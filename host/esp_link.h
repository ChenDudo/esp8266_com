#ifndef ESP_LINK_H
#define ESP_LINK_H
/* ============================================================================
 * esp_link.h —— 行协议解析（MCU 侧）
 *
 * 这一层只干两件事：
 *   1) 把 UART 中断喂进来的字节攒成"行"，滤掉 ESP8266 上电时的 74880bps 噪声；
 *   2) 把合法的行解析成结构化消息（+OK / -ERR / @EVT + key=value）。
 * 不懂业务，不知道什么叫"配网"。
 * ==========================================================================*/

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_MSG_NONE = 0,
    ESP_MSG_OK,      /* +OK  <VERB> [k=v ...]   —— 命令应答 */
    ESP_MSG_ERR,     /* -ERR <code> <text>              —— 命令出错 */
    ESP_MSG_EVT      /* @EVT <NAME> [k=v ...]   —— 异步事件 */
} esp_msg_kind_t;

#define ESP_MSG_MAX_KV   6
#define ESP_KV_KEY_LEN   16
#define ESP_KV_VAL_LEN   48

typedef struct {
    char key[ESP_KV_KEY_LEN];
    char val[ESP_KV_VAL_LEN];   /* 已还原 %20/%3D/%25 转义 */
} esp_kv_t;

typedef struct {
    esp_msg_kind_t kind;
    char           verb[24];    /* OK：命令名 / EVT：事件名 */
    int32_t        code;        /* 仅 ERR */
    char           text[32];    /* 仅 ERR */
    esp_kv_t       kv[ESP_MSG_MAX_KV];
    uint8_t        kv_count;
} esp_msg_t;

typedef struct {
    volatile uint32_t rx_bytes;       /* 收到的原始字节数 */
    volatile uint32_t overflow;       /* 环形缓冲溢出 / 超长行 */
    volatile uint32_t lines_ok;       /* 解析成功的行 */
    volatile uint32_t lines_dropped;  /* 丢弃的行（启动噪声等） */
} esp_link_stats_t;

void Esp_LinkInit(void);
/* 在 UART 接收中断里调用 */
void Esp_LinkFeedByte(uint8_t b);
void Esp_LinkFeed(const uint8_t *p, uint16_t n);
/* 丢缓冲 + 丢半行（复位 ESP 时调用，防止把上一轮的残留当成新消息） */
void Esp_LinkReset(void);
/* 主循环里循环调用，取到一条消息返回 true */
bool Esp_LinkPoll(esp_msg_t *msg);

const esp_link_stats_t *Esp_LinkStats(void);

/* 取值工具 */
const char *Esp_KvGet(const esp_msg_t *msg, const char *key);
bool        Esp_KvGetStr(const esp_msg_t *msg, const char *key, char *out, uint16_t out_len);
int32_t     Esp_KvGetInt(const esp_msg_t *msg, const char *key, int32_t def);

#ifdef __cplusplus
}
#endif

#endif /* ESP_LINK_H */
