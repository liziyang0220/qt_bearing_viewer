#pragma once

#include "analysisframe.h"
#include <QMainWindow>      //主窗口的基类。
#include <QSqlDatabase>     //用于 SQLite 数据库操作

//前向声明
class QComboBox;
class QLabel;
class QLineSeries;
class QPushButton;
class QTableWidget;
class QThread;
class QTimer;
class QValueAxis;
class DataWorker;

//类声明与构造函数
class MainWindow final : public QMainWindow{
    //Qt 宏，必须放在类定义的第一行。启用信号槽机制和元对象系统
    Q_OBJECT

public:
    //explicit:防止构造函数被隐式类型转换
    explicit MainWindow(QWidget *parent = nullptr);
    //析构函数，负责在窗口关闭时安全停止工作线程、关闭数据库，避免资源泄漏或崩溃
    ~MainWindow() override;

//发送给工作线程的命令
signals:
    void requestLoad(const QString &metadataPath);  //请求加载 JSON 元数据
    void requestStart();                            //请求开始回放
    void requestPause();                            //请求暂停
    void requestStop();                             //请求停止并回到起点
    void requestSpeed(double speed);                //请求切换倍速（1/2/4 倍速）

//接收工作线程的结果和用户操作
private slots:
    //点击“打开数据”按钮时调用，弹出文件选择对话框，然后发出 requestLoad 信号
    void chooseDataset();
    //收到 DataWorker::metadataLoaded 信号后调用，更新界面上的元数据标签（名称、采样率、通道信息等）
    void onMetadataLoaded(const DataMetadata &metadata);
    //收到 DataWorker::frameReady 信号后调用。这是更新界面的核心函数，负责更新四张图表和两组指标标签
    void onFrameReady(const AnalysisFrame &frame);
    //收到 DataWorker::statusMessage 信号后调用，更新状态栏文字；如果是错误，弹出警告框
    void onStatusMessage(const QString &message,bool isError);
    //点击“保存当前记录”按钮时调用，把当前窗口的指标写入 SQLite
    void saveCurrentRecord();
    //刷新历史记录表格
    void refreshHistory();
    //每秒读取一次树莓派当前进程的CPU、内存、线程数和温度
    void updatePerformanceInfo();

//辅助函数
private:
    //创建所有控件：按钮、标签、速度下拉框、统计面板、历史表格等
    void buildInterface();
    //创建四套图表：振动时域、振动频谱、音频时域、音频频谱
    void buildCharts();
    //初始化 SQLite 数据库，建表，准备历史记录
    void initializeDatabase();
    //根据当前状态启用或禁用按钮（如未加载数据时禁用“开始”）
    void setControlsEnabled(bool enabled);
    //通用图表更新函数。因为振动和音频的绘图逻辑完全一样，只是传入的曲线和坐标轴不同，所以提取为一个函数复用
    void updateChannelCharts(const QVector<double> &timeMs,   
                             const SignalAnalysis &analysis,
                             QLineSeries *timeSeries,
                             QValueAxis *timeAxisX,
                             QValueAxis *timeAxisY,
                             QLineSeries *spectrumSeries,
                             QValueAxis *spectrumAxisX,
                             QValueAxis *spectrumAxisY);

    //成员变量：线程、数据库和状态
    QThread *workerThread_ = nullptr;   //工作线程对象
    DataWorker *worker_ = nullptr;      //工作线程中的计算对象
    QSqlDatabase database_;             //SQLite 数据库连接
    DataMetadata metadata_;             //当前数据集的元数据
    AnalysisFrame currentFrame_;        //当前窗口的分析结果，用于保存记录
    bool hasFrame_ = false;             //标记当前是否有可保存的分析结果

    //成员变量：UI控件
    //1.标签类（显示信息）
    QLabel *fileLabel_ = nullptr;         // 显示当前文件路径
    QLabel *statusLabel_ = nullptr;       // 状态栏提示
    QLabel *metadataLabel_ = nullptr;     // 数据集元信息
    QLabel *vibrationRmsLabel_ = nullptr; // 振动 RMS
    QLabel *vibrationPeakLabel_ = nullptr;
    QLabel *vibrationKurtosisLabel_ = nullptr;
    QLabel *vibrationFrequencyLabel_ = nullptr;
    QLabel *audioRmsLabel_ = nullptr;     // 音频 RMS
    QLabel *audioPeakLabel_ = nullptr;
    QLabel *audioKurtosisLabel_ = nullptr;
    QLabel *audioFrequencyLabel_ = nullptr;
    QLabel *processingLabel_ = nullptr;   // 两路 FFT 总耗时
    //窗口底部显示树莓派运行状态
    QLabel *performanceLabel_ = nullptr;
    //每秒触发一次性能信息更新
    QTimer *performanceTimer_ = nullptr;

    //2.按钮和下拉框(用户控制)
    QPushButton *startButton_ = nullptr;
    QPushButton *pauseButton_ = nullptr;
    QPushButton *stopButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QComboBox *speedBox_ = nullptr;

    //3.图表曲线和坐标轴(四张图)
    QLineSeries *vibrationTimeSeries_ = nullptr;      // 振动时域曲线
    QLineSeries *vibrationSpectrumSeries_ = nullptr;  // 振动频谱曲线
    QLineSeries *audioTimeSeries_ = nullptr;          // 音频时域曲线
    QLineSeries *audioSpectrumSeries_ = nullptr;      // 音频频谱曲线

    QValueAxis *vibrationTimeAxisX_ = nullptr;        // 振动时域 X 轴
    QValueAxis *vibrationTimeAxisY_ = nullptr;        // 振动时域 Y 轴
    QValueAxis *vibrationSpectrumAxisX_ = nullptr;    // 振动频谱 X 轴
    QValueAxis *vibrationSpectrumAxisY_ = nullptr;    // 振动频谱 Y 轴
    QValueAxis *audioTimeAxisX_ = nullptr;            // 音频时域 X 轴
    QValueAxis *audioTimeAxisY_ = nullptr;            // 音频时域 Y 轴
    QValueAxis *audioSpectrumAxisX_ = nullptr;        // 音频频谱 X 轴
    QValueAxis *audioSpectrumAxisY_ = nullptr;        // 音频频谱 Y 轴
    
    //4.历史记录表格
    QTableWidget *historyTable_ = nullptr;          //用于显示 SQLite 中保存的历史记录，最多显示 20 条
};


