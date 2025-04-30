/*
 * ESP32匿名聊天服务器实现
 * 主要功能：
 * 1. 通过HTTP服务器提供RESTful API接口
 * 2. 使用SSE(Server-Sent Events)实现实时消息推送
 * 3. 使用NVS(非易失性存储)持久化聊天记录
 * 4. 线程安全的消息存储和访问机制
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
#define SSE_RETRY_TIMEOUT 3000  // SSE客户端重连超时时间(毫秒)
#define MAX_SSE_CLIENTS 10      // 最大SSE客户端连接数
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

/* SSE客户端结构体 */
typedef struct sse_client {
    httpd_handle_t hd;        // HTTP服务器句柄
    int fd;                   // Socket文件描述符
    uint32_t last_activity;   // 最后活动时间(用于检测超时)
    struct sse_client *next;  // 指向下一个客户端的指针
} sse_client_t;

static sse_client_t *sse_clients = NULL; // SSE客户端链表头指针
static SemaphoreHandle_t sse_mutex = NULL; // SSE客户端列表的互斥锁
static int sse_client_count = 0;          // 当前连接的SSE客户端数量

// 在chat_storage_t定义后，添加函数前向声明
static void cleanup_sse_clients(void);
static void add_sse_client(httpd_handle_t hd, int fd);
void remove_sse_client(httpd_handle_t hd, int fd);
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
 * @brief 清理过期或无效的SSE客户端
 *
 * 检查所有SSE客户端连接，移除超过5分钟无活动的客户端
 * 并发送关闭消息通知客户端
 */
static void cleanup_sse_clients(void) {
    uint32_t current_time = (uint32_t)time(NULL);

    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        sse_client_t **client_ptr = &sse_clients;
        while (*client_ptr) {
            sse_client_t *client = *client_ptr;

            // 检查客户端是否超过5分钟没有活动
            if (current_time - client->last_activity > 300) {
                *client_ptr = client->next;
                ESP_LOGI(CHAT_TAG, "Removing inactive SSE client: %d", client->fd);

                // 尝试发送关闭消息，但不依赖发送结果
                int ret = httpd_socket_send(client->hd, client->fd, "event: close\ndata: {}\n\n", 22, 0);
                if (ret < 0) {
                    ESP_LOGW(CHAT_TAG, "Failed to send close event to client %d", client->fd);
                }

                free(client);
                sse_client_count--;
                continue;
            }

            client_ptr = &((*client_ptr)->next);
        }
        xSemaphoreGive(sse_mutex);
    }
}

/**
 * @brief 添加新的SSE客户端
 *
 * @param hd HTTP服务器句柄
 * @param fd 客户端socket文件描述符
 *
 * 注意：如果达到最大客户端限制，新连接将被拒绝
 */
static void add_sse_client(httpd_handle_t hd, int fd) {
    // 首先清理过期客户端
    cleanup_sse_clients();

    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        // 检查是否达到最大客户端限制
        if (sse_client_count >= MAX_SSE_CLIENTS) {
            ESP_LOGW(CHAT_TAG, "Maximum SSE clients reached, rejecting new connection");
            xSemaphoreGive(sse_mutex);
            return;
        }

        sse_client_t *client = malloc(sizeof(sse_client_t));
        if (client) {
            client->hd = hd;
            client->fd = fd;
            client->last_activity = (uint32_t)time(NULL);
            client->next = sse_clients;
            sse_clients = client;
            sse_client_count++;
            ESP_LOGI(CHAT_TAG, "Added SSE client: %d (total: %d)", fd, sse_client_count);
        } else {
            ESP_LOGE(CHAT_TAG, "Failed to allocate memory for SSE client");
        }
        xSemaphoreGive(sse_mutex);
    }
}

/**
 * @brief 移除SSE客户端
 *
 * @param hd HTTP服务器句柄
 * @param fd 客户端socket文件描述符
 *
 * 从客户端链表中移除指定客户端并释放资源
 */
void remove_sse_client(httpd_handle_t hd, int fd) {
    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        sse_client_t **client = &sse_clients;
        while (*client) {
            if ((*client)->fd == fd && (*client)->hd == hd) {
                sse_client_t *to_remove = *client;
                *client = (*client)->next;
                free(to_remove);
                sse_client_count--;
                ESP_LOGI(CHAT_TAG, "Removed SSE client: %d (total: %d)", fd, sse_client_count);
                break;
            }
            client = &(*client)->next;
        }
        xSemaphoreGive(sse_mutex);
    }
}

/**
 * @brief SSE事件处理器
 *
 * 处理客户端的SSE连接请求，设置SSE相关头部，建立长连接，
 * 并负责向客户端发送初始消息和保持连接活跃的ping。
 *
 * @param req HTTP请求对象指针
 * @return ESP_OK 正常处理结束，ESP_FAIL 处理失败
 *
 * 技术特点：
 * 1. 使用Server-Sent Events实现服务器推送技术
 * 2. 支持自动重连和客户端活动检测
 * 3. 设置长连接最大时间限制（10分钟）
 * 4. 使用ping消息保持连接活跃
 */
static esp_err_t sse_handler(httpd_req_t *req) {
    ESP_LOGI(CHAT_TAG, "SSE handler called");

    // 设置SSE必要的HTTP头
    httpd_resp_set_type(req, "text/event-stream"); // SSE内容类型
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache"); // 禁止缓存
    httpd_resp_set_hdr(req, "Connection", "keep-alive"); // 保持连接
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // 允许跨域访问

    // 将客户端添加到SSE客户端列表
    int fd = httpd_req_to_sockfd(req);  // 获取客户端socket文件描述符
    add_sse_client(req->handle, fd);    // 调用函数添加客户端到管理列表

    // 检查是否成功添加客户端（可能因为达到上限而被拒绝）
    bool client_added = false;
    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        sse_client_t *client = sse_clients;
        while (client) {
            if (client->fd == fd) {
                client_added = true;
                break;
            }
            client = client->next;
        }
        xSemaphoreGive(sse_mutex);
    }

    if (!client_added) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Maximum clients reached");
        return ESP_FAIL;
    }

    // 发送初始消息历史记录
    char *messages_json = get_chat_messages_json();
    if (messages_json) {
        char *buffer;
        int asprintf_result = asprintf(&buffer, "event: messages\ndata: %s\n\nretry: %d\n\n",
                     messages_json, SSE_RETRY_TIMEOUT);

        if (asprintf_result != -1) {
            int ret = httpd_socket_send(req->handle, fd, buffer, strlen(buffer), 0);
            free(buffer);

            if (ret < 0) {
                ESP_LOGE(CHAT_TAG, "Failed to send initial messages");
                remove_sse_client(req->handle, fd);
                free(messages_json);
                return ESP_FAIL;
            }
        } else {
            ESP_LOGE(CHAT_TAG, "Failed to allocate memory for SSE message");
            remove_sse_client(req->handle, fd);
            free(messages_json);
            return ESP_FAIL;
        }
        free(messages_json);
    }

    // 保持连接开放 - 减少ping频率以减轻服务器负担
    int ping_count = 0;               // ping计数器
    bool connection_active = true;    // 连接状态标志

    // 注意：如果需要任务看门狗，应在main.c中配置
    // 这里只使用简单的连接检查和超时机制

    // 设置最大连接时间（10分钟）
    uint32_t start_time = (uint32_t)time(NULL);  // 记录连接开始时间
    uint32_t max_connection_time = 600;          // 最大连接时间600秒（10分钟）

    while (connection_active) {
        vTaskDelay(pdMS_TO_TICKS(2000));  // 增加延迟到2秒

        // 检查连接是否仍然有效 - 使用socket发送0字节数据来检测连接状态
        int check_ret = httpd_socket_send(req->handle, fd, NULL, 0, 0);
        if (check_ret < 0) {
            ESP_LOGW(CHAT_TAG, "SSE connection no longer valid, closing");
            break;
        }

        // 检查是否超过最大连接时间
        uint32_t current_time = (uint32_t)time(NULL);
        if (current_time - start_time > max_connection_time) {
            ESP_LOGI(CHAT_TAG, "SSE connection reached maximum time limit (%lu seconds), closing", max_connection_time);
            // 发送关闭消息
            const char *close_msg = "event: close\ndata: {\"reason\":\"timeout\"}\n\n";
            httpd_socket_send(req->handle, fd, close_msg, strlen(close_msg), 0);
            break;
        }

        // 每5次循环发送一次ping（10秒一次）
        if (++ping_count >= 5) {
            // 发送ping保持连接
            const char *ping = "event: ping\ndata: {}\n\nretry: 3000\n\n";
            int ret = httpd_socket_send(req->handle, fd, ping, strlen(ping), 0);
            if (ret < 0) {
                ESP_LOGE(CHAT_TAG, "Failed to send ping, closing connection");
                connection_active = false;
                break;
            }
            ping_count = 0;

            // 同时清理过期客户端
            cleanup_sse_clients();
        }
    }

    // Remove client (should also happen in disconnect handler)
    remove_sse_client(req->handle, fd);
    return ESP_OK;
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
 * @brief 通知所有SSE客户端新消息
 *
 * @param event_name 事件名称，如"message"、"ping"等
 * @param data 事件数据(JSON格式)，包含要推送的具体内容
 *
 * 该函数实现实时消息推送的核心功能：
 * 1. 遍历所有已连接的SSE客户端
 * 2. 构造标准SSE格式的消息（event/data/retry格式）
 * 3. 尝试发送消息，如果失败则移除客户端
 * 4. 更新客户端最后活动时间
 * 5. 自动处理内存管理，确保无内存泄漏
 */
void notify_sse_clients(const char *event_name, const char *data) {
    uint32_t current_time = (uint32_t)time(NULL);

    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        sse_client_t **client_ptr = &sse_clients;
        while (*client_ptr) {
            sse_client_t *client = *client_ptr;

            // 构造SSE消息格式: event: name\ndata: data\n\n
            char *buffer;
            int asprintf_result = asprintf(&buffer, "event: %s\ndata: %s\n\nretry: %d\n\n",
                         event_name, data, SSE_RETRY_TIMEOUT);

            if (asprintf_result != -1) {
                // 尝试发送消息到客户端
                int ret = httpd_socket_send(client->hd, client->fd, buffer, strlen(buffer), 0);
                free(buffer);

                if (ret < 0) {
                    // 发送失败，移除客户端
                    *client_ptr = client->next;
                    ESP_LOGI(CHAT_TAG, "Failed to notify client %d, removing", client->fd);
                    free(client);
                    sse_client_count--;
                    continue;
                } else {
                    // 更新最后活动时间
                    client->last_activity = current_time;
                }
            } else {
                ESP_LOGE(CHAT_TAG, "Failed to allocate memory for SSE notification");
                // 内存分配失败，但不移除客户端，继续处理下一个
            }

            client_ptr = &((*client_ptr)->next);
        }
        xSemaphoreGive(sse_mutex);
    }
}

/**
 * @brief 初始化聊天服务器
 *
 * 创建必要的互斥锁并从NVS加载历史聊天消息
 *
 * 实现细节：
 * 1. 创建用于保护消息存储的互斥锁
 * 2. 创建用于保护SSE客户端列表的互斥锁
 * 3. 从NVS（非易失性存储）加载之前保存的聊天历史
 * 4. 恢复消息计数和存储位置指针
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

    // 创建SSE客户端列表互斥锁，保证客户端管理的线程安全
    sse_mutex = xSemaphoreCreateMutex();
    if (sse_mutex == NULL) {
        ESP_LOGE(CHAT_TAG, "Failed to create SSE mutex");
        vSemaphoreDelete(chat_mutex);
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
 * 接收JSON格式的聊天消息，验证后存储到内存和NVS，并通过SSE通知所有客户端
 *
 * @param req HTTP请求对象，包含消息内容和客户端信息
 * @return ESP_OK 处理成功
 * @return ESP_FAIL 处理失败
 *
 * 功能说明：
 * 1. 验证请求内容长度和JSON格式
 * 2. 检查必填字段(uuid, username, message)和长度限制
 * 3. 调用add_chat_message存储消息
 * 4. 通过notify_sse_clients通知所有连接的客户端
 * 5. 返回适当的HTTP状态码和响应
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
        // Create JSON for the new message
        cJSON *msg_obj = cJSON_CreateObject();
        if (msg_obj == NULL) {
            ESP_LOGE(CHAT_TAG, "Failed to create JSON object");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON");
            cJSON_Delete(root);
            return ESP_FAIL;
        }

        cJSON_AddStringToObject(msg_obj, "uuid", uuid);
        cJSON_AddStringToObject(msg_obj, "username", username);
        cJSON_AddStringToObject(msg_obj, "message", message);
        cJSON_AddNumberToObject(msg_obj, "timestamp", (uint32_t)time(NULL));

        char *msg_json = cJSON_PrintUnformatted(msg_obj);
        cJSON_Delete(msg_obj);

        // Notify all connected clients
        if (msg_json) {
            notify_sse_clients("message", msg_json);
            free(msg_json);
        } else {
            ESP_LOGE(CHAT_TAG, "Failed to print JSON message");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON message");
            cJSON_Delete(root);
            return ESP_FAIL;
        }

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
 * @brief 注册聊天服务器的URI处理函数
 *
 * 为聊天服务器注册所有必要的HTTP请求处理函数，构建RESTful API
 *
 * 注册的处理函数包括：
 * 1. OPTIONS请求处理函数 - 支持CORS预检请求
 * 2. SSE事件流接口 - 实现实时消息推送
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

    // Handler for SSE events - 用于服务器向客户端推送实时消息
    httpd_uri_t sse_uri = {
        .uri = "/api/chat/events",
        .method = HTTP_GET,
        .handler = sse_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sse_uri);

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

/**
 * @brief Socket断开连接处理函数
 *
 * 当HTTP客户端断开连接时，从SSE客户端列表中移除相应的客户端
 * 该函数会被注册到HTTP服务器的close回调
 *
 * @param arg HTTP服务器句柄
 * @param sockfd 断开连接的socket文件描述符
 *
 * 说明：
 * 1. 该函数由HTTP服务器在客户端断开时自动调用
 * 2. 用于清理资源，防止资源泄漏
 * 3. 维护SSE客户端列表的准确性
 */
void chat_disconnect_handler(void* arg, int sockfd) {
    ESP_LOGI(CHAT_TAG, "Client disconnected: %d", sockfd);

    // 获取活动的httpd实例
    httpd_handle_t hd = (httpd_handle_t)arg;
    if (hd != NULL) {
        // 从SSE客户端列表中移除断开的客户端
        remove_sse_client(hd, sockfd);
    }
}