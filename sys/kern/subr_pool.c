/*	$OpenBSD: subr_pool.c,v 1.22 2002/01/25 15:50:22 art Exp $	*/
/*	$NetBSD: subr_pool.c,v 1.61 2001/09/26 07:14:56 chs Exp $	*/

/*-
 * Copyright (c) 1997, 1999, 2000 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Paul Kranenburg; by Jason R. Thorpe of the Numerical Aerospace
 * Simulation Facility, NASA Ames Research Center.
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
 *	This product includes software developed by the NetBSD
 *	Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/pool.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>

#include <uvm/uvm.h>

/*
 * XXX - for now.
 */
#define SIMPLELOCK_INITIALIZER { SLOCK_UNLOCKED }
#ifdef LOCKDEBUG
#define simple_lock_freecheck(a, s) do { /* nothing */ } while (0)
#define simple_lock_only_held(lkp, str) do { /* nothing */ } while (0)
#endif

/*
 * Pool resource management utility.
 *
 * Memory is allocated in pages which are split into pieces according
 * to the pool item size. Each page is kept on a list headed by `pr_pagelist'
 * in the pool structure and the individual pool items are on a linked list
 * headed by `ph_itemlist' in each page header. The memory for building
 * the page list is either taken from the allocated pages themselves (for
 * small pool items) or taken from an internal pool of page headers (`phpool').
 */

/* List of all pools */
TAILQ_HEAD(,pool) pool_head = TAILQ_HEAD_INITIALIZER(pool_head);

/* Private pool for page header structures */
static struct pool phpool;

/* # of seconds to retain page after last use */
int pool_inactive_time = 10;

/* Next candidate for drainage (see pool_drain()) */
static struct pool	*drainpp;

/* This spin lock protects both pool_head and drainpp. */
struct simplelock pool_head_slock = SIMPLELOCK_INITIALIZER;

struct pool_item_header {
	/* Page headers */
	TAILQ_ENTRY(pool_item_header)
				ph_pagelist;	/* pool page list */
	TAILQ_HEAD(,pool_item)	ph_itemlist;	/* chunk list for this page */
	LIST_ENTRY(pool_item_header)
				ph_hashlist;	/* Off-page page headers */
	int			ph_nmissing;	/* # of chunks in use */
	caddr_t			ph_page;	/* this page's address */
	struct timeval		ph_time;	/* last referenced */
};
TAILQ_HEAD(pool_pagelist,pool_item_header);

struct pool_item {
#ifdef DIAGNOSTIC
	int pi_magic;
#endif
#define	PI_MAGIC 0xdeadbeef
	/* Other entries use only this list entry */
	TAILQ_ENTRY(pool_item)	pi_list;
};

#define	PR_HASH_INDEX(pp,addr) \
	(((u_long)(addr) >> (pp)->pr_alloc->pa_pageshift) & (PR_HASHTABSIZE - 1))

#define	POOL_NEEDS_CATCHUP(pp)						\
	((pp)->pr_nitems < (pp)->pr_minitems)

/*
 * Every pool get a unique serial number assigned to it. If this counter
 * wraps, we're screwed, but we shouldn't create so many pools anyway.
 */
unsigned int pool_serial;

/*
 * Pool cache management.
 *
 * Pool caches provide a way for constructed objects to be cached by the
 * pool subsystem.  This can lead to performance improvements by avoiding
 * needless object construction/destruction; it is deferred until absolutely
 * necessary.
 *
 * Caches are grouped into cache groups.  Each cache group references
 * up to 16 constructed objects.  When a cache allocates an object
 * from the pool, it calls the object's constructor and places it into
 * a cache group.  When a cache group frees an object back to the pool,
 * it first calls the object's destructor.  This allows the object to
 * persist in constructed form while freed to the cache.
 *
 * Multiple caches may exist for each pool.  This allows a single
 * object type to have multiple constructed forms.  The pool references
 * each cache, so that when a pool is drained by the pagedaemon, it can
 * drain each individual cache as well.  Each time a cache is drained,
 * the most idle cache group is freed to the pool in its entirety.
 *
 * Pool caches are layed on top of pools.  By layering them, we can avoid
 * the complexity of cache management for pools which would not benefit
 * from it.
 */

/* The cache group pool. */
static struct pool pcgpool;

/* The pool cache group. */
#define	PCG_NOBJECTS		16
struct pool_cache_group {
	TAILQ_ENTRY(pool_cache_group)
		pcg_list;	/* link in the pool cache's group list */
	u_int	pcg_avail;	/* # available objects */
				/* pointers to the objects */
	void	*pcg_objects[PCG_NOBJECTS];
};

static void	pool_cache_reclaim(struct pool_cache *);

static int	pool_catchup(struct pool *);
static void	pool_prime_page(struct pool *, caddr_t,
		    struct pool_item_header *);

void *pool_allocator_alloc(struct pool *, int);
void pool_allocator_free(struct pool *, void *);

static void pool_print1(struct pool *, const char *,
	int (*)(const char *, ...));

/*
 * Pool log entry. An array of these is allocated in pool_init().
 */
struct pool_log {
	const char	*pl_file;
	long		pl_line;
	int		pl_action;
#define	PRLOG_GET	1
#define	PRLOG_PUT	2
	void		*pl_addr;
};

/* Number of entries in pool log buffers */
#ifndef POOL_LOGSIZE
#define	POOL_LOGSIZE	10
#endif

int pool_logsize = POOL_LOGSIZE;

#ifdef POOL_DIAGNOSTIC
static __inline void
pr_log(struct pool *pp, void *v, int action, const char *file, long line)
{
	int n = pp->pr_curlogentry;
	struct pool_log *pl;

	if ((pp->pr_roflags & PR_LOGGING) == 0)
		return;

	/*
	 * Fill in the current entry. Wrap around and overwrite
	 * the oldest entry if necessary.
	 */
	pl = &pp->pr_log[n];
	pl->pl_file = file;
	pl->pl_line = line;
	pl->pl_action = action;
	pl->pl_addr = v;
	if (++n >= pp->pr_logsize)
		n = 0;
	pp->pr_curlogentry = n;
}

static void
pr_printlog(struct pool *pp, struct pool_item *pi,
    int (*pr)(const char *, ...))
{
	int i = pp->pr_logsize;
	int n = pp->pr_curlogentry;

	if ((pp->pr_roflags & PR_LOGGING) == 0)
		return;

	/*
	 * Print all entries in this pool's log.
	 */
	while (i-- > 0) {
		struct pool_log *pl = &pp->pr_log[n];
		if (pl->pl_action != 0) {
			if (pi == NULL || pi == pl->pl_addr) {
				(*pr)("\tlog entry %d:\n", i);
				(*pr)("\t\taction = %s, addr = %p\n",
				    pl->pl_action == PRLOG_GET ? "get" : "put",
				    pl->pl_addr);
				(*pr)("\t\tfile: %s at line %lu\n",
				    pl->pl_file, pl->pl_line);
			}
		}
		if (++n >= pp->pr_logsize)
			n = 0;
	}
}

static __inline void
pr_enter(struct pool *pp, const char *file, long line)
{

	if (__predict_false(pp->pr_entered_file != NULL)) {
		printf("pool %s: reentrancy at file %s line %ld\n",
		    pp->pr_wchan, file, line);
		printf("         previous entry at file %s line %ld\n",
		    pp->pr_entered_file, pp->pr_entered_line);
		panic("pr_enter");
	}

	pp->pr_entered_file = file;
	pp->pr_entered_line = line;
}

static __inline void
pr_leave(struct pool *pp)
{

	if (__predict_false(pp->pr_entered_file == NULL)) {
		printf("pool %s not entered?\n", pp->pr_wchan);
		panic("pr_leave");
	}

	pp->pr_entered_file = NULL;
	pp->pr_entered_line = 0;
}

static __inline void
pr_enter_check(struct pool *pp, int (*pr)(const char *, ...))
{

	if (pp->pr_entered_file != NULL)
		(*pr)("\n\tcurrently entered from file %s line %ld\n",
		    pp->pr_entered_file, pp->pr_entered_line);
}
#else
#define	pr_log(pp, v, action, file, line)
#define	pr_printlog(pp, pi, pr)
#define	pr_enter(pp, file, line)
#define	pr_leave(pp)
#define	pr_enter_check(pp, pr)
#endif /* POOL_DIAGNOSTIC */

/*
 * Return the pool page header based on page address.
 */
static __inline struct pool_item_header *
pr_find_pagehead(struct pool *pp, caddr_t page)
{
	struct pool_item_header *ph;

	if ((pp->pr_roflags & PR_PHINPAGE) != 0)
		return ((struct pool_item_header *)(page + pp->pr_phoffset));

	for (ph = LIST_FIRST(&pp->pr_hashtab[PR_HASH_INDEX(pp, page)]);
	     ph != NULL;
	     ph = LIST_NEXT(ph, ph_hashlist)) {
		if (ph->ph_page == page)
			return (ph);
	}
	return (NULL);
}

/*
 * Remove a page from the pool.
 */
static __inline void
pr_rmpage(struct pool *pp, struct pool_item_header *ph,
     struct pool_pagelist *pq)
{
	int s;

	/*
	 * If the page was idle, decrement the idle page count.
	 */
	if (ph->ph_nmissing == 0) {
#ifdef DIAGNOSTIC
		if (pp->pr_nidle == 0)
			panic("pr_rmpage: nidle inconsistent");
		if (pp->pr_nitems < pp->pr_itemsperpage)
			panic("pr_rmpage: nitems inconsistent");
#endif
		pp->pr_nidle--;
	}

	pp->pr_nitems -= pp->pr_itemsperpage;

	/*
	 * Unlink a page from the pool and release it (or queue it for release).
	 */
	TAILQ_REMOVE(&pp->pr_pagelist, ph, ph_pagelist);
	if (pq) {
		TAILQ_INSERT_HEAD(pq, ph, ph_pagelist);
	} else {
		pool_allocator_free(pp, ph->ph_page);
		if ((pp->pr_roflags & PR_PHINPAGE) == 0) {
			LIST_REMOVE(ph, ph_hashlist);
			s = splhigh();
			pool_put(&phpool, ph);
			splx(s);
		}
	}
	pp->pr_npages--;
	pp->pr_npagefree++;

	if (pp->pr_curpage == ph) {
		/*
		 * Find a new non-empty page header, if any.
		 * Start search from the page head, to increase the
		 * chance for "high water" pages to be freed.
		 */
		TAILQ_FOREACH(ph, &pp->pr_pagelist, ph_pagelist)
			if (TAILQ_FIRST(&ph->ph_itemlist) != NULL)
				break;

		pp->pr_curpage = ph;
	}
}

/*
 * Initialize the given pool resource structure.
 *
 * We export this routine to allow other kernel parts to declare
 * static pools that must be initialized before malloc() is available.
 */
void
pool_init(struct pool *pp, size_t size, u_int align, u_int ioff, int flags,
    const char *wchan, struct pool_allocator *palloc)
{
	int off, slack, i;

#ifdef POOL_DIAGNOSTIC
	/*
	 * Always log if POOL_DIAGNOSTIC is defined.
	 */
	if (pool_logsize != 0)
		flags |= PR_LOGGING;
#endif

	/*
	 * Check arguments and construct default values.
	 */
	if (palloc == NULL)
		palloc = &pool_allocator_kmem;
	if ((palloc->pa_flags & PA_INITIALIZED) == 0) {
		if (palloc->pa_pagesz == 0)
			palloc->pa_pagesz = PAGE_SIZE;

		TAILQ_INIT(&palloc->pa_list);
		
		simple_lock_init(&palloc->pa_slock);
		palloc->pa_pagemask = ~(palloc->pa_pagesz - 1);
		palloc->pa_pageshift = ffs(palloc->pa_pagesz) - 1;
		palloc->pa_flags |= PA_INITIALIZED;
	}

	if (align == 0)
		align = ALIGN(1);

	if (size < sizeof(struct pool_item))
		size = sizeof(struct pool_item);

	size = ALIGN(size);
#ifdef DIAGNOSTIC
	if (size > palloc->pa_pagesz)
		panic("pool_init: pool item size (%lu) too large",
		      (u_long)size);
#endif

	/*
	 * Initialize the pool structure.
	 */
	TAILQ_INIT(&pp->pr_pagelist);
	TAILQ_INIT(&pp->pr_cachelist);
	pp->pr_curpage = NULL;
	pp->pr_npages = 0;
	pp->pr_minitems = 0;
	pp->pr_minpages = 0;
	pp->pr_maxpages = UINT_MAX;
	pp->pr_roflags = flags;
	pp->pr_flags = 0;
	pp->pr_size = size;
	pp->pr_align = align;
	pp->pr_wchan = wchan;
	pp->pr_alloc = palloc;
	pp->pr_nitems = 0;
	pp->pr_nout = 0;
	pp->pr_hardlimit = UINT_MAX;
	pp->pr_hardlimit_warning = NULL;
	pp->pr_hardlimit_ratecap.tv_sec = 0;
	pp->pr_hardlimit_ratecap.tv_usec = 0;
	pp->pr_hardlimit_warning_last.tv_sec = 0;
	pp->pr_hardlimit_warning_last.tv_usec = 0;
	pp->pr_drain_hook = NULL;
	pp->pr_drain_hook_arg = NULL;
	pp->pr_serial = ++pool_serial;
	if (pool_serial == 0)
		panic("pool_init: too much uptime");

	/*
	 * Decide whether to put the page header off page to avoid
	 * wasting too large a part of the page. Off-page page headers
	 * go on a hash table, so we can match a returned item
	 * with its header based on the page address.
	 * We use 1/16 of the page size as the threshold (XXX: tune)
	 */
	if (pp->pr_size < palloc->pa_pagesz/16) {
		/* Use the end of the page for the page header */
		pp->pr_roflags |= PR_PHINPAGE;
		pp->pr_phoffset = off =
			palloc->pa_pagesz - ALIGN(sizeof(struct pool_item_header));
	} else {
		/* The page header will be taken from our page header pool */
		pp->pr_phoffset = 0;
		off = palloc->pa_pagesz;
		for (i = 0; i < PR_HASHTABSIZE; i++) {
			LIST_INIT(&pp->pr_hashtab[i]);
		}
	}

	/*
	 * Alignment is to take place at `ioff' within the item. This means
	 * we must reserve up to `align - 1' bytes on the page to allow
	 * appropriate positioning of each item.
	 *
	 * Silently enforce `0 <= ioff < align'.
	 */
	pp->pr_itemoffset = ioff = ioff % align;
	pp->pr_itemsperpage = (off - ((align - ioff) % align)) / pp->pr_size;
	KASSERT(pp->pr_itemsperpage != 0);

	/*
	 * Use the slack between the chunks and the page header
	 * for "cache coloring".
	 */
	slack = off - pp->pr_itemsperpage * pp->pr_size;
	pp->pr_maxcolor = (slack / align) * align;
	pp->pr_curcolor = 0;

	pp->pr_nget = 0;
	pp->pr_nfail = 0;
	pp->pr_nput = 0;
	pp->pr_npagealloc = 0;
	pp->pr_npagefree = 0;
	pp->pr_hiwat = 0;
	pp->pr_nidle = 0;

#ifdef POOL_DIAGNOSTIC
	if (flags & PR_LOGGING) {
		if (kmem_map == NULL ||
		    (pp->pr_log = malloc(pool_logsize * sizeof(struct pool_log),
		     M_TEMP, M_NOWAIT)) == NULL)
			pp->pr_roflags &= ~PR_LOGGING;
		pp->pr_curlogentry = 0;
		pp->pr_logsize = pool_logsize;
	}
#endif

	pp->pr_entered_file = NULL;
	pp->pr_entered_line = 0;

	simple_lock_init(&pp->pr_slock);

	/*
	 * Initialize private page header pool and cache magazine pool if we
	 * haven't done so yet.
	 * XXX LOCKING.
	 */
	if (phpool.pr_size == 0) {
		pool_init(&phpool, sizeof(struct pool_item_header), 0, 0,
		    0, "phpool", NULL);
		pool_init(&pcgpool, sizeof(struct pool_cache_group), 0, 0,
		    0, "pcgpool", NULL);
	}

	/* Insert into the list of all pools. */
	simple_lock(&pool_head_slock);
	TAILQ_INSERT_TAIL(&pool_head, pp, pr_poollist);
	simple_unlock(&pool_head_slock);

	/* Insert into the list of pools using this allocator. */
	simple_lock(&palloc->pa_slock);
	TAILQ_INSERT_TAIL(&palloc->pa_list, pp, pr_alloc_list);
	simple_unlock(&palloc->pa_slock);
}

/*
 * De-commision a pool resource.
 */
void
pool_destroy(struct pool *pp)
{
	struct pool_item_header *ph;
	struct pool_cache *pc;

	/*
	 * Locking order: pool_allocator -> pool
	 */
	simple_lock(&pp->pr_alloc->pa_slock);
	TAILQ_REMOVE(&pp->pr_alloc->pa_list, pp, pr_alloc_list);
	simple_unlock(&pp->pr_alloc->pa_slock);

	/* Destroy all caches for this pool. */
	while ((pc = TAILQ_FIRST(&pp->pr_cachelist)) != NULL)
		pool_cache_destroy(pc);

#ifdef DIAGNOSTIC
	if (pp->pr_nout != 0) {
		pr_printlog(pp, NULL, printf);
		panic("pool_destroy: pool busy: still out: %u\n",
		    pp->pr_nout);
	}
#endif

	/* Remove all pages */
	if ((pp->pr_roflags & PR_STATIC) == 0)
		while ((ph = TAILQ_FIRST(&pp->pr_pagelist)) != NULL)
			pr_rmpage(pp, ph, NULL);

	/* Remove from global pool list */
	simple_lock(&pool_head_slock);
	TAILQ_REMOVE(&pool_head, pp, pr_poollist);
	if (drainpp == pp) {
		drainpp = NULL;
	}
	simple_unlock(&pool_head_slock);

#ifdef POOL_DIAGNOSTIC
	if ((pp->pr_roflags & PR_LOGGING) != 0)
		free(pp->pr_log, M_TEMP);
#endif
}

void
pool_set_drain_hook(struct pool *pp, void (*fn)(void *, int), void *fnarg)
{
	/*
	 * XXX - no locking, must be called just after pool_init.
	 */
#ifdef DIAGNOSTIC
	if (pp->pr_drain_hook != NULL)
		panic("pool_set_drain_hook(%s): already set", pp->pr_wchan);
#endif
	pp->pr_drain_hook = fn;
	pp->pr_drain_hook_arg = fnarg;
}

static __inline struct pool_item_header *
pool_alloc_item_header(struct pool *pp, caddr_t storage, int flags)
{
	struct pool_item_header *ph;
	int s;

	LOCK_ASSERT(simple_lock_held(&pp->pr_slock) == 0);

	if ((pp->pr_roflags & PR_PHINPAGE) != 0)
		ph = (struct pool_item_header *) (storage + pp->pr_phoffset);
	else {
		s = splhigh();
		ph = pool_get(&phpool, flags);
		splx(s);
	}

	return (ph);
}

/*
 * Grab an item from the pool; must be called at appropriate spl level
 */
void *
#ifdef POOL_DIAGNOSTIC
_pool_get(struct pool *pp, int flags, const char *file, long line)
#else
pool_get(struct pool *pp, int flags)
#endif
{
	struct pool_item *pi;
	struct pool_item_header *ph;
	void *v;

#ifdef DIAGNOSTIC
	if (__predict_false((pp->pr_roflags & PR_STATIC) &&
			    (flags & PR_MALLOCOK))) {
		pr_printlog(pp, NULL, printf);
		panic("pool_get: static");
	}

	if (__predict_false(curproc == NULL && /* doing_shutdown == 0 && XXX*/
			    (flags & PR_WAITOK) != 0))
		panic("pool_get: must have NOWAIT");

#ifdef LOCKDEBUG
	if (flags & PR_WAITOK)
		simple_lock_only_held(NULL, "pool_get(PR_WAITOK)");
#endif
#endif /* DIAGNOSTIC */

	simple_lock(&pp->pr_slock);
	pr_enter(pp, file, line);

 startover:
	/*
	 * Check to see if we've reached the hard limit.  If we have,
	 * and we can wait, then wait until an item has been returned to
	 * the pool.
	 */
#ifdef DIAGNOSTIC
	if (__predict_false(pp->pr_nout > pp->pr_hardlimit)) {
		pr_leave(pp);
		simple_unlock(&pp->pr_slock);
		panic("pool_get: %s: crossed hard limit", pp->pr_wchan);
	}
#endif
	if (__predict_false(pp->pr_nout == pp->pr_hardlimit)) {
		if (pp->pr_drain_hook != NULL) {
			/*
			 * Since the drain hook is likely to free memory
			 * to this pool unlock, call hook, relock and check
			 * hardlimit condition again.
			 */
			simple_unlock(&pp->pr_slock);
			(*pp->pr_drain_hook)(pp->pr_drain_hook_arg, flags);
			simple_lock(&pp->pr_slock);
			if (pp->pr_nout < pp->pr_hardlimit)
				goto startover;
		}

		if ((flags & PR_WAITOK) && !(flags & PR_LIMITFAIL)) {
			/*
			 * XXX: A warning isn't logged in this case.  Should
			 * it be?
			 */
			pp->pr_flags |= PR_WANTED;
			pr_leave(pp);
			ltsleep(pp, PSWP, pp->pr_wchan, 0, &pp->pr_slock);
			pr_enter(pp, file, line);
			goto startover;
		}

		/*
		 * Log a message that the hard limit has been hit.
		 */
		if (pp->pr_hardlimit_warning != NULL &&
		    ratecheck(&pp->pr_hardlimit_warning_last,
			      &pp->pr_hardlimit_ratecap))
			log(LOG_ERR, "%s\n", pp->pr_hardlimit_warning);

		pp->pr_nfail++;

		pr_leave(pp);
		simple_unlock(&pp->pr_slock);
		return (NULL);
	}

	/*
	 * The convention we use is that if `curpage' is not NULL, then
	 * it points at a non-empty bucket. In particular, `curpage'
	 * never points at a page header which has PR_PHINPAGE set and
	 * has no items in its bucket.
	 */
	if ((ph = pp->pr_curpage) == NULL) {
#ifdef DIAGNOSTIC
		if (pp->pr_nitems != 0) {
			simple_unlock(&pp->pr_slock);
			printf("pool_get: %s: curpage NULL, nitems %u\n",
			    pp->pr_wchan, pp->pr_nitems);
			panic("pool_get: nitems inconsistent\n");
		}
#endif

		/*
		 * Call the back-end page allocator for more memory.
		 * Release the pool lock, as the back-end page allocator
		 * may block.
		 */
		pr_leave(pp);
		simple_unlock(&pp->pr_slock);
		v = pool_allocator_alloc(pp, flags);
		if (__predict_true(v != NULL))
			ph = pool_alloc_item_header(pp, v, flags);
		simple_lock(&pp->pr_slock);
		pr_enter(pp, file, line);

		if (__predict_false(v == NULL || ph == NULL)) {
			if (v != NULL)
				pool_allocator_free(pp, v);

			/*
			 * We were unable to allocate a page or item
			 * header, but we released the lock during
			 * allocation, so perhaps items were freed
			 * back to the pool.  Check for this case.
			 */
			if (pp->pr_curpage != NULL)
				goto startover;

			if ((flags & PR_WAITOK) == 0) {
				pp->pr_nfail++;
				pr_leave(pp);
				simple_unlock(&pp->pr_slock);
				return (NULL);
			}

			/*
			 * Wait for items to be returned to this pool.
			 *
			 * XXX: maybe we should wake up once a second and
			 * try again?
			 */
			pp->pr_flags |= PR_WANTED;
			/* PA_WANTED is already set on the allocator */
			pr_leave(pp);
			ltsleep(pp, PSWP, pp->pr_wchan, 0, &pp->pr_slock);
			pr_enter(pp, file, line);
			goto startover;
		}

		/* We have more memory; add it to the pool */
		pool_prime_page(pp, v, ph);
		pp->pr_npagealloc++;

		/* Start the allocation process over. */
		goto startover;
	}

	if (__predict_false((v = pi = TAILQ_FIRST(&ph->ph_itemlist)) == NULL)) {
		pr_leave(pp);
		simple_unlock(&pp->pr_slock);
		panic("pool_get: %s: page empty", pp->pr_wchan);
	}
#ifdef DIAGNOSTIC
	if (__predict_false(pp->pr_nitems == 0)) {
		pr_leave(pp);
		simple_unlock(&pp->pr_slock);
		printf("pool_get: %s: items on itemlist, nitems %u\n",
		    pp->pr_wchan, pp->pr_nitems);
		panic("pool_get: nitems inconsistent\n");
	}
#endif

#ifdef POOL_DIAGNOSTIC
	pr_log(pp, v, PRLOG_GET, file, line);
#endif

#ifdef DIAGNOSTIC
	if (__predict_false(pi->pi_magic != PI_MAGIC)) {
		pr_printlog(pp, pi, printf);
		panic("pool_get(%s): free list modified: magic=%x; page %p;"
		       " item addr %p\n",
			pp->pr_wchan, pi->pi_magic, ph->ph_page, pi);
	}
#endif

	/*
	 * Remove from item list.
	 */
	TAILQ_REMOVE(&ph->ph_itemlist, pi, pi_list);
	pp->pr_nitems--;
	pp->pr_nout++;
	if (ph->ph_nmissing == 0) {
#ifdef DIAGNOSTIC
		if (__predict_false(pp->pr_nidle == 0))
			panic("pool_get: nidle inconsistent");
#endif
		pp->pr_nidle--;
	}
	ph->ph_nmissing++;
	if (TAILQ_FIRST(&ph->ph_itemlist) == NULL) {
#ifdef DIAGNOSTIC
		if (__predict_false(ph->ph_nmissing != pp->pr_itemsperpage)) {
			pr_leave(pp);
			simple_unlock(&pp->pr_slock);
			panic("pool_get: %s: nmissing inconsistent",
			    pp->pr_wchan);
		}
#endif
		/*
		 * Find a new non-empty page header, if any.
		 * Start search from the page head, to increase
		 * the chance for "high water" pages to be freed.
		 *
		 * Migrate empty pages to the end of the list.  This
		 * will speed the update of curpage as pages become
		 * idle.  Empty pages intermingled with idle pages
		 * is no big deal.  As soon as a page becomes un-empty,
		 * it will move back to the head of the list.
		 */
		TAILQ_REMOVE(&pp->pr_pagelist, ph, ph_pagelist);
		TAILQ_INSERT_TAIL(&pp->pr_pagelist, ph, ph_pagelist);
		TAILQ_FOREACH(ph, &pp->pr_pagelist, ph_pagelist)
			if (TAILQ_FIRST(&ph->ph_itemlist) != NULL)
				break;

		pp->pr_curpage = ph;
	}

	pp->pr_nget++;

	/*
	 * If we have a low water mark and we are now below that low
	 * water mark, add more items to the pool.
	 */
	if (POOL_NEEDS_CATCHUP(pp) && pool_catchup(pp) != 0) {
		/*
		 * XXX: Should we log a warning?  Should we set up a timeout
		 * to try again in a second or so?  The latter could break
		 * a caller's assumptions about interrupt protection, etc.
		 */
	}

	pr_leave(pp);
	simple_unlock(&pp->pr_slock);
	return (v);
}

/*
 * Internal version of pool_put().  Pool is already locked/entered.
 */
static void
pool_do_put(struct pool *pp, void *v)
{
	struct pool_item *pi = v;
	struct pool_item_header *ph;
	caddr_t page;
	int s;

	LOCK_ASSERT(simple_lock_held(&pp->pr_slock));

	page = (caddr_t)((vaddr_t)v & pp->pr_alloc->pa_pagemask);

#ifdef DIAGNOSTIC
	if (__predict_false(pp->pr_nout == 0)) {
		printf("pool %s: putting with none out\n",
		    pp->pr_wchan);
		panic("pool_put");
	}
#endif

	if (__predict_false((ph = pr_find_pagehead(pp, page)) == NULL)) {
		pr_printlog(pp, NULL, printf);
		panic("pool_put: %s: page header missing", pp->pr_wchan);
	}

#ifdef LOCKDEBUG
	/*
	 * Check if we're freeing a locked simple lock.
	 */
	simple_lock_freecheck((caddr_t)pi, ((caddr_t)pi) + pp->pr_size);
#endif

	/*
	 * Return to item list.
	 */
#ifdef DIAGNOSTIC
	pi->pi_magic = PI_MAGIC;
#endif
#ifdef DEBUG
	{
		int i, *ip = v;

		for (i = 0; i < pp->pr_size / sizeof(int); i++) {
			*ip++ = PI_MAGIC;
		}
	}
#endif

	TAILQ_INSERT_HEAD(&ph->ph_itemlist, pi, pi_list);
	ph->ph_nmissing--;
	pp->pr_nput++;
	pp->pr_nitems++;
	pp->pr_nout--;

	/* Cancel "pool empty" condition if it exists */
	if (pp->pr_curpage == NULL)
		pp->pr_curpage = ph;

	if (pp->pr_flags & PR_WANTED) {
		pp->pr_flags &= ~PR_WANTED;
		if (ph->ph_nmissing == 0)
			pp->pr_nidle++;
		wakeup((caddr_t)pp);
		return;
	}

	/*
	 * If this page is now complete, do one of two things:
	 *
	 *	(1) If we have more pages than the page high water
	 *	    mark, free the page back to the system.
	 *
	 *	(2) Move it to the end of the page list, so that
	 *	    we minimize our chances of fragmenting the
	 *	    pool.  Idle pages migrate to the end (along with
	 *	    completely empty pages, so that we find un-empty
	 *	    pages more quickly when we update curpage) of the
	 *	    list so they can be more easily swept up by
	 *	    the pagedaemon when pages are scarce.
	 */
	if (ph->ph_nmissing == 0) {
		pp->pr_nidle++;
		if (pp->pr_npages > pp->pr_maxpages) {
			pr_rmpage(pp, ph, NULL);
		} else {
			TAILQ_REMOVE(&pp->pr_pagelist, ph, ph_pagelist);
			TAILQ_INSERT_TAIL(&pp->pr_pagelist, ph, ph_pagelist);

			/*
			 * Update the timestamp on the page.  A page must
			 * be idle for some period of time before it can
			 * be reclaimed by the pagedaemon.  This minimizes
			 * ping-pong'ing for memory.
			 */
			s = splclock();
			ph->ph_time = mono_time;
			splx(s);

			/*
			 * Update the current page pointer.  Just look for
			 * the first page with any free items.
			 *
			 * XXX: Maybe we want an option to look for the
			 * page with the fewest available items, to minimize
			 * fragmentation?
			 */
			TAILQ_FOREACH(ph, &pp->pr_pagelist, ph_pagelist)
				if (TAILQ_FIRST(&ph->ph_itemlist) != NULL)
					break;

			pp->pr_curpage = ph;
		}
	}
	/*
	 * If the page has just become un-empty, move it to the head of
	 * the list, and make it the current page.  The next allocation
	 * will get the item from this page, instead of further fragmenting
	 * the pool.
	 */
	else if (ph->ph_nmissing == (pp->pr_itemsperpage - 1)) {
		TAILQ_REMOVE(&pp->pr_pagelist, ph, ph_pagelist);
		TAILQ_INSERT_HEAD(&pp->pr_pagelist, ph, ph_pagelist);
		pp->pr_curpage = ph;
	}
}

/*
 * Return resource to the pool; must be called at appropriate spl level
 */
#ifdef POOL_DIAGNOSTIC
void
_pool_put(struct pool *pp, void *v, const char *file, long line)
{

	simple_lock(&pp->pr_slock);
	pr_enter(pp, file, line);

	pr_log(pp, v, PRLOG_PUT, file, line);

	pool_do_put(pp, v);

	pr_leave(pp);
	simple_unlock(&pp->pr_slock);
}
#undef pool_put
#endif /* POOL_DIAGNOSTIC */

void
pool_put(struct pool *pp, void *v)
{

	simple_lock(&pp->pr_slock);

	pool_do_put(pp, v);

	simple_unlock(&pp->pr_slock);
}

#ifdef POOL_DIAGNOSTIC
#define		pool_put(h, v)	_pool_put((h), (v), __FILE__, __LINE__)
#endif

/*
 * Add N items to the pool.
 */
int
pool_prime(struct pool *pp, int n)
{
	struct pool_item_header *ph;
	caddr_t cp;
	int newpages, error = 0;

	simple_lock(&pp->pr_slock);

	newpages = roundup(n, pp->pr_itemsperpage) / pp->pr_itemsperpage;

	while (newpages-- > 0) {
		simple_unlock(&pp->pr_slock);
		cp = pool_allocator_alloc(pp, PR_NOWAIT);
		if (__predict_true(cp != NULL))
			ph = pool_alloc_item_header(pp, cp, PR_NOWAIT);
		simple_lock(&pp->pr_slock);

		if (__predict_false(cp == NULL || ph == NULL)) {
			error = ENOMEM;
			if (cp != NULL)
				pool_allocator_free(pp, cp);
			break;
		}

		pool_prime_page(pp, cp, ph);
		pp->pr_npagealloc++;
		pp->pr_minpages++;
	}

	if (pp->pr_minpages >= pp->pr_maxpages)
		pp->pr_maxpages = pp->pr_minpages + 1;	/* XXX */

	simple_unlock(&pp->pr_slock);
	return (0);
}

/*
 * Add a page worth of items to the pool.
 *
 * Note, we must be called with the pool descriptor LOCKED.
 */
static void
pool_prime_page(struct pool *pp, caddr_t storage, struct pool_item_header *ph)
{
	struct pool_item *pi;
	caddr_t cp = storage;
	unsigned int align = pp->pr_align;
	unsigned int ioff = pp->pr_itemoffset;
	int n;

#ifdef DIAGNOSTIC
	if (((u_long)cp & (pp->pr_alloc->pa_pagesz - 1)) != 0)
		panic("pool_prime_page: %s: unaligned page", pp->pr_wchan);
#endif

	if ((pp->pr_roflags & PR_PHINPAGE) == 0)
		LIST_INSERT_HEAD(&pp->pr_hashtab[PR_HASH_INDEX(pp, cp)],
		    ph, ph_hashlist);

	/*
	 * Insert page header.
	 */
	TAILQ_INSERT_HEAD(&pp->pr_pagelist, ph, ph_pagelist);
	TAILQ_INIT(&ph->ph_itemlist);
	ph->ph_page = storage;
	ph->ph_nmissing = 0;
	memset(&ph->ph_time, 0, sizeof(ph->ph_time));

	pp->pr_nidle++;

	/*
	 * Color this page.
	 */
	cp = (caddr_t)(cp + pp->pr_curcolor);
	if ((pp->pr_curcolor += align) > pp->pr_maxcolor)
		pp->pr_curcolor = 0;

	/*
	 * Adjust storage to apply aligment to `pr_itemoffset' in each item.
	 */
	if (ioff != 0)
		cp = (caddr_t)(cp + (align - ioff));

	/*
	 * Insert remaining chunks on the bucket list.
	 */
	n = pp->pr_itemsperpage;
	pp->pr_nitems += n;

	while (n--) {
		pi = (struct pool_item *)cp;

		/* Insert on page list */
		TAILQ_INSERT_TAIL(&ph->ph_itemlist, pi, pi_list);
#ifdef DIAGNOSTIC
		pi->pi_magic = PI_MAGIC;
#endif
		cp = (caddr_t)(cp + pp->pr_size);
	}

	/*
	 * If the pool was depleted, point at the new page.
	 */
	if (pp->pr_curpage == NULL)
		pp->pr_curpage = ph;

	if (++pp->pr_npages > pp->pr_hiwat)
		pp->pr_hiwat = pp->pr_npages;
}

/*
 * Used by pool_get() when nitems drops below the low water mark.  This
 * is used to catch up nitmes with the low water mark.
 *
 * Note 1, we never wait for memory here, we let the caller decide what to do.
 *
 * Note 2, this doesn't work with static pools.
 *
 * Note 3, we must be called with the pool already locked, and we return
 * with it locked.
 */
static int
pool_catchup(struct pool *pp)
{
	struct pool_item_header *ph;
	caddr_t cp;
	int error = 0;

	if (pp->pr_roflags & PR_STATIC) {
		/*
		 * We dropped below the low water mark, and this is not a
		 * good thing.  Log a warning.
		 *
		 * XXX: rate-limit this?
		 */
		printf("WARNING: static pool `%s' dropped below low water "
		    "mark\n", pp->pr_wchan);
		return (0);
	}

	while (POOL_NEEDS_CATCHUP(pp)) {
		/*
		 * Call the page back-end allocator for more memory.
		 *
		 * XXX: We never wait, so should we bother unlocking
		 * the pool descriptor?
		 */
		simple_unlock(&pp->pr_slock);
		cp = pool_allocator_alloc(pp, PR_NOWAIT);
		if (__predict_true(cp != NULL))
			ph = pool_alloc_item_header(pp, cp, PR_NOWAIT);
		simple_lock(&pp->pr_slock);
		if (__predict_false(cp == NULL || ph == NULL)) {
			if (cp != NULL)
				pool_allocator_free(pp, cp);
			error = ENOMEM;
			break;
		}
		pool_prime_page(pp, cp, ph);
		pp->pr_npagealloc++;
	}

	return (error);
}

void
pool_setlowat(struct pool *pp, int n)
{
	int error;

	simple_lock(&pp->pr_slock);

	pp->pr_minitems = n;
	pp->pr_minpages = (n == 0)
		? 0
		: roundup(n, pp->pr_itemsperpage) / pp->pr_itemsperpage;

	/* Make sure we're caught up with the newly-set low water mark. */
	if (POOL_NEEDS_CATCHUP(pp) && (error = pool_catchup(pp) != 0)) {
		/*
		 * XXX: Should we log a warning?  Should we set up a timeout
		 * to try again in a second or so?  The latter could break
		 * a caller's assumptions about interrupt protection, etc.
		 */
	}

	simple_unlock(&pp->pr_slock);
}

void
pool_sethiwat(struct pool *pp, int n)
{

	simple_lock(&pp->pr_slock);

	pp->pr_maxpages = (n == 0)
		? 0
		: roundup(n, pp->pr_itemsperpage) / pp->pr_itemsperpage;

	simple_unlock(&pp->pr_slock);
}

void
pool_sethardlimit(struct pool *pp, int n, const char *warnmess, int ratecap)
{

	simple_lock(&pp->pr_slock);

	pp->pr_hardlimit = n;
	pp->pr_hardlimit_warning = warnmess;
	pp->pr_hardlimit_ratecap.tv_sec = ratecap;
	pp->pr_hardlimit_warning_last.tv_sec = 0;
	pp->pr_hardlimit_warning_last.tv_usec = 0;

	/*
	 * In-line version of pool_sethiwat(), because we don't want to
	 * release the lock.
	 */
	pp->pr_maxpages = (n == 0)
		? 0
		: roundup(n, pp->pr_itemsperpage) / pp->pr_itemsperpage;

	simple_unlock(&pp->pr_slock);
}

/*
 * Release all complete pages that have not been used recently.
 *
 * Returns non-zero if any pages have been reclaimed.
 */
int
#ifdef POOL_DIAGNOSTIC
_pool_reclaim(struct pool *pp, const char *file, long line)
#else
pool_reclaim(struct pool *pp)
#endif
{
	struct pool_item_header *ph, *phnext;
	struct pool_cache *pc;
	struct timeval curtime;
	struct pool_pagelist pq;
	int s;

	if (pp->pr_roflags & PR_STATIC)
		return 0;

	if (pp->pr_drain_hook != NULL) {
		/*
		 * The drain hook must be called with the pool unlocked.
		 */
		(*pp->pr_drain_hook)(pp->pr_drain_hook_arg, PR_NOWAIT);
	}

	if (simple_lock_try(&pp->pr_slock) == 0)
		return 0;
	pr_enter(pp, file, line);

	TAILQ_INIT(&pq);

	/*
	 * Reclaim items from the pool's caches.
	 */
	TAILQ_FOREACH(pc, &pp->pr_cachelist, pc_poollist)
		pool_cache_reclaim(pc);

	s = splclock();
	curtime = mono_time;
	splx(s);

	for (ph = TAILQ_FIRST(&pp->pr_pagelist); ph != NULL; ph = phnext) {
		phnext = TAILQ_NEXT(ph, ph_pagelist);

		/* Check our minimum page claim */
		if (pp->pr_npages <= pp->pr_minpages)
			break;

		if (ph->ph_nmissing == 0) {
			struct timeval diff;
			timersub(&curtime, &ph->ph_time, &diff);
			if (diff.tv_sec < pool_inactive_time)
				continue;

			/*
			 * If freeing this page would put us below
			 * the low water mark, stop now.
			 */
			if ((pp->pr_nitems - pp->pr_itemsperpage) <
			    pp->pr_minitems)
				break;

			pr_rmpage(pp, ph, &pq);
		}
	}

	pr_leave(pp);
	simple_unlock(&pp->pr_slock);
	if (TAILQ_EMPTY(&pq)) {
		return 0;
	}
	while ((ph = TAILQ_FIRST(&pq)) != NULL) {
		TAILQ_REMOVE(&pq, ph, ph_pagelist);
		pool_allocator_free(pp, ph->ph_page);
		if (pp->pr_roflags & PR_PHINPAGE) {
			continue;
		}
		LIST_REMOVE(ph, ph_hashlist);
		s = splhigh();
		pool_put(&phpool, ph);
		splx(s);
	}

	return 1;
}


/*
 * Drain pools, one at a time.
 *
 * Note, we must never be called from an interrupt context.
 */
void
pool_drain(void *arg)
{
	struct pool *pp;
	int s;

	pp = NULL;
	s = splvm();
	simple_lock(&pool_head_slock);
	if (drainpp == NULL) {
		drainpp = TAILQ_FIRST(&pool_head);
	}
	if (drainpp) {
		pp = drainpp;
		drainpp = TAILQ_NEXT(pp, pr_poollist);
	}
	simple_unlock(&pool_head_slock);
	pool_reclaim(pp);
	splx(s);
}

/*
 * Diagnostic helpers.
 */
void
pool_printit(struct pool *pp, const char *modif, int (*pr)(const char *, ...))
{
	int s;

	s = splvm();
	if (simple_lock_try(&pp->pr_slock) == 0) {
		printf("pool %s is locked; try again later\n",
		    pp->pr_wchan);
		splx(s);
		return;
	}
	pool_print1(pp, modif, printf);
	simple_unlock(&pp->pr_slock);
	splx(s);
}

static void
pool_print1(struct pool *pp, const char *modif, int (*pr)(const char *, ...))
{
	struct pool_item_header *ph;
	struct pool_cache *pc;
	struct pool_cache_group *pcg;
#ifdef DIAGNOSTIC
	struct pool_item *pi;
#endif
	int i, print_log = 0, print_pagelist = 0, print_cache = 0;
	char c;

	while ((c = *modif++) != '\0') {
		if (c == 'l')
			print_log = 1;
		if (c == 'p')
			print_pagelist = 1;
		if (c == 'c')
			print_cache = 1;
		modif++;
	}

	(*pr)("POOL %s: size %u, align %u, ioff %u, roflags 0x%08x\n",
	    pp->pr_wchan, pp->pr_size, pp->pr_align, pp->pr_itemoffset,
	    pp->pr_roflags);
	(*pr)("\talloc %p\n", pp->pr_alloc);
	(*pr)("\tminitems %u, minpages %u, maxpages %u, npages %u\n",
	    pp->pr_minitems, pp->pr_minpages, pp->pr_maxpages, pp->pr_npages);
	(*pr)("\titemsperpage %u, nitems %u, nout %u, hardlimit %u\n",
	    pp->pr_itemsperpage, pp->pr_nitems, pp->pr_nout, pp->pr_hardlimit);

	(*pr)("\n\tnget %lu, nfail %lu, nput %lu\n",
	    pp->pr_nget, pp->pr_nfail, pp->pr_nput);
	(*pr)("\tnpagealloc %lu, npagefree %lu, hiwat %u, nidle %lu\n",
	    pp->pr_npagealloc, pp->pr_npagefree, pp->pr_hiwat, pp->pr_nidle);

	if (print_pagelist == 0)
		goto skip_pagelist;

	if ((ph = TAILQ_FIRST(&pp->pr_pagelist)) != NULL)
		(*pr)("\n\tpage list:\n");
	for (; ph != NULL; ph = TAILQ_NEXT(ph, ph_pagelist)) {
		(*pr)("\t\tpage %p, nmissing %d, time %lu,%lu\n",
		    ph->ph_page, ph->ph_nmissing,
		    (u_long)ph->ph_time.tv_sec,
		    (u_long)ph->ph_time.tv_usec);
#ifdef DIAGNOSTIC
		TAILQ_FOREACH(pi, &ph->ph_itemlist, pi_list) {
			if (pi->pi_magic != PI_MAGIC) {
				(*pr)("\t\t\titem %p, magic 0x%x\n",
				    pi, pi->pi_magic);
			}
		}
#endif
	}
	if (pp->pr_curpage == NULL)
		(*pr)("\tno current page\n");
	else
		(*pr)("\tcurpage %p\n", pp->pr_curpage->ph_page);

 skip_pagelist:

	if (print_log == 0)
		goto skip_log;

	(*pr)("\n");
	if ((pp->pr_roflags & PR_LOGGING) == 0)
		(*pr)("\tno log\n");
	else
		pr_printlog(pp, NULL, pr);

 skip_log:

	if (print_cache == 0)
		goto skip_cache;

	TAILQ_FOREACH(pc, &pp->pr_cachelist, pc_poollist) {
		(*pr)("\tcache %p: allocfrom %p freeto %p\n", pc,
		    pc->pc_allocfrom, pc->pc_freeto);
		(*pr)("\t    hits %lu misses %lu ngroups %lu nitems %lu\n",
		    pc->pc_hits, pc->pc_misses, pc->pc_ngroups, pc->pc_nitems);
		TAILQ_FOREACH(pcg, &pc->pc_grouplist, pcg_list) {
			(*pr)("\t\tgroup %p: avail %d\n", pcg, pcg->pcg_avail);
			for (i = 0; i < PCG_NOBJECTS; i++)
				(*pr)("\t\t\t%p\n", pcg->pcg_objects[i]);
		}
	}

 skip_cache:

	pr_enter_check(pp, pr);
}

int
pool_chk(struct pool *pp, const char *label)
{
	struct pool_item_header *ph;
	int r = 0;

	simple_lock(&pp->pr_slock);

	TAILQ_FOREACH(ph, &pp->pr_pagelist, ph_pagelist) {
		struct pool_item *pi;
		int n;
		caddr_t page;

		page = (caddr_t)((vaddr_t)ph & pp->pr_alloc->pa_pagemask);
		if (page != ph->ph_page &&
		    (pp->pr_roflags & PR_PHINPAGE) != 0) {
			if (label != NULL)
				printf("%s: ", label);
			printf("pool(%p:%s): page inconsistency: page %p;"
			       " at page head addr %p (p %p)\n", pp,
				pp->pr_wchan, ph->ph_page,
				ph, page);
			r++;
			goto out;
		}

		for (pi = TAILQ_FIRST(&ph->ph_itemlist), n = 0;
		     pi != NULL;
		     pi = TAILQ_NEXT(pi,pi_list), n++) {

#ifdef DIAGNOSTIC
			if (pi->pi_magic != PI_MAGIC) {
				if (label != NULL)
					printf("%s: ", label);
				printf("pool(%s): free list modified: magic=%x;"
				       " page %p; item ordinal %d;"
				       " addr %p (p %p)\n",
					pp->pr_wchan, pi->pi_magic, ph->ph_page,
					n, pi, page);
				panic("pool");
			}
#endif
			page = (caddr_t)((vaddr_t)pi & pp->pr_alloc->pa_pagemask);
			if (page == ph->ph_page)
				continue;

			if (label != NULL)
				printf("%s: ", label);
			printf("pool(%p:%s): page inconsistency: page %p;"
			       " item ordinal %d; addr %p (p %p)\n", pp,
				pp->pr_wchan, ph->ph_page,
				n, pi, page);
			r++;
			goto out;
		}
	}
out:
	simple_unlock(&pp->pr_slock);
	return (r);
}

/*
 * pool_cache_init:
 *
 *	Initialize a pool cache.
 *
 *	NOTE: If the pool must be protected from interrupts, we expect
 *	to be called at the appropriate interrupt priority level.
 */
void
pool_cache_init(struct pool_cache *pc, struct pool *pp,
    int (*ctor)(void *, void *, int),
    void (*dtor)(void *, void *),
    void *arg)
{

	TAILQ_INIT(&pc->pc_grouplist);
	simple_lock_init(&pc->pc_slock);

	pc->pc_allocfrom = NULL;
	pc->pc_freeto = NULL;
	pc->pc_pool = pp;

	pc->pc_ctor = ctor;
	pc->pc_dtor = dtor;
	pc->pc_arg  = arg;

	pc->pc_hits   = 0;
	pc->pc_misses = 0;

	pc->pc_ngroups = 0;

	pc->pc_nitems = 0;

	simple_lock(&pp->pr_slock);
	TAILQ_INSERT_TAIL(&pp->pr_cachelist, pc, pc_poollist);
	simple_unlock(&pp->pr_slock);
}

/*
 * pool_cache_destroy:
 *
 *	Destroy a pool cache.
 */
void
pool_cache_destroy(struct pool_cache *pc)
{
	struct pool *pp = pc->pc_pool;

	/* First, invalidate the entire cache. */
	pool_cache_invalidate(pc);

	/* ...and remove it from the pool's cache list. */
	simple_lock(&pp->pr_slock);
	TAILQ_REMOVE(&pp->pr_cachelist, pc, pc_poollist);
	simple_unlock(&pp->pr_slock);
}

static __inline void *
pcg_get(struct pool_cache_group *pcg)
{
	void *object;
	u_int idx;

	KASSERT(pcg->pcg_avail <= PCG_NOBJECTS);
	KASSERT(pcg->pcg_avail != 0);
	idx = --pcg->pcg_avail;

	KASSERT(pcg->pcg_objects[idx] != NULL);
	object = pcg->pcg_objects[idx];
	pcg->pcg_objects[idx] = NULL;

	return (object);
}

static __inline void
pcg_put(struct pool_cache_group *pcg, void *object)
{
	u_int idx;

	KASSERT(pcg->pcg_avail < PCG_NOBJECTS);
	idx = pcg->pcg_avail++;

	KASSERT(pcg->pcg_objects[idx] == NULL);
	pcg->pcg_objects[idx] = object;
}

/*
 * pool_cache_get:
 *
 *	Get an object from a pool cache.
 */
void *
pool_cache_get(struct pool_cache *pc, int flags)
{
	struct pool_cache_group *pcg;
	void *object;

#ifdef LOCKDEBUG
	if (flags & PR_WAITOK)
		simple_lock_only_held(NULL, "pool_cache_get(PR_WAITOK)");
#endif

	simple_lock(&pc->pc_slock);

	if ((pcg = pc->pc_allocfrom) == NULL) {
		TAILQ_FOREACH(pcg, &pc->pc_grouplist, pcg_list) {
			if (pcg->pcg_avail != 0) {
				pc->pc_allocfrom = pcg;
				goto have_group;
			}
		}

		/*
		 * No groups with any available objects.  Allocate
		 * a new object, construct it, and return it to
		 * the caller.  We will allocate a group, if necessary,
		 * when the object is freed back to the cache.
		 */
		pc->pc_misses++;
		simple_unlock(&pc->pc_slock);
		object = pool_get(pc->pc_pool, flags);
		if (object != NULL && pc->pc_ctor != NULL) {
			if ((*pc->pc_ctor)(pc->pc_arg, object, flags) != 0) {
				pool_put(pc->pc_pool, object);
				return (NULL);
			}
		}
		return (object);
	}

 have_group:
	pc->pc_hits++;
	pc->pc_nitems--;
	object = pcg_get(pcg);

	if (pcg->pcg_avail == 0)
		pc->pc_allocfrom = NULL;

	simple_unlock(&pc->pc_slock);

	return (object);
}

/*
 * pool_cache_put:
 *
 *	Put an object back to the pool cache.
 */
void
pool_cache_put(struct pool_cache *pc, void *object)
{
	struct pool_cache_group *pcg;
	int s;

	simple_lock(&pc->pc_slock);

	if ((pcg = pc->pc_freeto) == NULL) {
		TAILQ_FOREACH(pcg, &pc->pc_grouplist, pcg_list) {
			if (pcg->pcg_avail != PCG_NOBJECTS) {
				pc->pc_freeto = pcg;
				goto have_group;
			}
		}

		/*
		 * No empty groups to free the object to.  Attempt to
		 * allocate one.
		 */
		simple_unlock(&pc->pc_slock);
		s = splvm();
		pcg = pool_get(&pcgpool, PR_NOWAIT);
		splx(s);
		if (pcg != NULL) {
			memset(pcg, 0, sizeof(*pcg));
			simple_lock(&pc->pc_slock);
			pc->pc_ngroups++;
			TAILQ_INSERT_TAIL(&pc->pc_grouplist, pcg, pcg_list);
			if (pc->pc_freeto == NULL)
				pc->pc_freeto = pcg;
			goto have_group;
		}

		/*
		 * Unable to allocate a cache group; destruct the object
		 * and free it back to the pool.
		 */
		pool_cache_destruct_object(pc, object);
		return;
	}

 have_group:
	pc->pc_nitems++;
	pcg_put(pcg, object);

	if (pcg->pcg_avail == PCG_NOBJECTS)
		pc->pc_freeto = NULL;

	simple_unlock(&pc->pc_slock);
}

/*
 * pool_cache_destruct_object:
 *
 *	Force destruction of an object and its release back into
 *	the pool.
 */
void
pool_cache_destruct_object(struct pool_cache *pc, void *object)
{

	if (pc->pc_dtor != NULL)
		(*pc->pc_dtor)(pc->pc_arg, object);
	pool_put(pc->pc_pool, object);
}

/*
 * pool_cache_do_invalidate:
 *
 *	This internal function implements pool_cache_invalidate() and
 *	pool_cache_reclaim().
 */
static void
pool_cache_do_invalidate(struct pool_cache *pc, int free_groups,
    void (*putit)(struct pool *, void *))
{
	struct pool_cache_group *pcg, *npcg;
	void *object;
	int s;

	for (pcg = TAILQ_FIRST(&pc->pc_grouplist); pcg != NULL;
	     pcg = npcg) {
		npcg = TAILQ_NEXT(pcg, pcg_list);
		while (pcg->pcg_avail != 0) {
			pc->pc_nitems--;
			object = pcg_get(pcg);
			if (pcg->pcg_avail == 0 && pc->pc_allocfrom == pcg)
				pc->pc_allocfrom = NULL;
			if (pc->pc_dtor != NULL)
				(*pc->pc_dtor)(pc->pc_arg, object);
			(*putit)(pc->pc_pool, object);
		}
		if (free_groups) {
			pc->pc_ngroups--;
			TAILQ_REMOVE(&pc->pc_grouplist, pcg, pcg_list);
			if (pc->pc_freeto == pcg)
				pc->pc_freeto = NULL;
			s = splvm();
			pool_put(&pcgpool, pcg);
			splx(s);
		}
	}
}

/*
 * pool_cache_invalidate:
 *
 *	Invalidate a pool cache (destruct and release all of the
 *	cached objects).
 */
void
pool_cache_invalidate(struct pool_cache *pc)
{

	simple_lock(&pc->pc_slock);
	pool_cache_do_invalidate(pc, 0, pool_put);
	simple_unlock(&pc->pc_slock);
}

/*
 * pool_cache_reclaim:
 *
 *	Reclaim a pool cache for pool_reclaim().
 */
static void
pool_cache_reclaim(struct pool_cache *pc)
{

	simple_lock(&pc->pc_slock);
	pool_cache_do_invalidate(pc, 1, pool_do_put);
	simple_unlock(&pc->pc_slock);
}

/*
 * We have three different sysctls.
 * kern.pool.npools - the number of pools.
 * kern.pool.pool.<pool#> - the pool struct for the pool#.
 * kern.pool.name.<pool#> - the name for pool#.[6~
 */
int
sysctl_dopool(int *name, u_int namelen, char *where, size_t *sizep)
{
	struct pool *pp, *foundpool = NULL;
	size_t buflen = where != NULL ? *sizep : 0;
	int npools = 0, s;
	unsigned int lookfor;
	size_t len;

	switch (*name) {
	case KERN_POOL_NPOOLS:
		if (namelen != 1 || buflen != sizeof(int))
			return (EINVAL);
		lookfor = 0;
		break;
	case KERN_POOL_NAME:
		if (namelen != 2 || buflen < 1)
			return (EINVAL);
		lookfor = name[1];
		break;
	case KERN_POOL_POOL:
		if (namelen != 2 || buflen != sizeof(struct pool))
			return (EINVAL);
		lookfor = name[1];
		break;
	default:
		return (EINVAL);
	}

	s = splvm();
	simple_lock(&pool_head_slock);

	TAILQ_FOREACH(pp, &pool_head, pr_poollist) {
		npools++;
		if (lookfor == pp->pr_serial) {
			foundpool = pp;
			break;
		}
	}

	simple_unlock(&pool_head_slock);
	splx(s);

	if (lookfor != 0 && foundpool == NULL)
		return (ENOENT);

	switch (*name) {
	case KERN_POOL_NPOOLS:
		return copyout(&npools, where, buflen);
	case KERN_POOL_NAME:
		len = strlen(foundpool->pr_wchan) + 1;
		if (*sizep < len)
			return (ENOMEM);
		*sizep = len;
		return copyout(foundpool->pr_wchan, where, len);
	case KERN_POOL_POOL:
		return copyout(foundpool, where, buflen);
	}
	/* NOTREACHED */
	return (0); /* XXX - Stupid gcc */
}

/*
 * Pool backend allocators.
 *
 * Each pool has a backend allocator that handles allocation, deallocation
 * and any additional draining that might be needed.
 *
 * We provide two standard allocators.
 *  pool_alloc_kmem - the default used when no allocator is specified.
 *  pool_alloc_nointr - used for pools that will not be accessed in
 *   interrupt context.
 */
void	*pool_page_alloc(struct pool *, int);
void	pool_page_free(struct pool *, void *);
void	*pool_page_alloc_nointr(struct pool *, int);
void	pool_page_free_nointr(struct pool *, void *);

struct pool_allocator pool_allocator_kmem = {
	pool_page_alloc, pool_page_free, 0,
};
struct pool_allocator pool_allocator_nointr = {
	pool_page_alloc_nointr, pool_page_free_nointr, 0,
};

/*
 * XXX - we have at least three different resources for the same allocation
 *  and each resource can be depleted. First we have the ready elements in
 *  the pool. Then we have the resource (typically a vm_map) for this
 *  allocator, then we have physical memory. Waiting for any of these can
 *  be unnecessary when any other is freed, but the kernel doesn't support
 *  sleeping on multiple addresses, so we have to fake. The caller sleeps on
 *  the pool (so that we can be awakened when an item is returned to the pool),
 *  but we set PA_WANT on the allocator. When a page is returned to
 *  the allocator and PA_WANT is set pool_allocator_free will wakeup all
 *  sleeping pools belonging to this allocator. (XXX - thundering herd).
 */

void *
pool_allocator_alloc(struct pool *org, int flags)
{
	struct pool_allocator *pa = org->pr_alloc;
	struct pool *pp, *start;
	int s, freed;
	void *res;

	do {
		if ((res = (*pa->pa_alloc)(org, flags)) != NULL)
			return (res);
		if ((flags & PR_WAITOK) == 0) {
			/*
			 * We only run the drain hook here if PR_NOWAIT.
			 * In other cases the hook will be run in
			 * pool_reclaim.
			 */
			if (org->pr_drain_hook == NULL)
				break;
			(*org->pr_drain_hook)(org->pr_drain_hook_arg, flags);
			if ((res = (*pa->pa_alloc)(org, flags)) != NULL)
				continue;
			break;
		}

		/*
		 * Drain all pools, except 'org', that use this allocator.
		 * We do this to reclaim va space. pa_alloc is responsible
		 * for waiting for physical memory.
		 * XXX - we risk looping forever if start if someone calls
		 *  pool_destroy on 'start'. But there is no other way to
		 *  have potentially sleeping pool_reclaim, non-sleeping
		 *  locks on pool_allocator and some stirring of drained
		 *  pools in the allocator.
		 * XXX - maybe we should use pool_head_slock for locking
		 *  the allocators?
		 */
		freed = 0;

		s = splvm();
		simple_lock(&pa->pa_slock);
		pp = start = TAILQ_FIRST(&pa->pa_list);
		do {
			TAILQ_REMOVE(&pa->pa_list, pp, pr_alloc_list);
			TAILQ_INSERT_TAIL(&pa->pa_list, pp, pr_alloc_list);
			if (pp == org)
				continue;
			simple_unlock(&pa->pa_list);
			freed = pool_reclaim(pp)
			simple_lock(&pa->pa_list);
		} while ((pp = TAILQ_FIRST(&pa->pa_list)) != start && !freed);

		if (!freed) {
			/*
			 * We set PA_WANT here, the caller will most likely
			 * sleep waiting for pages (if not, this won't hurt
			 * that much) and there is no way to set this in the
			 * caller without violating locking order.
			 */
			pa->pa_flags |= PA_WANT;
		}
		simple_unlock(&pa->pa_slock);
		splx(s);
	} while (freed);
	return (NULL);
}

void
pool_allocator_free(struct pool *pp, void *v)
{
	struct pool_allocator *pa = pp->pr_alloc;

	(*pa->pa_free)(pp, v);

	simple_lock(&pa->pa_slock);
	if ((pa->pa_flags & PA_WANT) == 0) {
		simple_unlock(&pa->pa_slock);
		return;
	}

	TAILQ_FOREACH(pp, &pa->pa_list, pr_alloc_list) {
		simple_lock(&pp->pr_slock);
		if ((pp->pr_flags & PR_WANTED) != 0) {
			pp->pr_flags &= ~PR_WANTED;
			wakeup(pp);
		}
	}
	pa->pa_flags &= ~PA_WANT;
	simple_unlock(&pa->pa_slock);
}

void *
pool_page_alloc(struct pool *pp, int flags)
{
	boolean_t waitok = (flags & PR_WAITOK) ? TRUE : FALSE;

	return ((void *)uvm_km_alloc_poolpage(waitok));
}

void
pool_page_free(struct pool *pp, void *v)
{
	uvm_km_free_poolpage((vaddr_t)v);
}

void *
pool_page_alloc_nointr(struct pool *pp, int flags)
{
	boolean_t waitok = (flags & PR_WAITOK) ? TRUE : FALSE;

	return ((void *)uvm_km_alloc_poolpage1(kernel_map, uvm.kernel_object,
	    waitok));
}

void
pool_page_free_nointr(struct pool *pp, void *v)
{
	uvm_km_free_poolpage1(kernel_map, (vaddr_t)v);
}
