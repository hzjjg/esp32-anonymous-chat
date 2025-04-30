/* HTTP Restful API Server

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <fcntl.h>
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "cJSON.h"
#include "chat_server.h" // 包含聊天服务器相关的函数声明

static const char *REST_TAG = "esp-rest"; // 定义日志标签，用于ESP日志系统

// 检查宏，用于检查条件a，如果为假，则打印错误日志并跳转到goto_tag
// 这是一个辅助宏，简化错误处理流程，提高代码可读性
#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(REST_TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128) // 定义文件路径最大长度，ESP_VFS_PATH_MAX是ESP-IDF定义的文件系统路径最大长度
#define SCRATCH_BUFSIZE (10240)                // 定义临时缓冲区大小，用于读写文件和处理HTTP请求

// REST服务器上下文结构体，存储服务器运行时需要的状态和数据
typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1]; // Web服务器根目录路径，存储静态文件的位置
    char scratch[SCRATCH_BUFSIZE];        // 临时缓冲区，用于读写文件和请求体
} rest_server_context_t;

// 检查文件扩展名的宏，忽略大小写，用于根据文件类型设置正确的Content-Type
#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

/**
 * @brief 根据文件扩展名设置HTTP响应的Content-Type
 *
 * @param req HTTP请求对象指针
 * @param filepath 文件路径
 * @return esp_err_t ESP_OK表示成功
 *
 * 分析文件扩展名并设置合适的MIME类型，确保浏览器能正确解析响应内容
 */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain"; // 默认为纯文本类型
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml"; // 注意：SVG的正确MIME类型通常是 image/svg+xml，这里可能需要修正
    }
    return httpd_resp_set_type(req, type);
}

/**
 * @brief 通用GET请求处理函数，用于发送文件内容
 *
 * 处理对Web服务器根目录下文件的GET请求，读取文件内容并作为HTTP响应发送。
 * 如果请求URI是'/'，则默认发送index.html。
 *
 * @param req HTTP请求对象指针
 * @return esp_err_t ESP_OK表示成功，ESP_FAIL表示失败
 *
 * 该函数是一个通用的静态文件服务器实现，适用于HTML/CSS/JS等网页资源
 */
static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    // 获取REST服务器上下文，包含了服务器配置和临时缓冲区
    rest_server_context_t *rest_context = (rest_server_context_t *)req->user_ctx;
    // 构建完整的文件路径，组合基础路径和URI
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') { // 如果URI以'/'结尾
        strlcat(filepath, "/index.html", sizeof(filepath)); // 默认请求index.html
    } else {
        strlcat(filepath, req->uri, sizeof(filepath)); // 否则使用URI作为相对路径
    }
    // 打开文件，以只读模式
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(REST_TAG, "Failed to open file : %s", filepath);
        // 文件打开失败，发送500错误
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    // 根据文件扩展名设置Content-Type
    set_content_type_from_file(req, filepath);

    // 使用临时缓冲区读取和发送文件内容
    char *chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        // 分块读取文件，提高内存使用效率
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(REST_TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            // 发送文件块作为HTTP响应，使用httpd_resp_send_chunk支持大文件传输
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(REST_TAG, "File sending failed!");
                // 发送失败，中止发送并发送500错误
                httpd_resp_sendstr_chunk(req, NULL);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    // 文件发送完成，关闭文件描述符
    close(fd);
    ESP_LOGI(REST_TAG, "File sending complete");
    // 发送空块表示响应结束，是HTTP分块传输的结束标记
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief 处理灯光亮度控制的POST请求（示例）
 *
 * 接收包含RGB值的JSON数据，并打印日志。实际应用中可以控制LED灯的亮度和颜色。
 *
 * @param req HTTP请求对象指针
 * @return esp_err_t ESP_OK表示成功，ESP_FAIL表示失败
 *
 * 这是一个示例函数，演示如何接收和处理JSON格式的POST请求数据
 */
static esp_err_t light_brightness_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len; // 获取请求体总长度
    int cur_len = 0;
    char *buf = ((rest_server_context_t *)(req->user_ctx))->scratch; // 使用临时缓冲区存储请求数据
    int received = 0;
    // 检查请求体是否过大，防止缓冲区溢出
    if (total_len >= SCRATCH_BUFSIZE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    // 循环接收请求体数据，处理分块传输的情况
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0'; // 添加字符串结束符，方便后续处理

    // 解析JSON数据，使用cJSON库
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(REST_TAG, "Failed to parse JSON: %s", buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        return ESP_FAIL;
    }
    // 获取RGB值
    cJSON *red_obj = cJSON_GetObjectItem(root, "red");
    cJSON *green_obj = cJSON_GetObjectItem(root, "green");
    cJSON *blue_obj = cJSON_GetObjectItem(root, "blue");

    // 验证JSON结构和数据类型
    if (!red_obj || !green_obj || !blue_obj || !cJSON_IsNumber(red_obj) || !cJSON_IsNumber(green_obj) || !cJSON_IsNumber(blue_obj)) {
        ESP_LOGE(REST_TAG, "Invalid JSON structure: %s", buf);
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid RGB values");
        return ESP_FAIL;
    }

    // 提取RGB值，在实际应用中可用于控制LED灯
    int red = red_obj->valueint;
    int green = green_obj->valueint;
    int blue = blue_obj->valueint;
    ESP_LOGI(REST_TAG, "Light control: red = %d, green = %d, blue = %d", red, green, blue);
    cJSON_Delete(root);
    // 发送成功响应
    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

/**
 * @brief 处理获取系统信息的GET请求
 *
 * 返回包含ESP-IDF版本和芯片核心数的JSON响应，可用于前端显示系统信息。
 *
 * @param req HTTP请求对象指针
 * @return esp_err_t ESP_OK表示成功
 *
 * 该函数演示如何构建和返回JSON格式的响应数据
 */
static esp_err_t system_info_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json"); // 设置响应类型为JSON
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info); // 获取芯片信息
    cJSON_AddStringToObject(root, "version", IDF_VER); // 添加IDF版本
    cJSON_AddNumberToObject(root, "cores", chip_info.cores); // 添加核心数
    const char *sys_info = cJSON_Print(root); // 将JSON对象转换为字符串
    httpd_resp_sendstr(req, sys_info); // 发送JSON字符串
    free((void *)sys_info); // 释放JSON字符串内存
    cJSON_Delete(root); // 删除JSON对象，释放内存
    return ESP_OK;
}

/**
 * @brief 处理获取温度数据的GET请求（示例）
 *
 * 返回包含随机温度值的JSON响应。在实际应用中，可以连接温度传感器并返回真实数据。
 *
 * @param req HTTP请求对象指针
 * @return esp_err_t ESP_OK表示成功
 *
 * 这是一个示例函数，演示如何返回传感器数据的API接口
 */
static esp_err_t temperature_data_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json"); // 设置响应类型为JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "raw", esp_random() % 20); // 添加一个随机数作为温度值
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 启动RESTful API服务器
 *
 * 初始化并启动HTTP服务器，注册各个URI的处理函数。
 *
 * @param base_path Web服务器的根目录路径，用于存储静态文件
 * @return esp_err_t ESP_OK表示成功，ESP_FAIL表示失败
 *
 * 该函数是REST服务器初始化的入口点，负责注册所有API路由和处理函数
 */
esp_err_t start_rest_server(const char *base_path)
{
    REST_CHECK(base_path, "wrong base path", err); // 检查base_path是否有效
    // 分配REST服务器上下文内存
    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    // 复制根目录路径到上下文
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); // 获取默认HTTP服务器配置
    config.max_open_sockets = 16; // 增加最大并发连接数，以支持更多SSE客户端，默认值通常较小
    config.uri_match_fn = httpd_uri_match_wildcard; // 启用通配符URI匹配，支持模式如/api/*

    // 添加断开连接的处理程序，用于清理聊天客户端列表
    config.lru_purge_enable = true; // 启用LRU连接清理，在连接数达到上限时自动关闭最不活跃的连接
    config.open_fn = NULL; // 连接建立时的回调，这里未使用
    config.close_fn = chat_disconnect_handler; // 连接关闭时的回调，用于移除SSE客户端

    ESP_LOGI(REST_TAG, "Starting HTTP Server");
    // 启动HTTP服务器
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    /* 注册系统信息API路由 */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/v1/system/info", // URI路径
        .method = HTTP_GET,           // HTTP方法
        .handler = system_info_get_handler, // 处理函数指针
        .user_ctx = rest_context      // 用户上下文，传递给处理函数
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* 注册温度数据API路由 */
    httpd_uri_t temperature_data_get_uri = {
        .uri = "/api/v1/temp/raw",
        .method = HTTP_GET,
        .handler = temperature_data_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &temperature_data_get_uri);

    /* 注册灯光控制API路由 */
    httpd_uri_t light_brightness_post_uri = {
        .uri = "/api/v1/light/brightness",
        .method = HTTP_POST,
        .handler = light_brightness_post_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &light_brightness_post_uri);

    // 注册聊天服务器相关的URI处理函数 (SSE事件流, POST消息, GET UUID)
    register_chat_uri_handlers(server);

    /* 注册通用文件处理路由(通配符匹配) */
    httpd_uri_t common_get_uri = {
        .uri = "/*", // 通配符，匹配所有其他GET请求，必须放在最后注册
        .method = HTTP_GET,
        .handler = rest_common_get_handler, // 使用通用文件发送处理函数
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    return ESP_OK;
err_start: // 启动服务器失败的错误处理标签
    free(rest_context);
err: // 其他错误的错误处理标签
    return ESP_FAIL;
}
