/*
 * ESP32聊天消息存储系统实现
 * 主要功能：
 * 1. 提供聊天消息存储和检索功能
 * 2. 使用NVS(非易失性存储)持久化聊天记录
 * 3. 线程安全的消息存储和访问机制
 * 4. 消息缓存机制，减少重复JSON序列化开销
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

// 缓存相关变量
static char *cached_full_messages_json = NULL; // 缓存所有消息的JSON
static uint32_t cached_full_messages_timestamp = 0; // 缓存生成时间戳
static bool cache_valid = false; // 缓存是否有效

// 添加消息计数器，用于批量保存
static int new_messages_count = 0;

// 函数前向声明
static esp_err_t save_message_to_nvs(nvs_handle_t nvs_handle, int index, const chat_message_t *message);
static esp_err_t load_message_from_nvs(nvs_handle_t nvs_handle, int index, chat_message_t *message);
static esp_err_t save_chat_history(void);
static void invalidate_cache(void);
static void save_chat_history_task(void *pvParameters);

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

    // 使用更简单的格式保存，减少JSON开销
    char msg_buf[MAX_UUID_LENGTH + MAX_USERNAME_LENGTH + MAX_MESSAGE_LENGTH + 32];
    snprintf(msg_buf, sizeof(msg_buf), "%s|%s|%s|%lu",
             message->uuid, message->username, message->message, message->timestamp);

    esp_err_t err = nvs_set_str(nvs_handle, key, msg_buf);
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

    // 分配内存并读取字符串
    char *msg_buf = malloc(required_size);
    if (!msg_buf) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(nvs_handle, key, msg_buf, &required_size);
    if (err != ESP_OK) {
        free(msg_buf);
        return err;
    }

    // 尝试解析简单格式
    char *uuid_end = strchr(msg_buf, '|');
    if (uuid_end) {
        *uuid_end = '\0';
        char *username_start = uuid_end + 1;
        char *username_end = strchr(username_start, '|');

        if (username_end) {
            *username_end = '\0';
            char *message_start = username_end + 1;
            char *message_end = strchr(message_start, '|');

            if (message_end) {
                *message_end = '\0';
                char *timestamp_str = message_end + 1;

                // 复制字段到消息结构体
                strlcpy(message->uuid, msg_buf, MAX_UUID_LENGTH);
                strlcpy(message->username, username_start, MAX_USERNAME_LENGTH);
                strlcpy(message->message, message_start, MAX_MESSAGE_LENGTH);
                message->timestamp = (uint32_t)atoi(timestamp_str);

                free(msg_buf);
                return ESP_OK;
            }
        }
    }

    // 如果简单格式解析失败，尝试旧的JSON格式
    cJSON *msg_obj = cJSON_Parse(msg_buf);
    free(msg_buf);

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
        strlcpy(message->username, username_obj->valuestring, MAX_USERNAME_LENGTH);
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
 * @brief 使缓存失效
 *
 * 当添加新消息或其他改变消息状态的操作发生时调用
 */
static void invalidate_cache(void) {
    if (cached_full_messages_json != NULL) {
        free(cached_full_messages_json);
        cached_full_messages_json = NULL;
    }
    cache_valid = false;
    ESP_LOGD(STORAGE_TAG, "Message cache invalidated");
}

/**
 * @brief 获取聊天消息的JSON数组
 *
 * 将当前存储的聊天消息转换为JSON格式字符串，使用缓存减少重复转换
 *
 * @return char* JSON字符串指针，调用者负责释放内存
 * @return NULL 获取失败
 */
char* chat_storage_get_messages_json(void) {
    uint32_t current_time = chat_storage_get_current_time();

    // 如果缓存有效且在有效期内，直接返回缓存的副本
    if (cache_valid && cached_full_messages_json != NULL &&
        (current_time - cached_full_messages_timestamp) < CACHE_VALID_TIME) {
        ESP_LOGD(STORAGE_TAG, "Using cached messages JSON");
        return strdup(cached_full_messages_json);
    }

    // 需要重新生成JSON
    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        ESP_LOGE(STORAGE_TAG, "Failed to create JSON array");
        return NULL;
    }

    // 使用栈上分配的临时数组来避免小消息数量时的内存分配
    int count = 0;
    int start_idx = 0;
    chat_message_t stack_messages[16]; // 对于少量消息使用栈内存
    chat_message_t *temp_messages = NULL;
    bool using_heap = false;

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        count = chat_storage.count;
        if (count > 0) {
            if (count > 16) {
                // 只有当消息数量较多时才使用堆内存
                temp_messages = malloc(count * sizeof(chat_message_t));
                using_heap = true;
                if (!temp_messages) {
                    xSemaphoreGive(chat_mutex);
                    cJSON_Delete(root);
                    ESP_LOGE(STORAGE_TAG, "Failed to allocate memory for messages");
                    return NULL;
                }
            } else {
                // 对于少量消息，使用栈上的临时数组
                temp_messages = stack_messages;
            }

            if (count == MAX_MESSAGES) {
                // 如果消息达到最大容量，则从next_index开始（环形缓冲区的最老消息位置）
                start_idx = chat_storage.next_index;
            }

            // 拷贝消息到临时数组以减少锁定时间
            for (int i = 0; i < count; i++) {
                int idx = (start_idx + i) % MAX_MESSAGES;
                memcpy(&temp_messages[i], &chat_storage.messages[idx], sizeof(chat_message_t));
            }
        }
        xSemaphoreGive(chat_mutex);
    } else {
        cJSON_Delete(root);
        return NULL;
    }

    // 按时间顺序添加所有消息到JSON数组（现在不持锁）
    for (int i = 0; i < count; i++) {
        cJSON *message = cJSON_CreateObject();
        if (message == NULL) {
            ESP_LOGE(STORAGE_TAG, "Failed to create JSON message object");
            if (using_heap && temp_messages) free(temp_messages);
            cJSON_Delete(root);
            return NULL;
        }

        // 添加消息各字段到JSON对象
        cJSON_AddStringToObject(message, "uuid", temp_messages[i].uuid);
        cJSON_AddStringToObject(message, "username", temp_messages[i].username);
        cJSON_AddStringToObject(message, "message", temp_messages[i].message);
        cJSON_AddNumberToObject(message, "timestamp", temp_messages[i].timestamp);
        cJSON_AddItemToArray(root, message);
    }

    // 释放临时消息数组（如果使用的是堆内存）
    if (using_heap && temp_messages) {
        free(temp_messages);
    }

    // 将JSON数组转换为字符串
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str != NULL) {
        // 更新缓存
        invalidate_cache(); // 先清除旧缓存
        cached_full_messages_json = strdup(json_str); // 创建独立副本用于缓存
        if (cached_full_messages_json) {
            cached_full_messages_timestamp = current_time;
            cache_valid = true;
            ESP_LOGD(STORAGE_TAG, "Updated messages JSON cache");
        }
        return json_str; // 返回原始字符串，调用者负责释放
    }

    return NULL; // 如果创建失败，直接返回NULL
}

/**
 * @brief 获取指定时间戳后的消息JSON
 *
 * @param since_timestamp 时间戳
 * @param has_new_messages 输出参数，是否有新消息
 * @return char* JSON字符串指针，调用者负责释放内存
 */
char* chat_storage_get_messages_since_json(uint32_t since_timestamp, bool *has_new_messages) {
    // 初始化输出变量
    if (has_new_messages) {
        *has_new_messages = false;
    }

    if (chat_mutex == NULL) {
        ESP_LOGE(STORAGE_TAG, "Chat mutex is NULL");
        return NULL;
    }

    // 创建响应对象
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        ESP_LOGE(STORAGE_TAG, "Failed to create JSON response");
        return NULL;
    }

    // 创建消息数组
    cJSON *messages_array = cJSON_CreateArray();
    if (!messages_array) {
        ESP_LOGE(STORAGE_TAG, "Failed to create messages array");
        cJSON_Delete(response);
        return NULL;
    }
    cJSON_AddItemToObject(response, "messages", messages_array);

    // 创建临时存储
    int count = 0;
    int start_idx = 0;
    int found_new_messages = 0;
    chat_message_t *messages_copy = NULL;

    // 获取互斥锁，复制所有需要的数据
    BaseType_t mutex_result = xSemaphoreTake(chat_mutex, pdMS_TO_TICKS(1000)); // 添加超时
    if (mutex_result != pdTRUE) {
        ESP_LOGE(STORAGE_TAG, "Failed to take mutex within timeout");
        cJSON_AddBoolToObject(response, "has_new_messages", false);
        cJSON_AddStringToObject(response, "error", "Server busy, try again later");
        char *json_str = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        return json_str;
    }

    // 复制必要的数据，最小化持锁时间
    count = chat_storage.count;
    if (count == MAX_MESSAGES) {
        start_idx = chat_storage.next_index;
    }

    // 只有在有消息时才分配内存
    if (count > 0) {
        messages_copy = malloc(count * sizeof(chat_message_t));
        if (messages_copy) {
            // 复制所有消息
            for (int i = 0; i < count; i++) {
                int idx = (start_idx + i) % MAX_MESSAGES;
                memcpy(&messages_copy[i], &chat_storage.messages[idx], sizeof(chat_message_t));
            }
        } else {
            ESP_LOGE(STORAGE_TAG, "Failed to allocate memory for messages copy");
            // 内存分配失败，立即释放互斥锁
            xSemaphoreGive(chat_mutex);
            cJSON_AddBoolToObject(response, "has_new_messages", false);
            cJSON_AddStringToObject(response, "error", "Server out of memory");
            char *json_str = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);
            return json_str;
        }
    }

    // 尽快释放互斥锁
    xSemaphoreGive(chat_mutex);

    // 现在处理复制的数据，不再持有互斥锁
    if (count > 0 && messages_copy != NULL) {
        for (int i = 0; i < count; i++) {
            // 检查消息是否新于给定时间戳
            if (messages_copy[i].timestamp > since_timestamp) {
                cJSON *message = cJSON_CreateObject();
                if (message) {
                    cJSON_AddStringToObject(message, "uuid", messages_copy[i].uuid);
                    cJSON_AddStringToObject(message, "username", messages_copy[i].username);
                    cJSON_AddStringToObject(message, "message", messages_copy[i].message);
                    cJSON_AddNumberToObject(message, "timestamp", messages_copy[i].timestamp);
                    cJSON_AddItemToArray(messages_array, message);
                    found_new_messages++;
                }
            }
        }

        // 释放临时内存
        free(messages_copy);
    }

    // 设置是否有新消息
    if (has_new_messages) {
        *has_new_messages = (found_new_messages > 0);
    }
    cJSON_AddBoolToObject(response, "has_new_messages", found_new_messages > 0);

    // 生成JSON字符串并清理
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
    // 先获取消息数据，再打开NVS句柄，可减少NVS句柄持有时间
    int count = 0;
    int start_idx = 0;

    // 使用栈内存优化小数据集
    chat_message_t stack_messages[16]; // 栈上的临时缓冲区
    chat_message_t *messages_to_save = NULL;
    bool using_heap = false;

    // 先从聊天存储结构中获取消息数据
    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        count = chat_storage.count;

        // 计算开始索引
        if (count == MAX_MESSAGES) {
            start_idx = chat_storage.next_index;
        }

        // 如果有消息需要保存，则复制数据
        if (count > 0) {
            if (count > 16) {
                // 只有当消息较多时才分配堆内存
                messages_to_save = malloc(count * sizeof(chat_message_t));
                using_heap = true;
                if (!messages_to_save) {
                    ESP_LOGE(STORAGE_TAG, "Failed to allocate memory for messages");
                    xSemaphoreGive(chat_mutex);
                    return ESP_ERR_NO_MEM;
                }
            } else {
                // 对于少量消息使用栈内存
                messages_to_save = stack_messages;
            }

            // 复制消息数据到临时数组
            for (int i = 0; i < count; i++) {
                int msg_idx = (start_idx + i) % MAX_MESSAGES;
                memcpy(&messages_to_save[i], &chat_storage.messages[msg_idx], sizeof(chat_message_t));
            }
        }
        xSemaphoreGive(chat_mutex);
    } else {
        return ESP_FAIL;
    }

    // 如果没有消息，不需要保存
    if (count == 0) {
        ESP_LOGI(STORAGE_TAG, "No messages to save");
        return ESP_OK;
    }

    // 打开NVS进行写入
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("chat", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        if (using_heap && messages_to_save) {
            free(messages_to_save);
        }
        ESP_LOGE(STORAGE_TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // 保存消息计数
    err = nvs_set_i32(nvs_handle, NVS_MSG_COUNT_KEY, count);
    if (err != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Error saving message count: %s", esp_err_to_name(err));
        if (using_heap && messages_to_save) {
            free(messages_to_save);
        }
        nvs_close(nvs_handle);
        return err;
    }

    // 保存每条消息
    bool save_error = false;
    if (count > 0 && messages_to_save != NULL) {
        for (int i = 0; i < count; i++) {
            err = save_message_to_nvs(nvs_handle, i, &messages_to_save[i]);
            if (err != ESP_OK) {
                ESP_LOGE(STORAGE_TAG, "Error saving message %d: %s", i, esp_err_to_name(err));
                save_error = true;
                break;
            }
        }
    }

    // 释放临时数组（如果使用的是堆内存）
    if (using_heap && messages_to_save) {
        free(messages_to_save);
    }

    // 提交更改
    if (!save_error) {
        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(STORAGE_TAG, "Error committing NVS: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(STORAGE_TAG, "Chat history saved successfully (%d messages)", count);
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
    if (!uuid || !username || !message) {
        ESP_LOGE(STORAGE_TAG, "Invalid parameters: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        // 获取当前存储位置
        int idx = chat_storage.next_index;

        // 复制消息数据到存储结构
        strlcpy(chat_storage.messages[idx].uuid, uuid, MAX_UUID_LENGTH);
        strlcpy(chat_storage.messages[idx].username, username, MAX_USERNAME_LENGTH);
        strlcpy(chat_storage.messages[idx].message, message, MAX_MESSAGE_LENGTH);
        chat_storage.messages[idx].timestamp = chat_storage_get_current_time();

        // 更新环形缓冲区指针和消息计数
        chat_storage.next_index = (chat_storage.next_index + 1) % MAX_MESSAGES;
        if (chat_storage.count < MAX_MESSAGES) {
            chat_storage.count++;
        }

        // 增加新消息计数
        new_messages_count++;

        xSemaphoreGive(chat_mutex);

        // 使缓存失效，因为消息已更新
        invalidate_cache();

        // 简化持久化策略：当累积超过MIN_MESSAGES_TO_SAVE条消息时保存
        if (new_messages_count >= MIN_MESSAGES_TO_SAVE) {
            ESP_LOGI(STORAGE_TAG, "Saving chat history after %d new messages", new_messages_count);
            new_messages_count = 0;

            // 使用单独任务保存，避免阻塞主线程
            TaskHandle_t save_task_handle = NULL;
            if (xTaskCreate(save_chat_history_task, "save_chat", 8192, NULL, 5, &save_task_handle) != pdPASS) {
                ESP_LOGW(STORAGE_TAG, "Failed to create save task, saving synchronously");
                save_chat_history();
            }
        }

        return ESP_OK;
    }

    return ESP_FAIL;
}

/**
 * @brief 保存聊天历史的任务函数
 *
 * 在单独的任务中保存聊天历史，避免阻塞主线程
 *
 * @param pvParameters 任务参数（未使用）
 */
static void save_chat_history_task(void *pvParameters) {
    ESP_LOGI(STORAGE_TAG, "Starting chat history save task");
    save_chat_history();
    vTaskDelete(NULL);
}

/**
 * @brief 释放聊天存储系统资源
 *
 * 保存未保存的消息并释放缓存和互斥锁等资源
 */
void chat_storage_deinit(void) {
    ESP_LOGI(STORAGE_TAG, "Deinitializing chat storage...");

    // 确保所有消息都被保存到NVS
    if (new_messages_count > 0) {
        ESP_LOGI(STORAGE_TAG, "Saving %d pending messages before shutdown", new_messages_count);
        save_chat_history();
        new_messages_count = 0;
    }

    // 释放缓存
    invalidate_cache();

    // 释放互斥锁
    if (chat_mutex != NULL) {
        vSemaphoreDelete(chat_mutex);
        chat_mutex = NULL;
    }

    ESP_LOGI(STORAGE_TAG, "Chat storage deinitialized successfully");
}