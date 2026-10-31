// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 Sultan Alsawaf <sultan@kerneltoast.com>.
 * Copyright (C) 2026 genie1997 <genie1997.dev@gmail.com>.
 */

/* Capacity Aware Superset Scheduler placement and a busy-window clock floor on the vendor hooks */

#include <linux/cpufreq.h>
#include <linux/cpuidle.h>
#include <linux/module.h>
#include <linux/sched/clock.h>
#include <trace/events/sched.h>
#include <trace/hooks/sched.h>

#include "kernel/sched/sched.h"

static bool enable = true;
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "Place CFS tasks with CASS");

static unsigned int eas_gate = 200;
module_param(eas_gate, uint, 0644);
MODULE_PARM_DESC(eas_gate, "Unboosted wakeups below this utilization keep energy aware placement");

static int newborn_util_max;
module_param(newborn_util_max, int, 0644);
MODULE_PARM_DESC(newborn_util_max, "Most utilization a new task starts with, -1 = the kernel's");

static unsigned int rt_mode = 2;
module_param_named(rt, rt_mode, uint, 0644);
MODULE_PARM_DESC(rt, "RT task placement: 0 kernel, 1 CASS, 2 CASS preferring the smaller idle CPU");

static unsigned int busy_window_ms = 16;
module_param(busy_window_ms, uint, 0644);
MODULE_PARM_DESC(busy_window_ms, "Clock floor from a cluster's busy share of a window, 0 off");

static unsigned int fork_busy_pct = 75;
module_param(fork_busy_pct, uint, 0644);
MODULE_PARM_DESC(fork_busy_pct,
		 "Forks while the CPUs stayed this busy (% of one CPU) for two windows start as usual, 0 off");

struct cass_busy {
	u64 win_start;
	u64 busy_since;
	u64 cur;
	u64 prev;
	u64 prev2;
};

static DEFINE_PER_CPU(struct cass_busy, cass_busy);

static inline u64 cass_window_start(u64 now, u64 win)
{
	u64 rem;

	div64_u64_rem(now, win, &rem);
	return now - rem;
}

/* Account busy time up to @now; @prev keeps the busy time of the last complete window */
static void cass_busy_update(struct cass_busy *b, u64 now, u64 win)
{
	u64 start = b->win_start;
	u64 end = start + win;

	if (!start || now - start >= 2 * win) {
		/* A whole window passed with no event, so it was all busy or all idle */
		b->prev = b->busy_since ? win : 0;
		b->prev2 = b->prev;
		b->win_start = cass_window_start(now, win);
		b->cur = b->busy_since ? now - b->win_start : 0;
	} else if (now >= end) {
		if (b->busy_since)
			b->cur += end - max(b->busy_since, start);
		b->prev2 = b->prev;
		b->prev = min(b->cur, win);
		b->win_start = end;
		b->cur = b->busy_since ? now - end : 0;
	} else if (b->busy_since) {
		b->cur += now - max(b->busy_since, start);
	}

	if (b->busy_since)
		b->busy_since = now;
}

static void cass_sched_switch(void *unused, bool preempt, struct task_struct *prev,
			      struct task_struct *next, unsigned int prev_state)
{
	struct cass_busy *b = this_cpu_ptr(&cass_busy);
	u64 win = (u64)READ_ONCE(busy_window_ms) * NSEC_PER_MSEC;
	u64 now;

	if (!win || is_idle_task(prev) == is_idle_task(next))
		return;

	now = sched_clock();
	cass_busy_update(b, now, win);
	b->busy_since = is_idle_task(next) ? 0 : now;
}

/* Busy time of @cpu in the last two complete windows, read from another CPU without a lock */
static void cass_busy_windows(int cpu, u64 now, u64 win, u64 *last, u64 *before)
{
	struct cass_busy *b = per_cpu_ptr(&cass_busy, cpu);
	u64 start = READ_ONCE(b->win_start);
	u64 since = READ_ONCE(b->busy_since);
	u64 cur = READ_ONCE(b->cur);
	u64 prev = min(READ_ONCE(b->prev), win);

	if (!start || (now >= start && now - start >= 2 * win)) {
		*last = since ? win : 0;
		*before = *last;
	} else if (now >= start + win) {
		*last = min(cur + (since ? start + win - max(since, start) : 0), win);
		*before = prev;
	} else {
		*last = prev;
		*before = min(READ_ONCE(b->prev2), win);
	}
}

/* True when the CPUs together stayed busy for two whole windows: a stream of work, not a burst */
static bool cass_sustained(void)
{
	unsigned int pct = READ_ONCE(fork_busy_pct);
	u64 win = (u64)READ_ONCE(busy_window_ms) * NSEC_PER_MSEC;
	u64 now, last = 0, before = 0, l, b4;
	int i;

	if (!pct || !win)
		return false;

	now = sched_clock();
	for_each_online_cpu(i) {
		cass_busy_windows(i, now, win, &l, &b4);
		last += l;
		before += b4;
	}

	return last * 100 >= win * pct && before * 100 >= win * pct;
}

struct cass_cpu_cand {
	int cpu;
	unsigned int exit_lat;
	unsigned long cap;
	unsigned long cap_max;
	unsigned long cap_no_therm;
	unsigned long cap_orig;
	unsigned long eff_util;
	unsigned long hard_util;
	unsigned long util;
};

static inline unsigned long cass_task_util(struct task_struct *p)
{
	return max(READ_ONCE(p->se.avg.util_avg),
		   READ_ONCE(p->se.avg.util_est) & ~UTIL_AVG_UNCHANGED);
}

static inline bool cass_fits(unsigned long util, int cpu)
{
	return util * 1280 < cpu_rq(cpu)->cpu_capacity * 1024;
}

static inline bool cass_idle_cpu(int cpu, struct task_struct *p)
{
	struct rq *rq = cpu_rq(cpu);

	if (available_idle_cpu(cpu))
		return true;

	/* A runqueue with only SCHED_IDLE tasks counts as idle for a normal task */
	return rq->nr_running && rq->nr_running == rq->cfs.idle_h_nr_running &&
	       !task_has_idle_policy(p);
}

static inline unsigned long cass_min_freq_cap(int cpu, unsigned long cap_orig)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	if (!policy || !policy->cpuinfo.max_freq)
		return 0;

	return READ_ONCE(policy->min) * cap_orig / policy->cpuinfo.max_freq;
}

static void cass_cpu_util(struct cass_cpu_cand *c, int this_cpu, bool sync)
{
	struct rq *rq = cpu_rq(c->cpu);
	struct cfs_rq *cfs_rq = &rq->cfs;
	unsigned long est;

	c->util = READ_ONCE(cfs_rq->avg.util_avg);
	if (sched_feat(UTIL_EST)) {
		est = READ_ONCE(cfs_rq->avg.util_est);
		if (est > c->util) {
			/* Don't deduct current's util from estimated util */
			sync = false;
			c->util = est;
		}
	}

	/* A sync wake deducts current's util unless current is RT, which has no PELT */
	if (sync && c->cpu == this_cpu && !rt_task(current))
		c->util -= min(c->util, READ_ONCE(current->se.avg.util_avg));

	c->hard_util = cpu_util_rt(rq) + cpu_util_dl(rq) + cpu_util_irq(rq);
	c->cap = c->cap_max - min(c->hard_util, c->cap_max - 1);
	c->cap_no_therm = c->cap_orig - min(c->hard_util, c->cap_orig - 1);
}

/* The prime CPU is the last one when its capacity differs from the CPU before it */
static inline bool cass_prime_cpu(const struct cass_cpu_cand *c)
{
	return c->cpu == nr_cpu_ids - 1 &&
	       arch_scale_cpu_capacity(nr_cpu_ids - 2) != SCHED_CAPACITY_SCALE;
}

/* Returns true if @a is a better CPU than @b */
static bool cass_cpu_better(const struct cass_cpu_cand *a,
			    const struct cass_cpu_cand *b, unsigned long p_util,
			    int this_cpu, int prev_cpu, bool sync, bool rt)
{
#define cass_cmp(a, b) ({ res = (a) - (b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;

	/* Prefer the CPU that's not overloaded */
	if (cass_cmp(b->eff_util / b->cap_max, a->eff_util / a->cap_max))
		goto done;

	/* Prefer the CPU that's less overloaded if they're both overloaded */
	if (b->eff_util > b->cap_max && a->eff_util > a->cap_max &&
	    cass_cmp(b->eff_util * SCHED_CAPACITY_SCALE / b->cap_max,
		     a->eff_util * SCHED_CAPACITY_SCALE / a->cap_max))
		goto done;

	/* Prefer the CPU that fits the task */
	if (cass_cmp(cass_fits(p_util, a->cpu), cass_fits(p_util, b->cpu)))
		goto done;

	/* Prefer the CPU that isn't the single fastest one in the system */
	if (cass_cmp(cass_prime_cpu(b), cass_prime_cpu(a)))
		goto done;

	/* Prefer the smaller idle CPU for a task that fits it, sync wakes aside; RT on request */
	if ((!rt || READ_ONCE(rt_mode) == 2) && a->exit_lat && b->exit_lat &&
	    cass_fits(p_util, a->cpu) &&
	    !(sync && (a->cpu == this_cpu || b->cpu == this_cpu)) &&
	    cass_cmp(b->cap_orig, a->cap_orig))
		goto done;

	/* Prefer the CPU with lower relative utilization */
	if (cass_cmp(b->util, a->util))
		goto done;

	/* Prefer the CPU that is idle (only relevant for uclamped tasks) */
	if (cass_cmp(!!a->exit_lat, !!b->exit_lat))
		goto done;

	/* Prefer the current CPU for sync wakes */
	if (sync && (cass_eq(a->cpu, this_cpu) || !cass_cmp(b->cpu, this_cpu)))
		goto done;

	/* Prefer the CPU with higher capacity */
	if (cass_cmp(a->cap, b->cap))
		goto done;

	/* Prefer the CPU with lower idle exit latency */
	if (cass_cmp(b->exit_lat, a->exit_lat))
		goto done;

	/* Prefer the previous CPU; all CPUs here share one LLC, so no cache tiebreak */
	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

done:
	/* @a is a better CPU than @b if @res is positive */
	return res > 0;
#undef cass_cmp
#undef cass_eq
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync,
			 unsigned long p_util, bool rt)
{
	/* Initialize @best such that @best always has a valid CPU at the end */
	struct cass_cpu_cand cands[2], *best = cands;
	int this_cpu = raw_smp_processor_id();
	unsigned long uc_min;
	bool has_idle = false;
	int cidx = 0, cpu;

	uc_min = uclamp_eff_value(p, UCLAMP_MIN);

	/* Not preemptible here, so idle_get_state() needs no rcu_read_lock() */
	for_each_cpu_and(cpu, p->cpus_ptr, cpu_active_mask) {
		struct cass_cpu_cand *curr = &cands[cidx];
		struct cpuidle_state *idle_state;
		struct rq *rq = cpu_rq(cpu);
		unsigned long hw;

		curr->cap_orig = arch_scale_cpu_capacity(cpu);
		hw = READ_ONCE(rq->avg_hw.load_avg);
		curr->cap_max = curr->cap_orig - min(hw, curr->cap_orig - 1);

		/* Prefer the CPU that more closely meets the uclamp minimum */
		if (curr->cap_max < uc_min && curr->cap_max < best->cap_max)
			continue;

		/* For sync wakes the waker's CPU counts as idle if current is its only task */
		curr->cpu = cpu;
		if ((sync && cpu == this_cpu && rq->nr_running == 1) ||
		    cass_idle_cpu(cpu, p)) {
			/* Idle CPUs win unless @p is boosted past this floor or it is the prime */
			if (!has_idle &&
			    uc_min <= cass_min_freq_cap(cpu, curr->cap_orig) &&
			    !cass_prime_cpu(curr)) {
				best = curr;
				has_idle = true;
			}

			/* Nonzero exit latency indicates this CPU is idle */
			curr->exit_lat = 1;
			idle_state = idle_get_state(rq);
			if (idle_state)
				curr->exit_lat += idle_state->exit_latency;
		} else {
			/* Skip non-idle CPUs if there's an idle candidate */
			if (has_idle)
				continue;

			curr->exit_lat = 0;
		}

		cass_cpu_util(curr, this_cpu, sync);

		/* Add @p's util to a CPU that isn't its own to see the load it would have */
		if (cpu != task_cpu(p))
			curr->util += p_util;

		/* What the CPU governor sees; tells whether a throttled CPU is overloaded */
		curr->eff_util = max(curr->util + curr->hard_util, uc_min);

		/* Clamp the utilization to the minimum performance threshold */
		if (curr->util < uc_min)
			curr->util = uc_min;

		/* Relative util without thermal pressure: throttled CPUs get no higher P-states */
		curr->util = curr->util * SCHED_CAPACITY_SCALE / curr->cap_no_therm;

		if (best == curr ||
		    cass_cpu_better(curr, best, p_util, this_cpu, prev_cpu, sync, rt)) {
			best = curr;
			cidx ^= 1;
		}
	}

	return best->cpu;
}

static void cass_select_task_rq_fair(void *unused, struct task_struct *p,
				     int prev_cpu, int sd_flag, int wake_flags,
				     int *new_cpu)
{
	bool sync;

	if (!READ_ONCE(enable))
		return;

	/* Don't balance on exec since we don't know what @p will look like */
	if (sd_flag & SD_BALANCE_EXEC) {
		*new_cpu = prev_cpu;
		return;
	}

	if ((wake_flags & WF_CURRENT_CPU) ||
	    unlikely(!cpumask_intersects(p->cpus_ptr, cpu_active_mask)))
		return;

	/* Light unboosted wakeups stay with energy aware placement while it runs */
	if ((wake_flags & WF_TTWU) && sched_energy_enabled() &&
	    !READ_ONCE(this_rq()->rd->overutilized) &&
	    cass_task_util(p) < READ_ONCE(eas_gate) &&
	    !uclamp_eff_value(p, UCLAMP_MIN))
		return;

	/* A fork inside a stream of work is placed the kernel's way */
	if ((sd_flag & SD_BALANCE_FORK) && cass_sustained())
		return;

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	*new_cpu = cass_best_cpu(p, prev_cpu, sync, cass_task_util(p), false);
}

static void cass_select_task_rq_rt(void *unused, struct task_struct *p,
				   int prev_cpu, int sd_flag, int wake_flags,
				   int *new_cpu)
{
	bool sync;

	if (!READ_ONCE(rt_mode))
		return;

	/* Only wakeups and forks pick a CPU; RT tasks have no PELT util of their own */
	if (!(wake_flags & (WF_TTWU | WF_FORK)) ||
	    unlikely(!cpumask_intersects(p->cpus_ptr, cpu_active_mask)))
		return;

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	*new_cpu = cass_best_cpu(p, prev_cpu, sync, 0, true);
}

/* PELT forgets the work of ended tasks, so a cluster's busy share keeps a floor under the clock */
static void cass_cpu_util_cfs_boost(void *unused, int cpu, unsigned long *ret)
{
	u64 win = (u64)READ_ONCE(busy_window_ms) * NSEC_PER_MSEC;
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	struct cpufreq_policy *policy;
	unsigned long util, cap = arch_scale_cpu_capacity(cpu);
	u64 now, busy = 0, last, before;
	int i;

	if (!win)
		return;

	policy = cpufreq_cpu_get_raw(cpu);
	if (!policy)
		return;

	/* The kernel's own value first, as cpu_util(cpu, NULL, -1, 1) gives it */
	util = max(READ_ONCE(cfs_rq->avg.util_avg), READ_ONCE(cfs_rq->avg.runnable_avg));
	if (sched_feat(UTIL_EST))
		util = max_t(unsigned long, util, READ_ONCE(cfs_rq->avg.util_est));
	util = min(util, cap);

	now = sched_clock();
	for_each_cpu(i, policy->related_cpus) {
		cass_busy_windows(i, now, win, &last, &before);
		busy += last;
	}
	busy = min(busy, win);

	/* Busy share times the capacity at the current clock = the work the cluster did */
	busy = div64_u64(busy * cap * per_cpu(arch_freq_scale, cpu), win * SCHED_CAPACITY_SCALE);
	*ret = max_t(unsigned long, util, min_t(unsigned long, busy, cap));
}

static void cass_post_init_entity_util_avg(void *unused, struct sched_entity *se)
{
	int max = READ_ONCE(newborn_util_max);

	if (max < 0 || !entity_is_task(se) || se->avg.util_avg <= max ||
	    cass_sustained())
		return;

	se->avg.util_avg = max;
	se->avg.runnable_avg = max;
}

static int __init sched_cass_init(void)
{
	int ret;

	ret = register_trace_android_rvh_select_task_rq_fair(cass_select_task_rq_fair, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_select_task_rq_rt(cass_select_task_rq_rt, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_cpu_util_cfs_boost(cass_cpu_util_cfs_boost, NULL);
	if (ret)
		return ret;

	ret = register_trace_sched_switch(cass_sched_switch, NULL);
	if (ret)
		return ret;

	return register_trace_android_rvh_post_init_entity_util_avg(cass_post_init_entity_util_avg,
								    NULL);
}
module_init(sched_cass_init);

MODULE_DESCRIPTION("Capacity Aware Superset Scheduler task placement");
MODULE_LICENSE("GPL");
