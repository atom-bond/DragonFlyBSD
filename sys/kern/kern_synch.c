/*-
 * Copyright (c) 1982, 1986, 1990, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_synch.c	8.9 (Berkeley) 5/19/95
 * $FreeBSD: src/sys/kern/kern_synch.c,v 1.87.2.6 2002/10/13 07:29:53 kbyanc Exp $
 * $DragonFly: src/sys/kern/kern_synch.c,v 1.91 2008/09/09 04:06:13 dillon Exp $
 */

#include "opt_ktrace.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/signalvar.h>
#include <sys/resourcevar.h>
#include <sys/vmmeter.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/uio.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif
#include <sys/xwait.h>
#include <sys/ktr.h>
#include <sys/serialize.h>

#include <sys/signal2.h>
#include <sys/thread2.h>
#include <sys/spinlock2.h>
#include <sys/mutex2.h>
#include <sys/mplock2.h>

#include <machine/cpu.h>
#include <machine/smp.h>

TAILQ_HEAD(tslpque, thread);

static void sched_setup (void *dummy);
SYSINIT(sched_setup, SI_SUB_KICK_SCHEDULER, SI_ORDER_FIRST, sched_setup, NULL)

int	hogticks;
int	lbolt;
int	lbolt_syncer;
int	sched_quantum;		/* Roundrobin scheduling quantum in ticks. */
int	ncpus;
int	ncpus2, ncpus2_shift, ncpus2_mask;
int	ncpus_fit, ncpus_fit_mask;
int	safepri;
int	tsleep_now_works;

static struct callout loadav_callout;
static struct callout schedcpu_callout;
MALLOC_DEFINE(M_TSLEEP, "tslpque", "tsleep queues");

#define __DEALL(ident)	__DEQUALIFY(void *, ident)

#if !defined(KTR_TSLEEP)
#define KTR_TSLEEP	KTR_ALL
#endif
KTR_INFO_MASTER(tsleep);
KTR_INFO(KTR_TSLEEP, tsleep, tsleep_beg, 0, "tsleep enter %p", sizeof(void *));
KTR_INFO(KTR_TSLEEP, tsleep, tsleep_end, 1, "tsleep exit", 0);
KTR_INFO(KTR_TSLEEP, tsleep, wakeup_beg, 2, "wakeup enter %p", sizeof(void *));
KTR_INFO(KTR_TSLEEP, tsleep, wakeup_end, 3, "wakeup exit", 0);
KTR_INFO(KTR_TSLEEP, tsleep, ilockfail,  4, "interlock failed %p", sizeof(void *));

#define logtsleep1(name)	KTR_LOG(tsleep_ ## name)
#define logtsleep2(name, val)	KTR_LOG(tsleep_ ## name, val)

struct loadavg averunnable =
	{ {0, 0, 0}, FSCALE };	/* load average, of runnable procs */
/*
 * Constants for averages over 1, 5, and 15 minutes
 * when sampling at 5 second intervals.
 */
static fixpt_t cexp[3] = {
	0.9200444146293232 * FSCALE,	/* exp(-1/12) */
	0.9834714538216174 * FSCALE,	/* exp(-1/60) */
	0.9944598480048967 * FSCALE,	/* exp(-1/180) */
};

static void	endtsleep (void *);
static void	loadav (void *arg);
static void	schedcpu (void *arg);
#ifdef SMP
static void	tsleep_wakeup(struct thread *td);
#endif

/*
 * Adjust the scheduler quantum.  The quantum is specified in microseconds.
 * Note that 'tick' is in microseconds per tick.
 */
static int
sysctl_kern_quantum(SYSCTL_HANDLER_ARGS)
{
	int error, new_val;

	new_val = sched_quantum * ustick;
	error = sysctl_handle_int(oidp, &new_val, 0, req);
        if (error != 0 || req->newptr == NULL)
		return (error);
	if (new_val < ustick)
		return (EINVAL);
	sched_quantum = new_val / ustick;
	hogticks = 2 * sched_quantum;
	return (0);
}

SYSCTL_PROC(_kern, OID_AUTO, quantum, CTLTYPE_INT|CTLFLAG_RW,
	0, sizeof sched_quantum, sysctl_kern_quantum, "I", "");

/*
 * If `ccpu' is not equal to `exp(-1/20)' and you still want to use the
 * faster/more-accurate formula, you'll have to estimate CCPU_SHIFT below
 * and possibly adjust FSHIFT in "param.h" so that (FSHIFT >= CCPU_SHIFT).
 *
 * To estimate CCPU_SHIFT for exp(-1/20), the following formula was used:
 *     1 - exp(-1/20) ~= 0.0487 ~= 0.0488 == 1 (fixed pt, *11* bits).
 *
 * If you don't want to bother with the faster/more-accurate formula, you
 * can set CCPU_SHIFT to (FSHIFT + 1) which will use a slower/less-accurate
 * (more general) method of calculating the %age of CPU used by a process.
 *
 * decay 95% of `lwp_pctcpu' in 60 seconds; see CCPU_SHIFT before changing
 */
#define CCPU_SHIFT	11

static fixpt_t ccpu = 0.95122942450071400909 * FSCALE; /* exp(-1/20) */
SYSCTL_INT(_kern, OID_AUTO, ccpu, CTLFLAG_RD, &ccpu, 0, "");

/*
 * kernel uses `FSCALE', userland (SHOULD) use kern.fscale 
 */
int     fscale __unused = FSCALE;	/* exported to systat */
SYSCTL_INT(_kern, OID_AUTO, fscale, CTLFLAG_RD, 0, FSCALE, "");

/*
 * Recompute process priorities, once a second.
 *
 * Since the userland schedulers are typically event oriented, if the
 * estcpu calculation at wakeup() time is not sufficient to make a
 * process runnable relative to other processes in the system we have
 * a 1-second recalc to help out.
 *
 * This code also allows us to store sysclock_t data in the process structure
 * without fear of an overrun, since sysclock_t are guarenteed to hold 
 * several seconds worth of count.
 *
 * WARNING!  callouts can preempt normal threads.  However, they will not
 * preempt a thread holding a spinlock so we *can* safely use spinlocks.
 */
static int schedcpu_stats(struct proc *p, void *data __unused);
static int schedcpu_resource(struct proc *p, void *data __unused);

static void
schedcpu(void *arg)
{
	allproc_scan(schedcpu_stats, NULL);
	allproc_scan(schedcpu_resource, NULL);
	wakeup((caddr_t)&lbolt);
	wakeup((caddr_t)&lbolt_syncer);
	callout_reset(&schedcpu_callout, hz, schedcpu, NULL);
}

/*
 * General process statistics once a second
 */
static int
schedcpu_stats(struct proc *p, void *data __unused)
{
	struct lwp *lp;

	crit_enter();
	p->p_swtime++;
	FOREACH_LWP_IN_PROC(lp, p) {
		if (lp->lwp_stat == LSSLEEP)
			lp->lwp_slptime++;

		/*
		 * Only recalculate processes that are active or have slept
		 * less then 2 seconds.  The schedulers understand this.
		 */
		if (lp->lwp_slptime <= 1) {
			p->p_usched->recalculate(lp);
		} else {
			lp->lwp_pctcpu = (lp->lwp_pctcpu * ccpu) >> FSHIFT;
		}
	}
	crit_exit();
	return(0);
}

/*
 * Resource checks.  XXX break out since ksignal/killproc can block,
 * limiting us to one process killed per second.  There is probably
 * a better way.
 */
static int
schedcpu_resource(struct proc *p, void *data __unused)
{
	u_int64_t ttime;
	struct lwp *lp;

	crit_enter();
	if (p->p_stat == SIDL || 
	    p->p_stat == SZOMB ||
	    p->p_limit == NULL
	) {
		crit_exit();
		return(0);
	}

	ttime = 0;
	FOREACH_LWP_IN_PROC(lp, p) {
		/*
		 * We may have caught an lp in the middle of being
		 * created, lwp_thread can be NULL.
		 */
		if (lp->lwp_thread) {
			ttime += lp->lwp_thread->td_sticks;
			ttime += lp->lwp_thread->td_uticks;
		}
	}

	switch(plimit_testcpulimit(p->p_limit, ttime)) {
	case PLIMIT_TESTCPU_KILL:
		killproc(p, "exceeded maximum CPU limit");
		break;
	case PLIMIT_TESTCPU_XCPU:
		if ((p->p_flag & P_XCPU) == 0) {
			p->p_flag |= P_XCPU;
			ksignal(p, SIGXCPU);
		}
		break;
	default:
		break;
	}
	crit_exit();
	return(0);
}

/*
 * This is only used by ps.  Generate a cpu percentage use over
 * a period of one second.
 *
 * MPSAFE
 */
void
updatepcpu(struct lwp *lp, int cpticks, int ttlticks)
{
	fixpt_t acc;
	int remticks;

	acc = (cpticks << FSHIFT) / ttlticks;
	if (ttlticks >= ESTCPUFREQ) {
		lp->lwp_pctcpu = acc;
	} else {
		remticks = ESTCPUFREQ - ttlticks;
		lp->lwp_pctcpu = (acc * ttlticks + lp->lwp_pctcpu * remticks) /
				ESTCPUFREQ;
	}
}

/*
 * tsleep/wakeup hash table parameters.  Try to find the sweet spot for
 * like addresses being slept on.
 */
#define TABLESIZE	1024
#define LOOKUP(x)	(((intptr_t)(x) >> 6) & (TABLESIZE - 1))

static cpumask_t slpque_cpumasks[TABLESIZE];

/*
 * General scheduler initialization.  We force a reschedule 25 times
 * a second by default.  Note that cpu0 is initialized in early boot and
 * cannot make any high level calls.
 *
 * Each cpu has its own sleep queue.
 */
void
sleep_gdinit(globaldata_t gd)
{
	static struct tslpque slpque_cpu0[TABLESIZE];
	int i;

	if (gd->gd_cpuid == 0) {
		sched_quantum = (hz + 24) / 25;
		hogticks = 2 * sched_quantum;

		gd->gd_tsleep_hash = slpque_cpu0;
	} else {
		gd->gd_tsleep_hash = kmalloc(sizeof(slpque_cpu0), 
					    M_TSLEEP, M_WAITOK | M_ZERO);
	}
	for (i = 0; i < TABLESIZE; ++i)
		TAILQ_INIT(&gd->gd_tsleep_hash[i]);
}

/*
 * This is a dandy function that allows us to interlock tsleep/wakeup
 * operations with unspecified upper level locks, such as lockmgr locks,
 * simply by holding a critical section.  The sequence is:
 *
 *	(acquire upper level lock)
 *	tsleep_interlock(blah)
 *	(release upper level lock)
 *	tsleep(blah, ...)
 *
 * Basically this functions queues us on the tsleep queue without actually
 * descheduling us.  When tsleep() is later called with PINTERLOCK it
 * assumes the thread was already queued, otherwise it queues it there.
 *
 * Thus it is possible to receive the wakeup prior to going to sleep and
 * the race conditions are covered.
 */
static __inline void
_tsleep_interlock(globaldata_t gd, const volatile void *ident, int flags)
{
	thread_t td = gd->gd_curthread;
	int id;

	crit_enter_quick(td);
	if (td->td_flags & TDF_TSLEEPQ) {
		id = LOOKUP(td->td_wchan);
		TAILQ_REMOVE(&gd->gd_tsleep_hash[id], td, td_sleepq);
		if (TAILQ_FIRST(&gd->gd_tsleep_hash[id]) == NULL)
			atomic_clear_int(&slpque_cpumasks[id], gd->gd_cpumask);
	} else {
		td->td_flags |= TDF_TSLEEPQ;
	}
	id = LOOKUP(ident);
	TAILQ_INSERT_TAIL(&gd->gd_tsleep_hash[id], td, td_sleepq);
	atomic_set_int(&slpque_cpumasks[id], gd->gd_cpumask);
	td->td_wchan = ident;
	td->td_wdomain = flags & PDOMAIN_MASK;
	crit_exit_quick(td);
}

void
tsleep_interlock(const volatile void *ident, int flags)
{
	_tsleep_interlock(mycpu, ident, flags);
}

/*
 * Remove thread from sleepq.  Must be called with a critical section held.
 */
static __inline void
_tsleep_remove(thread_t td)
{
	globaldata_t gd = mycpu;
	int id;

	KKASSERT(td->td_gd == gd);
	if (td->td_flags & TDF_TSLEEPQ) {
		td->td_flags &= ~TDF_TSLEEPQ;
		id = LOOKUP(td->td_wchan);
		TAILQ_REMOVE(&gd->gd_tsleep_hash[id], td, td_sleepq);
		if (TAILQ_FIRST(&gd->gd_tsleep_hash[id]) == NULL)
			atomic_clear_int(&slpque_cpumasks[id], gd->gd_cpumask);
		td->td_wchan = NULL;
		td->td_wdomain = 0;
	}
}

void
tsleep_remove(thread_t td)
{
	_tsleep_remove(td);
}

/*
 * This function removes a thread from the tsleep queue and schedules
 * it.  This function may act asynchronously.  The target thread may be
 * sleeping on a different cpu.
 *
 * This function mus be called while in a critical section but if the
 * target thread is sleeping on a different cpu we cannot safely probe
 * td_flags.
 */
static __inline
void
_tsleep_wakeup(struct thread *td)
{
#ifdef SMP
	globaldata_t gd = mycpu;

	if (td->td_gd != gd) {
		lwkt_send_ipiq(td->td_gd, (ipifunc1_t)tsleep_wakeup, td);
		return;
	}
#endif
	_tsleep_remove(td);
	if (td->td_flags & TDF_TSLEEP_DESCHEDULED) {
		td->td_flags &= ~TDF_TSLEEP_DESCHEDULED;
		lwkt_schedule(td);
	}
}

#ifdef SMP
static
void
tsleep_wakeup(struct thread *td)
{
	_tsleep_wakeup(td);
}
#endif


/*
 * General sleep call.  Suspends the current process until a wakeup is
 * performed on the specified identifier.  The process will then be made
 * runnable with the specified priority.  Sleeps at most timo/hz seconds
 * (0 means no timeout).  If flags includes PCATCH flag, signals are checked
 * before and after sleeping, else signals are not checked.  Returns 0 if
 * awakened, EWOULDBLOCK if the timeout expires.  If PCATCH is set and a
 * signal needs to be delivered, ERESTART is returned if the current system
 * call should be restarted if possible, and EINTR is returned if the system
 * call should be interrupted by the signal (return EINTR).
 *
 * Note that if we are a process, we release_curproc() before messing with
 * the LWKT scheduler.
 *
 * During autoconfiguration or after a panic, a sleep will simply
 * lower the priority briefly to allow interrupts, then return.
 */
int
tsleep(const volatile void *ident, int flags, const char *wmesg, int timo)
{
	struct thread *td = curthread;
	struct lwp *lp = td->td_lwp;
	struct proc *p = td->td_proc;		/* may be NULL */
	globaldata_t gd;
	int sig;
	int catch;
	int id;
	int error;
	int oldpri;
	struct callout thandle;

	/*
	 * NOTE: removed KTRPOINT, it could cause races due to blocking
	 * even in stable.  Just scrap it for now.
	 */
	if (tsleep_now_works == 0 || panicstr) {
		/*
		 * After a panic, or before we actually have an operational
		 * softclock, just give interrupts a chance, then just return;
		 *
		 * don't run any other procs or panic below,
		 * in case this is the idle process and already asleep.
		 */
		splz();
		oldpri = td->td_pri & TDPRI_MASK;
		lwkt_setpri_self(safepri);
		lwkt_switch();
		lwkt_setpri_self(oldpri);
		return (0);
	}
	logtsleep2(tsleep_beg, ident);
	gd = td->td_gd;
	KKASSERT(td != &gd->gd_idlethread);	/* you must be kidding! */

	/*
	 * NOTE: all of this occurs on the current cpu, including any
	 * callout-based wakeups, so a critical section is a sufficient
	 * interlock.
	 *
	 * The entire sequence through to where we actually sleep must
	 * run without breaking the critical section.
	 */
	catch = flags & PCATCH;
	error = 0;
	sig = 0;

	crit_enter_quick(td);

	KASSERT(ident != NULL, ("tsleep: no ident"));
	KASSERT(lp == NULL ||
		lp->lwp_stat == LSRUN ||	/* Obvious */
		lp->lwp_stat == LSSTOP,		/* Set in tstop */
		("tsleep %p %s %d",
			ident, wmesg, lp->lwp_stat));

	/*
	 * Setup for the current process (if this is a process). 
	 */
	if (lp) {
		if (catch) {
			/*
			 * Early termination if PCATCH was set and a
			 * signal is pending, interlocked with the
			 * critical section.
			 *
			 * Early termination only occurs when tsleep() is
			 * entered while in a normal LSRUN state.
			 */
			if ((sig = CURSIG(lp)) != 0)
				goto resume;

			/*
			 * Early termination if PCATCH was set and a
			 * mailbox signal was possibly delivered prior to
			 * the system call even being made, in order to
			 * allow the user to interlock without having to
			 * make additional system calls.
			 */
			if (p->p_flag & P_MAILBOX)
				goto resume;

			/*
			 * Causes ksignal to wake us up when.
			 */
			lp->lwp_flag |= LWP_SINTR;
		}
	}

	/*
	 * We interlock the sleep queue if the caller has not already done
	 * it for us.
	 */
	if ((flags & PINTERLOCKED) == 0) {
		id = LOOKUP(ident);
		_tsleep_interlock(gd, ident, flags);
	}

	/*
	 *
	 * If no interlock was set we do an integrated interlock here.
	 * Make sure the current process has been untangled from
	 * the userland scheduler and initialize slptime to start
	 * counting.  We must interlock the sleep queue before doing
	 * this to avoid wakeup/process-ipi races which can occur under
	 * heavy loads.
	 */
	if (lp) {
		p->p_usched->release_curproc(lp);
		lp->lwp_slptime = 0;
	}

	/*
	 * If the interlocked flag is set but our cpu bit in the slpqueue
	 * is no longer set, then a wakeup was processed inbetween the
	 * tsleep_interlock() (ours or the callers), and here.  This can
	 * occur under numerous circumstances including when we release the
	 * current process.
	 *
	 * Extreme loads can cause the sending of an IPI (e.g. wakeup()'s)
	 * to process incoming IPIs, thus draining incoming wakeups.
	 */
	if ((td->td_flags & TDF_TSLEEPQ) == 0) {
		logtsleep2(ilockfail, ident);
		goto resume;
	}

	/*
	 * scheduling is blocked while in a critical section.  Coincide
	 * the descheduled-by-tsleep flag with the descheduling of the
	 * lwkt.
	 */
	lwkt_deschedule_self(td);
	td->td_flags |= TDF_TSLEEP_DESCHEDULED;
	td->td_wmesg = wmesg;

	/*
	 * Setup the timeout, if any
	 */
	if (timo) {
		callout_init(&thandle);
		callout_reset(&thandle, timo, endtsleep, td);
	}

	/*
	 * Beddy bye bye.
	 */
	if (lp) {
		/*
		 * Ok, we are sleeping.  Place us in the SSLEEP state.
		 */
		KKASSERT((lp->lwp_flag & LWP_ONRUNQ) == 0);
		/*
		 * tstop() sets LSSTOP, so don't fiddle with that.
		 */
		if (lp->lwp_stat != LSSTOP)
			lp->lwp_stat = LSSLEEP;
		lp->lwp_ru.ru_nvcsw++;
		lwkt_switch();

		/*
		 * And when we are woken up, put us back in LSRUN.  If we
		 * slept for over a second, recalculate our estcpu.
		 */
		lp->lwp_stat = LSRUN;
		if (lp->lwp_slptime)
			p->p_usched->recalculate(lp);
		lp->lwp_slptime = 0;
	} else {
		lwkt_switch();
	}

	/* 
	 * Make sure we haven't switched cpus while we were asleep.  It's
	 * not supposed to happen.  Cleanup our temporary flags.
	 */
	KKASSERT(gd == td->td_gd);

	/*
	 * Cleanup the timeout.
	 */
	if (timo) {
		if (td->td_flags & TDF_TIMEOUT) {
			td->td_flags &= ~TDF_TIMEOUT;
			error = EWOULDBLOCK;
		} else {
			callout_stop(&thandle);
		}
	}

	/*
	 * Make sure we have been removed from the sleepq.  This should
	 * have been done for us already.
	 */
	_tsleep_remove(td);
	td->td_wmesg = NULL;
	if (td->td_flags & TDF_TSLEEP_DESCHEDULED) {
		td->td_flags &= ~TDF_TSLEEP_DESCHEDULED;
		kprintf("td %p (%s) unexpectedly rescheduled\n",
			td, td->td_comm);
	}

	/*
	 * Figure out the correct error return.  If interrupted by a
	 * signal we want to return EINTR or ERESTART.  
	 *
	 * If P_MAILBOX is set no automatic system call restart occurs
	 * and we return EINTR.  P_MAILBOX is meant to be used as an
	 * interlock, the user must poll it prior to any system call
	 * that it wishes to interlock a mailbox signal against since
	 * the flag is cleared on *any* system call that sleeps.
	 */
resume:
	if (p) {
		if (catch && error == 0) {
			if ((p->p_flag & P_MAILBOX) && sig == 0) {
				error = EINTR;
			} else if (sig != 0 || (sig = CURSIG(lp))) {
				if (SIGISMEMBER(p->p_sigacts->ps_sigintr, sig))
					error = EINTR;
				else
					error = ERESTART;
			}
		}
		lp->lwp_flag &= ~(LWP_BREAKTSLEEP | LWP_SINTR);
		p->p_flag &= ~P_MAILBOX;
	}
	logtsleep1(tsleep_end);
	crit_exit_quick(td);
	return (error);
}

/*
 * Interlocked spinlock sleep.  An exclusively held spinlock must
 * be passed to ssleep().  The function will atomically release the
 * spinlock and tsleep on the ident, then reacquire the spinlock and
 * return.
 *
 * This routine is fairly important along the critical path, so optimize it
 * heavily.
 */
int
ssleep(const volatile void *ident, struct spinlock *spin, int flags,
       const char *wmesg, int timo)
{
	globaldata_t gd = mycpu;
	int error;

	_tsleep_interlock(gd, ident, flags);
	spin_unlock_wr_quick(gd, spin);
	error = tsleep(ident, flags | PINTERLOCKED, wmesg, timo);
	spin_lock_wr_quick(gd, spin);

	return (error);
}

int
lksleep(const volatile void *ident, struct lock *lock, int flags,
	const char *wmesg, int timo)
{
	globaldata_t gd = mycpu;
	int error;

	_tsleep_interlock(gd, ident, flags);
	lockmgr(lock, LK_RELEASE);
	error = tsleep(ident, flags | PINTERLOCKED, wmesg, timo);
	lockmgr(lock, LK_EXCLUSIVE);

	return (error);
}

/*
 * Interlocked mutex sleep.  An exclusively held mutex must be passed
 * to mtxsleep().  The function will atomically release the mutex
 * and tsleep on the ident, then reacquire the mutex and return.
 */
int
mtxsleep(const volatile void *ident, struct mtx *mtx, int flags,
	 const char *wmesg, int timo)
{
	globaldata_t gd = mycpu;
	int error;

	_tsleep_interlock(gd, ident, flags);
	mtx_unlock(mtx);
	error = tsleep(ident, flags | PINTERLOCKED, wmesg, timo);
	mtx_lock_ex_quick(mtx, wmesg);

	return (error);
}

/*
 * Interlocked serializer sleep.  An exclusively held serializer must
 * be passed to zsleep().  The function will atomically release
 * the serializer and tsleep on the ident, then reacquire the serializer
 * and return.
 */
int
zsleep(const volatile void *ident, struct lwkt_serialize *slz, int flags,
       const char *wmesg, int timo)
{
	globaldata_t gd = mycpu;
	int ret;

	ASSERT_SERIALIZED(slz);

	_tsleep_interlock(gd, ident, flags);
	lwkt_serialize_exit(slz);
	ret = tsleep(ident, flags | PINTERLOCKED, wmesg, timo);
	lwkt_serialize_enter(slz);

	return ret;
}

/*
 * Directly block on the LWKT thread by descheduling it.  This
 * is much faster then tsleep(), but the only legal way to wake
 * us up is to directly schedule the thread.
 *
 * Setting TDF_SINTR will cause new signals to directly schedule us.
 *
 * This routine must be called while in a critical section.
 */
int
lwkt_sleep(const char *wmesg, int flags)
{
	thread_t td = curthread;
	int sig;

	if ((flags & PCATCH) == 0 || td->td_lwp == NULL) {
		td->td_flags |= TDF_BLOCKED;
		td->td_wmesg = wmesg;
		lwkt_deschedule_self(td);
		lwkt_switch();
		td->td_wmesg = NULL;
		td->td_flags &= ~TDF_BLOCKED;
		return(0);
	}
	if ((sig = CURSIG(td->td_lwp)) != 0) {
		if (SIGISMEMBER(td->td_proc->p_sigacts->ps_sigintr, sig))
			return(EINTR);
		else
			return(ERESTART);
			
	}
	td->td_flags |= TDF_BLOCKED | TDF_SINTR;
	td->td_wmesg = wmesg;
	lwkt_deschedule_self(td);
	lwkt_switch();
	td->td_flags &= ~(TDF_BLOCKED | TDF_SINTR);
	td->td_wmesg = NULL;
	return(0);
}

/*
 * Implement the timeout for tsleep.
 *
 * We set LWP_BREAKTSLEEP to indicate that an event has occured, but
 * we only call setrunnable if the process is not stopped.
 *
 * This type of callout timeout is scheduled on the same cpu the process
 * is sleeping on.  Also, at the moment, the MP lock is held.
 */
static void
endtsleep(void *arg)
{
	thread_t td = arg;
	struct lwp *lp;

	ASSERT_MP_LOCK_HELD(curthread);
	crit_enter();

	/*
	 * cpu interlock.  Thread flags are only manipulated on
	 * the cpu owning the thread.  proc flags are only manipulated
	 * by the older of the MP lock.  We have both.
	 */
	if (td->td_flags & TDF_TSLEEP_DESCHEDULED) {
		td->td_flags |= TDF_TIMEOUT;

		if ((lp = td->td_lwp) != NULL) {
			lp->lwp_flag |= LWP_BREAKTSLEEP;
			if (lp->lwp_proc->p_stat != SSTOP)
				setrunnable(lp);
		} else {
			_tsleep_wakeup(td);
		}
	}
	crit_exit();
}

/*
 * Make all processes sleeping on the specified identifier runnable.
 * count may be zero or one only.
 *
 * The domain encodes the sleep/wakeup domain AND the first cpu to check
 * (which is always the current cpu).  As we iterate across cpus
 *
 * This call may run without the MP lock held.  We can only manipulate thread
 * state on the cpu owning the thread.  We CANNOT manipulate process state
 * at all.
 *
 * _wakeup() can be passed to an IPI so we can't use (const volatile
 * void *ident).
 */
static void
_wakeup(void *ident, int domain)
{
	struct tslpque *qp;
	struct thread *td;
	struct thread *ntd;
	globaldata_t gd;
#ifdef SMP
	cpumask_t mask;
#endif
	int id;

	crit_enter();
	logtsleep2(wakeup_beg, ident);
	gd = mycpu;
	id = LOOKUP(ident);
	qp = &gd->gd_tsleep_hash[id];
restart:
	for (td = TAILQ_FIRST(qp); td != NULL; td = ntd) {
		ntd = TAILQ_NEXT(td, td_sleepq);
		if (td->td_wchan == ident && 
		    td->td_wdomain == (domain & PDOMAIN_MASK)
		) {
			KKASSERT(td->td_gd == gd);
			_tsleep_remove(td);
			if (td->td_flags & TDF_TSLEEP_DESCHEDULED) {
				td->td_flags &= ~TDF_TSLEEP_DESCHEDULED;
				lwkt_schedule(td);
				if (domain & PWAKEUP_ONE)
					goto done;
			}
			goto restart;
		}
	}

#ifdef SMP
	/*
	 * We finished checking the current cpu but there still may be
	 * more work to do.  Either wakeup_one was requested and no matching
	 * thread was found, or a normal wakeup was requested and we have
	 * to continue checking cpus.
	 *
	 * It should be noted that this scheme is actually less expensive then
	 * the old scheme when waking up multiple threads, since we send 
	 * only one IPI message per target candidate which may then schedule
	 * multiple threads.  Before we could have wound up sending an IPI
	 * message for each thread on the target cpu (!= current cpu) that
	 * needed to be woken up.
	 *
	 * NOTE: Wakeups occuring on remote cpus are asynchronous.  This
	 * should be ok since we are passing idents in the IPI rather then
	 * thread pointers.
	 */
	if ((domain & PWAKEUP_MYCPU) == 0 &&
	    (mask = slpque_cpumasks[id] & gd->gd_other_cpus) != 0) {
		lwkt_send_ipiq2_mask(mask, _wakeup, ident,
				     domain | PWAKEUP_MYCPU);
	}
#endif
done:
	logtsleep1(wakeup_end);
	crit_exit();
}

/*
 * Wakeup all threads tsleep()ing on the specified ident, on all cpus
 */
void
wakeup(const volatile void *ident)
{
    _wakeup(__DEALL(ident), PWAKEUP_ENCODE(0, mycpu->gd_cpuid));
}

/*
 * Wakeup one thread tsleep()ing on the specified ident, on any cpu.
 */
void
wakeup_one(const volatile void *ident)
{
    /* XXX potentially round-robin the first responding cpu */
    _wakeup(__DEALL(ident), PWAKEUP_ENCODE(0, mycpu->gd_cpuid) | PWAKEUP_ONE);
}

/*
 * Wakeup threads tsleep()ing on the specified ident on the current cpu
 * only.
 */
void
wakeup_mycpu(const volatile void *ident)
{
    _wakeup(__DEALL(ident), PWAKEUP_MYCPU);
}

/*
 * Wakeup one thread tsleep()ing on the specified ident on the current cpu
 * only.
 */
void
wakeup_mycpu_one(const volatile void *ident)
{
    /* XXX potentially round-robin the first responding cpu */
    _wakeup(__DEALL(ident), PWAKEUP_MYCPU|PWAKEUP_ONE);
}

/*
 * Wakeup all thread tsleep()ing on the specified ident on the specified cpu
 * only.
 */
void
wakeup_oncpu(globaldata_t gd, const volatile void *ident)
{
#ifdef SMP
    if (gd == mycpu) {
	_wakeup(__DEALL(ident), PWAKEUP_MYCPU);
    } else {
	lwkt_send_ipiq2(gd, _wakeup, __DEALL(ident), PWAKEUP_MYCPU);
    }
#else
    _wakeup(__DEALL(ident), PWAKEUP_MYCPU);
#endif
}

/*
 * Wakeup one thread tsleep()ing on the specified ident on the specified cpu
 * only.
 */
void
wakeup_oncpu_one(globaldata_t gd, const volatile void *ident)
{
#ifdef SMP
    if (gd == mycpu) {
	_wakeup(__DEALL(ident), PWAKEUP_MYCPU | PWAKEUP_ONE);
    } else {
	lwkt_send_ipiq2(gd, _wakeup, __DEALL(ident),
			PWAKEUP_MYCPU | PWAKEUP_ONE);
    }
#else
    _wakeup(__DEALL(ident), PWAKEUP_MYCPU | PWAKEUP_ONE);
#endif
}

/*
 * Wakeup all threads waiting on the specified ident that slept using
 * the specified domain, on all cpus.
 */
void
wakeup_domain(const volatile void *ident, int domain)
{
    _wakeup(__DEALL(ident), PWAKEUP_ENCODE(domain, mycpu->gd_cpuid));
}

/*
 * Wakeup one thread waiting on the specified ident that slept using
 * the specified  domain, on any cpu.
 */
void
wakeup_domain_one(const volatile void *ident, int domain)
{
    /* XXX potentially round-robin the first responding cpu */
    _wakeup(__DEALL(ident),
	    PWAKEUP_ENCODE(domain, mycpu->gd_cpuid) | PWAKEUP_ONE);
}

/*
 * setrunnable()
 *
 * Make a process runnable.  The MP lock must be held on call.  This only
 * has an effect if we are in SSLEEP.  We only break out of the
 * tsleep if LWP_BREAKTSLEEP is set, otherwise we just fix-up the state.
 *
 * NOTE: With the MP lock held we can only safely manipulate the process
 * structure.  We cannot safely manipulate the thread structure.
 */
void
setrunnable(struct lwp *lp)
{
	crit_enter();
	ASSERT_MP_LOCK_HELD(curthread);
	if (lp->lwp_stat == LSSTOP)
		lp->lwp_stat = LSSLEEP;
	if (lp->lwp_stat == LSSLEEP && (lp->lwp_flag & LWP_BREAKTSLEEP))
		_tsleep_wakeup(lp->lwp_thread);
	crit_exit();
}

/*
 * The process is stopped due to some condition, usually because p_stat is
 * set to SSTOP, but also possibly due to being traced.  
 *
 * NOTE!  If the caller sets SSTOP, the caller must also clear P_WAITED
 * because the parent may check the child's status before the child actually
 * gets to this routine.
 *
 * This routine is called with the current lwp only, typically just
 * before returning to userland.
 *
 * Setting LWP_BREAKTSLEEP before entering the tsleep will cause a passive
 * SIGCONT to break out of the tsleep.
 */
void
tstop(void)
{
	struct lwp *lp = curthread->td_lwp;
	struct proc *p = lp->lwp_proc;

	crit_enter();
	/*
	 * If LWP_WSTOP is set, we were sleeping
	 * while our process was stopped.  At this point
	 * we were already counted as stopped.
	 */
	if ((lp->lwp_flag & LWP_WSTOP) == 0) {
		/*
		 * If we're the last thread to stop, signal
		 * our parent.
		 */
		p->p_nstopped++;
		lp->lwp_flag |= LWP_WSTOP;
		wakeup(&p->p_nstopped);
		if (p->p_nstopped == p->p_nthreads) {
			p->p_flag &= ~P_WAITED;
			wakeup(p->p_pptr);
			if ((p->p_pptr->p_sigacts->ps_flag & PS_NOCLDSTOP) == 0)
				ksignal(p->p_pptr, SIGCHLD);
		}
	}
	while (p->p_stat == SSTOP) {
		lp->lwp_flag |= LWP_BREAKTSLEEP;
		lp->lwp_stat = LSSTOP;
		tsleep(p, 0, "stop", 0);
	}
	p->p_nstopped--;
	lp->lwp_flag &= ~LWP_WSTOP;
	crit_exit();
}

/*
 * Yield / synchronous reschedule.  This is a bit tricky because the trap
 * code might have set a lazy release on the switch function.   Setting
 * P_PASSIVE_ACQ will ensure that the lazy release executes when we call
 * switch, and that we are given a greater chance of affinity with our
 * current cpu.
 *
 * We call lwkt_setpri_self() to rotate our thread to the end of the lwkt
 * run queue.  lwkt_switch() will also execute any assigned passive release
 * (which usually calls release_curproc()), allowing a same/higher priority
 * process to be designated as the current process.  
 *
 * While it is possible for a lower priority process to be designated,
 * it's call to lwkt_maybe_switch() in acquire_curproc() will likely
 * round-robin back to us and we will be able to re-acquire the current
 * process designation.
 *
 * MPSAFE
 */
void
uio_yield(void)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	lwkt_setpri_self(td->td_pri & TDPRI_MASK);
	if (p) {
		p->p_flag |= P_PASSIVE_ACQ;
		lwkt_switch();
		p->p_flag &= ~P_PASSIVE_ACQ;
	} else {
		lwkt_switch();
	}
}

/*
 * Compute a tenex style load average of a quantity on
 * 1, 5 and 15 minute intervals.
 */
static int loadav_count_runnable(struct lwp *p, void *data);

static void
loadav(void *arg)
{
	struct loadavg *avg;
	int i, nrun;

	nrun = 0;
	alllwp_scan(loadav_count_runnable, &nrun);
	avg = &averunnable;
	for (i = 0; i < 3; i++) {
		avg->ldavg[i] = (cexp[i] * avg->ldavg[i] +
		    nrun * FSCALE * (FSCALE - cexp[i])) >> FSHIFT;
	}

	/*
	 * Schedule the next update to occur after 5 seconds, but add a
	 * random variation to avoid synchronisation with processes that
	 * run at regular intervals.
	 */
	callout_reset(&loadav_callout, hz * 4 + (int)(krandom() % (hz * 2 + 1)),
		      loadav, NULL);
}

static int
loadav_count_runnable(struct lwp *lp, void *data)
{
	int *nrunp = data;
	thread_t td;

	switch (lp->lwp_stat) {
	case LSRUN:
		if ((td = lp->lwp_thread) == NULL)
			break;
		if (td->td_flags & TDF_BLOCKED)
			break;
		++*nrunp;
		break;
	default:
		break;
	}
	return(0);
}

/* ARGSUSED */
static void
sched_setup(void *dummy)
{
	callout_init(&loadav_callout);
	callout_init(&schedcpu_callout);

	/* Kick off timeout driven events by calling first time. */
	schedcpu(NULL);
	loadav(NULL);
}

