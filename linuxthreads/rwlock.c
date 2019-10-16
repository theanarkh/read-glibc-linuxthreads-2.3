/* Read-write lock implementation.
   Copyright (C) 1998, 2000 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Xavier Leroy <Xavier.Leroy@inria.fr>
   and Ulrich Drepper <drepper@cygnus.com>, 1998.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include <bits/libc-lock.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include "internals.h"
#include "queue.h"
#include "spinlock.h"
#include "restart.h"

/* Function called by pthread_cancel to remove the thread from
   waiting inside pthread_rwlock_timedrdlock or pthread_rwlock_timedwrlock. */

static int rwlock_rd_extricate_func(void *obj, pthread_descr th)
{
  pthread_rwlock_t *rwlock = obj;
  int did_remove = 0;

  __pthread_lock(&rwlock->__rw_lock, NULL);
  did_remove = remove_from_queue(&rwlock->__rw_read_waiting, th);
  __pthread_unlock(&rwlock->__rw_lock);

  return did_remove;
}

static int rwlock_wr_extricate_func(void *obj, pthread_descr th)
{
  pthread_rwlock_t *rwlock = obj;
  int did_remove = 0;

  __pthread_lock(&rwlock->__rw_lock, NULL);
  did_remove = remove_from_queue(&rwlock->__rw_write_waiting, th);
  __pthread_unlock(&rwlock->__rw_lock);

  return did_remove;
}

/*
 * Check whether the calling thread already owns one or more read locks on the
 * specified lock. If so, return a pointer to the read lock info structure
 * corresponding to that lock.
 */
// 判断锁是否在当前线程的读锁列表中
static pthread_readlock_info *
rwlock_is_in_list(pthread_descr self, pthread_rwlock_t *rwlock)
{
  pthread_readlock_info *info;

  for (info = THREAD_GETMEM (self, p_readlock_list); info != NULL;
       info = info->pr_next)
    {
      if (info->pr_lock == rwlock)
	return info;
    }

  return NULL;
}

/*
 * Add a new lock to the thread's list of locks for which it has a read lock.
 * A new info node must be allocated for this, which is taken from the thread's
 * free list, or by calling malloc. If malloc fails, a null pointer is
 * returned. Otherwise the lock info structure is initialized and pushed
 * onto the thread's list.
 */
// 把锁加入当前线程的读锁队列
static pthread_readlock_info *
rwlock_add_to_list(pthread_descr self, pthread_rwlock_t *rwlock)
{
  pthread_readlock_info *info = THREAD_GETMEM (self, p_readlock_free);
  // 有可用的读锁，更新头指针指向下一个读锁
  if (info != NULL)
    THREAD_SETMEM (self, p_readlock_free, info->pr_next);
  else
    // 否则申请一个新的读锁结构体
    info = malloc(sizeof *info);

  if (info == NULL)
    return NULL;
  // 头插法插入读锁队列
  info->pr_lock_count = 1;
  info->pr_lock = rwlock;
  info->pr_next = THREAD_GETMEM (self, p_readlock_list);
  THREAD_SETMEM (self, p_readlock_list, info);

  return info;
}

/*
 * If the thread owns a read lock over the given pthread_rwlock_t,
 * and this read lock is tracked in the thread's lock list,
 * this function returns a pointer to the info node in that list.
 * It also decrements the lock count within that node, and if
 * it reaches zero, it removes the node from the list.
 * If nothing is found, it returns a null pointer.
 */
// 从队列里删除锁
static pthread_readlock_info *
rwlock_remove_from_list(pthread_descr self, pthread_rwlock_t *rwlock)
{
  pthread_readlock_info **pinfo;

  for (pinfo = &self->p_readlock_list; *pinfo != NULL; pinfo = &(*pinfo)->pr_next)
    {
      if ((*pinfo)->pr_lock == rwlock)
	{
	  pthread_readlock_info *info = *pinfo;
    // 支持嵌套获得锁
	  if (--info->pr_lock_count == 0)
      // 指向info节点的下一个节点，即删除info节点
	    *pinfo = info->pr_next;
	  return info;
	}
    }

  return NULL;
}

/*
 * This function checks whether the conditions are right to place a read lock.
 * It returns 1 if so, otherwise zero. The rwlock's internal lock must be
 * locked upon entry.
 */
// 是否可以获得读锁
static int
rwlock_can_rdlock(pthread_rwlock_t *rwlock, int have_lock_already)
{
  /* Can't readlock; it is write locked. */
  // 有写者，不能再加任何类型的锁
  if (rwlock->__rw_writer != NULL)
    return 0;

  /* Lock prefers readers; get it. */
  // 没有写者，优先读者，则可以加写锁
  if (rwlock->__rw_kind == PTHREAD_RWLOCK_PREFER_READER_NP)
    return 1;

  /* Lock prefers writers, but none are waiting. */
  // 没有写者也不是优先读者，但是优先写者，但是没却有等待写的线程，则可以加读锁
  if (queue_is_empty(&rwlock->__rw_write_waiting))
    return 1;

  /* Writers are waiting, but this thread already has a read lock */
  // 没有写者也不是优先读者，优先写者，并且有等待写的线程，但是当前线程获得了该读写锁的读锁，则可以加锁
  if (have_lock_already)
    return 1;

  /* Writers are waiting, and this is a new lock */
  return 0;
}

/*
 * This function helps support brain-damaged recursive read locking
 * semantics required by Unix 98, while maintaining write priority.
 * This basically determines whether this thread already holds a read lock
 * already. It returns 1 if so, otherwise it returns 0.
 *
 * If the thread has any ``untracked read locks'' then it just assumes
 * that this lock is among them, just to be safe, and returns 1.
 *
 * Also, if it finds the thread's lock in the list, it sets the pointer
 * referenced by pexisting to refer to the list entry.
 *
 * If the thread has no untracked locks, and the lock is not found
 * in its list, then it is added to the list. If this fails,
 * then *pout_of_mem is set to 1.
 */
// 线程是否获得了某个读锁
static int
rwlock_have_already(pthread_descr *pself, pthread_rwlock_t *rwlock,
    pthread_readlock_info **pexisting, int *pout_of_mem)
{
  pthread_readlock_info *existing = NULL;
  int out_of_mem = 0, have_lock_already = 0;
  pthread_descr self = *pself;

  if (rwlock->__rw_kind == PTHREAD_RWLOCK_PREFER_WRITER_NP)
    {
      if (!self)
	*pself = self = thread_self();
      // 该锁是否在当前线程已经获得的读锁队列
      existing = rwlock_is_in_list(self, rwlock);
      // 非空说明已经在线程的读锁队列，返回1
      if (existing != NULL
	  || THREAD_GETMEM (self, p_untracked_readlock_count) > 0)
	have_lock_already = 1;
      else
	{
    // 否则加入读锁队列
	  existing = rwlock_add_to_list(self, rwlock);
    // 加入失败说明没有内存
	  if (existing == NULL)
	    out_of_mem = 1;
	}
    }
  // 是否加入失败
  *pout_of_mem = out_of_mem;
  // 返回读锁的信息
  *pexisting = existing;

  return have_lock_already;
}
// 初始化读写锁
int
__pthread_rwlock_init (pthread_rwlock_t *rwlock,
		       const pthread_rwlockattr_t *attr)
{
  __pthread_init_lock(&rwlock->__rw_lock);
  rwlock->__rw_readers = 0;
  rwlock->__rw_writer = NULL;
  rwlock->__rw_read_waiting = NULL;
  rwlock->__rw_write_waiting = NULL;

  if (attr == NULL)
    {
      rwlock->__rw_kind = PTHREAD_RWLOCK_DEFAULT_NP;
      rwlock->__rw_pshared = PTHREAD_PROCESS_PRIVATE;
    }
  else
    {
      rwlock->__rw_kind = attr->__lockkind;
      rwlock->__rw_pshared = attr->__pshared;
    }

  return 0;
}
strong_alias (__pthread_rwlock_init, pthread_rwlock_init)

// 销毁读写锁
int
__pthread_rwlock_destroy (pthread_rwlock_t *rwlock)
{
  int readers;
  _pthread_descr writer;

  __pthread_lock (&rwlock->__rw_lock, NULL);
  readers = rwlock->__rw_readers;
  writer = rwlock->__rw_writer;
  __pthread_unlock (&rwlock->__rw_lock);
  // 还有读写着则不能销毁
  if (readers > 0 || writer != NULL)
    return EBUSY;

  return 0;
}
strong_alias (__pthread_rwlock_destroy, pthread_rwlock_destroy)
// 获取读锁
int
__pthread_rwlock_rdlock (pthread_rwlock_t *rwlock)
{
  pthread_descr self = NULL;
  pthread_readlock_info *existing;
  int out_of_mem, have_lock_already;
  // 当前线程是否已经获得了该读锁
  have_lock_already = rwlock_have_already(&self, rwlock,
					  &existing, &out_of_mem);

  if (self == NULL)
    self = thread_self ();
  // 循环判断是否可以获取读锁了
  for (;;)
    {
      __pthread_lock (&rwlock->__rw_lock, self);
      // 是否可以获得该读锁
      if (rwlock_can_rdlock(rwlock, have_lock_already))
	break;
      // 不能获得则加入等待获取读锁队列
      enqueue (&rwlock->__rw_read_waiting, self);
      __pthread_unlock (&rwlock->__rw_lock);
      // 挂起
      suspend (self); /* This is not a cancellation point */
    }
  // 可以获得该读锁则锁的读者加一
  ++rwlock->__rw_readers;
  __pthread_unlock (&rwlock->__rw_lock);
  // have_lock_already为1说明exsiting非空,out_of_mem等于1说明exsiting是NULL
  if (have_lock_already || out_of_mem)
    { 
      // 递归获得，加一，即获得了该读锁两次
      if (existing != NULL)
	++existing->pr_lock_count;
      else
	++self->p_untracked_readlock_count;
    }

  return 0;
}
strong_alias (__pthread_rwlock_rdlock, pthread_rwlock_rdlock)

int
__pthread_rwlock_timedrdlock (pthread_rwlock_t *rwlock,
			      const struct timespec *abstime)
{
  pthread_descr self = NULL;
  pthread_readlock_info *existing;
  int out_of_mem, have_lock_already;
  pthread_extricate_if extr;

  if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000)
    return EINVAL;

  have_lock_already = rwlock_have_already(&self, rwlock,
					  &existing, &out_of_mem);

  if (self == NULL)
    self = thread_self ();

  /* Set up extrication interface */
  extr.pu_object = rwlock;
  extr.pu_extricate_func = rwlock_rd_extricate_func;

  /* Register extrication interface */
  __pthread_set_own_extricate_if (self, &extr);

  for (;;)
    {
      __pthread_lock (&rwlock->__rw_lock, self);

      if (rwlock_can_rdlock(rwlock, have_lock_already))
	break;

      enqueue (&rwlock->__rw_read_waiting, self);
      __pthread_unlock (&rwlock->__rw_lock);
      /* This is not a cancellation point */
      if (timedsuspend (self, abstime) == 0)
	{
	  int was_on_queue;

	  __pthread_lock (&rwlock->__rw_lock, self);
	  was_on_queue = remove_from_queue (&rwlock->__rw_read_waiting, self);
	  __pthread_unlock (&rwlock->__rw_lock);

	  if (was_on_queue)
	    {
	      __pthread_set_own_extricate_if (self, 0);
	      return ETIMEDOUT;
	    }

	  /* Eat the outstanding restart() from the signaller */
	  suspend (self);
	}
    }

  __pthread_set_own_extricate_if (self, 0);

  ++rwlock->__rw_readers;
  __pthread_unlock (&rwlock->__rw_lock);

  if (have_lock_already || out_of_mem)
    {
      if (existing != NULL)
	++existing->pr_lock_count;
      else
	++self->p_untracked_readlock_count;
    }

  return 0;
}
strong_alias (__pthread_rwlock_timedrdlock, pthread_rwlock_timedrdlock)
// 非阻塞获得读锁
int
__pthread_rwlock_tryrdlock (pthread_rwlock_t *rwlock)
{
  pthread_descr self = thread_self();
  pthread_readlock_info *existing;
  int out_of_mem, have_lock_already;
  int retval = EBUSY;
  // 是否已经获得了该读锁
  have_lock_already = rwlock_have_already(&self, rwlock,
      &existing, &out_of_mem);

  __pthread_lock (&rwlock->__rw_lock, self);

  /* 0 is passed to here instead of have_lock_already.
     This is to meet Single Unix Spec requirements:
     if writers are waiting, pthread_rwlock_tryrdlock
     does not acquire a read lock, even if the caller has
     one or more read locks already. */
  // 写死0，即使当前线程获得了该读锁，第二次获取的时候，如果有等待写的线程，则优先让写
  if (rwlock_can_rdlock(rwlock, 0))
    {
      ++rwlock->__rw_readers;
      retval = 0;
    }

  __pthread_unlock (&rwlock->__rw_lock);

  if (retval == 0)
    {
      if (have_lock_already || out_of_mem)
	{ 
    // 递归获得了锁，加一
	  if (existing != NULL)
	    ++existing->pr_lock_count;
	  else
	    ++self->p_untracked_readlock_count;
	}
    }

  return retval;
}
strong_alias (__pthread_rwlock_tryrdlock, pthread_rwlock_tryrdlock)

// 加写锁
int
__pthread_rwlock_wrlock (pthread_rwlock_t *rwlock)
{
  pthread_descr self = thread_self ();

  while(1)
    {
      __pthread_lock (&rwlock->__rw_lock, self);
      // 没有读者，也没有写者
      if (rwlock->__rw_readers == 0 && rwlock->__rw_writer == NULL)
	{
    // 设置当前线程为写者
	  rwlock->__rw_writer = self;
	  __pthread_unlock (&rwlock->__rw_lock);
	  return 0;
	}

      /* Suspend ourselves, then try again */
      // 把当前线程插入等待获取写锁队列
      enqueue (&rwlock->__rw_write_waiting, self);
      __pthread_unlock (&rwlock->__rw_lock);
      // 挂起当前线程
      suspend (self); /* This is not a cancellation point */
    }
}
strong_alias (__pthread_rwlock_wrlock, pthread_rwlock_wrlock)

// 限时阻塞式获得写锁
int
__pthread_rwlock_timedwrlock (pthread_rwlock_t *rwlock,
			      const struct timespec *abstime)
{
  pthread_descr self;
  pthread_extricate_if extr;

  if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000)
    return EINVAL;

  self = thread_self ();

  /* Set up extrication interface */
  extr.pu_object = rwlock;
  extr.pu_extricate_func =  rwlock_wr_extricate_func;

  /* Register extrication interface */
  // 设置p_extricate字段
  __pthread_set_own_extricate_if (self, &extr);

  while(1)
    {
      __pthread_lock (&rwlock->__rw_lock, self);
      // 没有读写者
      if (rwlock->__rw_readers == 0 && rwlock->__rw_writer == NULL)
	{
    // 直接设置当前线程为写者，获得写锁
	  rwlock->__rw_writer = self;
    // 清空p_extricate字段
	  __pthread_set_own_extricate_if (self, 0);
	  __pthread_unlock (&rwlock->__rw_lock);
	  return 0;
	}

      /* Suspend ourselves, then try again */
      // 插入等待获得写锁的队列
      enqueue (&rwlock->__rw_write_waiting, self);
      __pthread_unlock (&rwlock->__rw_lock);
      /* This is not a cancellation point */
      // 限时阻塞
      if (timedsuspend (self, abstime) == 0)
	{
	  int was_on_queue;

	  __pthread_lock (&rwlock->__rw_lock, self);
    // 删除成功则返回1，说明存在该节点
	  was_on_queue = remove_from_queue (&rwlock->__rw_write_waiting, self);
	  __pthread_unlock (&rwlock->__rw_lock);

	  if (was_on_queue)
	    {
	      __pthread_set_own_extricate_if (self, 0);
	      return ETIMEDOUT;
	    }

	  /* Eat the outstanding restart() from the signaller */
	  suspend (self);
	}
    }
}
strong_alias (__pthread_rwlock_timedwrlock, pthread_rwlock_timedwrlock)

// 非阻塞式获取写锁
int
__pthread_rwlock_trywrlock (pthread_rwlock_t *rwlock)
{
  int result = EBUSY;

  __pthread_lock (&rwlock->__rw_lock, NULL);
  // 没有读者和写者，才能获取写锁，否则直接返回，不阻塞
  if (rwlock->__rw_readers == 0 && rwlock->__rw_writer == NULL)
    {
      rwlock->__rw_writer = thread_self ();
      result = 0;
    }
  __pthread_unlock (&rwlock->__rw_lock);

  return result;
}
strong_alias (__pthread_rwlock_trywrlock, pthread_rwlock_trywrlock)

// 解锁
int
__pthread_rwlock_unlock (pthread_rwlock_t *rwlock)
{
  pthread_descr torestart;
  pthread_descr th;

  __pthread_lock (&rwlock->__rw_lock, NULL);
  // 有写者
  if (rwlock->__rw_writer != NULL)
    {
      /* Unlocking a write lock.  */
      // 写者不是当前线程，则不能解锁，返回没有权限解锁
      if (rwlock->__rw_writer != thread_self ())
	{
	  __pthread_unlock (&rwlock->__rw_lock);
	  return EPERM;
	}
      // 没有写者或者写者是当前线程，重置写者字段
      rwlock->__rw_writer = NULL;
      /*
       1 有等待获取写锁的线程并且优先让他们获得锁，
       2 没有等待获得写锁的线程
       则唤醒等待读的线程（如果是满足2，则不管是否优先让读，或者等待读锁的队列是否为空）
      */
      if ((rwlock->__rw_kind == PTHREAD_RWLOCK_PREFER_READER_NP
	   && !queue_is_empty(&rwlock->__rw_read_waiting))
	  || (th = dequeue(&rwlock->__rw_write_waiting)) == NULL)
	{
	  /* Restart all waiting readers.  */
	  torestart = rwlock->__rw_read_waiting;
	  rwlock->__rw_read_waiting = NULL;
	  __pthread_unlock (&rwlock->__rw_lock);
	  while ((th = dequeue (&torestart)) != NULL)
	    restart (th);
	}
      else
    // 否则唤醒等待获取写锁的线程
	{
	  /* Restart one waiting writer.  */
	  __pthread_unlock (&rwlock->__rw_lock);
	  restart (th);
	}
    }
  // 没有写者，但是可能有等待写的线程
  else
    {
      /* Unlocking a read lock.  */
      // 也没有读者
      if (rwlock->__rw_readers == 0)
	{
	  __pthread_unlock (&rwlock->__rw_lock);
    // 返回没有权限，因为当前线程没有获得这个锁
	  return EPERM;
	}
      // 没有写者，有读者，读者减一
      --rwlock->__rw_readers;
      // 没有读者了
      if (rwlock->__rw_readers == 0)
	/* Restart one waiting writer, if any.  */
  // 获得等待写的线程队列，准备唤醒
	th = dequeue (&rwlock->__rw_write_waiting);
      else
      // 还有读者，则不需要唤醒等待写的线程
	th = NULL;

      __pthread_unlock (&rwlock->__rw_lock);
      // 需要唤醒写者，则唤醒
      if (th != NULL)
	restart (th);

      /* Recursive lock fixup */

      if (rwlock->__rw_kind == PTHREAD_RWLOCK_PREFER_WRITER_NP)
	{
	  pthread_descr self = thread_self();
    // 从读锁队列中删除 
	  pthread_readlock_info *victim = rwlock_remove_from_list(self, rwlock);
    // 存在队列里
	  if (victim != NULL)
	    {
        // 引用数为0，则插入free队列
	      if (victim->pr_lock_count == 0)
		{
		  victim->pr_next = THREAD_GETMEM (self, p_readlock_free);
		  THREAD_SETMEM (self, p_readlock_free, victim);
		}
	    }
    // 不存在队列
	  else
	    {
        // untrack的锁减一
	      int val = THREAD_GETMEM (self, p_untracked_readlock_count);
	      if (val > 0)
		THREAD_SETMEM (self, p_untracked_readlock_count, val - 1);
	    }
	}
    }

  return 0;
}
strong_alias (__pthread_rwlock_unlock, pthread_rwlock_unlock)


// 初始化读写锁属性
int
pthread_rwlockattr_init (pthread_rwlockattr_t *attr)
{
  attr->__lockkind = 0;
  attr->__pshared = PTHREAD_PROCESS_PRIVATE;

  return 0;
}


int
__pthread_rwlockattr_destroy (pthread_rwlockattr_t *attr)
{
  return 0;
}
strong_alias (__pthread_rwlockattr_destroy, pthread_rwlockattr_destroy)


int
pthread_rwlockattr_getpshared (const pthread_rwlockattr_t *attr, int *pshared)
{
  *pshared = attr->__pshared;
  return 0;
}


int
pthread_rwlockattr_setpshared (pthread_rwlockattr_t *attr, int pshared)
{
  if (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)
    return EINVAL;

  /* For now it is not possible to shared a conditional variable.  */
  if (pshared != PTHREAD_PROCESS_PRIVATE)
    return ENOSYS;

  attr->__pshared = pshared;

  return 0;
}

// 获得读写锁的类型
int
pthread_rwlockattr_getkind_np (const pthread_rwlockattr_t *attr, int *pref)
{
  *pref = attr->__lockkind;
  return 0;
}

// 设置读写锁的类型
int
pthread_rwlockattr_setkind_np (pthread_rwlockattr_t *attr, int pref)
{
  if (pref != PTHREAD_RWLOCK_PREFER_READER_NP
      && pref != PTHREAD_RWLOCK_PREFER_WRITER_NP
      && pref != PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
      && pref != PTHREAD_RWLOCK_DEFAULT_NP)
    return EINVAL;

  attr->__lockkind = pref;

  return 0;
}
