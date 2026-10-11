#pragma once
/* ============================================================================
 * app_cfg.h —— 全部编译期可调参数
 *
 * 每个宏都用 #ifndef 包住，所以可以在 platformio.ini 的 build_flags 里
 * 用 -DXXX=yyy 覆盖，不必改这个文件。
 * ==========================================================================*/

/* ---------------------------------------------------------------------------
 * 1. 设备身份 / 热点
 * -------------------------------------------------------------------------*/
#ifndef PROV_AP_SSID_PREFIX
#define PROV_AP_SSID_PREFIX   "MyDev-"      /* 热点名 = 前缀 + 芯片ID后4位HEX */
#endif
#ifndef PROV_AP_PWD
#define PROV_AP_PWD           "12345678"    /* 热点密码(至少8位)，印在设备标签上 */
#endif
#ifndef PROV_DEV_NAME
#define PROV_DEV_NAME         "espnode"     /* 默认设备名(mDNS / 门户页显示) */
#endif
#ifndef PROV_AP_CHANNEL
#define PROV_AP_CHANNEL       1
#endif
#ifndef PROV_AP_MAX_CONN
#define PROV_AP_MAX_CONN      4
#endif

/* ---------------------------------------------------------------------------
 * 2. 上电联网 / 重试
 * -------------------------------------------------------------------------*/
#ifndef CFG_STA_CONNECT_TIMEOUT_MS
#define CFG_STA_CONNECT_TIMEOUT_MS   15000UL  /* 单次连接超时 */
#endif
#ifndef CFG_STA_RETRY_PER_CRED
#define CFG_STA_RETRY_PER_CRED       2        /* 每个凭据最多尝试次数 */
#endif
#ifndef CFG_REPROV_AFTER_FAILS
#define CFG_REPROV_AFTER_FAILS       3        /* 连续 N 次上电失败则清凭据进配网 */
#endif

/* ---------------------------------------------------------------------------
 * 3. 联网判定三层策略（见 docs/provisioning_design.md 2.2）
 * -------------------------------------------------------------------------*/
#ifndef CFG_PROBE_TIMEOUT_MS
#define CFG_PROBE_TIMEOUT_MS         3000UL   /* 外网探测总预算 */
#endif
#ifndef CFG_PROBE_INTERVAL_MS
#define CFG_PROBE_INTERVAL_MS        60000UL  /* ONLINE 下的探测间隔 */
#endif
#ifndef CFG_PROBE_FAIL_LIMIT
#define CFG_PROBE_FAIL_LIMIT         3        /* 连续失败几次判定"无外网"(降级上报) */
#endif
#ifndef CFG_LINK_LOST_TIMEOUT_MS
#define CFG_LINK_LOST_TIMEOUT_MS     30000UL  /* 掉线多久后主动重连 */
#endif

/* ---------------------------------------------------------------------------
 * 4. 配网门户
 * -------------------------------------------------------------------------*/
#ifndef CFG_PROV_TIMEOUT_MS
#define CFG_PROV_TIMEOUT_MS          300000UL /* 配网模式超时(5min)后自动重启 */
#endif
#ifndef CFG_PROV_TEARDOWN_DELAY_MS
#define CFG_PROV_TEARDOWN_DELAY_MS   8000UL   /* 配网成功后热点保留多久(让手机看完结果页) */
#endif
#ifndef CFG_SCAN_MAX
#define CFG_SCAN_MAX                 16       /* 门户页最多列出多少个 AP */
#endif
#ifndef CFG_USE_DHCP_OPT114
#define CFG_USE_DHCP_OPT114          1        /* RFC7710：让手机自动弹出配网页 */
#endif

/* ---------------------------------------------------------------------------
 * 5. 与 MCU 的串口链路
 * -------------------------------------------------------------------------*/
#ifndef CFG_HOST_BAUD
#define CFG_HOST_BAUD                115200UL /* UART0(GPIO1/GPIO3) 与 MCU 通信 */
#endif
#ifndef CFG_HOST_HB_MS
#define CFG_HOST_HB_MS               5000UL   /* 向 MCU 发心跳 @EVT HB */
#endif
#ifndef CFG_HOST_SILENCE_RESET_MS
#define CFG_HOST_SILENCE_RESET_MS    0UL      /* MCU 静默多久复位自己；0=关闭 */
#endif

/* ---------------------------------------------------------------------------
 * 6. 其它
 * -------------------------------------------------------------------------*/
#ifndef CFG_ENABLE_MDNS
#define CFG_ENABLE_MDNS              1
#endif
#ifndef CFG_WIFI_NO_SLEEP
#define CFG_WIFI_NO_SLEEP            1        /* 1=关省电更稳；0=平均电流更低 */
#endif
#ifndef CFG_FW_VERSION
#define CFG_FW_VERSION               "0.1.0"
#endif
#ifndef CFG_PROTO_VERSION
#define CFG_PROTO_VERSION            1
#endif
#ifndef CFG_MINIMAL_UI
#define CFG_MINIMAL_UI               0
#endif
