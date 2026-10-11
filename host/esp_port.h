#ifndef ESP_PORT_H
#define ESP_PORT_H
/* ============================================================================
 * esp_port.h —— 移植层（唯一与具体 MCU 相关的接口）
 *
 * 换 MCU 只需要实现这 4 个函数并调用一次 Esp_PortBind()，其余代码原样可用。
 * 参考实现见 port/hc32l19x/esp_port_hc32l19x.c
 * ==========================================================================*/

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 向 ESP8266 发送一段字节（阻塞式即可，长度都很短） */
    void     (*uart_tx)(const uint8_t *data, uint16_t len);
    /* 单调递增的毫秒计数；比较一律用 (uint32_t)(a - b) 形式，回绕安全 */
    uint32_t (*tick_ms)(void);
    /* ESP8266 的 RST 脚：0 = 拉低（复位），1 = 释放（运行） */
    void     (*rst_write)(uint8_t level);
    /* 毫秒延时 */
    void     (*delay_ms)(uint32_t ms);
} esp_port_t;

/* 应用初始化时调用一次（一般由 Esp_HwInit() 内部完成） */
void Esp_PortBind(const esp_port_t *port);
/* 取当前绑定的移植层；未绑定时返回 NULL */
const esp_port_t *Esp_Port(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_PORT_H */
