#ifndef TIMER_H_
#define TIMER_H_

#include <netinet/in.h>
#include <stdio.h>
#include <time.h>

#include "../myLog/myLog.h"

// 超时计时器(链表单体)
class OutTimer;
// 客户端连接资源
struct ClientData;
// 超时计时器链表
class OutTimerList;

class OutTimer
{
public:
    OutTimer();

public:
    // 存储的是超时的绝对时刻（格林尼治时间1970年1月1日00:00:00到当前时刻的时长
    time_t expireTime;
    // 定时事件，回调函数
    void (*expireFunc)(ClientData *);
    // 连接资源
    ClientData *clientData;
    // 作为链表单元，需要前驱后继指针
    OutTimer *pre;
    OutTimer *next;
};

struct ClientData
{
    sockaddr_in address;
    int sockFd;
    OutTimer *timer;
};

class OutTimerList
{
public:
    OutTimerList();
    ~OutTimerList();

    void add_timer(OutTimer *timer);    // 添加定时器(保证升序)
    void adjust_timer(OutTimer *timer); // 调整定时器(当有一个连接活跃了，需要更新定时器的超时时间，也需要调用此函数更新定时器链表的顺序，保证升序)
    void del_timer(OutTimer *timer);    // 删除定时器
    void tick();                        // 定时任务处理函数，每一个时钟间隔执行一次

private:
    void add_timer(OutTimer *timer, OutTimer *subHead); //(保证在subHead之后升序)

private:
    OutTimer *head;
    OutTimer *tail;
};

#endif