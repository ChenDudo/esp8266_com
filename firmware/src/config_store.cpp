#include "config_store.h"
#include "app_cfg.h"
#include <EEPROM.h>

/* ------------------------------- 槽布局 ---------------------------------- */
#define CFG_STORE_MAGIC   0x31494657UL   /* "WIFI1" */
#define CFG_STORE_VER     1
#define CFG_SLOT_SIZE     512
#define CFG_SLOT_COUNT    2
#define CFG_EEPROM_SIZE   (CFG_SLOT_SIZE * CFG_SLOT_COUNT)

struct SlotHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t seq;
    uint16_t len;       /* 负载字节数 */
    uint16_t crc;       /* 负载的 CRC16-CCITT */
};

struct Slot {
    SlotHeader hdr;
    AppCfg     cfg;
};

static_assert(sizeof(Slot) <= CFG_SLOT_SIZE, "AppCfg too large for one slot");

/* ------------------------------- CRC16 ----------------------------------- */
static uint16_t crc16_ccitt(const uint8_t* data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ------------------------------ 默认值 ----------------------------------- */
void ConfigStore::loadDefaults()
{
    memset(&_cfg, 0, sizeof(_cfg));
    _cfg.version = CFG_STORE_VER;
    strncpy(_cfg.devName, PROV_DEV_NAME, sizeof(_cfg.devName) - 1);
}

void ConfigStore::sanitize()
{
    if (_cfg.credCount > CFG_MAX_CREDS) {
        _cfg.credCount = 0;
    }
    for (uint8_t i = 0; i < CFG_MAX_CREDS; i++) {
        _cfg.creds[i].ssid[CFG_SSID_LEN - 1] = '\0';
        _cfg.creds[i].pass[CFG_PASS_LEN - 1] = '\0';
    }
    _cfg.devName[CFG_NAME_LEN - 1] = '\0';
    if (_cfg.devName[0] == '\0') {
        strncpy(_cfg.devName, PROV_DEV_NAME, sizeof(_cfg.devName) - 1);
    }
    /* 清掉没有 ssid 的空洞 */
    uint8_t n = 0;
    for (uint8_t i = 0; i < _cfg.credCount; i++) {
        if (_cfg.creds[i].ssid[0] != '\0') {
            if (n != i) _cfg.creds[n] = _cfg.creds[i];
            n++;
        }
    }
    _cfg.credCount = n;
}

/* ------------------------------ 槽扫描 ----------------------------------- */
int8_t ConfigStore::scanSlots(uint16_t* seqOut)
{
    int8_t   best    = -1;
    uint16_t bestSeq = 0;

    for (uint8_t i = 0; i < CFG_SLOT_COUNT; i++) {
        Slot s;
        EEPROM.get((int)(i * CFG_SLOT_SIZE), s);
        if (s.hdr.magic   != CFG_STORE_MAGIC) continue;
        if (s.hdr.version != CFG_STORE_VER)   continue;
        if (s.hdr.len     != sizeof(AppCfg))  continue;
        if (crc16_ccitt((const uint8_t*)&s.cfg, s.hdr.len) != s.hdr.crc) continue;
        if (best < 0 || s.hdr.seq > bestSeq) {
            best    = (int8_t)i;
            bestSeq = s.hdr.seq;
        }
    }
    if (seqOut) *seqOut = bestSeq;
    return best;
}

/* ------------------------------- 载入 ------------------------------------ */
bool ConfigStore::begin()
{
    EEPROM.begin(CFG_EEPROM_SIZE);

    _valid      = false;
    _activeSlot = -1;
    _lastCrc    = 0;
    loadDefaults();

    uint16_t seq = 0;
    int8_t   idx = scanSlots(&seq);
    if (idx < 0) return false;

    Slot s;
    EEPROM.get((int)(idx * CFG_SLOT_SIZE), s);
    memcpy(&_cfg, &s.cfg, sizeof(AppCfg));
    sanitize();
    _activeSlot = idx;
    _valid      = true;
    _lastCrc    = s.hdr.crc;
    return true;
}

/* ------------------------------- 保存 ------------------------------------ */
bool ConfigStore::save()
{
    _cfg.version = CFG_STORE_VER;

    uint16_t seq = 0;
    int8_t   cur = scanSlots(&seq);
    uint8_t  next = (cur < 0) ? 0 : (uint8_t)((cur + 1) % CFG_SLOT_COUNT);

    Slot s;
    memset(&s, 0, sizeof(s));
    s.hdr.magic   = CFG_STORE_MAGIC;
    s.hdr.version = CFG_STORE_VER;
    s.hdr.seq     = (uint16_t)(seq + 1);
    s.hdr.len     = (uint16_t)sizeof(AppCfg);
    s.hdr.crc     = crc16_ccitt((const uint8_t*)&_cfg, (uint16_t)sizeof(AppCfg));
    memcpy(&s.cfg, &_cfg, sizeof(AppCfg));

    EEPROM.put((int)(next * CFG_SLOT_SIZE), s);
    if (!EEPROM.commit()) {
        return false;
    }
    _activeSlot = (int8_t)next;
    _valid      = true;
    _lastCrc    = s.hdr.crc;
    return true;
}

/* ------------------------- 凭据增删改 ------------------------------------ */
const WifiCred* ConfigStore::credAt(uint8_t i) const
{
    if (i >= _cfg.credCount || i >= CFG_MAX_CREDS) return NULL;
    return &_cfg.creds[i];
}

bool ConfigStore::addOrUpdateCredential(const char* ssid, const char* pass)
{
    if (ssid == NULL || ssid[0] == '\0')            return false;
    if (strlen(ssid) >= CFG_SSID_LEN)               return false;
    if (pass != NULL && strlen(pass) >= CFG_PASS_LEN) return false;

    WifiCred nc;
    memset(&nc, 0, sizeof(nc));
    strncpy(nc.ssid, ssid, CFG_SSID_LEN - 1);
    strncpy(nc.pass, (pass != NULL) ? pass : "", CFG_PASS_LEN - 1);

    int8_t found = -1;
    for (uint8_t i = 0; i < _cfg.credCount && i < CFG_MAX_CREDS; i++) {
        if (strcmp(_cfg.creds[i].ssid, ssid) == 0) {
            found = (int8_t)i;
            break;
        }
    }

    uint8_t n = _cfg.credCount;
    if (found < 0) {
        if (n >= CFG_MAX_CREDS) n = CFG_MAX_CREDS - 1;   /* 满了就丢末尾那个 */
        else                    n = (uint8_t)(n + 1);
    } else {
        n = (uint8_t)(found + 1);
    }
    n = (uint8_t)MIN(n, (uint8_t)CFG_MAX_CREDS);

    for (int8_t i = (int8_t)n - 1; i > 0; i--) {
        _cfg.creds[i] = _cfg.creds[i - 1];
    }
    _cfg.creds[0]    = nc;
    _cfg.credCount   = n;
    _cfg.flags      |= 0x0001;
    return true;
}

void ConfigStore::clearCredentials()
{
    memset(_cfg.creds, 0, sizeof(_cfg.creds));
    _cfg.credCount = 0;
    _cfg.failCount = 0;
    _cfg.flags    &= (uint16_t)~0x0001;
}

void ConfigStore::setDevName(const char* name)
{
    if (name == NULL || name[0] == '\0') return;
    memset(_cfg.devName, 0, sizeof(_cfg.devName));
    strncpy(_cfg.devName, name, sizeof(_cfg.devName) - 1);
}

/* ------------------------------ 复位 ------------------------------------- */
void ConfigStore::factoryReset()
{
    Slot s;
    memset(&s, 0, sizeof(s));
    for (uint8_t i = 0; i < CFG_SLOT_COUNT; i++) {
        EEPROM.put((int)(i * CFG_SLOT_SIZE), s);
    }
    EEPROM.commit();
    loadDefaults();
    _valid      = false;
    _activeSlot = -1;
    _lastCrc    = 0;
}
