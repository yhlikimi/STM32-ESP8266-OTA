
/*
日志： 
			
	1、设置对应的版本  OTA_VERSION （esp8266.h）
	2、Bootloader 虽然成功把固件从 APP2 复制到了 APP1，但固件向量表中的复位入口仍指向 APP2。因此 APP1 校验失败。
	  因此设置的ROM1向量偏移应该按照App1来进行设置！！！即都配置为：IROM1: 0x08007800, Size: 0x3C000 (240k)；
	3、阿里云上的固件号设置与要与 OTA_VERSION，且固件好要大于当前版本才会更新 
	4、设置user->: fromelf --bin --output ./app.bin ./test_led/*.axf ,生成bin文件
	*/	
 /***********************************************************************************/ /*
 
	Bootloader:   IROM1: 0x08000000, Size: 0x7800  (30k)
	App1      :   IROM1: 0x08007800, Size: 0x3C000 (240k)
	App2      :   IROM1: 0x08043800, Size: 0x3C000 (240k) //App2用来存放通过OTA下载的新固件	
	
 总大小：0x80000  512k				
*/


#include "main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOSConfig.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "led.h"
#include "usart.h"
#include "lcd.h"
#include "touch.h"
#include "beep.h"
#include "key.h"
#include "dht11.h"
#include "esp8266.h"

#include "lvgl.h"
#include "lv_port_disp_template.h"
#include "lv_port_indev_template.h"




UART_HandleTypeDef huart1;
IWDG_HandleTypeDef hiwdg;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void IWDG_Init(void);
static void Watchdog_PrintResetCause(void);

#define WATCHDOG_TASK_PRIO       3
#define WATCHDOG_TASK_STACK_SIZE 128
#define WATCHDOG_CHECK_MS        1000U
#define WATCHDOG_STARTUP_GRACE_MS 5000U
#define WATCHDOG_FAST_TASK_MAX_MS 3000U
#define WATCHDOG_DHT_MAX_MS       5000U
#define WATCHDOG_REPORT_MAX_MS   12000U
	
#define LED_TASK_PRIO       2
#define UART_TASK_PRIO      2
#define LVGL_TASK_PRIO      (2) 
#define DHT11_TASK_PRIO     (2) 
#define KEY_TASK_PRIO       2

#define LED_TASK_STK_SIZE   256
#define UART_TASK_STK_SIZE  256
#define LVGL_TASK_STACK_SIZE   (2048)  
#define DHT11_TASK_STACK_SIZE  256 
#define KEY_TASK_STACK_SIZE   256



TaskHandle_t LED0_Task_Handler;
TaskHandle_t LED1_Task_Handler;
TaskHandle_t UART_Task_Handler;
TaskHandle_t LVGL_TASK_Handler ;
TaskHandle_t DHT11_TASK_Handler ;
TaskHandle_t KEY_TASK_Handler;


QueueHandle_t UART_Queue_Handler;
//声明任务函数
void LED0_Task(void *pvParameters);
void LED1_Task(void *pvParameters);
void UART_Task(void *pvParameters);
void LVGL_Task(void *pvParameters);
void DHT11_Task(void *pvParameters);
void KEY_Task(void *pvParameters);
static void Watchdog_Task(void *pvParameters);

//void vStartupTask(void *pvParameters);
void vESP8266_ReceiveTask(void *pvParameters);
void vDataReportTask(void *pvParameters);

// uart buffer
uint8_t UART_RX_BUF[64];
uint8_t UART_RX_INDEX = 0;

uint8_t uart_rx_byte = 0; 
uint8_t x = 0;
uint8_t lcd_id[12];

// save data for dht11 : tempature and humidity
uint32_t temperature=20;
uint32_t humidity=20;
//温度阈值
uint32_t g_temp_threshold=50 ;
uint8_t beep_enable=0;
uint8_t esp_ok;

static volatile uint8_t g_mqtt_connected = 0U;
static volatile uint8_t g_version_reported = 0U;
static volatile uint8_t g_time_synced = 0U;
static volatile uint8_t g_time_request_pending = 0U;
static TickType_t g_time_request_tick = 0U;
static uint64_t g_unix_time_base_ms = 0U;
static TickType_t g_time_tick_base = 0U;

SemaphoreHandle_t xDHT11Mutex;

static lv_obj_t *time_label;
static lv_obj_t *status_label;
static lv_obj_t *temperature_label;
static lv_obj_t *humidity_label;
static lv_obj_t *threshold_label;
static lv_obj_t *alarm_lamp;
static lv_obj_t *alarm_label;
static lv_obj_t *sync_status_label;
static void create_test_ui(void);
static void update_dashboard(void);
static uint8_t parse_cloud_time_response(const char *message);
static void request_cloud_time_if_needed(void);
void btn_event_cb(lv_event_t *e);

//IWDG 任务心跳
#define WDG_HEARTBEAT_DHT       (1U << 0)
#define WDG_HEARTBEAT_KEY       (1U << 1)
#define WDG_HEARTBEAT_LVGL      (1U << 2)
#define WDG_HEARTBEAT_REPORT    (1U << 3)
static volatile TickType_t g_wdg_last_dht_tick;
static volatile TickType_t g_wdg_last_key_tick;
static volatile TickType_t g_wdg_last_lvgl_tick;
static volatile TickType_t g_wdg_last_report_tick;

static void Watchdog_Heartbeat(uint32_t heartbeat)
{
    TickType_t now = xTaskGetTickCount();

    if ((heartbeat & WDG_HEARTBEAT_DHT) != 0U) {
        g_wdg_last_dht_tick = now;
    }
    if ((heartbeat & WDG_HEARTBEAT_KEY) != 0U) {
        g_wdg_last_key_tick = now;
    }
    if ((heartbeat & WDG_HEARTBEAT_LVGL) != 0U) {
        g_wdg_last_lvgl_tick = now;
    }
    if ((heartbeat & WDG_HEARTBEAT_REPORT) != 0U) {
        g_wdg_last_report_tick = now;
    }
}

	
int main(void)
{

  HAL_Init();
	//App1 
	SCB->VTOR = APP1_ADDRESS;    // 0x08007800
	
	SystemClock_Config();
	LED_Init();
	//huart1.Instance = USART_UX; 
	usart_gpioaf_init(&huart1);  
	usart_init(115200 ); 
	Watchdog_PrintResetCause();
	beep_Init();
	lcd_init();    
	key_init();  
//触摸屏初始化	
	tp_dev.init();                       
	lcd_clear(BLUE);
	HAL_Delay(1000);
	lcd_fill(0, 0, 100, 100, RED);	
	
	printf("app1\r\n");
	while (dht11_init());		
	
	printf("initial lvgl... \r\n");
	lv_init(); 
	lv_port_disp_init();
	lv_port_indev_init(); 
	printf("lvgl initial done!\r\n");
	
	printf("initial esp8266... \r\n");
	esp8266_MspInit();
	/* STM32复位时ESP8266可能仍停留在HTTP透传模式 */
	esp8266_exit_transparent_mode();
	esp_ok = esp8266_init();
	if (esp_ok) {
			printf("esp8266 initial done!\r\n");
			g_mqtt_connected = 1U;
	} else {
			printf("esp8266 initial failed!\r\n");
	}
	printf("esp8266 initial done!\r\n");
	
	
	
	xDHT11Mutex = xSemaphoreCreateMutex();
	xMqttDataSemaphore = xSemaphoreCreateBinary();
	xEsp8266TxMutex = xSemaphoreCreateMutex();
	if (esp_ok) {
		esp8266_report_version(OTA_VERSION);
		g_version_reported = 1U;
	}
/*********************** create tasks *******************************/

	BaseType_t ret;

//xTaskCreate((TaskFunction_t)vStartupTask, "Startup", 128, NULL, 2, NULL);	

//处理接收的esp8266接收的数据
ret=xTaskCreate(
        vESP8266_ReceiveTask,   // 任务函数
        "ESP8266_RX",           // 任务名称
        2048,                   // OTA头部、Flash分块和MD5校验需要更大任务栈
        NULL,                   // 参数
        2,                      // 优先级（低于启动任务的3）
        NULL                    // 任务句柄
    );	
printf("ESP8266_RX create: %d\r\n", ret);

// esp8266数据上报任务
xTaskCreate(vDataReportTask, "Report", 512, NULL, 2, NULL);

//按键扫描任务	

xTaskCreate((TaskFunction_t)KEY_Task,
                (const char *)"KEY_Task",
                (uint16_t)KEY_TASK_STACK_SIZE,
                (void *)NULL,
                (UBaseType_t)KEY_TASK_PRIO,
                (TaskHandle_t *)&KEY_TASK_Handler);	
	
//温湿度采集任务	
xTaskCreate((TaskFunction_t)DHT11_Task,
                (const char *)"DHT11_Task",
                (uint16_t)DHT11_TASK_STACK_SIZE,
                (void *)NULL,
                (UBaseType_t)DHT11_TASK_PRIO,
                (TaskHandle_t *)&DHT11_TASK_Handler);	
	
	
ret=xTaskCreate((TaskFunction_t)LED0_Task,
                (const char *)"LED0_Task",
                (uint16_t)LED_TASK_STK_SIZE,
                (void *)NULL,
                (UBaseType_t)LED_TASK_PRIO,
                (TaskHandle_t *)&LED0_Task_Handler);
								printf("LED0 create: %d\r\n", ret);

xTaskCreate((TaskFunction_t)LED1_Task,
                (const char *)"LED1_Task",
                (uint16_t)LED_TASK_STK_SIZE,
                (void *)NULL,
                (UBaseType_t)LED_TASK_PRIO,
                (TaskHandle_t *)&LED1_Task_Handler);														
								
//串口打印任务		
/*								
 xTaskCreate((TaskFunction_t)UART_Task,
                (const char *)"UART_Task",
                (uint16_t)UART_TASK_STK_SIZE,
                (void *)NULL,
                (UBaseType_t)UART_TASK_PRIO,
                (TaskHandle_t *)&UART_Task_Handler);
								
*/
// LVGL显示任务		
								
 BaseType_t lvgltask_ret=xTaskCreate((TaskFunction_t)LVGL_Task,
                (const char *)"LVGL_Task",
                (uint16_t)LVGL_TASK_STACK_SIZE,
                (void *)NULL,
                (UBaseType_t)LVGL_TASK_PRIO,
                (TaskHandle_t *)&LVGL_TASK_Handler);

    printf("lvgltask_ret=%d",(int)lvgltask_ret);

//创建看门狗任务
	ret = xTaskCreate(Watchdog_Task,
				  "Watchdog",
				  WATCHDOG_TASK_STACK_SIZE,
				  NULL,
				  WATCHDOG_TASK_PRIO,
				  NULL);
	printf("Watchdog create: %d\r\n", ret);
								
	if (ret != pdPASS) {
		Error_Handler();
	}

								
		printf("Free heap before scheduler: %d\r\n", xPortGetFreeHeapSize());	
		printf("create tasks done! \r\n");

		/* Start IWDG only after all peripherals and RTOS objects are ready.
		 * 40 kHz LSI / 256 / (4095 + 1) gives about 26.2 s nominal timeout. */
		IWDG_Init();
		printf("IWDG started: check=%lu ms, timeout about 26 s\r\n",
		       (unsigned long)WATCHDOG_CHECK_MS);
								
		vTaskStartScheduler();
								
	
  while (1)
  {
    
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};


  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}


static void MX_USART1_UART_Init(void)
{

  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }

}


static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_RESET);

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5|GPIO_PIN_8, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
	HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

}


static void IWDG_Init(void)
{
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
    hiwdg.Init.Reload = 4095;

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK) {
        Error_Handler();
    }
}

static void Watchdog_PrintResetCause(void)
{
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) {
        printf("Reset cause: IWDG timeout\r\n");
    }
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

static void Watchdog_Task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t now;
    TickType_t dht_age;
    TickType_t key_age;
    TickType_t lvgl_age;
    TickType_t report_age;

    (void)pvParameters;

    /* Give every monitored task enough time to start and report once. */
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(WATCHDOG_STARTUP_GRACE_MS));

    for (;;) {
        now = xTaskGetTickCount();
        dht_age = now - g_wdg_last_dht_tick;
        key_age = now - g_wdg_last_key_tick;
        lvgl_age = now - g_wdg_last_lvgl_tick;
        report_age = now - g_wdg_last_report_tick;

        if (dht_age <= pdMS_TO_TICKS(WATCHDOG_DHT_MAX_MS) &&
            key_age <= pdMS_TO_TICKS(WATCHDOG_FAST_TASK_MAX_MS) &&
            lvgl_age <= pdMS_TO_TICKS(WATCHDOG_FAST_TASK_MAX_MS) &&
            report_age <= pdMS_TO_TICKS(WATCHDOG_REPORT_MAX_MS)) {
            HAL_IWDG_Refresh(&hiwdg);
        } else {
            printf("IWDG: stale task, age(ms) DHT=%lu KEY=%lu LVGL=%lu REPORT=%lu\r\n",
                   (unsigned long)(dht_age * portTICK_PERIOD_MS),
                   (unsigned long)(key_age * portTICK_PERIOD_MS),
                   (unsigned long)(lvgl_age * portTICK_PERIOD_MS),
                   (unsigned long)(report_age * portTICK_PERIOD_MS));

            /* Do not feed again after a health failure.  IWDG will reset the
             * MCU and the bootloader will enter APP1 normally. */
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000U));
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(WATCHDOG_CHECK_MS));
    }
}

/*************  TASK Founction  ******************/


// 上报数据
void vDataReportTask(void *pvParameters)
{
		uint32_t report_temperature;
		uint32_t report_humidity;

    while (1)
    {
		/* esp8266_init() may consume the first +MQTTCONNECTED notification
		 * before this receive task starts.  Check the connection state here as
		 * a fallback so the first cloud-time request is never missed. */
		request_cloud_time_if_needed();

				if(xSemaphoreTake(xDHT11Mutex, portMAX_DELAY) == pdTRUE){
					report_temperature = temperature;
					report_humidity = humidity;
					xSemaphoreGive(xDHT11Mutex);
				esp8266_publish_data(report_temperature, report_humidity);
        }
		Watchdog_Heartbeat(WDG_HEARTBEAT_REPORT);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static uint8_t parse_u64_json_field(const char *message, const char *field,
                                    uint64_t *value)
{
    const char *p = strstr(message, field);
    uint64_t result = 0U;
    uint8_t digits = 0U;

    if (p == NULL) return 0U;
    p = strchr(p, ':');
    if (p == NULL) return 0U;
    p++;
    while (*p == ' ' || *p == '\"') p++;
    while (*p >= '0' && *p <= '9') {
        result = result * 10ULL + (uint64_t)(*p - '0');
        digits++;
        p++;
    }
    if (digits == 0U) return 0U;
    *value = result;
    return 1U;
}

//解析时间
static uint8_t parse_cloud_time_response(const char *message)
{
    uint64_t device_send_ms;
    uint64_t server_recv_ms;
    uint64_t server_send_ms;
    uint32_t elapsed_ms;
    TickType_t recv_tick;

    if (strstr(message, "/ext/ntp/") == NULL ||
        strstr(message, "/response") == NULL) {
        return 0U;
    }
    if (!parse_u64_json_field(message, "deviceSendTime", &device_send_ms) ||
        !parse_u64_json_field(message, "serverRecvTime", &server_recv_ms) ||
        !parse_u64_json_field(message, "serverSendTime", &server_send_ms)) {
        printf("Cloud time response parse failed\r\n");
        return 1U;
    }

    recv_tick = xTaskGetTickCount();
    elapsed_ms = (uint32_t)(recv_tick - (TickType_t)device_send_ms);
    taskENTER_CRITICAL();
    g_unix_time_base_ms =
        (server_recv_ms + server_send_ms + (uint64_t)elapsed_ms) / 2ULL;
    g_time_tick_base = recv_tick;
    g_time_synced = 1U;
    g_time_request_pending = 0U;
    taskEXIT_CRITICAL();

    printf("Cloud time synchronized\r\n");
    return 1U;
}

static void request_cloud_time_if_needed(void)
{
    TickType_t now = xTaskGetTickCount();
    uint8_t should_request = 0U;

    taskENTER_CRITICAL();
    if (g_mqtt_connected && !g_time_synced) {
        if (!g_time_request_pending ||
            (now - g_time_request_tick) >= pdMS_TO_TICKS(15000U)) {
            /* Reserve this request before leaving the critical section so
             * the receive and report tasks cannot send it simultaneously. */
            g_time_request_pending = 1U;
            g_time_request_tick = now;
            should_request = 1U;
        }
    }
    taskEXIT_CRITICAL();

    if (!should_request) {
        return;
    }

    if (esp8266_request_cloud_time((uint32_t)now)) {
        printf("Cloud time request sent automatically\r\n");
    } else {
        /* ESP8266 may temporarily be occupied by MQTT/OTA traffic.  Clear
         * pending so the next report cycle can try again. */
        taskENTER_CRITICAL();
        g_time_request_pending = 0U;
        taskEXIT_CRITICAL();
        printf("Cloud time request deferred: ESP8266 busy\r\n");
    }
}


// 解析下发的数据：   esp8266.c 中实现
void vESP8266_ReceiveTask(void *pvParameters)
{
    uint8_t message[RX_BUF_SIZE + 1];
    uint16_t message_len;

    // 任务进入无限循环
    while (1)
    {
        // 阻塞等待信号量（永久等待，直到收到数据）
        if (xSemaphoreTake(xMqttDataSemaphore, portMAX_DELAY) == pdTRUE)
        {
            taskENTER_CRITICAL();
            message_len = rx_parse_len;
            if (message_len > RX_BUF_SIZE) message_len = RX_BUF_SIZE;
            memcpy(message, rx_parse_buffer, message_len);
            message[message_len] = '\0';
            taskEXIT_CRITICAL();

            // 【关键】此时 rx_parse_buffer 中有完整的一帧数据，长度为 rx_parse_len
            // 1. 打印接收到的数据（调试用?
				
             printf("ESP8266 Received (%d bytes):\r\n", message_len);
             printf("%.*s\r\n", message_len, message);
            
            // 2. 判断数据类型
		    // 检查是否为属性设置指令（物模型标准 method 字段）
            if (parse_cloud_time_response((char *)message))
            {
                /* Aliyun NTP response handled above. */
            }
            else if (strstr((char*)message, "thing.service.property.set") != NULL)
            {
                // 提取 TempThreshold
                char *p = strstr((char*)message, "TempThreshold");
                if (p != NULL)
                {
                    int threshold = 0;
                    // 从 "TempThreshold":20 中提取数字（注意可能存在空格）
                    char *colon = strchr(p, ':');
                    if (colon != NULL && sscanf(colon + 1, " %d", &threshold) == 1)
                    {
                        printf("New threshold received: %d\r\n", threshold);
                        g_temp_threshold = threshold; // 更新全局阈值
                    }
                    else
                    {
                        printf("Failed to parse threshold value.\r\n");
                    }
                }
            }
            else if (strstr((char*)message, "+MQTTCONNECTED") != NULL)
            {
                // MQTT连接成功（异步通知）
                printf(">>> MQTT Connected to cloud!\r\n");
                g_mqtt_connected = 1U;
                if (!g_version_reported) {
                    esp8266_report_version(OTA_VERSION);
                    g_version_reported = 1U;
                }
                request_cloud_time_if_needed();
            }
            else if (strstr((char*)message, "+MQTTDISCONNECTED") != NULL)
            {
                // MQTT断开连接
                printf(">>> MQTT Disconnected!\r\n");
								g_mqtt_connected = 0U;
								g_version_reported = 0U;
								g_time_request_pending = 0U;
            }
						else if (strstr((char*)message, "/ota/device/upgrade") != NULL) {
							// OTA升级
									ota_info_t ota_info = {0};
									parse_ota_notification((char*)message, &ota_info);
									printf("OTA: New version: %s, size: %u\r\n", ota_info.version, ota_info.size);
									if (version_compare(ota_info.version, OTA_VERSION) <= 0) {
									printf("OTA: version %s is not newer than current %s, ignored\r\n",
												ota_info.version, OTA_VERSION);
												continue;
									}
									printf(" start OTA upgrade\r\n");
									ota_download_firmware(ota_info.url, ota_info.size, ota_info.md5);
						}
            else
            {
                printf(">>> Other response (AT command echo/response)\r\n");
            }
        }
    }
}

// 按键扫描任务
void KEY_Task(void *pvParameters){
		
	uint32_t last_key0_press_time = 0;
    uint32_t last_key1_press_time = 0;
	uint32_t last_key2_press_time = 0;
    const uint32_t debounce_ms = 30;
		//key0 key1 
    uint8_t last0 = 1, last1 = 1,last2=0; 
    uint8_t current0, current1,current2;

    while (1)
    {
        current0 = KEY0;
        current1 = KEY1;
		current2 = WK_UP;
		
        if (last0 == 1 && current0 == 0) {
            if ((xTaskGetTickCount() - last_key0_press_time) >= pdMS_TO_TICKS(debounce_ms)) {
                beep_Off(); 
                last_key0_press_time = xTaskGetTickCount();
            }
        }
        last0 = current0;

        if (last1 == 1 && current1 == 0) {
            if ((((xTaskGetTickCount() - last_key1_press_time) >= pdMS_TO_TICKS(debounce_ms))) && beep_enable) {
                beep_On(); 
                last_key1_press_time = xTaskGetTickCount();
            }
        }
        last1 = current1;
		
		//beep 总开关
		if (last2 == 0 && current2 == 1) {
            if (((xTaskGetTickCount() - last_key2_press_time) >= pdMS_TO_TICKS(debounce_ms))) {
                beep_enable=!beep_enable;
                last_key2_press_time = xTaskGetTickCount();
            }
        }
        last2 = current2;

		Watchdog_Heartbeat(WDG_HEARTBEAT_KEY);
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }	
	

}

//传感器采集数据任务
void DHT11_Task(void *pvParameters){
	
	(void)pvParameters;
    uint8_t temp, hum;
    while (1)
    {
			
			dht11_read_data(&temp, &hum);
			if(xSemaphoreTake(xDHT11Mutex, portMAX_DELAY) == pdTRUE) {
            temperature = temp;
            humidity    = hum;
			if(((temperature>g_temp_threshold || temperature==g_temp_threshold)) && beep_enable){
				beep_On();
			}
			else{
				beep_Off();
			}
			
            xSemaphoreGive(xDHT11Mutex);
        }
			Watchdog_Heartbeat(WDG_HEARTBEAT_DHT);
				vTaskDelay(pdMS_TO_TICKS(1000));
		}
}


void LED0_Task(void *pvParameters)
{
 
    (void)pvParameters;
    
    while (1)
    {
        HAL_GPIO_TogglePin(LED0_PORT, LED0_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));  // ?? 500ms
    }
}

void LED1_Task(void *pvParameters)
{
 
    (void)pvParameters;
    
    while (1)
    {
        HAL_GPIO_TogglePin(LED1_PORT, LED1_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));  // ?? 500ms
    }
}

//串口任务，打印
void UART_Task(void *pvParameters)
{		
		uint8_t temp, hum;
		while(1){
		   if (xSemaphoreTake(xDHT11Mutex, portMAX_DELAY) == pdTRUE) {
           temp = temperature;
           hum  = humidity;
           xSemaphoreGive(xDHT11Mutex);
       }
			 printf("Temp: %d C, Hum: %d %%,Tempthreadhold:%d \r\n", temp, hum,g_temp_threshold);
			vTaskDelay(pdMS_TO_TICKS(2000));
		}
	
}
      
void LVGL_Task(void *pvParameters)
{
 
    create_test_ui();   
	
    TickType_t xLastWakeTime = xTaskGetTickCount();
    TickType_t last_dashboard_update = 0U;
    const TickType_t xFrequency = pdMS_TO_TICKS(20);   

    for (;;)
    {		
		if ((xTaskGetTickCount() - last_dashboard_update) >= pdMS_TO_TICKS(200U)) {
			update_dashboard();
			last_dashboard_update = xTaskGetTickCount();
        }
        lv_timer_handler();
		Watchdog_Heartbeat(WDG_HEARTBEAT_LVGL);
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}


/*************  Sub Founction  ******************/


static void create_test_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *top_bar;
    lv_obj_t *btn;
    lv_obj_t *btn_label;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0xDDEBF2), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    top_bar = lv_obj_create(scr);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_size(top_bar, 320, 28);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x5799CF), 0);

    time_label = lv_label_create(top_bar);
    lv_label_set_text(time_label, "--:--");
    lv_obj_set_pos(time_label, 28, 7);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);

    status_label = lv_label_create(top_bar);
    lv_label_set_text(status_label, "MQTT OFFLINE");
    lv_obj_set_pos(status_label, 135, 7);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);

    humidity_label = lv_label_create(scr);
    lv_label_set_text(humidity_label, "Hum: --%");
    lv_obj_set_pos(humidity_label, 24, 58);
    lv_obj_set_size(humidity_label, 94, 30);
    lv_obj_set_style_pad_all(humidity_label, 8, 0);
    lv_obj_set_style_bg_opa(humidity_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(humidity_label, lv_color_hex(0x5799CF), 0);
    lv_obj_set_style_text_color(humidity_label, lv_color_white(), 0);

    temperature_label = lv_label_create(scr);
    lv_label_set_text(temperature_label, "Temp: -- C");
    lv_obj_set_pos(temperature_label, 24, 98);
    lv_obj_set_size(temperature_label, 94, 30);
    lv_obj_set_style_pad_all(temperature_label, 8, 0);
    lv_obj_set_style_bg_opa(temperature_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(temperature_label, lv_color_hex(0x5799CF), 0);
    lv_obj_set_style_text_color(temperature_label, lv_color_white(), 0);

    threshold_label = lv_label_create(scr);
    lv_label_set_text(threshold_label, "Threshold: -- C");
    lv_obj_set_pos(threshold_label, 135, 58);
    lv_obj_set_size(threshold_label, 142, 30);
    lv_obj_set_style_pad_all(threshold_label, 8, 0);
    lv_obj_set_style_bg_opa(threshold_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(threshold_label, lv_color_hex(0x5799CF), 0);
    lv_obj_set_style_text_color(threshold_label, lv_color_white(), 0);

    alarm_lamp = lv_obj_create(scr);
    lv_obj_set_pos(alarm_lamp, 184, 98);
    lv_obj_set_size(alarm_lamp, 34, 34);
    lv_obj_set_style_radius(alarm_lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(alarm_lamp, 0, 0);
    lv_obj_set_style_pad_all(alarm_lamp, 0, 0);
    lv_obj_clear_flag(alarm_lamp, LV_OBJ_FLAG_SCROLLABLE);

    alarm_label = lv_label_create(alarm_lamp);
    lv_label_set_text(alarm_label, "OK");
    lv_obj_center(alarm_label);
    lv_obj_set_style_text_color(alarm_label, lv_color_white(), 0);

    btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 140, 38);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x74628F), 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL); 

    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "SYNC CLOUD TIME");
    lv_obj_center(btn_label);

    sync_status_label = lv_label_create(scr);
    lv_label_set_text(sync_status_label, "Time not synchronized");
    lv_obj_align(sync_status_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_color(sync_status_label, lv_color_hex(0x596673), 0);
}

static void update_dashboard(void)
{
    uint32_t temp;
    uint32_t hum;
    uint32_t threshold;
    uint8_t alarm_active;
    uint8_t synced;
    uint64_t base_ms;
    TickType_t base_tick;
    uint64_t now_seconds;
    uint32_t seconds_in_day;
    char text[48];

    if (xSemaphoreTake(xDHT11Mutex, pdMS_TO_TICKS(5U)) == pdTRUE) {
        temp = temperature;
        hum = humidity;
        threshold = g_temp_threshold;
        alarm_active = (beep_enable && temp >= threshold) ? 1U : 0U;
        xSemaphoreGive(xDHT11Mutex);

        snprintf(text, sizeof(text), "Temp: %lu C", (unsigned long)temp);
        lv_label_set_text(temperature_label, text);
        snprintf(text, sizeof(text), "Hum: %lu%%", (unsigned long)hum);
        lv_label_set_text(humidity_label, text);
        snprintf(text, sizeof(text), "Threshold: %lu C", (unsigned long)threshold);
        lv_label_set_text(threshold_label, text);

		lv_obj_set_style_bg_color(alarm_lamp,
								  alarm_active ? lv_color_hex(0xE60012) :
														 lv_color_hex(0x28A745), 0);
		lv_label_set_text(alarm_label, alarm_active ? "ALM" : "OK");
		lv_label_set_text(status_label, alarm_active ? "ALARM MODE" : "NORMAL MODE");
    }

    taskENTER_CRITICAL();
    synced = g_time_synced;
    base_ms = g_unix_time_base_ms;
    base_tick = g_time_tick_base;
    taskEXIT_CRITICAL();

    if (synced) {
        now_seconds = (base_ms +
                      (uint32_t)(xTaskGetTickCount() - base_tick)) / 1000ULL;
        /* Display China Standard Time (UTC+8). */
        seconds_in_day = (uint32_t)((now_seconds + 8ULL * 3600ULL) % 86400ULL);
        snprintf(text, sizeof(text), "%02lu:%02lu",
                 (unsigned long)(seconds_in_day / 3600U),
                 (unsigned long)((seconds_in_day % 3600U) / 60U));
        lv_label_set_text(time_label, text);
        lv_label_set_text(sync_status_label, "Cloud time synchronized");
    }
}


void btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!g_mqtt_connected) {
            lv_label_set_text(sync_status_label, "MQTT is offline");
        } else if (esp8266_request_cloud_time((uint32_t)xTaskGetTickCount())) {
            lv_label_set_text(sync_status_label, "Synchronizing...");
        } else {
            lv_label_set_text(sync_status_label, "ESP8266 is busy");
        }
    }
}

// stack overflow hook 
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("Stack overflow in task: %s\r\n", pcTaskName);
    while(1); 
}



void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{

  if (htim->Instance == TIM1) {
    HAL_IncTick();
  }

}


void Error_Handler(void)
{

  __disable_irq();
  while (1)
  {
  }

}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif 
