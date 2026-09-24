#pragma once

#include "analysisframe.h"

#include <QFile>
#include <QObject>
#include <QTimer>

class QJsonObject;

//类声明与构造函数 final表示这个类禁止被继承。它是一个具体的工作类，不需要作为基类
class DataWorker final : public QObject {
    Q_OBJECT

public:
    //explicit防止构造函数被用于隐式类型转换
    //默认没有父对象.如果有父对象，Qt 不允许移动线程
    explicit DataWorker(QObject *parent = nullptr);

//接收主线程的命令
public slots:
    //这些槽函数都在工作线程执行，因此文件读取和两次FFT不会阻塞主线程
    void loadDataset(const QString &metadataPath);
    void startPlayback();
    void pausePlayback();
    void stopPlayback();
    void setPlaybackSpeed(double speed);    //接收 1.0、2.0、4.0 等倍速，调整 QTimer 的触发间隔

signals:
    void metadataLoaded(const DataMetadata &metadata);
    void frameReady(const AnalysisFrame &frame);
    void statusMessage(const QString &message,bool isError);
    void playbackFinished();

//private slots：内部定时器触发
private slots:
    void processNextWindow();
private:
    //读取 JSON 文件，解析采样率、样本数、通道数组等；兼容新版 channels 和旧版单通道格式。
    bool parseMetadata(const QString &metadataPath,DataMetadata &metadata,QString &errorMessage);
    //解析单个通道对象，解析 data_file 路径、校验文件存在性、文件大小是否为 4 的整数倍、样本数是否与 JSON 一致。
    bool parseChannel(const QJsonObject &object,const QString &baseDirectory,const QString &expectedRole,ChannelMetadata &channel,qint64 expectedSampleCount,QString &errorMessage) const;
    //打开振动和音频两个 QFile，任意一个失败则整体加载失败
    bool openDataFiles(QString &errorMessage);
    //从指定文件读取 2048 个 float32 样本，处理小端序字节到 double 的转换。
    bool readWindow(QFile &file,QVector<double> &samples,QString &errorMessage) const;
    //把两个文件的读取位置重置到开头
    void rewindDataFiles();
    //根据采样率、窗口大小和倍速计算定时器间隔，更新 QTimer
    void updateTimerInterval();
    //对一路 2048 点窗口进行去均值、AC RMS、峰值、峭度计算，加汉宁窗后做 FFT，找主频。
    SignalAnalysis analyzeSignal(const QVector<double> &samples) const;

    QFile vibrationFile_;           //振动通道文件对象（AIN 1）
    QFile audioFile_;               //音频通道文件对象（AIN 8）
    QTimer *timer_ = nullptr;       //驱动回放的定时器
    DataMetadata metadata_;         //当前数据集的元数据
    int windowSize_ = 2048;         //窗口大小，默认 2048
    double playbackSpeed_ = 1.0;    //当前倍速，默认 1.0
    qint64 nextSample_ = 0;         //同步核心：下一个要读取的样本索引，两路共用
    qint64 windowIndex_ = 0;        //当前窗口序号，从 0 开始
    bool datasetReady_ = false;     //标记数据集是否加载并准备好回放
};