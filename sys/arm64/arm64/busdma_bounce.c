/*-
 * Copyright (c) 1997, 1998 Justin T. Gibbs.
 * Copyright (c) 2015 The FreeBSD Foundation
 * All rights reserved.
 *
 * Portions of this software were developed by Semihalf
 * under sponsorship of the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/proc.h>
#include <sys/memdesc.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/md_var.h>
#include <arm64/include/bus_dma_impl.h>

#define MAX_BPAGES 4096

enum {
	BUS_DMA_COULD_BOUNCE	= 0x01,
	BUS_DMA_MIN_ALLOC_COMP	= 0x02,
	BUS_DMA_KMEM_ALLOC	= 0x04,
};

struct bounce_zone;
extern struct bus_dma_impl bus_dma_nobounce_kernel_impl;

struct bus_dma_tag {
	struct bus_dma_tag_common common;
	int			map_count;
	int			bounce_flags;
	bus_dma_segment_t	*segments;
	struct bounce_zone	*bounce_zone;
};

struct bounce_page {
	vm_offset_t	vaddr;		/* kva of bounce buffer */
	bus_addr_t	busaddr;	/* Physical address */
	vm_offset_t	datavaddr;	/* kva of client data */
	bus_addr_t	dataaddr;	/* client physical address */
	bus_size_t	datacount;	/* client data count */
	STAILQ_ENTRY(bounce_page) links;
};

int busdma_swi_pending;

struct bounce_zone {
	STAILQ_ENTRY(bounce_zone) links;
	STAILQ_HEAD(bp_list, bounce_page) bounce_page_list;
	int		total_bpages;
	int		free_bpages;
	int		reserved_bpages;
	int		active_bpages;
	int		total_bounced;
	int		total_deferred;
	int		map_count;
	bus_size_t	alignment;
	bus_addr_t	lowaddr;
	char		zoneid[8];
	char		lowaddrid[20];
	struct sysctl_ctx_list sysctl_tree;
	struct sysctl_oid *sysctl_tree_top;
};

static struct mtx bounce_lock;
static int total_bpages;
static int busdma_zonecount;
static STAILQ_HEAD(, bounce_zone) bounce_zone_list;

static SYSCTL_NODE(_hw, OID_AUTO, busdma, CTLFLAG_RD, 0, "Busdma parameters");
SYSCTL_INT(_hw_busdma, OID_AUTO, total_bpages, CTLFLAG_RD, &total_bpages, 0,
	   "Total bounce pages");

struct bus_dmamap {
	struct bp_list	       bpages;
	int		       pagesneeded;
	int		       pagesreserved;
	bus_dma_tag_t	       dmat;
	struct memdesc	       mem;
	bus_dmamap_callback_t *callback;
	void		      *callback_arg;
	STAILQ_ENTRY(bus_dmamap) links;
};

static STAILQ_HEAD(, bus_dmamap) bounce_map_waitinglist;
static STAILQ_HEAD(, bus_dmamap) bounce_map_callbacklist;
static struct bus_dmamap nobounce_dmamap;

static void init_bounce_pages(void *dummy);
static int alloc_bounce_zone(bus_dma_tag_t dmat);
static int alloc_bounce_pages(bus_dma_tag_t dmat, u_int numpages);
static int reserve_bounce_pages(bus_dma_tag_t dmat, bus_dmamap_t map,
    int commit);
static bus_addr_t add_bounce_page(bus_dma_tag_t dmat, bus_dmamap_t map,
    vm_offset_t vaddr, bus_addr_t addr, bus_size_t size);
static void free_bounce_page(bus_dma_tag_t dmat, struct bounce_page *bpage);
int run_filter(bus_dma_tag_t dmat, bus_addr_t paddr);
static void _bus_dmamap_count_pages(bus_dma_tag_t dmat, bus_dmamap_t map,
    pmap_t pmap, void *buf, bus_size_t buflen, int flags);
static void _bus_dmamap_count_phys(bus_dma_tag_t dmat, bus_dmamap_t map,
    vm_paddr_t buf, bus_size_t buflen, int flags);
static int _bus_dmamap_reserve_pages(bus_dma_tag_t dmat, bus_dmamap_t map,
    int flags);

/*
 * Allocate a device specific dma_tag.
 */
static int
bounce_bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
    bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
    bus_dma_filter_t *filter, void *filterarg, bus_size_t maxsize,
    int nsegments, bus_size_t maxsegsz, int flags, bus_dma_lock_t *lockfunc,
    void *lockfuncarg, bus_dma_tag_t *dmat)
{
	bus_dma_tag_t newtag;
	int error;

	*dmat = NULL;
	error = common_bus_dma_tag_create(parent != NULL ? &parent->common :
	    NULL, alignment, boundary, lowaddr, highaddr, filter, filterarg,
	    maxsize, nsegments, maxsegsz, flags, lockfunc, lockfuncarg,
	    sizeof (struct bus_dma_tag), (void **)&newtag);
	if (error != 0)
		return (error);

	newtag->common.impl = &bus_dma_bounce_impl;
	newtag->map_count = 0;
	newtag->segments = NULL;

	if (parent != NULL && ((newtag->common.filter != NULL) ||
	    ((parent->bounce_flags & BUS_DMA_COULD_BOUNCE) != 0)))
		newtag->bounce_flags |= BUS_DMA_COULD_BOUNCE;

	if (newtag->common.lowaddr < ptoa((vm_paddr_t)Maxmem) ||
	    newtag->common.alignment > 1)
		newtag->bounce_flags |= BUS_DMA_COULD_BOUNCE;

	if ((flags & BUS_DMA_KERNEL) && !(newtag->bounce_flags & BUS_DMA_COULD_BOUNCE))
		newtag->common->impl = &bus_dma_nobounce_kernel_impl;

	if (((newtag->bounce_flags & BUS_DMA_COULD_BOUNCE) != 0) &&
	    (flags & BUS_DMA_ALLOCNOW) != 0) {
		struct bounce_zone *bz;

		/* Must bounce */
		if ((error = alloc_bounce_zone(newtag)) != 0) {
			free(newtag, M_DEVBUF);
			return (error);
		}
		bz = newtag->bounce_zone;

		if (ptoa(bz->total_bpages) < maxsize) {
			int pages;

			pages = atop(maxsize) - bz->total_bpages;

			/* Add pages to our bounce pool */
			if (alloc_bounce_pages(newtag, pages) < pages)
				error = ENOMEM;
		}
		/* Performed initial allocation */
		newtag->bounce_flags |= BUS_DMA_MIN_ALLOC_COMP;
	} else
		error = 0;

	if (error != 0)
		free(newtag, M_DEVBUF);
	else
		*dmat = newtag;
	CTR4(KTR_BUSDMA, "%s returned tag %p tag flags 0x%x error %d",
	    __func__, newtag, (newtag != NULL ? newtag->common.flags : 0),
	    error);
	return (error);
}

static int
bounce_bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
	bus_dma_tag_t dmat_copy, parent;
	int error;

	error = 0;
	dmat_copy = dmat;

	if (dmat != NULL) {
		if (dmat->map_count != 0) {
			error = EBUSY;
			goto out;
		}
		while (dmat != NULL) {
			parent = (bus_dma_tag_t)dmat->common.parent;
			atomic_subtract_int(&dmat->common.ref_count, 1);
			if (dmat->common.ref_count == 0) {
				if (dmat->segments != NULL)
					free(dmat->segments, M_DEVBUF);
				free(dmat, M_DEVBUF);
				/*
				 * Last reference count, so
				 * release our reference
				 * count on our parent.
				 */
				dmat = parent;
			} else
				dmat = NULL;
		}
	}
out:
	CTR3(KTR_BUSDMA, "%s tag %p error %d", __func__, dmat_copy, error);
	return (error);
}

/*
 * Allocate a handle for mapping from kva/uva/physical
 * address space into bus device space.
 */
static int
bounce_bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp)
{
	struct bounce_zone *bz;
	int error, maxpages, pages;

	error = 0;

	if (dmat->segments == NULL) {
		dmat->segments = (bus_dma_segment_t *)malloc(
		    sizeof(bus_dma_segment_t) * dmat->common.nsegments,
		    M_DEVBUF, M_NOWAIT);
		if (dmat->segments == NULL) {
			CTR3(KTR_BUSDMA, "%s: tag %p error %d",
			    __func__, dmat, ENOMEM);
			return (ENOMEM);
		}
	}

	/*
	 * Bouncing might be required if the driver asks for an active
	 * exclusion region, a data alignment that is stricter than 1, and/or
	 * an active address boundary.
	 */
	if (dmat->bounce_flags & BUS_DMA_COULD_BOUNCE) {
		/* Must bounce */
		if (dmat->bounce_zone == NULL) {
			if ((error = alloc_bounce_zone(dmat)) != 0)
				return (error);
		}
		bz = dmat->bounce_zone;

		*mapp = (bus_dmamap_t)malloc(sizeof(**mapp), M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (*mapp == NULL) {
			CTR3(KTR_BUSDMA, "%s: tag %p error %d",
			    __func__, dmat, ENOMEM);
			return (ENOMEM);
		}

		/* Initialize the new map */
		STAILQ_INIT(&((*mapp)->bpages));

		/*
		 * Attempt to add pages to our pool on a per-instance
		 * basis up to a sane limit.
		 */
		if (dmat->common.alignment > 1)
			maxpages = MAX_BPAGES;
		else
			maxpages = MIN(MAX_BPAGES, Maxmem -
			    atop(dmat->common.lowaddr));
		if ((dmat->bounce_flags & BUS_DMA_MIN_ALLOC_COMP) == 0 ||
		    (bz->map_count > 0 && bz->total_bpages < maxpages)) {
			pages = MAX(atop(dmat->common.maxsize), 1);
			pages = MIN(maxpages - bz->total_bpages, pages);
			pages = MAX(pages, 1);
			if (alloc_bounce_pages(dmat, pages) < pages)
				error = ENOMEM;
			if ((dmat->bounce_flags & BUS_DMA_MIN_ALLOC_COMP)
			    == 0) {
				if (error == 0) {
					dmat->bounce_flags |=
					    BUS_DMA_MIN_ALLOC_COMP;
				}
			} else
				error = 0;
		}
		bz->map_count++;
	} else {
		*mapp = NULL;
	}
	if (error == 0)
		dmat->map_count++;
	CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
	    __func__, dmat, dmat->common.flags, error);
	return (error);
}

/*
 * Destroy a handle for mapping from kva/uva/physical
 * address space into bus device space.
 */
static int
bounce_bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map)
{

	if (map != NULL && map != &nobounce_dmamap) {
		if (STAILQ_FIRST(&map->bpages) != NULL) {
			CTR3(KTR_BUSDMA, "%s: tag %p error %d",
			    __func__, dmat, EBUSY);
			return (EBUSY);
		}
		if (dmat->bounce_zone)
			dmat->bounce_zone->map_count--;
		free(map, M_DEVBUF);
	}
	dmat->map_count--;
	CTR2(KTR_BUSDMA, "%s: tag %p error 0", __func__, dmat);
	return (0);
}


/*
 * Allocate a piece of memory that can be efficiently mapped into
 * bus device space based on the constraints lited in the dma tag.
 * A dmamap to for use with dmamap_load is also allocated.
 */
static int
bounce_bus_dmamem_alloc(bus_dma_tag_t dmat, void** vaddr, int flags,
    bus_dmamap_t *mapp)
{
	/*
	 * XXX ARM64TODO:
	 * This bus_dma implementation requires IO-Coherent architecutre.
	 * If IO-Coherency is not guaranteed, the BUS_DMA_COHERENT flag has
	 * to be implented using non-cacheable memory.
	 */

	vm_memattr_t attr;
	int mflags;

	if (flags & BUS_DMA_NOWAIT)
		mflags = M_NOWAIT;
	else
		mflags = M_WAITOK;

	/* If we succeed, no mapping/bouncing will be required */
	*mapp = NULL;

	if (dmat->segments == NULL) {
		dmat->segments = (bus_dma_segment_t *)malloc(
		    sizeof(bus_dma_segment_t) * dmat->common.nsegments,
		    M_DEVBUF, mflags);
		if (dmat->segments == NULL) {
			CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
			    __func__, dmat, dmat->common.flags, ENOMEM);
			return (ENOMEM);
		}
	}
	if (flags & BUS_DMA_ZERO)
		mflags |= M_ZERO;
	if (flags & BUS_DMA_NOCACHE)
		attr = VM_MEMATTR_UNCACHEABLE;
	else
		attr = VM_MEMATTR_DEFAULT;

	/*
	 * XXX:
	 * (dmat->alignment < dmat->maxsize) is just a quick hack; the exact
	 * alignment guarantees of malloc need to be nailed down, and the
	 * code below should be rewritten to take that into account.
	 *
	 * In the meantime, we'll warn the user if malloc gets it wrong.
	 */
	if ((dmat->common.maxsize <= PAGE_SIZE) &&
	   (dmat->common.alignment < dmat->common.maxsize) &&
	    dmat->common.lowaddr >= ptoa((vm_paddr_t)Maxmem) &&
	    attr == VM_MEMATTR_DEFAULT) {
		*vaddr = malloc(dmat->common.maxsize, M_DEVBUF, mflags);
	} else if (dmat->common.nsegments >= btoc(dmat->common.maxsize) &&
	    dmat->common.alignment <= PAGE_SIZE &&
	    (dmat->common.boundary == 0 ||
	    dmat->common.boundary >= dmat->common.lowaddr)) {
		/* Page-based multi-segment allocations allowed */
		*vaddr = (void *)kmem_alloc_attr(kernel_arena,
		    dmat->common.maxsize, mflags, 0ul, dmat->common.lowaddr,
		    attr);
		dmat->bounce_flags |= BUS_DMA_KMEM_ALLOC;
	} else {
		*vaddr = (void *)kmem_alloc_contig(kernel_arena,
		    dmat->common.maxsize, mflags, 0ul, dmat->common.lowaddr,
		    dmat->common.alignment != 0 ? dmat->common.alignment : 1ul,
		    dmat->common.boundary, attr);
		dmat->bounce_flags |= BUS_DMA_KMEM_ALLOC;
	}
	if (*vaddr == NULL) {
		CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
		    __func__, dmat, dmat->common.flags, ENOMEM);
		return (ENOMEM);
	} else if (vtophys(*vaddr) & (dmat->common.alignment - 1)) {
		printf("bus_dmamem_alloc failed to align memory properly.\n");
	}
	CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x error %d",
	    __func__, dmat, dmat->common.flags, 0);
	return (0);
}

/*
 * Free a piece of memory and it's allociated dmamap, that was allocated
 * via bus_dmamem_alloc.  Make the same choice for free/contigfree.
 */
static void
bounce_bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map)
{
	/*
	 * dmamem does not need to be bounced, so the map should be
	 * NULL and the BUS_DMA_KMEM_ALLOC flag cleared if malloc()
	 * was used and set if kmem_alloc_contig() was used.
	 */
	if (map != NULL)
		panic("bus_dmamem_free: Invalid map freed\n");
	if ((dmat->bounce_flags & BUS_DMA_KMEM_ALLOC) == 0)
		free(vaddr, M_DEVBUF);
	else
		kmem_free(kernel_arena, (vm_offset_t)vaddr,
		    dmat->common.maxsize);
	CTR3(KTR_BUSDMA, "%s: tag %p flags 0x%x", __func__, dmat,
	    dmat->bounce_flags);
}

static void
_bus_dmamap_count_phys(bus_dma_tag_t dmat, bus_dmamap_t map, vm_paddr_t buf,
    bus_size_t buflen, int flags)
{
	bus_addr_t curaddr;
	bus_size_t sgsize;

	if ((map != &nobounce_dmamap && map->pagesneeded == 0)) {
		/*
		 * Count the number of bounce pages
		 * needed in order to complete this transfer
		 */
		curaddr = buf;
		while (buflen != 0) {
			sgsize = MIN(buflen, dmat->common.maxsegsz);
			if (bus_dma_run_filter(&dmat->common, curaddr)) {
				sgsize = MIN(sgsize, PAGE_SIZE);
				map->pagesneeded++;
			}
			curaddr += sgsize;
			buflen -= sgsize;
		}
		CTR1(KTR_BUSDMA, "pagesneeded= %d\n", map->pagesneeded);
	}
}

static void
_bus_dmamap_count_pages(bus_dma_tag_t dmat, bus_dmamap_t map, pmap_t pmap,
    void *buf, bus_size_t buflen, int flags)
{
	vm_offset_t vaddr;
	vm_offset_t vendaddr;
	bus_addr_t paddr;
	bus_size_t sg_len;

	if ((map != &nobounce_dmamap && map->pagesneeded == 0)) {
		CTR4(KTR_BUSDMA, "lowaddr= %d Maxmem= %d, boundary= %d, "
		    "alignment= %d", dmat->common.lowaddr,
		    ptoa((vm_paddr_t)Maxmem),
		    dmat->common.boundary, dmat->common.alignment);
		CTR3(KTR_BUSDMA, "map= %p, nobouncemap= %p, pagesneeded= %d",
		    map, &nobounce_dmamap, map->pagesneeded);
		/*
		 * Count the number of bounce pages
		 * needed in order to complete this transfer
		 */
		vaddr = (vm_offset_t)buf;
		vendaddr = (vm_offset_t)buf + buflen;

		while (vaddr < vendaddr) {
			sg_len = PAGE_SIZE - ((vm_offset_t)vaddr & PAGE_MASK);
			if (pmap == kernel_pmap)
				paddr = pmap_kextract(vaddr);
			else
				paddr = pmap_extract(pmap, vaddr);
			if (bus_dma_run_filter(&dmat->common, paddr) != 0) {
				sg_len = roundup2(sg_len,
				    dmat->common.alignment);
				map->pagesneeded++;
			}
			vaddr += sg_len;
		}
		CTR1(KTR_BUSDMA, "pagesneeded= %d\n", map->pagesneeded);
	}
}

static int
_bus_dmamap_reserve_pages(bus_dma_tag_t dmat, bus_dmamap_t map, int flags)
{

	/* Reserve Necessary Bounce Pages */
	mtx_lock(&bounce_lock);
	if (flags & BUS_DMA_NOWAIT) {
		if (reserve_bounce_pages(dmat, map, 0) != 0) {
			mtx_unlock(&bounce_lock);
			return (ENOMEM);
		}
	} else {
		if (reserve_bounce_pages(dmat, map, 1) != 0) {
			/* Queue us for resources */
			STAILQ_INSERT_TAIL(&bounce_map_waitinglist, map, links);
			mtx_unlock(&bounce_lock);
			return (EINPROGRESS);
		}
	}
	mtx_unlock(&bounce_lock);

	return (0);
}

/*
 * Add a single contiguous physical range to the segment list.
 */
static int
_bus_dmamap_addseg(bus_dma_tag_t dmat, bus_addr_t curaddr,
    bus_size_t sgsize, bus_dma_segment_t *segs, int *segp)
{
	bus_addr_t baddr, bmask;
	int seg;

	/*
	 * Make sure we don't cross any boundaries.
	 */
	bmask = ~(dmat->common.boundary - 1);
	if (dmat->common.boundary > 0) {
		baddr = (curaddr + dmat->common.boundary) & bmask;
		if (sgsize > (baddr - curaddr))
			sgsize = (baddr - curaddr);
	}

	/*
	 * Insert chunk into a segment, coalescing with
	 * previous segment if possible.
	 */
	seg = *segp;
	if (seg == -1) {
		seg = 0;
		segs[seg].ds_addr = curaddr;
		segs[seg].ds_len = sgsize;
	} else {
		if (curaddr == segs[seg].ds_addr + segs[seg].ds_len &&
		    (segs[seg].ds_len + sgsize) <= dmat->common.maxsegsz &&
		    (dmat->common.boundary == 0 ||
		     (segs[seg].ds_addr & bmask) == (curaddr & bmask)))
			segs[seg].ds_len += sgsize;
		else {
			if (++seg >= dmat->common.nsegments)
				return (0);
			segs[seg].ds_addr = curaddr;
			segs[seg].ds_len = sgsize;
		}
	}
	*segp = seg;
	return (sgsize);
}

/*
 * Utility function to load a physical buffer.  segp contains
 * the starting segment on entrace, and the ending segment on exit.
 */
static int
bounce_bus_dmamap_load_phys(bus_dma_tag_t dmat, bus_dmamap_t map,
    vm_paddr_t buf, bus_size_t buflen, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	bus_size_t sgsize;
	bus_addr_t curaddr;
	int error;

	if (map == NULL)
		map = &nobounce_dmamap;

	if (segs == NULL)
		segs = dmat->segments;

	if ((dmat->bounce_flags & BUS_DMA_COULD_BOUNCE) != 0) {
		_bus_dmamap_count_phys(dmat, map, buf, buflen, flags);
		if (map->pagesneeded != 0) {
			error = _bus_dmamap_reserve_pages(dmat, map, flags);
			if (error)
				return (error);
		}
	}

	while (buflen > 0) {
		curaddr = buf;
		sgsize = MIN(buflen, dmat->common.maxsegsz);
		if (((dmat->bounce_flags & BUS_DMA_COULD_BOUNCE) != 0) &&
		    map->pagesneeded != 0 &&
		    bus_dma_run_filter(&dmat->common, curaddr)) {
			sgsize = MIN(sgsize, PAGE_SIZE);
			curaddr = add_bounce_page(dmat, map, 0, curaddr,
			    sgsize);
		}
		sgsize = _bus_dmamap_addseg(dmat, curaddr, sgsize, segs,
		    segp);
		if (sgsize == 0)
			break;
		buf += sgsize;
		buflen -= sgsize;
	}

	/*
	 * Did we fit?
	 */
	return (buflen != 0 ? EFBIG : 0); /* XXX better return value here? */
}

/*
 * Utility function to load a linear buffer.  segp contains
 * the starting segment on entrace, and the ending segment on exit.
 */
static int
bounce_bus_dmamap_load_buffer(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf,
    bus_size_t buflen, pmap_t pmap, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	bus_size_t sgsize, max_sgsize;
	bus_addr_t curaddr;
	vm_offset_t vaddr;
	int error;

	if (map == NULL)
		map = &nobounce_dmamap;

	if (segs == NULL)
		segs = dmat->segments;

	if ((dmat->bounce_flags & BUS_DMA_COULD_BOUNCE) != 0) {
		_bus_dmamap_count_pages(dmat, map, pmap, buf, buflen, flags);
		if (map->pagesneeded != 0) {
			error = _bus_dmamap_reserve_pages(dmat, map, flags);
			if (error)
				return (error);
		}
	}

	vaddr = (vm_offset_t)buf;
	while (buflen > 0) {
		/*
		 * Get the physical address for this segment.
		 */
		if (pmap == kernel_pmap)
			curaddr = pmap_kextract(vaddr);
		else
			curaddr = pmap_extract(pmap, vaddr);

		/*
		 * Compute the segment size, and adjust counts.
		 */
		max_sgsize = MIN(buflen, dmat->common.maxsegsz);
		sgsize = PAGE_SIZE - ((vm_offset_t)curaddr & PAGE_MASK);
		if (((dmat->bounce_flags & BUS_DMA_COULD_BOUNCE) != 0) &&
		    map->pagesneeded != 0 &&
		    bus_dma_run_filter(&dmat->common, curaddr)) {
			sgsize = roundup2(sgsize, dmat->common.alignment);
			sgsize = MIN(sgsize, max_sgsize);
			curaddr = add_bounce_page(dmat, map, vaddr, curaddr,
			    sgsize);
		} else {
			sgsize = MIN(sgsize, max_sgsize);
		}
		sgsize = _bus_dmamap_addseg(dmat, curaddr, sgsize, segs,
		    segp);
		if (sgsize == 0)
			break;
		vaddr += sgsize;
		buflen -= sgsize;
	}

	/*
	 * Did we fit?
	 */
	return (buflen != 0 ? EFBIG : 0); /* XXX better return value here? */
}

static int
nobounce_bus_dmamap_load_kernel_buffer(bus_dma_tag_t dmat, bus_dmamap_t map __unused, void *buf,
    bus_size_t buflen, pmap_t pmap __unused, int flags __unused, bus_dma_segment_t *segs,
    int *segp)
{
	bus_size_t sgsize, max_sgsize;
	bus_addr_t curaddr;
	vm_offset_t vaddr;

	if (segs == NULL)
		segs = dmat->segments;

	vaddr = (vm_offset_t)buf;
	while (buflen > 0) {
		/*
		 * Get the physical address for this segment.
		 */
		curaddr = pmap_kextract(vaddr);

		/*
		 * Compute the segment size, and adjust counts.
		 */
		max_sgsize = MIN(buflen, dmat->common.maxsegsz);
		sgsize = PAGE_SIZE - ((vm_offset_t)curaddr & PAGE_MASK);
		sgsize = MIN(sgsize, max_sgsize);
		sgsize = _bus_dmamap_addseg(dmat, curaddr, sgsize, segs,
		    segp);
		if (sgsize == 0)
			break;
		vaddr += sgsize;
		buflen -= sgsize;
	}

	/*
	 * Did we fit?
	 */
	return (buflen != 0 ? EFBIG : 0); /* XXX better return value here? */
}

static void
bounce_bus_dmamap_waitok(bus_dma_tag_t dmat, bus_dmamap_t map,
    struct memdesc *mem, bus_dmamap_callback_t *callback, void *callback_arg)
{

	if (map == NULL)
		return;
	map->mem = *mem;
	map->dmat = dmat;
	map->callback = callback;
	map->callback_arg = callback_arg;
}

static bus_dma_segment_t *
bounce_bus_dmamap_complete(bus_dma_tag_t dmat, bus_dmamap_t map,
    bus_dma_segment_t *segs, int nsegs, int error)
{

	if (segs == NULL)
		segs = dmat->segments;
	return (segs);
}

/*
 * Release the mapping held by map.
 */
static void
bounce_bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	struct bounce_page *bpage;

	while ((bpage = STAILQ_FIRST(&map->bpages)) != NULL) {
		STAILQ_REMOVE_HEAD(&map->bpages, links);
		free_bounce_page(dmat, bpage);
	}
}

static void
bounce_bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map,
    bus_dmasync_op_t op)
{
	struct bounce_page *bpage;

	/*
	 * XXX ARM64TODO:
	 * This bus_dma implementation requires IO-Coherent architecutre.
	 * If IO-Coherency is not guaranteed, cache operations have to be
	 * added to this function.
	 */

	if ((bpage = STAILQ_FIRST(&map->bpages)) != NULL) {
		/*
		 * Handle data bouncing.  We might also
		 * want to add support for invalidating
		 * the caches on broken hardware
		 */
		CTR4(KTR_BUSDMA, "%s: tag %p tag flags 0x%x op 0x%x "
		    "performing bounce", __func__, dmat,
		    dmat->common.flags, op);

		if ((op & BUS_DMASYNC_PREWRITE) != 0) {
			while (bpage != NULL) {
				if (bpage->datavaddr != 0) {
					bcopy((void *)bpage->datavaddr,
					    (void *)bpage->vaddr,
					    bpage->datacount);
				} else {
					physcopyout(bpage->dataaddr,
					    (void *)bpage->vaddr,
					    bpage->datacount);
				}
				bpage = STAILQ_NEXT(bpage, links);
			}
			dmat->bounce_zone->total_bounced++;
		}

		if ((op & BUS_DMASYNC_POSTREAD) != 0) {
			while (bpage != NULL) {
				if (bpage->datavaddr != 0) {
					bcopy((void *)bpage->vaddr,
					    (void *)bpage->datavaddr,
					    bpage->datacount);
				} else {
					physcopyin((void *)bpage->vaddr,
					    bpage->dataaddr,
					    bpage->datacount);
				}
				bpage = STAILQ_NEXT(bpage, links);
			}
			dmat->bounce_zone->total_bounced++;
		}
	}
}

static void
init_bounce_pages(void *dummy __unused)
{

	total_bpages = 0;
	STAILQ_INIT(&bounce_zone_list);
	STAILQ_INIT(&bounce_map_waitinglist);
	STAILQ_INIT(&bounce_map_callbacklist);
	mtx_init(&bounce_lock, "bounce pages lock", NULL, MTX_DEF);
}
SYSINIT(bpages, SI_SUB_LOCK, SI_ORDER_ANY, init_bounce_pages, NULL);

static struct sysctl_ctx_list *
busdma_sysctl_tree(struct bounce_zone *bz)
{
	return (&bz->sysctl_tree);
}

static struct sysctl_oid *
busdma_sysctl_tree_top(struct bounce_zone *bz)
{
	return (bz->sysctl_tree_top);
}

static int
alloc_bounce_zone(bus_dma_tag_t dmat)
{
	struct bounce_zone *bz;

	/* Check to see if we already have a suitable zone */
	STAILQ_FOREACH(bz, &bounce_zone_list, links) {
		if ((dmat->common.alignment <= bz->alignment) &&
		    (dmat->common.lowaddr >= bz->lowaddr)) {
			dmat->bounce_zone = bz;
			return (0);
		}
	}

	if ((bz = (struct bounce_zone *)malloc(sizeof(*bz), M_DEVBUF,
	    M_NOWAIT | M_ZERO)) == NULL)
		return (ENOMEM);

	STAILQ_INIT(&bz->bounce_page_list);
	bz->free_bpages = 0;
	bz->reserved_bpages = 0;
	bz->active_bpages = 0;
	bz->lowaddr = dmat->common.lowaddr;
	bz->alignment = MAX(dmat->common.alignment, PAGE_SIZE);
	bz->map_count = 0;
	snprintf(bz->zoneid, 8, "zone%d", busdma_zonecount);
	busdma_zonecount++;
	snprintf(bz->lowaddrid, 18, "%#jx", (uintmax_t)bz->lowaddr);
	STAILQ_INSERT_TAIL(&bounce_zone_list, bz, links);
	dmat->bounce_zone = bz;

	sysctl_ctx_init(&bz->sysctl_tree);
	bz->sysctl_tree_top = SYSCTL_ADD_NODE(&bz->sysctl_tree,
	    SYSCTL_STATIC_CHILDREN(_hw_busdma), OID_AUTO, bz->zoneid,
	    CTLFLAG_RD, 0, "");
	if (bz->sysctl_tree_top == NULL) {
		sysctl_ctx_free(&bz->sysctl_tree);
		return (0);	/* XXX error code? */
	}

	SYSCTL_ADD_INT(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "total_bpages", CTLFLAG_RD, &bz->total_bpages, 0,
	    "Total bounce pages");
	SYSCTL_ADD_INT(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "free_bpages", CTLFLAG_RD, &bz->free_bpages, 0,
	    "Free bounce pages");
	SYSCTL_ADD_INT(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "reserved_bpages", CTLFLAG_RD, &bz->reserved_bpages, 0,
	    "Reserved bounce pages");
	SYSCTL_ADD_INT(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "active_bpages", CTLFLAG_RD, &bz->active_bpages, 0,
	    "Active bounce pages");
	SYSCTL_ADD_INT(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "total_bounced", CTLFLAG_RD, &bz->total_bounced, 0,
	    "Total bounce requests");
	SYSCTL_ADD_INT(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "total_deferred", CTLFLAG_RD, &bz->total_deferred, 0,
	    "Total bounce requests that were deferred");
	SYSCTL_ADD_STRING(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "lowaddr", CTLFLAG_RD, bz->lowaddrid, 0, "");
	SYSCTL_ADD_UAUTO(busdma_sysctl_tree(bz),
	    SYSCTL_CHILDREN(busdma_sysctl_tree_top(bz)), OID_AUTO,
	    "alignment", CTLFLAG_RD, &bz->alignment, "");

	return (0);
}

static int
alloc_bounce_pages(bus_dma_tag_t dmat, u_int numpages)
{
	struct bounce_zone *bz;
	int count;

	bz = dmat->bounce_zone;
	count = 0;
	while (numpages > 0) {
		struct bounce_page *bpage;

		bpage = (struct bounce_page *)malloc(sizeof(*bpage), M_DEVBUF,
						     M_NOWAIT | M_ZERO);

		if (bpage == NULL)
			break;
		bpage->vaddr = (vm_offset_t)contigmalloc(PAGE_SIZE, M_DEVBUF,
		    M_NOWAIT, 0ul, bz->lowaddr, PAGE_SIZE, 0);
		if (bpage->vaddr == 0) {
			free(bpage, M_DEVBUF);
			break;
		}
		bpage->busaddr = pmap_kextract(bpage->vaddr);
		mtx_lock(&bounce_lock);
		STAILQ_INSERT_TAIL(&bz->bounce_page_list, bpage, links);
		total_bpages++;
		bz->total_bpages++;
		bz->free_bpages++;
		mtx_unlock(&bounce_lock);
		count++;
		numpages--;
	}
	return (count);
}

static int
reserve_bounce_pages(bus_dma_tag_t dmat, bus_dmamap_t map, int commit)
{
	struct bounce_zone *bz;
	int pages;

	mtx_assert(&bounce_lock, MA_OWNED);
	bz = dmat->bounce_zone;
	pages = MIN(bz->free_bpages, map->pagesneeded - map->pagesreserved);
	if (commit == 0 && map->pagesneeded > (map->pagesreserved + pages))
		return (map->pagesneeded - (map->pagesreserved + pages));
	bz->free_bpages -= pages;
	bz->reserved_bpages += pages;
	map->pagesreserved += pages;
	pages = map->pagesneeded - map->pagesreserved;

	return (pages);
}

static bus_addr_t
add_bounce_page(bus_dma_tag_t dmat, bus_dmamap_t map, vm_offset_t vaddr,
		bus_addr_t addr, bus_size_t size)
{
	struct bounce_zone *bz;
	struct bounce_page *bpage;

	KASSERT(dmat->bounce_zone != NULL, ("no bounce zone in dma tag"));
	KASSERT(map != NULL && map != &nobounce_dmamap,
	    ("add_bounce_page: bad map %p", map));

	bz = dmat->bounce_zone;
	if (map->pagesneeded == 0)
		panic("add_bounce_page: map doesn't need any pages");
	map->pagesneeded--;

	if (map->pagesreserved == 0)
		panic("add_bounce_page: map doesn't need any pages");
	map->pagesreserved--;

	mtx_lock(&bounce_lock);
	bpage = STAILQ_FIRST(&bz->bounce_page_list);
	if (bpage == NULL)
		panic("add_bounce_page: free page list is empty");

	STAILQ_REMOVE_HEAD(&bz->bounce_page_list, links);
	bz->reserved_bpages--;
	bz->active_bpages++;
	mtx_unlock(&bounce_lock);

	if (dmat->common.flags & BUS_DMA_KEEP_PG_OFFSET) {
		/* Page offset needs to be preserved. */
		bpage->vaddr |= addr & PAGE_MASK;
		bpage->busaddr |= addr & PAGE_MASK;
	}
	bpage->datavaddr = vaddr;
	bpage->dataaddr = addr;
	bpage->datacount = size;
	STAILQ_INSERT_TAIL(&(map->bpages), bpage, links);
	return (bpage->busaddr);
}

static void
free_bounce_page(bus_dma_tag_t dmat, struct bounce_page *bpage)
{
	struct bus_dmamap *map;
	struct bounce_zone *bz;

	bz = dmat->bounce_zone;
	bpage->datavaddr = 0;
	bpage->datacount = 0;
	if (dmat->common.flags & BUS_DMA_KEEP_PG_OFFSET) {
		/*
		 * Reset the bounce page to start at offset 0.  Other uses
		 * of this bounce page may need to store a full page of
		 * data and/or assume it starts on a page boundary.
		 */
		bpage->vaddr &= ~PAGE_MASK;
		bpage->busaddr &= ~PAGE_MASK;
	}

	mtx_lock(&bounce_lock);
	STAILQ_INSERT_HEAD(&bz->bounce_page_list, bpage, links);
	bz->free_bpages++;
	bz->active_bpages--;
	if ((map = STAILQ_FIRST(&bounce_map_waitinglist)) != NULL) {
		if (reserve_bounce_pages(map->dmat, map, 1) == 0) {
			STAILQ_REMOVE_HEAD(&bounce_map_waitinglist, links);
			STAILQ_INSERT_TAIL(&bounce_map_callbacklist,
			    map, links);
			busdma_swi_pending = 1;
			bz->total_deferred++;
			swi_sched(vm_ih, 0);
		}
	}
	mtx_unlock(&bounce_lock);
}

void
busdma_swi(void)
{
	bus_dma_tag_t dmat;
	struct bus_dmamap *map;

	mtx_lock(&bounce_lock);
	while ((map = STAILQ_FIRST(&bounce_map_callbacklist)) != NULL) {
		STAILQ_REMOVE_HEAD(&bounce_map_callbacklist, links);
		mtx_unlock(&bounce_lock);
		dmat = map->dmat;
		(dmat->common.lockfunc)(dmat->common.lockfuncarg, BUS_DMA_LOCK);
		bus_dmamap_load_mem(map->dmat, map, &map->mem,
		    map->callback, map->callback_arg, BUS_DMA_WAITOK);
		(dmat->common.lockfunc)(dmat->common.lockfuncarg,
		    BUS_DMA_UNLOCK);
		mtx_lock(&bounce_lock);
	}
	mtx_unlock(&bounce_lock);
}

struct bus_dma_impl bus_dma_bounce_impl = {
	.tag_create = bounce_bus_dma_tag_create,
	.tag_destroy = bounce_bus_dma_tag_destroy,
	.map_create = bounce_bus_dmamap_create,
	.map_destroy = bounce_bus_dmamap_destroy,
	.mem_alloc = bounce_bus_dmamem_alloc,
	.mem_free = bounce_bus_dmamem_free,
	.load_phys = bounce_bus_dmamap_load_phys,
	.load_buffer = bounce_bus_dmamap_load_buffer,
	.load_ma = bus_dmamap_load_ma_triv,
	.map_waitok = bounce_bus_dmamap_waitok,
	.map_complete = bounce_bus_dmamap_complete,
	.map_unload = bounce_bus_dmamap_unload,
	.map_sync = bounce_bus_dmamap_sync
};

struct bus_dma_impl bus_dma_nobounce_kernel_impl = {
	.tag_create = bounce_bus_dma_tag_create,
	.tag_destroy = bounce_bus_dma_tag_destroy,
	.map_create = bounce_bus_dmamap_create,
	.map_destroy = bounce_bus_dmamap_destroy,
	.mem_alloc = bounce_bus_dmamem_alloc,
	.mem_free = bounce_bus_dmamem_free,
	.load_phys = bounce_bus_dmamap_load_phys,
	.load_buffer = nobounce_bus_dmamap_load_kernel_buffer,
	.load_ma = bus_dmamap_load_ma_triv,
	.map_waitok = bounce_bus_dmamap_waitok,
	.map_complete = bounce_bus_dmamap_complete,
	.map_unload = bounce_bus_dmamap_unload,
	.map_sync = bounce_bus_dmamap_sync
};
