/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        scheduler.c include file.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2012 Alexandre Cassen, <acassen@linux-vs.org>
 */

#ifndef _SCHEDULER_H
#define _SCHEDULER_H

/* system includes */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <sys/epoll.h>
#include "timer.h"
#include "rbtree.h"


#define CHECKTID(m)  do {  \
						pid_t tid = gettid(); \
						assert(m != NULL); \
						if (m->last_tid && m->last_tid != tid) { \
							log_message(LOG_ERR, "%s: tid %i master %p", (char *)__FUNCTION__, tid, (void *)(m)); \
							m->last_tid = tid; \
						} \
					} while (0)

/* Thread itself. */
typedef struct _thread {
	unsigned long id;
	unsigned char type;		/* thread type */
	struct _thread *next;		/* next pointer of the thread */
	struct _thread *prev;		/* previous pointer of the thread */
	struct rb_node node;            /* rbtree node */
	struct _thread_master *master;	/* pointer to the struct thread_master. */
	int (*func) (struct _thread *);	/* event function */
	void *arg;			/* event argument */
	timeval_t sands;		/* rest of time sands value. */
	union {
		int val;		/* second argument of the event. */
		int fd;			/* file descriptor in case of read/write. */
		struct {
			pid_t pid;	/* process id a child thread is wanting. */
			int status;	/* return status of the process */
		} c;
	} u;
} thread_t;

/* Linked list of thread. */
typedef struct _thread_list {
	thread_t *head;
	thread_t *tail;
	int count;
} thread_list_t;

#define PID_HASHSIZE 64

/* Master of the theads. */
typedef struct _thread_master {
	struct rb_root wait;

	thread_list_t event;
	thread_list_t ready;
	thread_list_t unuse;
	thread_list_t snmp;

	thread_list_t child_hash[PID_HASHSIZE];

	int epollfd;
	unsigned long alloc;
	time_storage_t tstore;
	int stop_flag;
	pid_t last_tid;
} thread_master_t;
#define TIME_NOW(m) (m->tstore.now)

/* Thread types. */
#define THREAD_READ		0
#define THREAD_WRITE		1
#define THREAD_TIMER		2
#define THREAD_EVENT		3
#define THREAD_CHILD		4
#define THREAD_READY		5
#define THREAD_UNUSED		6
#define THREAD_WRITE_TIMEOUT	7
#define THREAD_READ_TIMEOUT	8
#define THREAD_CHILD_TIMEOUT	9
#define THREAD_TERMINATE	10
#define THREAD_READY_FD		11
#define THREAD_SNMP_FD		12

/* MICRO SEC def */
#define BOOTSTRAP_DELAY TIMER_HZ
#define RESPAWN_TIMER	60*TIMER_HZ

/* Macros. */
#define THREAD_ARG(X) ((X)->arg)
#define THREAD_FD(X)  ((X)->u.fd)
#define THREAD_VAL(X) ((X)->u.val)
#define THREAD_CHILD_PID(X) ((X)->u.c.pid)
#define THREAD_CHILD_STATUS(X) ((X)->u.c.status)

/* global vars exported */
extern thread_master_t *master;
#ifdef _WITH_SNMP_
extern int snmp_enable;
#endif

/* Prototypes. */
extern thread_master_t *thread_make_master(void);
extern void thread_init_master(thread_master_t *master);
extern thread_t *thread_add_terminate_event(thread_master_t *);
extern void thread_destroy_master(thread_master_t *);
extern void thread_destroy_queues(thread_master_t *);
extern thread_t *thread_add_read(thread_master_t *, int (*func) (thread_t *), void *, int, long);
extern thread_t *thread_add_write(thread_master_t *, int (*func) (thread_t *), void *, int, long);
extern thread_t *thread_add_timer(thread_master_t *, int (*func) (thread_t *), void *, long);
extern thread_t *thread_add_child(thread_master_t *, int (*func) (thread_t *), void *, pid_t, long);
extern thread_t *thread_add_event(thread_master_t *, int (*func) (thread_t *), void *, int);
extern int thread_cancel(thread_t *);
extern void thread_cancel_event(thread_master_t *, void *);
extern thread_t *thread_fetch(thread_master_t *, thread_t *);
extern void thread_child_handler(void *, int);
extern void thread_call(thread_t *);
extern void launch_scheduler(void);
extern void set_time_master(thread_master_t *master);
extern void thread_cleanup_master(thread_master_t *master);
extern void register_signal_reader(thread_master_t *master);

#endif
