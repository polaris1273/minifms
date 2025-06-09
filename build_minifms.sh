#!/bin/bash

# MiniFMS Pro 编译脚本
# 支持 Windows 和 Linux 平台

echo " 正在编译 MiniFMS Pro..."

# 检测操作系统
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
    # Windows 环境
    echo "🖥️ 检测到 Windows 环境"
    g++ -std=c++17 -O2 -Wall -Wextra -o minifms.exe minifms.cpp
    echo "✅ 编译完成! 运行: ./minifms.exe"
else
    # Linux 环境
    echo "🐧 检测到 Linux 环境"
    g++ -std=c++17 -O2 -Wall -Wextra -pthread -lrt -o minifms minifms.cpp
    echo "✅ 编译完成! 运行: ./minifms"
fi
