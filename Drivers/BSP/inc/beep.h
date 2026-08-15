
#ifndef __BEEP_H
#define __BEEP_H

#include "stm32f1xx_hal.h"

#define BEEP_PORT  GPIOB
#define BEEP_PIN   GPIO_PIN_8

#define BEEP_ON_S   GPIO_PIN_SET
#define BEEP_OFF_S  GPIO_PIN_RESET

void beep_Init(void);
void beep_On(void);
void beep_Off(void);

#endif



