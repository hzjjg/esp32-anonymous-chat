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

#define MAX_MESSAGES 100
#define MAX_MESSAGE_LENGTH 150
#define MAX_UUID_LENGTH 37  // 36 chars + null terminator
#define SSE_RETRY_TIMEOUT 3000 // milliseconds
#define MAX_SSE_CLIENTS 10     // 限制最大SSE客户端数量
#define NVS_MSG_KEY_PREFIX "msg_"  // NVS消息键前缀
#define NVS_MSG_COUNT_KEY "msg_count" // 存储消息计数的键

static const char *CHAT_TAG = "chat-server";
static SemaphoreHandle_t chat_mutex = NULL;

typedef struct {
    char uuid[MAX_UUID_LENGTH];
    char username[32];
    char message[MAX_MESSAGE_LENGTH];
    uint32_t timestamp;
} chat_message_t;

typedef struct {
    chat_message_t messages[MAX_MESSAGES];
    int count;
    int next_index;
} chat_storage_t;

static chat_storage_t chat_storage = {
    .count = 0,
    .next_index = 0
};

// List of connected SSE clients
typedef struct sse_client {
    httpd_handle_t hd;
    int fd;
    uint32_t last_activity;   // 添加最后活动时间
    struct sse_client *next;
} sse_client_t;

static sse_client_t *sse_clients = NULL;
static SemaphoreHandle_t sse_mutex = NULL;
static int sse_client_count = 0; // 跟踪SSE客户端计数

// Generate UUID v4
static void generate_uuid(char *uuid_str) {
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));

    // Set version to 4 (random UUID)
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    // Set variant to DCE 1.1
    uuid[8] = (uuid[8] & 0x3F) | 0x80;

    snprintf(uuid_str, MAX_UUID_LENGTH,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11],
             uuid[12], uuid[13], uuid[14], uuid[15]);
}

// 从NVS中保存单条消息
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

// 从NVS中加载单条消息
static esp_err_t load_message_from_nvs(nvs_handle_t nvs_handle, int index, chat_message_t *message) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_MSG_KEY_PREFIX, index);

    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvs_handle, key, NULL, &required_size);
    if (err != ESP_OK) {
        return err;
    }

    char *msg_json = malloc(required_size);
    if (!msg_json) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(nvs_handle, key, msg_json, &required_size);
    if (err != ESP_OK) {
        free(msg_json);
        return err;
    }

    cJSON *msg_obj = cJSON_Parse(msg_json);
    free(msg_json);

    if (!msg_obj) {
        return ESP_FAIL;
    }

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

// Initialize chat server
esp_err_t chat_server_init(void) {
    chat_mutex = xSemaphoreCreateMutex();
    if (chat_mutex == NULL) {
        ESP_LOGE(CHAT_TAG, "Failed to create chat mutex");
        return ESP_FAIL;
    }

    sse_mutex = xSemaphoreCreateMutex();
    if (sse_mutex == NULL) {
        ESP_LOGE(CHAT_TAG, "Failed to create SSE mutex");
        vSemaphoreDelete(chat_mutex);
        return ESP_FAIL;
    }

    // Load chat history from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("chat", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(CHAT_TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return ESP_OK; // Continue without loading
    }

    // 获取消息数量
    int32_t msg_count = 0;
    err = nvs_get_i32(nvs_handle, NVS_MSG_COUNT_KEY, &msg_count);
    if (err == ESP_OK && msg_count > 0) {
        if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
            // 加载每条消息
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

            chat_storage.count = loaded_count;
            chat_storage.next_index = loaded_count % MAX_MESSAGES;
            ESP_LOGI(CHAT_TAG, "Loaded %d messages from NVS", loaded_count);

            xSemaphoreGive(chat_mutex);
        }
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

// Save chat history to NVS
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

// Add a new chat message
esp_err_t add_chat_message(const char *uuid, const char *username, const char *message) {
    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        int idx = chat_storage.next_index;

        strlcpy(chat_storage.messages[idx].uuid, uuid, MAX_UUID_LENGTH);
        strlcpy(chat_storage.messages[idx].username, username, sizeof(chat_storage.messages[idx].username));
        strlcpy(chat_storage.messages[idx].message, message, MAX_MESSAGE_LENGTH);
        chat_storage.messages[idx].timestamp = (uint32_t)time(NULL);

        chat_storage.next_index = (chat_storage.next_index + 1) % MAX_MESSAGES;
        if (chat_storage.count < MAX_MESSAGES) {
            chat_storage.count++;
        }

        xSemaphoreGive(chat_mutex);

        // Persist to NVS
        save_chat_history();

        return ESP_OK;
    }

    return ESP_FAIL;
}

// Get chat messages as JSON array
char* get_chat_messages_json(void) {
    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        ESP_LOGE(CHAT_TAG, "Failed to create JSON array");
        return NULL;
    }

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        int start_idx = 0;
        if (chat_storage.count == MAX_MESSAGES) {
            start_idx = chat_storage.next_index;
        }

        for (int i = 0; i < chat_storage.count; i++) {
            int idx = (start_idx + i) % MAX_MESSAGES;
            cJSON *message = cJSON_CreateObject();
            if (message == NULL) {
                ESP_LOGE(CHAT_TAG, "Failed to create JSON message object");
                xSemaphoreGive(chat_mutex);
                cJSON_Delete(root);
                return NULL;
            }
            
            cJSON_AddStringToObject(message, "uuid", chat_storage.messages[idx].uuid);
            cJSON_AddStringToObject(message, "username", chat_storage.messages[idx].username);
            cJSON_AddStringToObject(message, "message", chat_storage.messages[idx].message);
            cJSON_AddNumberToObject(message, "timestamp", chat_storage.messages[idx].timestamp);
            cJSON_AddItemToArray(root, message);
        }

        xSemaphoreGive(chat_mutex);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

// 清理过期或无效的SSE客户端
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

// Add a new SSE client
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

// Remove an SSE client
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

// Notify all SSE clients about a new message
void notify_sse_clients(const char *event_name, const char *data) {
    uint32_t current_time = (uint32_t)time(NULL);

    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        sse_client_t **client_ptr = &sse_clients;
        while (*client_ptr) {
            sse_client_t *client = *client_ptr;

            // Format: event: name\ndata: data\n\n
            char *buffer;
            int asprintf_result = asprintf(&buffer, "event: %s\ndata: %s\n\nretry: %d\n\n",
                         event_name, data, SSE_RETRY_TIMEOUT);
            
            if (asprintf_result != -1) {
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

// SSE event handler
static esp_err_t sse_handler(httpd_req_t *req) {
    ESP_LOGI(CHAT_TAG, "SSE handler called");

    // Set headers for SSE
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Add this client to the SSE clients list
    int fd = httpd_req_to_sockfd(req);
    add_sse_client(req->handle, fd);

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

    // Send initial messages
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

    // Keep the connection open - 减少ping频率以减轻服务器负担
    int ping_count = 0;
    bool connection_active = true;
    
    // 注意：如果需要任务看门狗，应在main.c中配置
    // 这里只使用简单的连接检查和超时机制
    
    // 设置最大连接时间（10分钟）
    uint32_t start_time = (uint32_t)time(NULL);
    uint32_t max_connection_time = 600; // 10分钟
    
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

// 添加OPTIONS请求处理程序，帮助处理CORS
static esp_err_t options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Handler for posting new chat messages
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

// Handler for generating UUID
static esp_err_t generate_uuid_handler(httpd_req_t *req) {
    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char uuid[MAX_UUID_LENGTH];
    generate_uuid(uuid);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "uuid", uuid);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// Register chat server URI handlers
esp_err_t register_chat_uri_handlers(httpd_handle_t server) {
    // Handler for OPTIONS requests (CORS)
    httpd_uri_t options_uri = {
        .uri = "/api/chat/*",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &options_uri);

    // Handler for SSE events
    httpd_uri_t sse_uri = {
        .uri = "/api/chat/events",
        .method = HTTP_GET,
        .handler = sse_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sse_uri);

    // Handler for posting messages
    httpd_uri_t post_message_uri = {
        .uri = "/api/chat/message",
        .method = HTTP_POST,
        .handler = post_message_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &post_message_uri);

    // Handler for generating UUID
    httpd_uri_t generate_uuid_uri = {
        .uri = "/api/chat/uuid",
        .method = HTTP_GET,
        .handler = generate_uuid_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &generate_uuid_uri);

    return ESP_OK;
}

// Socket disconnect handler (to be registered with httpd)
void chat_disconnect_handler(void* arg, int sockfd) {
    ESP_LOGI(CHAT_TAG, "Client disconnected: %d", sockfd);

    // 获取活动的httpd实例
    httpd_handle_t hd = (httpd_handle_t)arg;
    if (hd != NULL) {
        remove_sse_client(hd, sockfd);
    }
}