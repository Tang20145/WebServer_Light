#include "myHttp.h"
#include <stdlib.h>
#include <stdio.h>

// 初始化
int myHttp::myEpollfd = -1;
int myHttp::userCnt = 0;

const char *doc_root = "/root/projects/WebServer/root";

mutx userCnt_mtx; // 保护用户总数
mutx users_mtx;   // 保护数据users
map<string, string> users;

#pragma region http响应状态消息

const char *http_title_ok_200 = "OK";
const char *http_title_error_400 = "Bad Request"; // 语法错误400
const char *http_title_error_403 = "Forbidden";
const char *http_title_error_404 = "Not Found";
const char *http_title_error_500 = "Internal Error";
const char *http_400_content = "Your request has bad syntax or is inherently impossible to staisfy.\n"; // 错误详情，会写到消息体里
const char *http_403_content = "You do not have permission to get file form this server.\n";
const char *http_404_content = "The requested file was not found on this server.\n";
const char *http_500_content = "There was an unusual problem serving the request file.\n";
const char *http_200_blank_content = "<html><body></body></html>";

#pragma endregion

#pragma region epoll操作函数，不需要其他源文件调用，就不用在头文件中声明了

// 设置套接字nonblock
int setNonblock(int fd)
{
    int lastOp = fcntl(fd, F_GETFL);
    int newOp = lastOp | O_NONBLOCK;
    fcntl(fd, F_SETFL, newOp);
    return lastOp;
}

// 向epoll描述符注册事件(自动设置非阻塞)
void registerFd(int epollfd, int fd, bool oneShot)
{
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLHUP;
    if (oneShot)
        ev.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    setNonblock(fd);
}

// 重置oneShot以让文件描述符触发并得到处理之后再重新开始监听
// oneShot可以防止同一个socket被多个http单元读取，导致读取错误
void restartFdOneshot(int epollfd, int fd, int op)
{
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = op | EPOLLONESHOT | EPOLLHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev);
}

// 移除epollfd中的文件描述符
void rmFd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

#pragma endregion

//////////////////////////////////////以下为myHttp类成员函数

// 初始化，代表新的客户端连接，将地址以及套接字保存，同时重置其他变量
void myHttp::init(int sockfd, const sockaddr_in &addr)
{
    reset();
    // 监听者的文件描述符加入epoll
    mySockfd = sockfd;
    myAddress = addr;

    int x = 1;
    setsockopt(mySockfd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
    registerFd(myEpollfd, mySockfd, true);
    setNonblock(sockfd);

    userCnt_mtx.lock();
    userCnt++; // 新http连接，连接数增加
    userCnt_mtx.unlock();
}

// 初始化map结构
void myHttp::initMySQLUsers(mysqlPool *mysqlConnectPool)
{
    MYSQL *sql = NULL;
    mysqlPoolRAII tmpSqlConnection(&sql, mysqlConnectPool); // 初始化即获取资源，函数结束后，作为局部变量失效后释放资

    if (mysql_query(sql, "SELECT `username`,`passwd` FROM users"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(sql));
    }

    // 语句执行结果
    MYSQL_RES *result = mysql_store_result(sql);

    if (result == NULL)
        return;

    // 列数
    int dataCnt = mysql_num_fields(result);

    // 求列信息
    MYSQL_FIELD *dataFields = mysql_fetch_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string username = row[0];
        string passwd = row[1];
        users[username] = passwd;
    }
}

// 重置一些其他变量
void myHttp::reset()
{
    mysqlConnection = NULL;
    memset(readRequest, 0, sizeof(readRequest));
    memset(writeResponse, 0, sizeof(writeResponse));
    memset(fileName, 0, sizeof(fileName));

    readIndex = 0;
    readCheckIndex = 0;
    readAnalyzedCnt = 0;
    startLine = 0;

    checkStatus = REQUESTLINE;
    method = GET;

    writeIndex = 0;
    url = NULL;
    version = NULL;
    host = NULL;
    contentLen = 0;
    linger = false;
    cgi = 0;

    fileAddress = NULL;
    ivCnt = 0;
    postStr = NULL;

    bytesSent = 0;
    bytesToSend = 0;
}

// 自我关闭，代表这个用户断开连接了
void myHttp::closeConnection()
{
    if (mySockfd != -1)
    {

        rmFd(myEpollfd, mySockfd);
        reset();

        // 减小静态总连接数
        userCnt_mtx.lock();
        userCnt--;
        userCnt_mtx.unlock();

        mySockfd = -1;
    }
}

// http单元受事件触发运行总函数
void myHttp::process()
{

    HTTP_CODE readRet = process_read(); // 完成HTTP消息的接收、解析，返回解析结果

    if (readRet == NO_REQUEST) // 如果请求不完整，重新读
    {
        // 重新设置回epoll去监听没接收完的部分，process_read函数会继续之前已经读取的部分继续读取、解析
        restartFdOneshot(myEpollfd, mySockfd, EPOLLIN);
        return;
    }

    bool writeRet = process_write(readRet); // 准备好响应报文
    if (!writeRet)
    {
        closeConnection(); // 关闭连接自动重置一系列变量
    }
    restartFdOneshot(myEpollfd, mySockfd, EPOLLOUT); // 等待可写事件触发
}

#pragma region 请求报文相关
// 循环读取客户数据，直到无数据可读或对方关闭连接
bool myHttp::read_all()
{
    if (readIndex >= READ_BUF_SIZE)
        return false;
    int readBytes = 0;
    while (1)
    {
        readBytes = recv(mySockfd, readRequest + readIndex, READ_BUF_SIZE - readIndex, 0);
        if (readBytes == -1)
        {
            // 非阻塞边缘触发下需要一次性读完
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        else if (readBytes == 0)
        {
            return false;
        }
        readIndex += readBytes;
    }

    return true;
}

// 验证请求消息行的完整有效性
// 同时将行末尾格式更改为\0\0以便解析函数进行消息解析，更新readCheckIndex到已经验证过的位置的后一位
myHttp::LINE_STATUS myHttp::parse_line()
{
    char temp;

    for (; readCheckIndex < readIndex; readCheckIndex++)
    {
        temp = readRequest[readCheckIndex];
        // 有可能接收到完整行
        if (temp == '\r')
        {
            if ((readCheckIndex + 1) == readIndex) // 如果直接结尾而没有'\n'，说明接收不完整，需要继续接收
            {
                return LINE_OPEN;
            }
            else if (readRequest[readCheckIndex + 1] == '\n') // 完整行
            {
                readRequest[readCheckIndex++] = '\0';
                readRequest[readCheckIndex++] = '\0';
                return LINE_OK;
            }
            // 不符合则语法错误
            return LINE_BAD;
        }

        // 如果是没读取完，重新读取到此字符，说明有可能是完整行（如何它前面是'\r'
        else if (temp == '\n')
        {
            if (readCheckIndex > 1 && readRequest[readCheckIndex - 1] == '\r')
            {
                readRequest[readCheckIndex - 1] = '\0';
                readRequest[readCheckIndex++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 没接收完整行
    return LINE_OPEN;
}

// 解析请求行（请求头部、请求消息体）之前，parse_line函数已经将每一行的末尾符号\r\n重新赋值为\0\0
// 处理请求行
myHttp::HTTP_CODE myHttp::decode_request_line(char *text)
{
    LOG_INFO("%d %s", mySockfd, text);
    LOG_FLUSH();

    char *tmp;
    // 请求行中的各个部分是用' '或'\t'分隔的
    // char *strpbrk(const char *s1, const char *s2);
    // s1 中搜索任何出现在 s2中的字符，返回第一次出现该字符位置的指针，没有则返回NULL
    tmp = strpbrk(text, " \t");
    if (!tmp)
    {
        return BAD_REQUEST;
    }
    // 修改分隔符，移位，用于获取下一个部分，下方同理
    *tmp = '\0';
    tmp++;

    // 而此时的text在第一个'\0'之前就是请求方式的部分了
    char *methodStr = text;
    // strcasecmp是不考虑大小写的字符串比较
    if (strcasecmp(methodStr, "GET") == 0)
        method = GET;
    else if (strcasecmp(methodStr, "POST") == 0)
    {
        method = POST;
        cgi = 1;
    }
    else // 只使用GET POST，其他归为语法错误
        return BAD_REQUEST;

    // size_t strspn(const char *str1, const char *str2)
    // strspn是找到字符串str1中第一个不存在于str2中的字符的索引
    // tmp跳过了第一个分隔符，不确定后面有无，继续移动到没有为止
    tmp += strspn(tmp, " \t");
    url = tmp; // 得到url

    // 同理找到版本号
    tmp = strpbrk(tmp, " \t");
    if (!tmp)
        return BAD_REQUEST;

    *tmp = '\0';
    tmp++;
    tmp += strspn(tmp, " \t");

    version = tmp;

    if (!((strcasecmp(version, "HTTP/1.1") == 0) || (strcasecmp(version, "HTTP/1.0") == 0)))
    {
        return BAD_REQUEST;
    }
    // 请求资源特殊情况的修正
    if (strncasecmp(url, "http://", 7) == 0) // 此函数比价前n个字符，不考虑大小写
    {
        url += 7;
        url = strchr(url, '/'); // 此函数获取url第一个'/'字符的指针
    }
    if (strncasecmp(url, "https://", 8) == 0)
    {
        url += 8;
        url = strchr(url, '/');
    }
    // 没有访问资源，语法错误
    if (!url || url[0] != '/')
        return BAD_REQUEST;

    checkStatus = HEADER; // 改变状态机状态，进入下一阶段，解析请求头部
    return NO_REQUEST;    // 只解析完请求行，没有解析完
}

// 解析请求消息头部，主要分析connection字段，content-length字段，其他字段可以直接跳过
// 本函数还有另一个任务，就是解析完请求头部之后，再次作为解析消息体函数的调用者，
myHttp::HTTP_CODE myHttp::decode_headers(char *text)
{
    // 判断空行还是请求头（如果是空行，\r\n应该已经被parse_line设置为\0\0，所以text[0]='\0'表示如果遇到空行，就说明请求头部已经被完全解析过了，现在是在解析消息体或者解析结尾阶段
    if (text[0] == '\0')
    {
        // 日志
        LOG_INFO("%d %s", mySockfd, text);
        LOG_FLUSH();

        if (contentLen != 0) // contentLen应该会在上次本函数作为解析请求头部函数被调用的过程中已经从请求头部中获得
        {
            checkStatus = CONTENT; // 有消息长度，则是POST，需要进行消息体处理
            // 就此，readCheckIndex应该是空行开头，但是返回NO_REQUEST会让process_read解析HTTP消息的状态机下一次循环parse_line使得readCheckIndex到达空行结尾的下一位，即消息体的首部
            return NO_REQUEST; // 本质上还是没读完，所以NO_REQUEST
        }
        // 如果没有contentLen，那么就一定是GET请求方式了，也标志着HTTP消息解析的结束
        // 就此readCheckIndex应该是空行的开头
        return GET_REQUEST;
    }

    // 接下来就是各种头部字段的解析了
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        // 日志
        LOG_INFO("%d %s", mySockfd, text);
        LOG_FLUSH();

        text += 11;
        // 跳过分隔符，下方同理
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            // 长连接
            linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", strlen("Content-length:")) == 0)
    {
        // 日志
        LOG_INFO("%d %s", mySockfd, text);
        LOG_FLUSH();

        text += 15;
        text += strspn(text, " \t");
        contentLen = atol(text); // 将字符串转换为长整数
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        // 日志
        LOG_INFO("%d %s", mySockfd, text);
        LOG_FLUSH();

        text += 5;
        text += strspn(text, " \t");
        host = text;
    }
    else
    {
        LOG_INFO("%d Unkown header:%s", mySockfd, text);
        LOG_FLUSH();
    }
    return NO_REQUEST; // 没有解析完，要循环解析直到遇到空行为止（无论是GET还是POST，请求头部之后都会是空行
}

// 处理消息体
myHttp::HTTP_CODE myHttp::decode_content(char *text)
{
    // 查看是否读完
    // 在HTTP协议中，消息体的结尾并没有一个特殊的分隔符，如"\0"来标记消息体的结束
    // 如果HTTP消息已经读完，此处readIndex是已经读完的消息的末尾的后一位索引，即HTTP请求消息长度
    if (readIndex >= (readCheckIndex + contentLen))
    {
        text[contentLen] = '\0'; // 结尾赋值分隔符

        // POST的消息体内容
        postStr = text;

        // 消息体日志
        LOG_INFO("%d HTTP content:%s", mySockfd, text);
        LOG_FLUSH();

        return GET_REQUEST;
    }
    return NO_REQUEST; // 没读完，退出循环，重新read_all
}

// 解析readRequest消息的总函数
myHttp::HTTP_CODE myHttp::process_read()
{
    LINE_STATUS lineStatus = LINE_OK; // 初始化验证行的状态机状态
    HTTP_CODE ret = NO_REQUEST;       // 初始化HTTP请求解析结果状态
    char *text = NULL;

    // 循环条件自动验证消息行的完整有效性
    // 但是验证消息行的完整有效性之前先判断是否已经读到请求消息的消息体
    // 注意此处或判断左右条件不能互换，因为当checkStatus == CONTENT，startLine已经设置好了
    // 而HTTP请求消息的消息体末尾是没有\r\n的，parse_line会使得lineStatus==LINE_OPEN导致无法读取消息体退出循环
    // 所以，HTTP请求消息POST的消息体的完整性只需要在decode_content函数内验证即可
    while ((checkStatus == CONTENT && lineStatus == LINE_OK) || (lineStatus = parse_line()) == LINE_OK)
    {
        text = get_line();
        startLine = readCheckIndex;
        // readCheckedIndex在parse_line函数中已经被定位到已经验证完整有效性的行的行末的下一位
        // 此处startLine是给下一次get_line使用的索引

        // HTTP消息解析状态机，状态指示当前解析HTTP的部分是请求行、请求头部、消息体中的一个
        switch (checkStatus)
        {
        case REQUESTLINE:
        {
            // 解析请求行
            ret = decode_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case HEADER:
        {
            // 解析请求头部
            ret = decode_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CONTENT:
        {
            // 解析消息体
            ret = decode_content(text);

            if (ret == GET_REQUEST)
            {
                return do_request();
            }

            lineStatus = LINE_OPEN; // 退出循环

            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }
    return NO_REQUEST; // 行没有读取完，即没读完
}

// 给解析HTTP消息总函数调用，表示得到当前需要解析的行地址
char *myHttp::get_line()
{
    return readRequest + startLine;
}

// 检查HTTP请求信息，为响应报文做准备
myHttp::HTTP_CODE myHttp::do_request()
{
    // 初始化为根目录
    strcpy(fileName, doc_root);
    int len = strlen(fileName);

    const char *p = strchr(url, '/'); // strchr()查找字符串中的一个字符，并返回该字符在字符串中第一次出现的位置

    // 登录或注册的提交
    // 本质上就是接收的url表示处理逻辑而不表示对应文件，在这个分支根据处理逻辑结果选择对应的url
    if (cgi == 1 && ((strcmp(url, "/submit_register") == 0) || (strcmp(url, "/submit_login") == 0)))
    {

        char flag = *(strchr(url, '_') + 1); // r是注册，l是登录
        // 将用户名和密码提取出来
        char username[100];
        char passwd[100];

        {
            int i;
            char *x;

            x = strchr(postStr, '=');
            x++;
            i = 0;
            while (*x != '&')
            {
                username[i++] = *x;
                x++;
            }
            username[i] = '\0';

            x = strrchr(x, '=');
            x++;
            i = 0;
            while (*x != '\0')
            {
                passwd[i++] = *x;
                x++;
            }
            passwd[i] = '\0';
        }

        LOG_DEBUG("get username:%s,passwd:%s", username, passwd);
        LOG_FLUSH();

        // 注册
        if (flag == 'r')
        {
            // 对应数据表创建
            // CREATE TABLE IF NOT EXISTS users
            // (
            // username CHAR ( 50 ),
            // passwd CHAR ( 50 )
            // )
            // ENGINE = INNODB;
            char *mysqlInsert = new char[200];
            // INSERT语句创建
            snprintf(mysqlInsert, 200, "INSERT INTO users(`username`,`passwd`) VALUES('%s','%s')", username, passwd);
            if (users.find(username) == users.end()) // 不存在这个用户名
            {
                // 向数据库插入新的用户信息
                users_mtx.lock();
                int ret = mysql_query(mysqlConnection, mysqlInsert);
                users_mtx.unlock();

                if (ret != 0) // 插入数据库失败
                {
                    // 在这个if分支中url原长度至少为 strlen("/submit_login") = 13
                    // 我不想破坏 version指针的内容，所以此处用的url最长取13
                    strcpy(url, (char *)"/regFail.html");
                    LOG_DEBUG("insert DB failed");
                    LOG_FLUSH();
                }
                else // 如果数据库已经成功插入新的用户，那么可以插入map（如果此时别的用户注册了相同用户名，会先在插入数据的步骤中失败而不会选择插入map
                {
                    users_mtx.lock();
                    users.insert(pair<string, string>(username, passwd));
                    users_mtx.unlock();
                    strcpy(url, "/login.html");
                }
            }
            else
            {
                strcpy(url, (char *)"/regFail.html");
            }

            delete[] mysqlInsert;
        }

        // 如果是登录
        else if (flag == 'l')
        {
            if (users.find(username) != users.end() && users[username] == passwd)
            {
                strcpy(url, "/main.html");
            }
            else
            {
                strcpy(url, "/logFail.html");
            }
        }
    }
    // 主界面
    if ((strcmp(p, "/") == 0) || strncasecmp(p, "/welcome.html", strlen("/welcome.html")) == 0)
    {
        char *tmpStr = new char[200];

        strcpy(tmpStr, "/welcome.html");
        strncpy(fileName + len, tmpStr, strlen(tmpStr));

        delete[] tmpStr;
    }
    // 选择注册或登录界面
    else if (strncasecmp(p, "/loginChoose.html", strlen("/loginChoose.html")) == 0)
    {
        char *tmpStr = new char[200];

        strcpy(tmpStr, "/loginChoose.html");
        strncpy(fileName + len, tmpStr, strlen(tmpStr));

        delete[] tmpStr;
    }
    // 注册界面
    else if (strncasecmp(p, "/register.html", strlen("/register.html")) == 0)
    {
        char *tmpStr = new char[200];

        strcpy(tmpStr, "/register.html");
        strncpy(fileName + len, tmpStr, strlen(tmpStr));

        delete[] tmpStr;
    }
    // 登录界面
    else if (strncasecmp(p, "/login.html", strlen("/login.html")) == 0)
    {
        char *tmpStr = new char[200];
        strcpy(tmpStr, "/login.html");
        strncpy(fileName + len, tmpStr, strlen(tmpStr));

        delete[] tmpStr;
    }
    else if (strncasecmp(p, "/gallery.html", strlen("/gallery.html")) == 0)
    {
        char *tmpStr = new char[200];
        strcpy(tmpStr, "/gallery.html");
        strncpy(fileName + len, tmpStr, strlen(tmpStr));

        delete[] tmpStr;
    }
    else if (strncasecmp(p, "/support.html", strlen("/support.html")) == 0)
    {
        char *tmpStr = new char[200];
        strcpy(tmpStr, "/support.html");
        strncpy(fileName + len, tmpStr, strlen(tmpStr));

        delete[] tmpStr;
    }

    else // 都不匹配，直接用url（用于发给浏览器ico
    {
        strncpy(fileName + len, url, FILENAME_LEN - len - 1);
    }

    // int stat(const char *pathname, struct stat *statbuf);
    // 成功返回0，将pathname路径指向的文件/文件夹的信息存储在statbuf结构体内
    if (stat(fileName, &fileStat) != 0)
    {
        LOG_INFO("%d do_request NO_RESOURCE", mySockfd);
        LOG_FLUSH();
        return NO_RESOURCE;
    }
    // S_ISDIR()，也是stat头文件的函数，判断文件是否是目录
    // st_mode 结构体stat的成员变量，存储文件类型、权限信息
    if (S_ISDIR(fileStat.st_mode))
    {
        LOG_INFO("%d do_request BAD_REQUEST", mySockfd);
        LOG_FLUSH();
        return BAD_REQUEST;
    }

    int fileFd = open(fileName, O_RDONLY); // 只读方式打开文件
    // #include <sys/mman.h>
    // void* mmap(void* start,size_t length,int prot,int flags,int fd,off_t offset);
    // 将文件映射到内存
    fileAddress = (char *)mmap(0, fileStat.st_size, PROT_READ, MAP_PRIVATE, fileFd, 0);
    close(fileFd);
    return FILE_REQUEST; // 被process_read调用，最终返回到process函数内
}
#pragma endregion

#pragma region 响应报文相关

// 向writeResponse添加"内容"
bool myHttp::add_response(const char *format, ...)
{
    // 写入不能超过容量
    if (writeIndex >= WRITE_BUF_SIZE)
        return false;

    va_list arg_list;

    va_start(arg_list, format);

    // int _vsnprintf(char* str, size_t size, const char* format, va_list ap);
    //
    // char *str [out],把生成的格式化的字符串存放在这里.
    // size_t size [in], str可接受的最大字符数 (非字节数，UNICODE一个字符两个字节),防止产生数组越界.
    // const char *format [in], 指定输出格式的字符串，它决定了需要提供的可变参数的类型、个数和顺序。
    // va_list ap [in], va_list变量

    // 用于将可变参数写入响应报文
    int len = vsnprintf(writeResponse + writeIndex, WRITE_BUF_SIZE - 1 - writeIndex, format, arg_list);
    // 我认为是>，==的情况应该是刚好writeIndex == WRITE_BUF_SIZE - 1，而Write_BUF_SIZE-1没写，刚好作为结束符
    if (len > WRITE_BUF_SIZE - 1 - writeIndex)
    {
        va_end(arg_list);
        return false;
    }

    writeIndex += len;

    va_end(arg_list);

    return true;
}

// 写入状态行
bool myHttp::write_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 写入消息报头
bool myHttp::write_headers(int contentLen_)
{
    write_content_length(contentLen_);
    write_linger();
    write_blank_line();
}

// 写入Content-length字段
bool myHttp::write_content_length(int contentLen_)
{
    return add_response("Content-Length:%d\r\n", contentLen_);
}

// 写入文本类型字段
bool myHttp::write_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 写入连接状态通知
bool myHttp::write_linger()
{
    // 使用三目运算符实时判断是否保持连接
    return add_response("Connection:%s\r\n", (linger == true) ? "keep-alive" : "close");
}

// 写入空行
bool myHttp::write_blank_line()
{
    return add_response("%s", "\r\n");
}

// 写入content
bool myHttp::write_content(const char *content)
{
    return add_response("%s", content);
}

bool myHttp::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    // 500
    case INTERNAL_ERROR:
    {
        write_status_line(500, http_title_error_500); // 状态行
        write_headers(strlen(http_500_content));      // 消息头部
        if (!write_content(http_500_content))         // 消息体
            return false;
        break;
    }
    // 以下同理
    //  404
    case BAD_REQUEST:
    {
        write_status_line(404, http_title_error_404);
        write_headers(strlen(http_404_content));
        if (!write_content(http_404_content))
            return false;
        break;
    }
    // 403
    case FORBIDDEN_REQUEST:
    {
        write_status_line(403, http_title_error_403);
        write_headers(strlen(http_403_content));
        if (!write_content(http_403_content))
            return false;
        break;
    }
    // 200
    case FILE_REQUEST:
    {
        write_status_line(200, http_title_ok_200);
        // 如果请求资源存在
        if (fileStat.st_size != 0)
        {
            write_headers(fileStat.st_size);
            // iovec 结构体用于描述多个缓冲区，使得单个系统调用能够读写多个缓冲区
            // 这可以提高 I/O 效率，因为它减少了系统调用的次数
            // 这里就可以将iovec指向响应报文+所请求的文件，以便一次系统调用将消息和文件发送出去
            ivs[0].iov_base = writeResponse;
            ivs[0].iov_len = writeIndex;

            ivs[1].iov_base = fileAddress;     // 文件映射的内存
            ivs[1].iov_len = fileStat.st_size; // 文件大小

            ivCnt = 2;

            bytesToSend = writeIndex + fileStat.st_size; // 为send_response 进行初始化

            return true;
        }
        else
        {
            // 如果文件大小为0，返回空白页面
            write_headers(strlen(http_200_blank_content));
            if (!write_content(http_200_blank_content))
                return false;
        }
        break;
    }
    default:
        return false;
    }
    // 以上，只有FILE_REQUEST需要两个缓冲区，其他的都已经写在请求报文内了
    ivCnt = 1;
    ivs[0].iov_base = writeResponse;
    ivs[0].iov_len = writeIndex;

    return true;
}

void myHttp::unmap()
{
    if (fileAddress)
    {
        munmap(fileAddress, fileStat.st_size);
        fileAddress = NULL;
    }
}

#pragma endregion

// 将准备好的响应报文发送到客户端
bool myHttp::send_response()
{
    int tmpSentBytes = 0;
    int offset = 0;

    // 如果发送的报文为空
    if (bytesToSend == 0)
    {
        restartFdOneshot(myEpollfd, mySockfd, EPOLLIN);
        reset();
        return true;
    }

    while (1)
    {
        // writev 函数是 Unix 和类 Unix 系统中用于高效 I/O 操作的系统调用
        // 可以从多个缓冲区收集数据并写入文件描述符，或从文件描述符读取数据散布到多个缓冲区
        // 避免多次拷贝数据并多次调用读写函数
        // 配合iovec结构体使用
        tmpSentBytes = writev(mySockfd, ivs, ivCnt); // 响应报文全部发送

        if (tmpSentBytes > 0) // 正常发送
        {
            bytesSent += tmpSentBytes;

            offset = bytesSent - writeIndex; // writeIndex是头部的长度，或者说，不包含file的响应消息的长度
        }
        else if (tmpSentBytes == -1)
        {
            if (errno == EAGAIN) // 无法完成，网络资源暂时不可用或缓冲区已满
            {
                // 重新设置好数据，等待下一次可写事件触发

                if (bytesSent >= ivs[0].iov_len) // 状态行以及头部已经完成
                {
                    // 不用继续发送头部消息
                    ivs[0].iov_len = 0;
                    ivs[1].iov_base = offset + fileAddress;
                    ivs[1].iov_len = bytesToSend;
                }
                else
                {
                    ivs[0].iov_base = writeResponse + bytesSent;
                    ivs[0].iov_len -= bytesSent;
                }

                restartFdOneshot(myEpollfd, mySockfd, EPOLLOUT);
                return true;
            }
            // 其他问题，失败，取消映射
            unmap();
            return false;
        }

        bytesToSend -= tmpSentBytes; // 剩余要发送的数据长度

        if (bytesToSend <= 0) // 如果发送完成
        {
            unmap();
            restartFdOneshot(myEpollfd, mySockfd, EPOLLIN); // 先继续保持监听，套接字移除工作交由timer

            // 完毕发送，重置写、读缓冲区以及相关变量
            {
                // 读
                checkStatus = REQUESTLINE;         // 早已完成HTTP解析，需要重置解析状态
                memset(readRequest, 0, readIndex); // 重置读缓冲区
                readCheckIndex = 0;
                readIndex = 0;
                startLine = 0;
                memset(fileName, 0, sizeof(fileName));

                memset(writeResponse, 0, writeIndex); // 发送完成，清空发送缓冲
                writeIndex = 0;
                bytesSent = 0;
                bytesToSend = 0;
            }
            if (!linger)
            {
                reset();
                return false; // 用来表示发送错误，目的是让连接直接断开，由定时器回调函数帮忙处理
            }
            return true;
        }
    }
}