#include "timer.h"

OutTimer::OutTimer() : pre(NULL), next(NULL) {}

OutTimerList::OutTimerList() : head(NULL), tail(NULL) {}

OutTimerList::~OutTimerList()
{
    OutTimer *tmpTimer = head;
    while (tmpTimer)
    {
        head = tmpTimer->next;
        delete tmpTimer;
        tmpTimer = head;
    }
}

void OutTimerList::add_timer(OutTimer *timer)
{
    if (!timer)
        return;

    if (!head)
    {
        head = tail = timer;
        return;
    }

    add_timer(timer, head);
}

// 当定时器链表内的定时器有更新时需要调用此函数以保证升序
void OutTimerList::adjust_timer(OutTimer *timer)
{
    if (!timer)
        return;
    OutTimer *tmpTimer = timer->next;
    // 更新的定时器的expireTime只可能变大，所以看他是否需要向链表尾部移动即可
    if (!tmpTimer || (timer->expireTime < tmpTimer->expireTime)) // 尾节点或比next小，已满足升序
    {
        return;
    }

    // 将定时器移出并重新从当前(本timer的next)插入
    timer->next->pre = timer->pre;

    if (timer->pre)
        timer->pre->next = timer->next;
    if (timer == head)
        head = timer->next;
    // 不可能是尾节点(因为timer->next在此不为空)

    add_timer(timer, tmpTimer);

}

// 将定时器移出OutTimerList并delete
void OutTimerList::del_timer(OutTimer *timer)
{
    if (!timer)
        return;

    if (head == timer)
        head = timer->next;
    if (tail == timer)
        tail = timer->pre;

    if (timer->pre)
    {
        timer->pre->next = timer->next;
    }
    if (timer->next)
    {
        timer->next->pre = timer->pre;
    }

    delete timer;
}

// 以subHead为子头节点，默认subHead->pre以前都已经升序，在subHead处插入timer，保证升序
// 允许subHead==head，但subHead必须有值
void OutTimerList::add_timer(OutTimer *timer, OutTimer *subHead)
{

    if (!subHead)
        return;
    OutTimer *tmpTimer = subHead;

    // 找到第一个timer能插入的位置(没找到就是尾部)
    while (tmpTimer)
    {
        if (tmpTimer->expireTime > timer->expireTime)
        {
            break;
        }
        tmpTimer = tmpTimer->next;
    }

    // 如果是找到了(在tmpTimer前插入timer)
    if (tmpTimer)
    {
        if (tmpTimer == head) // 如果是插入头部前面，头部需要改变
        {
            head = timer;
        }
        // 插入
        timer->pre = tmpTimer->pre;
        tmpTimer->pre = timer;
        timer->next = tmpTimer;
        if (timer->pre)
        {
            timer->pre->next = timer;
        }
    }
    // 如果没找到，就插入尾部，并更新尾部
    else
    {
        if(head == tail)
        {

        }      
        timer->pre = tail->pre;
        timer->next = tail->next;
        tail = timer;
        if (!timer->pre)//前面为空，说明插入计时器前head==tail，现在head的next为空
        {
            head->next = timer;
            timer->pre = head;
        }
        else
        {
            timer->pre->next = timer;
        }
    }
}

// 定时任务函数（将OutTimerList中超时的定时器删除，并调用对应的回调函数，关闭套接字连接
void OutTimerList::tick()
{
    if (!head)
        return;

    time_t currentTime = time(NULL);
    OutTimer *tmpTimer = head;
    while (tmpTimer)
    {
        // 链表为升序，此节点没超时，后面的也没超时
        if (currentTime < tmpTimer->expireTime)
        {
            break;
        }

        LOG_INFO("timer tick");
        LOG_FLUSH();

        // 执行超时事件回调函数
        tmpTimer->expireFunc(tmpTimer->clientData);

        // 处理之后，从链表中删除，重置head节点
        head = tmpTimer->next; // 因为升序，所以此前的节点一定也都超时
        if (head)
            head->pre = NULL;
        delete tmpTimer;
        tmpTimer = head;
    }
}