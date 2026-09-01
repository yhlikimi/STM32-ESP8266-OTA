

/*
	DHT11.h模块










*/






#ifndef __DHT11_H
#define __DHT11_H 

#include "stm32f1xx_hal.h"


/******************************************************************************************/


#define DHT11_DQ_GPIO_PORT                  GPIOG
#define DHT11_DQ_GPIO_PIN                   GPIO_PIN_13
#define DHT11_VCC_PORT						GPIOG
#define DHT11_VCC_PIN						GPIO_PIN_10
#define DHT11_GND_PORT						GPIOG
#define DHT11_GND_PIN						GPIO_PIN_14



#define DHT11_DQ_GPIO_CLK_ENABLE()          do{ __HAL_RCC_GPIOG_CLK_ENABLE(); }while(0)   




/******************************************************************************************/


#define DHT11_DQ_OUT(x)     do{ x ? \
                                HAL_GPIO_WritePin(DHT11_DQ_GPIO_PORT, DHT11_DQ_GPIO_PIN, GPIO_PIN_SET) : \
                                HAL_GPIO_WritePin(DHT11_DQ_GPIO_PORT, DHT11_DQ_GPIO_PIN, GPIO_PIN_RESET); \
                            }while(0)                                                
#define DHT11_DQ_IN         HAL_GPIO_ReadPin(DHT11_DQ_GPIO_PORT, DHT11_DQ_GPIO_PIN)  


uint8_t dht11_init(void);   
uint8_t dht11_check(void);  
uint8_t dht11_read_data(uint8_t *temp,uint8_t *humi);   

#endif
















