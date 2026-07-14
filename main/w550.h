#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"

/**
 * @brief Initialize the W5500 register driver.
 */
igw_err_t w5500_init(void);

/**
 * @brief Read bytes from a W5500 register block.
 */
igw_err_t w5500_read(uint16_t address, uint8_t control, uint8_t *data, size_t length);

/**
 * @brief Write bytes to a W5500 register block.
 */
igw_err_t w5500_write(uint16_t address, uint8_t control, const uint8_t *data, size_t length);
