#include <sys/socket.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <iostream>

#include "threadpool/threadpool.h"
#include "myHttp/myHttp.h"
#include "mysql/mysqlPool.h"
#include "synchronize/synchronize.h"
#include "timer/timer.h"

#define LT

#pragma region 变量

int ret; // 临时存储返回值用于校验

int epollfd;

// 最多连接数
const int MAX_FD = 65536;
const int MAX_EVENT_NUM = 10000;
// 服务器是否停止
bool stop_server = false;

// 线程池
threadPool<myHttp> *threadPoolHttps;

// 数据库连接池
mysqlPool *mysqlConnectPool;

// 时钟系统相关变量
const int CHECK_TIME = 5;       // 检查非活跃连接的时间间隔
int pipeFd[2];                  // 管道
bool checkTimeout = false;      // 默认超时状态
ClientData clientDatas[MAX_FD]; // 连接资源
OutTimerList outTimerList;      // 定时器链表
// OutTimerList outTimerList[MAX_FD]; // 定时器链表

#pragma endregion

#pragma region 函数

#pragma region myHttp
extern int setNonblock(int fd);
extern void registerFd(int epollfd, int fd, bool oneShot);// 来自myHttp.cpp
extern void registerFd(int epollfd, int fd, bool oneShot);

#pragma endregion

// 将info消息在服务器上显示，同时将其发送到客户端并关闭连接
void error_send(int fd, const char *info)
{
    printf("%s\n", info);
    send(fd, info, strlen(info), 0);
    close(fd);
}

#pragma region 时钟系统

// 信号处理函数
// 统一事件源
// 信号处理函数 往管道的写端写入信号值，主循环则从管道的读端读出信号值，使用epoll来监听管道读端的可读事件
// 以此，减少信号处理函数异步执行时间
void signal_transport(int sig)
{
    // 可重入函数，send会修改errno，所以需要对齐进行恢复
    int saveErrno = errno;
    int sendSig = sig;

    send(pipeFd[1], (char *)&sendSig, sizeof(sendSig), 0);

    errno = saveErrno;
}

// 修改信号处理方式
void setSignal(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;           // 使被信号打断的系统调用自动重新发起
    sigfillset(&sa.sa_mask);                 // 将sa_mask信号集初始化，然后把所有的信号加入到此信号集里，也就是说，异步执行信号处理函数期间屏蔽所有信号
    assert(sigaction(sig, &sa, NULL) != -1); // 修改执行信号的处理方式
}

// 作为时钟的回调函数，处理非活跃连接（或直接关闭连接，可以同时将套接字的定时器以及epoll监听去除
void closeUnactiveSocket(ClientData *clientData)
{
    assert(clientData);
    // 取消在epoll上的注册监听事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, clientData->sockFd, 0);

    // 关闭
    close(clientData->sockFd);

    // 减少连接数
    myHttp::userCnt--;
}

// 定时任务，更新时钟（清除不必要的时钟
void timer_update()
{
    outTimerList.tick();
    alarm(CHECK_TIME);
}

#pragma endregion

#pragma endregion

////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    // 日志初始化
    MyLog::get_instance()->init("./record/ServerLog");
    LOG_INFO("start server");
    LOG_FLUSH();
    int port = atoi(argv[1]);

#pragma region 监听套接字一套流程
    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    assert(serv_sock >= 0);

    int ret = 0;
    sockaddr_in address;
    memset(&address, 0, sizeof(sockaddr_in));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int x = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
    ret = bind(serv_sock, (sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(serv_sock, 5);
    assert(ret >= 0);

#pragma endregion

    // 创建MAX_FD个myHttp，对应MAX_FD个最大客户端数量
    myHttp *https = new myHttp[MAX_FD];
    assert(https);

    epoll_event myEvents[MAX_EVENT_NUM];
    epollfd = epoll_create(1);
    assert(epollfd != -1);
    registerFd(epollfd, serv_sock, false); // 因为是监听的套接字，不需要一次性读完
    myHttp::myEpollfd = epollfd;

    // 数据库连接池初始化
    mysqlPool *mysqlConnectPool = mysqlPool::GetInstance();
    mysqlConnectPool->init(8, "localhost", 3306, "root", "root", "webserverDB", 0);
    // 对应数据库创建
    // CREATE DATABASE IF NOT EXISTS webserverDB;
    // 初始化线程池的数据库信息
    https[0].initMySQLUsers(mysqlConnectPool);

    // 线程池
    try
    {
        // 使用数据库连接池初始化
        threadPoolHttps = new threadPool<myHttp>(mysqlConnectPool);
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
    }

#pragma region 时钟系统

    assert(socketpair(PF_UNIX, SOCK_STREAM, 0, pipeFd) != -1);
    setNonblock(pipeFd[1]);                // 1是写端
    registerFd(epollfd, pipeFd[0], false); // pipeFd[0]只有管理线程会读取，不需要用oneShot避免错误读取

    setSignal(SIGALRM, signal_transport, false); // 时钟信号
    setSignal(SIGTERM, signal_transport, false); // 中断信号

    alarm(CHECK_TIME); // 开始定时

#pragma endregion

    // 服务器接收http请求
    while (!stop_server)
    {
        int num = epoll_wait(epollfd, myEvents, MAX_EVENT_NUM, -1);
        // 检查 errno 是否不等于 EINTR。EINTR 表示系统调用被信号中断，这是正常的情况，不应该视为错误。
        // 如果 epoll_wait 返回负值且 errno 不是 EINTR，这意味着发生了其他类型的错误，如资源不足、无效的 epoll 文件描述符等
        // 这时应该跳出循环或采取其他错误处理措施
        if (num < 0 && errno != EINTR)
        {
            printf("num: %d\n", num);
            break;
        }

        for (int i = 0; i < num; i++)
        {
            int fdGot = myEvents[i].data.fd;

            // 检测到新的连接
            if (fdGot == serv_sock)
            {

                sockaddr_in clientAddr;
                socklen_t clientAddrLen = sizeof(clientAddr);

                int newFd = accept(serv_sock, (sockaddr *)&clientAddr, &clientAddrLen);
                if (newFd < 0)
                {
                    // 日志，连接失败
                    LOG_ERROR("new connection failed");
                    LOG_FLUSH();
                    continue;
                }
                if (myHttp::userCnt >= MAX_FD || newFd >= MAX_FD)
                {
                    error_send(newFd, "server busy");
                    // 日志，连接过多
                    LOG_ERROR("refuse new connection , server connections reach upper limit");
                    LOG_FLUSH();
                    continue;
                }
                // 直接端口映射，空间换时间，https[i]的i即socket
                https[newFd].init(newFd, clientAddr);

                // 时钟相关初始化
                {
                    clientDatas[newFd].address = clientAddr;
                    clientDatas[newFd].sockFd = newFd;

                    // 给连接资源以及定时器链表加上定时器
                    OutTimer *tmpTimer = new OutTimer;
                    tmpTimer->clientData = &clientDatas[newFd];
                    tmpTimer->expireFunc = closeUnactiveSocket;
                    time_t currentTime = time(NULL);
                    tmpTimer->expireTime = currentTime + 3 * CHECK_TIME;
                    clientDatas[newFd].timer = tmpTimer;
                    outTimerList.add_timer(tmpTimer);
                }
            }
            // 处理异常
            else if (myEvents[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 关闭连接，移除对应定时器
                OutTimer *tmpTimer = clientDatas[fdGot].timer;
                tmpTimer->expireFunc(&clientDatas[fdGot]);

                if (tmpTimer)
                {
                    outTimerList.del_timer(tmpTimer);
                }
                continue;
            }
            // 如果是时钟到时间了，处理定时信号
            else if ((fdGot == pipeFd[0]) && (myEvents[i].events & EPOLLIN))
            {
                int sigNum;
                char signalStr[1024];

                // 从管道读出信号值
                ret = recv(pipeFd[0], signalStr, sizeof(signalStr), 0);
                if (ret == -1)
                {
                    // 处理错误?
                    continue;
                }
                else if (ret == 0)
                {
                    // 没有东西
                    continue;
                }
                else
                {
                    for (int j = 0; j < ret; j++)
                    {
                        switch (signalStr[j])
                        {
                        case SIGALRM:
                        {
                            checkTimeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                            break;
                        }
                        }
                    }
                }
            }
            // 可读事件
            else if (myEvents[i].events & EPOLLIN)
            {

                // 取出当前连接对应的定时器
                OutTimer *tmpTimer = clientDatas[fdGot].timer;
                if (https[fdGot].read_all()) // 成功读完读缓冲区(读到对应的myHttp类中)
                {
                    // 将任务添加入线程池
                    threadPoolHttps->requireAdd(&https[fdGot]);
                    // 更新时钟
                    if (tmpTimer)
                    {
                        time_t currentTime = time(NULL);
                        tmpTimer->expireTime = currentTime + 3 * CHECK_TIME;
                        outTimerList.adjust_timer(tmpTimer);
                    }
                }
                else
                {
                    // 关闭连接
                    tmpTimer->expireFunc(&clientDatas[fdGot]);
                    if (tmpTimer)
                        outTimerList.del_timer(tmpTimer);
                }
            }
            // 可写事件
            else if (myEvents[i].events & EPOLLOUT)
            {
                OutTimer *tmpTimer = clientDatas[fdGot].timer;
                if (https[fdGot].send_response()) // 响应报文未出错（或连接是非保留，即linger==false
                {
                    if (tmpTimer) // 更新定时器
                    {
                        time_t currentTime = time(NULL);
                        tmpTimer->expireTime = currentTime + 3 * CHECK_TIME;
                        outTimerList.adjust_timer(tmpTimer);
                    }
                }
                else
                {
                    tmpTimer->expireFunc(&clientDatas[fdGot]);
                    if (tmpTimer)
                    {
                        outTimerList.del_timer(tmpTimer);
                    }
                }
            }

            if (checkTimeout)
            {
                timer_update();
                checkTimeout = false;
            }
        }
    }
}
