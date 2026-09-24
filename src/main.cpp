#include "analysisframe.h"
#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[]) {
    // 创建 Qt 应用程序对象。
    // QApplication 负责管理应用程序级资源、事件循环以及 GUI 相关初始化。
    QApplication application(argc, argv);

    // 自定义类型需要注册后才能安全地通过跨线程信号槽传递。
    // DataMetadata 表示数据集元数据，AnalysisFrame 表示一帧分析结果。
    // 注册后，Qt 元对象系统可以在队列连接（QueuedConnection）中
    // 对这些类型进行复制、构造和传递。
    qRegisterMetaType<DataMetadata>("DataMetadata");
    qRegisterMetaType<AnalysisFrame>("AnalysisFrame");

    // 创建主窗口对象
    MainWindow window;

    // 显示主窗口
    window.show();

    // 进入 Qt 事件循环。
    // 程序会在此等待并处理事件，直到用户关闭窗口或调用 quit() 退出。
    return application.exec();
}