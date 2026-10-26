// SPDX-License-Identifier: GPL-2.0
/* ringdev: simulated accelerator with submission/completion rings. */
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "ringdev_uapi.h"

#define DEV_NAME	"ringdev"
#define READ_BATCH	32

static unsigned int ring_entries = 64;
module_param(ring_entries, uint, 0444);
MODULE_PARM_DESC(ring_entries, "SQ/CQ entries per context, power of 2 (default 64)");

static unsigned int data_pages = 256;
module_param(data_pages, uint, 0444);
MODULE_PARM_DESC(data_pages, "Data buffer pages per context (default 256)");

static unsigned int tick_us = 100;
module_param(tick_us, uint, 0444);
MODULE_PARM_DESC(tick_us, "Simulated per-job latency in microseconds (default 100)");

struct sq_slot {
	struct ringdev_job job;
	u64 t_submit;
};

/*
 * One per open(). Lifetime: file->private_data holds one kref, the engine
 * holds another while it executes a job. Fields marked [rd.lock] are only
 * touched with rd.lock held.
 */
struct ringdev_ctx {
	struct kref ref;
	struct list_head node;		/* [rd.lock] */
	void *data;			/* vmalloc_user, data_bytes */
	pid_t pid;			/* opener, for debugfs only */

	struct sq_slot *sq;		/* ring_entries slots */
	struct ringdev_cqe *cq;
	u32 sq_head, sq_tail;		/* [rd.lock] free-running */
	u32 cq_head, cq_tail;		/* [rd.lock] writes; read locklessly by wait/poll */
	u32 outstanding;		/* [rd.lock] queued + in flight + unread */
	bool busy;			/* [rd.lock] writes; engine is running our job */
	bool dead;			/* [rd.lock] release() started */

	struct mutex read_lock;		/* serialises read()/RESET vs. CQ head */
	wait_queue_head_t cq_wait;	/* CQ became non-empty */
	wait_queue_head_t idle_wait;	/* busy went false */

	u64 submitted, completed, failed;	/* [rd.lock] */
};

static struct {
	dev_t devt;
	struct cdev cdev;
	struct class *cls;
	struct device *dev;
	struct dentry *dbg;
	struct task_struct *engine;

	struct mutex lock;		/* ctx list, ring indices, counters */
	struct list_head ctxs;		/* round-robin order, rotated by engine */
	unsigned int active;
	atomic_t queued;		/* jobs in all SQs; engine wake condition */
	wait_queue_head_t eng_wait;
	u64 submitted, completed, failed, busy_ns;
} rd;

static u64 data_bytes(void)
{
	return (u64)data_pages << PAGE_SHIFT;
}

static void ctx_free(struct kref *ref)
{
	struct ringdev_ctx *ctx = container_of(ref, struct ringdev_ctx, ref);

	kvfree(ctx->cq);
	kvfree(ctx->sq);
	vfree(ctx->data);
	kfree(ctx);
}

/* ---- job execution (engine only, no locks held) ---- */

static bool range_ok(u64 off, u64 len)
{
	return off <= data_bytes() && len <= data_bytes() - off;
}

static bool overlaps(u64 a, u64 b, u64 len)
{
	return a < b + len && b < a + len;
}

/*
 * Bounds were checked once against the constant data_bytes, so user
 * space racing on the shared buffer can corrupt data but not memory.
 */
static int run_job(struct ringdev_ctx *ctx, const struct ringdev_job *j,
		   u64 *result)
{
	u8 *base = ctx->data;
	u32 done, n, i;

	if (j->flags)
		return -EINVAL;

	switch (j->op) {
	case RINGDEV_OP_FILL:
		if (j->len % 4 || j->dst % 4)
			return -EINVAL;
		if (!range_ok(j->dst, j->len))
			return -ERANGE;
		for (done = 0; done < j->len; done += n) {
			n = min_t(u32, j->len - done, SZ_64K);
			memset32((u32 *)(base + j->dst + done), (u32)j->arg, n / 4);
			cond_resched();
		}
		return 0;
	case RINGDEV_OP_COPY:
		if (!range_ok(j->src0, j->len) || !range_ok(j->dst, j->len))
			return -ERANGE;
		if (overlaps(j->src0, j->dst, j->len))
			return -EINVAL;
		for (done = 0; done < j->len; done += n) {
			n = min_t(u32, j->len - done, SZ_64K);
			memcpy(base + j->dst + done, base + j->src0 + done, n);
			cond_resched();
		}
		return 0;
	case RINGDEV_OP_VADD_U32:
		if (j->len % 4 || j->src0 % 4 || j->src1 % 4 || j->dst % 4)
			return -EINVAL;
		if (!range_ok(j->src0, j->len) || !range_ok(j->src1, j->len) ||
		    !range_ok(j->dst, j->len))
			return -ERANGE;
		for (done = 0; done < j->len; done += n) {
			const u32 *a = (u32 *)(base + j->src0 + done);
			const u32 *b = (u32 *)(base + j->src1 + done);
			u32 *d = (u32 *)(base + j->dst + done);

			n = min_t(u32, j->len - done, SZ_64K);
			for (i = 0; i < n / 4; i++)
				d[i] = a[i] + b[i];
			cond_resched();
		}
		return 0;
	case RINGDEV_OP_CHECKSUM: {
		u64 sum = 0;

		if (!range_ok(j->src0, j->len))
			return -ERANGE;
		for (done = 0; done < j->len; done += n) {
			const u8 *p = base + j->src0 + done;

			n = min_t(u32, j->len - done, SZ_64K);
			for (i = 0; i < n; i++)
				sum += p[i];
			cond_resched();
		}
		*result = sum;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

/* Called with rd.lock held. Rotating the list gives round-robin. */
static struct ringdev_ctx *pick_ctx(struct sq_slot *slot)
{
	struct ringdev_ctx *ctx;

	list_for_each_entry(ctx, &rd.ctxs, node) {
		if (ctx->dead || ctx->sq_head == ctx->sq_tail)
			continue;

		*slot = ctx->sq[ctx->sq_head++ & (ring_entries - 1)];
		atomic_dec(&rd.queued);
		WRITE_ONCE(ctx->busy, true);
		kref_get(&ctx->ref);
		list_move_tail(&ctx->node, &rd.ctxs);
		return ctx;
	}
	return NULL;
}

static int engine_thread(void *unused)
{
	while (!kthread_should_stop()) {
		struct ringdev_ctx *ctx;
		struct ringdev_cqe cqe = {};
		struct sq_slot slot;
		u64 t0, t1;

		wait_event_interruptible(rd.eng_wait,
					 atomic_read(&rd.queued) || kthread_should_stop());

		mutex_lock(&rd.lock);
		ctx = pick_ctx(&slot);
		mutex_unlock(&rd.lock);
		if (!ctx)
			continue;

		t0 = ktime_get_ns();
		if (tick_us)
			fsleep(tick_us);	/* simulated hardware latency */
		cqe.id = slot.job.id;
		cqe.status = run_job(ctx, &slot.job, &cqe.result);
		t1 = ktime_get_ns();
		cqe.latency_ns = t1 - slot.t_submit;

		mutex_lock(&rd.lock);
		/* flow control guarantees a free CQ slot */
		ctx->cq[ctx->cq_tail & (ring_entries - 1)] = cqe;
		WRITE_ONCE(ctx->cq_tail, ctx->cq_tail + 1);
		WRITE_ONCE(ctx->busy, false);
		ctx->completed++;
		rd.completed++;
		rd.busy_ns += t1 - t0;
		if (cqe.status) {
			ctx->failed++;
			rd.failed++;
		}
		mutex_unlock(&rd.lock);

		wake_up(&ctx->cq_wait);
		wake_up(&ctx->idle_wait);
		kref_put(&ctx->ref, ctx_free);
	}
	return 0;
}

/* ---- file operations ---- */

static int ringdev_open(struct inode *inode, struct file *file)
{
	struct ringdev_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* vmalloc_user() zeroes, so user space never sees stale kernel memory */
	ctx->data = vmalloc_user(data_bytes());
	ctx->sq = kvcalloc(ring_entries, sizeof(*ctx->sq), GFP_KERNEL);
	ctx->cq = kvcalloc(ring_entries, sizeof(*ctx->cq), GFP_KERNEL);
	if (!ctx->data || !ctx->sq || !ctx->cq) {
		kvfree(ctx->cq);
		kvfree(ctx->sq);
		vfree(ctx->data);
		kfree(ctx);
		return -ENOMEM;
	}

	ctx->pid = task_tgid_nr(current);
	kref_init(&ctx->ref);
	mutex_init(&ctx->read_lock);
	init_waitqueue_head(&ctx->cq_wait);
	init_waitqueue_head(&ctx->idle_wait);

	mutex_lock(&rd.lock);
	list_add_tail(&ctx->node, &rd.ctxs);
	rd.active++;
	mutex_unlock(&rd.lock);

	file->private_data = ctx;
	return 0;
}

static int ringdev_release(struct inode *inode, struct file *file)
{
	struct ringdev_ctx *ctx = file->private_data;

	mutex_lock(&rd.lock);
	ctx->dead = true;
	atomic_sub(ctx->sq_tail - ctx->sq_head, &rd.queued);	/* drop unstarted jobs */
	ctx->sq_head = ctx->sq_tail;
	mutex_unlock(&rd.lock);

	/*
	 * The engine's kref would keep the memory valid; waiting keeps
	 * "active contexts == 0" honest and nothing runs after close.
	 */
	wait_event(ctx->idle_wait, !READ_ONCE(ctx->busy));

	mutex_lock(&rd.lock);
	list_del(&ctx->node);
	rd.active--;
	mutex_unlock(&rd.lock);

	kref_put(&ctx->ref, ctx_free);
	return 0;
}

/*
 * The vma holds a reference on the file, so release() (and the vfree in it)
 * cannot run while the buffer is still mapped; the mapped pages are also
 * refcounted by vm_insert_page().
 */
static int ringdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ringdev_ctx *ctx = file->private_data;

	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != data_bytes() ||
	    !(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	return remap_vmalloc_range(vma, ctx->data, 0);
}

static ssize_t ringdev_read(struct file *file, char __user *buf, size_t size,
			    loff_t *ppos)
{
	struct ringdev_ctx *ctx = file->private_data;
	struct ringdev_cqe tmp[READ_BATCH];
	u32 avail, n, i;
	int ret;

	if (size < sizeof(struct ringdev_cqe))
		return -EINVAL;

	for (;;) {
		if (!(file->f_flags & O_NONBLOCK)) {
			ret = wait_event_interruptible(ctx->cq_wait,
					READ_ONCE(ctx->cq_head) != READ_ONCE(ctx->cq_tail));
			if (ret)
				return ret;
		}

		mutex_lock(&ctx->read_lock);
		mutex_lock(&rd.lock);
		avail = ctx->cq_tail - ctx->cq_head;
		n = min3(avail, (u32)(size / sizeof(tmp[0])), (u32)READ_BATCH);
		for (i = 0; i < n; i++)
			tmp[i] = ctx->cq[(ctx->cq_head + i) & (ring_entries - 1)];
		mutex_unlock(&rd.lock);

		if (n) {
			/* Copy before consuming so a bad buffer loses nothing. */
			if (copy_to_user(buf, tmp, n * sizeof(tmp[0]))) {
				mutex_unlock(&ctx->read_lock);
				return -EFAULT;
			}
			mutex_lock(&rd.lock);
			WRITE_ONCE(ctx->cq_head, ctx->cq_head + n);
			ctx->outstanding -= n;
			mutex_unlock(&rd.lock);
			mutex_unlock(&ctx->read_lock);
			return n * sizeof(tmp[0]);
		}
		mutex_unlock(&ctx->read_lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/* Another reader took the entries; wait again. */
	}
}

static __poll_t ringdev_poll(struct file *file, poll_table *wait)
{
	struct ringdev_ctx *ctx = file->private_data;

	poll_wait(file, &ctx->cq_wait, wait);
	if (READ_ONCE(ctx->cq_head) != READ_ONCE(ctx->cq_tail))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static long ringdev_submit(struct ringdev_ctx *ctx, void __user *uarg)
{
	struct ringdev_submit sub;
	struct ringdev_job *jobs;
	u32 n, i;
	u64 now;
	long ret;

	if (copy_from_user(&sub, uarg, sizeof(sub)))
		return -EFAULT;
	if (!sub.count || sub.count > ring_entries || sub.reserved)
		return -EINVAL;

	/*
	 * Copy the descriptors into kernel memory once; only this copy is
	 * ever validated and executed.
	 */
	jobs = kvmalloc_array(sub.count, sizeof(*jobs), GFP_KERNEL);
	if (!jobs)
		return -ENOMEM;
	if (copy_from_user(jobs, u64_to_user_ptr(sub.jobs_ptr),
			   sub.count * sizeof(*jobs))) {
		ret = -EFAULT;
		goto out;
	}

	now = ktime_get_ns();
	mutex_lock(&rd.lock);
	n = min(sub.count, ring_entries - ctx->outstanding);
	if (!n) {
		mutex_unlock(&rd.lock);
		ret = -EAGAIN;
		goto out;
	}
	for (i = 0; i < n; i++) {
		struct sq_slot *s = &ctx->sq[ctx->sq_tail++ & (ring_entries - 1)];

		s->job = jobs[i];
		s->t_submit = now;
	}
	ctx->outstanding += n;
	ctx->submitted += n;
	atomic_add(n, &rd.queued);
	rd.submitted += n;
	mutex_unlock(&rd.lock);
	wake_up(&rd.eng_wait);

	sub.count = n;
	ret = copy_to_user(uarg, &sub, sizeof(sub)) ? -EFAULT : 0;
out:
	kvfree(jobs);
	return ret;
}

/* Drop queued jobs and unread completions; an in-flight job still lands. */
static long ringdev_reset(struct ringdev_ctx *ctx)
{
	mutex_lock(&ctx->read_lock);
	mutex_lock(&rd.lock);
	atomic_sub(ctx->sq_tail - ctx->sq_head, &rd.queued);
	ctx->sq_head = ctx->sq_tail;
	WRITE_ONCE(ctx->cq_head, ctx->cq_tail);
	ctx->outstanding = ctx->busy;
	mutex_unlock(&rd.lock);
	mutex_unlock(&ctx->read_lock);
	return 0;
}

static long ringdev_get_stats(struct ringdev_ctx *ctx, void __user *uarg)
{
	struct ringdev_stats st = {};

	mutex_lock(&rd.lock);
	st.ctx_submitted = ctx->submitted;
	st.ctx_completed = ctx->completed;
	st.ctx_failed = ctx->failed;
	st.ctx_outstanding = ctx->outstanding;
	st.submitted = rd.submitted;
	st.completed = rd.completed;
	st.failed = rd.failed;
	st.busy_ns = rd.busy_ns;
	st.active_ctx = rd.active;
	mutex_unlock(&rd.lock);

	return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
}

static long ringdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ringdev_ctx *ctx = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case RINGDEV_IOC_GET_INFO: {
		struct ringdev_info info = {
			.version = RINGDEV_VERSION,
			.ring_entries = ring_entries,
			.data_bytes = data_bytes(),
			.tick_us = tick_us,
		};

		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case RINGDEV_IOC_SUBMIT:
		return ringdev_submit(ctx, uarg);
	case RINGDEV_IOC_RESET:
		return ringdev_reset(ctx);
	case RINGDEV_IOC_GET_STATS:
		return ringdev_get_stats(ctx, uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ringdev_fops = {
	.owner		= THIS_MODULE,
	.open		= ringdev_open,
	.release	= ringdev_release,
	.read		= ringdev_read,
	.poll		= ringdev_poll,
	.unlocked_ioctl	= ringdev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.mmap		= ringdev_mmap,
};

static int stats_show(struct seq_file *m, void *unused)
{
	struct ringdev_ctx *ctx;

	mutex_lock(&rd.lock);
	seq_printf(m, "active_ctx %u\nqueued %d\nsubmitted %llu\ncompleted %llu\nfailed %llu\nbusy_ns %llu\n",
		   rd.active, atomic_read(&rd.queued), rd.submitted, rd.completed,
		   rd.failed, rd.busy_ns);
	seq_puts(m, "\npid submitted completed failed outstanding busy\n");
	list_for_each_entry(ctx, &rd.ctxs, node)
		seq_printf(m, "%d %llu %llu %llu %u %d\n", ctx->pid, ctx->submitted,
			   ctx->completed, ctx->failed, ctx->outstanding, ctx->busy);
	mutex_unlock(&rd.lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

/* World-accessible node so the tests don't need root. */
static char *ringdev_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static int __init ringdev_init(void)
{
	int ret;

	if (!is_power_of_2(ring_entries) || ring_entries > RINGDEV_MAX_RING ||
	    !data_pages || data_pages > 16384)
		return -EINVAL;

	mutex_init(&rd.lock);
	INIT_LIST_HEAD(&rd.ctxs);
	atomic_set(&rd.queued, 0);
	init_waitqueue_head(&rd.eng_wait);

	ret = alloc_chrdev_region(&rd.devt, 0, 1, DEV_NAME);
	if (ret)
		return ret;

	rd.engine = kthread_run(engine_thread, NULL, "ringdev-engine");
	if (IS_ERR(rd.engine)) {
		ret = PTR_ERR(rd.engine);
		goto err_region;
	}

	cdev_init(&rd.cdev, &ringdev_fops);
	rd.cdev.owner = THIS_MODULE;
	ret = cdev_add(&rd.cdev, rd.devt, 1);
	if (ret)
		goto err_engine;

	rd.cls = class_create(DEV_NAME);
	if (IS_ERR(rd.cls)) {
		ret = PTR_ERR(rd.cls);
		goto err_cdev;
	}
	rd.cls->devnode = ringdev_devnode;

	rd.dev = device_create(rd.cls, NULL, rd.devt, NULL, DEV_NAME);
	if (IS_ERR(rd.dev)) {
		ret = PTR_ERR(rd.dev);
		goto err_class;
	}

	rd.dbg = debugfs_create_dir(DEV_NAME, NULL);
	debugfs_create_file("stats", 0444, rd.dbg, NULL, &stats_fops);

	pr_info("ringdev: loaded ring_entries=%u data_pages=%u tick_us=%u\n",
		ring_entries, data_pages, tick_us);
	return 0;

err_class:
	class_destroy(rd.cls);
err_cdev:
	cdev_del(&rd.cdev);
err_engine:
	kthread_stop(rd.engine);
err_region:
	unregister_chrdev_region(rd.devt, 1);
	return ret;
}

/*
 * Open files pin the module, so no context exists here; just make sure no
 * new open can start before the engine goes away.
 */
static void __exit ringdev_exit(void)
{
	debugfs_remove_recursive(rd.dbg);
	device_destroy(rd.cls, rd.devt);
	class_destroy(rd.cls);
	cdev_del(&rd.cdev);
	kthread_stop(rd.engine);
	WARN_ON(!list_empty(&rd.ctxs));
	unregister_chrdev_region(rd.devt, 1);
	pr_info("ringdev: unloaded\n");
}

module_init(ringdev_init);
module_exit(ringdev_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simulated ring-based accelerator");
