
#ifndef __LED_H
#define __LED_H

#include "stm32f1xx_hal.h"

#define LED0_PORT   GPIOB
#define LED0_PIN    GPIO_PIN_5
#define LED0_ON_S     GPIO_PIN_RESET  
#define LED0_OFF_S     GPIO_PIN_SET  


#define LED1_PORT   GPIOE
#define LED1_PIN    GPIO_PIN_5
#define LED1_ON_S     GPIO_PIN_RESET  
#define LED1_OFF_S    GPIO_PIN_RESET 

void LED_Init(void);
void LED0_ON(void);
void LED0_OFF(void);
void LED1_ON(void);
void LED1_OFF(void);
void LED0_TOGGLE(void);
void LED1_TOGGLE(void);
#endif












