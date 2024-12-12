#include "myLog.h"

MyLog *MyLog::get_instance()
{
    static MyLog instance;
    return &instance;
}

MyLog::MyLog()
{
    logLineCnt = 0;
    isSync = false;
}

MyLog::~MyLog()
{
    if (myFp != NULL)
    {
        fclose(myFp);
    }
}

// 其中logFileName为日志文件的绝对路径名称参考
bool MyLog::init(const char *logFileName, int logBufSize_, int maxLine_)
{
    logBufSize = logBufSize_;
    logBuf = new char[logBufSize];
    memset(logBuf, 0, sizeof(logBuf));

    maxLine = maxLine_;

    // 以创建日志时间作日志的名字
    time_t t = time(NULL);
    struct tm *sysTime = localtime(&t); // localtime将t的值用本地时间表示填入结构体sys_time
    struct tm myTime = *sysTime;

    const char *p = strrchr(logFileName, '/'); // 从后往前找到第一个对应的字符
    char logFullName[256];                // 临时存储日志全名

    // 定义一个当前创建的日志文件名
    if (p == NULL) // 当logFileName没有'/'，也就是没有指定目录
    {
        // 那就直接将时间+文件名作为日志文件名
        //%02d：表示一个整数，并且如果数字小于两位数，则用零填充（用于月份和日期）
        snprintf(logFullName, 256, "%d_%02d_%02d_%s.txt", myTime.tm_year + 1900, myTime.tm_mon + 1, myTime.tm_mday, logFileName);
    }
    else
    {
        strcpy(logName, p + 1);                             // 将文件名存入成员变量
        strncpy(dirName, logFileName, p - logFileName + 1); // 路径会包含'/'所以需要+1
        // 日志路径/YYYY_MM_DD_日志名
        snprintf(logFullName, 256, "%s%d_%02d_%02d_%s.txt", dirName, myTime.tm_year + 1900, myTime.tm_mon + 1, myTime.tm_mday, logName);
    }

    today = myTime.tm_mday;

    myFp = fopen(logFullName, "a");
    if (myFp == NULL)
        return false;
    return true;
}

// 注意，所有涉及到内部变量的赋值的部分都需要加锁，因为这个单例本身就是临界资源
void MyLog::write_log(int type,const char *format, ...)
{
    struct timeval nowTimeval = {0, 0};
    gettimeofday(&nowTimeval, NULL);
    time_t t = nowTimeval.tv_sec;
    struct tm *sysTime = localtime(&t);
    struct tm myTime = *sysTime;
    char logHead[16] = {0}; // 日志消息的头部，表示当前消息是哪种类型

    // 头部追加日志消息类型
    switch (type)
    {
    case 0:
    {
        strcpy(logHead, "[debug]:");
        break;
    }
    case 1:
    {
        strcpy(logHead, "[info]:");
        break;
    }
    case 2:
    {
        strcpy(logHead, "[warn]:");
        break;
    }
    case 3:
    {
        strcpy(logHead, "[error]:");
        break;
    }
    default:
    {
        strcpy(logHead, "[info]:");
        break;
    }
    }

    //初始化可变参数，放在互斥锁外
    va_list valList;
    va_start(valList, format);

    log_mtx.lock();// 此锁保护today myFp
    logLineCnt++;

    // 日志不是今天或写入日志行数是最大行的倍数(说明刚好需要创建新的日志文件，或者是服务器运行跨日期了)
    if (today != myTime.tm_mday || logLineCnt % maxLine == 0)
    {
        char newLogFullName[256] = {0};
        fflush(myFp);//如果创建新的文件需要fflush掉缓冲区之后创建新的文件描述符，这样就不用担心别的已经添加到缓冲区的日志内容损失
        fclose(myFp);
        char nameDate[16] = {0}; // 日志的日期部分

        // 名称的日期部分
        snprintf(nameDate, 16, "%d_%02d_%02d_", myTime.tm_year + 1900, myTime.tm_mon + 1, myTime.tm_mday);

        // 同样的：日志路径/YYYY_MM_DD_日志名
        snprintf(newLogFullName, 256, "%s%s%s", dirName, nameDate, logName);

        // 如果过了一天，创建新的日志，更新日期、行数
        if (today != myTime.tm_mday)
        {
            today = myTime.tm_mday;
            logLineCnt = 0;
        }
        else // 超过最大行，追加日志编号
        {
            snprintf(newLogFullName + strlen(newLogFullName), 256 - strlen(newLogFullName), ".%lld.txt", logLineCnt / maxLine);
        }
        myFp = fopen(newLogFullName, "a");
    }

    log_mtx.unlock();


    log_mtx.lock();//保护logBuf
    // 写日志内容
    // 日志时间前缀 YYYY-MM-DD HH:MM:SS.
    int headLen = snprintf(logBuf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                           myTime.tm_year + 1900, myTime.tm_mon + 1, myTime.tm_mday,
                           myTime.tm_hour, myTime.tm_min, myTime.tm_sec, nowTimeval.tv_sec, logHead);
    // 日志内容本体
    int contentLen = vsnprintf(logBuf + headLen, logBufSize, format, valList);
    logBuf[headLen + contentLen] = '\n';
    logBuf[headLen + contentLen + 1] = '\0';

    std::string logStr = logBuf;

    log_mtx.unlock();

    // 日志写入缓冲区,IO操作速度慢，放在临界区操作之外的空间，减少占用临界资源时间
    fputs(logStr.c_str(),myFp);

    va_end(valList);
}

// 直接将缓冲冲入文件
void MyLog::flush()
{
    log_mtx.lock();
    fflush(myFp);
    log_mtx.unlock();
}