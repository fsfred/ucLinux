/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
 * Internal non-public definitions that provide either classic
 * or preemptable semantics.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright Red Hat, 2009
 * Copyright IBM Corporation, 2009
 *
 * Author: Ingo Molnar <mingo@elte.hu>
 *	   Paul E. McKenney <paulmck@linux.vnet.ibm.com>
 */

#include <linux/delay.h>

#ifdef CONFIG_TREE_PREEMPT_RCU

struct rcu_state rcu_preempt_state = RCU_STATE_INITIALIZER(rcu_preempt_state);
DEFINE_PER_CPU(struct rcu_data, rcu_preempt_data);

static int rcu_preempted_readers_exp(struct rcu_node *rnp);

/*
 * Tell them what RCU they are running.
 */
static void __init rcu_bootup_announce(void)
{
	printk(KERN_INFO
	       "Experimental preemptable hierarchical RCU implementation.\n");
}

/*
 * Return the number of RCU-preempt batches processed thus far
 * for debug and statistics.
 */
long rcu_batches_completed_preempt(void)
{
	return rcu_preempt_state.completed;
}
EXPORT_SYMBOL_GPL(rcu_batches_completed_preempt);

/*
 * Return the number of RCU batches processed thus far for debug & stats.
 */
long rcu_batches_completed(void)
{
	return rcu_batches_completed_preempt();
}
EXPORT_SYMBOL_GPL(rcu_batches_completed);

/*
 * Record a preemptable-RCU quiescent state for the specified CPU.  Note
 * that this just means that the task currently running on the CPU is
 * not in a quiescent state.  There might be any number of tasks blocked
 * while in an RCU read-side critical section.
 */
static void rcu_preempt_qs(int cpu)
{
	struct rcu_data *rdp = &per_cpu(rcu_preempt_data, cpu);
	rdp->passed_quiesc_completed = rdp->gpnum - 1;
	barrier();
	rdp->passed_quiesc = 1;
}

/*
 * We have entered the scheduler, and the current task might soon be
 * context-switched away from.  If this task is in an RCU read-side
 * critical section, we will no longer be able to rely on the CPU to
 * record that fact, so we enqueue the task on the appropriate entry
 * of the blocked_tasks[] array.  The task will dequeue itself when
 * it exits the outermost enclosing RCU read-side critical section.
 * Therefore, the current grace period cannot be permitted to complete
 * until the blocked_tasks[] entry indexed by the low-order bit of
 * rnp->gpnum empties.
 *
 * Caller must disable preemption.
 */
static void rcu_preempt_note_context_switch(int cpu)
{
	struct task_struct *t = current;
	unsigned long flags;
	int phase;
	struct rcu_data *rdp;
	struct rcu_node *rnp;

	if (t->rcu_read_lock_nesting &&
	    (t->rcu_read_unlock_special & RCU_READ_UNLOCK_BLOCKED) == 0) {

		/* Possibly blocking in an RCU read-side critical section. */
		rdp = rcu_preempt_state.rda[cpu];
		rnp = rdp->mynode;
		spin_lock_irqsave(&rnp->lock, flags);
		t->rcu_read_unlock_special |= RCU_READ_UNLOCK_BLOCKED;
		t->rcu_blocked_node = rnp;

		/*
		 * If this CPU has already checked in, then this task
		 * will hold up the next grace period rather than the
		 * current grace period.  Queue the task accordingly.
		 * If the task is queued for the current grace period
		 * (i.e., this CPU has not yet passed through a quiescent
		 * state for the current grace period), then as long
		 * as that task remains queued, the current grace period
		 * cannot end.
		 *
		 * But first, note that the current CPU must still be
		 * on line!
		 */
		WARN_ON_ONCE((rdp->grpmask & rnp->qsmaskinit) == 0);
		WARN_ON_ONCE(!list_empty(&t->rcu_node_entry));
		phase = (rnp->gpnum + !(rnp->qsmask & rdp->grpmask)) & 0x1;
		list_add(&t->rcu_node_entry, &rnp->blocked_tasks[phase]);
		spin_unlock_irqrestore(&rnp->lock, flags);
	}

	/*
	 * Either we were not in an RCU read-side critical section to
	 * begin with, or we have now recorded that critical section
	 * globally.  Either way, we can now note a quiescent state
	 * for this CPU.  Again, if we were in an RCU read-side critical
	 * section, and if that critical section was blocking the current
	 * grace period, then the fact that the task has been enqueued
	 * means that we continue to block the current grace period.
	 */
	rcu_preempt_qs(cpu);
	local_irq_save(flags);
	t->rcu_read_unlock_special &= ~RCU_READ_UNLOCK_NEED_QS;
	local_irq_restore(flags);
}

/*
 * Tree-preemptable RCU implementation for rcu_read_lock().
 * Just increment ->rcu_read_lock_nesting, shared state will be updated
 * if we block.
 */
void __rcu_read_lock(void)
{
	ACCESS_ONCE(current->rcu_read_lock_nesting)++;
	barrier();  /* needed if we ever invoke rcu_read_lock in rcutree.c */
}
EXPORT_SYMBOL_GPL(__rcu_read_lock);

/*
 * Check for preempted RCU readers blocking the current grace period
 * for the specified rcu_node structure.  If the caller needs a reliable
 * answer, it must hold the rcu_node's ->lock.
 */
static int rcu_preempted_readers(struct rcu_node *rnp)
{
	int phase = rnp->gpnum & 0x1;

	return !list_empty(&rnp->blocked_tasks[phase]) ||
	       !list_empty(&rnp->blocked_tasks[phase + 2]);
}

/*
 * Record a quiescent state for all tasks that were previously queued
 * on the specified rcu_node structure and that were blocking the current
 * RCU grace period.  The caller must hold the specified rnp->lock with
 * irqs disabled, and this lock is released upon return, but irqs remain
 * disabled.
 */
static void rcu_report_unblock_qs_rnp(struct rcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	unsigned long mask;
	struct rcu_node *rnp_p;

	if (rnp->qsmask != 0 || rcu_preempted_readers(rnp)) {
		spin_unlock_irqrestore(&rnp->lock, flags);
		return;  /* Still need more quiescent states! */
	}

	rnp_p = rnp->parent;
	if (rnp_p == NULL) {
		/*
		 * Either there is only one rcu_node in the tree,
		 * or tasks were kicked up to root rcu_node due to
		 * CPUs going offline.
		 */
		rcu_report_qs_rsp(&rcu_preempt_state, flags);
		return;
	}

	/* Report up the rest of the hierarchy. */
	mask = rnp->grpmask;
	spin_unlock(&rnp->lock);	/* irqs remain disabled. */
	spin_lock(&rnp_p->lock);	/* irqs already disabled. */
	rcu_report_qs_rnp(mask, &rcu_preempt_state, rnp_p, flags);
}

/*
 * Handle special cases during rcu_read_unlock(), such as needing to
 * notify RCU core processing or task having blocked during the RCU
 * read-side critical section.
 */
static void rcu_read_unlock_special(struct task_struct *t)
{
	int empty;
	int empty_exp;
	unsigned long flags;
	struct rcu_node *rnp;
	int special;

	/* NMI handlers cannot block and cannot safely manipulate state. */
	if (in_nmi())
		return;

	local_irq_save(flags);

	/*
	 * If RCU core is waiting for this CPU to exit critical section,
	 * let it know that we have done so.
	 */
	special = t->rcu_read_unlock_special;
	if (special & RCU_READ_UNLOCK_NEED_QS) {
		t->rcu_read_unlock_special &= ~RCU_READ_UNLOCK_NEED_QS;
		rcu_preempt_qs(smp_processor_id());
	}

	/* Hardware IRQ handlers cannot block. */
	if (in_irq()) {
		local_irq_restore(flags);
		return;
	}

	/* Clean up if blocked during RCU read-side critical section. */
	if (special & RCU_READ_UNLOCK_BLOCKED) {
		t->rcu_read_unlock_special &= ~RCU_READ_UNLOCK_BLOCKED;

		/*
		 * Remove this task from the list it blocked on.  The
		 * task can migrate while we acquire the lock, but at
		 * most one time.  So at most two passes through loop.
		 */
		for (;;) {
			rnp = t->rcu_blocked_node;
			spin_lock(&rnp->lock);  /* irqs already disabled. */
			if (rnp == t->rcu_blocked_node)
				break;
			spin_unlock(&rnp->lock);  /* irqs remain disabled. */
		}
		empty = !rcu_preempted_readers(rnp);
		empty_exp = !rcu_preempted_readers_exp(rnp);
		smp_mb(); /* ensure expedited fastpath sees end of RCU c-s. */
		list_del_init(&t->rcu_node_entry);
		t->rcu_blocked_node = NULL;

		/*
		 * If this was the last task on the current list, and if
		 * we aren't waiting on any CPUs, report the quiescent state.
		 * Note that rcu_report_unblock_qs_rnp() releases rnp->lock.
		 */
		if (empty)
			spin_unlock_irqrestore(&rnp->lock, flags);
		else
			rcu_report_unblock_qs_rnp(rnp, flags);

		/*
		 * If this was the last task on the expedited lists,
		 * then we need to report up the rcu_node hierarchy.
		 */
		if (!empty_exp && !rcu_preempted_readers_exp(rnp))
			rcu_report_exp_rnp(&rcu_preempt_state, rnp);
	} else {
		local_irq_restore(flags);
	}
}

/*
 * Tree-preemptable RCU implementation for rcu_read_unlock().
 * Decrement ->rcu_read_lock_nesting.  If the result is zero (outermost
 * rcu_read_unlock()) and ->rcu_read_unlock_special is non-zero, then
 * invoke rcu_read_unlock_special() to clean up after a context switch
 * in an RCU read-side critical section and other special cases.
 */
void __rcu_read_unlock(void)
{
	struct task_struct *t = current;

	barrier();  /* needed if we ever invoke rcu_read_unlock in rcutree.c */
	if (--ACCESS_ONCE(t->rcu_read_lock_nesting) == 0 &&
	    unlikely(ACCESS_ONCE(t->rcu_read_unlock_special)))
		rcu_read_unlock_special(t);
}
EXPORT_SYMBOL_GPL(__rcu_read_unlock);

#ifdef CONFIG_RCU_CPU_STALL_DETECTOR

/*
 * Scan the current list of tasks blocked within RCU read-side critical
 * sections, printing out the tid of each.
 */
static void rcu_print_task_stall(struct rcu_node *rnp)
{
	unsigned long flags;
	struct list_head *lp;
	int phase;
	struct task_struct *t;

	if (rcu_preempted_readers(rnp)) {
		spin_lock_irqsave(&rnp->lock, flags);
		phase = rnp->gpnum & 0x1;
		lp = &rnp->blocked_tasks[phase];
		list_for_each_entry(t, lp, rcu_node_entry)
			printk(" P%d", t->pid);
		spin_unlock_irqrestore(&rnp->lock, flags);
	}
}

#endif /* #ifdef CONFIG_RCU_CPU_STALL_DETECTOR */

/*
 * Check that the list of blocked tasks for the newly completed grace
 * period is in fact empty.  It is a serious bug to complete a grace
 * period that still has RCU readers blocked!  This function must be
 * invoked -before- updating this rnp's ->gpnum, and the rnp's ->lock
 * must be held by the caller.
 */
static void rcu_preempt_check_blocked_tasks(struct rcu_node *rnp)
{
	WARN_ON_ONCE(rcu_preempted_readers(rnp));
	WARN_ON_ONCE(rnp->qsmask);
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Handle tasklist migration for case in which all CPUs covered by the
 * specified rcu_node have gone offline.  Move them up to the root
 * rcu_node.  The reason for not just moving them to the immediate
 * parent is to remove the need for rcu_read_unlock_special() to
 * make more than two attempts to acquire the target rcu_node's lock.
 * Returns true if there were tasks blocking the current RCU grace
 * period.
 *
 * Returns 1 if there was previously a task blocking the current grace
 * period on the specified rcu_node structure.
 *
 * The caller must hold rnp->lock with irqs disabled.
 */
static int rcu_preempt_offline_tasks(struct rcu_state *rsp,
				     struct rcu_node *rnp,
				     struct rcu_data *rdp)
{
	int i;
	struct list_head *lp;
	struct list_head *lp_root;
	int retval = 0;
	struct rcu_node *rnp_root = rcu_get_root(rsp);
	struct task_struct *tp;

	if (rnp == rnp_root) {
		WARN_ONCE(1, "Last CPU thought to be offlined?");
		return 0;  /* Shouldn't happen: at least one CPU online. */
	}
	WARN_ON_ONCE(rnp != rdp->mynode &&
		     (!list_empty(&rnp->blocked_tasks[0]) ||
		      !list_empty(&rnp->blocked_tasks[1]) ||
		      !list_empty(&rnp->blocked_tasks[2]) ||
		      !list_empty(&rnp->blocked_tasks[3])));

	/*
	 * Move tasks up to root rcu_node.  Rely on the fact that the
	 * root rcu_node can be at most one ahead of the rest of the
	 * rcu_nodes in terms of gp_num value.  This fact allows us to
	 * move the blocked_tasks[] array directly, element by element.
	 */
	if (rcu_preempted_readers(rnp))
		retval |= RCU_OFL_TASKS_NORM_GP;
	if (rcu_preempted_readers_exp(rnp))
		retval |= RCU_OFL_TASKS_EXP_GP;
	for (i = 0; i < 4; i++) {
		lp = &rnp->blocked_tasks[i];
		lp_root = &rnp_root->blocked_tasks[i];
		while (!list_empty(lp)) {
			tp = list_entry(lp->next, typeof(*tp), rcu_node_entry);
			spin_lock(&rnp_root->lock); /* irqs already disabled */
			list_del(&tp->rcu_node_entry);
			tp->rcu_blocked_node = rnp_root;
			list_add(&tp->rcu_node_entry, lp_root);
			spin_unlock(&rnp_root->lock); /* irqs remain disabled */
		}
	}
	return retval;
}

/*
 * Do CPU-offline processing for preemptable RCU.
 */
static void rcu_preempt_offline_cpu(int cpu)
{
	__rcu_offline_cpu(cpu, &rcu_preempt_state);
}

#endif /* #ifdef CONFIG_HOTPLUG_CPU */

/*
 * Check for a quiescent state from the current CPU.  When a task blocks,
 * the task is recorded in the corresponding CPU's rcu_node structure,
 * which is checked elsewhere.
 *
 * Caller must disable hard irqs.
 */
static void rcu_preempt_check_callbacks(int cpu)
{
	struct task_struct *t = current;

	if (t->rcu_read_lock_nesting == 0) {
		t->rcu_read_unlock_special &= ~RCU_READ_UNLOCK_NEED_QS;
		rcu_preempt_qs(cpu);
		return;
	}
	if (per_cpu(rcu_preempt_data, cpu).qs_pending)
		t->rcu_read_unlock_special |= RCU_READ_UNLOCK_NEED_QS;
}

/*
 * Process callbacks for preemptable RCU.
 */
static void rcu_preempt_process_callbacks(void)
{
	__rcu_process_callbacks(&rcu_preempt_state,
				&__get_cpu_var(rcu_preempt_data));
}

/*
 * Queue a preemptable-RCU callback for invocation after a grace period.
 */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *rcu))
{
	__call_rcu(head, func, &rcu_preempt_state);
}
EXPORT_SYMBOL_GPL(call_rcu);

/**
 * synchronize_rcu - wait until a grace period has elapsed.
 *
 * Control will return to the caller some time after a full grace
 * period has elapsed, in other words after all currently executing RCU
 * read-side critical sections have completed.  RCU read-side critical
 * sections are delimited by rcu_read_lock() and rcu_read_unlock(),
 * and may be nested.
 */
void synchronize_rcu(void)
{
	struct rcu_synchronize rcu;

	if (!rcu_scheduler_active)
		return;

	init_completion(&rcu.completion);
	/* Will wake me after RCU finished. */
	call_rcu(&rcu.head, wakeme_after_rcu);
	/* Wait for it. */
	wait_for_completion(&rcu.completion);
}
EXPORT_SYMBOL_GPL(synchronize_rcu);

static DECLARE_WAIT_QUEUE_HEAD(sync_rcu_preempt_exp_wq);
static long sync_rcu_preempt_exp_count;
static DEFINE_MUTEX(sync_rcu_preempt_exp_mutex);

/*
 * Return non-zero if there are any tasks in RCU read-side critical
 * sections blocking the current preemptible-RCU expedited grace period.
 * If there is no preemptible-RCU expedited grace period currently in
 * progress, returns zero unconditionally.
 */
static int rcu_preempted_readers_exp(struct rcu_node *rnp)
{
	return !list_empty(&rnp->blocked_tasks[2]) ||
	       !list_empty(&rnp->blocked_tasks[3]);
}

/*
 * return non-zero if there is no RCU expedited grace period in progress
 * for the specified rcu_node structure, in other words, if all CPUs and
 * tasks covered by the specified rcu_node structure have done their bit
 * for the current expedited grace period.  Works only for preemptible
 * RCU -- other RCU implementation use other means.
 *
 * Caller must hold sync_rcu_preempt_exp_mutex.
 */
static int sync_rcu_preempt_exp_done(struct rcu_node *rnp)
{
	return !rcu_preempted_readers_exp(rnp) &&
	       ACCESS_ONCE(rnp->expmask) == 0;
}

/*
 * Report the exit from RCU read-side critical section for the last task
 * that queued itself during or before the current expedited preemptible-RCU
 * grace period.  This event is reported either to the rcu_node structure on
 * which the task was queued or to one of that rcu_node structure's ancestors,
 * recursively up the tree.  (Calm down, calm down, we do the recursion
 * iteratively!)
 *
 * Caller must hold sync_rcu_preempt_exp_mutex.
 */
static void rcu_report_exp_rnp(struct rcu_state *rsp, struct rcu_node *rnp)
{
	unsigned long flags;
	unsigned long mask;

	spin_lock_irqsave(&rnp->lock, flags);
	for (;;) {
		if (!sync_rcu_preempt_exp_done(rnp))
			break;
		if (rnp->parent == NULL) {
			wake_up(&sync_rcu_preempt_exp_wq);
			break;
		}
		mask = rnp->grpmask;
		spin_unlock(&rnp->lock); /* irqs remain disabled */
		rnp = rnp->parent;
		spin_lock(&rnp->lock); /* irqs already disabled */
		rnp->expmask &= ~mask;
	}
	spin_unlock_irqrestore(&rnp->lock, flags);
}

/*
 * Snapshot the tasks blocking the newly started preemptible-RCU expedited
 * grace period for the specified rcu_node structure.  If there are no such
 * tasks, report it up the rcu_node hierarchy.
 *
 * Caller must hold sync_rcu_preempt_exp_mutex and rsp->onofflock.
 */
static void
sync_rcu_preempt_exp_init(struct rcu_state *rsp, struct rcu_node *rnp)
{
	int must_wait;

	spin_lock(&rnp->lock); /* irqs already disabled */
	list_splice_init(&rnp->blocked_tasks[0], &rnp->blocked_tasks[2]);
	list_splice_init(&rnp->blocked_tasks[1], &rnp->blocked_tasks[3]);
	must_wait = rcu_preempted_readers_exp(rnp);
	spin_unlock(&rnp->lock); /* irqs remain disabled */
	if (!must_wait)
		rcu_report_exp_rnp(rsp, rnp);
}

/*
 * Wait for an rcu-preempt grace period, but expedite it.  The basic idea
 * is to invoke synchronize_sched_expedited() to push all the tasks to
 * the ->blocked_tasks[] lists, move all entries from the first set of
 * ->blocked_tasks[] lists to the second set, and finally wait for this
 * second set to drain.
 */
void synchronize_rcu_expedited(void)
{
	unsigned long flags;
	struct rcu_node *rnp;
	struct rcu_state *rsp = &rcu_preempt_state;
	long snap;
	int trycount = 0;

	smp_mb(); /* Caller's modifications seen first by other CPUs. */
	snap = ACCESS_ONCE(sync_rcu_preempt_exp_count) + 1;
	smp_mb(); /* Above access cannot bleed into critical section. */

	/*
	 * Acquire lock, falling back to synchronize_rcu() if too many
	 * lock-acquisition failures.  Of course, if someone does the
	 * expedited grace period for us, just leave.
	 */
	while (!mutex_trylock(&sync_rcu_preempt_exp_mutex)) {
		if (trycount++ < 10)
			udelay(trycount * num_online_cpus());
		else {
			synchronize_rcu();
			return;
		}
		if ((ACCESS_ONCE(sync_rcu_preempt_exp_count) - snap) > 0)
			goto mb_ret; /* Others did our work for us. */
	}
	if ((ACCESS_ONCE(sync_rcu_preempt_exp_count) - snap) > 0)
		goto unlock_mb_ret; /* Others did our work for us. */

	/* force all RCU readers onto blocked_tasks[]. */
	synchronize_sched_expedited();

	spin_lock_irqsave(&rsp->onofflock, flags);

	/* Initialize ->expmask for all non-leaf rcu_node structures. */
	rcu_for_each_nonleaf_node_breadth_first(rsp, rnp) {
		spin_lock(&rnp->lock); /* irqs already disabled. */
		rnp->expmask = rnp->qsmaskinit;
		spin_unlock(&rnp->lock); /* irqs remain disabled. */
	}

	/* Snapshot current state of ->blocked_tasks[] lists. */
	rcu_for_each_leaf_node(rsp, rnp)
		sync_rcu_preempt_exp_init(rsp, rnp);
	if (NUM_RCU_NODES > 1)
		sync_rcu_preempt_exp_init(rsp, rcu_get_root(rsp));

	spin_unlock_irqrestore(&rsp->onofflock, flags);

	/* Wait for snapshotted ->blocked_tasks[] lists to drain. */
	rnp = rcu_get_root(rsp);
	wait_event(sync_rcu_preempt_exp_wq,
		   sync_rcu_preempt_exp_done(rnp));

	/* Clean up and exit. */
	smp_mb(); /* ensure expedited GP seen before counter increment. */
	ACCESS_ONCE(sync_rcu_preempt_exp_count)++;
unlock_mb_ret:
	mutex_unlock(&sync_rcu_preempt_exp_mutex);
mb_ret:
	smp_mb(); /* ensure subsequent action seen after grace period. */
}
EXPORT_SYMBOL_GPL(synchronize_rcu_expedited);

/*
 * Check to see if there is any immediate preemptable-RCU-related work
 * to be done.
 */
static int rcu_preempt_pending(int cpu)
{
	return __rcu_pending(&rcu_preempt_state,
			     &per_cpu(rcu_preempt_data, cpu));
}

/*
 * Does preemptable RCU need the CPU to stay out of dynticks mode?
 */
static int rcu_preempt_needs_cpu(int cpu)
{
	return !!per_cpu(rcu_preempt_data, cpu).nxtlist;
}

/**
 * rcu_barrier - Wait until all in-flight call_rcu() callbacks complete.
 */
void rcu_barrier(void)
{
	_rcu_barrier(&rcu_preempt_state, call_rcu);
}
EXPORT_SYMBOL_GPL(rcu_barrier);

/*
 * Initialize preemptable RCU's per-CPU data.
 */
static void __cpuinit rcu_preempt_init_percpu_data(int cpu)
{
	rcu_init_percpu_data(cpu, &rcu_preempt_state, 1);
}

/*
 * Move preemptable RCU's callbacks to ->orphan_cbs_list.
 */
static void rcu_preempt_send_cbs_to_orphanage(void)
{
	rcu_send_cbs_to_orphanage(&rcu_preempt_state);
}

/*
 * Initialize preemptable RCU's state structures.
 */
static void __init __rcu_init_preempt(void)
{
	RCU_INIT_FLAVOR(&rcu_preempt_state, rcu_preempt_data);
}

/*
 * Check for a task exiting while in a preemptable-RCU read-side
 * critical section, clean up if so.  No need to issue warnings,
 * as debug_check_no_locks_held() already does this if lockdep
 * is enabled.
 */
void exit_rcu(void)
{
	struct task_struct *t = current;

	if (t->rcu_read_lock_nesting == 0)
		return;
	t->rcu_read_lock_nesting = 1;
	rcu_read_unlock();
}

#else /* #ifdef CONFIG_TREE_PREEMPT_RCU */

/*
 * Tell them what RCU they are running.
 */
static void __init rcu_bootup_announce(void)
{
	printk(KERN_INFO "Hierarchical RCU implementation.\n");
}

/*
 * Return the number of RCU batches processed thus far for debug & stats.
 */
long rcu_batches_completed(void)
{
	return rcu_batches_completed_sched();
}
EXPORT_SYMBOL_GPL(rcu_batches_completed);

/*
 * Because preemptable RCU does not exist, we never have to check for
 * CPUs being in quiescent states.
 */
static void rcu_preempt_note_context_switch(int cpu)
{
}

/*
 * Because preemptable RCU does not exist, there are never any preempted
 * RCU readers.
 */
static int rcu_preempted_readers(struct rcu_node *rnp)
{
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU

/* Because preemptible RCU does not exist, no quieting of tasks. */
static void rcu_report_unblock_qs_rnp(struct rcu_node *rnp, unsigned long flags)
{
	spin_unlock_irqrestore(&rnp->lock, flags);
}

#endif /* #ifdef CONFIG_HOTPLUG_CPU */

#ifdef CONFIG_RCU_CPU_STALL_DETECTOR

/*
 * Because preemptable RCU does not exist, we never have to check for
 * tasks blocked within RCU read-side critical sections.
 */
static void rcu_print_task_stall(struct rcu_node *rnp)
{
}

#endif /* #ifdef CONFIG_RCU_CPU_STALL_DETECTOR */

/*
 * Because there is no preemptable RCU, there can be no readers blocked,
 * so there is no need to check for blocked tasks.  So check only for
 * bogus qsmask values.
 */
static void rcu_preempt_check_blocked_tasks(struct rcu_node *rnp)
{
	WARN_ON_ONCE(rnp->qsmask);
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Because preemptable RCU does not exist, it never needs to migrate
 * tasks that were blocked within RCU read-side critical sections, and
 * such non-existent tasks cannot possibly have been blocking the current
 * grace period.
 */
static int rcu_preempt_offline_tasks(struct rcu_state *rsp,
				     struct rcu_node *rnp,
				     struct rcu_data *rdp)
{
	return 0;
}

/*
 * Because preemptable RCU does not exist, it never needs CPU-offline
 * processing.
 */
static void rcu_preempt_offline_cpu(int cpu)
{
}

#endif /* #ifdef CONFIG_HOTPLUG_CPU */

/*
 * Because preemptable RCU does not exist, it never has any callbacks
 * to check.
 */
static void rcu_preempt_check_callbacks(int cpu)
{
}

/*
 * Because preemptable RCU does not exist, it never has any callbacks
 * to process.
 */
static void rcu_preempt_process_callbacks(void)
{
}

/*
 * In classic RCU, call_rcu() is just call_rcu_sched().
 */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *rcu))
{
	call_rcu_sched(head, func);
}
EXPORT_SYMBOL_GPL(call_rcu);

/*
 * Wait for an rcu-preempt grace period, but make it happen quickly.
 * But because preemptable RCU does not exist, map to rcu-sched.
 */
void synchronize_rcu_expedited(void)
{
	synchronize_sched_expedited();
}
EXPORT_SYMBOL_GPL(synchronize_rcu_expedited);

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Because preemptable RCU does not exist, there is never any need to
 * report on tasks preempted in RCU read-side critical sections during
 * expedited RCU grace periods.
 */
static void rcu_report_exp_rnp(struct rcu_state *rsp, struct rcu_node *rnp)
{
	return;
}

#endif /* #ifdef CONFIG_HOTPLUG_CPU */

/*
 * Because preemptable RCU does not exist, it never has any work to do.
 */
static int rcu_preempt_pending(int cpu)
{
	return 0;
}

/*
 * Because preemptable RCU does not exist, it never needs any CPU.
 */
static int rcu_preempt_needs_cpu(int cpu)
{
	return 0;
}

/*
 * Because preemptable RCU does not exist, rcu_barrier() is just
 * another name for rcu_barrier_sched().
 */
void rcu_barrier(void)
{
	rcu_barrier_sched();
}
EXPORT_SYMBOL_GPL(rcu_barrier);

/*
 * Because preemptable RCU does not exist, there is no per-CPU
 * data to initialize.
 */
static void __cpuinit rcu_preempt_init_percpu_data(int cpu)
{
}

/*
 * Because there is no preemptable RCU, there are no callbacks to move.
 */
static void rcu_preempt_send_cbs_to_orphanage(void)
{
}

/*
 * Because preemptable RCU does not exist, it need not be initialized.
 */
static void __init __rcu_init_preempt(void)
{
}

#endif /* #else #ifdef CONFIG_TREE_PREEMPT_RCU */
