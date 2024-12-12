#ifndef MYHTTP_H_
#define MYHTTP_H_

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <map>

#include <mysql/mysql.h>
#include <pthread.h>

#include "../myLog/myLog.h"
#include "../mysql/mysqlPool.h"

using namespace std;

// 是http消息的处理单元 接收消息->触发epoll事件->取一个http单元读取接收的消息->http单元作一个Task任务，加入任务队列->线程池自动获取任务队列，进行处理
class myHttp
{
public:
#pragma region 一些全局设置
    // 读取文件的名称m_real_file大小
    static const int FILENAME_LEN = 200;
    // 读缓冲区大小
    static const int READ_BUF_SIZE = 2048;
    // 写缓冲区大小
    static const int WRITE_BUF_SIZE = 1024;
    // 请求报文请求方式
    enum METHOD
    {
        GET,
        POST
    };

    // 报文解析结果类型
    enum HTTP_CODE
    {
        NO_REQUEST,        // 请求不完整
        GET_REQUEST,       // 完整的请求
        FILE_REQUEST,      // 请求的文件资源可以正常访问
        BAD_REQUEST,       // 请求有语法错误
        NO_RESOURCE,       // 请求资源不存在
        FORBIDDEN_REQUEST, // 请求资源禁止访问，没有读取权限
        INTERNAL_ERROR,    // 服务器错误
        CLOSED_CONNECTION
    };

    // 有限状态机，主状态机
    enum CHECK_STATUS
    {
        REQUESTLINE, // 请求行
        HEADER,      // 请求头部
        CONTENT      // 消息体
    };
    // 从状态机状态
    enum LINE_STATUS
    {
        LINE_OK,  // 完整接收一行
        LINE_BAD, // 行格式错误
        LINE_OPEN // 接收不完整
    };

#pragma endregion

    // 构造、析构
    myHttp() {};
    ~myHttp() {};

#pragma region 函数

    // 初始化套接字
    void init(int sockfd, const sockaddr_in &addr);

    // 初始化map users，用于在线验证登录
    void initMySQLUsers(mysqlPool * mysqlConnectPool);

    // 重置
    void reset();

    // 关闭http连接
    void closeConnection();

    // 运行，由epoll事件触发，交由线程池调用，也就是作为具体的线程函数
    // (包括接收HTTP请求消息、解析请求消息、获取资源、得到响应消息，发出响应消息
    void process();
#pragma region 解析HTTP相关
    // 解析readRequest的总函数，返回报文解析结果
    HTTP_CODE process_read();

    // 验证请求消息行的完整有效性
    LINE_STATUS parse_line();

    // 解析http请求行，获得请求方法、url、http版本号
    HTTP_CODE decode_request_line(char *text);

    // 解析http消息头部，且调用一次解析出一个头部的变量
    HTTP_CODE decode_headers(char *text);

    // 解析http消息体
    HTTP_CODE decode_content(char *text);

    // 检查HTTP请求信息，为响应报文做准备
    HTTP_CODE do_request();

#pragma endregion

#pragma region 写入响应报文相关
    // 生成响应报文
    bool process_write(HTTP_CODE httpCode);

    // 给其他写入响应报文的函数调用，格式化写入响应报文
    bool add_response(const char *format, ...);

    // 写入状态行
    bool write_status_line(int status, const char *title);

    // 写入消息报头
    bool write_headers(int contentLen_);

    // 在parse_line验证完完整性之后，
    char *get_line();

    // 读取从浏览器发来的全部数据
    bool read_all();

    // 写入响应报文
    bool write_response();

    // 写入Content-length字段
    bool write_content_length(int contentLen_);

    // 写入文本类型字段
    bool write_content_type();

    // 写入连接状态，通知浏览器保持连接或关闭
    bool write_linger();

    // 写入空行
    bool write_blank_line();

    // 写入content
    bool write_content(const char *content);

    // 取消文件内存映射
    void unmap();

#pragma endregion

    // 由主线程检测写事件触发，将准备好的响应报文发送到客户端
    bool send_response();
    // 获取地址
    sockaddr_in *get_address();

#pragma endregion

#pragma region 成员变量
    // 公有成员
    static int myEpollfd;
    static int userCnt;
    MYSQL *mysqlConnection; // 数据库连接

private:
    int mySockfd;
    sockaddr_in myAddress;

    // 读请求消息
    char readRequest[READ_BUF_SIZE];
    int readIndex;       // 读得readRequest的末尾位置之后一位
    int readCheckIndex;  // 当前已经解析readRequest的部分末尾的位置
    int readAnalyzedCnt; // 已解析字符个数
    int startLine;       // 行当前的起始索引

    // 响应消息
    char writeResponse[WRITE_BUF_SIZE];
    int writeIndex; // 写入writeResponse的末尾位置之后一位

    // 状态
    CHECK_STATUS checkStatus;
    METHOD method;

    // 解析请求报文中对应的6个变量
    char fileName[FILENAME_LEN];
    char *url;
    char *version;
    char *host;
    int contentLen;
    bool linger; // 是否保持连接
    int cgi;     // 是否启用POST

    char *fileAddress;    // 读取服务器上的文件地址内存映射
    struct stat fileStat; // 文件信息
    struct iovec ivs[2];  // 用于描述多个缓冲区，使得单个系统调用能够读写多个缓冲区
    int ivCnt;            // iovec缓冲区有效数量
    char *postStr;        // post指令的字符串

    int bytesSent;
    int bytesToSend;

#pragma endregion
};

#pragma region 资源访问规则
// '/'
//  GET请求，跳转到 welcome.html 欢迎界面

// '/register.html'
// POST请求，跳转到 register.html 注册界面

// '/login.html'
// POST请求，跳转到 login.html 登录界面

// 'submit_register'
// POST请求，注册

// 'submit_login'
// POST请求，登录

#pragma endregion

#endif