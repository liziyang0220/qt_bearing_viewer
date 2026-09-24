#!/usr/bin/env python3
"""独立计算每一路首窗口指标，用于和 Qt/C++ 结果交叉验证。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

#
def analyze(values: np.ndarray, sample_rate: float, window_size: int) -> dict[str, float]:
    """使用与C++相同的去均值、汉宁窗和频点定义计算参考值"""
    #检查数据长度
    if len(values) < window_size:
        raise ValueError(f"数据只有 {len(values)} 点,不足 {window_size} 点窗口")
    #截取首窗口转换为float64
    window = values[:window_size].astype(np.float64)
    #去均值消除直流分量 后续RMS只反映交流分量  FFT不会在0Hz处出现巨大峰值
    centered = window - window.mean()
    #计算方差
    variance = np.mean(centered**2)
    #计算频谱
    spectrum = np.abs(np.fft.rfft(centered * np.hanning(len(window)))) / len(window)
    #计算频率轴
    frequencies = np.fft.rfftfreq(len(window), 1.0 / sample_rate)
    #找主频
    dominant = frequencies[1 + np.argmax(spectrum[1:])]
    #返回指标字典
    return {
        "rms": float(np.sqrt(variance)),
        "peak": float(np.max(np.abs(centered))),
        "kurtosis": float(np.mean(centered**4) / variance**2) if variance > 1e-15 else 0.0,
        "dominant_frequency_hz": float(dominant),
    }

#主函数
def main() -> None:
    #解析命令行参数
    parser = argparse.ArgumentParser()
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--window", type=int, default=2048)
    args = parser.parse_args()
    if args.window <= 1:
        raise ValueError("window 必须大于1")

    #读取JSON元数据
    meta = json.loads(args.metadata.read_text(encoding="utf-8"))
    #新版 channels 数组支持同步双通道；旧版顶层 data_file 自动包装为振动通道。
    channels = meta.get("channels")
    if channels is None:
        channels = [
            {
                "role": "vibration",
                "channel": meta.get("channel", "unknown"),
                "signal_name": meta.get("signal_name", "unknown"),
                "data_file": meta["data_file"],
            }
        ]
    #读取期望样本数
    expected_count = int(meta.get("sample_count", 0))
    #遍历每个通道
    for channel in channels:
        #构造数据文件路径
        data_path = args.metadata.parent / channel["data_file"]
        #读取二进制数据
        values = np.fromfile(data_path, dtype="<f4")
        #校验样本数
        if expected_count and len(values) != expected_count:
            raise ValueError(
                f"{channel['role']} 样本数 {len(values)} 与 JSON 的 {expected_count} 不一致"
            )
        #调用 analyze() 计算指标
        result = analyze(values, float(meta["sample_rate"]), args.window)
        #打印当前通道的基本信息
        print(
            f"[{channel['role']}] {channel.get('channel', '')}"
            f"{channel.get('signal_name', '')}"
        )
        #打印指标结果
        print(f"sample_count={len(values)}")        #实际读出的样本数
        print(f"rms={result['rms']:.8f}")           #保留 8 位小数
        print(f"peak={result['peak']:.8f}")         #保留 8 位小数
        print(f"kurtosis={result['kurtosis']:.8f}") #保留 8 位小数
        print(f"dominant_frequency_hz={result['dominant_frequency_hz']:.4f}") #保留 4 位小数

if __name__ == "__main__":
    main()
