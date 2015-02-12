/*
 * Written by Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "internal.h"
#include "thread.h"
#include "mutex.h"
#include "cond.h"
#include "clock.h"

static int cobalt_mutex_init_inner(struct cobalt_mutex_shadow *shadow,
				   struct cobalt_mutex *mutex,
				   struct cobalt_mutex_state *state,
				   const struct cobalt_mutexattr *attr)
{
	int synch_flags = XNSYNCH_PRIO | XNSYNCH_OWNER;
	struct cobalt_ppd *sys_ppd;
	struct cobalt_kqueues *kq;
	spl_t s;
	int err;

	kq = cobalt_kqueues(attr->pshared);
	sys_ppd = cobalt_ppd_get(attr->pshared);
	err = xnregistry_enter_anon(mutex, &shadow->handle);
	if (err < 0)
		return err;

	mutex->handle = shadow->handle;
	shadow->magic = COBALT_MUTEX_MAGIC;
	shadow->lockcnt = 0;

	shadow->attr = *attr;
	shadow->state_offset = cobalt_umm_offset(&sys_ppd->umm, state);

	if (attr->protocol == PTHREAD_PRIO_INHERIT)
		synch_flags |= XNSYNCH_PIP;

	mutex->magic = COBALT_MUTEX_MAGIC;
	xnsynch_init(&mutex->synchbase, synch_flags, &state->owner);
	state->flags = (attr->type == PTHREAD_MUTEX_ERRORCHECK
			? COBALT_MUTEX_ERRORCHECK : 0);
	mutex->attr = *attr;
	mutex->owningq = kq;
	INIT_LIST_HEAD(&mutex->conds);

	xnlock_get_irqsave(&nklock, s);
	list_add_tail(&mutex->link, &kq->mutexq);
	xnlock_put_irqrestore(&nklock, s);

	return 0;
}

static void
cobalt_mutex_destroy_inner(xnhandle_t handle, struct cobalt_kqueues *q)
{
	struct cobalt_mutex *mutex;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
	mutex = xnregistry_lookup(handle, NULL);
	if (!cobalt_obj_active(mutex, COBALT_MUTEX_MAGIC, typeof(*mutex))) {
		xnlock_put_irqrestore(&nklock, s);
		printk(XENO_WARN "mutex_destroy: invalid mutex %x\n",
			mutex ? mutex->magic : ~0);
		return;
	}
	xnregistry_remove(handle);
	list_del(&mutex->link);
	/*
	 * synchbase wait queue may not be empty only when this
	 * function is called from cobalt_mutex_pkg_cleanup, hence the
	 * absence of xnsched_run().
	 */
	xnsynch_destroy(&mutex->synchbase);
	cobalt_mark_deleted(mutex);
	xnlock_put_irqrestore(&nklock, s);

	cobalt_umm_free(&cobalt_ppd_get(mutex->attr.pshared)->umm,
			mutex->synchbase.fastlock);
	xnfree(mutex);
}

/* must be called with nklock locked, interrupts off. */
int __cobalt_mutex_acquire_unchecked(struct xnthread *cur,
				     struct cobalt_mutex *mutex,
				     const struct timespec *ts)
{
	if (ts) {
		if (ts->tv_nsec >= ONE_BILLION)
			return -EINVAL;
		xnsynch_acquire(&mutex->synchbase, ts2ns(ts) + 1, XN_REALTIME);
	} else
		xnsynch_acquire(&mutex->synchbase, XN_INFINITE, XN_RELATIVE);

	if (xnthread_test_info(cur, XNBREAK | XNRMID | XNTIMEO)) {
		if (xnthread_test_info(cur, XNBREAK))
			return -EINTR;
		else if (xnthread_test_info(cur, XNTIMEO))
			return -ETIMEDOUT;
		else /* XNRMID */
			return -EINVAL;
	}

	return 0;
}

int cobalt_mutex_release(struct xnthread *cur,
			 struct cobalt_mutex *mutex)
{
	struct cobalt_mutex_state *state;
	struct cobalt_cond *cond;
	unsigned long flags;
	int need_resched;

	if (!cobalt_obj_active(mutex, COBALT_MUTEX_MAGIC, struct cobalt_mutex))
		 return -EINVAL;

#if XENO_DEBUG(USER)
	if (mutex->owningq != cobalt_kqueues(mutex->attr.pshared))
		return -EPERM;
#endif
	state = container_of(mutex->synchbase.fastlock, struct cobalt_mutex_state, owner);
	flags = state->flags;
	need_resched = 0;
	if ((flags & COBALT_MUTEX_COND_SIGNAL)) {
		state->flags = flags & ~COBALT_MUTEX_COND_SIGNAL;
		if (!list_empty(&mutex->conds)) {
			list_for_each_entry(cond, &mutex->conds, mutex_link)
				need_resched |=
				cobalt_cond_deferred_signals(cond);
		}
	}
	need_resched |= xnsynch_release(&mutex->synchbase, cur) != NULL;

	return need_resched;
}

int __cobalt_mutex_timedlock_break(struct cobalt_mutex_shadow __user *u_mx,
				   const void __user *u_ts,
				   int (*fetch_timeout)(struct timespec *ts,
							const void __user *u_ts))
{
	struct xnthread *curr = xnthread_current();
	struct timespec ts, *tsp = NULL;
	struct cobalt_mutex *mutex;
	xnhandle_t handle;
	spl_t s;
	int ret;

	/* We need a valid thread handle for the fast lock. */
	if (curr->handle == XN_NO_HANDLE)
		return -EPERM;

	handle = cobalt_get_handle_from_user(&u_mx->handle);
redo:
	xnlock_get_irqsave(&nklock, s);

	mutex = xnregistry_lookup(handle, NULL);

	if (!cobalt_obj_active(mutex, COBALT_MUTEX_MAGIC, struct cobalt_mutex)) {
		ret = -EINVAL;
		goto out;
	}
#if XENO_DEBUG(USER)
	if (mutex->owningq != cobalt_kqueues(mutex->attr.pshared)) {
		ret = -EPERM;
		goto out;
	}
#endif
	if (xnsynch_owner_check(&mutex->synchbase, curr)) {
		if (fetch_timeout) {
			xnlock_put_irqrestore(&nklock, s);
			ret = fetch_timeout(&ts, u_ts);
			if (ret)
				return ret;

			fetch_timeout = NULL;
			tsp = &ts;
			goto redo; /* Revalidate handle. */
		}
		ret = __cobalt_mutex_acquire_unchecked(curr, mutex, tsp);
		xnlock_put_irqrestore(&nklock, s);
		return ret;
	}

	/* We already own the mutex, something looks wrong. */

	ret = -EBUSY;
	switch(mutex->attr.type) {
	case PTHREAD_MUTEX_NORMAL:
		/* Attempting to relock a normal mutex, deadlock. */
#if XENO_DEBUG(USER)
		printk(XENO_WARN
		       "thread %s deadlocks on non-recursive mutex\n",
		       curr->name);
#endif
		/* Make the caller hang. */
		__cobalt_mutex_acquire_unchecked(curr, mutex, NULL);
		break;

	case PTHREAD_MUTEX_ERRORCHECK:
	case PTHREAD_MUTEX_RECURSIVE:
		/*
		 * Recursive mutexes are handled in user-space, so
		 * these cases should never happen.
		 */
		ret = -EINVAL;
		break;
	}
out:
	xnlock_put_irqrestore(&nklock, s);

	return ret;
}

COBALT_SYSCALL(mutex_check_init, current,
	       (struct cobalt_mutex_shadow __user *u_mx))
{
	struct cobalt_mutex *mutex;
	xnhandle_t handle;
	int err;
	spl_t s;

	handle = cobalt_get_handle_from_user(&u_mx->handle);

	xnlock_get_irqsave(&nklock, s);
	mutex = xnregistry_lookup(handle, NULL);
	if (cobalt_obj_active(mutex, COBALT_MUTEX_MAGIC, typeof(*mutex)))
		/* mutex is already in a queue. */
		err = -EBUSY;
	else
		err = 0;

	xnlock_put_irqrestore(&nklock, s);
	return err;
}

COBALT_SYSCALL(mutex_init, current,
	       (struct cobalt_mutex_shadow __user *u_mx,
		const struct cobalt_mutexattr __user *u_attr))
{
	struct cobalt_mutex_state *state;
	struct cobalt_mutexattr attr;
	struct cobalt_mutex_shadow mx;
	struct cobalt_mutex *mutex;
	int err;

	if (__xn_safe_copy_from_user(&mx, u_mx, sizeof(mx)))
		return -EFAULT;

	if (__xn_safe_copy_from_user(&attr, u_attr, sizeof(attr)))
		return -EFAULT;

	mutex = xnmalloc(sizeof(*mutex));
	if (mutex == NULL)
		return -ENOMEM;

	state = cobalt_umm_alloc(&cobalt_ppd_get(attr.pshared)->umm,
				 sizeof(*state));
	if (state == NULL) {
		xnfree(mutex);
		return -EAGAIN;
	}

	err = cobalt_mutex_init_inner(&mx, mutex, state, &attr);
	if (err) {
		xnfree(mutex);
		cobalt_umm_free(&cobalt_ppd_get(attr.pshared)->umm, state);
		return err;
	}

	return __xn_safe_copy_to_user(u_mx, &mx, sizeof(*u_mx));
}

COBALT_SYSCALL(mutex_destroy, current,
	       (struct cobalt_mutex_shadow __user *u_mx))
{
	struct cobalt_mutex *mutex;
	struct cobalt_mutex_shadow mx;
	spl_t s;
	int err;

	if (__xn_safe_copy_from_user(&mx, u_mx, sizeof(mx)))
		return -EFAULT;

	xnlock_get_irqsave(&nklock, s);
	mutex = xnregistry_lookup(mx.handle, NULL);
	if (!cobalt_obj_active(mutex, COBALT_MUTEX_MAGIC, typeof(*mutex))) {
		err = -EINVAL;
		goto err_unlock;
	}
	if (cobalt_kqueues(mutex->attr.pshared) != mutex->owningq) {
		err = -EPERM;
		goto err_unlock;
	}

	if (xnsynch_fast_owner_check(mutex->synchbase.fastlock,
					XN_NO_HANDLE) != 0) {
		err = -EBUSY;
		goto err_unlock;
	}

	if (!list_empty(&mutex->conds)) {
		err = -EBUSY;
	  err_unlock:
		xnlock_put_irqrestore(&nklock, s);
		return err;
	}

	cobalt_mark_deleted(&mx);
	xnlock_put_irqrestore(&nklock, s);
	cobalt_mutex_destroy_inner(mx.handle, mutex->owningq);

	return __xn_safe_copy_to_user(u_mx, &mx, sizeof(*u_mx));
}

COBALT_SYSCALL(mutex_trylock, primary,
	       (struct cobalt_mutex_shadow __user *u_mx))
{
	struct xnthread *curr = xnthread_current();
	struct cobalt_mutex *mutex;
	xnhandle_t handle;
	spl_t s;
	int err;

	handle = cobalt_get_handle_from_user(&u_mx->handle);

	xnlock_get_irqsave(&nklock, s);
	mutex = xnregistry_lookup(handle, NULL);
	if (!cobalt_obj_active(mutex, COBALT_MUTEX_MAGIC, typeof(*mutex))) {
		err = -EINVAL;
		goto err_unlock;
	}

	err = xnsynch_fast_acquire(mutex->synchbase.fastlock, curr->handle);
	switch(err) {
	case 0:
		if (xnthread_test_state(curr, XNWEAK))
			curr->res_count++;
		break;

/* This should not happen, as recursive mutexes are handled in
   user-space */
	case -EBUSY:
		err = -EINVAL;
		break;

	case -EAGAIN:
		err = -EBUSY;
		break;
	}
  err_unlock:
	xnlock_put_irqrestore(&nklock, s);

	return err;
}

COBALT_SYSCALL(mutex_lock, primary,
	       (struct cobalt_mutex_shadow __user *u_mx))
{
	return __cobalt_mutex_timedlock_break(u_mx, NULL, NULL);
}

static inline int mutex_fetch_timeout(struct timespec *ts,
				      const void __user *u_ts)
{
	return u_ts == NULL ? -EFAULT :
		__xn_safe_copy_from_user(ts, u_ts, sizeof(*ts));
}

COBALT_SYSCALL(mutex_timedlock, primary,
	       (struct cobalt_mutex_shadow __user *u_mx,
		const struct timespec __user *u_ts))
{
	return __cobalt_mutex_timedlock_break(u_mx, u_ts, mutex_fetch_timeout);
}

COBALT_SYSCALL(mutex_unlock, nonrestartable,
	       (struct cobalt_mutex_shadow __user *u_mx))
{
	struct cobalt_mutex *mutex;
	struct xnthread *curr;
	xnhandle_t handle;
	int err;
	spl_t s;

	handle = cobalt_get_handle_from_user(&u_mx->handle);
	curr = xnthread_current();

	xnlock_get_irqsave(&nklock, s);
	mutex = xnregistry_lookup(handle, NULL);
	err = cobalt_mutex_release(curr, mutex);
	if (err < 0)
		goto out;

	if (err) {
		xnsched_run();
		err = 0;
	}
 out:
	xnlock_put_irqrestore(&nklock, s);

	return err;
}

void cobalt_mutexq_cleanup(struct cobalt_kqueues *q)
{
	struct cobalt_mutex *mutex, *tmp;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (list_empty(&q->mutexq))
		goto out;

	list_for_each_entry_safe(mutex, tmp, &q->mutexq, link) {
		xnlock_put_irqrestore(&nklock, s);
		cobalt_mutex_destroy_inner(mutex->handle, q);
		xnlock_get_irqsave(&nklock, s);
	}
out:
	xnlock_put_irqrestore(&nklock, s);
}

void cobalt_mutex_pkg_init(void)
{
	INIT_LIST_HEAD(&cobalt_global_kqueues.mutexq);
}

void cobalt_mutex_pkg_cleanup(void)
{
	cobalt_mutexq_cleanup(&cobalt_global_kqueues);
}
