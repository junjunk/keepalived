/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Scheduling framework. This code is highly inspired from
 *              the thread management routine (thread.c) present in the 
 *              very nice zebra project (http://www.zebra.org).
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

/* SNMP should be included first: it redefines "FREE" */
#ifdef _WITH_SNMP_
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
#include <net-snmp/agent/snmp_vars.h>
#undef FREE
extern int snmp;
#endif

#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <unistd.h>
#include "scheduler.h"
#include "memory.h"
#include "utils.h"
#include "signals.h"
#include "logger.h"
#include "bitops.h"

/* global vars */
thread_master_t *master = NULL;

/* Make thread master. */
thread_master_t *
thread_make_master(void)
{
	thread_master_t *new;
	int epollfd;

	new = (thread_master_t *) MALLOC(sizeof (thread_master_t));
	epollfd = epoll_create1(0);
	if (epollfd == -1)
	{
		log_message(LOG_ERR, "epoll_create1: %s", strerror(errno));
		assert(0);
	}
	new->epollfd = epollfd;
	return new;
}

/* error-logging wrapper around epoll_ctl */
static int
do_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	static struct epoll_event dummy_ev =
	{
		.events = 0,
		.data = {.ptr = NULL}
	};

	int ret = epoll_ctl(epfd, op, fd, event ? event : &dummy_ev);
	if (-1 == ret && __test_bit(LOG_DETAIL_BIT, &debug))
		log_message (
			LOG_ERR, "epoll_ctl: can't %s fd %d: %s",
			op == EPOLL_CTL_ADD ? "add" : "remove",
			fd, strerror(errno)
		);

	return ret;
}

static thread_list_t *
get_thread_list(thread_t * thread)
{
	switch (thread->type) {
	case THREAD_READ:
	case THREAD_WRITE:
	case THREAD_TIMER:
	case THREAD_CHILD:
		return NULL;
	case THREAD_SNMP_FD:
		return &thread->master->snmp;
	case THREAD_EVENT:
		return &thread->master->event;
	case THREAD_READY:
	case THREAD_READY_FD:
		return &thread->master->ready;
	default:
		assert(0);
	}
	return NULL;
}

/* Add a new thread to the list. */
static void
thread_list_add(thread_list_t * list, thread_t * thread)
{
	thread->next = NULL;
	thread->prev = list->tail;
	if (list->tail)
		list->tail->next = thread;
	else
		list->head = thread;
	list->tail = thread;
	list->count++;
}

/* Add a new thread to the list. */
void
thread_list_add_before(thread_list_t * list, thread_t * point
		       , thread_t * thread)
{
	thread->next = point;
	thread->prev = point->prev;
	if (point->prev)
		point->prev->next = thread;
	else
		list->head = thread;
	point->prev = thread;
	list->count++;
}

/* Add a thread in the list sorted by timeval */
void
thread_list_add_timeval(struct rb_root * root, thread_t * thread)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		thread_t *this = rb_entry(*new, thread_t, node);
		parent = *new;
		if (timer_cmp(thread->sands, this->sands) <= 0)
			new = &((*new)->rb_left);
		else
			new = &((*new)->rb_right);
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&thread->node, parent, new);
	rb_insert_color(&thread->node, root);
}

/* Delete a thread from the list. */
thread_t *
thread_list_delete(thread_list_t * list, thread_t * thread)
{
	if (thread->next)
		thread->next->prev = thread->prev;
	else
		list->tail = thread->prev;
	if (thread->prev)
		thread->prev->next = thread->next;
	else
		list->head = thread->next;
	thread->next = thread->prev = NULL;
	list->count--;
	return thread;
}

/* Free all unused thread. */
static void
thread_clean_unuse(thread_master_t * m)
{
	thread_t *thread;

	thread = m->unuse.head;
	while (thread) {
		thread_t *t;

		t = thread;
		thread = t->next;

		thread_list_delete(&m->unuse, t);

		/* free the thread */
		FREE(t);
		m->alloc--;
	}
}

/* Move thread to unuse list. */
static void
thread_add_unuse(thread_master_t * m, thread_t * thread)
{
	assert(m != NULL);
	assert(thread->next == NULL);
	assert(thread->prev == NULL);
	assert(thread->type == THREAD_UNUSED);
	thread_list_add(&m->unuse, thread);
}

/* Move list element to unuse queue */
static void
thread_destroy_list(thread_master_t * m, thread_list_t thread_list)
{
	thread_t *thread;

	thread = thread_list.head;

	while (thread) {
		thread_t *t;

		t = thread;
		thread = t->next;

		if (t->type == THREAD_READY_FD ||
		    t->type == THREAD_READ ||
		    t->type == THREAD_WRITE ||
		    t->type == THREAD_READ_TIMEOUT ||
		    t->type == THREAD_WRITE_TIMEOUT)
			close (t->u.fd);
		else if (t->type == THREAD_SNMP_FD)
			/* net-snmp seems to close its sockets before that,
			 * so logging and assertion is not needed here */
			epoll_ctl(t->master->epollfd, EPOLL_CTL_DEL
				     , t->u.fd, NULL);

		thread_list_delete(&thread_list, t);
		t->type = THREAD_UNUSED;
		thread_add_unuse(m, t);
	}
}

/* Move list element to unuse queue */
static void
thread_destroy_tree(thread_master_t * m, struct rb_root * root)
{
	thread_t *t;
	struct rb_node *node = rb_first(root);

	while (node) {
		t = rb_entry(node, thread_t, node);
		node = rb_next(node);

		if (t->type == THREAD_READY_FD ||
		    t->type == THREAD_READ ||
		    t->type == THREAD_WRITE ||
		    t->type == THREAD_READ_TIMEOUT ||
		    t->type == THREAD_WRITE_TIMEOUT)
			close (t->u.fd);
		else if (t->type == THREAD_SNMP_FD)
			/* net-snmp seems to close its sockets before that,
			 * so logging and assertion is not needed here */
			epoll_ctl(t->master->epollfd, EPOLL_CTL_DEL
				     , t->u.fd, NULL);

		t->type = THREAD_UNUSED;
		thread_add_unuse(m, t);
	}
	*root = RB_ROOT;
}

/* Cleanup master */
static void
thread_cleanup_master(thread_master_t * m)
{
	/* Unuse current thread lists */
	thread_destroy_tree(m, &m->read);
	thread_destroy_tree(m, &m->write);
	thread_destroy_tree(m, &m->timer);
	thread_destroy_tree(m, &m->child);

	thread_destroy_list(m, m->event);
	thread_destroy_list(m, m->ready);
	thread_destroy_list(m, m->snmp);

	/* Clear all FDs */
	if (m->epollfd >= 0)
		close(m->epollfd);
	m->epollfd = -1;

	/* Clean garbage */
	thread_clean_unuse(m);
}

/* Stop thread scheduler. */
void
thread_destroy_master(thread_master_t * m)
{
	thread_cleanup_master(m);
	FREE(m);
}

/* Delete top of the list and return it. */
thread_t *
thread_trim_head(thread_list_t * list)
{
	if (list->head)
		return thread_list_delete(list, list->head);
	return NULL;
}

/* Make new thread. */
thread_t *
thread_new(thread_master_t * m)
{
	thread_t *new;

	/* If one thread is already allocated return it */
	if (m->unuse.head) {
		new = thread_trim_head(&m->unuse);
		memset(new, 0, sizeof (thread_t));
		return new;
	}

	new = (thread_t *) MALLOC(sizeof (thread_t));
	m->alloc++;
	return new;
}

static thread_t *
thread_add_io(unsigned char type, thread_master_t * m,
	      int (*func) (thread_t *) , void *arg, int fd, long timer)
{
	thread_t *thread;
	struct epoll_event ev;

	assert(m != NULL);

	thread = thread_new(m);
	thread->type = type;
	thread->id = 0;
	thread->master = m;
	thread->func = func;
	thread->arg = arg;
	thread->u.fd = fd;

	ev.events = type == THREAD_READ ? EPOLLIN : EPOLLOUT;
	ev.data.ptr = thread;
	if (do_epoll_ctl(m->epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		thread->type = THREAD_UNUSED;
		thread_add_unuse(m, thread);
		return NULL;
	}

	/* Compute read timeout value */
	set_time_now();
	thread->sands = timer_add_long(time_now, timer);

	/* Sort the thread. */
	thread_list_add_timeval((type == THREAD_READ ? &m->read : &m->write), thread);

	return thread;
}

/* Add new read thread. */
thread_t *
thread_add_read(thread_master_t * m, int (*func) (thread_t *)
		, void *arg, int fd, long timer)
{
	return thread_add_io(THREAD_READ, m, func, arg, fd, timer);
}

/* Add new write thread. */
thread_t *
thread_add_write(thread_master_t * m, int (*func) (thread_t *)
		 , void *arg, int fd, long timer)
{
	return thread_add_io(THREAD_WRITE, m, func, arg, fd, timer);
}

/* Add timer event thread. */
thread_t *
thread_add_timer(thread_master_t * m, int (*func) (thread_t *)
		 , void *arg, long timer)
{
	thread_t *thread;

	assert(m != NULL);

	thread = thread_new(m);
	thread->type = THREAD_TIMER;
	thread->id = 0;
	thread->master = m;
	thread->func = func;
	thread->arg = arg;

	/* Do we need jitter here? */
	set_time_now();
	thread->sands = timer_add_long(time_now, timer);

	/* Sort by timeval. */
	thread_list_add_timeval(&m->timer, thread);

	return thread;
}

/* Add a child thread. */
thread_t *
thread_add_child(thread_master_t * m, int (*func) (thread_t *)
		 , void * arg, pid_t pid, long timer)
{
	thread_t *thread;

	assert(m != NULL);

	thread = thread_new(m);
	thread->type = THREAD_CHILD;
	thread->id = 0;
	thread->master = m;
	thread->func = func;
	thread->arg = arg;
	thread->u.c.pid = pid;
	thread->u.c.status = 0;

	/* Compute write timeout value */
	set_time_now();
	thread->sands = timer_add_long(time_now, timer);

	/* Sort by timeval. */
	thread_list_add_timeval(&m->child, thread);

	return thread;
}

/* Add simple event thread. */
thread_t *
thread_add_event(thread_master_t * m, int (*func) (thread_t *)
		 , void *arg, int val)
{
	thread_t *thread;

	assert(m != NULL);

	thread = thread_new(m);
	thread->type = THREAD_EVENT;
	thread->id = 0;
	thread->master = m;
	thread->func = func;
	thread->arg = arg;
	thread->u.val = val;
	thread_list_add(&m->event, thread);

	return thread;
}

/* Add simple event thread. */
thread_t *
thread_add_terminate_event(thread_master_t * m)
{
	thread_t *thread;

	assert(m != NULL);

	thread = thread_new(m);
	thread->type = THREAD_TERMINATE;
	thread->id = 0;
	thread->master = m;
	thread->func = NULL;
	thread->arg = NULL;
	thread->u.val = 0;
	thread_list_add(&m->event, thread);

	return thread;
}

/* Cancel thread from scheduler. */
int
thread_cancel(thread_t * thread)
{
	thread_list_t *list;

	if (!thread)
		return -1;

	if (thread->type == THREAD_READ ||
	    thread->type == THREAD_WRITE ||
	    thread->type == THREAD_SNMP_FD)
		if (do_epoll_ctl(thread->master->epollfd, EPOLL_CTL_DEL
			         , thread->u.fd, NULL))
			assert(0);
	list = get_thread_list(thread);
	if (list)
		thread_list_delete(list, thread);
	else switch (thread->type)
	{
	case THREAD_READ:
		rb_erase(&thread->node, &thread->master->read);
		break;
	case THREAD_WRITE:
		rb_erase(&thread->node, &thread->master->write);
		break;
	case THREAD_TIMER:
		rb_erase(&thread->node, &thread->master->timer);
		break;
	case THREAD_CHILD:
		rb_erase(&thread->node, &thread->master->child);
		break;
	}

	thread->type = THREAD_UNUSED;
	thread_add_unuse(thread->master, thread);
	return 0;
}

/* Delete all events which has argument value arg. */
void
thread_cancel_event(thread_master_t * m, void *arg)
{
	thread_t *thread;

	thread = m->event.head;
	while (thread) {
		thread_t *t;

		t = thread;
		thread = t->next;

		if (t->arg == arg)
			thread_cancel(t);
	}
}

/* Update timer value */
static void
thread_update_timer(struct rb_root *root, timeval_t *timer_min)
{
	struct rb_node *node = rb_first(root);

	if (node) {
		thread_t *t = rb_entry(node, thread_t, node);
		if (!timer_isnull(*timer_min)) {
			if (timer_cmp(t->sands, *timer_min) <= 0) {
				*timer_min = t->sands;
			}
		} else {
			*timer_min = t->sands;
		}
	}
}

/* Compute the wait timer. Take care of timeouted fd */
static void
thread_compute_timer(thread_master_t * m, timeval_t * timer_wait)
{
	timeval_t timer_min;

	/* Prepare timer */
	timer_reset(timer_min);
	thread_update_timer(&m->timer, &timer_min);
	thread_update_timer(&m->write, &timer_min);
	thread_update_timer(&m->read, &timer_min);
	thread_update_timer(&m->child, &timer_min);

	/* Take care about monothonic clock */
	if (!timer_isnull(timer_min)) {
		timer_min = timer_sub(timer_min, time_now);
		if (timer_min.tv_sec < 0) {
			timer_reset(timer_min);
		} else if (timer_min.tv_sec >= 1) {
			timer_min.tv_sec = 1;
			timer_min.tv_usec = 0;
		}

		*timer_wait = timer_min;
	} else {
		timer_wait->tv_sec = 1;
		timer_wait->tv_usec = 0;
	}
}

static void
process_timeout_threads(thread_master_t *m, struct rb_root *root,
			unsigned char new_type)
{
	struct rb_node *node;
	thread_t *t;

	while ((node = rb_first(root))) {
		t = rb_entry(node, thread_t, node);
		if (timer_cmp(time_now, t->sands) < 0)
			break;
		rb_erase(node, root);

		if (t->type == THREAD_READ ||
		    t->type == THREAD_WRITE
		)
			if (do_epoll_ctl(m->epollfd, EPOLL_CTL_DEL
					 , t->u.fd, NULL))
				assert(0);
		t->type = new_type;
		thread_list_add(&m->ready, t);
	}
}

static void
register_signal_reader(thread_master_t * m)
{
	int signal_fd;
	struct epoll_event ev;

	signal_fd = signal_rfd();
	ev.events = EPOLLIN;
	ev.data.ptr = NULL; /* special value indicating signal pipe */
	if (do_epoll_ctl(m->epollfd, EPOLL_CTL_ADD, signal_fd, &ev))
		assert(0);
}

#ifdef _WITH_SNMP_
static void
prepare_snmp_epoll(thread_master_t * m, timeval_t * timer_wait)
{
	timeval_t snmp_timer_wait;
	int fdsetsize = FD_SETSIZE;
	/* fd_set for iterate over it */
	long readfd[FD_SETSIZE / (8 * sizeof(long))];
	long n;
	int snmpblock = 0;
	int i, fd, bit;
	struct epoll_event ev;
	thread_t *t;

	/* When SNMP is enabled, we may have to epoll on additional
	 * FD. snmp_select_info() will add them to `readfd'. The trick
	 * with this function is its last argument. We need to set it
	 * to 0 and we need to use the provided new timer only if it
	 * is still set to 0. */
	memcpy(&snmp_timer_wait, timer_wait, sizeof(timeval_t));
	memset(readfd, 0, sizeof (readfd));
	int ret = snmp_select_info(&fdsetsize, (fd_set *)&readfd
				   , &snmp_timer_wait, &snmpblock);
	if (! ret)
		return;
	if (snmpblock == 0)
		memcpy(timer_wait, &snmp_timer_wait, sizeof(timeval_t));

	/* iterate over readfd and register read preudo-threads */
	for(i = 0; ret && (i < ARRAY_LEN(readfd)); i++) {
		if ((n = readfd[i])) {
			fd = i * sizeof(long) * 8;
			do {
				bit = ffsl(n);
				n >>= bit;
				fd += bit;

				t = thread_new(m);
				t->master = m;
				t->type = THREAD_SNMP_FD;
				t->u.fd = fd - 1;
				thread_list_add(&m->snmp, t);
				ev.events = EPOLLIN;
				ev.data.ptr = t;
				if (do_epoll_ctl(m->epollfd, EPOLL_CTL_ADD
						 , t->u.fd, &ev))
					assert(0);
				--ret;
			} while (n);
		}
	}
}
#endif

/* Fetch next ready thread. */
thread_t *
thread_fetch(thread_master_t * m, thread_t * fetch)
{
	thread_t *thread;
	timeval_t timer_wait;
	int nevents, n;
	struct epoll_event events[64];

	assert(m != NULL);

	/* Timer initialization */
	memset(&timer_wait, 0, sizeof (timeval_t));

retry:	/* When thread can't fetch try to find next thread again. */

	/* If there is event process it first. */
	while ((thread = thread_trim_head(&m->event))) {
		*fetch = *thread;

		/* If daemon hanging event is received return NULL pointer */
		if (thread->type == THREAD_TERMINATE) {
			thread->type = THREAD_UNUSED;
			thread_add_unuse(m, thread);
			return NULL;
		}
		thread->type = THREAD_UNUSED;
		thread_add_unuse(m, thread);
		return fetch;
	}

	/* If there is ready threads process them */
	while ((thread = thread_trim_head(&m->ready))) {
		*fetch = *thread;
		thread->type = THREAD_UNUSED;
		thread_add_unuse(m, thread);
		return fetch;
	}

	/*
	 * Re-read the current time to get the maximum accuracy.
	 * Calculate select wait timer. Take care of timeouted fd.
	 */
	set_time_now();
	thread_compute_timer(m, &timer_wait);

#ifdef _WITH_SNMP_
	int snmp_events = 0;
	fd_set snmp_fdset;

	if (snmp) {
		while (m->snmp.head)
			thread_cancel(m->snmp.head);
		prepare_snmp_epoll(m, &timer_wait);
	}
#endif

	nevents = epoll_wait(m->epollfd, events, ARRAY_LEN(events)
			     , timer_milli(timer_wait));
	if (nevents < 0) {
		if (errno == EINTR)
			goto retry;
		/* Real error. */
		DBG("epoll_wait error: %s", strerror(errno));
		assert(0);
	}

	/* Update current time */
	set_time_now();

	/* Timeout children */
	process_timeout_threads (m, &m->child, THREAD_CHILD_TIMEOUT);

	/* turn pending read/write theads into ready. */
	for (n = 0; n < nevents; ++n) {
		thread_t *t = events[n].data.ptr;
		if (! t) {
			/* handle signals synchronously,
			 * including child reaping */
			signal_run_callback();
		}
#ifdef _WITH_SNMP_
		else if (t->type == THREAD_SNMP_FD) {
			if (! snmp_events)
				FD_ZERO(&snmp_fdset);
			FD_SET(t->u.fd, &snmp_fdset);
			++snmp_events;
		}
#endif
		else {
			rb_erase(&t->node, t->type == THREAD_READ ? &m->read : &m->write);
			thread_list_add(&m->ready, t);
			t->type = THREAD_READY_FD;
			if (do_epoll_ctl(m->epollfd, EPOLL_CTL_DEL
					 , t->u.fd, NULL))
				assert(0);
		}
	}

	/* process timed-out IO. */
	process_timeout_threads (m, &m->read, THREAD_READ_TIMEOUT);
	process_timeout_threads (m, &m->write, THREAD_WRITE_TIMEOUT);

	/* Timer thread. */
	process_timeout_threads (m, &m->timer, THREAD_READY);

	/* Return one event. */
	thread = thread_trim_head(&m->ready);

#ifdef _WITH_SNMP_
	if (snmp) {
		if (snmp_events > 0)
			snmp_read(&snmp_fdset);
		else if (nevents == 0)
			snmp_timeout();
		run_alarms();
		netsnmp_check_outstanding_agent_requests();
		while (m->snmp.head)
			thread_cancel(m->snmp.head);
	}
#endif

	/* There is no ready thread. */
	if (!thread)
		goto retry;

	*fetch = *thread;
	thread->type = THREAD_UNUSED;
	thread_add_unuse(m, thread);

	return fetch;
}

/* Synchronous signal handler to reap child processes */
void
thread_child_handler(void * v, int sig)
{
	thread_master_t * m = v;
	struct rb_node *node, *next;

	/*
	 * This is O(n^2), but there will only be a few entries on
	 * this list.
	 */
	pid_t pid;
	int status;
	while ((pid = waitpid(-1, &status, WNOHANG))) {
		if (pid == -1) {
			if (errno == ECHILD)
				return;
			DBG("waitpid error: %s", strerror(errno));
			assert(0);
		} else {
			node = rb_first(&m->child);
			while (node) {
				thread_t *t = rb_entry(node, thread_t, node);
				next = rb_next(node);
				if (pid == t->u.c.pid) {
					rb_erase(node, &m->child);
					t->u.c.status = status;
					t->type = THREAD_READY;
					thread_list_add(&m->ready, t);
					break;
				}
				node = next;
			}
		}
	}
}


/* Make unique thread id for non pthread version of thread manager. */
unsigned long int
thread_get_id(void)
{
	static unsigned long int counter = 0;
	return ++counter;
}

/* Call thread ! */
void
thread_call(thread_t * thread)
{
	thread->id = thread_get_id();
	(*thread->func) (thread);
}

/* Our infinite scheduling loop */
void
launch_scheduler(void)
{
	thread_t thread;

	signal_set(SIGCHLD, thread_child_handler, master);
	register_signal_reader(master);

	/*
	 * Processing the master thread queues,
	 * return and execute one ready thread.
	 */
	while (thread_fetch(master, &thread)) {
		/* Run until error, used for debuging only */
#ifdef _DEBUG_
		if (__test_bit(LOG_DETAIL_BIT, &debug) &&
		    __test_bit(MEM_ERR_DETECT_BIT, &debug)
		) {
			__clear_bit(LOG_DETAIL_BIT, &debug);
			__clear_bit(MEM_ERR_DETECT_BIT, &debug);
			thread_add_terminate_event(master);
		}
#endif
		thread_call(&thread);
	}
}
