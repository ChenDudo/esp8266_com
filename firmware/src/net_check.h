#pragma once
/* ============================================================================
 * net_check.h —— 外网可达性探测（联网判定的第 3 层，见设计文档 2.2）
 *
 * 只做"能连上就算通"的 TCP 连接测试，不走 DNS，所以路由器没外网时能快速失败。
 * 注意：这个函数是阻塞的，预算内最多阻塞 budgetMs 毫秒，只在 ONLINE 状态下调用。
 * ==========================================================================*/

#include <Arduino.h>
#include "app_cfg.h"

class NetCheck {
public:
    void begin();

    /* 返回 true = 外网可达。detail() 里是命中的目标或失败原因 */
    bool probe(uint32_t budgetMs = CFG_PROBE_TIMEOUT_MS);

    const char* detail() const { return _detail; }

    void setEnabled(bool en) { _enabled = en; }
    bool enabled() const     { return _enabled; }

private:
    const char* _detail;
    bool        _enabled;
};
