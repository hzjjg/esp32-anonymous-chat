/* HTTP Restful API Server

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>          // 提供字符串操作函数，如strcpy、strcmp等
#include <fcntl.h>           // 提供文件控制选项，用于文件操作的标志如O_RDONLY
#include "esp_http_server.h" // ESP32的HTTP服务器库，提供创建和管理HTTP服务器的功能
#include "esp_chip_info.h"   // 提供获取ESP32芯片信息的功能，如芯片型号、核心数等
#include "esp_random.h"      // 提供随机数生成功能
#include "esp_log.h"         // ESP32的日志系统，用于输出调试和信息日志
#include "esp_vfs.h"         // 虚拟文件系统接口，用于文件操作
#include "cJSON.h"           // 轻量级JSON解析和生成库，用于处理JSON数据
#include "chat_server.h"     // 包含聊天服务器相关的函数声明
#include "chat_storage.h"    // 包含聊天存储相关的函数声明

static const char *REST_TAG = "esp-rest"; // 定义日志标签，用于ESP日志系统
static httpd_handle_t server_instance = NULL; // 存储服务器实例句柄
static rest_server_context_t *rest_context = NULL; // 存储REST上下文

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
    rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    // 复制根目录路径到上下文
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); // 获取默认HTTP服务器配置
    config.max_open_sockets = 7; // 将最大并发连接数从16修改为7，以符合系统限制
    config.uri_match_fn = httpd_uri_match_wildcard; // 启用通配符URI匹配，支持模式如/api/*

    ESP_LOGI(REST_TAG, "Starting HTTP Server");
    // 启动HTTP服务器
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    // 保存服务器实例句柄以便于后续停止服务器
    server_instance = server;

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

    // 注册聊天服务器相关的URI处理函数
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
    rest_context = NULL;
err: // 其他错误的错误处理标签
    return ESP_FAIL;
}

/**
 * @brief 停止RESTful API服务器并释放资源
 *
 * 停止HTTP服务器，释放分配的资源，确保聊天存储系统正确清理
 *
 * @return esp_err_t ESP_OK表示成功，ESP_FAIL表示失败
 */
esp_err_t stop_rest_server(void)
{
    esp_err_t err = ESP_OK;

    // 如果服务器实例存在，停止它
    if (server_instance != NULL) {
        ESP_LOGI(REST_TAG, "Stopping HTTP Server");
        err = httpd_stop(server_instance);
        if (err != ESP_OK) {
            ESP_LOGE(REST_TAG, "Failed to stop HTTP server: %s", esp_err_to_name(err));
        }
        server_instance = NULL;
    }

    // 释放REST上下文资源
    if (rest_context != NULL) {
        free(rest_context);
        rest_context = NULL;
    }

    // 清理聊天存储系统，确保所有消息都被保存
    chat_storage_deinit();

    ESP_LOGI(REST_TAG, "HTTP Server stopped and resources released");
    return err;
}
