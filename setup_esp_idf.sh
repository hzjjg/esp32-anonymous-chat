#!/bin/bash

# 设置ESP-IDF环境变量
export IDF_PATH=/Users/hzj/esp/v5.4.1/esp-idf

# 添加别名到当前会话
alias get_idf='. $IDF_PATH/export.sh'

# 输出使用说明
echo "ESP-IDF环境变量已设置"
echo "请运行以下命令来激活ESP-IDF环境："
echo "source $IDF_PATH/export.sh"
echo "激活后，您就可以使用 'idf.py' 命令了"