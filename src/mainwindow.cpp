#include "mainwindow.h"
#include "dataworker.h"

#include <QChart>           //图表容器：setTitle、addSeries、addAxis
#include <QChartView>       //把图表包成控件放进 2×2 网格
#include <QCoreApplication> // Qt 核心应用类：管理非 GUI 程序的事件循环和全局资源
#include <QFile>            // Qt 文件类：用于文件的打开、读取、写入等操作
#include <QProcess>         // Qt 进程类：用于启动外部程序、读写其输入输出、监控其状态
#include <QStringList>      // Qt 字符串列表类：常用于保存多个 QString 参数或结果
#include <QTimer>           // Qt 定时器类：用于周期性触发任务，例如定时采样
#include <QComboBox>        //把图表包成控件放进 2×2 网格
#include <QDateTime>        //currentDateTime().toString(Qt::ISODate) 记录保存时间
#include <QDir>             //mkpath 建目录 + 拼接 history.db 路径
#include <QFileDialog>      //选择数据集 JSON 元数据
#include <QGridLayout>      //每图 X/Y 两根数值轴，共 8 根
#include <QGroupBox>        //"当前同步窗口指标"分组框
#include <QHeaderView>      //表头 ResizeToContents 自适应
#include <QHBoxLayout>      //顶部按钮工具栏
#include <QLabel>           //文件/元数据/指标/状态文字
#include <QLegend>          //图例，代码里 legend()->hide() 隐藏
#include <QLineSeries>      //4 条折线（振动/音频 × 时域/频谱）
#include <QMessageBox>      //warning / information 弹窗
#include <QPainter>         //关抗锯齿，降低树莓派绘制开销
#include <QPushButton>      //打开/开始/暂停/停止/保存
#include <QSqlError>        //失败时 lastError().text() 报错
#include <QSqlQuery>        //失败时 lastError().text() 报错
#include <QStandardPaths>   //AppDataLocation 取数据目录
#include <QStatusBar>       //底部状态栏，statusLabel 常驻
#include <QTableWidget>     //历史记录表（11 列）
#include <QThread>          //new QThread / moveToThread / quit / wait
#include <QValueAxis>       //每图 X/Y 两根数值轴，共 8 根
#include <QVariant>         //无音频时绑定空值 addBindValue(QVariant())
#include <QVBoxLayout>      //主界面垂直布局

#include <algorithm>        //std::minmax_element 求 Y 轴范围、std::max 下限保护


//构造函数与析构函数
//1.构造函数
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    //先构建界面（buildInterface）和初始化数据库（initializeDatabase）
    buildInterface();
    //每秒更新一次树莓派性能文本
    //这个定时器属于主窗口，但只执行很轻量的系统信息读取
    performanceTimer_ = new QTimer(this);
    performanceTimer_->setInterval(1000);
    connect(performanceTimer_,&QTimer::timeout,this,&MainWindow::updatePerformanceInfo);
    performanceTimer_->start();

    //窗口刚打开时立即显示一次，而不是等待第一秒
    updatePerformanceInfo();

    initializeDatabase();

    //DataWorker 没有父对象，移动到工作线程后由线程 finished 信号负责释放
    workerThread_ = new QThread(this);
    worker_ = new DataWorker;
    worker_->moveToThread(workerThread_);
    //线程结束 → 自动清理 worker
    connect(workerThread_,&QThread::finished,worker_,&QObject::deleteLater);
    //主线程 → 工作线程（命令方向，5 条）
    connect(this, &MainWindow::requestLoad, worker_, &DataWorker::loadDataset);
    connect(this, &MainWindow::requestStart, worker_, &DataWorker::startPlayback);
    connect(this, &MainWindow::requestPause, worker_, &DataWorker::pausePlayback);
    connect(this, &MainWindow::requestStop, worker_, &DataWorker::stopPlayback);
    connect(this, &MainWindow::requestSpeed, worker_, &DataWorker::setPlaybackSpeed);
    //工作线程 → 主线程（结果方向，4 条）
    connect(worker_, &DataWorker::metadataLoaded, this, &MainWindow::onMetadataLoaded);
    connect(worker_, &DataWorker::frameReady, this, &MainWindow::onFrameReady);
    connect(worker_, &DataWorker::statusMessage, this, &MainWindow::onStatusMessage);
    connect(worker_, &DataWorker::playbackFinished, this, [this] {
        startButton_->setText(QStringLiteral("重新播放"));
    });
    //启动线程
    workerThread_->start();
}

//2.析构函数
MainWindow::~MainWindow(){
    //先结束工作线程事件循环，再等待退出，避免关闭窗口后仍有定时器访问文件
    workerThread_->quit();
    workerThread_->wait(3000);
    if(database_.isOpen())
    {
        database_.close();
    }
}

//updatePerformanceInfo实时获取并显示当前 Qt 程序自身的性能指标
void MainWindow::updatePerformanceInfo() {
    // 如果性能标签控件尚未创建，则直接返回，避免空指针访问
    if (performanceLabel_ == nullptr) {
        return;
    }

    // 获取当前 Qt 程序自己的进程号（PID）
    const QString pid =
        QString::number(QCoreApplication::applicationPid());

    // 使用 ps 命令读取当前进程的 CPU、RSS 内存和线程数。
    // 这些字段与 scripts/collect_metrics.sh 中使用的字段保持一致。
    QProcess ps;
    QStringList arguments;
    arguments << QStringLiteral("-p")
              << pid
              << QStringLiteral("-o")
              << QStringLiteral("%cpu=")   // CPU 占用率
              << QStringLiteral("-o")
              << QStringLiteral("rss=")    // 常驻内存大小（KB）
              << QStringLiteral("-o")
              << QStringLiteral("nlwp=");  // 线程数

    // 启动 ps 进程，参数已准备好
    ps.start(QStringLiteral("ps"), arguments);

    // 最多等待 200 ms，避免系统命令异常时长时间阻塞界面
    if (!ps.waitForFinished(200)) {
        performanceLabel_->setText(
            QStringLiteral("设备性能：读取失败"));
        return;
    }

    // ps 输出类似：
    // 8.2 64256 8
    // simplified() 会去掉首尾空白并合并连续空格
    const QString output =
        QString::fromLocal8Bit(ps.readAllStandardOutput()).simplified();

    // 按空格分割输出，跳过空字段
    const QStringList columns =
        output.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    // 至少需要 3 列：CPU、RSS、线程数
    if (columns.size() < 3) {
        performanceLabel_->setText(
            QStringLiteral("设备性能：数据格式异常"));
        return;
    }

    // 用于记录解析是否成功的标志
    bool cpuOk = false;
    bool rssOk = false;
    bool threadOk = false;

    // 解析各字段：CPU 百分比、RSS（KB）、线程数
    const double cpuPercent =
        columns.at(0).toDouble(&cpuOk);
    const qint64 rssKb =
        columns.at(1).toLongLong(&rssOk);
    const qint64 threadCount =
        columns.at(2).toLongLong(&threadOk);

    // 任一字段解析失败，则提示数值解析失败并返回
    if (!cpuOk || !rssOk || !threadOk) {
        performanceLabel_->setText(
            QStringLiteral("设备性能：数值解析失败"));
        return;
    }

    // Raspberry Pi Linux 温度文件通常以千分之一摄氏度保存
    QFile temperatureFile(
        QStringLiteral("/sys/class/thermal/thermal_zone0/temp"));

    // 温度文本默认显示 "--"，表示未读取到
    QString temperatureText = QStringLiteral("--");
    if (temperatureFile.open(QIODevice::ReadOnly)) {
        bool temperatureOk = false;
        const double rawTemperature =
            QString::fromLatin1(
                temperatureFile.readAll().trimmed())
                .toDouble(&temperatureOk);

        if (temperatureOk) {
            // 原始值除以 1000 转换为摄氏度
            const double temperatureC = rawTemperature / 1000.0;
            temperatureText =
                QStringLiteral("%1 °C")
                    .arg(temperatureC, 0, 'f', 1);
        }
    }

    // RSS 从 KB 转换为 MB，使用 1024 作为换算单位
    const double rssMb =
        static_cast<double>(rssKb) / 1024.0;

    // 将所有性能信息格式化后显示到 performanceLabel_ 上
    performanceLabel_->setText(
        QStringLiteral(
            "设备性能: CPU %1% | RSS %2 MB | 线程 %3 | 温度 %4")
            .arg(cpuPercent, 0, 'f', 1)
            .arg(rssMb, 0, 'f', 1)
            .arg(threadCount)
            .arg(temperatureText));
}

//buildInterface：构建界面
void MainWindow::buildInterface(){
    setWindowTitle(QStringLiteral("轴承振动与音视频同步回放可视化终端"));
    resize(1400,920);
    //1.中央控件与根布局
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    //2.顶部工具栏
    auto *toolbar = new QHBoxLayout;
    //创建“打开数据”、“开始”、“暂停”、“停止”、“保存当前记录”五个按钮
    auto *openButton = new QPushButton(QStringLiteral("打开数据"),central);
    startButton_ = new QPushButton(QStringLiteral("开始"),central);
    pauseButton_ = new QPushButton(QStringLiteral("暂停"),central);
    stopButton_ = new QPushButton(QStringLiteral("停止"),central);
    saveButton_ = new QPushButton(QStringLiteral("保存当前记录"),central);
    //创建速度下拉框，每个选项绑定一个 double 值（1.0、2.0、4.0）
    speedBox_ = new QComboBox(central);
    speedBox_->addItem(QStringLiteral("1倍速"),1.0);
    speedBox_->addItem(QStringLiteral("2倍速"),2.0);
    speedBox_->addItem(QStringLiteral("4倍速"),4.0);
    //创建文件标签，显示当前文件路径，并允许鼠标选中复制
    fileLabel_ = new QLabel(QString("尚未选择数据"),central);
    fileLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    toolbar->addWidget(openButton);
    toolbar->addWidget(startButton_);
    toolbar->addWidget(pauseButton_);
    toolbar->addWidget(stopButton_);
    toolbar->addWidget(new QLabel(QStringLiteral("速度: "),central));
    toolbar->addWidget(speedBox_);
    toolbar->addWidget(saveButton_);
    toolbar->addWidget(fileLabel_,1);
    root->addLayout(toolbar);

    //3.元数据标签
    metadataLabel_ = new QLabel(QStringLiteral("等待加载数据集"),central);
    metadataLabel_->setWordWrap(true);
    root->addWidget(metadataLabel_);

    //4.四张图 ————界面的主角
    buildCharts();
    auto *chartGrid = new QGridLayout;
    auto *vibrationTimeView = new QChartView(vibrationTimeSeries_->chart(),central);
    auto *vibrationSpectrumView = new QChartView(vibrationSpectrumSeries_->chart(),central);
    auto *audioTimeView = new QChartView(audioTimeSeries_->chart(),central);
    auto *audioSpectrumView = new QChartView(audioSpectrumSeries_->chart(),central);
    for(QChartView *view:{vibrationTimeView,vibrationSpectrumView,audioTimeView,audioSpectrumView}){
        //树莓派上关闭曲线抗锯齿可显著减少重复绘制开销
        view->setRenderHint(QPainter::Antialiasing,false);
        view->setMinimumHeight(220);
    }
    chartGrid->addWidget(vibrationTimeView,0,0);
    chartGrid->addWidget(vibrationSpectrumView,0,1);
    chartGrid->addWidget(audioTimeView, 1, 0);
    chartGrid->addWidget(audioSpectrumView, 1, 1);
    chartGrid->setColumnStretch(0, 1);
    chartGrid->setColumnStretch(1, 1);
    root->addLayout(chartGrid, 1);

    //5.指标面板
    auto *statisticsBox = new QGroupBox(QStringLiteral("当前同步窗口指标"), central);
    auto *statistics = new QGridLayout(statisticsBox);
    vibrationRmsLabel_ = new QLabel("-", statisticsBox);
    vibrationPeakLabel_ = new QLabel("-", statisticsBox);
    vibrationKurtosisLabel_ = new QLabel("-", statisticsBox);
    vibrationFrequencyLabel_ = new QLabel("-", statisticsBox);
    audioRmsLabel_ = new QLabel("-", statisticsBox);
    audioPeakLabel_ = new QLabel("-", statisticsBox);
    audioKurtosisLabel_ = new QLabel("-", statisticsBox);
    audioFrequencyLabel_ = new QLabel("-", statisticsBox);
    processingLabel_ = new QLabel("-", statisticsBox);
 
    const QStringList headers = {QStringLiteral("信号"), QStringLiteral("AC RMS"),
                        QStringLiteral("峰值（去均值）"), QStringLiteral("峭度"),
                        QStringLiteral("主频")};
    for (int column = 0; column < headers.size(); ++column) {
        statistics->addWidget(new QLabel(headers[column], statisticsBox), 0, column);
    }
    statistics->addWidget(new QLabel(QStringLiteral("AIN 1 轴承振动"), statisticsBox), 1, 0);
    statistics->addWidget(vibrationRmsLabel_, 1, 1);
    statistics->addWidget(vibrationPeakLabel_, 1, 2);
    statistics->addWidget(vibrationKurtosisLabel_, 1, 3);
    statistics->addWidget(vibrationFrequencyLabel_, 1, 4);
    statistics->addWidget(new QLabel(QStringLiteral("AIN 8 音频"), statisticsBox), 2, 0);
    statistics->addWidget(audioRmsLabel_, 2, 1);
    statistics->addWidget(audioPeakLabel_, 2, 2);
    statistics->addWidget(audioKurtosisLabel_, 2, 3);
    statistics->addWidget(audioFrequencyLabel_, 2, 4);
    statistics->addWidget(new QLabel(QStringLiteral("两路 FFT 总耗时"), statisticsBox), 3, 0);
    statistics->addWidget(processingLabel_, 3, 1);
    root->addWidget(statisticsBox);

    //6.历史记录表
    historyTable_ = new QTableWidget(0, 11, central);
    historyTable_->setHorizontalHeaderLabels({
        QStringLiteral("保存时间"), QStringLiteral("数据集"), QStringLiteral("标签"),
        QStringLiteral("振动 RMS"), QStringLiteral("振动峰值"), QStringLiteral("振动峭度"),
        QStringLiteral("振动主频/Hz"), QStringLiteral("音频 RMS"),
        QStringLiteral("音频峰值"), QStringLiteral("音频峭度"),
        QStringLiteral("音频主频/Hz")
    });
    historyTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    historyTable_->setMaximumHeight(155);
    root->addWidget(historyTable_);

    //7.收尾
    setCentralWidget(central);
    statusLabel_ = new QLabel(QStringLiteral("就绪"), this);
    statusBar()->addPermanentWidget(statusLabel_);

    //使用QLabel显示性能信息，不使用可编辑文本框
    //性能信息只读，适合放在窗口底部状态栏
    performanceLabel_ = new QLabel(QStringLiteral("设备性能: 等待采样"),this);
    performanceLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addPermanentWidget(performanceLabel_);

    setControlsEnabled(false);

    //信号连接
    //主线程槽：弹文件框
    connect(openButton, &QPushButton::clicked, this, &MainWindow::chooseDataset);
    //跨线程 → worker 槽
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::requestStart);
    //跨线程 → worker 槽
    connect(pauseButton_, &QPushButton::clicked, this, &MainWindow::requestPause);
    //跨线程 → worker 槽
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::requestStop);
    //主线程槽：写 SQLite
    connect(saveButton_, &QPushButton::clicked, this, &MainWindow::saveCurrentRecord);
    connect(speedBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        emit requestSpeed(speedBox_->itemData(index).toDouble());
    });

}

//buildCharts:创建四张图表
void MainWindow::buildCharts(){
    // 每一路使用独立 Y 轴。AIN 1 与 AIN 8 都是未标定 raw 值，幅值范围可能
    // 完全不同，若叠在同一坐标轴上会压扁较小信号并造成错误比较。
    //lambda createChart 的参数
    auto createChart = [this](const QString &title,             //图表标题
                              QLineSeries *&series,             //输出参数。传入的是某个成员指针的引用，lambda 内部创建 QLineSeries 后，会直接写回这个成员变量
                              QValueAxis *&axisX,               //输出参数，用于写回 X 轴指针
                              QValueAxis *&axisY,               //输出参数，用于写回 Y 轴指针
                              bool frequencyDomain){
        series = new QLineSeries(this);
        auto *chart = new QChart;
        chart->setTitle(title);
        chart->legend()->hide();
        chart->addSeries(series);
        axisX = new QValueAxis(chart);
        axisY = new QValueAxis(chart);
        axisX->setTitleText(frequencyDomain ? QStringLiteral("频率/Hz"):QStringLiteral("时间/ms"));
        axisY->setTitleText(frequencyDomain ? QStringLiteral("幅度/dB"):QStringLiteral("原始幅值/raw"));
        chart->addAxis(axisX,Qt::AlignBottom);
        chart->addAxis(axisY,Qt::AlignLeft);
        series->attachAxis(axisX);
        series->attachAxis(axisY);
    };

    createChart(QStringLiteral("AIN 1 轴承振动 - 时域"), vibrationTimeSeries_,
            vibrationTimeAxisX_, vibrationTimeAxisY_, false);
    createChart(QStringLiteral("AIN 1 轴承振动 - FFT"), vibrationSpectrumSeries_,
            vibrationSpectrumAxisX_, vibrationSpectrumAxisY_, true);
    createChart(QStringLiteral("AIN 8 音频 - 时域"), audioTimeSeries_,
            audioTimeAxisX_, audioTimeAxisY_, false);
    createChart(QStringLiteral("AIN 8 音频 - FFT"), audioSpectrumSeries_,
            audioSpectrumAxisX_, audioSpectrumAxisY_, true);
}

//用户交互槽函数
//1.让用户通过文件对话框选择一个数据集元数据文件（JSON），然后把选择的路径显示到界面上，并发出一个 requestLoad(path) 信号，请求后续逻辑去加载这个数据集
void MainWindow::chooseDataset() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择数据集元数据"), QString(),
        QStringLiteral("JSON 元数据 (*.json)"));
    if (!path.isEmpty()) {
        fileLabel_->setText(path);
        emit requestLoad(path);
    }
}
//2.onMetadataLoaded在数据集元数据加载完成后被调用
void MainWindow::onMetadataLoaded(const DataMetadata &metadata) {
    // 保存加载到的元数据，供后续采集、显示和计算使用
    metadata_ = metadata;

    // 刚加载完元数据，还没有收到数据帧，重置帧标志
    hasFrame_ = false;

    // 将开始按钮恢复为“开始”状态
    startButton_->setText(QStringLiteral("开始"));

    // 根据是否存在音频通道，生成音频描述文本
    const QString audioDescription = metadata.hasAudio
        ? QStringLiteral("%1(%2,%3)")
              .arg(metadata.audio.channelName, metadata.audio.signalName,
                   metadata.audio.unit)
        : QStringLiteral("无（兼容旧版单通道数据）");

    // 将元数据格式化后显示到界面的 metadataLabel_ 上
    metadataLabel_->setText(
        QStringLiteral("名称：%1 标签：%2 采样率：%3 Hz 样本数：%4\n"
                       "振动：%5(%6,%7) 音频：%8 转速：%9 rpm 负载：%10")
            .arg(metadata.name, metadata.label)                  // 名称、标签
            .arg(metadata.sampleRate, 0, 'f', 0)                 // 采样率，保留 0 位小数
            .arg(metadata.sampleCount)                           // 样本数
            .arg(metadata.vibration.channelName, metadata.vibration.signalName,
                 metadata.vibration.unit)                        // 振动通道信息
            .arg(audioDescription)                               // 音频描述
            .arg(metadata.rpm, 0, 'f', 0)                        // 转速，保留 0 位小数
            .arg(metadata.load, 0, 'f', 0));                     // 负载，保留 0 位小数

    // 如果数据集中没有音频通道，则清空音频相关图表和统计标签
    if (!metadata.hasAudio) {
        audioTimeSeries_->clear();          // 清空音频时域序列
        audioSpectrumSeries_->clear();      // 清空音频频谱序列
        audioRmsLabel_->setText(QStringLiteral("无音频")); // RMS 显示“无音频”
        audioPeakLabel_->setText("-");      // 峰值显示 "-"
        audioKurtosisLabel_->setText("-");  // 峭度显示 "-"
        audioFrequencyLabel_->setText("-"); // 主频显示 "-"
    }

    // 元数据加载完成，启用界面控件，允许用户继续操作
    setControlsEnabled(true);
}
//3.updateChannelCharts根据给定的时间轴和信号分析结果，更新一个通道的时域图和频域图
void MainWindow::updateChannelCharts(const QVector<double> &timeMs,
                                     const SignalAnalysis &analysis,
                                     QLineSeries *timeSeries,
                                     QValueAxis *timeAxisX,
                                     QValueAxis *timeAxisY,
                                     QLineSeries *spectrumSeries,
                                     QValueAxis *spectrumAxisX,
                                     QValueAxis *spectrumAxisY) {
    // 参数说明：
    // timeMs          —— 时域横坐标，单位毫秒
    // analysis        —— 信号分析结果，包含时域样本 samples、频率轴 frequencyHz、幅度 magnitudeDb
    // timeSeries      —— 时域折线序列
    // timeAxisX/Y     —— 时域图的 X/Y 轴
    // spectrumSeries  —— 频域折线序列
    // spectrumAxisX/Y —— 频域图的 X/Y 轴

    // 如果时间轴或分析结果为空（例如无数据、未计算或数据无效），
    // 则清空时域和频域序列，避免显示旧数据，然后直接返回。
    if (timeMs.isEmpty() || analysis.samples.isEmpty() ||
        analysis.frequencyHz.isEmpty() || analysis.magnitudeDb.isEmpty()) {
        timeSeries->clear();
        spectrumSeries->clear();
        return;
    }

    // ---------- 更新时域图 ----------

    // 构建时域数据点：横坐标为 timeMs，纵坐标为 analysis.samples
    QList<QPointF> timePoints;
    timePoints.reserve(analysis.samples.size());
    for (int i = 0; i < analysis.samples.size(); ++i) {
        timePoints.append(QPointF(timeMs[i], analysis.samples[i]));
    }

    // 用时域数据点替换折线序列中的全部数据
    timeSeries->replace(timePoints);

    // 找出时域样本中的最小值和最大值，用于自动设置 Y 轴范围
    const auto [minimum, maximum] =
        std::minmax_element(analysis.samples.cbegin(), analysis.samples.cend());

    // 计算 Y 轴上下边距：取幅值范围的 10%，但不小于 1e-6，避免范围为 0 时出错
    const double timeMargin = std::max(1e-6, (*maximum - *minimum) * 0.1);

    // 设置时域 X 轴范围：从第一个时间点到最后一个时间点
    timeAxisX->setRange(timeMs.first(), timeMs.last());

    // 设置时域 Y 轴范围：在最小值和最大值基础上各留出 timeMargin 的边距
    timeAxisY->setRange(*minimum - timeMargin, *maximum + timeMargin);

    // ---------- 更新频域图 ----------

    // 构建频域数据点：横坐标为频率 frequencyHz，纵坐标为幅度 magnitudeDb
    QList<QPointF> spectrumPoints;
    spectrumPoints.reserve(analysis.frequencyHz.size());
    for (int i = 0; i < analysis.frequencyHz.size(); ++i) {
        spectrumPoints.append(
            QPointF(analysis.frequencyHz[i], analysis.magnitudeDb[i]));
    }

    // 用频域数据点替换折线序列中的全部数据
    spectrumSeries->replace(spectrumPoints);

    // 找出频域幅度中的最小值和最大值，用于自动设置 Y 轴范围
    const auto [dbMinimum, dbMaximum] =
        std::minmax_element(analysis.magnitudeDb.cbegin(),
                            analysis.magnitudeDb.cend());

    // 设置频域 X 轴范围：从 0 到采样率的一半（奈奎斯特频率）
    spectrumAxisX->setRange(0.0, metadata_.sampleRate / 2.0);

    // 设置频域 Y 轴范围：从最小 dB 到最大 dB 再加 3 dB 顶部余量
    spectrumAxisY->setRange(*dbMinimum, *dbMaximum + 3.0);
}

//onFrameReady当一帧分析结果准备好时，更新界面上的所有相关显示
void MainWindow::onFrameReady(const AnalysisFrame &frame) {
    // 保存当前分析帧，供后续操作（如保存、导出、重新计算）使用
    currentFrame_ = frame;

    // 标记已经收到至少一帧数据
    hasFrame_ = true;

    // ---------- 更新振动通道图表 ----------
    // 使用当前帧的时间轴和振动分析结果，刷新振动时域图与频域图
    updateChannelCharts(frame.timeMs, frame.vibration,
                        vibrationTimeSeries_, vibrationTimeAxisX_, vibrationTimeAxisY_,
                        vibrationSpectrumSeries_, vibrationSpectrumAxisX_,
                        vibrationSpectrumAxisY_);

    // ---------- 更新音频通道图表（仅当存在音频时） ----------
    if (frame.hasAudio) {
        // 使用当前帧的时间轴和音频分析结果，刷新音频时域图与频域图
        updateChannelCharts(frame.timeMs, frame.audio,
                            audioTimeSeries_, audioTimeAxisX_, audioTimeAxisY_,
                            audioSpectrumSeries_, audioSpectrumAxisX_, audioSpectrumAxisY_);
    }

    // ---------- 更新振动通道统计标签 ----------
    // RMS，保留 5 位小数
    vibrationRmsLabel_->setText(QString::number(frame.vibration.rms, 'f', 5));
    // 峰值，保留 5 位小数
    vibrationPeakLabel_->setText(QString::number(frame.vibration.peak, 'f', 5));
    // 峭度，保留 3 位小数
    vibrationKurtosisLabel_->setText(QString::number(frame.vibration.kurtosis, 'f', 3));
    // 主频，保留 1 位小数，并附加单位 Hz
    vibrationFrequencyLabel_->setText(
        QStringLiteral("%1 Hz").arg(frame.vibration.dominantFrequency, 0, 'f', 1));

    // ---------- 更新音频通道统计标签（仅当存在音频时） ----------
    if (frame.hasAudio) {
        // 音频 RMS，保留 5 位小数
        audioRmsLabel_->setText(QString::number(frame.audio.rms, 'f', 5));
        // 音频峰值，保留 5 位小数
        audioPeakLabel_->setText(QString::number(frame.audio.peak, 'f', 5));
        // 音频峭度，保留 3 位小数
        audioKurtosisLabel_->setText(QString::number(frame.audio.kurtosis, 'f', 3));
        // 音频主频，保留 1 位小数，并附加单位 Hz
        audioFrequencyLabel_->setText(
            QStringLiteral("%1 Hz").arg(frame.audio.dominantFrequency, 0, 'f', 1));
    }

    // ---------- 更新处理耗时标签 ----------
    // 显示本帧处理耗时，保留 3 位小数，单位 ms
    processingLabel_->setText(QStringLiteral("%1 ms").arg(frame.processingMs, 0, 'f', 3));

    // ---------- 更新状态栏 ----------
    // 显示当前同步窗口编号以及该窗口的起始样本索引
    statusLabel_->setText(QStringLiteral("同步窗口 %1,起始样本 %2")
                              .arg(frame.windowindex)
                              .arg(frame.firstSample));
}

//onStatusMessage统一处理状态消息的显示
void MainWindow::onStatusMessage(const QString &message, bool isError) {
    // 无论是否为错误消息，都先把消息文本显示到状态标签上
    statusLabel_->setText(message);

    // 如果 isError 为 true，表示这是一条错误消息
    if (isError) {
        // 弹出警告对话框：
        // 父窗口为当前 MainWindow，
        // 标题为“运行提示”，
        // 内容为传入的 message
        QMessageBox::warning(this, QStringLiteral("运行提示"), message);
    }
}

//initializeDatabase初始化 SQLite 数据库，并准备好历史记录表
void MainWindow::initializeDatabase() {
    // 创建/获取名为 "bearing_audio_history" 的 SQLite 数据库连接
    database_ = QSqlDatabase::addDatabase("QSQLITE", "bearing_audio_history");

    // 获取当前应用的本地数据目录（不同操作系统路径不同）
    const QString appData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // 确保该数据目录存在，不存在则递归创建
    QDir().mkpath(appData);

    // 设置数据库文件路径：AppDataLocation/history.db
    database_.setDatabaseName(QDir(appData).filePath("history.db"));

    // 尝试打开数据库，失败则在状态栏提示错误并直接返回
    if (!database_.open()) {
        statusBar()->showMessage(QStringLiteral("数据库打开失败：%1")
                                     .arg(database_.lastError().text()));
        return;
    }

    // 使用新的表名，不破坏用户已经由旧版程序保存的 analysis_history 表。
    // 音频缺失时对应字段写入 SQL NULL，避免把“没有数据”误记为数值 0。
    QSqlQuery query(database_);

    // 创建历史记录表 dual_analysis_history（如果尚不存在）
    // 字段包括：id、保存时间、数据集、标签、振动统计量、音频统计量
    if (!query.exec(
            "CREATE TABLE IF NOT EXISTS dual_analysis_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, saved_at TEXT NOT NULL, "
            "dataset TEXT NOT NULL, label TEXT NOT NULL, "
            "vibration_rms REAL, vibration_peak REAL, vibration_kurtosis REAL, "
            "vibration_frequency REAL, audio_rms REAL, audio_peak REAL, "
            "audio_kurtosis REAL, audio_frequency REAL)")) {
        // 建表失败，在状态栏显示具体错误信息
        statusBar()->showMessage(QStringLiteral("建表失败：%1").arg(query.lastError().text()));
    }

    // 刷新历史记录显示（从数据库读取并更新界面）
    refreshHistory();
}

//saveCurrentRecord把当前分析帧的振动和音频统计结果保存到 SQLite 数据库的历史记录表 dual_analysis_history 中
void MainWindow::saveCurrentRecord() {
    // 如果没有可保存的分析帧，或者数据库未打开，则提示用户并返回
    if (!hasFrame_ || !database_.isOpen()) {
        QMessageBox::information(this, QStringLiteral("保存记录"),
                                 QStringLiteral("当前没有可保存的分析结果"));
        return;
    }

    // 准备插入语句，向历史记录表 dual_analysis_history 写入一条记录
    QSqlQuery query(database_);
    query.prepare(
        "INSERT INTO dual_analysis_history "
        "(saved_at, dataset, label, vibration_rms, vibration_peak, "
        "vibration_kurtosis, vibration_frequency, audio_rms, audio_peak, "
        "audio_kurtosis, audio_frequency) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    // 绑定保存时间：当前日期时间，ISO 8601 格式
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));

    // 绑定数据集名称和标签
    query.addBindValue(metadata_.name);
    query.addBindValue(metadata_.label);

    // 绑定振动通道的统计量：RMS、峰值、峭度、主频
    query.addBindValue(currentFrame_.vibration.rms);
    query.addBindValue(currentFrame_.vibration.peak);
    query.addBindValue(currentFrame_.vibration.kurtosis);
    query.addBindValue(currentFrame_.vibration.dominantFrequency);

    // 如果当前帧包含音频，则绑定音频通道的统计量
    if (currentFrame_.hasAudio) {
        query.addBindValue(currentFrame_.audio.rms);
        query.addBindValue(currentFrame_.audio.peak);
        query.addBindValue(currentFrame_.audio.kurtosis);
        query.addBindValue(currentFrame_.audio.dominantFrequency);
    } else {
        // 没有音频时，为音频的 4 个字段绑定 SQL NULL（QVariant 默认构造即为 NULL）
        // 这样数据库中不会把“没有音频”误记为数值 0
        for (int i = 0; i < 4; ++i) {
            query.addBindValue(QVariant());
        }
    }

    // 执行插入语句，失败则弹出警告并返回
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("数据库错误"), query.lastError().text());
        return;
    }

    // 插入成功后，刷新界面上的历史记录显示
    refreshHistory();
}

//refreshHistory从 SQLite 数据库中读取最近 20 条历史记录，并刷新界面上的历史记录表格 historyTable_
void MainWindow::refreshHistory() {
    // 如果数据库未打开，直接返回，不做任何操作
    if (!database_.isOpen()) {
        return;
    }

    // 创建查询对象，准备从历史记录表中读取数据
    QSqlQuery query(database_);

    // 查询最近 20 条历史记录，按 id 降序排列（最新的在最前）
    // 查询字段包括：保存时间、数据集、标签、振动统计量、音频统计量
    if (!query.exec(
            "SELECT saved_at, dataset, label, vibration_rms, vibration_peak, "
            "vibration_kurtosis, vibration_frequency, audio_rms, audio_peak, "
            "audio_kurtosis, audio_frequency FROM dual_analysis_history "
            "ORDER BY id DESC LIMIT 20")) {
        // 查询失败时，在状态栏显示错误信息，然后返回
        statusBar()->showMessage(QStringLiteral("读取历史记录失败：%1")
                                     .arg(query.lastError().text()));
        return;
    }

    // 清空表格中原有的所有行，准备重新填充
    historyTable_->setRowCount(0);

    // 遍历查询结果集，逐行填充表格
    while (query.next()) {
        // 获取当前表格的行数，作为新插入行的索引
        const int row = historyTable_->rowCount();

        // 在表格末尾插入一个新行
        historyTable_->insertRow(row);

        // 循环填充该行的 11 个列
        for (int column = 0; column < 11; ++column) {
            // 将查询结果中对应列的值转为字符串，并设置为表格单元格的内容
            historyTable_->setItem(row, column,
                                   new QTableWidgetItem(query.value(column).toString()));
        }
    }
}

//批量启用或禁用界面上与采集流程相关的控件
void MainWindow::setControlsEnabled(bool enabled) {
    // 根据传入的 enabled 值，统一启用或禁用与采集控制相关的按钮和控件。
    // 这样可以在数据未加载或正在处理时，避免用户误操作。

    startButton_->setEnabled(enabled);    // 开始按钮
    pauseButton_->setEnabled(enabled);    // 暂停按钮
    stopButton_->setEnabled(enabled);     // 停止按钮
    saveButton_->setEnabled(enabled);     // 保存按钮
    speedBox_->setEnabled(enabled);       // 速度选择框
}
