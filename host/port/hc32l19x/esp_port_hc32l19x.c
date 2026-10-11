/* ============================================================================
 * esp_port_hc32l19x.c —— HC32L196 移植层
 *
 * 提供的四件事：串口发送、毫秒 tick、RST 脚控制、毫秒延时。
 * 接收走中断：Uart1_IRQHandler() 把字节喂给 Esp_LinkFeedByte()。
 * （DDL 的 interrupts_hc32l19x.c 里 UART1_3_IRQHandler() 会调用 Uart1_IRQHandler()）
 *
 * 注意：stc_gpio_cfg_t 必须显式给 enCtrlMode 赋值（GpioAHB）。
 *       DDL_ZERO_STRUCT 会把 enCtrlMode 清成 0，而 0 是 GpioFastIO，
 *       Gpio_Init 会把它写进 CTRL2.AHB_SEL —— 那样端口就不走 AHB 寄存器了。
 * ==========================================================================*/

#include "ddl.h"
#include "uart.h"
#include "gpio.h"
#include "sysctrl.h"
#include "esp_port.h"
#include "esp_link.h"
#include "esp_hw.h"

/* ------------------------- 硬件配置：按你的板子改 ------------------------ */
#define ESP_UART            M0P_UART1
#define ESP_UART_NVIC       UART1_3_IRQn
#define ESP_TX_PORT         GpioPortA
#define ESP_TX_PIN          GpioPin2
#define ESP_RX_PORT         GpioPortA
#define ESP_RX_PIN          GpioPin3
#define ESP_RST_PORT        GpioPortB
#define ESP_RST_PIN         GpioPin0
#define ESP_BAUD_DEFAULT    115200u
#define ESP_TX_POLL_TIMEOUT 20000u   /* Uart_SendDataPollTimeOut 的计数上限 */

static volatile uint32_t s_tick_ms;

/* --------------------------------------------------------- SysTick 1ms --- */
/* 注意：如果你的工程已经用 SysTick 做别的事（比如 RTOS 节拍），
 *       把 tick_ms() 改成你自己的毫秒计数即可，这个中断函数删掉。 */
void SysTick_IRQHandler(void)
{
    s_tick_ms++;
}

static uint32_t port_tick_ms(void)
{
    return s_tick_ms;
}

static void port_delay_ms(uint32_t ms)
{
    uint32_t t0 = s_tick_ms;
    while ((uint32_t)(s_tick_ms - t0) < ms) {
        /* 忙等：只在初始化阶段用，时间都很短 */
    }
}

static void port_rst_write(uint8_t level)
{
    if (level != 0u) {
        Gpio_SetIO(ESP_RST_PORT, ESP_RST_PIN);
    } else {
        Gpio_ClrIO(ESP_RST_PORT, ESP_RST_PIN);
    }
}

static void port_uart_tx(const uint8_t *data, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        (void)Uart_SendDataPollTimeOut(ESP_UART, data[i], ESP_TX_POLL_TIMEOUT);
    }
}

static const esp_port_t s_port = {
    port_uart_tx,
    port_tick_ms,
    port_rst_write,
    port_delay_ms
};

/* ------------------------------------------------------------ UART 中断 --- */
void Uart1_IRQHandler(void)
{
    if (Uart_GetStatus(ESP_UART, UartRC)) {
        Uart_ClrStatus(ESP_UART, UartRC);
        Esp_LinkFeedByte(Uart_ReceiveData(ESP_UART));
    }
    if (Uart_GetStatus(ESP_UART, UartFE)) {
        Uart_ClrStatus(ESP_UART, UartFE);      /* 帧错误：清掉，避免中断反复触发 */
    }
}

/* ------------------------------------------------------------- 初始化 --- */
void Esp_HwInit(uint32_t baud)
{
    stc_gpio_cfg_t gpioCfg;
    stc_uart_cfg_t uartCfg;

    Sysctrl_SetPeripheralGate(SysctrlPeripheralGpio, TRUE);
    Sysctrl_SetPeripheralGate(SysctrlPeripheralUart1, TRUE);

    /* RST：默认释放（高电平），这样 MCU 复位期间 ESP 也能自己跑（建议 RST 上再加 10k 上拉） */
    DDL_ZERO_STRUCT(gpioCfg);
    gpioCfg.enDir       = GpioDirOut;
    gpioCfg.bOutputVal  = TRUE;
    gpioCfg.enDrv       = GpioDrvH;
    gpioCfg.enPu        = GpioPuDisable;
    gpioCfg.enPd        = GpioPdDisable;
    gpioCfg.enOD        = GpioOdDisable;
    gpioCfg.enCtrlMode  = GpioAHB;
    Gpio_Init(ESP_RST_PORT, ESP_RST_PIN, &gpioCfg);

    /* UART1 TX */
    DDL_ZERO_STRUCT(gpioCfg);
    gpioCfg.enDir       = GpioDirOut;
    gpioCfg.bOutputVal  = TRUE;
    gpioCfg.enCtrlMode  = GpioAHB;
    Gpio_Init(ESP_TX_PORT, ESP_TX_PIN, &gpioCfg);
    Gpio_SetAfMode(ESP_TX_PORT, ESP_TX_PIN, GpioAf1);

    /* UART1 RX */
    DDL_ZERO_STRUCT(gpioCfg);
    gpioCfg.enDir       = GpioDirIn;
    gpioCfg.enCtrlMode  = GpioAHB;
    Gpio_Init(ESP_RX_PORT, ESP_RX_PIN, &gpioCfg);
    Gpio_SetAfMode(ESP_RX_PORT, ESP_RX_PIN, GpioAf1);

    /* UART1：8N1，模式3。
     * 无校验就是 enMmdorCk = UartMskDataOrAddr（= 0，即 SCON 里不置校验位）；
     * 注意 ESP8266 是 8N1，别照抄官方 uart_int 例程里的 UartMskEven（偶校验）。 */
    DDL_ZERO_STRUCT(uartCfg);
    uartCfg.enRunMode       = UartMskMode3;
    uartCfg.enStopBit       = UartMsk1bit;
    uartCfg.enMmdorCk       = UartMskDataOrAddr;
    uartCfg.stcBaud.u32Baud = (baud != 0u) ? baud : ESP_BAUD_DEFAULT;
    uartCfg.stcBaud.enClkDiv = UartMsk8Or16Div;
    uartCfg.stcBaud.u32Pclk  = Sysctrl_GetPClkFreq();
    (void)Uart_Init(ESP_UART, &uartCfg);

    Uart_ClrStatus(ESP_UART, UartRC);
    Uart_ClrStatus(ESP_UART, UartTC);
    (void)Uart_EnableIrq(ESP_UART, UartRxIrq);
    EnableNvic(ESP_UART_NVIC, IrqLevel3, TRUE);

    /* 1ms 系统节拍，给协议层做超时用 */
    (void)SysTick_Config(SystemCoreClock / 1000u);

    Esp_PortBind(&s_port);
}
