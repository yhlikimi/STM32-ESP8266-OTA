


#include "beep.h"


void beep_Init(void){
		GPIO_InitTypeDef GPIO_InitStructure = {0};
		__HAL_RCC_GPIOB_CLK_ENABLE();
		GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_PP;
		GPIO_InitStructure.Pin = BEEP_PIN;
		GPIO_InitStructure.Pull = GPIO_PULLDOWN;
		GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH;
		HAL_GPIO_Init(BEEP_PORT, &GPIO_InitStructure);
		beep_Off();
}

void beep_On(void){
	HAL_GPIO_WritePin(BEEP_PORT, BEEP_PIN,BEEP_ON_S);
}

void beep_Off(void){
	HAL_GPIO_WritePin(BEEP_PORT, BEEP_PIN,BEEP_OFF_S);
}



















