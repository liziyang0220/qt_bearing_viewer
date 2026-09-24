#include "dataworker.h"

#include <QDir>             //处理路径和文件信息
#include <QElapsedTimer>
#include <QFileInfo>        //处理路径和文件信息
#include <QJsonArray>       //解析 JSON 元数据
#include <QJsonDocument>    //解析 JSON 元数据
#include <QJsonObject>      //解析 JSON 元数据
#include <QtEndian>         //提供 qFromLittleEndian，用于处理小端序字节

#include <algorithm>        //提供 std::clamp、std::max
#include <cmath>            //提供 std::cos、std::sqrt、std::hypot、std::log10
#include <cstring>          //提供 std::memcpy
#include <fftw3.h>          //FFTW3 库，用于计算 FFT

namespace {
constexpr double kPi = 3.14159265358979323846;  //定义在匿名命名空间中，仅在当前 .cpp 文件内可见，避免污染全局命名空间
}

//构造函数 开一个定时器，定时触发`processNextWindow`函数，一般用来做历史数据回放、周期性数据处理。
DataWorker::DataWorker(QObject *parent) : QObject(parent),timer_(new QTimer(this)){
    // PreciseTimer 尽量减小回放定时误差。它不保证硬实时，但适合历史数据模拟回放
    timer_->setTimerType(Qt::PreciseTimer);
    //connect(发送者, 信号, 接收者, 槽函数)
    connect(timer_,&QTimer::timeout,this,&DataWorker::processNextWindow);
}

// parseChannel:解析单路通道并校验文件
bool DataWorker::parseChannel(const QJsonObject &object,const QString &baseDirectory,const QString &expectedRole,ChannelMetadata &channel,qint64 expectedSampleCount,QString &errorMessage) const{
    //1.提取基本字段
    channel.role = object.value("role").toString(expectedRole);
    channel.channelName = object.value("channel").toString("unknown");
    channel.signalName = object.value("signal_name").toString("unknown");
    channel.unit = object.value("unit").toString("raw");
    //2.校验data_file
    const QString relativeDataFile = object.value("data_file").toString();
    if(relativeDataFile.isEmpty()){
        errorMessage = QStringLiteral("%1 通道缺少 data_file 字段").arg(expectedRole);
        return false;
    }
    //3.解析绝对路径
    channel.dataFile = QDir(baseDirectory).absoluteFilePath(relativeDataFile);
    //4.校验文件存在性和大小
    const QFileInfo dataInfo(channel.dataFile);
    //文件必须存在且是普通文件
    if(!dataInfo.exists() || !dataInfo.isFile()){
        errorMessage = QStringLiteral("找不到 %1 通道数据文件: %2").arg(expectedRole,channel.dataFile);
        return false;
    }
    //文件大小必须是 4 字节（float）的整数倍，否则说明数据格式不对
    if(dataInfo.size() % static_cast<qint64>(sizeof(float)) != 0)
    {
        errorMessage = QStringLiteral("%1 通道文件长度不是 float32 字节数的整数倍").arg(expectedRole);
        return false;
    }
    //5.校验样本数
    const qint64 actualCount = dataInfo.size() / static_cast<qint64>(sizeof(float));
    if(actualCount != expectedSampleCount)
    {
        errorMessage = QStringLiteral("%1 通道样本数为 %2,与 JSON 的 %3 不一致").arg(expectedRole).arg(actualCount).arg(expectedSampleCount);
        return false;
    }
    return true;
}

// parseMetadata：解析整个 JSON 元数据
bool DataWorker::parseMetadata(const QString &metadataPath,DataMetadata &metadata,QString &errorMessage){
    //1.打开并解析JSON
    QFile metadataFile(metadataPath);
    if(!metadataFile.open(QIODevice::ReadOnly)){
        errorMessage = QStringLiteral("无法打开元数据文件: %1").arg(metadataFile.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(metadataFile.readAll(),&parseError);
    if(parseError.error != QJsonParseError::NoError || !document.isObject()){
        errorMessage = QStringLiteral("JSON 格式错误: %1").arg(parseError.errorString());
        return false;
    }

    //2.提取顶层字段
    const QJsonObject root = document.object();
    metadata.name = root.value("name").toString(QFileInfo(metadataPath).baseName());
    metadata.label = root.value("label").toString("unknown");
    metadata.sampleRate = root.value("sample_rate").toDouble();
    metadata.rpm = root.value("rpm").toDouble();
    metadata.load = root.value("load").toDouble();
    metadata.sampleCount = static_cast<qint64>(root.value("sample_count").toDouble());

    //3.校验采样率和样本数
    if(metadata.sampleRate <= 0.0){
        errorMessage = QStringLiteral("sample_rate 必须大于 0");
        return false;
    }
    if(metadata.sampleCount < windowSize_){
        errorMessage = QStringLiteral("数据不足 %1 点,无法形成一个分析窗口").arg(windowSize_);
        return false;
    }

    //4.新版格式：channels数组
    const QString baseDirectory = QFileInfo(metadataPath).absolutePath();
    if(root.value("channels").isArray())
    {
        QJsonObject vibrationObject;
        QJsonObject audioObject;
        const QJsonArray channels = root.value("channels").toArray();
        for(const QJsonValue &value : channels)
        {
            if(!value.isObject()){
                continue;
            }
            const QJsonObject channelObject = value.toObject();
            const QString role = channelObject.value("role").toString();
            if(role == "vibration")
            {
                vibrationObject = channelObject;
            }else if(role == "audio"){
                audioObject = channelObject;
            }
        }
        if(vibrationObject.isEmpty()){
            errorMessage = QStringLiteral("channels 中缺少 role=vibration 的振动通道");
            return false;
        }
        if(!parseChannel(vibrationObject,baseDirectory,"vibration",metadata.vibration,metadata.sampleCount,errorMessage)){
            return false;
        }
        //**频通道只要存在，就必须校验成功，一旦音频校验失败，整个数据集加载失败，return false**。
        metadata.hasAudio = !audioObject.isEmpty();
        if(metadata.hasAudio && !parseChannel(audioObject,baseDirectory,"audio",metadata.audio,metadata.sampleCount,errorMessage)){
            return false;
        }
    }else{
        //5.旧版格式：顶层 data_file
        QJsonObject legacyChannel;
        legacyChannel.insert("role","vibration");
        legacyChannel.insert("data_file",root.value("data_file"));
        legacyChannel.insert("channel",root.value("channel"));
        legacyChannel.insert("signal_name",root.value("signal_name"));
        legacyChannel.insert("unit",root.value("unit"));
        if(!parseChannel(legacyChannel,baseDirectory,"vibration",metadata.vibration,metadata.sampleCount,errorMessage)){
            return false;
        }
        metadata.hasAudio = false;
    }
    return true;
}

//openDataFiles：打开两个数据文件
bool DataWorker::openDataFiles(QString &errorMessage){
    //给 QFile 对象设置要操作的文件路径，**此时还没有打开文件**，只是记录文件名
    vibrationFile_.setFileName(metadata_.vibration.dataFile);
    if(!vibrationFile_.open(QIODevice::ReadOnly)){
        errorMessage = QStringLiteral("无法打开振动数据: %1").arg(vibrationFile_.errorString());
        return false;
    }
    if(metadata_.hasAudio){
        audioFile_.setFileName(metadata_.audio.dataFile);
        if(!audioFile_.open(QIODevice::ReadOnly)){
            errorMessage = QStringLiteral("无法打开音频数据: %1").arg(audioFile_.errorString());
            vibrationFile_.close();
            return false;
        }
    }
    return true;
}

//loadDataset：加载数据集
void DataWorker::loadDataset(const QString &metadataPath){
    //安全清理，加载新数据集之前，先清理上一次正在回放的资源
    timer_->stop();
    //防止加载新文件时，旧文件还占用文件句柄，出现资源冲突
    vibrationFile_.close();
    audioFile_.close();
    datasetReady_ = false;
    //临时结构体，用来存放本次新解析出来的元数据
    QString errorMessage;
    DataMetadata parsed;
    if(!parseMetadata(metadataPath,parsed,errorMessage)){
        //发射 Qt 信号，把错误消息发给 UI 界面
        //第二个参数`true`一般代表这是**错误消息**（UI 可以标红提示）
        emit statusMessage(errorMessage,true);
        return;
    }
    //解析 JSON 成功，把临时的 parsed 拷贝到类成员变量`metadata_`，现在 metadata_保存新数据集的通道信息、采样点数、是否有音频等
    metadata_ = parsed;
    if(!openDataFiles(errorMessage))
    {
        //打开文件失败：发送错误信号给 UI，函数 return 终止加载
        emit statusMessage(errorMessage,true);
        return;
    }
    //回放的计数器清零！非常关键，用于回放模拟
    nextSample_ = 0;
    windowIndex_ = 0;
    //数据集**全部加载完毕，就绪**
    datasetReady_ = true;
    //更新定时器的定时周期。一般内部会根据**采样率**计算定时器间隔，保证回放速度和真实采集速度一致
    updateTimerInterval();
    //发射信号`metadataLoaded`，把元数据传给 UI。UI 收到后可以绘制通道名称、坐标轴、显示采样信息
    emit metadataLoaded(metadata_);
    //第二个参数`false`，代表**普通提示信息，不是错误**，UI 一般正常黑色文字展示
    emit statusMessage(metadata_.hasAudio ? QStringLiteral("双通道数据集已加载,可以开始同步回放") : QStringLiteral("单通道数据集一加载,音频图将保持空白"),false);
}

//回放控制相关函数
//**启动数据回放**的入口
void DataWorker::startPlayback(){
    //判断数据集是否加载完成
    if(!datasetReady_){
        emit statusMessage(QStringLiteral("请先选择元数据 JSON 文件"),true);
        return;
    }
    //如果**下一次读取窗口会超出总样本末尾**（回放到头了）
    if(nextSample_ + windowSize_ > metadata_.sampleCount)
    {
        rewindDataFiles();
        nextSample_ = 0;
        windowIndex_ = 0;
    }
    //重新计算定时器的定时周期
    updateTimerInterval();
    timer_->start();
    emit statusMessage(QStringLiteral("正在同步回放"),false);
}

//暂停回放
void DataWorker::pausePlayback(){
    timer_->stop();
    emit statusMessage(QStringLiteral("已暂停"),false);
}
//停止回放（完全停止，复位）
void DataWorker::stopPlayback(){
    timer_->stop();
    //把振动、音频二进制文件的文件指针 seek 回到**文件开头**
    rewindDataFiles();
    //样本索引重置
    nextSample_ = 0;
    //窗口编号清零
    windowIndex_ = 0;
    emit statusMessage(QStringLiteral("已停止并回到数据起点"),false);
}
//把打开的二进制数据文件的读取指针，移动到文件最开头
void DataWorker::rewindDataFiles(){
    if(vibrationFile_.isOpen()){
        vibrationFile_.seek(0);
    }
    if(audioFile_.isOpen()){
        audioFile_.seek(0);
    }
}
//setPlaybackSpeed 设置回放速度
void DataWorker::setPlaybackSpeed(double speed){
    playbackSpeed_ = std::clamp(speed,1.0,4.0);
    updateTimerInterval();
}
//更新定时器时间间隔（核心计算公式）
void DataWorker::updateTimerInterval(){
    if(metadata_.sampleRate <= 0.0)
    {
        return;
    }
    //一个窗口表示windowSize/sampleRate秒。倍速只是改变窗口触发间隔，
    //不改变采样率和频率坐标,因此 1/2/4倍速下FFT主频应该保持一致
    const double intervalMs = 1000.0 * windowSize_ / (metadata_.sampleRate * playbackSpeed_);
    //保证最终定时器间隔 ≥ 1ms
    timer_->setInterval(std::max(1,static_cast<int>(std::lround(intervalMs))));
}

//从二进制文件一次性读取一窗口(windowSize_)个float类型的采样点.小端序解析，放到`QVector<double>`里
bool DataWorker::readWindow(QFile &file,QVector<double> &samples,QString &errorMessage) const{
    const qint64 bytesRequired = static_cast<qint64>(windowSize_) * static_cast<qint64>(sizeof(float));
    const QByteArray bytes = file.read(bytesRequired);
    if(bytes.size() != bytesRequired)
    {
        errorMessage = QStringLiteral("读取 %1 时遇到意外文件结尾").arg(file.fileName());
        return false;
    }
    //把输出容器`samples`预先分配好空间，大小等于窗口样本数，避免循环内频繁扩容
    samples.resize(windowSize_);
    for(int i=0;i<windowSize_;++i)
    {
        //数据文件规定为 little-endian float32。先按整数完成字节序转换，
        //再用 memcpy 还原 float，避免未对齐指针和严格别名规则导致未定义行为
        quint32 raw = 0;
        std::memcpy(&raw,bytes.constData() + i * sizeof(float),sizeof(raw));
        //`qFromLittleEndian()`：把小端序的 4 字节数据，转成当前 CPU 主机序
        raw = qFromLittleEndian(raw);
        float value = 0.0F;
        std::memcpy(&value,&raw,sizeof(value));
        samples[i] = static_cast<double>(value);
    }
    return true;
}
//定时器 timeout 触发的核心回调函数
void DataWorker::processNextWindow(){
    //如果数据集没有加载好，或者振动文件没有打开
    if(!datasetReady_ || !vibrationFile_.isOpen())
    {
        timer_->stop();
        return;
    }
    //下一窗读取是否超出全部样本总数
    if(nextSample_ + windowSize_ > metadata_.sampleCount){
        timer_->stop();
        emit playbackFinished();
        emit statusMessage(QStringLiteral("数据回放完成"),false);
        return;
    }

    QVector<double> vibrationSamples;
    QVector<double> audioSamples;
    //保存 readWindow 读取出错时的错误文本
    QString errorMessage;
    if(!readWindow(vibrationFile_,vibrationSamples,errorMessage) || (metadata_.hasAudio && !readWindow(audioFile_,audioSamples,errorMessage))){
        timer_->stop();
        emit statusMessage(errorMessage,true);
        return;
    }

    //两路必须先读取同一个窗口，再一起分析和发送。nextSample_只递增一次，
    //从而保证振动与音频曲线的起始样本和时间轴完全一致
    //Qt 高精度计时器，用来统计**后面信号分析代码消耗了多少时间**
    QElapsedTimer elapsed;
    elapsed.start();
    //创建一帧`AnalysisFrame`结构体，这是传给 UI 的数据载体
    AnalysisFrame frame;
    frame.firstSample = nextSample_;
    frame.windowindex = windowIndex_++;
    frame.hasAudio = metadata_.hasAudio;
    //生成每个样本对应的**时间戳（单位 ms）**
    frame.timeMs.resize(windowSize_);
    for(int i=0;i<windowSize_;++i)
    {
        //时间(秒) = 样本序号 / 采样率Hz
        //`nextSample_ + i`：当前窗口内第 i 个样本的全局样本编号
        frame.timeMs[i] = 1000.0 * (nextSample_ + i) / metadata_.sampleRate;
    }
    frame.vibration = analyzeSignal(vibrationSamples);
    if(metadata_.hasAudio)
    {
        frame.audio = analyzeSignal(audioSamples);  
    }
    //`elapsed.nsecsElapsed()`：获取从`start()`到现在的**纳秒**
    //除以`1e6` → 转成毫秒，保存到 frame
    frame.processingMs = elapsed.nsecsElapsed() / 1e6;
    //更新全局样本索引：本窗口处理完毕，下一次读取的起始样本号向后移动 windowSize_个样本
    nextSample_ += windowSize_;
    emit frameReady(frame);
}

//analyzeSignal：单路信号分析与 FFT
SignalAnalysis DataWorker::analyzeSignal(const QVector<double> &samples) const{
    SignalAnalysis result;
    //保存原始样本到 result.samples，供时域图显示
    result.samples = samples;

    //先计算均值，再对交流分量计算RMS、峰值和峭度。实验室模拟量含直流
    //偏置，如果直接对原始值计算RMS，会把采集卡偏置误认为机械振动能量
    //1.计算窗口内所有样本的均值
    double mean = 0.0;
    for(double value : samples)
    {
        mean += value;
    }
    mean /= samples.size();
    //2.计算 AC RMS、峰值、峭度
    double varianceSum = 0.0;
    double fourthMomentSum = 0.0;
    double centeredPeak = 0.0;
    for(double value : samples)
    {
        const double centered = value - mean;
        const double squared = centered*centered;
        varianceSum += squared;
        fourthMomentSum += squared*squared;
        centeredPeak = std::max(centeredPeak,std::abs(centered));
    }
    //方差
    const double variance = varianceSum / samples.size();
    //RMS 是去均值后的有效值
    result.rms = std::sqrt(variance);
    //峰值
    result.peak = centeredPeak;
    //峭度
    result.kurtosis = variance > 1e-15 ? (fourthMomentSum / samples.size()) / (variance*variance) : 0.0;
    //3.分配FFTW内存
    //**分配内存，准备做快速傅里叶变换，把时域振动信号转为频域频谱**。
    const int n = samples.size();
    double* input = static_cast<double*>(fftw_malloc(sizeof(double)*n));
    //存放 FFT 计算之后的**频域复数结果**（幅值 + 相位）
    fftw_complex* output = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex)*(n/2+1)));
    if(!input || !output)
    {
        //内存分配失败
        fftw_free(input);
        fftw_free(output);
        return result;
    }
    //4.加汉宁窗
    //汉宁窗减轻有限窗口截断造成的频谱泄露。FFT输入仍然减去均值，
    //因此0Hz附近的直流峰不会掩盖真正的机械或声学频率成分
    for(int i=0;i<n;++i)
    {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n-1));
        input[i] = (samples[i] - mean)*hann;
    }
    //5.执行FFT
    //plan 里面保存 FFTW 选择的算法、计算步骤。**plan 本身不存信号数据**，只是告诉 fftw_execute 怎么算
    fftw_plan plan = fftw_plan_dft_r2c_1d(n,input,output,FFTW_ESTIMATE);
    if(!plan)
    {
        fftw_free(output);
        fftw_free(input);
        return result;
    }
    //真正执行 FFT 计算的函数
    fftw_execute(plan);
    //6.计算频谱和主频
    //遍历 FFT 输出，计算每个频率点、幅值、分贝 (dB)，并找出主频（最大幅值对应的频率）
    //实数 FFT 输出的频谱点数量
    const int bins = n/2 + 1;
    //存放每个谱点对应的**频率 **
    result.frequencyHz.resize(bins);
    //存放每个谱点幅值
    result.magnitudeDb.resize(bins);
    //记录循环里找到的最大幅值
    double maximumMagnitude = -1.0;
    //最大幅值对应的谱点下标
    int maximumBin = 0;
    for(int k=0;k<bins;++k)
    {
        const double magnitude = std::hypot(output[k][0],output[k][1]) / n;
        result.frequencyHz[k] = k * metadata_.sampleRate / n;
        result.magnitudeDb[k] = 20.0 * std::log10(std::max(magnitude,1e-12));
        //跳过直流频点，只在1至Nyquist频点中寻找主频
        if(k>0 && magnitude>maximumMagnitude)
        {
            maximumMagnitude = magnitude;
            maximumBin = k;
        }
    }
    //循环结束，用找到的最大幅值的 bin，计算**主频 dominantFrequency**
    result.dominantFrequency = maximumBin * metadata_.sampleRate / n;
    //释放 FFTW 资源
    fftw_destroy_plan(plan);
    fftw_free(output);
    fftw_free(input);
    return result;
}