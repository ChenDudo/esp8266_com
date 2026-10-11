#pragma once
/* ============================================================================
 * host_link.h —— 与 MCU 的串口行协议（ESP 侧）
 *
 * 线格式（全部 ASCII，'\r\n' 结束，值与值之间用空格）：
 *     MCU → ESP   # <VERB> [key=value ...]
 *     ESP → MCU   +OK <VERB> [key=value ...]
 *                 -ERR <code> <text>
 *                 @EVT <NAME> [key=value ...]
 *
 * 值里出现的空格 / '=' 会被转义成 %20 / %3D，所以值本身不会破坏分词。
 * 详见 docs/uart_protocol.md。
 * ==========================================================================*/

#include <Arduino.h>
#include "app_cfg.h"

typedef bool (*HostCmdHandler)(const char* verb, const char* args);

class HostLink {
public:
    void begin(uint32_t baud = CFG_HOST_BAUD);
    void loop();

    void setHandler(HostCmdHandler h) { _handler = h; }

    /* ---- 回复 ---- */
    void raw(const String& line);
    void ok(const char* verb);
    void ok(const char* verb, const char* k, const String& v);
    void err(int32_t code, const char* text);

    /* ---- 事件 ---- */
    void eventLine(const String& name, const String& kvs);
    void event(const char* name) { eventLine(String(name), String()); }
    void event1(const char* n, const char* k1, const String& v1)
        { eventLine(String(n), String(k1) + "=" + val(v1)); }
    void event2(const char* n, const char* k1, const String& v1, const char* k2, const String& v2)
        { eventLine(String(n), String(k1) + "=" + val(v1) + " " + String(k2) + "=" + val(v2)); }
    void event3(const char* n, const char* k1, const String& v1, const char* k2, const String& v2,
                const char* k3, const String& v3)
        { eventLine(String(n), String(k1) + "=" + val(v1) + " " + String(k2) + "=" + val(v2) +
                     " " + String(k3) + "=" + val(v3)); }

    /* ---- 参数解析（给命令处理用） ---- */
    static bool    argGet(const char* args, const char* key, char* out, size_t outLen);
    static int32_t argGetInt(const char* args, const char* key, int32_t def);
    static String  val(const String& v);          /* 转义空格与 '=' */
    static String  unval(const String& v);        /* 反向还原 */

    uint32_t rxLines() const    { return _rxLines; }
    uint32_t droppedLines() const { return _dropped; }
    uint32_t lastCmdMs() const  { return _lastCmdMs; }

private:
    char           _buf[160];
    uint16_t       _len;
    uint32_t       _rxLines;
    uint32_t       _dropped;
    uint32_t       _lastCmdMs;
    HostCmdHandler _handler;

    void handleLine(char* line);
};
