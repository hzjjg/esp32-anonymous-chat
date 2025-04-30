#ifndef _CHAT_SERVER_H_
#define _CHAT_SERVER_H_

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief 初始化聊天服务器
 *
 * 初始化聊天存储系统并设置相关资源
 *
 * @return ESP_OK 成功，或错误码
 */
esp_err_t chat_server_init(void);

/**
 * @brief 注册聊天服务器的URI处理函数
 *
 * @param server HTTP服务器句柄
 *
 * @return ESP_OK 成功，或错误码
 */
esp_err_t register_chat_uri_handlers(httpd_handle_t server);

#endif /* _CHAT_SERVER_H_ */