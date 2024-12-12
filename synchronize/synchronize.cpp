#include "synchronize.h"

mutx::mutx()
{
    if (pthread_mutex_init(&mtx, NULL) != 0)
    {
        throw std::exception();
    }
}

mutx::~mutx()
{
    pthread_mutex_destroy(&mtx);
}

bool mutx::lock()
{
    return pthread_mutex_lock(&mtx) == 0;
}

bool mutx::unlock()
{
    return pthread_mutex_unlock(&mtx) == 0;
}

pthread_mutex_t *mutx::get_mutex_p()
{
    return &mtx;
}

sem::sem()
{
    if (sem_init(&sm, 0, 0) != 0)
    {
        throw std::exception();
    }
}

sem::sem(int cnt)
{
    if (sem_init(&sm, 0, cnt) != 0)
    {
        throw std::exception();
    }
}

bool sem::wait()
{
    return sem_wait(&sm) == 0;
}

bool sem::post()
{
    return sem_post(&sm) == 0;
}

sem::~sem()
{
    sem_destroy(&sm);
}

cond::cond()
{
    if (pthread_cond_init(&cd, NULL) != 0)
    {
        throw std::exception();
    }
}

cond::~cond()
{
    pthread_cond_destroy(&cd);
}

bool cond::wait(pthread_mutex_t *p_mtx)
{
    return pthread_cond_wait(&cd, p_mtx) == 0;
}

bool cond::timewait(pthread_mutex_t *p_mtx, timespec t)
{
    return pthread_cond_timedwait(&cd, p_mtx, &t) == 0;
}

bool cond::signal()
{
    return pthread_cond_signal(&cd) == 0;
}

bool cond::broadcast()
{
    return pthread_cond_broadcast(&cd) == 0;
}