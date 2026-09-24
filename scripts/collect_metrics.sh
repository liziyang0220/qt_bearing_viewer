#!/usr/bin/env bash
# 使用 bash 解释器执行本脚本

set -euo pipefail
# set -e：命令返回非零状态时立即退出
# set -u：使用未定义变量时报错
# set -o pipefail：管道中任意命令失败，整个管道返回失败

# 检查是否至少传入一个参数（PID）
if [[ $# -lt 1 ]]; then
    echo "用法：$0 <PID> [seconds] [output.csv]"
    exit 1
fi

target_pid="$1"                      # 第一个参数：要监控的进程 PID
duration_seconds="${2:-1800}"        # 第二个参数：采样总时长（秒），默认 1800 秒（30 分钟）
output_file="${3:-metrics.csv}"      # 第三个参数：输出 CSV 文件名，默认 metrics.csv

# 校验 PID 是否为纯数字，并且该进程存在且当前用户有权限访问
if ! [[ "$target_pid" =~ ^[0-9]+$ ]] || ! kill -0 "$target_pid" 2>/dev/null; then
    echo "PID 不存在或当前用户无法访问：$target_pid"
    exit 1
fi

# 写入 CSV 表头
echo "timestamp,cpu_percent,rss_kb,threads,temp_c" > "$output_file"

# 按秒循环采样，共循环 duration_seconds 次
for ((second = 0; second < duration_seconds; ++second)); do
    # 如果目标进程已退出，则停止采样
    if ! kill -0 "$target_pid" 2>/dev/null; then
        echo "程序已退出，停止采样"
        break
    fi

    # 获取目标进程的 CPU 占用率（%）
    cpu_percent="$(ps -p "$target_pid" -o %cpu= | xargs)"
    # 获取目标进程的常驻内存大小（KB）
    rss_kb="$(ps -p "$target_pid" -o rss= | xargs)"
    # 获取目标进程的线程数
    threads="$(ps -p "$target_pid" -o nlwp= | xargs)"
    # 读取 CPU 温度原始值（通常为毫摄氏度）
    temp_raw="$(cat /sys/class/thermal/thermal_zone0/temp)"
    # 将原始温度除以 1000，转换为摄氏度，并保留 1 位小数
    temp_c="$(awk -v value="$temp_raw" 'BEGIN { printf "%.1f", value / 1000.0 }')"

    # 将当前时间戳和各项指标追加写入 CSV 文件
    echo "$(date --iso-8601=seconds),$cpu_percent,$rss_kb,$threads,$temp_c" >> "$output_file"

    # 等待 1 秒后进入下一次采样
    sleep 1
done

# 采样结束，输出提示信息
echo "Metrics written to $output_file"