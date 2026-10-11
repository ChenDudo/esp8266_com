/* ============================================================================
 * main.c —— HC32L196 + ESP8266 联网模块 示例工程（Keil MDK）
 *
 * 这个工程演示 MCU 侧怎么用根目录 host/ 里的驱动：
 *   1) Esp_HwInit()  配好 UART1(PA02/PA03) + 1ms 节拍 + RST 脚
 *   2) Esp_Init()    拉低 RST 复位模块，等它发 @EVT READY 完成握手
 *   3) 主循环 Esp_Poll() 驱动协议；业务只读 Esp_Status()
 *
 * 状态灯约定（官方 STK 板 LED = PD14，SetIO 点亮）：
 *   常亮          已联网 ONLINE
 *   慢闪 1s       配网模式 PROV —— 让用户去连 st->ap_ssid 这个热点
 *   快闪 200ms    正在连路由器 / 掉线重连 OFFLINE
 *   超快闪 100ms  正在复位模块 / 等握手 RESET、WAIT_READY
 *   熄灭          模块一直不响应 ERROR
 *
 * 上电按住 USER 键(PA7) 3 秒 = 清除 WiFi 记录并重新配网。
 *
 * 第一次上电没有 WiFi 记录时，模块会自己开热点，灯是慢闪；
 * 手机连上热点后打开 192.168.4.1 填 WiFi 密码，成功后灯变常亮。
 * ==========================================================================*/

#include "ddl.h"
#include "gpio.h"
#include "sysctrl.h"
#include "uart.h"
#include "esp_hw.h"
#include "esp_client.h"

/* ------------------------------ 用户配置 -------------------------------- */
/* 板载 LED / 按键：官方 STK 板是 PD14 / PA7（定义在 board_stkhc32l19x.h）。
 * 你自己的板子不一样就改这两组宏。 */
#define APP_LED_PORT        STK_LED_PORT
#define APP_LED_PIN         STK_LED_PIN
#define APP_KEY_PORT        STK_USER_PORT
#define APP_KEY_PIN         STK_USER_PIN
#define APP_KEY_HOLD_MS     3000u

/* 可选调试串口：默认关闭。
 * UART0 的复用在 L196 上是 PA08(AF1) / PA09(AF1) / PA14(AF2)，
 * 具体你板上引的是哪只脚，看原理图确认后再打开。
 * 打开后 app_log() 的内容会从串口吐出来（115200 8N1）。 */
#ifndef APP_DEBUG_UART
#define APP_DEBUG_UART      0        /* 改成 1（或编译时 -DAPP_DEBUG_UART=1）打开 */
#endif
#define APP_DBG_UART        M0P_UART0
#define APP_DBG_TX_PORT     GpioPortA
#define APP_DBG_TX_PIN      GpioPin9
#define APP_DBG_TX_AF       GpioAf1
#define APP_DBG_BAUD        115200u

/* ------------------------------ 内部变量 -------------------------------- */
/* 最近一条事件文本：不想接调试串口时，在 Keil 里把这几个变量加进 watch 就能看到。*/
static char    s_last_event[48];
static uint8_t s_key_reprov;      /* 上电按键触发"重新配网" */

/* ------------------------------ 小工具 ---------------------------------- */
void app_log(const char *s)
{
    uint16_t i = 0;

    while (s[i] != '\0' && i < (uint16_t)(sizeof(s_last_event) - 1u)) {
        s_last_event[i] = s[i];
        i++;
    }
    s_last_event[i] = '\0';

#if APP_DEBUG_UART
    for (i = 0; s[i] != '\0'; i++) {
        (void)Uart_SendDataPollTimeOut(APP_DBG_UART, (uint8_t)s[i], 20000u);
    }
    (void)Uart_SendDataPollTimeOut(APP_DBG_UART, (uint8_t)'\r', 20000u);
    (void)Uart_SendDataPollTimeOut(APP_DBG_UART, (uint8_t)'\n', 20000u);
#else
    (void)i;
#endif
}

/* ------------------------------ 板级初始化 ------------------------------ */
static void app_led(uint8_t on)
{
    if (on != 0u) {
        (void)Gpio_SetIO(APP_LED_PORT, APP_LED_PIN);     /* STK 板：SetIO = 点亮 */
    } else {
        (void)Gpio_ClrIO(APP_LED_PORT, APP_LED_PIN);
    }
}

static uint8_t app_key_down(void)
{
    return (Gpio_GetInputIO(APP_KEY_PORT, APP_KEY_PIN) == FALSE) ? 1u : 0u;
}

static void app_gpio_init(void)
{
    stc_gpio_cfg_t cfg;

    Sysctrl_SetPeripheralGate(SysctrlPeripheralGpio, TRUE);

    /* LED：输出，上电先灭 */
    DDL_ZERO_STRUCT(cfg);
    cfg.enDir       = GpioDirOut;
    cfg.bOutputVal  = FALSE;
    cfg.enDrv       = GpioDrvH;
    cfg.enPu        = GpioPuDisable;
    cfg.enPd        = GpioPdDisable;
    cfg.enOD        = GpioOdDisable;
    /* 必须显式写 AHB：DDL_ZERO_STRUCT 之后是 GpioFastIO(0)，
     * 那会让端口不走 AHB 寄存器，读回/输出都不对 */
    cfg.enCtrlMode  = GpioAHB;
    app_led(0);
    (void)Gpio_Init(APP_LED_PORT, APP_LED_PIN, &cfg);

    /* USER 按键：输入 + 内部上拉（板上已有外部上拉也无妨），按下为低 */
    DDL_ZERO_STRUCT(cfg);
    cfg.enDir       = GpioDirIn;
    cfg.enDrv       = GpioDrvL;
    cfg.enPu        = GpioPuEnable;
    cfg.enPd        = GpioPdDisable;
    cfg.enOD        = GpioOdDisable;
    cfg.enCtrlMode  = GpioAHB;
    (void)Gpio_Init(APP_KEY_PORT, APP_KEY_PIN, &cfg);
}

#if APP_DEBUG_UART
static void app_dbg_init(void)
{
    stc_gpio_cfg_t gpioCfg;
    stc_uart_cfg_t uartCfg;

    Sysctrl_SetPeripheralGate(SysctrlPeripheralUart0, TRUE);

    DDL_ZERO_STRUCT(gpioCfg);
    gpioCfg.enDir      = GpioDirOut;
    gpioCfg.bOutputVal = TRUE;
    gpioCfg.enDrv      = GpioDrvH;
    gpioCfg.enCtrlMode = GpioAHB;
    (void)Gpio_Init(APP_DBG_TX_PORT, APP_DBG_TX_PIN, &gpioCfg);
    Gpio_SetAfMode(APP_DBG_TX_PORT, APP_DBG_TX_PIN, APP_DBG_TX_AF);

    DDL_ZERO_STRUCT(uartCfg);
    uartCfg.enRunMode        = UartMskMode3;
    uartCfg.enStopBit        = UartMsk1bit;
    uartCfg.enMmdorCk        = UartMskDataOrAddr;   /* 无校验 = 8N1 */
    uartCfg.stcBaud.u32Baud  = APP_DBG_BAUD;
    uartCfg.stcBaud.enClkDiv = UartMsk8Or16Div;
    uartCfg.stcBaud.u32Pclk  = Sysctrl_GetPClkFreq();
    (void)Uart_Init(APP_DBG_UART, &uartCfg);
}
#endif

/* ------------------------------ 状态灯 ---------------------------------- */
static void app_led_task(void)
{
    static uint32_t t_next;
    static uint8_t  on;
    const esp_port_t *port = Esp_Port();
    uint32_t now;
    uint32_t period = 1000u;

    if (port == NULL || port->tick_ms == NULL) {
        return;
    }
    now = port->tick_ms();

    switch (Esp_Status()->state) {
    case ESP_ST_ONLINE:
        app_led(1);
        return;                                     /* 常亮 */
    case ESP_ST_PROV:
        period = 1000u;                             /* 慢闪：等人来配网 */
        break;
    case ESP_ST_OFFLINE:
        period = 200u;                              /* 快闪：连接 / 重连 */
        break;
    case ESP_ST_RESET:
    case ESP_ST_WAIT_READY:
        period = 100u;                              /* 超快闪：复位 / 握手 */
        break;
    default:
        app_led(0);                                 /* ERROR：熄灭报警 */
        return;
    }

    if ((int32_t)(now - t_next) >= 0) {
        on      = (uint8_t)(on == 0u);
        t_next  = now + period;
        app_led(on);
    }
}

/* ------------------------------ 模块回调 -------------------------------- */
static void on_ready(const char *fw, int32_t proto)
{
    (void)fw;
    (void)proto;
    app_log("esp: ready");
}

static void on_state_changed(esp_state_t old_st, esp_state_t new_st, const esp_status_t *st)
{
    (void)old_st;
    (void)st;
    switch (new_st) {
    case ESP_ST_ONLINE:     app_log("esp: ONLINE");              break;
    case ESP_ST_PROV:       app_log("esp: PROV, join hotspot");  break;
    case ESP_ST_OFFLINE:    app_log("esp: connecting/offline");  break;
    case ESP_ST_ERROR:      app_log("esp: NO RESPONSE!");        break;
    case ESP_ST_RESET:      app_log("esp: reset");               break;
    case ESP_ST_WAIT_READY: app_log("esp: wait ready");          break;
    default:                                                     break;
    }
}

static void on_prov_start(const char *ap_ssid, const char *ap_pwd)
{
    /* 这里最适合接显屏 / 蜂鸣器：
     * "请连接热点 <ap_ssid>，密码 <ap_pwd>，浏览器打开 192.168.4.1" */
    (void)ap_ssid;
    (void)ap_pwd;
    app_log("esp: hotspot up");
}

static void on_prov_done(const char *ssid, const char *ip)
{
    (void)ssid;
    (void)ip;
    app_log("esp: prov done");
}

static void on_got_ip(const char *ip, int16_t rssi)
{
    (void)ip;
    (void)rssi;
    app_log("esp: got ip");
}

static void on_fail(const char *msg)
{
    app_log(msg);       /* msg 是模块给的中文原因，例如"密码错误" */
}

static void on_timeout(void)
{
    app_log("esp: prov timeout");
}

static void on_error(int32_t code, const char *text)
{
    (void)code;
    (void)text;
    app_log("esp: cmd err");
}

/* 回调都在 Esp_Poll() 里被调用，尽量短（不要在这里面做长延时或大循环） */
static const esp_cbs_t s_cbs = {
    on_ready,
    on_state_changed,
    on_prov_start,
    on_prov_done,
    on_got_ip,
    on_fail,
    on_timeout,
    NULL,               /* on_wifi_lost：当前固件不发这个事件 */
    on_error
};

/* ------------------------------- main ----------------------------------- */
int32_t main(void)
{
    uint32_t t0;

    app_gpio_init();
#if APP_DEBUG_UART
    app_dbg_init();
#endif
    app_log("esp-node boot");

    /* 1) 移植层：UART1(PA02/PA03) + 1ms 节拍 + RST 脚。
     *    调用之后 Esp_Port()->tick_ms() 才可用。 */
    Esp_HwInit(115200u);

    /* 2) 上电按住 USER 键 3 秒 → 等会儿清掉 WiFi 记录重新配网 */
    s_key_reprov = 0u;
    if (app_key_down() != 0u) {
        t0 = Esp_Port()->tick_ms();
        while (app_key_down() != 0u &&
               (uint32_t)(Esp_Port()->tick_ms() - t0) < APP_KEY_HOLD_MS) {
            /* 等松手，或等够 3 秒 */
        }
        if ((uint32_t)(Esp_Port()->tick_ms() - t0) >= APP_KEY_HOLD_MS) {
            s_key_reprov = 1u;
            app_log("key held -> reprov");
        }
    }

    /* 3) 客户端：内部会拉低 RST 复位模块，然后等 @EVT READY */
    Esp_Init(&s_cbs);
    (void)Esp_WaitReady(8000u);

    if (s_key_reprov != 0u) {
        Esp_Reprov();                       /* 模块会重启并进入配网模式 */
        (void)Esp_WaitReady(8000u);
    }
    Esp_QueryStatus();                      /* 主动查一次状态 */

    for (;;) {
        Esp_Poll();                         /* 必须周期调用，建议 <= 20ms 一次 */
        app_led_task();

        /* ------------------ 你的业务代码放这里 ------------------
         * const esp_status_t *st = Esp_Status();
         *
         * if (st->prov_active) {
         *     // 正在配网：可以把 st->ap_ssid / st->ap_pwd 显示到屏上
         * }
         * if (st->state == ESP_ST_ONLINE) {
         *     // 已联网：上报数据 / 连服务器 / ...
         * }
         * if (st->degraded) {
         *     // 连上路由器但外网不通：可以提示用户
         * }
         * switch (st->state) { ... }   // 也可以做自己的状态机
         * ------------------------------------------------------- */
    }
}
