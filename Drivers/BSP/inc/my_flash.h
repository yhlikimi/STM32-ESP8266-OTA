

#ifndef   __FLASH_H
#define  __FLASH_H


#include "stm32f1xx_hal.h" 

HAL_StatusTypeDef Flash_Erase_Page(uint32_t PageAddress);
HAL_StatusTypeDef Flash_Write_Word(uint32_t Address, uint32_t Data);




#endif



