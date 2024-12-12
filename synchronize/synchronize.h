//用于线程同步，保护临界资源的库封装
#ifndef SYNCHRONIZE_H_
#define SYNCHRONIZE_H_
#include <exception>
#include <pthread.h>
#include <semaphore.h>

//互斥锁封装类
class mutx
{   
    public:
        mutx();//init初始化
        ~mutx();//释放锁

        bool lock();//上锁
        bool unlock();//解锁
        pthread_mutex_t* get_mutex_p();//获取互斥锁地址
    private:
        pthread_mutex_t mtx;//互斥锁
};

//无名信号量封装
class sem
{
    public:
    sem();//初始化，默认值为1
    sem(int cnt);//初始化，设置数值
    ~sem();//释放

    bool wait();//V操作
    bool post();//P操作

    private:

    sem_t sm;
};

//条件变量封装
class cond
{
    public:
    cond();
    ~cond();
    bool wait(pthread_mutex_t* p_mtx);
    bool timewait(pthread_mutex_t* p_mtx,timespec t);
    bool signal();
    bool broadcast();
    
    private:
    pthread_cond_t cd;
};

#endif