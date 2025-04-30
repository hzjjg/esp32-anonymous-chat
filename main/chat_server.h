#ifndef _CHAT_SERVER_H_
#define _CHAT_SERVER_H_

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief 初始化聊天服务器
 *
 * 创建必要的互斥锁并从NVS加载历史聊天消息
 *
 * @return ESP_OK 成功初始化
 * @return ESP_FAIL 初始化失败
 */
esp_err_t chat_server_init(void);

/**
 * @brief 注册聊天服务器的URI处理函数
 *
 * 为聊天服务器注册所有HTTP请求处理函数
 *
 * @param server HTTP服务器句柄
 * @return ESP_OK 注册成功
 * @return ESP_FAIL 注册失败
 */
esp_err_t register_chat_uri_handlers(httpd_handle_t server);

/**
 * @brief 添加带时间戳的消息
 *
 * @param uuid 用户UUID
 * @param username 用户名
 * @param message 消息内容
 * @param timestamp 客户端时间戳
 * @return esp_err_t
 */
esp_err_t chat_add_message_with_timestamp(const char *uuid, const char *username, const char *message, uint32_t timestamp);

#endif /* _CHAT_SERVER_H_ */