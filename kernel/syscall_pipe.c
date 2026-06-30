/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/bug.h"
#include "ember/bkl.h"

/*
 * pipe_wait_readable - block until pipe has data or all writers are gone.
 *
 * Returns:
 * 1        -- pipe is ready (has data or EOF)
 * -EAGAIN   -- O_NONBLOCK set and pipe is empty
 * -EINTR    -- signal pending.
 */
int
pipe_wait_readable(pipe_t * p, file_desc_t * desc)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	for (;;) {
		spin_lock(&p->lock);
		int ready = (p->count > 0 || p->writers == 0);
		spin_unlock(&p->lock);
		if (ready)
			return 1;
		if (desc->flags & O_NONBLOCK)
			return -EAGAIN;
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		if (current_proc) {
			uint32_t pend =
			    current_proc->sig_pending & ~current_proc->sig_mask;
			if (pend) {
				spin_unlock_irqrestore(&sched_lock, sf);
				return -EINTR;
			}
		}
		current_proc->state = PROC_SLEEPING;
		current_proc->wait_chan = p->wake_chan;
		spin_unlock_irqrestore(&sched_lock, sf);
		/*
		 * Recheck after registering sleep to prevent lost wakeup:
		 * the pipe state may have changed between our check and the
		 * moment we set SLEEPING.
		 */
		{
			spin_lock(&p->lock);
			int recheck = (p->count > 0 || p->writers == 0);
			spin_unlock(&p->lock);
			if (recheck) {
				sf = spin_lock_irqsave(&sched_lock);
				if (current_proc->state == PROC_SLEEPING)
					current_proc->state = PROC_READY;
				spin_unlock_irqrestore(&sched_lock, sf);
				continue;
			}
		}
		schedule();
	}
}

/*
 * pipe_wait_writable - block until pipe has space or all readers are gone.
 *
 * Returns:
 * 1        -- pipe is ready (has space or no readers)
 * -EAGAIN   -- O_NONBLOCK set and pipe is full
 * -EINTR    -- signal pending.
 */
int
pipe_wait_writable(pipe_t * p, file_desc_t * desc)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	for (;;) {
		spin_lock(&p->lock);
		int ready = (p->count < PIPE_BUF_SIZE || p->readers == 0);
		spin_unlock(&p->lock);
		if (ready)
			return 1;
		if (desc->flags & O_NONBLOCK)
			return -EAGAIN;
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		if (current_proc) {
			uint32_t pend =
			    current_proc->sig_pending & ~current_proc->sig_mask;
			if (pend) {
				spin_unlock_irqrestore(&sched_lock, sf);
				return -EINTR;
			}
		}
		current_proc->state = PROC_SLEEPING;
		current_proc->wait_chan = p->wake_chan;
		spin_unlock_irqrestore(&sched_lock, sf);
		/* Recheck after registering sleep to prevent lost wakeup. */
		{
			spin_lock(&p->lock);
			int recheck = (p->count < PIPE_BUF_SIZE
				       || p->readers == 0);
			spin_unlock(&p->lock);
			if (recheck) {
				sf = spin_lock_irqsave(&sched_lock);
				if (current_proc->state == PROC_SLEEPING)
					current_proc->state = PROC_READY;
				spin_unlock_irqrestore(&sched_lock, sf);
				continue;
			}
		}
		schedule();
	}
}

/* Check if pipe has no readers, under p->lock. */
int
pipe_no_readers(pipe_t * p)
{
	spin_lock(&p->lock);
	int r = (p->readers == 0);
	spin_unlock(&p->lock);
	return r;
}

/* Check if pipe is empty, under p->lock. */
int
pipe_is_empty(pipe_t * p)
{
	spin_lock(&p->lock);
	int r = (p->count == 0);
	spin_unlock(&p->lock);
	return r;
}
