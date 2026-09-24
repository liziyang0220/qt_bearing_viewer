#!/usr/bin/env bash
set -euo pipefail

#必须在Raspberry Pi OS 64 位的终端执行。本脚本只安装官方仓库依赖
sudo apt update
sudo apt install -y\
 build-essential cmake ninja-build git gdb pkg-config \
 qt6-base-dev qt6-charts-dev libqt6sql6-sqlite \
 libfftw3-dev python3 python3-numpy sysstat

echo "Dependencies installed.Verify with:uname -m && cmake --version && qmake6 --
version"