#!/usr/bin/env python3
"""生成可验证的双通道float32数据和配套JSON元数据。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

def main() -> None:
    parser = argparse.ArgumentParser(description="生成振动与音频同步测试数据")
    #解析命令行参数：输出目录、采样率、时长
    parser.add_argument("--output",default="data/sample",help="输出目录")
    parser.add_argument("--sample-rate",type=int,default=10000)
    parser.add_argument("--seconds",type=float,default=60.0)
    args = parser.parse_args()
    if args.sample_rate <= 0 or args.seconds <= 0:
        raise ValueError("sample-rate 和 seconds 必须大于0")

    output = Path(args.output)
    output.mkdir(parents=True,exist_ok=True)
    #样本数
    sample_count = int(args.sample_rate * args.seconds)
    #生成时间轴
    time = np.arange(sample_count,dtype=np.float64) / args.sample_rate
    #用固定随机种子生成可复现噪声
    rng = np.random.default_rng(20260908)

    #振动通道以1000Hz为已知主频，250Hz为较弱分量
    #主频1000Hz正弦波,较弱分量250Hz正弦波,高斯白噪声
    vibration = (
        0.8 * np.sin(2 * np.pi * 1000.0 * time)     
        + 0.2 * np.sin(2 * np.pi * 250.0 * time)
        + 0.03 * rng.standard_normal(sample_count)
    ).astype("<f4")
    #音频通道使用不同的 600 Hz 主频，便于证明两路没有读反或共用同一缓冲区。
    audio = (
        0.6 * np.sin(2 * np.pi * 600.0 * time)
        + 0.15 * np.sin(2 * np.pi * 1200.0 * time)
        + 0.02 * rng.standard_normal(sample_count)
    ).astype("<f4")

    #写入二进制文件
    vibration_name = "synthetic_AIN1.bin"
    audio_name = "synthetic_AIN8.bin"
    vibration.tofile(output / vibration_name)
    audio.tofile(output / audio_name)

    #构造JSON元数据
    metadata = {
        "format_version": 2,
        "name": "dual-channel FFT correctness sample",
        "label": "synthetic_test",
        "sample_rate": args.sample_rate,
        "sample_count": sample_count,
        "rpm": 1800,
        "load": 0,
        "channels": [
            {
                "role": "vibration",
                "channel": "AIN 1",
                "signal_name": "合成振动信号",
                "unit": "raw",
                "data_file": vibration_name,
            },
            {
                "role": "audio",
                "channel": "AIN 8",
                "signal_name": "合成音频信号",
                "unit": "raw",
                "data_file": audio_name,
            },
        ],
    }

    #写入JSON文件
    metadata_path = output / "dual_channel_test.json"
    metadata_path.write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    #打印结果
    print(f"Generated {sample_count} synchronized samples/channel in {output.resolve()}")

#程序入口
if __name__ == "__main__":
    main()





