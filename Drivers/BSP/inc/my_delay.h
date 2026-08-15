

/*

	 
*/



#ifndef __MY_DELAY_H
#define __MY_DELAY_H

//
#define OS_DELAY  


#include "stm32f1xx_hal.h"

#ifdef OS_DELAY

#include "FreeRTOS.h"
#include "task.h"

#endif

static uint32_t g_fac_us = 0;   //
void delay_init(void);
void delay_us(uint32_t nus);
void delay_ms(uint32_t nms);




#endif




