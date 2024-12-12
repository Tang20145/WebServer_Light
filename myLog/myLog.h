#ifndef MYLOG_H_
#define MYLOG_H_

#include <stdio.h>
#include <string.h>
#include <string>
#include <pthread.h>
#include "../synchronize/synchronize.h"

#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

#define LOG_DEBUG(format, ...) MyLog::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) MyLog::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARM(format, ...) MyLog::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) MyLog::get_instance()->write_log(3, format, ##__VA_ARGS__)

#define LOG_FLUSH() MyLog::get_instance()->flush()

// 单例模式日志类，

class MyLog
{
    // 共有函数
public:
    static MyLog *get_instance();

    bool init(const char *logFileName, int logBufSize_ = 8192, int maxLine_ = 5000000);
    void write_log(int type,const char *format, ...);
    void flush();

    // 私有成员函数
private:
    MyLog();
    ~MyLog();
    // 成员变量
private:
    char dirName[128];    // 路径名
    char logName[128];    // log文件名
    int maxLine;          // 日志最大行数
    int logBufSize;       // 日志缓冲区大小
    long long logLineCnt; // 日志行数
    int today;            // 当前是哪一天？？？？
    FILE *myFp;           // 打开log的文件指针
    char *logBuf;         // 缓冲区？？？
    // 阻塞队列，使用同步写入日志方式，不使用阻塞队列
    bool isSync;  // 同步标志
    mutx log_mtx; // 互斥锁
};

#endif