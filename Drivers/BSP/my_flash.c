

#include "my_flash.h"

/**
 * @brief 擦除指定地址所在的Flash页
 * @param PageAddress: 要擦除页中的任意一个地址
 * @retval HAL_OK: 成功, 其他: 失败
 */
HAL_StatusTypeDef Flash_Erase_Page(uint32_t PageAddress)
{
    HAL_StatusTypeDef status = HAL_OK;
    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t PageError = 0;

    /* 1. 解锁Flash，允许配置和编程[reference:3] */
    HAL_FLASH_Unlock();

    /* 2. 配置擦除参数[reference:5][reference:6] */
    EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;   // 页擦除模式
    EraseInitStruct.PageAddress = PageAddress;           // 页地址
    EraseInitStruct.NbPages = 1;                         // 擦除1页

    /* 3. 执行页擦除[reference:9][reference:10] */
    status = HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);

    /* 4. 锁定Flash，保护其内容[reference:12] */
    HAL_FLASH_Lock();

    return status;
}

/**
 * @brief 在指定地址写入一个32位字
 * @param Address: 要写入的地址（必须是4的倍数）
 * @param Data: 要写入的数据
 * @retval HAL_OK: 成功, 其他: 失败
 */
HAL_StatusTypeDef Flash_Write_Word(uint32_t Address, uint32_t Data)
{
    HAL_StatusTypeDef status = HAL_OK;

    /* 1. 解锁Flash */
    HAL_FLASH_Unlock();

    /* 2. 写入一个字（32位）[reference:14] */
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, Address, Data);

    /* 3. 锁定Flash */
    HAL_FLASH_Lock();

    return status;
}


















