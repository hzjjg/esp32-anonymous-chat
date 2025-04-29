| 支持的目标 | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- | -------- |

# HTTP RESTful API 服务器示例

（有关示例的更多信息，请参阅上一级'examples'目录中的README.md文件。）

## ESP32 开发教程

https://blog.duruofu.top/docs/03.Embedded/ESP32/2025-ESP32-Guide/README.html

## 概述

本示例主要介绍如何在ESP32上实现RESTful API服务器和HTTP服务器，并提供前端浏览器界面。

本示例设计了几个用于获取资源的API，如下所示：

| API                        | 方法   | 资源示例                                             | 描述                                                    | 页面URL |
| -------------------------- | ------ | ---------------------------------------------------- | ------------------------------------------------------- | ------- |
| `/api/v1/system/info`      | `GET`  | {<br />version:"v4.0-dev",<br />cores:2<br />}        | 客户端用于获取系统信息，如IDF版本、ESP32核心数等        | `/`     |
| `/api/v1/temp/raw`         | `GET`  | {<br />raw:22<br />}                                  | 客户端用于获取从传感器读取的原始温度数据                | `/chart` |
| `/api/v1/light/brightness` | `POST` | { <br />red:160,<br />green:160,<br />blue:160<br />} | 客户端用于向ESP32上传控制值以控制LED亮度                | `/light` |

**页面URL**是将向API发送请求的网页的URL。

### 关于mDNS

物联网设备的IP地址可能会不时变化，因此在网页中硬编码IP地址是不切实际的。在本示例中，我们使用`mDNS`解析域名`esp-home.local`，这样无论背后的实际IP地址是什么，我们都可以通过此URL访问Web服务器。有关mDNS的更多信息，请参见[此处](https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/protocols/mdns.html)。

**注意：mDNS默认安装在大多数操作系统上，或作为单独的软件包提供。**

### 关于部署模式

在开发模式下，每次更新html、js或css文件时都刷新整个网页将非常麻烦。因此，强烈建议通过`semihost`技术将网页部署到主机PC上。当浏览器获取网页时，ESP32可以转发位于主机PC上的所需文件。通过这种方式，在设计新页面时可以节省大量时间。

开发完成后，页面应部署到以下目的地之一：

* SPI闪存 - 当构建后的网站较小时推荐使用（例如小于2MB）。
* SD卡 - 当构建后的网站非常大，SPI闪存没有足够空间容纳时可以选择（例如大于2MB）。

### 关于前端框架

许多著名的前端框架（如Vue、React、Angular）可以在本示例中使用。这里我们仅以[Vue](https://vuejs.org/)为例，并采用[vuetify](https://vuetifyjs.com/)作为UI库。

## 如何使用示例

### 所需硬件

要运行此示例，您需要ESP32开发板（如ESP32-WROVER Kit、ESP32-Ethernet-Kit）或ESP32核心板（如ESP32-DevKitC）。如果选择通过semihosting部署网站，可能还需要额外的JTAG适配器。有关支持的JTAG适配器的更多信息，请参考[选择JTAG适配器](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/jtag-debugging/index.html#jtag-debugging-selecting-jtag-adapter)。或者，如果选择将网站部署到SD卡，则需要额外的SD插槽板。

#### 引脚分配：

仅当您将网站部署到SD卡时，本示例中使用以下引脚连接。

| ESP32  | SD卡   |
| ------ | ------- |
| GPIO2  | D0      |
| GPIO4  | D1      |
| GPIO12 | D2      |
| GPIO13 | D3      |
| GPIO14 | CLK     |
| GPIO15 | CMD     |


### 配置项目

打开项目配置菜单（`idf.py menuconfig`）。

在`Example Connection Configuration`菜单中：

* 根据您的开发板选择`Connect using`选项中的网络接口。目前我们支持Wi-Fi和以太网。
* 如果选择Wi-Fi接口，还需要设置：
  * ESP32将连接的Wi-Fi SSID和Wi-Fi密码。
* 如果选择以太网接口，还需要设置：
  * `Ethernet PHY`选项中的PHY模型，例如IP101。
  * `PHY Address`选项中的PHY地址，应由开发板原理图确定。
  * EMAC时钟模式，SMI使用的GPIO。

在`Example Configuration`菜单中：

* 在`mDNS Host Name`选项中设置域名。
* 在`Website deploy mode`中选择部署模式，目前我们支持将网站部署到主机PC、SD卡和SPI Nor闪存。
  * 如果选择`Deploy website to host (JTAG is needed)`，还需要在`Host path to mount (e.g. absolute path to web dist directory)`中指定网站的完整路径。
* 在`Website mount point in VFS`选项中设置网站的挂载点，默认值为`/www`。

### 构建和烧录

网页设计工作完成后，应通过运行以下命令编译它们：

```bash
cd path_to_this_example/front/web-demo
npm install
npm run build
```
> **_注意：_** 本示例需要`nodejs`版本`v10.19.0`

片刻之后，您将看到一个包含所有网站文件（如html、js、css、图片）的`dist`目录。

运行`idf.py -p PORT flash monitor`构建并烧录项目。

（要退出串行监视器，请键入``Ctrl-]``。）

有关配置和使用ESP-IDF构建项目的完整步骤，请参阅[入门指南](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html)。

### 通过semihost部署网站的额外步骤

在测试此部署模式时，我们需要运行支持semihost功能的最新版本OpenOCD：

```bash
openocd-esp32/bin/openocd -s openocd-esp32/share/openocd/scripts -f board/esp32-wrover-kit-3.3v.cfg
```

## 示例输出

### 在浏览器中渲染网页

在浏览器中，输入网站所在的URL（例如`http://esp-home.local`）。如果您的操作系统当前不支持mDNS服务，也可以输入ESP32获得的IP地址。

此外，本示例还启用了域名为`esp-home`的NetBIOS功能。如果您的操作系统支持并启用了NetBIOS（例如Windows原生支持NetBIOS），那么URL `http://esp-home`也应该可以工作。

![esp_home_local](https://dl.espressif.com/dl/esp-idf/docs/_static/esp_home_local.gif)

### ESP监视器输出

在*Light*页面，设置灯光颜色并点击确认按钮后，浏览器将向ESP32发送post请求，在控制台中，我们打印颜色值。

```bash
I (6115) example_connect: Connected to Ethernet
I (6115) example_connect: IPv4 address: 192.168.2.151
I (6325) esp-home: Partition size: total: 1920401, used: 1587575
I (6325) esp-rest: Starting HTTP Server
I (128305) esp-rest: File sending complete
I (128565) esp-rest: File sending complete
I (128855) esp-rest: File sending complete
I (129525) esp-rest: File sending complete
I (129855) esp-rest: File sending complete
I (137485) esp-rest: Light control: red = 50, green = 85, blue = 28
```

## 故障排除

1. 构建示例时出错：`...front/web-demo/dist doesn't exit. Please run 'npm run build' in ...front/web-demo`。
   * 当您选择将网站部署到SPI闪存时，请确保在构建此示例之前已生成`dist`目录。

（如有任何技术问题，请在GitHub上提出[issue](https://github.com/espressif/esp-idf/issues)。我们将尽快回复您。）
