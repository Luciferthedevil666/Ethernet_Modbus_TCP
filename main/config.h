#pragma once

#include "driver/spi_master.h"

#define GATEWAY_DEVICE_NAME "PLC01"

#define W5500_SPI_HOST SPI2_HOST
#define W5500_SPI_CLOCK_HZ 20000000
#define W5500_PHY_ADDR 1

#define CLOUD_GOOGLE_SCRIPT_HOST "script.google.com"
#define CLOUD_GOOGLE_SCRIPT_PATH "/macros/s/AKfycby1Htg3a0idXy6TQNXRXqb0zqzmIL4bxeptHTPGlPYOqZ5d1V_3A0G0lpp-6vMSe-jg/exec"
#define CLOUD_GOOGLE_SCRIPT_URL "https://script.google.com/macros/s/AKfycby1Htg3a0idXy6TQNXRXqb0zqzmIL4bxeptHTPGlPYOqZ5d1V_3A0G0lpp-6vMSe-jg/exec"
#define CLOUD_GOOGLE_SCRIPT_PORT 443U
#define CLOUD_GOOGLE_SCRIPT_USE_TLS 1U
