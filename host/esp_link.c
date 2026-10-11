#include "esp_link.h"
#include <string.h>
#include <stdlib.h>

#define ESP_LINK_RX_BUF   256u   /* 必须是 2 的幂以外的值也可以，这里用取模 */
#define ESP_LINK_LINE_MAX 160u   /* 单行最大长度，超过就丢掉重新同步 */

static volatile uint8_t  s_rx[ESP_LINK_RX_BUF];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;

static char              s_line[ESP_LINK_LINE_MAX];
static uint16_t          s_line_len;
static esp_link_stats_t  s_stats;

/* --------------------------------------------------------------- 缓冲 ---- */
void Esp_LinkInit(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_head     = 0;
    s_tail     = 0;
    s_line_len = 0;
    s_line[0]  = '\0';
}

void Esp_LinkFeedByte(uint8_t b)
{
    uint16_t next = (uint16_t)((s_head + 1u) % ESP_LINK_RX_BUF);

    s_stats.rx_bytes++;
    if (next == s_tail) {
        s_stats.overflow++;      /* 主循环没跟上，丢新数据 */
        return;
    }
    s_rx[s_head] = b;
    s_head       = next;
}

void Esp_LinkFeed(const uint8_t *p, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        Esp_LinkFeedByte(p[i]);
    }
}

void Esp_LinkReset(void)
{
    s_tail     = s_head;
    s_line_len = 0;
}

const esp_link_stats_t *Esp_LinkStats(void)
{
    return &s_stats;
}

/* --------------------------------------------------------------- 解析 ---- */
/* 启动噪声里含有不可打印字符，含则整行丢弃 */
static bool line_is_clean(const char *s)
{
    while (*s != '\0') {
        uint8_t c = (uint8_t)(*s++);
        if (c < 0x20u || c > 0x7Eu) {
            return false;
        }
    }
    return true;
}

/* 还原 %20 %3D %25 */
static void unescape(const char *in, uint16_t in_len, char *out, uint16_t out_len)
{
    uint16_t i = 0;
    uint16_t o = 0;

    while (i < in_len && o + 1u < out_len) {
        if (in[i] == '%' && (uint16_t)(i + 2u) < in_len) {
            char h1 = in[i + 1u];
            char h2 = in[i + 2u];
            int  a  = (h1 >= '0' && h1 <= '9') ? (h1 - '0')
                    : ((h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) : -1);
            int  b  = (h2 >= '0' && h2 <= '9') ? (h2 - '0')
                    : ((h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) : -1);
            if (a >= 0 && b >= 0) {
                out[o++] = (char)((a << 4) | b);
                i += 3u;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static uint16_t copy_clip(char *dst, uint16_t dst_len, const char *src, uint16_t src_len)
{
    if (src_len >= dst_len) {
        src_len = (uint16_t)(dst_len - 1u);
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return src_len;
}

static bool parse_line(char *line, esp_msg_t *msg)
{
    char *p;
    char *e;
    uint16_t n;

    memset(msg, 0, sizeof(*msg));

    if (!line_is_clean(line)) {
        return false;
    }

    p = line;
    if (strncmp(p, "+OK", 3) == 0) {
        msg->kind = ESP_MSG_OK;
        p += 3;
    } else if (strncmp(p, "-ERR", 4) == 0) {
        msg->kind = ESP_MSG_ERR;
        p += 4;
    } else if (strncmp(p, "@EVT", 4) == 0) {
        msg->kind = ESP_MSG_EVT;
        p += 4;
    } else {
        return false;
    }

    while (*p == ' ') p++;

    /* ---- -ERR <code> <text> ---- */
    if (msg->kind == ESP_MSG_ERR) {
        char tmp[12];
        e = p;
        while (*e != '\0' && *e != ' ') e++;
        n = (uint16_t)(e - p);
        if (n > 0) {
            copy_clip(tmp, sizeof(tmp), p, n);
            msg->code = (int32_t)strtol(tmp, NULL, 10);
        }
        p = e;
        while (*p == ' ') p++;
        e = p;
        while (*e != '\0' && *e != ' ') e++;
        n = (uint16_t)(e - p);
        copy_clip(msg->text, sizeof(msg->text), p, n);
        return true;
    }

    /* ---- 第一个 token：命令名 / 事件名 ---- */
    e = p;
    while (*e != '\0' && *e != ' ') e++;
    n = (uint16_t)(e - p);
    if (n == 0) {
        return false;
    }
    copy_clip(msg->verb, sizeof(msg->verb), p, n);
    p = e;

    /* ---- 其余 key=value ---- */
    while (*p != '\0' && msg->kv_count < ESP_MSG_MAX_KV) {
        char *eq;

        while (*p == ' ') p++;
        if (*p == '\0') break;

        e = p;
        while (*e != '\0' && *e != ' ') e++;

        eq = (char *)memchr(p, '=', (size_t)(e - p));
        if (eq != NULL && eq > p) {
            uint16_t klen = (uint16_t)(eq - p);
            uint16_t vlen = (uint16_t)(e - eq - 1);

            copy_clip(msg->kv[msg->kv_count].key, ESP_KV_KEY_LEN, p, klen);
            unescape(eq + 1, vlen, msg->kv[msg->kv_count].val, ESP_KV_VAL_LEN);
            msg->kv_count++;
        }
        p = e;
    }
    return true;
}

bool Esp_LinkPoll(esp_msg_t *msg)
{
    for (;;) {
        uint16_t head = s_head;
        uint8_t  b;

        if (s_tail == head) {
            return false;
        }
        b      = s_rx[s_tail];
        s_tail = (uint16_t)((s_tail + 1u) % ESP_LINK_RX_BUF);

        if (b == '\n') {
            uint16_t len = s_line_len;
            s_line_len   = 0;
            if (len == 0) {
                continue;                 /* 空行忽略 */
            }
            s_line[len] = '\0';
            if (parse_line(s_line, msg)) {
                s_stats.lines_ok++;
                return true;
            }
            s_stats.lines_dropped++;      /* 不是我们的行：丢掉，靠行首重新同步 */
        } else if (b != '\r') {
            if (s_line_len < (ESP_LINK_LINE_MAX - 1u)) {
                s_line[s_line_len++] = (char)b;
            } else {
                s_line_len = 0;
                s_stats.overflow++;
            }
        }
    }
}

/* --------------------------------------------------------------- 取值 ---- */
const char *Esp_KvGet(const esp_msg_t *msg, const char *key)
{
    uint8_t i;
    if (msg == NULL || key == NULL) return NULL;
    for (i = 0; i < msg->kv_count; i++) {
        if (strcmp(msg->kv[i].key, key) == 0) {
            return msg->kv[i].val;
        }
    }
    return NULL;
}

bool Esp_KvGetStr(const esp_msg_t *msg, const char *key, char *out, uint16_t out_len)
{
    const char *v = Esp_KvGet(msg, key);
    if (v == NULL || out == NULL || out_len == 0) return false;
    copy_clip(out, out_len, v, (uint16_t)strlen(v));
    return true;
}

int32_t Esp_KvGetInt(const esp_msg_t *msg, const char *key, int32_t def)
{
    const char *v = Esp_KvGet(msg, key);
    if (v == NULL) return def;
    return (int32_t)strtol(v, NULL, 10);
}
