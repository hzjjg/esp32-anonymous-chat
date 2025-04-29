# ESP-IDF 环境设置指南

## 问题解决

当您尝试运行 `idf.py menuconfig` 命令时遇到 "command not found: idf.py" 错误，这是因为ESP-IDF开发环境没有正确设置。我们已经检测到您的系统中已经下载了ESP-IDF v5.4.1，但尚未激活环境变量。

## 解决方案

我们已经为您创建了一个脚本文件 `setup_esp_idf.sh`，您可以按照以下步骤来设置ESP-IDF环境：

### 方法一：使用我们提供的脚本

1. 打开终端，进入项目目录：
   ```bash
   cd /Users/hzj/code/esp32-anonymous-chat
   ```

2. 运行我们提供的设置脚本：
   ```bash
   source setup_esp_idf.sh
   ```

3. 按照脚本提示，运行以下命令激活ESP-IDF环境：
   ```bash
   source /Users/hzj/esp/v5.4.1/esp-idf/export.sh
   ```

4. 现在您可以使用 `idf.py` 命令了，例如：
   ```bash
   idf.py menuconfig
   ```

### 方法二：直接激活ESP-IDF环境

每次打开新的终端会话时，您都需要激活ESP-IDF环境：

```bash
source /Users/hzj/esp/v5.4.1/esp-idf/export.sh
```

## 永久设置（可选）

如果您希望在每次打开终端时自动设置ESP-IDF环境，可以将以下命令添加到您的shell配置文件中（例如 `~/.zshrc` 或 `~/.bash_profile`）：

```bash
# ESP-IDF环境设置
export IDF_PATH=/Users/hzj/esp/v5.4.1/esp-idf
alias get_idf='. $IDF_PATH/export.sh'
```

然后，您可以在需要使用ESP-IDF时，只需运行 `get_idf` 命令即可激活环境。

## 验证安装

激活环境后，您可以运行以下命令验证ESP-IDF是否正确安装：

```bash
idf.py --version
```

应该会显示类似 `ESP-IDF v5.4.1-dirty` 的输出。

## 项目构建步骤

1. 配置项目：
   ```bash
   idf.py menuconfig
   ```

2. 构建项目：
   ```bash
   idf.py build
   ```

3. 烧录到ESP32设备：
   ```bash
   idf.py -p PORT flash
   ```
   （将PORT替换为您的设备端口，例如 `/dev/ttyUSB0` 或 `/dev/cu.SLAB_USBtoUART`）

4. 监视串行输出：
   ```bash
   idf.py -p PORT monitor
   ```

## 常见问题

- **问题**: 每次打开新终端都需要重新激活环境
  **解决方案**: 将激活命令添加到shell配置文件中，如上所述

- **问题**: 找不到串行端口
  **解决方案**: 确保已安装USB-串行驱动程序，并检查设备管理器中的端口名称

- **问题**: 构建失败
  **解决方案**: 确保已安装所有必要的依赖项，并检查错误消息以获取更多信息