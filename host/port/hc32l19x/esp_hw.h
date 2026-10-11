#ifndef ESP_HW_H
#define ESP_HW_H
/* ============================================================================
 * esp_hw.h —— HC32L196（HC32L19x DDL）的硬件移植层入口
 * ==========================================================================*/

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 UART1(PA02/PA03) + SysTick(1ms) + RST 引脚，并绑定移植层。
 * baud 传 0 表示用默认 115200。调用后即可 Esp_Init()。
 *
 * 需要包含的头文件路径（与你现有工程一致）：
 *   driver/inc, mcu/common, host, host/port/hc32l19x
 * 需要一起编译的源文件：
 *   driver/src/{ddl,uart,gpio,sysctrl}.c, mcu/common/{system_hc32l19x,interrupts_hc32l19x}.c
 */
void Esp_HwInit(uint32_t baud);

#ifdef __cplusplus
}
#endif

#endif /* ESP_HW_H */
