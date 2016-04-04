#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include "lock.h"

void lock(lock_t *lock)
{
	int ret = pthread_mutex_lock(lock);
	assert(!ret);
}

void unlock(lock_t *lock)
{
	int ret = pthread_mutex_unlock(lock);
	assert(!ret);
}

void lock_init(lock_t *lock)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
	pthread_mutex_init(lock, &attr);
	pthread_mutexattr_destroy(&attr);
}

int lock_destroy(lock_t *lock)
{
	return pthread_mutex_destroy(lock);
}
