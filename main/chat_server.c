/*
 * ESP32匿名聊天服务器实现
 * 主要功能：
 * 1. 通过HTTP服务器提供RESTful API接口
 * 2. 提供聊天相关的HTTP处理函数
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_random.h"
#include "chat_storage.h"
#include "chat_server.h"

static const char *CHAT_TAG = "chat-server"; // 日志标签

/**
 * @brief 设置CORS响应头
 *
 * 统一为所有API响应设置CORS头，支持跨域访问
 *
 * @param req HTTP请求对象
 */
static void set_cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

/**
 * @brief 生成UUID v4
 *
 * @param uuid_str 输出参数，用于存储生成的UUID字符串
 *
 * 说明：
 * 1. 使用ESP32的硬件随机数生成器创建16字节随机数，确保高随机性
 * 2. 设置版本位(第6字节的高4位)为0100(版本4)，表明这是随机生成的UUID
 * 3. 设置变体位(第8字节的高2位)为10(RFC 4122变体)
 * 4. 格式化为标准UUID字符串(8-4-4-4-12格式)
 * 5. 确保UUID的唯一性，用于区分不同的消息和客户端
 */
static void generate_uuid(char *uuid_str) {
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid)); // 使用硬件随机数生成器获取真随机数

    // 设置版本位为4(随机UUID)，保留低4位，高4位设为0100(4)
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    // 设置变体位为RFC 4122变体，保留低6位，高2位设为10
    uuid[8] = (uuid[8] & 0x3F) | 0x80;

    // 格式化为标准UUID字符串 (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    snprintf(uuid_str, MAX_UUID_LENGTH,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11],
             uuid[12], uuid[13], uuid[14], uuid[15]);
}

/**
 * @brief 初始化聊天服务器
 *
 * 创建必要的互斥锁并从NVS加载历史聊天消息
 *
 * 实现细节：
 * 1. 初始化聊天消息存储系统
 *
 * @return ESP_OK 成功初始化
 * @return ESP_FAIL 初始化失败
 */
esp_err_t chat_server_init(void) {
    // 初始化聊天消息存储系统
    esp_err_t err = chat_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(CHAT_TAG, "Failed to initialize chat storage");
        return err;
    }

    ESP_LOGI(CHAT_TAG, "Chat server initialized successfully");
    return ESP_OK;
}

// 添加OPTIONS请求处理程序，帮助处理CORS
static esp_err_t options_handler(httpd_req_t *req) {
    set_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief 处理新聊天消息的POST请求
 *
 * 接收JSON格式的聊天消息，验证后存储到内存和NVS
 *
 * @param req HTTP请求对象，包含消息内容和客户端信息
 * @return ESP_OK 处理成功
 * @return ESP_FAIL 处理失败
 *
 * 功能说明：
 * 1. 验证请求内容长度和JSON格式
 * 2. 检查必填字段(uuid, username, message)和长度限制
 * 3. 调用存储模块添加消息
 * 4. 返回适当的HTTP状态码和响应
 */
static esp_err_t post_message_handler(httpd_req_t *req) {
    // 设置CORS头
    set_cors_headers(req);

    // 检查内容长度是否超过限制
    if (req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
        return ESP_FAIL;
    }

    // 一次性分配接收缓冲区
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    // 一次性接收数据
    int total_received = httpd_req_recv(req, buf, req->content_len);
    if (total_received != req->content_len) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
    }
    buf[req->content_len] = '\0';

    // 解析JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf); // 立即释放接收缓冲区，减少内存占用时间

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // 提取并验证必要字段
    const char *uuid = NULL;
    const char *username = NULL;
    const char *message = NULL;
    bool is_valid = true;

    cJSON *uuid_obj = cJSON_GetObjectItem(root, "uuid");
    cJSON *username_obj = cJSON_GetObjectItem(root, "username");
    cJSON *message_obj = cJSON_GetObjectItem(root, "message");

    if (uuid_obj && cJSON_IsString(uuid_obj)) {
        uuid = uuid_obj->valuestring;
    } else {
        is_valid = false;
    }

    if (username_obj && cJSON_IsString(username_obj)) {
        username = username_obj->valuestring;
    } else {
        is_valid = false;
    }

    if (message_obj && cJSON_IsString(message_obj)) {
        message = message_obj->valuestring;
    } else {
        is_valid = false;
    }

    // 验证字段有效性和长度
    if (!is_valid || !uuid || !username || !message ||
        strlen(uuid) >= MAX_UUID_LENGTH ||
        strlen(username) >= 32 ||
        strlen(message) > MAX_MESSAGE_LENGTH ||
        strlen(message) == 0) {

        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid message format or field length");
        return ESP_FAIL;
    }

    // 添加消息到存储
    esp_err_t err = chat_storage_add_message(uuid, username, message);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "201 Created");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to add message");
    }

    return ESP_OK;
}

/**
 * @brief 生成UUID的请求处理函数
 *
 * 使用ESP32硬件随机数生成器创建符合RFC4122标准的UUID v4
 *
 * @param req HTTP请求对象
 * @return ESP_OK 生成成功
 * @return ESP_FAIL 生成失败
 */
static esp_err_t generate_uuid_handler(httpd_req_t *req) {
    // 设置CORS头，允许跨域访问
    set_cors_headers(req);

    // 生成UUID
    char uuid[MAX_UUID_LENGTH];
    generate_uuid(uuid);

    // 直接构建简单JSON响应
    char json_response[60]; // 足够容纳UUID JSON响应
    snprintf(json_response, sizeof(json_response), "{\"uuid\":\"%s\"}", uuid);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);

    return ESP_OK;
}

/**
 * @brief 获取特定时间戳后的消息
 *
 * @param req HTTP请求对象
 * @return ESP_OK 处理成功
 * @return ESP_FAIL 处理失败
 *
 * 该函数处理客户端的轮询请求，返回指定时间戳之后的所有消息
 * 客户端通过查询参数since_timestamp指定时间戳
 */
static esp_err_t get_messages_since_handler(httpd_req_t *req) {
    // 设置CORS头，允许跨域访问
    set_cors_headers(req);

    // 获取since_timestamp查询参数
    uint32_t since_timestamp = 0;
    char param[32];

    // 如果URL有查询参数
    if (httpd_req_get_url_query_len(req) > 0) {
        if (httpd_req_get_url_query_str(req, param, sizeof(param)) == ESP_OK) {
            // 直接提取since_timestamp参数
            char value[16];
            if (httpd_query_key_value(param, "since_timestamp", value, sizeof(value)) == ESP_OK) {
                since_timestamp = (uint32_t)atoi(value);
            }
        }
    }

    // 获取匹配消息
    bool has_new_messages = false;
    char *json_str = chat_storage_get_messages_since_json(since_timestamp, &has_new_messages);

    if (json_str) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        return ESP_OK;
    }

    // 如果获取消息失败，返回空JSON对象
    httpd_resp_set_type(req, "application/json");
    char error_response[128];
    snprintf(error_response, sizeof(error_response),
             "{\"messages\":[],"
             "\"has_new_messages\":false,"
             "\"error\":\"Failed to retrieve messages\"}");
    httpd_resp_sendstr(req, error_response);

    return ESP_OK;
}

/**
 * @brief 注册聊天服务器的URI处理函数
 *
 * 为聊天服务器注册所有必要的HTTP请求处理函数，构建RESTful API
 *
 * 注册的处理函数包括：
 * 1. OPTIONS请求处理函数 - 支持CORS预检请求
 * 2. 轮询API接口 - 实现客户端获取新消息
 * 3. 消息提交接口 - 处理新消息的添加
 * 4. UUID生成接口 - 为新用户生成唯一标识符
 *
 * @param server HTTP服务器句柄
 * @return ESP_OK 注册成功
 * @return ESP_FAIL 注册失败
 */
esp_err_t register_chat_uri_handlers(httpd_handle_t server) {
    // Handler for OPTIONS requests (CORS)
    httpd_uri_t options_uri = {
        .uri = "/api/chat/*",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &options_uri);

    // 添加轮询API处理函数 - 用于客户端轮询获取新消息
    httpd_uri_t get_messages_since_uri = {
        .uri = "/api/chat/messages",
        .method = HTTP_GET,
        .handler = get_messages_since_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &get_messages_since_uri);

    // Handler for posting messages - 用于客户端发送新消息
    httpd_uri_t post_message_uri = {
        .uri = "/api/chat/message",
        .method = HTTP_POST,
        .handler = post_message_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &post_message_uri);

    // Handler for generating UUID - 用于为新用户生成唯一标识符
    httpd_uri_t generate_uuid_uri = {
        .uri = "/api/chat/uuid",
        .method = HTTP_GET,
        .handler = generate_uuid_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &generate_uuid_uri);

    return ESP_OK;
}