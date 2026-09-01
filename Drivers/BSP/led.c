


#include "led.h"


void LED_Init(void){
	
			GPIO_InitTypeDef GPIO_InitStructure = {0};
			__HAL_RCC_GPIOB_CLK_ENABLE();
			GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_PP;
			GPIO_InitStructure.Pin = LED0_PIN;
			GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH;
			HAL_GPIO_Init(LED0_PORT, &GPIO_InitStructure);
			HAL_GPIO_WritePin(LED0_PORT, LED0_PIN,LED0_OFF_S);
			__HAL_RCC_GPIOE_CLK_ENABLE();
			GPIO_InitStructure.Pin = LED1_PIN;
			HAL_GPIO_Init(LED1_PORT, &GPIO_InitStructure);
			HAL_GPIO_WritePin(LED1_PORT, LED1_PIN,LED1_OFF_S);
	
}

void LED0_ON(void){
		HAL_GPIO_WritePin(LED0_PORT, LED0_PIN,LED0_ON_S);
}

void LED0_TOGGLE(void){
		HAL_GPIO_TogglePin(LED0_PORT, LED0_PIN);
}
void LED0_OFF(void){
		HAL_GPIO_WritePin(LED0_PORT, LED0_PIN,LED0_OFF_S);
}
void LED1_ON(void){
		HAL_GPIO_WritePin(LED1_PORT, LED1_PIN,LED1_ON_S);
}
void LED1_OFF(void){
	HAL_GPIO_WritePin(LED1_PORT, LED1_PIN,LED1_OFF_S);
}

void LED1_TOGGLE(void){
		HAL_GPIO_TogglePin(LED1_PORT, LED1_PIN);
}

