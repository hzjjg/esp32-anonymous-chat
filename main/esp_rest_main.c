/* HTTP Restful API Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_vfs_semihost.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "protocol_examples_common.h" // 包含网络连接相关的通用函数
#include "chat_server.h" // 包含聊天服务器相关的函数声明
#if CONFIG_EXAMPLE_WEB_DEPLOY_SD
#include "driver/sdmmc_host.h" // 如果配置为从SD卡部署Web，则包含SDMMC主机驱动
#endif

#define MDNS_INSTANCE "esp chat server" // 定义mDNS服务实例名称

static const char *TAG = "example"; // 定义日志标签

// 声明启动RESTful服务器的函数
esp_err_t start_rest_server(const char *base_path);

/**
 * @brief 初始化mDNS服务
 *
 * 设置mDNS主机名和实例名，并添加HTTP服务。
 */
static void initialise_mdns(void)
{
    // 初始化mDNS服务
    mdns_init();
    // 设置mDNS主机名，从menuconfig配置中读取
    mdns_hostname_set(CONFIG_EXAMPLE_MDNS_HOST_NAME);
    // 设置mDNS实例名
    mdns_instance_name_set(MDNS_INSTANCE);

    // 定义mDNS服务的TXT记录
    mdns_txt_item_t serviceTxtData[] = {
        {"board", "esp32"}, // 板子类型
        {"path", "/"}      // 服务路径
    };

    // 添加HTTP服务到mDNS
    ESP_ERROR_CHECK(mdns_service_add("ESP32-ChatServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

// 根据配置选择文件系统初始化方式
#if CONFIG_EXAMPLE_WEB_DEPLOY_SEMIHOST
/**
 * @brief 初始化文件系统（Semihost模式）
 *
 * 注册Semihost虚拟文件系统驱动，允许通过JTAG访问主机文件系统。
 * @return esp_err_t ESP_OK表示成功，ESP_FAIL表示失败。
 */
esp_err_t init_fs(void)
{
    // 注册Semihost VFS驱动，挂载点从menuconfig配置中读取
    esp_err_t ret = esp_vfs_semihost_register(CONFIG_EXAMPLE_WEB_MOUNT_POINT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register semihost driver (%s)!", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_SD
/**
 * @brief 初始化文件系统（SD卡模式）
 *
 * 初始化SD卡，并挂载FAT文件系统。
 * @return esp_err_t ESP_OK表示成功，ESP_FAIL表示失败。
 */
esp_err_t init_fs(void)
{
    // SDMMC主机配置，使用默认设置
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // SDMMC插槽配置，使用默认设置
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // 为SD卡引脚设置上拉电阻
    gpio_set_pull_mode(15, GPIO_PULLUP_ONLY); // CMD
    gpio_set_pull_mode(2, GPIO_PULLUP_ONLY);  // D0
    gpio_set_pull_mode(4, GPIO_PULLUP_ONLY);  // D1
    gpio_set_pull_mode(12, GPIO_PULLUP_ONLY); // D2
    gpio_set_pull_mode(13, GPIO_PULLUP_ONLY); // D3

    // FAT文件系统挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, // 如果挂载失败，则格式化SD卡
        .max_files = 4,                 // 最大打开文件数
        .allocation_unit_size = 16 * 1024 // 分配单元大小
    };

    sdmmc_card_t *card; // SD卡信息结构体指针
    // 挂载SD卡FAT文件系统
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_EXAMPLE_WEB_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }
    // 如果挂载成功，打印SD卡信息
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_SF
/**
 * @brief 初始化文件系统（SPIFFS模式）
 *
 * 初始化并注册SPIFFS文件系统。
 * @return esp_err_t ESP_OK表示成功，ESP_FAIL表示失败。
 */
esp_err_t init_fs(void)
{
    // SPIFFS文件系统配置
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_EXAMPLE_WEB_MOUNT_POINT, // 挂载点路径，从menuconfig配置中读取
        .partition_label = NULL,                    // 使用默认分区
        .max_files = 5,                             // 最大打开文件数
        .format_if_mount_failed = false             // 如果挂载失败，不格式化
    };
    // 注册SPIFFS文件系统
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    // 获取SPIFFS分区信息
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}
#endif

/**
 * @brief 应用程序主入口函数
 *
 * 初始化NVS、网络、mDNS、NetBIOS、文件系统，连接网络，
 * 初始化聊天服务器，并启动RESTful API服务器。
 */
void app_main(void)
{
    // 初始化NVS（非易失性存储）
    ESP_ERROR_CHECK(nvs_flash_init());
    // 初始化TCP/IP协议栈
    ESP_ERROR_CHECK(esp_netif_init());
    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // 初始化mDNS服务
    initialise_mdns();
    // 初始化NetBIOS服务
    netbiosns_init();
    // 设置NetBIOS名称，从menuconfig配置中读取
    netbiosns_set_name(CONFIG_EXAMPLE_MDNS_HOST_NAME);

    // 连接到网络（Wi-Fi或以太网，根据配置）
    ESP_ERROR_CHECK(example_connect());
    // 初始化文件系统（根据配置选择Semihost、SD卡或SPIFFS）
    ESP_ERROR_CHECK(init_fs());

    // 初始化聊天服务器
    ESP_ERROR_CHECK(chat_server_init());

    // 启动RESTful API服务器，Web根目录从menuconfig配置中读取
    ESP_ERROR_CHECK(start_rest_server(CONFIG_EXAMPLE_WEB_MOUNT_POINT));
}
