// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Native lv2 (PS3 / PSL1GHT) implementation of the chiaki thread primitives.
// The portable helpers (wait_pred, bool_pred_cond, affinity) stay in thread.c.

#ifdef __PS3__

#include <chiaki/thread.h>
#include <chiaki/time.h>

#include <string.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/systime.h>
#include <lv2/thread.h>

#define PS3_LV2_EBUSY     0x8001000A
#define PS3_LV2_ETIMEDOUT 0x8001000B

#define PS3_THREAD_PRIORITY   1000
#define PS3_THREAD_STACK_SIZE (256 * 1024)

static void ps3_thread_entry(void *param)
{
	ChiakiThread *thread = (ChiakiThread *)param;
	thread->ret = thread->func(thread->arg);
	thread->done = true;
	sysThreadExit(0);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_create(ChiakiThread *thread, ChiakiThreadFunc func, void *arg)
{
	thread->func = func;
	thread->arg = arg;
	thread->ret = NULL;
	thread->done = false;
	thread->thread = 0;
	s32 r = sysThreadCreate(&thread->thread, ps3_thread_entry, thread,
			PS3_THREAD_PRIORITY, PS3_THREAD_STACK_SIZE, THREAD_JOINABLE, (char *)"chiaki");
	if(r != 0)
		return CHIAKI_ERR_THREAD;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_join(ChiakiThread *thread, void **retval)
{
	u64 exit_code;
	if(sysThreadJoin(thread->thread, &exit_code) != 0)
		return CHIAKI_ERR_THREAD;
	if(retval)
		*retval = thread->ret;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_timedjoin(ChiakiThread *thread, void **retval, uint64_t timeout_ms)
{
	// lv2 has no timed join: poll the completion flag, then do a real join.
	uint64_t deadline = chiaki_time_now_monotonic_ms() + timeout_ms;
	while(!thread->done)
	{
		if(chiaki_time_now_monotonic_ms() >= deadline)
			return CHIAKI_ERR_TIMEOUT;
		sysUsleep(1000);
	}
	return chiaki_thread_join(thread, retval);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_set_name(ChiakiThread *thread, const char *name)
{
	(void)thread;
	(void)name;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_init(ChiakiMutex *mutex, bool rec)
{
	sys_mutex_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	attr.attr_recursive = rec ? SYS_MUTEX_ATTR_RECURSIVE : SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strncpy(attr.name, "chiaki", sizeof(attr.name));
	if(sysMutexCreate(&mutex->mutex, &attr) != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_fini(ChiakiMutex *mutex)
{
	if(sysMutexDestroy(mutex->mutex) != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_lock(ChiakiMutex *mutex)
{
	if(sysMutexLock(mutex->mutex, 0) != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_trylock(ChiakiMutex *mutex)
{
	s32 r = sysMutexTryLock(mutex->mutex);
	if(r == (s32)PS3_LV2_EBUSY)
		return CHIAKI_ERR_MUTEX_LOCKED;
	if(r != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_unlock(ChiakiMutex *mutex)
{
	if(sysMutexUnlock(mutex->mutex) != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_init(ChiakiCond *cond)
{
	cond->cond = 0;
	cond->bound_mutex = 0;
	cond->created = false;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_fini(ChiakiCond *cond)
{
	if(cond->created)
	{
		sysCondDestroy(cond->cond);
		cond->created = false;
	}
	return CHIAKI_ERR_SUCCESS;
}

// Called with mutex held.
static ChiakiErrorCode cond_ensure(ChiakiCond *cond, ChiakiMutex *mutex)
{
	if(cond->created)
	{
		// lv2 cannot rebind a condition variable to another mutex.
		return cond->bound_mutex == mutex->mutex ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_INVALID_DATA;
	}
	sys_cond_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.attr_pshared = SYS_COND_ATTR_PSHARED; // 0x200 == "not process shared" on lv2
	strncpy(attr.name, "chiaki", sizeof(attr.name));
	if(sysCondCreate(&cond->cond, mutex->mutex, &attr) != 0)
		return CHIAKI_ERR_UNKNOWN;
	cond->bound_mutex = mutex->mutex;
	__sync_synchronize();
	cond->created = true;
	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode cond_wait_us(ChiakiCond *cond, ChiakiMutex *mutex, u64 timeout_us)
{
	ChiakiErrorCode err = cond_ensure(cond, mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	s32 r = sysCondWait(cond->cond, timeout_us);
	if(r == (s32)PS3_LV2_ETIMEDOUT)
		return CHIAKI_ERR_TIMEOUT;
	if(r != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_wait(ChiakiCond *cond, ChiakiMutex *mutex)
{
	return cond_wait_us(cond, mutex, 0); // 0 = no timeout
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_timedwait(ChiakiCond *cond, ChiakiMutex *mutex, uint64_t timeout_ms)
{
	if(timeout_ms == 0)
		return CHIAKI_ERR_TIMEOUT;
	return cond_wait_us(cond, mutex, timeout_ms * 1000);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_signal(ChiakiCond *cond)
{
	// No kernel object yet means nobody has ever waited, so there is nobody to wake.
	if(!cond->created)
		return CHIAKI_ERR_SUCCESS;
	if(sysCondSignal(cond->cond) != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_broadcast(ChiakiCond *cond)
{
	if(!cond->created)
		return CHIAKI_ERR_SUCCESS;
	if(sysCondBroadcast(cond->cond) != 0)
		return CHIAKI_ERR_UNKNOWN;
	return CHIAKI_ERR_SUCCESS;
}

#endif // __PS3__
