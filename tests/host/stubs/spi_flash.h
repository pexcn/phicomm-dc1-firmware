#pragma once
#include "c_types.h"
#define SPI_FLASH_SEC_SIZE 4096
#define SPI_FLASH_RESULT_OK 0
int spi_flash_read(uint32 address, uint32 *data, uint32 size);
int spi_flash_write(uint32 address, uint32 *data, uint32 size);
int spi_flash_erase_sector(uint16 sector);
