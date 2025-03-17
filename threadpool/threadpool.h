#ifndef THREADPOOL_H_
#define THREADPOOL_H_

// 因为是模板类，所以相应的模板函数，实现必须写在头文件

#include <deque>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../synchronize/synchronize.h"
#include "../mysql/mysqlPool.h"

using namespace std;

// 线程池类，任务是池里的工作线程提取任务列表并执行读写操作，需要读数据库，所以要用到数据库连接池
template <typename Require>
class threadPool
{
public:
    // 构造、析构
    threadPool(mysqlPool *mysql_pool_, int max_thread_cnt_ = 10, int max_require_cnt_ = 10000);
    ~threadPool();

    // 添加请求
    bool requireAdd(Require *require);

private:
    // 内部函数
    // work_func用于创建工作线程，启动run()，由run()完成“提取任务列表并执行”这个任务
    // 也就是说其实run()才是真正的工作线程函数
    // 为什么不直接把work_func作为工作函数的全部呢?
    // 因为pthread_create不能使用类的成员函数，而静态函数不方便访问实例的成员变量
    static void *work_func(void *arg);
    void run();

private:
    // 成员变量
    // 线程池相关
    pthread_t *pool;    // 线程池
    int max_thread_cnt; // 最大线程数
    int thread_front;
    int thread_rear;

    // 请求队列相关
    deque<Require *> requires_pool; // 请求队列
    int max_require_cnt;

    // 同步
    mutx pool_mtx;           // 线程池
    mutx requires_mtx;       // 请求列表
    sem requireNotEmpty_sem; // 还未处理的请求信号量
    sem requireNotFull_sem;  // 还能添加请求的信号（请求未满
    bool stop;               // 线程池是否停止，工作线程只看是否true，可不加锁

    // 数据库连接池
    mysqlPool *mysql_pool;
};

#pragma region 定义

template <typename Require>
threadPool<Require>::threadPool(mysqlPool *mysql_pool_, int max_thread_cnt_, int max_require_cnt_)
    : mysql_pool(mysql_pool_), max_thread_cnt(max_thread_cnt_), max_require_cnt(max_require_cnt_)
{
    if (max_thread_cnt <= 0 || max_require_cnt <= 0)
        throw std::exception();

    requireNotFull_sem = sem(max_require_cnt);
    requireNotEmpty_sem = sem(0); // 其实默认初始化为0

    // 初始化线程池
    pool = new pthread_t[max_thread_cnt];

    for (int i = 0; i < max_thread_cnt; i++)
    {
        // 创建线程
        if (pthread_create(&pool[i], NULL, work_func, this) != 0)
        {
            delete[] pool;
            throw std::exception();
        }

        // 线程分离，省的自己回收，线程需要根据线程池对象的情况自行回收
        if (pthread_detach(pool[i]) != 0)
        {
            delete[] pool;
            throw std::exception();
        }
    }
}

template <typename Require>
threadPool<Require>::~threadPool()
{
    if (pool)
        delete[] pool;
}

template <typename Require>
bool threadPool<Require>::requireAdd(Require *require)
{
    requireNotFull_sem.wait();
    requires_mtx.lock();

    requires_pool.push_back(require);

    requireNotEmpty_sem.post();
    requires_mtx.unlock();

    return true;
}

template <typename Require>
void *threadPool<Require>::work_func(void *arg)
{
    threadPool<Require> *thread_pool = (threadPool<Require> *)arg;
    thread_pool->run();
    return thread_pool;
}

template <typename Require>
void threadPool<Require>::run()
{
    while (!stop)
    {
        // 取出任务
        requireNotEmpty_sem.wait();
        requires_mtx.lock();

        Require *require = requires_pool.front();
        requires_pool.pop_front();

        requireNotFull_sem.post();
        requires_mtx.unlock();

        // 执行任务
        if (!require)
            continue;

        // 用我们定义好的Require类

        mysqlPoolRAII mysqlConnectionGet(&require->mysqlConnection, mysql_pool); // 给require类配置好数据库连接
        require->process();                                                      // require类自行处理任务
    }
}
#pragma endregion

#endif