// API学习参考文章:
// https://blog.csdn.net/JMW1407/article/details/107612185

#ifndef MYSQLPOOL_H_
#define MYSQLPOOL_H_

#include "../synchronize/synchronize.h"
#include <deque>
#include <string>
#include <mysql/mysql.h>
#include <stdio.h>

using namespace std;

// 数据库连接池
// 用作单例，初始化以及释放资源都用自定义的成员函数
class mysqlPool
{
public:
    // 初始化
    // 因为这个函数是在服务器开启服务之前就应该设置好的，此处不需要互斥锁保护临界资源
    bool init(int maxConnect_, string ip_, unsigned int port_, string user_, string passwd_, string db_, int log_on_ = 0);
    void destroyPool();                      // 销毁释放
    bool returnConnection(MYSQL *returnSQL); // 调用者归还连接
    int getFreeCnt();                        // 获取闲置连接数
    MYSQL *getConnection();                  // 给调用者一个可用的连接

    static mysqlPool *GetInstance(); // 单例模式，用于获取唯一实例，静态函数大写开头一下，更清晰
private:
    mysqlPool(); // 单例模式下，构造函数禁止外界调用
    ~mysqlPool();
    // 连接池
    deque<MYSQL *> sqlDeque; // 连接池队列
    int maxConnect;          // 最大连接数
    int useConnect;          // 正在被使用的连接数
    int freeConnect;         // 闲置的连接数

    mutx pool_mtx;        // 保护数据库连接池
    sem poolNotEmpty_sem; // 连接池没有被用完的信号量，用于等待有闲置的连接给getConnection()返回
    sem poolNotFull_sem;  // 我认为用这个信号量确保连接数不超过maxConnect是比较规范的，表示调用者能归还给连接池的MYSQL连接个数

    // 主机信息
    string ip;         // 地址
    unsigned int port; // 端口号
    string user;       // 用户名
    string passwd;     // 密码
    string db;         // 数据库名

    // 日志
    int log_on;
};

// RAII（Resource Acquisition Is Initialization）资源获取即初始化（构造），资源释放即析构
// 原理：
// 下面这个类简称A
// A类的构造函数就是获取资源的函数，析构函数就是释放资源的函数
// 调用者想要获取连接时，创建一个对象(malloc或new),结束连接时，释放对象(free或delete)
class mysqlPoolRAII
{
public:
    mysqlPoolRAII(MYSQL **sql_p, mysqlPool *pool_); // 从数据库连接池pool中获取一个MYSQL*指针的指针
    ~mysqlPoolRAII();

private:
    MYSQL *sql;
    mysqlPool *pool;
};
#endif