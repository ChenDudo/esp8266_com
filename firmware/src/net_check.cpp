#include "net_check.h"
#include <ESP8266WiFi.h>

/* 探测目标：都是纯 IP，不依赖 DNS。任一个能连上就算"有外网"。
 * 说明：TCP 连 53 端口在绝大多数家庭路由器/NAT 上都放行，比 HTTP 请求更轻。*/
struct ProbeTarget {
    const char* ip;
    uint16_t    port;
    const char* name;
};

static const ProbeTarget kTargets[] = {
    { "223.5.5.5",    53, "alidns"      },
    { "119.29.29.29", 53, "dnspod"      },
    { "180.76.76.76", 53, "baidudns"    },
    { "1.1.1.1",      80, "cloudflare"  },
};
static const uint8_t kTargetCount = (uint8_t)(sizeof(kTargets) / sizeof(kTargets[0]));

void NetCheck::begin()
{
    _detail  = "init";
    _enabled = true;
}

bool NetCheck::probe(uint32_t budgetMs)
{
    if (!_enabled) {
        _detail = "disabled";
        return true;
    }
    if (WiFi.status() != WL_CONNECTED) {
        _detail = "no_link";
        return false;
    }

    uint32_t t0  = millis();
    uint32_t per = budgetMs / kTargetCount;
    if (per < 400) per = 400;

    for (uint8_t i = 0; i < kTargetCount; i++) {
        if ((uint32_t)(millis() - t0) >= budgetMs) break;

        IPAddress ip;
        if (!ip.fromString(kTargets[i].ip)) continue;

        WiFiClient c;
        bool ok = (c.connect(ip, kTargets[i].port, (int32_t)per) == 1);
        c.stop();
        if (ok) {
            _detail = kTargets[i].name;
            return true;
        }
    }

    _detail = "unreachable";
    return false;
}
