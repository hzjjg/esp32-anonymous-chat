#ifndef _CHAT_STORAGE_H_
#define _CHAT_STORAGE_H_

#include "esp_err.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* 系统配置常量 */
#define MAX_MESSAGES 100       // 最大存储消息数量
#define MAX_MESSAGE_LENGTH 150  // 单条消息最大长度
#define MAX_UUID_LENGTH 37      // UUID最大长度(36字符+空终止符)
#define MAX_USERNAME_LENGTH 32  // 用户名最大长度
#define NVS_MSG_KEY_PREFIX "msg_" // NVS存储消息的键前缀
#define NVS_MSG_COUNT_KEY "msg_count" // NVS存储消息总数的键
#define CACHE_VALID_TIME 30    // 缓存有效时间(秒)
#define MIN_MESSAGES_TO_SAVE 5 // 最少累积消息数量触发保存

/* 聊天消息结构体 */
typedef struct {
    char uuid[MAX_UUID_LENGTH];      // 消息唯一标识符
    char username[MAX_USERNAME_LENGTH]; // 用户名
    char message[MAX_MESSAGE_LENGTH]; // 消息内容
    uint32_t timestamp;             // 时间戳
} chat_message_t;

/* 聊天消息存储结构体 */
typedef struct {
    chat_message_t messages[MAX_MESSAGES]; // 消息环形缓冲区
    int count;                             // 当前存储的消息数量
    int next_index;                        // 下一条消息的存储位置
} chat_storage_t;

/**
 * @brief 初始化聊天存储系统
 *
 * 创建互斥锁并从NVS加载历史聊天消息
 *
 * @return ESP_OK 成功，其他为错误码
 */
esp_err_t chat_storage_init(void);

/**
 * @brief 添加新的聊天消息
 *
 * @param uuid 用户唯一标识符
 * @param username 用户名
 * @param message 消息内容
 * @return ESP_OK 添加成功
 * @return ESP_FAIL 添加失败
 */
esp_err_t chat_storage_add_message(const char *uuid, const char *username, const char *message);

/**
 * @brief 获取所有聊天消息的JSON字符串
 *
 * @return char* JSON字符串指针，调用者负责释放内存
 * @return NULL 获取失败
 */
char* chat_storage_get_messages_json(void);

/**
 * @brief 获取指定时间戳后的消息JSON
 *
 * @param since_timestamp 时间戳
 * @param has_new_messages 输出参数，是否有新消息
 * @return char* JSON字符串指针，调用者负责释放内存
 * @return NULL 获取失败
 */
char* chat_storage_get_messages_since_json(uint32_t since_timestamp, bool *has_new_messages);

/**
 * @brief 获取当前时间戳
 *
 * @return uint32_t 当前时间戳
 */
uint32_t chat_storage_get_current_time(void);

/**
 * @brief 释放聊天存储系统资源
 *
 * 保存所有未保存的消息并释放资源
 */
void chat_storage_deinit(void);

#endif /* _CHAT_STORAGE_H_ */