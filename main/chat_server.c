/*
 * ESP32匿名聊天服务器实现
 * 主要功能：
 * 1. 通过HTTP服务器提供RESTful API接口
 * 2. 使用NVS(非易失性存储)持久化聊天记录
 * 3. 线程安全的消息存储和访问机制
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"

/* 系统配置常量 */
#define MAX_MESSAGES 100       // 最大存储消息数量
#define MAX_MESSAGE_LENGTH 150  // 单条消息最大长度
#define MAX_UUID_LENGTH 37      // UUID最大长度(36字符+空终止符)
#define NVS_MSG_KEY_PREFIX "msg_" // NVS存储消息的键前缀
#define NVS_MSG_COUNT_KEY "msg_count" // NVS存储消息总数的键

static const char *CHAT_TAG = "chat-server"; // 日志标签
static SemaphoreHandle_t chat_mutex = NULL; // 聊天消息存储的互斥锁

/* 聊天消息结构体 */
typedef struct {
    char uuid[MAX_UUID_LENGTH];      // 消息唯一标识符
    char username[32];               // 用户名
    char message[MAX_MESSAGE_LENGTH]; // 消息内容
    uint32_t timestamp;             // 时间戳
} chat_message_t;

/* 聊天消息存储结构体 */
typedef struct {
    chat_message_t messages[MAX_MESSAGES]; // 消息环形缓冲区
    int count;                             // 当前存储的消息数量
    int next_index;                        // 下一条消息的存储位置
} chat_storage_t;

static chat_storage_t chat_storage = {
    .count = 0,
    .next_index = 0
};

// 在chat_storage_t定义后，添加函数前向声明
char* get_chat_messages_json(void);
static esp_err_t save_message_to_nvs(nvs_handle_t nvs_handle, int index, const chat_message_t *message);
static esp_err_t load_message_from_nvs(nvs_handle_t nvs_handle, int index, chat_message_t *message);

/**
 * @brief 保存单条消息到NVS
 * @param nvs_handle NVS句柄
 * @param index 消息索引
 * @param message 要保存的消息结构体指针
 * @return ESP_OK成功，其他为错误码
 * 说明：
 * 1. 使用"msg_"前缀+索引作为键名
 * 2. 将消息结构体转换为JSON格式存储
 * 3. 自动管理内存，确保不会泄漏
 */
static esp_err_t save_message_to_nvs(nvs_handle_t nvs_handle, int index, const chat_message_t *message) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_MSG_KEY_PREFIX, index);

    // 将消息转换为JSON字符串
    cJSON *msg_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(msg_obj, "uuid", message->uuid);
    cJSON_AddStringToObject(msg_obj, "username", message->username);
    cJSON_AddStringToObject(msg_obj, "message", message->message);
    cJSON_AddNumberToObject(msg_obj, "timestamp", message->timestamp);

    char *msg_json = cJSON_PrintUnformatted(msg_obj);
    cJSON_Delete(msg_obj);

    if (!msg_json) {
        return ESP_FAIL;
    }

    esp_err_t err = nvs_set_str(nvs_handle, key, msg_json);
    free(msg_json);

    return err;
}

/**
 * @brief 从NVS加载单条消息
 * @param nvs_handle NVS句柄
 * @param index 消息索引
 * @param message 输出参数，用于存储加载的消息
 * @return ESP_OK成功，其他为错误码
 * 说明：
 * 1. 先获取存储的JSON字符串大小
 * 2. 分配内存并读取JSON字符串
 * 3. 解析JSON并填充到消息结构体
 * 4. 自动管理内存，确保不会泄漏
 */
static esp_err_t load_message_from_nvs(nvs_handle_t nvs_handle, int index, chat_message_t *message) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_MSG_KEY_PREFIX, index);

    // 先获取所需存储空间大小
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvs_handle, key, NULL, &required_size);
    if (err != ESP_OK) {
        return err;
    }

    // 分配内存并读取JSON字符串
    char *msg_json = malloc(required_size);
    if (!msg_json) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(nvs_handle, key, msg_json, &required_size);
    if (err != ESP_OK) {
        free(msg_json);
        return err;
    }

    // 解析JSON
    cJSON *msg_obj = cJSON_Parse(msg_json);
    free(msg_json);

    if (!msg_obj) {
        return ESP_FAIL;
    }

    // 提取各字段
    cJSON *uuid_obj = cJSON_GetObjectItem(msg_obj, "uuid");
    cJSON *username_obj = cJSON_GetObjectItem(msg_obj, "username");
    cJSON *message_obj = cJSON_GetObjectItem(msg_obj, "message");
    cJSON *timestamp_obj = cJSON_GetObjectItem(msg_obj, "timestamp");

    if (uuid_obj && username_obj && message_obj && timestamp_obj) {
        strlcpy(message->uuid, uuid_obj->valuestring, MAX_UUID_LENGTH);
        strlcpy(message->username, username_obj->valuestring, sizeof(message->username));
        strlcpy(message->message, message_obj->valuestring, MAX_MESSAGE_LENGTH);
        message->timestamp = (uint32_t)timestamp_obj->valueint;
    } else {
        cJSON_Delete(msg_obj);
        return ESP_FAIL;
    }

    cJSON_Delete(msg_obj);
    return ESP_OK;
}

/**
 * @brief 获取聊天消息的JSON数组
 *
 * 将当前存储的聊天消息转换为JSON格式字符串，用于初始加载和API响应
 *
 * 实现细节：
 * 1. 创建JSON数组，准备存储所有消息
 * 2. 线程安全地获取消息数据（使用互斥锁）
 * 3. 计算起始索引，处理环形缓冲区的特性
 * 4. 按时间顺序添加每条消息到JSON数组
 * 5. 返回格式化的JSON字符串（调用者负责释放内存）
 *
 * @return char* JSON字符串指针，调用者负责释放内存
 * @return NULL 获取失败
 */
char* get_chat_messages_json(void) {
    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        ESP_LOGE(CHAT_TAG, "Failed to create JSON array");
        return NULL;
    }

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        int start_idx = 0;
        if (chat_storage.count == MAX_MESSAGES) {
            // 如果消息达到最大容量，则从next_index开始（环形缓冲区的最老消息位置）
            start_idx = chat_storage.next_index;
        }

        // 按时间顺序添加所有消息到JSON数组
        for (int i = 0; i < chat_storage.count; i++) {
            int idx = (start_idx + i) % MAX_MESSAGES;
            cJSON *message = cJSON_CreateObject();
            if (message == NULL) {
                ESP_LOGE(CHAT_TAG, "Failed to create JSON message object");
                xSemaphoreGive(chat_mutex);
                cJSON_Delete(root);
                return NULL;
            }

            // 添加消息各字段到JSON对象
            cJSON_AddStringToObject(message, "uuid", chat_storage.messages[idx].uuid);
            cJSON_AddStringToObject(message, "username", chat_storage.messages[idx].username);
            cJSON_AddStringToObject(message, "message", chat_storage.messages[idx].message);
            cJSON_AddNumberToObject(message, "timestamp", chat_storage.messages[idx].timestamp);
            cJSON_AddItemToArray(root, message);
        }

        xSemaphoreGive(chat_mutex);
    }

    // 将JSON数组转换为字符串
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
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
 * 1. 创建用于保护消息存储的互斥锁
 * 2. 从NVS（非易失性存储）加载之前保存的聊天历史
 * 3. 恢复消息计数和存储位置指针
 *
 * @return ESP_OK 成功初始化
 * @return ESP_FAIL 初始化失败
 */
esp_err_t chat_server_init(void) {
    // 创建消息互斥锁，保证消息读写的线程安全
    chat_mutex = xSemaphoreCreateMutex();
    if (chat_mutex == NULL) {
        ESP_LOGE(CHAT_TAG, "Failed to create chat mutex");
        return ESP_FAIL;
    }

    // 从NVS加载聊天历史记录
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("chat", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(CHAT_TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return ESP_OK; // 即使NVS打开失败，仍然继续初始化其他部分
    }

    // 获取保存的消息数量
    int32_t msg_count = 0;
    err = nvs_get_i32(nvs_handle, NVS_MSG_COUNT_KEY, &msg_count);
    if (err == ESP_OK && msg_count > 0) {
        if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
            // 逐条加载消息
            int loaded_count = 0;
            for (int i = 0; i < msg_count && i < MAX_MESSAGES; i++) {
                int idx = i % MAX_MESSAGES;
                err = load_message_from_nvs(nvs_handle, i, &chat_storage.messages[idx]);
                if (err == ESP_OK) {
                    loaded_count++;
                } else {
                    ESP_LOGW(CHAT_TAG, "Failed to load message %d: %s", i, esp_err_to_name(err));
                }
            }

            // 更新消息计数和下一个存储位置
            chat_storage.count = loaded_count;
            chat_storage.next_index = loaded_count % MAX_MESSAGES;
            ESP_LOGI(CHAT_TAG, "Loaded %d messages from NVS", loaded_count);

            xSemaphoreGive(chat_mutex);
        }
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

/**
 * @brief 保存聊天历史到NVS
 *
 * 将当前内存中的聊天消息保存到非易失性存储(NVS)
 *
 * @return ESP_OK 保存成功
 * @return 其他错误码 保存失败
 */
static esp_err_t save_chat_history(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("chat", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(CHAT_TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        // 保存消息计数
        err = nvs_set_i32(nvs_handle, NVS_MSG_COUNT_KEY, chat_storage.count);
        if (err != ESP_OK) {
            ESP_LOGE(CHAT_TAG, "Error saving message count: %s", esp_err_to_name(err));
            xSemaphoreGive(chat_mutex);
            nvs_close(nvs_handle);
            return err;
        }

        // 计算开始索引
        int start_idx = 0;
        if (chat_storage.count == MAX_MESSAGES) {
            start_idx = chat_storage.next_index;
        }

        // 保存每条消息
        bool save_error = false;
        for (int i = 0; i < chat_storage.count; i++) {
            int msg_idx = (start_idx + i) % MAX_MESSAGES;
            err = save_message_to_nvs(nvs_handle, i, &chat_storage.messages[msg_idx]);
            if (err != ESP_OK) {
                ESP_LOGE(CHAT_TAG, "Error saving message %d: %s", i, esp_err_to_name(err));
                save_error = true;
                break;
            }
        }

        xSemaphoreGive(chat_mutex);

        if (!save_error) {
            err = nvs_commit(nvs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(CHAT_TAG, "Error committing NVS: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(CHAT_TAG, "Chat history saved successfully (%d messages)", chat_storage.count);
            }
        }
    }

    nvs_close(nvs_handle);
    return err;
}

/**
 * @brief 添加新的聊天消息
 *
 * 将新消息添加到环形缓冲区，并持久化存储到NVS
 *
 * 实现细节：
 * 1. 使用互斥锁保证线程安全
 * 2. 在环形缓冲区的下一个位置存储消息
 * 3. 更新消息计数和下一个存储位置
 * 4. 调用save_chat_history将消息持久化到NVS
 *
 * @param uuid 用户唯一标识符
 * @param username 用户名
 * @param message 消息内容
 * @return ESP_OK 添加成功
 * @return ESP_FAIL 添加失败
 */
esp_err_t add_chat_message(const char *uuid, const char *username, const char *message) {
    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        // 获取当前存储位置
        int idx = chat_storage.next_index;

        // 复制消息数据到存储结构
        strlcpy(chat_storage.messages[idx].uuid, uuid, MAX_UUID_LENGTH);
        strlcpy(chat_storage.messages[idx].username, username, sizeof(chat_storage.messages[idx].username));
        strlcpy(chat_storage.messages[idx].message, message, MAX_MESSAGE_LENGTH);
        chat_storage.messages[idx].timestamp = (uint32_t)time(NULL);

        // 更新环形缓冲区指针和消息计数
        chat_storage.next_index = (chat_storage.next_index + 1) % MAX_MESSAGES;
        if (chat_storage.count < MAX_MESSAGES) {
            chat_storage.count++;
        }

        xSemaphoreGive(chat_mutex);

        // 持久化到NVS（非易失性存储）
        save_chat_history();

        return ESP_OK;
    }

    return ESP_FAIL;
}

// 添加OPTIONS请求处理程序，帮助处理CORS
static esp_err_t options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
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
 * 3. 调用add_chat_message存储消息
 * 4. 返回适当的HTTP状态码和响应
 */
static esp_err_t post_message_handler(httpd_req_t *req) {
    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // 检查内容长度是否超过限制
    if (req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
        return ESP_FAIL;
    }

    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *uuid_obj = cJSON_GetObjectItem(root, "uuid");
    cJSON *username_obj = cJSON_GetObjectItem(root, "username");
    cJSON *message_obj = cJSON_GetObjectItem(root, "message");

    if (!uuid_obj || !username_obj || !message_obj ||
        !cJSON_IsString(uuid_obj) || !cJSON_IsString(username_obj) || !cJSON_IsString(message_obj)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing required fields");
        return ESP_FAIL;
    }

    const char *uuid = uuid_obj->valuestring;
    const char *username = username_obj->valuestring;
    const char *message = message_obj->valuestring;

    // 验证字段长度
    if (strlen(uuid) >= MAX_UUID_LENGTH || strlen(username) >= 32 ||
        strlen(message) > MAX_MESSAGE_LENGTH || strlen(message) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid field length");
        return ESP_FAIL;
    }

    // Add message to storage
    esp_err_t err = add_chat_message(uuid, username, message);

    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "201 Created");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to add message");
    }

    cJSON_Delete(root);
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
 *
 * 实现细节：
 * 1. 调用generate_uuid函数生成16字节随机数
 * 2. 设置版本位和变体位
 * 3. 格式化为标准UUID字符串(8-4-4-4-12)
 * 4. 返回JSON格式的响应
 */
static esp_err_t generate_uuid_handler(httpd_req_t *req) {
    // 设置CORS头，允许跨域访问
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // 生成UUID
    char uuid[MAX_UUID_LENGTH];
    generate_uuid(uuid);

    // 创建JSON响应
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "uuid", uuid);

    // 转换为字符串并发送
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // 获取since_timestamp查询参数
    char *buf = NULL;
    size_t buf_len = 0;
    uint32_t since_timestamp = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[32];
            if (httpd_query_key_value(buf, "since_timestamp", param, sizeof(param)) == ESP_OK) {
                since_timestamp = (uint32_t)atoi(param);
            }
        }
        free(buf);
    }

    // 创建JSON数组，准备存储新消息
    cJSON *response = cJSON_CreateObject();
    cJSON *messages_array = cJSON_CreateArray();
    cJSON_AddItemToObject(response, "messages", messages_array);

    // 获取当前时间作为响应时间戳
    uint32_t current_time = (uint32_t)time(NULL);
    cJSON_AddNumberToObject(response, "server_time", current_time);

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        int start_idx = 0;
        if (chat_storage.count == MAX_MESSAGES) {
            start_idx = chat_storage.next_index;
        }

        // 遍历所有消息，寻找比since_timestamp更新的消息
        int found_new_messages = 0;
        for (int i = 0; i < chat_storage.count; i++) {
            int idx = (start_idx + i) % MAX_MESSAGES;

            // 如果消息时间戳大于客户端提供的时间戳，则添加到响应中
            if (chat_storage.messages[idx].timestamp > since_timestamp) {
                cJSON *message = cJSON_CreateObject();
                cJSON_AddStringToObject(message, "uuid", chat_storage.messages[idx].uuid);
                cJSON_AddStringToObject(message, "username", chat_storage.messages[idx].username);
                cJSON_AddStringToObject(message, "message", chat_storage.messages[idx].message);
                cJSON_AddNumberToObject(message, "timestamp", chat_storage.messages[idx].timestamp);
                cJSON_AddItemToArray(messages_array, message);
                found_new_messages++;
            }
        }

        xSemaphoreGive(chat_mutex);

        // 添加是否有新消息的标志
        cJSON_AddBoolToObject(response, "has_new_messages", found_new_messages > 0);
    }

    // 发送响应
    char *json_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    cJSON_Delete(response);
    free(json_str);

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