/*
* Weighted Round Robin Scheduling Class (mapped to the SCHED_WRR
* policy)
*/

#include "sched.h"

#include <linux/slab.h>
#include <linux/string.h>
#include <trace/events/sched.h>

int sched_wrr_timeslice = WRR_TIMESLICE;
extern char *task_group_path(struct task_group *tg);

void init_wrr_rq(struct wrr_rq *wrr_rq, struct rq *rq)
{
	struct wrr_prio_array *array;
	int i;

	array = &wrr_rq->active;
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	__set_bit(MAX_RT_PRIO, array->bitmap);

    wrr_rq->wrr_throttled = 0;
	wrr_rq->wrr_time = 0;
	wrr_rq->wrr_runtime = 0;

	raw_spin_lock_init(&wrr_rq->wrr_runtime_lock);
}

#define wrr_entity_is_task(wrr_se) (1)

static inline struct wrr_rq *wrr_rq_of_se(struct sched_wrr_entity *wrr_se)
{
	struct task_struct *p = container_of(wrr_se, struct task_struct, wrr);
	struct rq *rq = task_rq(p);
	return &rq->wrr;
}




static inline int on_wrr_rq(struct sched_wrr_entity *wrr_se)
{
	return !list_empty(&wrr_se->run_list);
}


typedef struct wrr_rq *wrr_rq_iter_t;

#define for_each_wrr_rq(wrr_rq, iter, rq) \
	for ((void) iter, wrr_rq = &rq->wrr; wrr_rq; wrr_rq = NULL)


#define for_each_leaf_wrr_rq(wrr_rq, rq) \
	for (wrr_rq = &rq->wrr; wrr_rq; wrr_rq = NULL)

#define for_each_sched_wrr_entity(wrr_se) \
	for (; wrr_se; wrr_se = NULL)


static inline struct wrr_rq *group_wrr_rq(struct sched_wrr_entity *wrr_se)
{
	return NULL;
}


static inline void sched_wrr_rq_enqueue(struct wrr_rq *wrr_rq)
{
	if (wrr_rq->wrr_nr_running)
		resched_task(container_of(wrr_rq, struct rq, wrr)->curr);
	
}

static inline int wrr_se_prio(struct sched_wrr_entity *wrr_se)
{
	struct task_struct *p = container_of(wrr_se, struct task_struct, wrr);
	return p->prio;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void update_curr_wrr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	
	u64 delta_exec;

	if (curr->sched_class != &wrr_sched_class)
		return;

	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;

	curr->se.exec_start = rq->clock_task;
}

static void put_prev_task_wrr(struct rq *rq, struct task_struct *p)
{
	update_curr_wrr(rq);
}

static void __dequeue_wrr_entity(struct sched_wrr_entity *wrr_se)
{
	struct wrr_rq *wrr_rq = wrr_rq_of_se(wrr_se);
	struct wrr_prio_array *array = &wrr_rq->active;
	list_del_init(&wrr_se->run_list);
	if (list_empty(array->queue + wrr_se_prio(wrr_se)))
		__clear_bit(wrr_se_prio(wrr_se), array->bitmap);

	wrr_rq->wrr_nr_running--;
}


static void dequeue_wrr_entity(struct sched_wrr_entity *wrr_se)
{
	struct sched_wrr_entity *back = NULL;

	for_each_sched_wrr_entity(wrr_se) {
		wrr_se->back = back;
		back = wrr_se;
	}

	for (wrr_se = back; wrr_se; wrr_se = wrr_se->back) {
		if (on_wrr_rq(wrr_se))
			__dequeue_wrr_entity(wrr_se);
	}
}

static void __enqueue_wrr_entity(struct sched_wrr_entity *wrr_se, bool head)
{
	struct wrr_rq *wrr_rq = wrr_rq_of_se(wrr_se);
	struct wrr_prio_array *array = &wrr_rq->active;
	struct list_head *queue = array->queue + wrr_se_prio(wrr_se);
	struct task_struct *p = container_of(wrr_se, struct task_struct, wrr);
	/*
	* Don't enqueue the group if its throttled, or when empty.
	* The latter is a consequence of the former when a child group
	* get throttled and the current group doesn't have any other
	* active members.
	*/
	if (head)
		list_add(&wrr_se->run_list, queue);
	else
		list_add_tail(&wrr_se->run_list, queue);
	__set_bit(wrr_se_prio(wrr_se), array->bitmap);

	if (!p->wrr.time_slice) {
		struct task_group *tg = p->sched_task_group;
		char *get_group = task_group_path(tg);
		if (get_group[1] == 'b')
			p->wrr.weight = 1;
		else
			p->wrr.weight = 10;
		p->wrr.time_slice = WRR_TIMESLICE * p->wrr.weight;
	}

	wrr_rq->wrr_nr_running++;
}

static void enqueue_wrr_entity(struct sched_wrr_entity *wrr_se, bool head)
{
	dequeue_wrr_entity(wrr_se);
	
	for_each_sched_wrr_entity(wrr_se)
		__enqueue_wrr_entity(wrr_se, head);
	
}

static void
requeue_wrr_entity(struct wrr_rq *wrr_rq, struct sched_wrr_entity *wrr_se, int head)
{
	if (on_wrr_rq(wrr_se)) {
		struct wrr_prio_array *array = &wrr_rq->active;
		struct list_head *queue = array->queue + wrr_se_prio(wrr_se);

		if (head)
			list_move(&wrr_se->run_list, queue);
		else
			list_move_tail(&wrr_se->run_list, queue);
	}
}


static void
enqueue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;

	enqueue_wrr_entity(wrr_se, flags & ENQUEUE_HEAD);

	inc_nr_running(rq);
}

static void dequeue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;

	update_curr_wrr(rq);
	dequeue_wrr_entity(wrr_se);

	dec_nr_running(rq);
}

static void requeue_task_wrr(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
	struct wrr_rq *wrr_rq;

	for_each_sched_wrr_entity(wrr_se) {
		wrr_rq = wrr_rq_of_se(wrr_se);
		requeue_wrr_entity(wrr_rq, wrr_se, head);
	}
}

static void yield_task_wrr(struct rq *rq)
{
	requeue_task_wrr(rq, rq->curr, 0);
}

static struct sched_wrr_entity *pick_next_wrr_entity(struct rq *rq,
						   struct wrr_rq *wrr_rq)
{
	struct wrr_prio_array *array = &wrr_rq->active;
	struct sched_wrr_entity *next = NULL;
	struct list_head *queue;
	int idx;

	idx = sched_find_first_bit(array->bitmap);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_wrr_entity, run_list);

	return next;
}

static struct task_struct *_pick_next_task_wrr(struct rq *rq)
{
	struct sched_wrr_entity *wrr_se;
	struct task_struct *p;
	struct wrr_rq *wrr_rq;

	wrr_rq = &rq->wrr;

	if (!wrr_rq->wrr_nr_running)
		return NULL;

	wrr_se = pick_next_wrr_entity(rq, wrr_rq);

	p = container_of(wrr_se, struct task_struct, wrr);
	p->se.exec_start = rq->clock_task;

	return p;
}

static struct task_struct *pick_next_task_wrr(struct rq *rq)
{
	struct task_struct *p = _pick_next_task_wrr(rq);

	/* The running task is never eligible for pushing 
	if (p)
		dequeue_pushable_task(rq, p);*/

	return p;
}


static void switched_to_wrr(struct rq *rq, struct task_struct *p)
{
	/*
	 * If we are already running, then there's nothing
	 * that needs to be done. But if we are not running
	 * we may need to preempt the current running task.
	 * If that current running task is also an wrr task
	 * then see if we can move to another run queue.
	 */
	if (p->on_rq && rq->curr != p) {
		if ( p->prio < rq->curr->prio)
			resched_task(rq->curr);
	}
}

static void task_tick_wrr(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
	struct task_group *tg = p->sched_task_group;
	char *get_group = task_group_path(tg);

	update_curr_wrr(rq);

	if (p->policy != SCHED_WRR)
		return;

	if (--p->wrr.time_slice)
		return;


	if (get_group[1] == 'b')
		p->wrr.weight = 1;
	else
		p->wrr.weight = 10;
	p->wrr.time_slice = WRR_TIMESLICE * p->wrr.weight;

	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are the
	 * only element on the queue
	 */
	for_each_sched_wrr_entity(wrr_se) {
		if (wrr_se->run_list.prev != wrr_se->run_list.next) {
			requeue_task_wrr(rq, p, 0);
			set_tsk_need_resched(p);
			return;
		}
	}
	printk("Timeslice = %d\n", p->wrr.time_slice);
}

static unsigned int get_rr_interval_wrr(struct rq *rq, struct task_struct *task)
{
	struct task_group *tg = task->sched_task_group;
	char *get_group = task_group_path(tg);
	int weight = 0;
	if (get_group[1] == 'b')
		weight = 1;
	else if(get_group[1] == '\0')
		weight = 10;
	return WRR_TIMESLICE * weight;
}

static void set_curr_task_wrr(struct rq *rq)
{
	struct task_struct *p = rq->curr;
	p->se.exec_start = rq->clock_task;
}

int alloc_wrr_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}

void free_wrr_sched_group(struct task_group *tg) {}

static void check_preempt_curr_wrr(struct rq *rq, struct task_struct *p, int flags) {}

static void switched_from_wrr(struct rq *rq, struct task_struct *p) {}

static void prio_changed_wrr(struct rq *rq, struct task_struct *p, int oldprio) {}

static void task_fork_wrr(struct task_struct *p) {}

const struct sched_class wrr_sched_class = {
	.next = &fair_sched_class,		/*not required*/
	.enqueue_task = enqueue_task_wrr,		/*required*/
	.dequeue_task = dequeue_task_wrr,		/*required*/
	.yield_task = yield_task_wrr,			/*required*/
	.check_preempt_curr = check_preempt_curr_wrr,/*not required*/

	.pick_next_task = pick_next_task_wrr,	/*required*/
	.put_prev_task = put_prev_task_wrr,	/*required*/

	.task_fork = task_fork_wrr,			/*required*/
#ifdef CONFIG_SMP
	.select_task_rq = select_task_rq_wrr,	/*never need impl*/
	.set_cpus_allowed = set_cpus_allowed_wrr,
	.rq_online = rq_online_wrr,	/*never need impl*/
	.rq_offline = rq_offline_wrr,	/*never need impl*/
	.pre_schedule = pre_schedule_wrr,		/*never need impl*/
	.post_schedule = post_schedule_wrr,	/*never need impl*/
	.task_woken = task_woken_wrr,			/*never need impl*/
#endif
	.switched_from = switched_from_wrr,	/*never need impl*/

	.set_curr_task = set_curr_task_wrr,/*required*/
	.task_tick = task_tick_wrr,			/*required*/

	.get_rr_interval = get_rr_interval_wrr,	/*required*/

	.prio_changed = prio_changed_wrr,		/*never need impl*/
	.switched_to = switched_to_wrr,		/*required*/
};
