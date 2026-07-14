#pragma once

/*==============================================================================
 * Includes
 *============================================================================*/
#include "driver/gpio.h"
/*==============================================================================
 * Public Macros
 *============================================================================*/
#define BOARD_NAME        "Industrial Gateway"
#define BOARD_VERSION     "1.0"
/*==============================================================================
 * Public Constants
 *============================================================================*/
static const gpio_num_t BOARD_ETH_CS_PIN = GPIO_NUM_10;
static const gpio_num_t BOARD_ETH_RST_PIN = GPIO_NUM_14;
static const gpio_num_t BOARD_ETH_INT_PIN = GPIO_NUM_8;
static const gpio_num_t BOARD_SPI_MOSI_PIN = GPIO_NUM_11;
static const gpio_num_t BOARD_SPI_MISO_PIN = GPIO_NUM_13;
static const gpio_num_t BOARD_SPI_SCK_PIN = GPIO_NUM_12;
/*==============================================================================
 * Public Enumerations
 *============================================================================*/

/*==============================================================================
 * Public Structures
 *============================================================================*/
typedef struct
{
    gpio_num_t cs;
    gpio_num_t rst;
    gpio_num_t irq;
} board_eth_config_t;
/*==============================================================================
 * Public Function Prototypes
 *============================================================================*/
 void board_init(void);
 void board_eth_reset_assert(void);
void board_eth_reset_release(void);
