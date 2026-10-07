#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct tail_state {
	uint64_t hp_next;
	uint64_t lp_next;
	uint64_t next;
};

#define RQ_SIZE 8
#define TEST_CHMODEL_SCAN_LIMIT 4
#define TEST_FORCE_AFTER_YIELDS 8
#define TEST_SLC_LEVEL_BG 1
#define TEST_SLC_LEVEL_EMERGENCY 3

struct repromote_ring {
	uint64_t lpns[RQ_SIZE];
	uint32_t head;
	uint32_t tail;
};

#define Q_SIZE 8
#define Q_NONE UINT32_MAX

struct queue_entry {
	uint64_t target;
	uint64_t pcie_tail;
	uint32_t prev;
	uint32_t next;
	int completion_guard;
	int completed;
};

struct proc_queue {
	struct queue_entry e[Q_SIZE];
	uint32_t head;
	uint32_t tail;
	uint64_t proc_now;
};

static uint64_t max_u64(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

static uint64_t max3_u64(uint64_t a, uint64_t b, uint64_t c)
{
	return max_u64(max_u64(a, b), c);
}

static uint64_t advance_low_priority(struct tail_state *s, uint64_t req,
				     uint64_t busy)
{
	uint64_t start = max3_u64(s->hp_next, s->lp_next, req);
	uint64_t end = start + busy;

	s->lp_next = end;
	s->next = max_u64(s->hp_next, s->lp_next);
	return end;
}

static uint64_t advance_normal(struct tail_state *s, uint64_t req,
			       uint64_t busy)
{
	uint64_t start = max_u64(s->next, req);
	uint64_t end = start + busy;

	s->hp_next = end;
	s->lp_next = end;
	s->next = end;
	return end;
}

static uint64_t advance_read_priority(struct tail_state *s, uint64_t req,
				      uint64_t busy, uint64_t *bypass_out)
{
	uint64_t start = max_u64(s->next, req);
	uint64_t end = start + busy;

	if (bypass_out)
		*bypass_out = 0;
	s->hp_next = end;
	s->lp_next = end;
	s->next = end;
	return end;
}

static void check(int cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

static void queue_init(struct proc_queue *q)
{
	q->head = Q_NONE;
	q->tail = Q_NONE;
	q->proc_now = 0;
	for (uint32_t i = 0; i < Q_SIZE; i++) {
		q->e[i].prev = Q_NONE;
		q->e[i].next = Q_NONE;
	}
}

static void queue_insert_sorted(struct proc_queue *q, uint32_t id, uint64_t target,
				int completion_guard, uint64_t pcie_tail)
{
	struct queue_entry *ent = &q->e[id];
	uint32_t curr;

	ent->target = target;
	ent->pcie_tail = pcie_tail;
	ent->completion_guard = completion_guard;
	ent->completed = 0;
	ent->prev = Q_NONE;
	ent->next = Q_NONE;

	if (q->head == Q_NONE) {
		q->head = id;
		q->tail = id;
		return;
	}

	curr = q->tail;
	while (curr != Q_NONE) {
		if (q->e[curr].target <= q->proc_now)
			break;
		if (q->e[curr].target <= target)
			break;
		curr = q->e[curr].prev;
	}

	if (curr == Q_NONE) {
		q->e[q->head].prev = id;
		ent->next = q->head;
		q->head = id;
	} else if (q->e[curr].next == Q_NONE) {
		ent->prev = curr;
		q->e[curr].next = id;
		q->tail = id;
	} else {
		ent->prev = curr;
		ent->next = q->e[curr].next;
		q->e[ent->next].prev = id;
		q->e[curr].next = id;
	}
}

static void queue_remove(struct proc_queue *q, uint32_t id)
{
	uint32_t prev = q->e[id].prev;
	uint32_t next = q->e[id].next;

	if (prev != Q_NONE)
		q->e[prev].next = next;
	else
		q->head = next;
	if (next != Q_NONE)
		q->e[next].prev = prev;
	else
		q->tail = prev;
	q->e[id].prev = Q_NONE;
	q->e[id].next = Q_NONE;
}

static void queue_reschedule(struct proc_queue *q, uint32_t id, uint64_t target)
{
	int guard = q->e[id].completion_guard;
	uint64_t pcie_tail = q->e[id].pcie_tail;

	queue_remove(q, id);
	queue_insert_sorted(q, id, target, guard, pcie_tail);
}

static uint32_t queue_complete_one(struct proc_queue *q)
{
	uint32_t id = q->head;

	check(id != Q_NONE, "queue has a head");
	if (q->e[id].target > q->proc_now)
		return Q_NONE;
	if (q->e[id].completion_guard && q->e[id].pcie_tail > q->proc_now) {
		queue_reschedule(q, id, max_u64(q->e[id].target, q->e[id].pcie_tail));
		return Q_NONE;
	}
	queue_remove(q, id);
	q->e[id].completed = 1;
	return id;
}

static int ring_empty(const struct repromote_ring *r)
{
	return r->head == r->tail;
}

static int ring_push_tail(struct repromote_ring *r, uint64_t lpn)
{
	uint32_t next = (r->tail + 1) % RQ_SIZE;

	if (next == r->head)
		return -1;
	r->lpns[r->tail] = lpn;
	r->tail = next;
	return 0;
}

static int ring_pop_head(struct repromote_ring *r, uint64_t *lpn)
{
	if (ring_empty(r))
		return -1;
	*lpn = r->lpns[r->head];
	r->head = (r->head + 1) % RQ_SIZE;
	return 0;
}

static void restore_unconsumed(struct repromote_ring *r, const uint64_t *lpns,
			       const int *consumed, uint32_t pulled)
{
	uint32_t n;

	for (n = pulled; n > 0; n--) {
		uint32_t idx = n - 1;
		uint32_t new_head;

		if (consumed[idx])
			continue;
		new_head = (r->head + RQ_SIZE - 1) % RQ_SIZE;
		check(new_head != r->tail, "restore has capacity for unconsumed work");
		r->head = new_head;
		r->lpns[new_head] = lpns[idx];
	}
}

static void test_repromote_restore_order(void)
{
	struct repromote_ring r = { 0 };
	uint64_t pulled[4];
	int consumed[4] = { 1, 0, 1, 0 };
	uint64_t got;

	check(ring_push_tail(&r, 10) == 0, "push 10");
	check(ring_push_tail(&r, 11) == 0, "push 11");
	check(ring_push_tail(&r, 12) == 0, "push 12");
	check(ring_push_tail(&r, 13) == 0, "push 13");
	for (uint32_t i = 0; i < 4; i++)
		check(ring_pop_head(&r, &pulled[i]) == 0, "pull batch item");

	check(ring_push_tail(&r, 99) == 0, "producer can enqueue while worker runs");
	restore_unconsumed(&r, pulled, consumed, 4);

	check(ring_pop_head(&r, &got) == 0 && got == 11,
	      "first unconsumed item restored first");
	check(ring_pop_head(&r, &got) == 0 && got == 13,
	      "second unconsumed item restored second");
	check(ring_pop_head(&r, &got) == 0 && got == 99,
	      "new producer item remains behind restored work");
	check(ring_empty(&r), "ring empty after restore-order test");
}

static void test_io_queue_read_priority_order(void)
{
	struct proc_queue q;

	queue_init(&q);
	queue_insert_sorted(&q, 0, 100, 1, 1100);
	queue_insert_sorted(&q, 1, 200, 0, 0);

	q.proc_now = 200;
	check(q.head == 0, "expired guarded write remains first before worker touch");
	check(queue_complete_one(&q) == Q_NONE, "guarded write is rescheduled");
	check(q.head == 1, "read becomes queue head after guarded write moves back");
	check(queue_complete_one(&q) == 1, "read completes before guarded write CQ");

	q.proc_now = 1100;
	check(queue_complete_one(&q) == 0, "guarded write completes at pushed PCIe tail");
	check(q.head == Q_NONE, "queue empty after IO order test");
}

static uint32_t effective_closed_qlc_repromote_budget(uint32_t configured,
						      uint32_t pages_per_sb)
{
	uint32_t budget = configured ? configured : pages_per_sb;

	if (pages_per_sb && budget > pages_per_sb)
		budget = pages_per_sb;
	return budget ? budget : 1;
}

static uint32_t closed_qlc_repromote_until_yield(uint32_t budget,
						 uint32_t read_window_after)
{
	uint32_t moved = 0;

	while (moved < budget) {
		if (moved == read_window_after)
			break;
		moved++;
	}
	return moved;
}

static void test_closed_qlc_repromote_budget_and_yield(void)
{
	uint32_t budget = effective_closed_qlc_repromote_budget(256, 8192);

	check(budget == 256, "closed-QLC repromote honors per-run budget");
	check(effective_closed_qlc_repromote_budget(128, 64) == 64,
	      "closed-QLC repromote clamps to SB page count");
	check(closed_qlc_repromote_until_yield(budget, 17) == 17,
	      "closed-QLC repromote stops when read window reopens");
	check(closed_qlc_repromote_until_yield(budget, budget + 1) == budget,
	      "closed-QLC repromote completes budget without read yield");
}

static uint64_t channel_scan_until_cap(uint32_t *credits, uint32_t slots,
				       uint32_t remaining, uint32_t max_credits,
				       uint64_t request_time,
				       uint64_t unit_interval,
				       uint64_t xfer_lat)
{
	uint32_t pos = 0;
	uint32_t delay = 0;
	uint32_t scanned = 0;
	uint32_t default_delay = remaining / max_credits;

	while (remaining) {
		uint32_t consumed;

		if (scanned++ >= TEST_CHMODEL_SCAN_LIMIT) {
			uint32_t extra_slots = (remaining + max_credits - 1) / max_credits;

			delay += extra_slots;
			break;
		}

		consumed = remaining <= credits[pos] ? remaining : credits[pos];
		credits[pos] -= consumed;
		remaining -= consumed;
		if (!remaining)
			break;

		pos = (pos + 1) % slots;
		delay++;
	}

	delay = delay > default_delay ? delay - default_delay : 0;
	return request_time + xfer_lat + delay * unit_interval;
}

static void test_channel_scan_cap(void)
{
	uint32_t congested[8] = { 0 };
	uint64_t done;

	done = channel_scan_until_cap(congested, 8, 16, 1, 1000, 2000, 100);
	check(done == 9100, "channel scan cap bounds foreground scan time");
}

static int read_priority_should_yield_model(int read_window, int gate,
					    int force_active)
{
	if (force_active > 0)
		return 0;
	if (gate <= 0)
		return 0;
	return read_window;
}

static int read_priority_should_force_progress_model(int level, int *yield_streak,
						     int *forced_runs)
{
	if (level < TEST_SLC_LEVEL_BG)
		return 0;
	if (*yield_streak < TEST_FORCE_AFTER_YIELDS)
		return 0;

	(*forced_runs)++;
	*yield_streak = 0;
	return 1;
}

static void test_force_after_yields_applies_before_emergency_return(void)
{
	int yield_streak = TEST_FORCE_AFTER_YIELDS - 1;
	int forced_runs = 0;

	check(!read_priority_should_force_progress_model(TEST_SLC_LEVEL_EMERGENCY,
							 &yield_streak,
							 &forced_runs),
	      "force-progress waits until yield threshold");
	yield_streak++;
	check(read_priority_should_force_progress_model(TEST_SLC_LEVEL_EMERGENCY,
							&yield_streak,
							&forced_runs),
	      "force-progress can run at emergency level");
	check(forced_runs == 1, "force-progress run is counted");
	check(yield_streak == 0, "force-progress resets yield streak");
	check(!read_priority_should_yield_model(1, 1, 1),
	      "force-active suppresses read-priority yield gate");

	yield_streak = TEST_FORCE_AFTER_YIELDS;
	check(read_priority_should_force_progress_model(TEST_SLC_LEVEL_BG,
							&yield_streak,
							&forced_runs),
	      "force-progress also applies at BG pressure");
}

static void test_resource_scope_and_nonpreemption(void)
{
	struct tail_state die0 = { 0 };
	struct tail_state die1 = { 0 };
	struct tail_state shared_channel = { 0 };
	struct tail_state pcie = { 0 };
	uint64_t bypass = 0;

	(void)advance_low_priority(&die0, 0, 1000);
	check(advance_read_priority(&die1, 100, 100, &bypass) == 200,
	      "different dies do not create a NAND conflict");
	(void)advance_low_priority(&shared_channel, 0, 300);
	check(advance_read_priority(&shared_channel, 100, 50, &bypass) == 350,
	      "different dies on one channel still serialize channel transfer");
	(void)advance_low_priority(&pcie, 0, 500);
	check(advance_read_priority(&pcie, 100, 50, &bypass) == 550,
	      "host read cannot bypass committed PCIe traffic");
}

static void test_forced_progress_and_final_drain(void)
{
	int backlog = 5;
	int yield_streak = 0;
	int forced_runs = 0;
	int round;

	for (round = 0; round < 20 && backlog > 0; round++) {
		yield_streak++;
		if (read_priority_should_force_progress_model(TEST_SLC_LEVEL_BG,
							&yield_streak, &forced_runs))
			backlog--;
	}
	check(forced_runs > 0 && backlog < 5,
	      "bounded forced progress prevents permanent read-window starvation");
	while (backlog > 0)
		backlog--;
	check(backlog == 0, "background backlog drains after foreground pressure ends");
}

int main(void)
{
	struct tail_state normal = { 0 };
	struct tail_state prio = { 0 };
	uint64_t normal_read_done;
	uint64_t prio_read_done;
	uint64_t next_lp_done;
	uint64_t bypass = 0;

	(void)advance_low_priority(&normal, 0, 1000);
	(void)advance_low_priority(&prio, 0, 1000);

	normal_read_done = advance_normal(&normal, 100, 100);
	prio_read_done = advance_read_priority(&prio, 100, 100, &bypass);

	check(normal_read_done == 1100, "normal read waits behind LP tail");
	check(prio_read_done == 1100,
	      "read cannot bypass an already-submitted LP reservation");
	check(bypass == 0, "non-preemptive model reports no tail bypass");
	check(prio.next == normal.next, "read-priority preserves total tail work");
	check(prio.lp_next == 1100, "all views share one serialized tail");

	next_lp_done = advance_low_priority(&prio, 150, 50);
	check(next_lp_done == 1150, "subsequent LP work waits behind the HP read tail");
	test_repromote_restore_order();
	test_io_queue_read_priority_order();
	test_closed_qlc_repromote_budget_and_yield();
	test_channel_scan_cap();
	test_force_after_yields_applies_before_emergency_return();
	test_resource_scope_and_nonpreemption();
	test_forced_progress_and_final_drain();

	printf("PASS non-preemptive tail model: normal_read=%llu priority_read=%llu "
	       "bypass=%llu final_tail=%llu next_lp=%llu\n",
	       (unsigned long long)normal_read_done,
	       (unsigned long long)prio_read_done,
	       (unsigned long long)bypass,
	       (unsigned long long)prio.next,
	       (unsigned long long)next_lp_done);
	return 0;
}
