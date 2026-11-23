#ifndef WIFI_H
#define WIFI_H

#include "esp_wifi.h"
#include "lwip/inet.h"

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif
void wifi_init_softap(void);
#ifdef __cplusplus
}
#endif

#endif
