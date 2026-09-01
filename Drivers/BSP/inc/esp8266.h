
#ifndef __ESP8266_H
#define __ESP8266_H


#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include "my_flash.h"

/* 初始化指令

1-重启ESP模块
AT+RESTORE

2-配置WIFI模式
AT+CWMODE=1 

3-进行服务器设置
AT+CIPSNTPCFG=1,8,"cn.ntp.org.cn","ntp.sjtu.edu.cn"

4-连接wifi
AT+CWJAP=<SSID>,<SSID_PASSWD>

5-设置MQTT属性
AT+MQTTUSERCFG=0,1,"NULL",<USERNAME>,<PASSWD>",0,0,""

6-设置MQTT ID
AT+MQTTCLIENTID=0,<CLIENTID>

7-发送MQTT域名，域名获取
AT+MQTTCONN=0,<MQTTHOSTURL>,1883,1

8- 订阅topic

*/

#define TX_PORT  GPIOB
#define RX_PORT  GPIOB
#define TX_PIN   GPIO_PIN_10
#define RX_PIN   GPIO_PIN_11

//初始化MQTT命令，根据自己的产品修改：
extern const char *PRODUCT_KEY;          // "h8sfOvOnleu"
extern const char *DEVICE_NAME;          // 你的设备名，如 "MyDevice"

extern const char *SSID;
extern const char *SSID_PASSWD;
extern const char *USERNAME;
extern const char *PASSWD;
 //","前面加了转义字符"\"
extern const char *CLIENTID;
extern const char *MQTTHOSTURL;


typedef struct {
    char host[64];
    char path[256];
    uint16_t port;
} url_info_t;	
	
typedef struct {
    char url[256];      // 固件下载地址
    uint32_t size;      // 固件大小（字节）
    char md5[33];       // MD5 校验值（32字符 + 结束符）
    char version[16];   // 新版本号
} ota_info_t;

/* 定义自己添加的topic指令


*/

#define OTA_VERSION      "4.0"

// /h8sfOvOnleu/demo/user/get
#define  MY_PUBLISH    "/%s/%s/user/update"
#define  MY_DESCRIBLE  "/%s/%s/user/get"

// /sys/h8sfOvOnleu/${deviceName}/thing/event/property/post
#define TOPIC_UPDATE   "/sys/%s/%s/thing/event/property/post"      // 上报

#define TOPIC_GET    "/sys/%s/%s/thing/service/property/set"      // 订阅

#define OTA_INFORM_TOPIC  "/ota/device/inform/%s/%s"        //设备上报固件升级信息         
#define OTA_UPGRADE       "/ota/device/upgrade/%s/%s"       //固件升级信息下行
//获取网络时间topic
#define NTP_REQUEST_TOPIC "/ext/ntp/%s/%s/request"
#define NTP_RESPONSE_PATH "/ext/ntp/%s/%s/response"
 

#define APP1_ADDRESS           0x08007800 
//定义OTA升级地址
#define APP2_ADDRESS  0x08043800   // APP2 Flash 起始地址
#define APP2_SIZE     0x3C000      // 240KB
//升级标志
#define FLAG_ADDRESS  0x0807F800       // 参数存储区
#define OTA_FLAG      0xAAAA5555



// DMA接收缓冲区大小（建议大于MTU）
#define RX_BUF_SIZE         2048
#define ESP8266_USART       USART3
// 声明全局句柄（在.c中定义）
extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef hdma_usart3_rx;
// 任务解析用的缓冲区（用于中断中复制数据，避免被覆盖）
extern uint8_t rx_parse_buffer[RX_BUF_SIZE + 1];
extern uint16_t rx_parse_len;
// 中断给任务发送的信号量
extern SemaphoreHandle_t xMqttDataSemaphore;
extern SemaphoreHandle_t xEsp8266TxMutex;



// 底层引脚初始化
void esp8266_MspInit(void);
//退出透传
void esp8266_exit_transparent_mode(void);
// 连接MQTT模块
uint8_t esp8266_init(void);
void esp8266_send_at(const char *cmd, uint32_t delayms);
uint8_t esp8266_send_at_wait(const char *cmd, const char *expected, uint32_t timeout_ms);
	//订阅
uint8_t esp8266_describe(void);
 //上报数据
void esp8266_publish_data(uint32_t temperature, uint32_t humidity);
//通过阿里云MQTT NTP服务请求互联网时间，device_send_ms使用本机单调毫秒计数
uint8_t esp8266_request_cloud_time(uint32_t device_send_ms);

//上报当前固件版本
void esp8266_report_version(const char * version);
//比较版本
 int version_compare(const char *new_version, const char *current_version);
// 下发OTA时，解析地址，MD5等信息。	
void parse_ota_notification(const char *buffer, ota_info_t *info) ;
// 解析域名函数
uint8_t parse_url(const char *url, url_info_t *info);
// 下载OTA固件
void ota_download_firmware(const char *url, uint32_t file_size, const char *expected_md5);

//Flash 擦除和写入函数
void flash_erase_app2_partition(void);
void write_to_app2_flash(uint8_t *data, uint32_t len);

void reset_flash_writer(void) ;
//设置升级标志
void set_update_flag(void) ;


#endif









