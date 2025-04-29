#ifndef _CHAT_SERVER_H_
#define _CHAT_SERVER_H_

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief 初始化聊天服务器
 *
 * 创建互斥锁并从NVS加载历史聊天消息
 *
 * @return ESP_OK 成功，或错误码
 */
esp_err_t chat_server_init(void);

/**
 * @brief 添加聊天消息
 *
 * @param uuid 用户UUID
 * @param username 用户名
 * @param message 消息内容
 *
 * @return ESP_OK 成功，或错误码
 */
esp_err_t add_chat_message(const char *uuid, const char *username, const char *message);

/**
 * @brief 获取聊天消息的JSON字符串
 *
 * @return 聊天消息的JSON字符串，调用者负责释放内存
 */
char* get_chat_messages_json(void);

/**
 * @brief 注册聊天服务器的URI处理函数
 *
 * @param server HTTP服务器句柄
 *
 * @return ESP_OK 成功，或错误码
 */
esp_err_t register_chat_uri_handlers(httpd_handle_t server);

/**
 * @brief 移除SSE客户端
 *
 * @param hd HTTP服务器句柄
 * @param fd Socket文件描述符
 */
void remove_sse_client(httpd_handle_t hd, int fd);

/**
 * @brief 向所有SSE客户端发送通知
 *
 * @param event_name 事件名
 * @param data 数据（JSON字符串）
 */
void notify_sse_clients(const char *event_name, const char *data);

/**
 * @brief 客户端断开连接处理函数
 *
 * @param arg 用户参数
 * @param hd HTTP服务器句柄
 * @param sockfd Socket文件描述符
 */
void chat_disconnect_handler(void* arg, httpd_handle_t hd, int sockfd);

#endif /* _CHAT_SERVER_H_ */