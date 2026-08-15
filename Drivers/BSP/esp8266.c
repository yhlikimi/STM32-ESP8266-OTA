/*
	
	使用时候需要注意：
		（1）MQTT参数 clientID和passwd 是动态更新的,要及时调整；
		（2）要注意字符转义；
		（3）先验证ESP8266基本参数；AT命令？，要先连上wifi;
		
	配置没有考虑每个AT命令的返回情况，没有处理发回来的字符串。
	
	
	
								
	

*/


#include "esp8266.h"
#include "my_delay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "stm32f1xx_hal_dma.h"  // 别忘了包含DMA库
	
// ---------- 全局变量定义 ----------
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart3_rx;	
	
/* USART3 DMA始终使用循环模式。普通模式在IDLE时把一帧交给MQTT任务；
 * OTA模式由下载任务直接从该环形缓冲区取数，Flash编程时DMA仍可继续收数。 */
static uint8_t usart3_rx_buffer[RX_BUF_SIZE];
uint8_t rx_parse_buffer[RX_BUF_SIZE + 1];
uint16_t rx_parse_len = 0;
//普通MQTT消息还是 OTA固件
static volatile uint8_t ota_rx_mode = 0;

static volatile uint16_t dma_read_pos = 0;
static uint16_t mqtt_dma_pos = 0;

// 信号量（需在主函数或任务创建中初始化）
SemaphoreHandle_t xMqttDataSemaphore = NULL;
SemaphoreHandle_t xEsp8266TxMutex = NULL;
	
// MQTT 连接参数： 	

const char *PRODUCT_KEY="h8sfOvOnleu";
const char *DEVICE_NAME="data";       //"demo";

const char *SSID="huahua";
const char *SSID_PASSWD="yhlikimi";
const char *USERNAME="data&h8sfOvOnleu";
const char *PASSWD="7254654997592deb69b4023188afacc03f555103ffda2bffd40bbbda22d68017";
//","前面加了转义字符"\"
const char *CLIENTID="h8sfOvOnleu.data|securemode=2\\,signmethod=hmacsha256\\,timestamp=1786009932394|";
const char *MQTTHOSTURL="iot-06z00i93m2wm64q.mqtt.iothub.aliyuncs.com";

//topic id
int topicid=0;	
uint32_t g_flash_current_addr = APP2_ADDRESS;
uint32_t g_flash_written = 0;	
	
	
void esp8266_MspInit(){
	// 使能GPIOB,USART3,DMA时钟
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_USART3_CLK_ENABLE();
	__HAL_RCC_DMA1_CLK_ENABLE();   
	
	// 配置GPIO：PB10为TX（复用推挽输出），PB11为RX（浮空输入）
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Pin = TX_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(TX_PORT, &GPIO_InitStruct);
	
	GPIO_InitStruct.Pin = RX_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(RX_PORT, &GPIO_InitStruct);
	
	//配置DMA（USART3_RX 对应 DMA1 通道3）
    hdma_usart3_rx.Instance = DMA1_Channel3;
    hdma_usart3_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;   // 外设到内存
    hdma_usart3_rx.Init.PeriphInc = DMA_PINC_DISABLE;       // 外设地址不增加
    hdma_usart3_rx.Init.MemInc = DMA_MINC_ENABLE;           // 内存地址递增
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode = DMA_CIRCULAR;                // 循环模式（自动覆盖）
    hdma_usart3_rx.Init.Priority = DMA_PRIORITY_HIGH;       // 优先级高一点
    HAL_DMA_Init(&hdma_usart3_rx);
	// 将DMA句柄绑定到UART句柄（HAL库内部关联）
    __HAL_LINKDMA(&huart3, hdmarx, hdma_usart3_rx);
	
	// 配置USART3：波特率115200，8数据位，1停止位，无校验[reference:5]
	huart3.Instance = USART3;
	huart3.Init.BaudRate = 115200;
	huart3.Init.WordLength = UART_WORDLENGTH_8B;
	huart3.Init.StopBits = UART_STOPBITS_1;
	huart3.Init.Parity = UART_PARITY_NONE;
	huart3.Init.Mode = UART_MODE_TX_RX;
	huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	HAL_UART_Init(&huart3);
	
	// 使能空闲中断
	 __HAL_UART_ENABLE_IT(&huart3, UART_IT_IDLE);
	// 启动DMA接收（连续不断循环接收，把数据放入大缓冲区）
    HAL_UART_Receive_DMA(&huart3, (uint8_t *)usart3_rx_buffer, RX_BUF_SIZE);
		HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
		
}
	
// 重写 USART3 的中断服务函数（在 stm32f1xx_it.c 中）
// 注意：务必屏蔽 stm32f1xx_it.c 中同名的函数，否则编译报错
void USART3_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // 检查是否触发了空闲中断（IDLE）
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_IDLE) != RESET) 
    {
			// 清空闲中断标志,防止重复触发（必须先读SR再读DR，或者用库函数）
        __HAL_UART_CLEAR_IDLEFLAG(&huart3);
        
        if (!ota_rx_mode && xMqttDataSemaphore != NULL) {
					//已经写的位置write_pos=buffer大小-还有多少字节未搬运
            uint16_t write_pos = RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
					//获取未处理的数据
            uint16_t len = (write_pos >= mqtt_dma_pos) ?
                           (write_pos - mqtt_dma_pos) :
                           (RX_BUF_SIZE - mqtt_dma_pos + write_pos);

            if (len > 0 && len <= RX_BUF_SIZE) {
                uint16_t first = RX_BUF_SIZE - mqtt_dma_pos;
                if (first > len) first = len;
                memcpy(rx_parse_buffer, &usart3_rx_buffer[mqtt_dma_pos], first);
                if (len > first) {
                    memcpy(&rx_parse_buffer[first], usart3_rx_buffer, len - first);
                }
                rx_parse_buffer[len] = '\0';
                rx_parse_len = len;
                mqtt_dma_pos = write_pos;
                xSemaphoreGiveFromISR(xMqttDataSemaphore, &xHigherPriorityTaskWoken);
            }
        }
    }
    
    // 调用HAL库的通用中断处理
    HAL_UART_IRQHandler(&huart3);
    
    // 如果信号量导致高优先级任务就绪，则立即切换
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}



// 辅助函数：发送一条 AT 指令（自动加 \r\n），并延时指定毫秒
 void esp8266_send_at(const char *cmd, uint32_t delayms)
{
    char send_buf[256];
    snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd);
    HAL_UART_Transmit(&huart3, (uint8_t*)send_buf, strlen(send_buf), 100);
    if (delayms > 0) {
        delay_ms(delayms);
    }
}

/* Initialization runs before the scheduler starts.  Read responses directly
 * from the already-running circular DMA buffer so each command is completed
 * before the next one is transmitted. */
uint8_t esp8266_send_at_wait(const char *cmd, const char *expected, uint32_t timeout_ms)
{
    char send_buf[256];
    char response[512];
    uint16_t read_pos = (uint16_t)(RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx));
    uint16_t used = 0U;
    uint32_t start = HAL_GetTick();
    int send_len;

    response[0] = '\0';
    send_len = snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd);
    if (send_len <= 0 || send_len >= (int)sizeof(send_buf) ||
        HAL_UART_Transmit(&huart3, (uint8_t *)send_buf, (uint16_t)send_len, 1000U) != HAL_OK) {
        return 0U;
    }

    while ((HAL_GetTick() - start) < timeout_ms) {
        uint16_t write_pos = (uint16_t)(RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx));
        while (read_pos != write_pos) {
            char ch = (char)usart3_rx_buffer[read_pos++];
            if (read_pos >= RX_BUF_SIZE) read_pos = 0U;

            if (used < sizeof(response) - 1U) {
                response[used++] = ch;
            } else {
                memmove(response, response + 1, sizeof(response) - 2U);
                response[sizeof(response) - 2U] = ch;
                used = sizeof(response) - 1U;
            }
            response[used] = '\0';

            if (strstr(response, expected) != NULL) {
                mqtt_dma_pos = read_pos;
                return 1U;
            }
            if (strstr(response, "ERROR") != NULL || strstr(response, "busy p...") != NULL) {
                printf("ESP8266 command failed: %s\r\n", cmd);
                mqtt_dma_pos = read_pos;
                return 0U;
            }
        }
        HAL_Delay(1U);
    }

    printf("ESP8266 timeout waiting for %s: %s\r\n", expected, cmd);
    mqtt_dma_pos = read_pos;
    return 0U;
}

static uint8_t esp8266_command_retry(const char *cmd, const char *expected,
                                     uint32_t timeout_ms, uint8_t attempts)
{
    for (uint8_t attempt = 0U; attempt < attempts; attempt++) {
        if (esp8266_send_at_wait(cmd, expected, timeout_ms)) return 1U;
        HAL_Delay(500U);
    }
    return 0U;
}
/*

	如果配置有问题，直接将模块连接到电脑，用串口配置。
	部分配置可以存储在ESP8266 flash中，可以先用串口助手设置好部分参数，完成初始化。
	配置1-4属于基础配置，我们直接利用串口助手连接ESP8266进行配置；
	打开手机热点连接；
	MQTT参数利用代码进行配置： 注意参数可能会变化，要及时更新。
*/


uint8_t esp8266_init(void){
		// 1. 恢复出厂设置（需要较长时间重启，延时 3000ms）
    //esp8266_send_at("AT+RESTORE", 3000);
    
    //2. 设置 WiFi 模式为 Station
    if (!esp8266_command_retry("AT+CWMODE=1", "OK", 2000U, 2U)) return 0U;
	
    // 3. 配置 NTP（可选）
    if (!esp8266_command_retry("AT+CIPSNTPCFG=1,8,\"cn.ntp.org.cn\",\"ntp.sjtu.edu.cn\"", "OK", 2000U, 2U)) return 0U;
    
    // 4. 连接 WiFi
    char wifi_cmd[128];
    snprintf(wifi_cmd, sizeof(wifi_cmd), "AT+CWJAP=\"%s\",\"%s\"", SSID, SSID_PASSWD);
    if (!esp8266_command_retry(wifi_cmd, "OK", 15000U, 2U)) return 0U;
		printf("4-%s\r\n",wifi_cmd);
    
    // 5. 设置 MQTT 用户属性
    char mqtt_user_cmd[256];
    snprintf(mqtt_user_cmd, sizeof(mqtt_user_cmd),
             "AT+MQTTUSERCFG=0,1,\"NULL\",\"%s\",\"%s\",0,0,\"\"",
             USERNAME, PASSWD);
    if (!esp8266_command_retry(mqtt_user_cmd, "OK", 3000U, 2U)) return 0U;
		printf("5-%s\r\n",mqtt_user_cmd);
    
    // 6. 设置 MQTT Client ID
    char mqtt_client_cmd[256];
    snprintf(mqtt_client_cmd, sizeof(mqtt_client_cmd),
             "AT+MQTTCLIENTID=0,\"%s\"", CLIENTID);
    if (!esp8266_command_retry(mqtt_client_cmd, "OK", 3000U, 2U)) return 0U;
		printf("6-%s\r\n",mqtt_client_cmd);
    
    // 7. 连接阿里云 MQTT 服务器
    char mqtt_conn_cmd[256];
    snprintf(mqtt_conn_cmd, sizeof(mqtt_conn_cmd),
             "AT+MQTTCONN=0,\"%s\",1883,1", MQTTHOSTURL);
    if (!esp8266_command_retry(mqtt_conn_cmd, "+MQTTCONNECTED", 15000U, 2U)) return 0U;
		printf("7-%s\r\n",mqtt_conn_cmd);
    
		if (!esp8266_command_retry("AT+MQTTSTATE?", "+MQTTSTATE:", 3000U, 2U)) return 0U;
		
		//订阅消息
		if (!esp8266_describe()) return 0U;
    // 所有指令已发送完成，不检查是否成功
    printf("All ESP8266 commands confirmed.\r\n");
		//esp8266_send_at("AT+GMR", 500);
		return 1U;
}


void esp8266_exit_transparent_mode(void)
{
    /* +++ 前后必须保持至少1秒，且 +++ 后不能加 \r\n */
    HAL_Delay(1200);

    HAL_UART_Transmit(&huart3, (uint8_t *)"+++", 3, 1000);

    HAL_Delay(1200);

    /* 此时应该已经回到AT命令模式 */
    esp8266_send_at("AT+CIPCLOSE", 500);
    esp8266_send_at("AT+CIPMODE=0", 500);

    /* 清理残留的HTTP接收数据 */
    rx_parse_len = 0;
    mqtt_dma_pos =
        RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx);

    HAL_Delay(500);

    printf("ESP8266: transparent mode exit attempted\r\n");
}

//发送数据上报：用于发送温湿度数据
void esp8266_publish_data(uint32_t temperature, uint32_t humidity)
{
		/* OTA使用同一个USART3透传HTTP固件流。此时任何AT+MQTTPUB
		 * 都会被当成HTTP数据，污染固件并破坏+++退出透传的静默时间。 */
		if (ota_rx_mode) {
			return;
		}
		if (xEsp8266TxMutex != NULL &&
			xSemaphoreTake(xEsp8266TxMutex, 0U) != pdTRUE) {
			return;
		}
		if (ota_rx_mode) {
			if (xEsp8266TxMutex != NULL) {
				xSemaphoreGive(xEsp8266TxMutex);
			}
			return;
		}
	  
		char topic[128];
    snprintf(topic, sizeof(topic), TOPIC_UPDATE, PRODUCT_KEY, DEVICE_NAME);

    // 构造 Payload，注意转义：双引号 -> \"，逗号前加 \,
    // C 字符串中：\\\" 产生 \"，\\, 产生 \,
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\\\"params\\\":{\\\"temperature\\\":%d\\,\\\"Humidity\\\":%d}}",
             temperature, humidity);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTPUB=0,\"%s\",\"%s\",1,0",
             topic, payload);

    esp8266_send_at(cmd, 500);
    printf("Published: %s\r\n", cmd);
		if (xEsp8266TxMutex != NULL) xSemaphoreGive(xEsp8266TxMutex);
	
}

uint8_t esp8266_request_cloud_time(uint32_t device_send_ms)
{
    char topic[128];
    char payload[96];
    char cmd[320];
    int len;

    if (ota_rx_mode || xEsp8266TxMutex == NULL ||
        xSemaphoreTake(xEsp8266TxMutex, 0U) != pdTRUE) {
        return 0U;
    }

    if (ota_rx_mode) {
        xSemaphoreGive(xEsp8266TxMutex);
        return 0U;
    }

    snprintf(topic, sizeof(topic), NTP_REQUEST_TOPIC, PRODUCT_KEY, DEVICE_NAME);
    snprintf(payload, sizeof(payload),
             "{\\\"deviceSendTime\\\":\\\"%lu\\\"}",
             (unsigned long)device_send_ms);
    len = snprintf(cmd, sizeof(cmd),
                   "AT+MQTTPUB=0,\"%s\",\"%s\",0,0",
                   topic, payload);

    if (len <= 0 || len >= (int)sizeof(cmd)) {
        xSemaphoreGive(xEsp8266TxMutex);
        return 0U;
    }

    esp8266_send_at(cmd, 0U);
    xSemaphoreGive(xEsp8266TxMutex);
    printf("Cloud time request sent\r\n");
    return 1U;
}

/**
 * @brief 上报当前固件版本号到阿里云OTA服务
 * @param version 版本号字符串，如 "1.0.0"
 */
void esp8266_report_version(const char *version)
{
    char topic[128];
    snprintf(topic, sizeof(topic), OTA_INFORM_TOPIC, PRODUCT_KEY, DEVICE_NAME);

    // 构造 Payload：{"id":"1","params":{"version":"1.0.0"}}
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\\\"id\\\":\\\"%d\\\"\\,\\\"params\\\":{\\\"version\\\":\\\"%s\\\"}}",
             ++topicid,version);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTPUB=0,\"%s\",\"%s\",1,0",
             topic, payload);

    /* Called after the receive task starts.  Do not consume the response here:
     * Aliyun may push an OTA notification immediately after subscription and
     * both messages must go through the common DMA message classifier. */
    esp8266_send_at(cmd, 0U);
    printf("Report version sent: %s\r\n", version);
}


//订阅 Topic
uint8_t esp8266_describe(void){
	uint8_t all_ok = 1U;
	
	//订阅属性上报Topic  (上报温湿度)
	  char topic[128];
    snprintf(topic, sizeof(topic), MY_DESCRIBLE, PRODUCT_KEY, DEVICE_NAME);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=0,\"%s\",1", topic);
    if (esp8266_send_at_wait(cmd, "OK", 3000U))
        printf("Subscribed to post topic: %s\r\n", topic);
    else
    {
        printf("Subscribe failed: %s\r\n", topic);
		all_ok = 0U;
    }
	
	  // 订阅属性设置Topic  (获取温度阈值)
    char topic2[128];
    snprintf(topic2, sizeof(topic2), TOPIC_GET, PRODUCT_KEY, DEVICE_NAME);
    char cmd2[256];
    snprintf(cmd2, sizeof(cmd2), "AT+MQTTSUB=0,\"%s\",1", topic2);
    if (esp8266_send_at_wait(cmd2, "OK", 3000U))
        printf("Subscribed to set topic: %s\r\n", topic2);
    else
    {
        printf("Subscribe failed: %s\r\n", topic2);
		all_ok = 0U;
    }
		
	 //订阅 OTA  (上报固件信息)
	/*
		char topic3[128];
    snprintf(topic3, sizeof(topic3), OTA_INFORM_TOPIC, PRODUCT_KEY, DEVICE_NAME);
    char cmd3[256];
    snprintf(cmd3, sizeof(cmd3), "AT+MQTTSUB=0,\"%s\",1", topic3);
    esp8266_send_at(cmd3, 500);
    printf("Subscribed to OTA topic: %s\r\n", topic3);
		*/
		
		//订阅升级信息下行 
		char topic4[128];
    snprintf(topic4, sizeof(topic4), OTA_UPGRADE, PRODUCT_KEY, DEVICE_NAME);
    char cmd4[256];
    snprintf(cmd4, sizeof(cmd4), "AT+MQTTSUB=0,\"%s\",1", topic4);
    if (esp8266_send_at_wait(cmd4, "OK", 3000U))
        printf("Subscribed to OTA upgrade topic: %s\r\n", topic4);
    else
    {
        printf("Subscribe failed: %s\r\n", topic4);
		all_ok = 0U;
    }

	/* Subscribe to Aliyun's built-in NTP response topic before the APP sends
	 * its first automatic cloud-time request. */
	char ntp_topic[128];
	char ntp_cmd[256];
	snprintf(ntp_topic, sizeof(ntp_topic), NTP_RESPONSE_PATH,
	         PRODUCT_KEY, DEVICE_NAME);
	snprintf(ntp_cmd, sizeof(ntp_cmd), "AT+MQTTSUB=0,\"%s\",0", ntp_topic);
	if (esp8266_send_at_wait(ntp_cmd, "OK", 3000U)) {
		printf("Subscribed to NTP response topic: %s\r\n", ntp_topic);
	} else {
		printf("Subscribe failed: %s\r\n", ntp_topic);
		all_ok = 0U;
	}
	
	return all_ok;
}

//下发OTA升级消息时：提取 url md5 version
void parse_ota_notification(const char *buffer, ota_info_t *info) {
    // 清空结构体
    memset(info, 0, sizeof(ota_info_t));
    
    // 1. 提取 url
    char *url_start = strstr(buffer, "\"url\":\"");
    if (url_start) {
        url_start += 7; // 跳过 "url":"
        char *url_end = strstr(url_start, "\"");
        if (url_end) {
            uint32_t len = url_end - url_start;
            if (len < sizeof(info->url)) {
                memcpy(info->url, url_start, len);
                info->url[len] = '\0';
            }
        }
    }
    
    // 2. 提取 size
    char *size_start = strstr(buffer, "\"size\":");
    if (size_start) {
        // 格式: "size":220096 或 "size": 220096 (可能带空格)
        if (sscanf(size_start, "\"size\":%u", &info->size) != 1) {
            // 如果失败，尝试跳过可能存在的空格
            sscanf(size_start, "\"size\": %u", &info->size);
        }
    }
    
    // 3. 提取 md5
    char *md5_start = strstr(buffer, "\"md5\":\"");
    uint32_t md5_prefix_len = 7U;
    if (md5_start == NULL) {
        md5_start = strstr(buffer, "\"sign\":\"");
        md5_prefix_len = 8U;
    }
    if (md5_start) {
        md5_start += md5_prefix_len;
        char *md5_end = strstr(md5_start, "\"");
        if (md5_end) {
            uint32_t len = md5_end - md5_start;
            if (len < sizeof(info->md5)) {
                memcpy(info->md5, md5_start, len);
                info->md5[len] = '\0';
            }
        }
    }
    
    // 4. 提取 version
    char *ver_start = strstr(buffer, "\"version\":\"");
    if (ver_start) {
        ver_start += 11; // 跳过 "version":"
        char *ver_end = strstr(ver_start, "\"");
        if (ver_end) {
            uint32_t len = ver_end - ver_start;
            if (len < sizeof(info->version)) {
                memcpy(info->version, ver_start, len);
                info->version[len] = '\0';
            }
        }
    }
}

/******************************   OTA升级相关   ************************************************/

//Flash 擦除和写入函数

void flash_erase_app2_partition(void) {
    HAL_FLASH_Unlock();
    for (uint32_t addr = APP2_ADDRESS; addr < APP2_ADDRESS + APP2_SIZE; addr += 2048) {
        FLASH_EraseInitTypeDef erase;
        erase.TypeErase = FLASH_TYPEERASE_PAGES;
        erase.PageAddress = addr;
        erase.NbPages = 1;
        uint32_t page_err = 0;
        HAL_FLASHEx_Erase(&erase, &page_err);
        if (page_err != 0xFFFFFFFF) {
            printf("OTA: Erase error at 0x%08X\r\n", addr);
        }
    }
    HAL_FLASH_Lock();
    printf("OTA: APP2 partition erased\r\n");
}

void write_to_app2_flash(uint8_t *data, uint32_t len) {
  
    if (g_flash_written  + len > APP2_SIZE) {
        printf("OTA: Write exceeds APP2 size!\r\n");
        return;
    }
    
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t word = 0;
        if (i + 4 <= len) {
            memcpy(&word, data + i, 4);
        } else {
            memcpy(&word, data + i, len - i);
        }
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, g_flash_current_addr , word);
        g_flash_current_addr  += 4;
        g_flash_written  += 4;
    }
    HAL_FLASH_Lock();
}

//比较版本
 int version_compare(const char *new_version, const char *current_version)
{
    unsigned int new_major = 0;
    unsigned int new_minor = 0;
    unsigned int new_patch = 0;
    unsigned int cur_major = 0;
    unsigned int cur_minor = 0;
    unsigned int cur_patch = 0;

    sscanf(new_version, "%u.%u.%u",
           &new_major, &new_minor, &new_patch);

    sscanf(current_version, "%u.%u.%u",
           &cur_major, &cur_minor, &cur_patch);

    if (new_major != cur_major)
        return new_major > cur_major ? 1 : -1;

    if (new_minor != cur_minor)
        return new_minor > cur_minor ? 1 : -1;

    if (new_patch != cur_patch)
        return new_patch > cur_patch ? 1 : -1;

    return 0;
}

/**
 * @brief 解析 HTTP URL，提取主机、路径和端口
 * @param url  完整 URL，如 "http://example.com/path/file.bin"
 * @param info 输出结构体，包含 host、path、port
 * @return 1 成功，0 失败
 */
uint8_t parse_url(const char *url, url_info_t *info) {
    memset(info, 0, sizeof(url_info_t));
    info->port = 80;  // 默认 HTTP 端口

    // 跳过协议前缀
    if (strstr(url, "http://") == url) {
        url += 7;
    } else if (strstr(url, "https://") == url) {
        url += 8;
        info->port = 443;  // 但我们后续会强制改为 80
    } else {
        return 0;  // 无效 URL
    }

    // 查找第一个 '/' 分割 host 和 path
    char *path_start = strstr(url, "/");
    if (!path_start) {
        return 0;
    }
    uint32_t host_len = path_start - url;
    if (host_len >= sizeof(info->host)) {
        return 0;
    }
    memcpy(info->host, url, host_len);
    info->host[host_len] = '\0';

    // 复制路径（包括查询参数）
    strncpy(info->path, path_start, sizeof(info->path) - 1);
    info->path[sizeof(info->path) - 1] = '\0';

    return 1;
}

//设置升级标志
void set_update_flag(void) {
    Flash_Erase_Page(FLAG_ADDRESS);
    Flash_Write_Word(FLAG_ADDRESS, OTA_FLAG);
    printf("OTA: Update flag set\r\n");
}
	
#define OTA_RETRY_MAX          3U
#define OTA_STREAM_TIMEOUT_MS  10000U
#define OTA_FLASH_BLOCK_SIZE   512U
#define OTA_HEADER_SIZE        1024U

typedef struct {
    uint32_t state[4];
    uint64_t bit_count;
    uint8_t buffer[64];
} ota_md5_context_t;

#define MD5_F(x,y,z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x,y,z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x,y,z) ((x) ^ (y) ^ (z))
#define MD5_I(x,y,z) ((y) ^ ((x) | ~(z)))
#define MD5_ROTL(x,n) (((x) << (n)) | ((x) >> (32U - (n))))
#define MD5_STEP(f,a,b,c,d,x,s,k) do { (a) += f((b),(c),(d)) + (x) + (k); (a) = MD5_ROTL((a),(s)); (a) += (b); } while (0)

static void ota_md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t x[16], a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0U; i < 16U; i++) {
        x[i] = (uint32_t)block[i * 4U] | ((uint32_t)block[i * 4U + 1U] << 8) |
               ((uint32_t)block[i * 4U + 2U] << 16) | ((uint32_t)block[i * 4U + 3U] << 24);
    }
    MD5_STEP(MD5_F,a,b,c,d,x[ 0], 7,0xd76aa478U); MD5_STEP(MD5_F,d,a,b,c,x[ 1],12,0xe8c7b756U);
    MD5_STEP(MD5_F,c,d,a,b,x[ 2],17,0x242070dbU); MD5_STEP(MD5_F,b,c,d,a,x[ 3],22,0xc1bdceeeU);
    MD5_STEP(MD5_F,a,b,c,d,x[ 4], 7,0xf57c0fafU); MD5_STEP(MD5_F,d,a,b,c,x[ 5],12,0x4787c62aU);
    MD5_STEP(MD5_F,c,d,a,b,x[ 6],17,0xa8304613U); MD5_STEP(MD5_F,b,c,d,a,x[ 7],22,0xfd469501U);
    MD5_STEP(MD5_F,a,b,c,d,x[ 8], 7,0x698098d8U); MD5_STEP(MD5_F,d,a,b,c,x[ 9],12,0x8b44f7afU);
    MD5_STEP(MD5_F,c,d,a,b,x[10],17,0xffff5bb1U); MD5_STEP(MD5_F,b,c,d,a,x[11],22,0x895cd7beU);
    MD5_STEP(MD5_F,a,b,c,d,x[12], 7,0x6b901122U); MD5_STEP(MD5_F,d,a,b,c,x[13],12,0xfd987193U);
    MD5_STEP(MD5_F,c,d,a,b,x[14],17,0xa679438eU); MD5_STEP(MD5_F,b,c,d,a,x[15],22,0x49b40821U);
    MD5_STEP(MD5_G,a,b,c,d,x[ 1], 5,0xf61e2562U); MD5_STEP(MD5_G,d,a,b,c,x[ 6], 9,0xc040b340U);
    MD5_STEP(MD5_G,c,d,a,b,x[11],14,0x265e5a51U); MD5_STEP(MD5_G,b,c,d,a,x[ 0],20,0xe9b6c7aaU);
    MD5_STEP(MD5_G,a,b,c,d,x[ 5], 5,0xd62f105dU); MD5_STEP(MD5_G,d,a,b,c,x[10], 9,0x02441453U);
    MD5_STEP(MD5_G,c,d,a,b,x[15],14,0xd8a1e681U); MD5_STEP(MD5_G,b,c,d,a,x[ 4],20,0xe7d3fbc8U);
    MD5_STEP(MD5_G,a,b,c,d,x[ 9], 5,0x21e1cde6U); MD5_STEP(MD5_G,d,a,b,c,x[14], 9,0xc33707d6U);
    MD5_STEP(MD5_G,c,d,a,b,x[ 3],14,0xf4d50d87U); MD5_STEP(MD5_G,b,c,d,a,x[ 8],20,0x455a14edU);
    MD5_STEP(MD5_G,a,b,c,d,x[13], 5,0xa9e3e905U); MD5_STEP(MD5_G,d,a,b,c,x[ 2], 9,0xfcefa3f8U);
    MD5_STEP(MD5_G,c,d,a,b,x[ 7],14,0x676f02d9U); MD5_STEP(MD5_G,b,c,d,a,x[12],20,0x8d2a4c8aU);
    MD5_STEP(MD5_H,a,b,c,d,x[ 5], 4,0xfffa3942U); MD5_STEP(MD5_H,d,a,b,c,x[ 8],11,0x8771f681U);
    MD5_STEP(MD5_H,c,d,a,b,x[11],16,0x6d9d6122U); MD5_STEP(MD5_H,b,c,d,a,x[14],23,0xfde5380cU);
    MD5_STEP(MD5_H,a,b,c,d,x[ 1], 4,0xa4beea44U); MD5_STEP(MD5_H,d,a,b,c,x[ 4],11,0x4bdecfa9U);
    MD5_STEP(MD5_H,c,d,a,b,x[ 7],16,0xf6bb4b60U); MD5_STEP(MD5_H,b,c,d,a,x[10],23,0xbebfbc70U);
    MD5_STEP(MD5_H,a,b,c,d,x[13], 4,0x289b7ec6U); MD5_STEP(MD5_H,d,a,b,c,x[ 0],11,0xeaa127faU);
    MD5_STEP(MD5_H,c,d,a,b,x[ 3],16,0xd4ef3085U); MD5_STEP(MD5_H,b,c,d,a,x[ 6],23,0x04881d05U);
    MD5_STEP(MD5_H,a,b,c,d,x[ 9], 4,0xd9d4d039U); MD5_STEP(MD5_H,d,a,b,c,x[12],11,0xe6db99e5U);
    MD5_STEP(MD5_H,c,d,a,b,x[15],16,0x1fa27cf8U); MD5_STEP(MD5_H,b,c,d,a,x[ 2],23,0xc4ac5665U);
    MD5_STEP(MD5_I,a,b,c,d,x[ 0], 6,0xf4292244U); MD5_STEP(MD5_I,d,a,b,c,x[ 7],10,0x432aff97U);
    MD5_STEP(MD5_I,c,d,a,b,x[14],15,0xab9423a7U); MD5_STEP(MD5_I,b,c,d,a,x[ 5],21,0xfc93a039U);
    MD5_STEP(MD5_I,a,b,c,d,x[12], 6,0x655b59c3U); MD5_STEP(MD5_I,d,a,b,c,x[ 3],10,0x8f0ccc92U);
    MD5_STEP(MD5_I,c,d,a,b,x[10],15,0xffeff47dU); MD5_STEP(MD5_I,b,c,d,a,x[ 1],21,0x85845dd1U);
    MD5_STEP(MD5_I,a,b,c,d,x[ 8], 6,0x6fa87e4fU); MD5_STEP(MD5_I,d,a,b,c,x[15],10,0xfe2ce6e0U);
    MD5_STEP(MD5_I,c,d,a,b,x[ 6],15,0xa3014314U); MD5_STEP(MD5_I,b,c,d,a,x[13],21,0x4e0811a1U);
    MD5_STEP(MD5_I,a,b,c,d,x[ 4], 6,0xf7537e82U); MD5_STEP(MD5_I,d,a,b,c,x[11],10,0xbd3af235U);
    MD5_STEP(MD5_I,c,d,a,b,x[ 2],15,0x2ad7d2bbU); MD5_STEP(MD5_I,b,c,d,a,x[ 9],21,0xeb86d391U);
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void ota_md5_init(ota_md5_context_t *ctx)
{
    ctx->state[0] = 0x67452301U; ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU; ctx->state[3] = 0x10325476U;
    ctx->bit_count = 0U;
}

static void ota_md5_update(ota_md5_context_t *ctx, const uint8_t *data, uint32_t len)
{
    uint32_t index = (uint32_t)((ctx->bit_count >> 3) & 63U);
    ctx->bit_count += (uint64_t)len << 3;
    while (len > 0U) {
        uint32_t copy = 64U - index;
        if (copy > len) copy = len;
        memcpy(&ctx->buffer[index], data, copy);
        index += copy; data += copy; len -= copy;
        if (index == 64U) { ota_md5_transform(ctx->state, ctx->buffer); index = 0U; }
    }
}

static void ota_md5_final(ota_md5_context_t *ctx, uint8_t digest[16])
{
    uint8_t padding[64] = {0x80U};
    uint8_t length[8];
    uint64_t bits = ctx->bit_count;
    uint32_t index = (uint32_t)((bits >> 3) & 63U);
    for (uint32_t i = 0U; i < 8U; i++) length[i] = (uint8_t)(bits >> (i * 8U));
    ota_md5_update(ctx, padding, (index < 56U) ? (56U - index) : (120U - index));
    ota_md5_update(ctx, length, 8U);
    for (uint32_t i = 0U; i < 4U; i++) {
        digest[i * 4U] = (uint8_t)ctx->state[i]; digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 16); digest[i * 4U + 3U] = (uint8_t)(ctx->state[i] >> 24);
    }
}

static int ota_md5_matches(const uint8_t digest[16], const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    char actual[33];
    if (expected == NULL || strlen(expected) != 32U) return 0;
    for (uint32_t i = 0U; i < 16U; i++) {
        actual[i * 2U] = hex[digest[i] >> 4]; actual[i * 2U + 1U] = hex[digest[i] & 0x0FU];
    }
    actual[32] = '\0';
    for (uint32_t i = 0U; i < 32U; i++) {
        if (actual[i] != (char)tolower((unsigned char)expected[i])) return 0;
    }
    return 1;
}

/* Returns the current producer position in the circular DMA buffer. */
static uint16_t ota_dma_write_pos(void)
{
    return (uint16_t)(RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx));
}

static void ota_stream_begin(void)
{
    taskENTER_CRITICAL();
    ota_rx_mode = 1U;
    dma_read_pos = ota_dma_write_pos();
    mqtt_dma_pos = dma_read_pos;
    taskEXIT_CRITICAL();
	printf("OTA: sensor/MQTT publishing suspended\r\n");
}

static void ota_stream_end(void)
{
    taskENTER_CRITICAL();
    mqtt_dma_pos = ota_dma_write_pos();
    dma_read_pos = mqtt_dma_pos;
    ota_rx_mode = 0U;
    rx_parse_len = 0U;
    taskEXIT_CRITICAL();
	printf("OTA: sensor/MQTT publishing resumed\r\n");
}

/* Read one byte without stopping DMA.  A 2 KB ring provides about 178 ms of
 * buffering at 115200 baud, enough to absorb STM32F1 Flash programming stalls. */
static int ota_stream_get_byte(uint8_t *value, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        uint16_t write_pos = ota_dma_write_pos();
        if (dma_read_pos != write_pos) {
            *value = usart3_rx_buffer[dma_read_pos];
            dma_read_pos++;
            if (dma_read_pos >= RX_BUF_SIZE) dma_read_pos = 0U;
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return 0;
}

static int ota_stream_wait_for(const char *expected, uint32_t timeout_ms)
{
    char response[256];
    size_t used = 0U;
    uint8_t ch;
    uint32_t start = HAL_GetTick();

    response[0] = '\0';
    while ((HAL_GetTick() - start) < timeout_ms) {
        uint32_t remain = timeout_ms - (HAL_GetTick() - start);
        if (!ota_stream_get_byte(&ch, remain)) break;

        if (used < sizeof(response) - 1U) {
            response[used++] = (char)ch;
        } else {
            memmove(response, response + 1, sizeof(response) - 2U);
            response[sizeof(response) - 2U] = (char)ch;
            used = sizeof(response) - 1U;
        }
        response[used] = '\0';

        if (strstr(response, expected) != NULL) return 1;
        if (strstr(response, "ERROR") != NULL || strstr(response, "FAIL") != NULL) {
            printf("OTA: ESP8266 response error: %s\r\n", response);
            return 0;
        }
    }
    printf("OTA: timeout waiting for %s\r\n", expected);
    return 0;
}

static int ota_send_at_wait(const char *cmd, const char *expected, uint32_t timeout_ms)
{
    char send_buf[256];
    int len = snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd);
    if (len <= 0 || len >= (int)sizeof(send_buf)) return 0;
    if (HAL_UART_Transmit(&huart3, (uint8_t *)send_buf, (uint16_t)len, 1000U) != HAL_OK) return 0;
    return ota_stream_wait_for(expected, timeout_ms);
}

static int ota_flash_write(uint32_t address, const uint8_t *data, uint32_t len)
{
    uint8_t aligned[OTA_FLASH_BLOCK_SIZE];
    uint32_t padded = (len + 3U) & ~3U;

    if (padded > sizeof(aligned) || address < APP2_ADDRESS ||
        address + padded > APP2_ADDRESS + APP2_SIZE) return 0;

    memset(aligned, 0xFF, padded);
    memcpy(aligned, data, len);
    HAL_FLASH_Unlock();
    for (uint32_t i = 0U; i < padded; i += 4U) {
        uint32_t word;
        memcpy(&word, &aligned[i], sizeof(word));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + i, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
    }
    HAL_FLASH_Lock();
    return 1;
}

static int ota_http_download(const url_info_t *url_info, uint32_t announced_size, const char *expected_md5)
{
    char command[160];
    char request[512];
    uint8_t header[OTA_HEADER_SIZE];
    uint8_t block[OTA_FLASH_BLOCK_SIZE];
    uint32_t header_len = 0U;
    uint32_t content_length;
    uint32_t received = 0U;
    uint32_t flash_addr = APP2_ADDRESS;
    uint8_t ch;
    int result = 0;
    ota_md5_context_t md5;
    uint8_t digest[16];

    ota_stream_begin();

    /* Erase before opening TCP; otherwise the HTTP body can overflow the ring
     * while the STM32F1 is busy erasing roughly 120 Flash pages. */
    flash_erase_app2_partition();

    if (!ota_send_at_wait("AT+CIPMUX=0", "OK", 2000U)) goto cleanup;
    if (!ota_send_at_wait("AT+CIPMODE=1", "OK", 2000U)) goto cleanup;

    snprintf(command, sizeof(command), "AT+CIPSTART=\"TCP\",\"%s\",%u",
             url_info->host, (unsigned)url_info->port);
    if (!ota_send_at_wait(command, "CONNECT", 10000U)) goto cleanup;
    if (!ota_send_at_wait("AT+CIPSEND", ">", 3000U)) goto cleanup;

    int request_len = snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: STM32-OTA\r\n"
             "Accept: application/octet-stream\r\n"
             "Accept-Encoding: identity\r\n"
             "Cache-Control: no-cache\r\n"
             "Connection: close\r\n\r\n",
             url_info->path, url_info->host);
    if (request_len <= 0 || request_len >= (int)sizeof(request) ||
        HAL_UART_Transmit(&huart3, (uint8_t *)request, (uint16_t)request_len, 1000U) != HAL_OK) {
        goto cleanup;
    }

    while (header_len < sizeof(header) - 1U) {
        if (!ota_stream_get_byte(&ch, OTA_STREAM_TIMEOUT_MS)) {
            printf("OTA: HTTP header timeout\r\n");
            goto cleanup;
        }
        header[header_len++] = ch;
        if (header_len >= 4U && memcmp(&header[header_len - 4U], "\r\n\r\n", 4U) == 0) break;
    }
    if (header_len >= sizeof(header) - 1U) {
        printf("OTA: HTTP header too large\r\n");
        goto cleanup;
    }
    header[header_len] = '\0';

    if (strstr((char *)header, "HTTP/1.1 200") == NULL &&
        strstr((char *)header, "HTTP/1.0 200") == NULL) {
        printf("OTA: HTTP status is not 200\r\n");
        goto cleanup;
    }
    if (strstr((char *)header, "Transfer-Encoding: chunked") != NULL ||
        strstr((char *)header, "transfer-encoding: chunked") != NULL) {
        printf("OTA: chunked response is unsupported\r\n");
        goto cleanup;
    }

    char *length_field = strstr((char *)header, "Content-Length:");
    if (length_field == NULL) length_field = strstr((char *)header, "content-length:");
    if (length_field == NULL) {
        printf("OTA: missing Content-Length\r\n");
        goto cleanup;
    }
    content_length = (uint32_t)strtoul(length_field + 15, NULL, 10);
    if (content_length == 0U || content_length > APP2_SIZE) {
        printf("OTA: invalid firmware size %u\r\n", content_length);
        goto cleanup;
    }
    if (announced_size != 0U && announced_size != content_length) {
        printf("OTA: size mismatch, MQTT=%u HTTP=%u\r\n", announced_size, content_length);
        goto cleanup;
    }

    printf("OTA: receiving %u bytes\r\n", content_length);
    ota_md5_init(&md5);

    while (received < content_length) {
        uint32_t block_len = content_length - received;
        if (block_len > sizeof(block)) block_len = sizeof(block);

        for (uint32_t i = 0U; i < block_len; i++) {
            if (!ota_stream_get_byte(&block[i], OTA_STREAM_TIMEOUT_MS)) {
                printf("OTA: body timeout at %u bytes\r\n", received + i);
                goto cleanup;
            }
        }
        if (!ota_flash_write(flash_addr, block, block_len)) {
            printf("OTA: Flash write failed at 0x%08X\r\n", flash_addr);
            goto cleanup;
        }
        ota_md5_update(&md5, block, block_len);
        flash_addr += (block_len + 3U) & ~3U;
        received += block_len;
        if ((received % 4096U) == 0U || received == content_length) {
            printf("OTA: %u%% (%u/%u)\r\n",
                   (unsigned)((received * 100U) / content_length), received, content_length);
        }
    }
    ota_md5_final(&md5, digest);
    if (!ota_md5_matches(digest, expected_md5)) {
        printf("OTA: MD5 verification failed\r\n");
        goto cleanup;
    }
    printf("OTA: MD5 verified\r\n");
    result = 1;

cleanup:
    /* ESP-AT transparent-mode escape requires guard time before and after +++. */
    vTaskDelay(pdMS_TO_TICKS(1100U));
    HAL_UART_Transmit(&huart3, (uint8_t *)"+++", 3U, 1000U);
    vTaskDelay(pdMS_TO_TICKS(1100U));
    ota_send_at_wait("AT+CIPMODE=0", "OK", 2000U);
    ota_send_at_wait("AT+CIPCLOSE", "OK", 2000U);
    ota_stream_end();
    return result;
}

void ota_download_firmware(const char *url, uint32_t file_size, const char *expected_md5)
{
    url_info_t url_info;
    char http_url[sizeof(((ota_info_t *)0)->url)];
    const char *download_url = url;

    if (url == NULL) {
        printf("OTA: missing download URL\r\n");
        return;
    }

    /* Aliyun sends an https:// URL in the MQTT notification.  This project
     * deliberately downloads through plain TCP/HTTP, so replace only the
     * scheme; host, path, auth_key and every signed query byte stay unchanged. */
    if (strncmp(url, "https://", 8U) == 0) {
        int len = snprintf(http_url, sizeof(http_url), "http://%s", url + 8U);
        if (len <= 0 || len >= (int)sizeof(http_url)) {
            printf("OTA: converted HTTP URL is too long\r\n");
            return;
        }
        download_url = http_url;
        printf("OTA: HTTPS notification URL converted to HTTP transport\r\n");
    } else if (strncmp(url, "http://", 7U) != 0) {
        printf("OTA: unsupported URL scheme\r\n");
        return;
    }
		//提取
    if (!parse_url(download_url, &url_info)) {
        printf("OTA: invalid download URL\r\n");
        return;
    }
    url_info.port = 80U;
		//确保固件size合适
    if (file_size == 0U || file_size > APP2_SIZE) {
        printf("OTA: announced size is invalid: %u\r\n", file_size);
        return;
    }
		//确保md5是有效的
    if (expected_md5 == NULL || strlen(expected_md5) != 32U) {
        printf("OTA: invalid or missing MD5\r\n");
        return;
    }

	/* Hold USART3 exclusively for the complete transparent HTTP transaction.
	 * The successful path resets the MCU; failure releases it below. */
	if (xEsp8266TxMutex != NULL) {
		xSemaphoreTake(xEsp8266TxMutex, portMAX_DELAY);
	}
    for (uint32_t retry = 0U; retry < OTA_RETRY_MAX; retry++) {
        printf("OTA: attempt %u/%u\r\n", (unsigned)(retry + 1U), (unsigned)OTA_RETRY_MAX);
        if (ota_http_download(&url_info, file_size, expected_md5)) {
            uint32_t initial_sp = *(volatile uint32_t *)APP2_ADDRESS;
            uint32_t reset_vector = *(volatile uint32_t *)(APP2_ADDRESS + 4U);
            /* The staged image may be linked for APP1 and later copied there by
             * the bootloader, so validate a Thumb reset vector in MCU Flash,
             * not specifically inside the APP2 staging partition. */
            if ((initial_sp & 0x2FFE0000U) != 0x20000000U ||
                (reset_vector & 1U) == 0U ||
                (reset_vector & ~1U) < 0x08000000U ||
                (reset_vector & ~1U) >= 0x08080000U) {
                printf("OTA: invalid application vector table\r\n");
                if (xEsp8266TxMutex != NULL) {
                    xSemaphoreGive(xEsp8266TxMutex);
                }
                return;
            }
            printf("OTA: download complete\r\n");
            set_update_flag();
			printf("Set update flag complete\r\n");
            HAL_NVIC_SystemReset();
        }
        vTaskDelay(pdMS_TO_TICKS(2000U));
    }
    printf("OTA: all retries failed\r\n");
	if (xEsp8266TxMutex != NULL) xSemaphoreGive(xEsp8266TxMutex);
}


#if 0 /* Old polling implementation: replaced by the DMA stream above. */
// 实际执行下载，返回 0 成功，-1 失败
static int do_ota_download(url_info_t *url_info, uint32_t file_size) {
    printf("OTA: Downloading from: %s:%d%s\r\n", url_info->host, url_info->port, url_info->path);

    // 1. 擦除 APP2 分区
    flash_erase_app2_partition();

    // 2. 设置透传模式
    esp8266_send_at("AT+CIPMUX=0", 500);
    esp8266_send_at("AT+CIPMODE=1", 500);

    // 3. 建立 TCP 连接
    char connect_cmd[128];
    snprintf(connect_cmd, sizeof(connect_cmd),
             "AT+CIPSTART=\"TCP\",\"%s\",%d",
             url_info->host, url_info->port);
    esp8266_send_at(connect_cmd, 5000);

    // 4. 发送 AT+CIPSEND 并等待 '>'
    esp8266_send_at("AT+CIPSEND", 0);
    uint8_t ch;
    uint32_t start = HAL_GetTick();
    int got_prompt = 0;
    while (HAL_GetTick() - start < 3000) {
        if (HAL_UART_Receive(&huart3, &ch, 1, 100) == HAL_OK) {
            if (ch == '>') { got_prompt = 1; break; }
        }
    }
    if (!got_prompt) {
        printf("OTA: No '>' prompt\r\n");
        goto exit_fail;
    }

    // 5. 发送 HTTP GET（添加 no-cache）
    char http_request[512];
    snprintf(http_request, sizeof(http_request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: ESP8266\r\n"
             "Accept: */*\r\n"
             "Cache-Control: no-cache\r\n"
             "Accept-Encoding: identity\r\n"
             "Connection: close\r\n"
             "\r\n",
             url_info->path, url_info->host);
		printf("http_request=: \r\n %s",http_request);
    HAL_UART_Transmit(&huart3, (uint8_t*)http_request, strlen(http_request), 1000);

    // 6. 接收头部
    #define HEADER_BUF_SIZE 1024
    uint8_t header_buf[HEADER_BUF_SIZE];
    int header_idx = 0, header_done = 0;
    uint32_t content_length = file_size;

    while (!header_done) {
        if (HAL_UART_Receive(&huart3, &ch, 1, 5000) == HAL_OK) {
            header_buf[header_idx++] = ch;
            if (header_idx >= 4 &&
                header_buf[header_idx-4] == '\r' &&
                header_buf[header_idx-3] == '\n' &&
                header_buf[header_idx-2] == '\r' &&
                header_buf[header_idx-1] == '\n') {
                header_done = 1;
            }
            if (header_idx >= HEADER_BUF_SIZE-4) {
                printf("OTA: Header overflow\r\n");
                goto exit_fail;
            }
        } else {
            printf("OTA: Header timeout\r\n");
            goto exit_fail;
        }
    }
    header_buf[header_idx] = '\0';
    printf("OTA: Header received\n");

    // 解析 Content-Length
    char *cl = strstr((char*)header_buf, "Content-Length:");
    if (cl) {
        char *end;
        unsigned long len = strtoul(cl + 15, &end, 10);
        if (end != cl + 15) content_length = (uint32_t)len;
    }
    printf("OTA: Content-Length: %u\n", content_length);

    // 检查 chunked
    if (strstr((char*)header_buf, "Transfer-Encoding: chunked")) {
        printf("OTA: Chunked encoding unsupported\r\n");
        goto exit_fail;
    }

    // 7. 接收主体（使用短超时循环）
    #define BUF_SIZE 256
    uint8_t write_buf[BUF_SIZE];
    uint8_t buf_idx = 0;
    uint32_t received = 0;
    uint32_t flash_addr = APP2_ADDRESS;
    uint32_t timeout_cnt = 0;
    const uint32_t MAX_TIMEOUT_CNT = 100; // 10秒

    HAL_FLASH_Unlock();
    while (received < content_length) {
        if (HAL_UART_Receive(&huart3, &ch, 1, OTA_TIMEOUT_MS) == HAL_OK) {
            write_buf[buf_idx++] = ch;
            timeout_cnt = 0;
            if (buf_idx == BUF_SIZE) {
                for (int i = 0; i < BUF_SIZE; i += 4) {
                    uint32_t word = *(uint32_t*)&write_buf[i];
                    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, flash_addr, word) != HAL_OK) {
                        printf("OTA: Flash write error\r\n");
                        HAL_FLASH_Lock();
                        goto exit_fail;
                    }
                    flash_addr += 4;
                }
                received += BUF_SIZE;
                buf_idx = 0;
                if (received % 4096 == 0)
                    printf("OTA: %d%% (%u/%u)\r\n", (received*100)/content_length, received, content_length);
            }
        } else {
            timeout_cnt++;
            if (timeout_cnt >= MAX_TIMEOUT_CNT) {
                printf("OTA: Body timeout at %u bytes\r\n", received);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1)); // 让出CPU
        }
    }

    // 尾部处理
    if (buf_idx > 0) {
        int orig = buf_idx;
        while (buf_idx % 4) write_buf[buf_idx++] = 0;
        for (int i = 0; i < buf_idx; i += 4) {
            uint32_t word = *(uint32_t*)&write_buf[i];
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, flash_addr, word) != HAL_OK) {
                printf("OTA: Flash write error trailing\r\n");
                HAL_FLASH_Lock();
                goto exit_fail;
            }
            flash_addr += 4;
        }
        received += orig;
        printf("OTA: Wrote trailing %d bytes\r\n", orig);
    }
    HAL_FLASH_Lock();

    // 退出透传
exit_fail:
    delay_ms(500);
    HAL_UART_Transmit(&huart3, (uint8_t*)"+++", 3, 1000);
    delay_ms(500);
    esp8266_send_at("AT+CIPMODE=0", 500);
    esp8266_send_at("AT+CIPCLOSE", 500);

    if (received == content_length) {
        printf("OTA: Download success!\r\n");
        set_update_flag();
        HAL_NVIC_SystemReset();
        return 0;
    } else {
        printf("OTA: Incomplete: %u / %u\r\n", received, content_length);
        return -1;
    }
}
//解析域名函数
void ota_download_firmware(const char *url, uint32_t file_size) {
    char http_url[256];
    if (strstr(url, "https://") == url) {
        snprintf(http_url, sizeof(http_url), "http://%s", url + 8);
        url = http_url;
    }

    url_info_t url_info;
    if (!parse_url(url, &url_info)) {
        printf("OTA: Invalid URL\r\n");
        return;
    }
    url_info.port = 80;  // 强制 HTTP

    for (int retry = 0; retry < OTA_RETRY_MAX; retry++) {
        printf("OTA: Attempt %d/%d\n", retry+1, OTA_RETRY_MAX);
        if (do_ota_download(&url_info, file_size) == 0) {
            return; // 成功
        }
        // 失败后稍等再重试
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    printf("OTA: All retries failed.\r\n");
		uart_dma_Enable();
}
#endif



//void ota_download_firmware(const char *url, uint32_t file_size) {
//    url_info_t url_info;
//    if (!   (url, &url_info)) {
//        printf("OTA: Invalid URL\r\n");
//        return;
//    }
//    
//    printf("OTA: Downloading from: %s%s\r\n", url_info.host, url_info.path);
//    printf("OTA: File size: %u bytes\r\n", file_size);
//    
//    // 1. 擦除 APP2 整个分区（按页擦除，每页 2KB）
//    flash_erase_app2_partition();
//    
//    // 2. 设置单连接、透传模式
//    esp8266_send_at("AT+CIPMUX=0", 500);
//    esp8266_send_at("AT+CIPMODE=1", 500);
//    
//    // 3. 建立 TCP 连接
//    char connect_cmd[128];
//    snprintf(connect_cmd, sizeof(connect_cmd),
//             "AT+CIPSTART=\"TCP\",\"%s\",%d",
//             url_info.host, url_info.port);
//    esp8266_send_at(connect_cmd, 5000) ;
//    printf("OTA: TCP connected : %s\r\n",connect_cmd);
//    
//		 // 4. 发送 AT+CIPSEND，并等待 '>' 提示符（必须等待！）
//    esp8266_send_at("AT+CIPSEND", 0);   // 不额外延时
//    uint8_t ch;
//    uint32_t start = HAL_GetTick();
//    int got_prompt = 0;
//    while (HAL_GetTick() - start < 3000) {
//        if (HAL_UART_Receive(&huart3, &ch, 1, 100) == HAL_OK) {
//            if (ch == '>') {
//                got_prompt = 1;
//                break;
//            }
//        }
//    }
//		if (!got_prompt) {
//        printf("OTA: Timeout waiting for '>' prompt\r\n");
//        goto exit;
//    }
//		printf("OTA: Got '>' prompt, sending HTTP request...\r\n");
//		
//    // 4. 进入透传发送模式
//    //  esp8266_send_at("AT+CIPSEND", 500);
//    // 此时 ESP8266 返回 '>' 提示符，后续发送的数据会直接发往网络
//    
//    // 5. 构造并发送 HTTP GET 请求（通过串口直接发送，不经过 AT 解析）
//    char http_request[512];
//    snprintf(http_request, sizeof(http_request),
//             "GET %s HTTP/1.1\r\n"
//             "Host: %s\r\n"
//             "User-Agent: ESP8266\r\n"
//             "Accept: */*\r\n"
//             "Accept-Encoding: identity\r\n"
//             "Connection: close\r\n"
//             "\r\n",
//             url_info.path, url_info.host);
//    
//    // 直接通过 HAL_UART_Transmit 发送（透传模式）
//    HAL_UART_Transmit(&huart3, (uint8_t*)http_request, strlen(http_request), 1000);
//    printf("OTA: HTTP request sent\r\n");
//    
//    // ========== 6. 接收 HTTP 响应（先解析头部，再接收主体） ==========
//    // 6.1 接收头部直到遇到空行 "\r\n\r\n"
//    uint8_t header_buf[1024];
//    int header_idx = 0;
//    int header_done = 0;
//    uint32_t content_length = file_size;   // 默认使用传入值

//    while (!header_done) {
//        if (HAL_UART_Receive(&huart3, &ch, 1, 5000) == HAL_OK) {
//            header_buf[header_idx++] = ch;
//            if (header_idx >= 4 &&
//                header_buf[header_idx - 4] == '\r' &&
//                header_buf[header_idx - 3] == '\n' &&
//                header_buf[header_idx - 2] == '\r' &&
//                header_buf[header_idx - 1] == '\n') {
//                header_done = 1;
//            }
//            if (header_idx >= sizeof(header_buf) - 1) {
//                printf("OTA: Header buffer overflow\r\n");
//                goto exit;
//            }
//        } else {
//            printf("OTA: Receive header timeout\r\n");
//            goto exit;
//        }
//    }
//    header_buf[header_idx] = '\0';   // 安全终止
//    printf("OTA: Header received (%d bytes)\r\n", header_idx);
//		// 可选：打印头部内容（调试用）
//     printf("Header:\n%s\n", header_buf);
//    // 6.2 解析 Content-Length（若存在则覆盖 file_size）
//    char *cl = strstr((char*)header_buf, "Content-Length:");
//    if (cl) {
//        content_length = atoi(cl + 15);
//        printf("OTA: Content-Length: %u\r\n", content_length);
//    } else {
//        printf("OTA: No Content-Length, using file_size %u\r\n", file_size);
//        // 若没有 Content-Length，则只能依赖 Connection: close 或 file_size
//    }

//    // 6.3 开始接收主体（body），按 4 字节对齐写入 Flash
//    uint32_t received = 0;
//    uint32_t flash_addr = APP2_ADDRESS;
//    uint8_t word_buf[4];
//    uint8_t idx = 0;

//    HAL_FLASH_Unlock();
//    while (received < content_length) {
//        if (HAL_UART_Receive(&huart3, &ch, 1, 5000) == HAL_OK) {
//            word_buf[idx++] = ch;
//            if (idx == 4) {
//                uint32_t word = *(uint32_t*)word_buf;
//                if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, flash_addr, word) == HAL_OK) {
//                    flash_addr += 4;
//                    received += 4;
//                    idx = 0;

//                    // 每 4KB 打印一次进度
//                    if (received % 4096 == 0) {
//                        printf("OTA: %d%% (%u/%u)\r\n",
//                               (received * 100) / content_length,
//                               received, content_length);
//                    }
//                } else {
//                    printf("OTA: Flash write error at 0x%08X\r\n", flash_addr);
//                    break;
//                }
//            }
//        } else {
//            printf("OTA: Body receive timeout at %u bytes\r\n", received);
//            break;
//        }
//    }

//    // 6.4 处理尾部不足 4 字节的情况（补 0 后写入）
//    if (idx > 0 && received < content_length) {
//    uint8_t orig_idx = idx;           // 保存原始字节数
//    while (idx < 4) {
//        word_buf[idx++] = 0;
//    }
//    uint32_t word = *(uint32_t*)word_buf;
//    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, flash_addr, word) == HAL_OK) {
//        received += orig_idx;          // 只增加实际接收的字节数
//        printf("OTA: Wrote trailing %d bytes (padded)\r\n", orig_idx);
//    } else {
//        printf("OTA: Flash write error at trailing bytes\r\n");
//    }
//	}
//    HAL_FLASH_Lock();

//    // 7. 退出透传模式
//exit:
//    delay_ms(500);
//    HAL_UART_Transmit(&huart3, (uint8_t*)"+++", 3, 1000);
//    delay_ms(500);
//    esp8266_send_at("AT+CIPMODE=0", 500);
//    esp8266_send_at("AT+CIPCLOSE", 500);

//    // 8. 检查结果
//    if (received == content_length) {
//        printf("OTA: Download success! (%u bytes)\r\n", received);
//        set_update_flag();
//        HAL_NVIC_SystemReset();   // 软复位
//    } else {
//        printf("OTA: Download incomplete: %u / %u\r\n", received, content_length);
//    }
//}
//	

	
	
	

