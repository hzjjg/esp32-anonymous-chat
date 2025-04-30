/*
 * ESP32聊天消息存储系统实现
 * 主要功能：
 * 1. 提供聊天消息存储和检索功能
 * 2. 使用NVS(非易失性存储)持久化聊天记录
 * 3. 线程安全的消息存储和访问机制
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "chat_storage.h"

static const char *STORAGE_TAG = "chat-storage"; // 日志标签
static SemaphoreHandle_t chat_mutex = NULL; // 聊天消息存储的互斥锁

static chat_storage_t chat_storage = {
    .count = 0,
    .next_index = 0
};

// 函数前向声明
static esp_err_t save_message_to_nvs(nvs_handle_t nvs_handle, int index, const chat_message_t *message);
static esp_err_t load_message_from_nvs(nvs_handle_t nvs_handle, int index, chat_message_t *message);
static esp_err_t save_chat_history(void);

/**
 * @brief 保存单条消息到NVS
 * @param nvs_handle NVS句柄
 * @param index 消息索引
 * @param message 要保存的消息结构体指针
 * @return ESP_OK成功，其他为错误码
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
 * @brief 获取当前时间戳
 *
 * @return uint32_t 当前时间戳
 */
uint32_t chat_storage_get_current_time(void) {
    return (uint32_t)time(NULL);
}

/**
 * @brief 获取聊天消息的JSON数组
 *
 * 将当前存储的聊天消息转换为JSON格式字符串
 *
 * @return char* JSON字符串指针，调用者负责释放内存
 * @return NULL 获取失败
 */
char* chat_storage_get_messages_json(void) {
    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        ESP_LOGE(STORAGE_TAG, "Failed to create JSON array");
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
                ESP_LOGE(STORAGE_TAG, "Failed to create JSON message object");
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
 * @brief 获取指定时间戳后的消息JSON
 *
 * @param since_timestamp 时间戳
 * @param has_new_messages 输出参数，是否有新消息
 * @return char* JSON字符串指针，调用者负责释放内存
 */
char* chat_storage_get_messages_since_json(uint32_t since_timestamp, bool *has_new_messages) {
    // 创建JSON数组，准备存储新消息
    cJSON *response = cJSON_CreateObject();
    cJSON *messages_array = cJSON_CreateArray();
    cJSON_AddItemToObject(response, "messages", messages_array);

    // 获取当前时间作为响应时间戳
    uint32_t current_time = chat_storage_get_current_time();
    cJSON_AddNumberToObject(response, "server_time", current_time);

    int found_new_messages = 0;

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        int start_idx = 0;
        if (chat_storage.count == MAX_MESSAGES) {
            start_idx = chat_storage.next_index;
        }

        // 遍历所有消息，寻找比since_timestamp更新的消息
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
    }

    // 添加是否有新消息的标志
    *has_new_messages = (found_new_messages > 0);
    cJSON_AddBoolToObject(response, "has_new_messages", found_new_messages > 0);

    // 发送响应
    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    return json_str;
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
        ESP_LOGE(STORAGE_TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        // 保存消息计数
        err = nvs_set_i32(nvs_handle, NVS_MSG_COUNT_KEY, chat_storage.count);
        if (err != ESP_OK) {
            ESP_LOGE(STORAGE_TAG, "Error saving message count: %s", esp_err_to_name(err));
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
                ESP_LOGE(STORAGE_TAG, "Error saving message %d: %s", i, esp_err_to_name(err));
                save_error = true;
                break;
            }
        }

        xSemaphoreGive(chat_mutex);

        if (!save_error) {
            err = nvs_commit(nvs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(STORAGE_TAG, "Error committing NVS: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(STORAGE_TAG, "Chat history saved successfully (%d messages)", chat_storage.count);
            }
        }
    }

    nvs_close(nvs_handle);
    return err;
}

/**
 * @brief 初始化聊天存储系统
 *
 * 创建互斥锁并从NVS加载历史聊天消息
 *
 * @return ESP_OK 成功，其他为错误码
 */
esp_err_t chat_storage_init(void) {
    // 创建消息互斥锁，保证消息读写的线程安全
    chat_mutex = xSemaphoreCreateMutex();
    if (chat_mutex == NULL) {
        ESP_LOGE(STORAGE_TAG, "Failed to create chat mutex");
        return ESP_FAIL;
    }

    // 从NVS加载聊天历史记录
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("chat", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
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
                    ESP_LOGW(STORAGE_TAG, "Failed to load message %d: %s", i, esp_err_to_name(err));
                }
            }

            // 更新消息计数和下一个存储位置
            chat_storage.count = loaded_count;
            chat_storage.next_index = loaded_count % MAX_MESSAGES;
            ESP_LOGI(STORAGE_TAG, "Loaded %d messages from NVS", loaded_count);

            xSemaphoreGive(chat_mutex);
        }
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

/**
 * @brief 添加新的聊天消息
 *
 * 将新消息添加到环形缓冲区，并持久化存储到NVS
 *
 * @param uuid 用户唯一标识符
 * @param username 用户名
 * @param message 消息内容
 * @return ESP_OK 添加成功
 * @return ESP_FAIL 添加失败
 */
esp_err_t chat_storage_add_message(const char *uuid, const char *username, const char *message) {
    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        // 获取当前存储位置
        int idx = chat_storage.next_index;

        // 复制消息数据到存储结构
        strlcpy(chat_storage.messages[idx].uuid, uuid, MAX_UUID_LENGTH);
        strlcpy(chat_storage.messages[idx].username, username, sizeof(chat_storage.messages[idx].username));
        strlcpy(chat_storage.messages[idx].message, message, MAX_MESSAGE_LENGTH);
        chat_storage.messages[idx].timestamp = chat_storage_get_current_time();

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