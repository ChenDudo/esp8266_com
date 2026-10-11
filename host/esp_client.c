#include "esp_client.h"
#include "esp_port.h"
#include <string.h>
#include <stdlib.h>

/* ---------------- 可调参数（可用 -D 覆盖） ---------------- */
#ifndef ESP_RST_LOW_MS
#define ESP_RST_LOW_MS        200u     /* RST 拉低时长 */
#endif
#ifndef ESP_WAIT_READY_MS
#define ESP_WAIT_READY_MS     6000u    /* 等 READY 的超时 */
#endif
#ifndef ESP_MAX_RESET
#define ESP_MAX_RESET         3u       /* 最多复位几次就放弃 */
#endif
#ifndef ESP_LINK_TIMEOUT_MS
#define ESP_LINK_TIMEOUT_MS   20000u   /* 模块静默多久判死（固件心跳 5s 一发） */
#endif

static esp_status_t s_st;
static esp_cbs_t    s_cbs;
static bool         s_cbs_valid;
static uint32_t     s_t_reset;
static uint32_t     s_deadline;

/* ------------------------------------------------------------- 小工具 ---- */
static uint32_t tick(void)
{
    const esp_port_t *p = Esp_Port();
    return (p != NULL && p->tick_ms != NULL) ? p->tick_ms() : 0u;
}

static void delay_ms(uint32_t ms)
{
    const esp_port_t *p = Esp_Port();
    if (p != NULL && p->delay_ms != NULL) {
        p->delay_ms(ms);
    }
}

static void set_state(esp_state_t ns)
{
    esp_state_t os = s_st.state;

    if (os == ns) {
        return;
    }
    s_st.state = ns;
    if (s_cbs_valid && s_cbs.on_state_changed != NULL) {
        s_cbs.on_state_changed(os, ns, &s_st);
    }
}

const char *Esp_StateName(esp_state_t st)
{
    switch (st) {
    case ESP_ST_RESET:      return "RESET";
    case ESP_ST_WAIT_READY: return "WAIT_READY";
    case ESP_ST_OFFLINE:    return "OFFLINE";
    case ESP_ST_PROV:       return "PROV";
    case ESP_ST_ONLINE:     return "ONLINE";
    case ESP_ST_ERROR:      return "ERROR";
    default:                return "?";
    }
}

static void copy_str(char *dst, uint16_t dst_len, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, (size_t)(dst_len - 1u));
    dst[dst_len - 1u] = '\0';
}

/* ------------------------------------------------------------- 发送 ------ */
void Esp_SendLine(const char *line)
{
    uint8_t  buf[192];
    uint16_t n = 0;
    const esp_port_t *p = Esp_Port();

    if (line == NULL || p == NULL || p->uart_tx == NULL) {
        return;
    }
    while (line[n] != '\0' && n < (uint16_t)(sizeof(buf) - 3u)) {
        buf[n] = (uint8_t)line[n];
        n++;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    p->uart_tx(buf, n);
}

/* 值里的空格 / '=' / '%' 转义，保证空格分词不出错 */
static uint16_t esc_to(char *out, uint16_t out_len, const char *in)
{
    uint16_t o = 0;
    while (*in != '\0' && (uint16_t)(o + 4u) < out_len) {
        char c = *in++;
        if (c == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else if (c == '=') {
            out[o++] = '%'; out[o++] = '3'; out[o++] = 'D';
        } else if (c == '%') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '5';
        } else if (c == '\r' || c == '\n') {
            /* 丢掉 */
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------- 分发 ------ */
static void handle_msg(esp_msg_t *m)
{
    s_st.last_msg_ms = tick();

    if (m->kind == ESP_MSG_OK) {
        /* #WIFI.STATUS? 的应答：顺手刷新一次状态（其余应答这里不关心） */
        if (strcmp(m->verb, "WIFI.STATUS") == 0) {
            const char *st   = Esp_KvGet(m, "state");
            const char *ip   = Esp_KvGet(m, "ip");
            const char *ssid = Esp_KvGet(m, "ssid");

            s_st.rssi        = (int16_t)Esp_KvGetInt(m, "rssi", (int32_t)s_st.rssi);
            s_st.degraded    = (uint8_t)Esp_KvGetInt(m, "degraded", (int32_t)s_st.degraded);
            s_st.prov_active = (uint8_t)Esp_KvGetInt(m, "prov", (int32_t)s_st.prov_active);
            if (ip != NULL && strcmp(ip, "-") != 0) {
                copy_str(s_st.ip, sizeof(s_st.ip), ip);
            }
            if (ssid != NULL && strcmp(ssid, "-") != 0) {
                copy_str(s_st.ssid, sizeof(s_st.ssid), ssid);
            }
            if (st != NULL) {
                esp_state_t ns = s_st.state;
                if (strcmp(st, "ONLINE") == 0) {
                    ns = ESP_ST_ONLINE;
                } else if (strcmp(st, "PROV") == 0) {
                    ns = ESP_ST_PROV;
                } else if (strcmp(st, "CONNECTING") == 0 || strcmp(st, "INIT") == 0) {
                    ns = ESP_ST_OFFLINE;
                }
                if (ns != s_st.state) {
                    if (ns == ESP_ST_ONLINE) {
                        s_st.online_since_ms = tick();
                    }
                    set_state(ns);
                }
            }
        }
        return;
    }

    if (m->kind == ESP_MSG_ERR) {
        if (s_cbs_valid && s_cbs.on_error != NULL) {
            s_cbs.on_error(m->code, m->text);
        }
        return;
    }

    /* ---- 事件 ---- */
    if (strcmp(m->verb, "READY") == 0) {
        copy_str(s_st.fw, sizeof(s_st.fw), Esp_KvGet(m, "ver"));
        s_st.proto       = Esp_KvGetInt(m, "proto", 0);
        s_st.reset_count = 0;
        set_state(ESP_ST_OFFLINE);
        if (s_cbs_valid && s_cbs.on_ready != NULL) {
            s_cbs.on_ready(s_st.fw, s_st.proto);
        }
        Esp_SendLine("#HELLO proto=1");
        return;
    }

    if (strcmp(m->verb, "HB") == 0) {
        const char *ip = Esp_KvGet(m, "ip");
        s_st.rssi     = (int16_t)Esp_KvGetInt(m, "rssi", 0);
        s_st.uptime_s = (uint32_t)Esp_KvGetInt(m, "up", 0);
        if (ip != NULL && strcmp(ip, "-") != 0) {
            copy_str(s_st.ip, sizeof(s_st.ip), ip);
        }
        return;
    }

    if (strcmp(m->verb, "WIFI_CONNECTING") == 0) {
        copy_str(s_st.ssid, sizeof(s_st.ssid), Esp_KvGet(m, "ssid"));
        s_st.prov_active = 0;
        set_state(ESP_ST_OFFLINE);
        return;
    }

    if (strcmp(m->verb, "GOT_IP") == 0) {
        const char *ip = Esp_KvGet(m, "ip");
        s_st.rssi        = (int16_t)Esp_KvGetInt(m, "rssi", 0);
        s_st.degraded    = 0;
        s_st.prov_active = 0;
        if (ip != NULL) {
            copy_str(s_st.ip, sizeof(s_st.ip), ip);
        }
        if (s_st.state != ESP_ST_ONLINE) {
            s_st.online_since_ms = tick();
            set_state(ESP_ST_ONLINE);
        }
        if (s_cbs_valid && s_cbs.on_got_ip != NULL) {
            s_cbs.on_got_ip(s_st.ip, s_st.rssi);
        }
        return;
    }

    if (strcmp(m->verb, "PROV_START") == 0) {
        copy_str(s_st.ap_ssid, sizeof(s_st.ap_ssid), Esp_KvGet(m, "ap"));
        copy_str(s_st.ap_pwd, sizeof(s_st.ap_pwd), Esp_KvGet(m, "pwd"));
        s_st.prov_active = 1;
        set_state(ESP_ST_PROV);
        if (s_cbs_valid && s_cbs.on_prov_start != NULL) {
            s_cbs.on_prov_start(s_st.ap_ssid, s_st.ap_pwd);
        }
        return;
    }

    if (strcmp(m->verb, "PROV_DONE") == 0) {
        const char *ip = Esp_KvGet(m, "ip");
        copy_str(s_st.ssid, sizeof(s_st.ssid), Esp_KvGet(m, "ssid"));
        s_st.prov_active = 0;
        if (ip != NULL) {
            copy_str(s_st.ip, sizeof(s_st.ip), ip);
        }
        if (s_st.state != ESP_ST_ONLINE) {
            s_st.online_since_ms = tick();
            set_state(ESP_ST_ONLINE);
        }
        if (s_cbs_valid && s_cbs.on_prov_done != NULL) {
            s_cbs.on_prov_done(s_st.ssid, s_st.ip);
        }
        return;
    }

    if (strcmp(m->verb, "PROV_FAIL") == 0) {
        copy_str(s_st.fail_msg, sizeof(s_st.fail_msg), Esp_KvGet(m, "msg"));
        if (s_cbs_valid && s_cbs.on_fail != NULL) {
            s_cbs.on_fail(s_st.fail_msg);
        }
        return;
    }

    if (strcmp(m->verb, "PROV_TIMEOUT") == 0) {
        if (s_cbs_valid && s_cbs.on_timeout != NULL) {
            s_cbs.on_timeout();
        }
        return;
    }

    if (strcmp(m->verb, "WIFI_LOST") == 0) {
        set_state(ESP_ST_OFFLINE);
        if (s_cbs_valid && s_cbs.on_wifi_lost != NULL) {
            s_cbs.on_wifi_lost();
        }
        return;
    }
    /* 其它未知事件忽略（向前兼容） */
}

/* ------------------------------------------------------------- API ------ */
void Esp_Init(const esp_cbs_t *cbs)
{
    if (cbs != NULL) {
        s_cbs       = *cbs;
        s_cbs_valid = true;
    } else {
        s_cbs_valid = false;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.state = ESP_ST_ERROR;      /* 先置成"不可能"，让硬复位一定触发状态变化 */
    Esp_LinkInit();
    Esp_HardReset();
}

void Esp_HardReset(void)
{
    const esp_port_t *p = Esp_Port();

    Esp_LinkReset();
    s_st.prov_active     = 0;
    s_st.degraded        = 0;
    s_st.online_since_ms = 0;
    s_st.ip[0]           = '\0';

    if (p != NULL && p->rst_write != NULL) {
        p->rst_write(0);            /* 拉低复位 */
    }
    s_t_reset = tick();
    set_state(ESP_ST_RESET);
}

void Esp_Poll(void)
{
    esp_msg_t m;
    uint32_t  t = tick();

    while (Esp_LinkPoll(&m)) {
        handle_msg(&m);
    }

    switch (s_st.state) {
    case ESP_ST_RESET:
        if ((uint32_t)(t - s_t_reset) >= ESP_RST_LOW_MS) {
            const esp_port_t *p = Esp_Port();
            if (p != NULL && p->rst_write != NULL) {
                p->rst_write(1);    /* 释放复位 */
            }
            s_st.reset_count++;
            s_deadline = t + ESP_WAIT_READY_MS;
            set_state(ESP_ST_WAIT_READY);
        }
        break;

    case ESP_ST_WAIT_READY:
        if ((int32_t)(t - s_deadline) >= 0) {
            if (s_st.reset_count >= ESP_MAX_RESET) {
                set_state(ESP_ST_ERROR);   /* 反复不响应：上层可以点灯报警 */
            } else {
                Esp_HardReset();
            }
        }
        break;

    case ESP_ST_OFFLINE:
    case ESP_ST_PROV:
    case ESP_ST_ONLINE:
        /* 模块固件每 5s 发一次 @EVT HB，长时间没消息说明它挂了 */
        if ((uint32_t)(t - s_st.last_msg_ms) > ESP_LINK_TIMEOUT_MS) {
            Esp_HardReset();
        }
        break;

    case ESP_ST_ERROR:
    default:
        break;
    }
}

bool Esp_WaitReady(uint32_t timeout_ms)
{
    uint32_t t0 = tick();

    while ((uint32_t)(tick() - t0) < timeout_ms) {
        Esp_Poll();
        if (s_st.state == ESP_ST_OFFLINE ||
            s_st.state == ESP_ST_ONLINE ||
            s_st.state == ESP_ST_PROV) {
            return true;
        }
        delay_ms(1);
    }
    return false;
}

bool Esp_Ping(uint32_t timeout_ms)
{
    uint32_t t0 = tick();

    Esp_SendLine("#PING");
    while ((uint32_t)(tick() - t0) < timeout_ms) {
        esp_msg_t m;
        while (Esp_LinkPoll(&m)) {
            if (m.kind == ESP_MSG_OK && strcmp(m.verb, "PING") == 0) {
                return true;
            }
            handle_msg(&m);
        }
        delay_ms(1);
    }
    return false;
}

void Esp_PingAsync(void)
{
    Esp_SendLine("#PING");
}

void Esp_Reprov(void)
{
    Esp_SendLine("#WIFI.REPROV");
}

void Esp_Connect(const char *ssid, const char *pass)
{
    char line[128];
    char es[70];
    char ep[70];

    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    esc_to(es, sizeof(es), ssid);
    esc_to(ep, sizeof(ep), (pass != NULL) ? pass : "");

    strcpy(line, "#WIFI.CONN ssid=");
    strncat(line, es, sizeof(line) - strlen(line) - 1u);
    strncat(line, " pass=", sizeof(line) - strlen(line) - 1u);
    strncat(line, ep, sizeof(line) - strlen(line) - 1u);
    Esp_SendLine(line);
}

void Esp_QueryStatus(void)
{
    Esp_SendLine("#WIFI.STATUS?");
}

const esp_status_t *Esp_Status(void)
{
    return &s_st;
}
