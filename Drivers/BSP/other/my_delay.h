

/*

	 
*/

#ifndef __MY_DELAY_H
#define __MY_DELAY_H


#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

static uint32_t g_fac_us = 0;   //
void delay_init(void);
void delay_us(uint32_t nus);
void delay_ms(uint32_t nms);




#endif




