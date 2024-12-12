#include "mysqlPool.h"

#include <map>
//用于控制数据库连接，有关用户的用户名，密码
map<string,string> usersInfo;
mutx usersInfo_mtx;

mysqlPool::mysqlPool()
{
    maxConnect = 0;
    useConnect = 0;
    freeConnect = 0;
    log_on = 0;
}

mysqlPool::~mysqlPool()
{
    destroyPool();
}

bool mysqlPool::init(int maxConnect_, string ip_, unsigned int port_, string user_, string passwd_, string db_, int log_on_)
{
    // 初始化
    maxConnect = maxConnect_;
    ip = ip_;
    port = port_;
    user = user_;
    passwd = passwd_;
    db = db_;
    log_on = log_on_;

    // 创建链接池
    sqlDeque.clear();
    for (int i = 0; i < maxConnect; i++)
    {
        MYSQL *sqlConnect = NULL;
        sqlConnect = mysql_init(sqlConnect);

        if (sqlConnect == NULL)
            return false;

        // 连接，data()函数转char*
        sqlConnect = mysql_real_connect(sqlConnect, ip.c_str(), user.c_str(), passwd.c_str(), db.c_str(), port, NULL, 0);

        if (sqlConnect == NULL)
            return false;

        sqlDeque.push_back(sqlConnect);
        freeConnect++;
    }

    // 连接池相关变量
    useConnect = 0;
    freeConnect = maxConnect;
    poolNotEmpty_sem = sem(maxConnect);
    poolNotFull_sem = sem(0); // 已满
}

// 释放连接池资源
void mysqlPool::destroyPool()
{
    pool_mtx.lock();

    while (!sqlDeque.empty())
    {
        MYSQL *s = sqlDeque.front();
        mysql_close(s);
        sqlDeque.pop_front();
    }
    useConnect = 0;
    freeConnect = 0;

    pool_mtx.unlock();
}

// 获取连接（消费者函数
MYSQL *mysqlPool::getConnection()
{
    // 有信号量的保护，我认为可以不使用empty()确保队列非空
    // 这一步wait要在lock之前，不然会死锁，这是一个典型的生产者-消费者模型
    // 信号量先等，等到了再锁，其实是很符合逻辑的
    poolNotEmpty_sem.wait();
    pool_mtx.lock();

    // 取走一个连接
    MYSQL *ret = sqlDeque.front();
    sqlDeque.pop_front();
    freeConnect--;
    useConnect++;

    poolNotFull_sem.post();
    pool_mtx.unlock();

    return ret;
}

// 归还连接（生产者函数
bool mysqlPool::returnConnection(MYSQL *returnSQL)
{
    if (returnSQL == NULL)
    {
        return false;
    }
    // 同理
    poolNotFull_sem.wait();
    pool_mtx.lock();

    sqlDeque.push_back(returnSQL);
    freeConnect++;
    useConnect--;

    poolNotEmpty_sem.post();
    pool_mtx.unlock();

    return true;
}

// 获取当前闲置连接数
int mysqlPool::getFreeCnt()
{
    int res;
    pool_mtx.lock();
    res = freeConnect;
    pool_mtx.unlock();
    return res;
}

///////////////////////////////////////////////////////

// 单例模式
mysqlPool *mysqlPool::GetInstance()
{
    static mysqlPool pool;
    return &pool;
}

mysqlPoolRAII::mysqlPoolRAII(MYSQL **sql_p, mysqlPool *pool_)
{
    // 资源获取
    *sql_p = pool_->getConnection();
    // 初始化
    sql = *sql_p;
    pool = pool_;
}

mysqlPoolRAII::~mysqlPoolRAII()
{
    pool->returnConnection(sql);
}