/*
 * Generic pidhash and scalable, time-bounded PID allocator
 *
 * (C) 2002-2003 William Irwin, IBM
 * (C) 2004 William Irwin, Oracle
 * (C) 2002-2004 Ingo Molnar, Red Hat
 *
 * pid-structures are backing objects for tasks sharing a given ID to chain
 * against. There is very little to them aside from hashing them and
 * parking tasks using given ID's on a list.
 *
 * The hash is always changed with the tasklist_lock write-acquired,
 * and the hash is only accessed with the tasklist_lock at least
 * read-acquired, so there's no additional SMP locking needed here.
 *
 * We have a list of bitmap pages, which bitmaps represent the PID space.
 * Allocating and freeing PIDs is completely lockless. The worst-case
 * allocation scenario when all but one out of 1 million PIDs possible are
 * allocated already: the scanning of 32 list entries and at most PAGE_SIZE
 * bytes. The typical fastpath is a single successful setbit. Freeing is O(1).
 *
 * Pid namespaces:
 *    (C) 2007 Pavel Emelyanov <xemul@openvz.org>, OpenVZ, SWsoft Inc.
 *    (C) 2007 Sukadev Bhattiprolu <sukadev@us.ibm.com>, IBM
 *     Many thanks to Oleg Nesterov for comments and help
 *
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/hash.h>
#include <linux/pid_namespace.h>
#include <linux/init_task.h>
#include <linux/syscalls.h>

#define pid_hashfn(nr, ns)	\
	hash_long((unsigned long)nr + (unsigned long)ns, pidhash_shift)
static struct hlist_head *pid_hash;
static int pidhash_shift;
struct pid init_struct_pid = INIT_STRUCT_PID;
static struct kmem_cache *pid_ns_cachep;

int pid_max = PID_MAX_DEFAULT;

#define RESERVED_PIDS		300

int pid_max_min = RESERVED_PIDS + 1;
int pid_max_max = PID_MAX_LIMIT;

#define BITS_PER_PAGE		(PAGE_SIZE*8)
#define BITS_PER_PAGE_MASK	(BITS_PER_PAGE-1)

static inline int mk_pid(struct pid_namespace *pid_ns,
		struct pidmap *map, int off)
{
	return (map - pid_ns->pidmap)*BITS_PER_PAGE + off;
}

#define find_next_offset(map, off)					\
		find_next_zero_bit((map)->page, BITS_PER_PAGE, off)

/*
 * PID-map pages start out as NULL, they get allocated upon
 * first use and are never deallocated. This way a low pid_max
 * value does not cause lots of bitmaps to be allocated, but
 * the scheme scales to up to 4 million PIDs, runtime.
 */
struct pid_namespace init_pid_ns = {
	.kref = {
		.refcount       = ATOMIC_INIT(2),
	},
	.pidmap = {
		[ 0 ... PIDMAP_ENTRIES-1] = { ATOMIC_INIT(BITS_PER_PAGE), NULL }
	},
	.last_pid = 0,
	.level = 0,
	.child_reaper = &init_task,
};
EXPORT_SYMBOL_GPL(init_pid_ns);

int is_container_init(struct task_struct *tsk)
{
	int ret = 0;
	struct pid *pid;

	rcu_read_lock();
	pid = task_pid(tsk);
	if (pid != NULL && pid->numbers[pid->level].nr == 1)
		ret = 1;
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL(is_container_init);

/*
 * Note: disable interrupts while the pidmap_lock is held as an
 * interrupt might come in and do read_lock(&tasklist_lock).
 *
 * If we don't disable interrupts there is a nasty deadlock between
 * detach_pid()->free_pid() and another cpu that does
 * spin_lock(&pidmap_lock) followed by an interrupt routine that does
 * read_lock(&tasklist_lock);
 *
 * After we clean up the tasklist_lock and know there are no
 * irq handlers that take it we can leave the interrupts enabled.
 * For now it is easier to be safe than to prove it can't happen.
 */

static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(pidmap_lock);

static fastcall void free_pidmap(struct pid_namespace *pid_ns, int pid)
{
	struct pidmap *map = pid_ns->pidmap + pid / BITS_PER_PAGE;
	int offset = pid & BITS_PER_PAGE_MASK;

	clear_bit(offset, map->page);
	atomic_inc(&map->nr_free);
}

static int alloc_pidmap(struct pid_namespace *pid_ns)
{
	int i, offset, max_scan, pid, last = pid_ns->last_pid;
	struct pidmap *map;

	pid = last + 1;
	if (pid >= pid_max)
		pid = RESERVED_PIDS;
	offset = pid & BITS_PER_PAGE_MASK;
	map = &pid_ns->pidmap[pid/BITS_PER_PAGE];
	max_scan = (pid_max + BITS_PER_PAGE - 1)/BITS_PER_PAGE - !offset;
	for (i = 0; i <= max_scan; ++i) {
		if (unlikely(!map->page)) {
			void *page = kzalloc(PAGE_SIZE, GFP_KERNEL);
			/*
			 * Free the page if someone raced with us
			 * installing it:
			 */
			spin_lock_irq(&pidmap_lock);
			if (map->page)
				kfree(page);
			else
				map->page = page;
			spin_unlock_irq(&pidmap_lock);
			if (unlikely(!map->page))
				break;
		}
		if (likely(atomic_read(&map->nr_free))) {
			do {
				if (!test_and_set_bit(offset, map->page)) {
					atomic_dec(&map->nr_free);
					pid_ns->last_pid = pid;
					return pid;
				}
				offset = find_next_offset(map, offset);
				pid = mk_pid(pid_ns, map, offset);
			/*
			 * find_next_offset() found a bit, the pid from it
			 * is in-bounds, and if we fell back to the last
			 * bitmap block and the final block was the same
			 * as the starting point, pid is before last_pid.
			 */
			} while (offset < BITS_PER_PAGE && pid < pid_max &&
					(i != max_scan || pid < last ||
					    !((last+1) & BITS_PER_PAGE_MASK)));
		}
		if (map < &pid_ns->pidmap[(pid_max-1)/BITS_PER_PAGE]) {
			++map;
			offset = 0;
		} else {
			map = &pid_ns->pidmap[0];
			offset = RESERVED_PIDS;
			if (unlikely(last == offset))
				break;
		}
		pid = mk_pid(pid_ns, map, offset);
	}
	return -1;
}

static int next_pidmap(struct pid_namespace *pid_ns, int last)
{
	int offset;
	struct pidmap *map, *end;

	offset = (last + 1) & BITS_PER_PAGE_MASK;
	map = &pid_ns->pidmap[(last + 1)/BITS_PER_PAGE];
	end = &pid_ns->pidmap[PIDMAP_ENTRIES];
	for (; map < end; map++, offset = 0) {
		if (unlikely(!map->page))
			continue;
		offset = find_next_bit((map)->page, BITS_PER_PAGE, offset);
		if (offset < BITS_PER_PAGE)
			return mk_pid(pid_ns, map, offset);
	}
	return -1;
}

fastcall void put_pid(struct pid *pid)
{
	struct pid_namespace *ns;

	if (!pid)
		return;

	ns = pid->numbers[pid->level].ns;
	if ((atomic_read(&pid->count) == 1) ||
	     atomic_dec_and_test(&pid->count)) {
		kmem_cache_free(ns->pid_cachep, pid);
		put_pid_ns(ns);
	}
}
EXPORT_SYMBOL_GPL(put_pid);

static void delayed_put_pid(struct rcu_head *rhp)
{
	struct pid *pid = container_of(rhp, struct pid, rcu);
	put_pid(pid);
}

fastcall void free_pid(struct pid *pid)
{
	/* We can be called with write_lock_irq(&tasklist_lock) held */
	int i;
	unsigned long flags;

	spin_lock_irqsave(&pidmap_lock, flags);
	for (i = 0; i <= pid->level; i++)
		hlist_del_rcu(&pid->numbers[i].pid_chain);
	spin_unlock_irqrestore(&pidmap_lock, flags);

	for (i = 0; i <= pid->level; i++)
		free_pidmap(pid->numbers[i].ns, pid->numbers[i].nr);

	call_rcu(&pid->rcu, delayed_put_pid);
}

struct pid *alloc_pid(struct pid_namespace *ns)
{
	struct pid *pid;
	enum pid_type type;
	int i, nr;
	struct pid_namespace *tmp;
	struct upid *upid;

	pid = kmem_cache_alloc(ns->pid_cachep, GFP_KERNEL);
	if (!pid)
		goto out;

	tmp = ns;
	for (i = ns->level; i >= 0; i--) {
		nr = alloc_pidmap(tmp);
		if (nr < 0)
			goto out_free;

		pid->numbers[i].nr = nr;
		pid->numbers[i].ns = tmp;
		tmp = tmp->parent;
	}

	get_pid_ns(ns);
	pid->level = ns->level;
	atomic_set(&pid->count, 1);
	for (type = 0; type < PIDTYPE_MAX; ++type)
		INIT_HLIST_HEAD(&pid->tasks[type]);

	spin_lock_irq(&pidmap_lock);
	for (i = ns->level; i >= 0; i--) {
		upid = &pid->numbers[i];
		hlist_add_head_rcu(&upid->pid_chain,
				&pid_hash[pid_hashfn(upid->nr, upid->ns)]);
	}
	spin_unlock_irq(&pidmap_lock);

out:
	return pid;

out_free:
	for (i++; i <= ns->level; i++)
		free_pidmap(pid->numbers[i].ns, pid->numbers[i].nr);

	kmem_cache_free(ns->pid_cachep, pid);
	pid = NULL;
	goto out;
}

struct pid * fastcall find_pid_ns(int nr, struct pid_namespace *ns)
{
	struct hlist_node *elem;
	struct upid *pnr;

	hlist_for_each_entry_rcu(pnr, elem,
			&pid_hash[pid_hashfn(nr, ns)], pid_chain)
		if (pnr->nr == nr && pnr->ns == ns)
			return container_of(pnr, struct pid,
					numbers[ns->level]);

	return NULL;
}
EXPORT_SYMBOL_GPL(find_pid_ns);

struct pid *find_vpid(int nr)
{
	return find_pid_ns(nr, current->nsproxy->pid_ns);
}
EXPORT_SYMBOL_GPL(find_vpid);

struct pid *find_pid(int nr)
{
	return find_pid_ns(nr, &init_pid_ns);
}
EXPORT_SYMBOL_GPL(find_pid);

/*
 * attach_pid() must be called with the tasklist_lock write-held.
 */
int fastcall attach_pid(struct task_struct *task, enum pid_type type,
		struct pid *pid)
{
	struct pid_link *link;

	link = &task->pids[type];
	link->pid = pid;
	hlist_add_head_rcu(&link->node, &pid->tasks[type]);

	return 0;
}

void fastcall detach_pid(struct task_struct *task, enum pid_type type)
{
	struct pid_link *link;
	struct pid *pid;
	int tmp;

	link = &task->pids[type];
	pid = link->pid;

	hlist_del_rcu(&link->node);
	link->pid = NULL;

	for (tmp = PIDTYPE_MAX; --tmp >= 0; )
		if (!hlist_empty(&pid->tasks[tmp]))
			return;

	free_pid(pid);
}

/* transfer_pid is an optimization of attach_pid(new), detach_pid(old) */
void fastcall transfer_pid(struct task_struct *old, struct task_struct *new,
			   enum pid_type type)
{
	new->pids[type].pid = old->pids[type].pid;
	hlist_replace_rcu(&old->pids[type].node, &new->pids[type].node);
	old->pids[type].pid = NULL;
}

struct task_struct * fastcall pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result = NULL;
	if (pid) {
		struct hlist_node *first;
		first = rcu_dereference(pid->tasks[type].first);
		if (first)
			result = hlist_entry(first, struct task_struct, pids[(type)].node);
	}
	return result;
}

/*
 * Must be called under rcu_read_lock() or with tasklist_lock read-held.
 */
struct task_struct *find_task_by_pid_type_ns(int type, int nr,
		struct pid_namespace *ns)
{
	return pid_task(find_pid_ns(nr, ns), type);
}

EXPORT_SYMBOL(find_task_by_pid_type_ns);

struct task_struct *find_task_by_pid(pid_t nr)
{
	return find_task_by_pid_type_ns(PIDTYPE_PID, nr, &init_pid_ns);
}
EXPORT_SYMBOL(find_task_by_pid);

struct task_struct *find_task_by_vpid(pid_t vnr)
{
	return find_task_by_pid_type_ns(PIDTYPE_PID, vnr,
			current->nsproxy->pid_ns);
}
EXPORT_SYMBOL(find_task_by_vpid);

struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns)
{
	return find_task_by_pid_type_ns(PIDTYPE_PID, nr, ns);
}
EXPORT_SYMBOL(find_task_by_pid_ns);

struct pid *get_task_pid(struct task_struct *task, enum pid_type type)
{
	struct pid *pid;
	rcu_read_lock();
	pid = get_pid(task->pids[type].pid);
	rcu_read_unlock();
	return pid;
}

struct task_struct *fastcall get_pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result;
	rcu_read_lock();
	result = pid_task(pid, type);
	if (result)
		get_task_struct(result);
	rcu_read_unlock();
	return result;
}

struct pid *find_get_pid(pid_t nr)
{
	struct pid *pid;

	rcu_read_lock();
	pid = get_pid(find_vpid(nr));
	rcu_read_unlock();

	return pid;
}

pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	struct upid *upid;
	pid_t nr = 0;

	if (pid && ns->level <= pid->level) {
		upid = &pid->numbers[ns->level];
		if (upid->ns == ns)
			nr = upid->nr;
	}
	return nr;
}

pid_t task_pid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return pid_nr_ns(task_pid(tsk), ns);
}
EXPORT_SYMBOL(task_pid_nr_ns);

pid_t task_tgid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return pid_nr_ns(task_tgid(tsk), ns);
}
EXPORT_SYMBOL(task_tgid_nr_ns);

pid_t task_pgrp_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return pid_nr_ns(task_pgrp(tsk), ns);
}
EXPORT_SYMBOL(task_pgrp_nr_ns);

pid_t task_session_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return pid_nr_ns(task_session(tsk), ns);
}
EXPORT_SYMBOL(task_session_nr_ns);

/*
 * Used by proc to find the first pid that is greater then or equal to nr.
 *
 * If there is a pid at nr this function is exactly the same as find_pid.
 */
struct pid *find_ge_pid(int nr, struct pid_namespace *ns)
{
	struct pid *pid;

	do {
		pid = find_pid_ns(nr, ns);
		if (pid)
			break;
		nr = next_pidmap(ns, nr);
	} while (nr > 0);

	return pid;
}
EXPORT_SYMBOL_GPL(find_get_pid);

struct pid_cache {
	int nr_ids;
	char name[16];
	struct kmem_cache *cachep;
	struct list_head list;
};

static LIST_HEAD(pid_caches_lh);
static DEFINE_MUTEX(pid_caches_mutex);

/*
 * creates the kmem cache to allocate pids from.
 * @nr_ids: the number of numerical ids this pid will have to carry
 */

static struct kmem_cache *create_pid_cachep(int nr_ids)
{
	struct pid_cache *pcache;
	struct kmem_cache *cachep;

	mutex_lock(&pid_caches_mutex);
	list_for_each_entry (pcache, &pid_caches_lh, list)
		if (pcache->nr_ids == nr_ids)
			goto out;

	pcache = kmalloc(sizeof(struct pid_cache), GFP_KERNEL);
	if (pcache == NULL)
		goto err_alloc;

	snprintf(pcache->name, sizeof(pcache->name), "pid_%d", nr_ids);
	cachep = kmem_cache_create(pcache->name,
			sizeof(struct pid) + (nr_ids - 1) * sizeof(struct upid),
			0, SLAB_HWCACHE_ALIGN, NULL);
	if (cachep == NULL)
		goto err_cachep;

	pcache->nr_ids = nr_ids;
	pcache->cachep = cachep;
	list_add(&pcache->list, &pid_caches_lh);
out:
	mutex_unlock(&pid_caches_mutex);
	return pcache->cachep;

err_cachep:
	kfree(pcache);
err_alloc:
	mutex_unlock(&pid_caches_mutex);
	return NULL;
}

#ifdef CONFIG_PID_NS
static struct pid_namespace *create_pid_namespace(int level)
{
	struct pid_namespace *ns;
	int i;

	ns = kmem_cache_alloc(pid_ns_cachep, GFP_KERNEL);
	if (ns == NULL)
		goto out;

	ns->pidmap[0].page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!ns->pidmap[0].page)
		goto out_free;

	ns->pid_cachep = create_pid_cachep(level + 1);
	if (ns->pid_cachep == NULL)
		goto out_free_map;

	kref_init(&ns->kref);
	ns->last_pid = 0;
	ns->child_reaper = NULL;
	ns->level = level;

	set_bit(0, ns->pidmap[0].page);
	atomic_set(&ns->pidmap[0].nr_free, BITS_PER_PAGE - 1);

	for (i = 1; i < PIDMAP_ENTRIES; i++) {
		ns->pidmap[i].page = 0;
		atomic_set(&ns->pidmap[i].nr_free, BITS_PER_PAGE);
	}

	return ns;

out_free_map:
	kfree(ns->pidmap[0].page);
out_free:
	kmem_cache_free(pid_ns_cachep, ns);
out:
	return ERR_PTR(-ENOMEM);
}

static void destroy_pid_namespace(struct pid_namespace *ns)
{
	int i;

	for (i = 0; i < PIDMAP_ENTRIES; i++)
		kfree(ns->pidmap[i].page);
	kmem_cache_free(pid_ns_cachep, ns);
}

struct pid_namespace *copy_pid_ns(unsigned long flags, struct pid_namespace *old_ns)
{
	struct pid_namespace *new_ns;

	BUG_ON(!old_ns);
	new_ns = get_pid_ns(old_ns);
	if (!(flags & CLONE_NEWPID))
		goto out;

	new_ns = ERR_PTR(-EINVAL);
	if (flags & CLONE_THREAD)
		goto out_put;

	new_ns = create_pid_namespace(old_ns->level + 1);
	if (!IS_ERR(new_ns))
		new_ns->parent = get_pid_ns(old_ns);

out_put:
	put_pid_ns(old_ns);
out:
	return new_ns;
}

void free_pid_ns(struct kref *kref)
{
	struct pid_namespace *ns, *parent;

	ns = container_of(kref, struct pid_namespace, kref);

	parent = ns->parent;
	destroy_pid_namespace(ns);

	if (parent != NULL)
		put_pid_ns(parent);
}
#endif /* CONFIG_PID_NS */

void zap_pid_ns_processes(struct pid_namespace *pid_ns)
{
	int nr;
	int rc;

	/*
	 * The last thread in the cgroup-init thread group is terminating.
	 * Find remaining pid_ts in the namespace, signal and wait for them
	 * to exit.
	 *
	 * Note:  This signals each threads in the namespace - even those that
	 * 	  belong to the same thread group, To avoid this, we would have
	 * 	  to walk the entire tasklist looking a processes in this
	 * 	  namespace, but that could be unnecessarily expensive if the
	 * 	  pid namespace has just a few processes. Or we need to
	 * 	  maintain a tasklist for each pid namespace.
	 *
	 */
	read_lock(&tasklist_lock);
	nr = next_pidmap(pid_ns, 1);
	while (nr > 0) {
		kill_proc_info(SIGKILL, SEND_SIG_PRIV, nr);
		nr = next_pidmap(pid_ns, nr);
	}
	read_unlock(&tasklist_lock);

	do {
		clear_thread_flag(TIF_SIGPENDING);
		rc = sys_wait4(-1, NULL, __WALL, NULL);
	} while (rc != -ECHILD);


	/* Child reaper for the pid namespace is going away */
	pid_ns->child_reaper = NULL;
	return;
}

/*
 * The pid hash table is scaled according to the amount of memory in the
 * machine.  From a minimum of 16 slots up to 4096 slots at one gigabyte or
 * more.
 */
void __init pidhash_init(void)
{
	int i, pidhash_size;
	unsigned long megabytes = nr_kernel_pages >> (20 - PAGE_SHIFT);

	pidhash_shift = max(4, fls(megabytes * 4));
	pidhash_shift = min(12, pidhash_shift);
	pidhash_size = 1 << pidhash_shift;

	printk("PID hash table entries: %d (order: %d, %Zd bytes)\n",
		pidhash_size, pidhash_shift,
		pidhash_size * sizeof(struct hlist_head));

	pid_hash = alloc_bootmem(pidhash_size *	sizeof(*(pid_hash)));
	if (!pid_hash)
		panic("Could not alloc pidhash!\n");
	for (i = 0; i < pidhash_size; i++)
		INIT_HLIST_HEAD(&pid_hash[i]);
}

void __init pidmap_init(void)
{
	init_pid_ns.pidmap[0].page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	/* Reserve PID 0. We never call free_pidmap(0) */
	set_bit(0, init_pid_ns.pidmap[0].page);
	atomic_dec(&init_pid_ns.pidmap[0].nr_free);

	init_pid_ns.pid_cachep = create_pid_cachep(1);
	if (init_pid_ns.pid_cachep == NULL)
		panic("Can't create pid_1 cachep\n");

	pid_ns_cachep = KMEM_CACHE(pid_namespace, SLAB_PANIC);
}
