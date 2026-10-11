#include "esp_port.h"

static const esp_port_t *s_port;

void Esp_PortBind(const esp_port_t *port)
{
    s_port = port;
}

const esp_port_t *Esp_Port(void)
{
    return s_port;
}
