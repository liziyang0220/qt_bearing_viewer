# Qt Bearing Viewer

基于 Raspberry Pi 5、Qt 6 和 C++17 开发的轴承振动与音频同步回放及特征分析终端。

## 项目背景

本项目面向课题组轴承故障诊断与后续边缘 AI 部署需求，对实验室采集的轴承历史数据进行同步回放、时频域特征计算和可视化验证。

当前版本主要用于比较正常、内圈故障和外圈故障数据之间的特征差异，为后续在 Raspberry Pi 5 上部署故障分类模型提供数据处理和可视化基础。

## 主要功能

- 同步回放 AIN1 轴承振动与 AIN8 音频数据
- 支持 1 倍、2 倍和 4 倍速回放
- 支持开始、暂停、继续和停止操作
- 显示振动与音频的时域波形和频谱
- 计算 RMS、峰值、峭度和主频
- 使用 QThread 分离数据处理线程与 GUI 主线程
- 使用 SQLite 保存分析结果和历史记录
- 实时显示 Raspberry Pi CPU、内存、线程和温度信息
- 支持使用 Python/NumPy 验证 C++ 特征计算结果

## 技术栈

- C++17
- Qt 6 Widgets / Charts / SQL
- QObject / QThread / QTimer
- FFTW3
- SQLite
- Python / NumPy
- CMake / Ninja
- Debian ARM64
- Raspberry Pi 5

## 数据处理流程

```text
JSON 元数据与 float32 双通道数据
                ↓
        按 2048 点窗口读取
                ↓
        去均值并计算时域特征
                ↓
             汉宁窗
                ↓
          FFTW3 频谱计算
                ↓
       Qt Charts 波形与频谱显示
                ↓
        SQLite 保存分析结果
```

实验室真实数据采样率为 10 kHz，项目主要使用：

- AIN1：轴承振动信号
- AIN8：音频信号

## 项目结构

```text
qt_bearing_viewer/
├── CMakeLists.txt
├── src/                         # Qt/C++ 核心程序
│   ├── main.cpp
│   ├── mainwindow.h
│   ├── mainwindow.cpp
│   ├── dataworker.h
│   ├── dataworker.cpp
│   └── analysisframe.h
├── tools/                       # 数据生成、转换与验证工具
│   ├── generate_sample_data.py
│   ├── validate_dataset.py
│   └── convert_lab_zip.py
└── scripts/                     # 环境安装与性能测试脚本
    ├── install_raspberry_pi_os.sh
    └── collect_metrics.sh
```

## 安装依赖

在 Raspberry Pi OS 或 Debian ARM64 中执行：

```bash
bash scripts/install_raspberry_pi_os.sh
```

## 生成测试数据

```bash
python3 tools/generate_sample_data.py \
    --output data/sample \
    --seconds 60
```

## 验证测试数据

```bash
python3 tools/validate_dataset.py \
    data/sample/dual_channel_test.json
```

## 编译项目

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build -j2
```

## 运行项目

```bash
./build/qt_bearing_viewer
```

运行后，在界面中选择数据集对应的 JSON 元数据文件，即可进行双通道同步回放和特征分析。

## 实验结果

项目已经在 Raspberry Pi 5 ARM64 Linux 环境中完成以下测试：

- 合成双通道数据正确性验证
- 正常、内圈故障和外圈故障真实数据回放
- 1 倍、2 倍和 4 倍速回放
- 暂停、继续和停止功能测试
- Python/NumPy 与 C++ 特征结果交叉验证
- CPU、内存、线程数和设备温度监测

## 数据说明

实验室真实轴承数据涉及数据权限且文件体积较大，因此未上传至本仓库。

用户可以运行 `generate_sample_data.py` 生成可复现的双通道合成数据，用于程序功能和 FFT 结果验证。

## 项目边界

当前版本完成了历史数据回放、特征计算、可视化、SQLite 记录和 Raspberry Pi 性能验证。

项目尚未集成训练后的 AI 故障分类模型。后续可在此基础上接入 ONNX Runtime 或 TensorFlow Lite，实现树莓派端的轴承故障分类。
