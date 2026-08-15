

#include "my_delay.h"


// 初始化
void delay_init(void)
{
    g_fac_us = SystemCoreClock / 1000000;
  
    // 1. 如果 LOAD 为 0（硬件完全未初始化），基于主频设置一个默认的重载值（1ms）
    if (SysTick->LOAD == 0) {
        SysTick->LOAD = SystemCoreClock / 1000;  // 72MHz -> 72000
    }
    
    // 2. 如果 SysTick 未使能，强行打开计数器（但绝对不开启中断 BIT1，避免 FreeRTOS 未就绪时触发中断）
    if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0) {
        SysTick->VAL = 0;                         // 清空当前值
        // BIT2: 时钟源 = AHB/8 (STM32F1固定); BIT0: 使能; BIT1(TICKINT): 保持0（不中断）
        SysTick->CTRL |= SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
        // 注意：此时没有使能中断，FreeRTOS 尚未接管，绝对不会进中断！
    }
}

// 微秒延时（自旋）
void delay_us(uint32_t nus)
{
    uint32_t ticks = nus * g_fac_us;
    uint32_t told = SysTick->VAL;
    uint32_t reload = SysTick->LOAD;
    uint32_t tcnt = 0, tnow;

    while (1)
    {
        tnow = SysTick->VAL;
        if (tnow != told)
        {
            if (tnow < told) tcnt += told - tnow;
            else tcnt += reload - tnow + told;
            told = tnow;
            if (tcnt >= ticks) break;
        }
    }
}

#ifdef OS_DELAY
// 毫秒延时（支持 FreeRTOS）
void delay_ms(uint32_t nms)
{
    // 调度器未启动或小于 2ms 时自旋
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED || nms < 2)
    {
        for (uint32_t i = 0; i < nms; i++)
            delay_us(1000);
        return;
    }

    // 在中断中调用则自旋
    if (xPortIsInsideInterrupt())
    {
        for (uint32_t i = 0; i < nms; i++)
            delay_us(1000);
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(nms));
    }
}

#else

	void delay_ms(uint32_t nms){
		for (uint32_t i = 0; i < nms; i++)
            delay_us(1000);
    return;
	}

#endif








