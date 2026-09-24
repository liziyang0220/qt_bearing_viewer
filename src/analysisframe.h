#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

//描述一路信号在磁盘上的位置和含义。振动和音频分别保存为独立的
//little-endian float32文件，避免回放时一次把完整数据载入内存
struct ChannelMetadata{
    QString role;           //程序角色：vibration或audio
    QString dataFile;       //由JSON所在目录解析得到的绝对路径
    QString channelName;    //原始采集名，例如AIN1 AIN8
    QString signalName;     //面向用户的名称 例如振1轴承，音频
    QString unit;           //未完成传感器标定时必须保持raw
};


//一份同步数据集的公共元信息。两路数据共享采样率，样本数，工况和标签
//因而同一窗口的第i个振动点与第i个音频点属于同一采样时刻
struct DataMetadata
{
    QString name;
    QString label;
    double sampleRate = 0.0;
    double rpm = 0.0;
    double load = 0.0;
    qint64 sampleCount = 0;
    ChannelMetadata vibration;
    ChannelMetadata audio;
    bool hasAudio = false; //兼容旧版只用AIN1的JSON文件
};

// 一路信号在一个分析窗口内的时域数据、频谱和统计特征
// 交流 RMS、峰值、峭度和 FFT 都基于去均值后的交流分量计算；samples
// 保留原始值，以便时域图如实显示采集数据中的直流偏置。
struct SignalAnalysis
{
    QVector<double> samples;            //该窗口的原始时域样本
    QVector<double> frequencyHz;        //FFT 频率轴，单位 Hz
    QVector<double> magnitudeDb;        //频谱幅值，单位 dB
    double rms = 0.0;                   //交流 RMS
    double peak = 0.0;                  //峰值
    double kurtosis = 0.0;              //峭度
    double dominantFrequency = 0.0;     //主频，单位 Hz
};


// 后台线程每处理完一个同步窗口，就把这份只读结果发送给界面线程。
// 界面线程不读取原始文件，也不执行 FFT，避免按钮和图表卡顿。
struct AnalysisFrame
{
    QVector<double> timeMs;         //该窗口的时间轴，单位毫秒
    SignalAnalysis vibration;       //振动通道分析结果
    SignalAnalysis audio;           //音频通道分析结果
    bool hasAudio = false;          //本帧是否包含音频
    qint64 windowindex = 0;         //窗口序号，从 0 开始
    qint64 firstSample = 0;         //该窗口第一个样本在完整文件中的索引
    double processingMs = 0.0;      //两路读取之后全部计算的总耗时
};

Q_DECLARE_METATYPE(DataMetadata)
Q_DECLARE_METATYPE(AnalysisFrame)
