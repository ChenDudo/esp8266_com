#include "host_link.h"

void HostLink::begin(uint32_t baud)
{
    _len       = 0;
    _rxLines   = 0;
    _dropped   = 0;
    _handler   = NULL;
    _lastCmdMs = millis();

    Serial.begin(baud);
    /* 必须关掉，否则 SDK 的调试输出会插进协议流里 */
    Serial.setDebugOutput(false);
    delay(30);
    while (Serial.available()) {
        Serial.read();              /* 丢掉上电噪声 */
    }
}

String HostLink::val(const String& v)
{
    String out;
    out.reserve(v.length() + 4);
    for (size_t i = 0; i < v.length(); i++) {
        char c = v[i];
        if (c == ' ')       out += F("%20");
        else if (c == '=')  out += F("%3D");
        else if (c == '%')  out += F("%25");
        else if (c == '\r' || c == '\n') continue;
        else                out += c;
    }
    return out;
}

String HostLink::unval(const String& v)
{
    String out;
    out.reserve(v.length());
    for (size_t i = 0; i < v.length(); i++) {
        if (v[i] == '%' && (i + 2) < v.length()) {
            char h1 = v[i + 1];
            char h2 = v[i + 2];
            int a = (h1 >= '0' && h1 <= '9') ? (h1 - '0') : ((h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) : -1);
            int b = (h2 >= '0' && h2 <= '9') ? (h2 - '0') : ((h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) : -1);
            if (a >= 0 && b >= 0) {
                out += (char)((a << 4) | b);
                i += 2;
                continue;
            }
        }
        out += v[i];
    }
    return out;
}

void HostLink::raw(const String& line)
{
    Serial.print(line);
    Serial.print(F("\r\n"));
}

void HostLink::ok(const char* verb)
{
    raw(String(F("+OK ")) + verb);
}

void HostLink::ok(const char* verb, const char* k, const String& v)
{
    raw(String(F("+OK ")) + verb + " " + k + "=" + val(v));
}

void HostLink::err(int32_t code, const char* text)
{
    raw(String(F("-ERR ")) + String((long)code) + " " + text);
}

void HostLink::eventLine(const String& name, const String& kvs)
{
    String s = F("@EVT ");
    s += name;
    if (kvs.length() > 0) {
        s += ' ';
        s += kvs;
    }
    raw(s);
}

void HostLink::loop()
{
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n') {
            if (_len > 0) {
                _buf[_len] = '\0';
                handleLine(_buf);
            }
            _len = 0;
        } else if (c != '\r') {
            if (_len < sizeof(_buf) - 1) {
                _buf[_len++] = (char)c;
            } else {
                _len = 0;           /* 超长行：丢掉并重新同步 */
                _dropped++;
            }
        }
    }
}

void HostLink::handleLine(char* line)
{
    uint16_t i = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] != '#') return;     /* 只认 MCU 发来的命令，其余（噪声）忽略 */
    i++;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] == '\0') return;

    char* verb = &line[i];
    char* sp   = strchr(verb, ' ');
    const char* args = "";
    if (sp != NULL) {
        *sp  = '\0';
        args = sp + 1;
    }

    _rxLines++;
    _lastCmdMs = millis();

    if (_handler != NULL && _handler(verb, args)) {
        return;
    }
    err(1, "unknown_cmd");
}

bool HostLink::argGet(const char* args, const char* key, char* out, size_t outLen)
{
    if (args == NULL || key == NULL || out == NULL || outLen == 0) return false;

    size_t klen = strlen(key);
    const char* p = args;
    while (*p != '\0') {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        const char* end = strchr(p, ' ');
        if (end == NULL) end = p + strlen(p);
        const char* eq = (const char*)memchr(p, '=', (size_t)(end - p));

        if (eq != NULL && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = (size_t)(end - eq - 1);
            if (vlen >= outLen) vlen = outLen - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = end;
    }
    return false;
}

int32_t HostLink::argGetInt(const char* args, const char* key, int32_t def)
{
    char tmp[24];
    if (!argGet(args, key, tmp, sizeof(tmp))) return def;
    return (int32_t)strtol(tmp, NULL, 0);
}
