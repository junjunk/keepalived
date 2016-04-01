#ifndef _LOCK_H
#define _LOCK_H

typedef pthread_mutex_t lock_t;

void lock(lock_t *lock);
void unlock(lock_t *lock);
void lock_init(lock_t *lock);
int lock_destroy(lock_t *lock);

#endif //_LOCK_H
