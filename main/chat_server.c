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
    struct sse_client *next;
} sse_client_t;

static sse_client_t *sse_clients = NULL;
static SemaphoreHandle_t sse_mutex = NULL;

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

    size_t required_size = 0;
    err = nvs_get_blob(nvs_handle, "messages", NULL, &required_size);
    if (err == ESP_OK && required_size > 0) {
        if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
            err = nvs_get_blob(nvs_handle, "messages", &chat_storage, &required_size);
            if (err != ESP_OK) {
                ESP_LOGE(CHAT_TAG, "Error loading chat history: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(CHAT_TAG, "Loaded %d messages from NVS", chat_storage.count);
            }
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
        err = nvs_set_blob(nvs_handle, "messages", &chat_storage, sizeof(chat_storage));
        if (err != ESP_OK) {
            ESP_LOGE(CHAT_TAG, "Error saving chat history: %s", esp_err_to_name(err));
        } else {
            err = nvs_commit(nvs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(CHAT_TAG, "Error committing NVS: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(CHAT_TAG, "Chat history saved successfully");
            }
        }
        xSemaphoreGive(chat_mutex);
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

    if (xSemaphoreTake(chat_mutex, portMAX_DELAY) == pdTRUE) {
        int start_idx = 0;
        if (chat_storage.count == MAX_MESSAGES) {
            start_idx = chat_storage.next_index;
        }

        for (int i = 0; i < chat_storage.count; i++) {
            int idx = (start_idx + i) % MAX_MESSAGES;
            cJSON *message = cJSON_CreateObject();
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

// Add a new SSE client
static void add_sse_client(httpd_handle_t hd, int fd) {
    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        sse_client_t *client = malloc(sizeof(sse_client_t));
        if (client) {
            client->hd = hd;
            client->fd = fd;
            client->next = sse_clients;
            sse_clients = client;
            ESP_LOGI(CHAT_TAG, "Added SSE client: %d", fd);
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
                ESP_LOGI(CHAT_TAG, "Removed SSE client: %d", fd);
                break;
            }
            client = &(*client)->next;
        }
        xSemaphoreGive(sse_mutex);
    }
}

// Notify all SSE clients about a new message
void notify_sse_clients(const char *event_name, const char *data) {
    if (xSemaphoreTake(sse_mutex, portMAX_DELAY) == pdTRUE) {
        for (sse_client_t *client = sse_clients; client; client = client->next) {
            // Format: event: name\ndata: data\n\n
            char *buffer;
            if (asprintf(&buffer, "event: %s\ndata: %s\n\nretry: %d\n\n",
                         event_name, data, SSE_RETRY_TIMEOUT) != -1) {
                httpd_socket_send(client->hd, client->fd, buffer, strlen(buffer), 0);
                free(buffer);
            }
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

    // Send initial messages
    char *messages_json = get_chat_messages_json();
    if (messages_json) {
        char *buffer;
        if (asprintf(&buffer, "event: messages\ndata: %s\n\nretry: %d\n\n",
                     messages_json, SSE_RETRY_TIMEOUT) != -1) {
            httpd_socket_send(req->handle, fd, buffer, strlen(buffer), 0);
            free(buffer);
        }
        free(messages_json);
    }

    // Keep the connection open
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Send a ping to keep the connection alive
        const char *ping = "event: ping\ndata: {}\n\nretry: 3000\n\n";
        int ret = httpd_socket_send(req->handle, fd, ping, strlen(ping), 0);
        if (ret < 0) {
            ESP_LOGE(CHAT_TAG, "Failed to send ping, closing connection");
            break;
        }
    }

    // Remove client (should also happen in disconnect handler)
    remove_sse_client(req->handle, fd);
    return ESP_OK;
}

// Handler for posting new chat messages
static esp_err_t post_message_handler(httpd_req_t *req) {
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

    const char *uuid = cJSON_GetObjectItem(root, "uuid")->valuestring;
    const char *username = cJSON_GetObjectItem(root, "username")->valuestring;
    const char *message = cJSON_GetObjectItem(root, "message")->valuestring;

    if (!uuid || !username || !message) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing required fields");
        return ESP_FAIL;
    }

    // Validate message length
    if (strlen(message) > MAX_MESSAGE_LENGTH) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Message too long");
        return ESP_FAIL;
    }

    // Add message to storage
    esp_err_t err = add_chat_message(uuid, username, message);

    if (err == ESP_OK) {
        // Create JSON for the new message
        cJSON *msg_obj = cJSON_CreateObject();
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
void chat_disconnect_handler(void* arg, httpd_handle_t hd, int sockfd) {
    ESP_LOGI(CHAT_TAG, "Client disconnected: %d", sockfd);
    remove_sse_client(hd, sockfd);
}