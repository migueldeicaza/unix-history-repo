/*
 * Copyright (c) 1991 Regents of the University of California.
 * All rights reserved.
 * Copyright (c) 1994 John S. Dyson
 * All rights reserved.
 * Copyright (c) 1994 David Greenman
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and William Jolitz of UUNET Technologies Inc.
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
 *	from:	@(#)pmap.c	7.7 (Berkeley)	5/12/91
 * $FreeBSD$
 */

/*
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#include "opt_pmap.h"
#include "opt_msgbuf.h"
#include "opt_kstack_pages.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/msgbuf.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/user.h>
#include <sys/vmmeter.h>
#include <sys/sysctl.h>
#ifdef SMP
#include <sys/smp.h>
#endif

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include <vm/vm_pageout.h>
#include <vm/vm_pager.h>
#include <vm/uma.h>

#include <machine/cpu.h>
#include <machine/cputypes.h>
#include <machine/md_var.h>
#include <machine/specialreg.h>
#if defined(SMP) || defined(APIC_IO)
#include <machine/smp.h>
#include <machine/apic.h>
#include <machine/segments.h>
#include <machine/tss.h>
#endif /* SMP || APIC_IO */

#define PMAP_KEEP_PDIRS
#ifndef PMAP_SHPGPERPROC
#define PMAP_SHPGPERPROC 200
#endif

#if defined(DIAGNOSTIC)
#define PMAP_DIAGNOSTIC
#endif

#define MINPV 2048

#if !defined(PMAP_DIAGNOSTIC)
#define PMAP_INLINE __inline
#else
#define PMAP_INLINE
#endif

/*
 * Get PDEs and PTEs for user/kernel address space
 */
#define	pmap_pde(m, v)	(&((m)->pm_pdir[(vm_offset_t)(v) >> PDRSHIFT]))
#define pdir_pde(m, v) (m[(vm_offset_t)(v) >> PDRSHIFT])

#define pmap_pde_v(pte)		((*(int *)pte & PG_V) != 0)
#define pmap_pte_w(pte)		((*(int *)pte & PG_W) != 0)
#define pmap_pte_m(pte)		((*(int *)pte & PG_M) != 0)
#define pmap_pte_u(pte)		((*(int *)pte & PG_A) != 0)
#define pmap_pte_v(pte)		((*(int *)pte & PG_V) != 0)

#define pmap_pte_set_w(pte, v) ((v)?(*(int *)pte |= PG_W):(*(int *)pte &= ~PG_W))
#define pmap_pte_set_prot(pte, v) ((*(int *)pte &= ~PG_PROT), (*(int *)pte |= (v)))

/*
 * Given a map and a machine independent protection code,
 * convert to a vax protection code.
 */
#define pte_prot(m, p)	(protection_codes[p])
static int protection_codes[8];

struct pmap kernel_pmap_store;
LIST_HEAD(pmaplist, pmap);
struct pmaplist allpmaps;

vm_offset_t avail_start;	/* PA of first available physical page */
vm_offset_t avail_end;		/* PA of last available physical page */
vm_offset_t virtual_avail;	/* VA of first avail page (after kernel bss) */
vm_offset_t virtual_end;	/* VA of last avail page (end of kernel AS) */
static boolean_t pmap_initialized = FALSE;	/* Has pmap_init completed? */
static int pgeflag;		/* PG_G or-in */
static int pseflag;		/* PG_PS or-in */

static vm_object_t kptobj;

static int nkpt;
vm_offset_t kernel_vm_end;
extern u_int32_t KERNend;

/*
 * Data for the pv entry allocation mechanism
 */
static uma_zone_t pvzone;
static struct vm_object pvzone_obj;
static int pv_entry_count = 0, pv_entry_max = 0, pv_entry_high_water = 0;
static int pmap_pagedaemon_waken = 0;

/*
 * All those kernel PT submaps that BSD is so fond of
 */
pt_entry_t *CMAP1 = 0;
static pt_entry_t *CMAP2, *CMAP3, *ptmmap;
caddr_t CADDR1 = 0, ptvmmap = 0;
static caddr_t CADDR2, CADDR3;
static pt_entry_t *msgbufmap;
struct msgbuf *msgbufp = 0;

/*
 * Crashdump maps.
 */
static pt_entry_t *pt_crashdumpmap;
static caddr_t crashdumpmap;

#ifdef SMP
extern pt_entry_t *SMPpt;
#endif
static pt_entry_t *PMAP1 = 0;
static pt_entry_t *PADDR1 = 0;

static PMAP_INLINE void	free_pv_entry(pv_entry_t pv);
static pt_entry_t *get_ptbase(pmap_t pmap);
static pv_entry_t get_pv_entry(void);
static void	i386_protection_init(void);
static __inline void	pmap_changebit(vm_page_t m, int bit, boolean_t setem);

static void	pmap_remove_all(vm_page_t m);
static vm_page_t pmap_enter_quick(pmap_t pmap, vm_offset_t va,
				      vm_page_t m, vm_page_t mpte);
static int pmap_remove_pte(pmap_t pmap, pt_entry_t *ptq, vm_offset_t sva);
static void pmap_remove_page(struct pmap *pmap, vm_offset_t va);
static int pmap_remove_entry(struct pmap *pmap, vm_page_t m,
					vm_offset_t va);
static boolean_t pmap_testbit(vm_page_t m, int bit);
static void pmap_insert_entry(pmap_t pmap, vm_offset_t va,
		vm_page_t mpte, vm_page_t m);

static vm_page_t pmap_allocpte(pmap_t pmap, vm_offset_t va);

static int pmap_release_free_page(pmap_t pmap, vm_page_t p);
static vm_page_t _pmap_allocpte(pmap_t pmap, unsigned ptepindex);
static pt_entry_t *pmap_pte_quick(pmap_t pmap, vm_offset_t va);
static vm_page_t pmap_page_lookup(vm_object_t object, vm_pindex_t pindex);
static int pmap_unuse_pt(pmap_t, vm_offset_t, vm_page_t);
static vm_offset_t pmap_kmem_choose(vm_offset_t addr);
static void *pmap_allocf(uma_zone_t zone, int bytes, u_int8_t *flags, int wait);

static pd_entry_t pdir4mb;

/*
 *	Routine:	pmap_pte
 *	Function:
 *		Extract the page table entry associated
 *		with the given map/virtual_address pair.
 */

PMAP_INLINE pt_entry_t *
pmap_pte(pmap, va)
	register pmap_t pmap;
	vm_offset_t va;
{
	pd_entry_t *pdeaddr;

	if (pmap) {
		pdeaddr = pmap_pde(pmap, va);
		if (*pdeaddr & PG_PS)
			return pdeaddr;
		if (*pdeaddr) {
			return get_ptbase(pmap) + i386_btop(va);
		}
	}
	return (0);
}

/*
 * Move the kernel virtual free pointer to the next
 * 4MB.  This is used to help improve performance
 * by using a large (4MB) page for much of the kernel
 * (.text, .data, .bss)
 */
static vm_offset_t
pmap_kmem_choose(vm_offset_t addr)
{
	vm_offset_t newaddr = addr;

#ifndef DISABLE_PSE
	if (cpu_feature & CPUID_PSE)
		newaddr = (addr + (NBPDR - 1)) & ~(NBPDR - 1);
#endif
	return newaddr;
}

/*
 *	Bootstrap the system enough to run with virtual memory.
 *
 *	On the i386 this is called after mapping has already been enabled
 *	and just syncs the pmap module with what has already been done.
 *	[We can't call it easily with mapping off since the kernel is not
 *	mapped with PA == VA, hence we would have to relocate every address
 *	from the linked base (virtual) address "KERNBASE" to the actual
 *	(physical) address starting relative to 0]
 */
void
pmap_bootstrap(firstaddr, loadaddr)
	vm_offset_t firstaddr;
	vm_offset_t loadaddr;
{
	vm_offset_t va;
	pt_entry_t *pte;
	int i;

	avail_start = firstaddr;

	/*
	 * XXX The calculation of virtual_avail is wrong. It's NKPT*PAGE_SIZE too
	 * large. It should instead be correctly calculated in locore.s and
	 * not based on 'first' (which is a physical address, not a virtual
	 * address, for the start of unused physical memory). The kernel
	 * page tables are NOT double mapped and thus should not be included
	 * in this calculation.
	 */
	virtual_avail = (vm_offset_t) KERNBASE + firstaddr;
	virtual_avail = pmap_kmem_choose(virtual_avail);

	virtual_end = VM_MAX_KERNEL_ADDRESS;

	/*
	 * Initialize protection array.
	 */
	i386_protection_init();

	/*
	 * Initialize the kernel pmap (which is statically allocated).
	 */
	kernel_pmap->pm_pdir = (pd_entry_t *) (KERNBASE + (u_int)IdlePTD);
	kernel_pmap->pm_active = -1;	/* don't allow deactivation */
	TAILQ_INIT(&kernel_pmap->pm_pvlist);
	LIST_INIT(&allpmaps);
	LIST_INSERT_HEAD(&allpmaps, kernel_pmap, pm_list);
	nkpt = NKPT;

	/*
	 * Reserve some special page table entries/VA space for temporary
	 * mapping of pages.
	 */
#define	SYSMAP(c, p, v, n)	\
	v = (c)va; va += ((n)*PAGE_SIZE); p = pte; pte += (n);

	va = virtual_avail;
	pte = (pt_entry_t *) pmap_pte(kernel_pmap, va);

	/*
	 * CMAP1/CMAP2 are used for zeroing and copying pages.
	 * CMAP3 is used for the idle process page zeroing.
	 */
	SYSMAP(caddr_t, CMAP1, CADDR1, 1)
	SYSMAP(caddr_t, CMAP2, CADDR2, 1)
	SYSMAP(caddr_t, CMAP3, CADDR3, 1)

	/*
	 * Crashdump maps.
	 */
	SYSMAP(caddr_t, pt_crashdumpmap, crashdumpmap, MAXDUMPPGS);

	/*
	 * ptvmmap is used for reading arbitrary physical pages via /dev/mem.
	 * XXX ptmmap is not used.
	 */
	SYSMAP(caddr_t, ptmmap, ptvmmap, 1)

	/*
	 * msgbufp is used to map the system message buffer.
	 * XXX msgbufmap is not used.
	 */
	SYSMAP(struct msgbuf *, msgbufmap, msgbufp,
	       atop(round_page(MSGBUF_SIZE)))

	/*
	 * ptemap is used for pmap_pte_quick
	 */
	SYSMAP(pt_entry_t *, PMAP1, PADDR1, 1);

	virtual_avail = va;

	*CMAP1 = *CMAP2 = 0;
	for (i = 0; i < NKPT; i++)
		PTD[i] = 0;

	pgeflag = 0;
#ifndef DISABLE_PG_G
	if (cpu_feature & CPUID_PGE)
		pgeflag = PG_G;
#endif
	
/*
 * Initialize the 4MB page size flag
 */
	pseflag = 0;
/*
 * The 4MB page version of the initial
 * kernel page mapping.
 */
	pdir4mb = 0;

#ifndef DISABLE_PSE
	if (cpu_feature & CPUID_PSE) {
		pd_entry_t ptditmp;
		/*
		 * Note that we have enabled PSE mode
		 */
		pseflag = PG_PS;
		ptditmp = *(PTmap + i386_btop(KERNBASE));
		ptditmp &= ~(NBPDR - 1);
		ptditmp |= PG_V | PG_RW | PG_PS | PG_U | pgeflag;
		pdir4mb = ptditmp;
	}
#endif
#ifndef SMP
	/*
	 * Turn on PGE/PSE.  SMP does this later on since the
	 * 4K page tables are required for AP boot (for now).
	 * XXX fixme.
	 */
	pmap_set_opt();
#endif
#ifdef SMP
	if (cpu_apic_address == 0)
		panic("pmap_bootstrap: no local apic! (non-SMP hardware?)");

	/* local apic is mapped on last page */
	SMPpt[NPTEPG - 1] = (pt_entry_t)(PG_V | PG_RW | PG_N | pgeflag |
	    (cpu_apic_address & PG_FRAME));
#endif
	invltlb();
}

/*
 * Enable 4MB page mode for MP startup.  Turn on PG_G support.
 * BSP will run this after all the AP's have started up.
 */
void
pmap_set_opt(void)
{
	pt_entry_t *pte;
	vm_offset_t va, endva;

	if (pgeflag && (cpu_feature & CPUID_PGE)) {
		load_cr4(rcr4() | CR4_PGE);
		invltlb();		/* Insurance */
	}
#ifndef DISABLE_PSE
	if (pseflag && (cpu_feature & CPUID_PSE)) {
		load_cr4(rcr4() | CR4_PSE);
		invltlb();		/* Insurance */
	}
#endif
	if (PCPU_GET(cpuid) == 0) {
#ifndef DISABLE_PSE
		if (pdir4mb) {
			kernel_pmap->pm_pdir[KPTDI] = PTD[KPTDI] = pdir4mb;
			invltlb();	/* Insurance */
		}
#endif
		if (pgeflag) {
			/* Turn on PG_G for text, data, bss pages. */
			va = (vm_offset_t)btext;
#ifndef DISABLE_PSE
			if (pseflag && (cpu_feature & CPUID_PSE)) {
				if (va < KERNBASE + (1 << PDRSHIFT))
					va = KERNBASE + (1 << PDRSHIFT);
			}
#endif
			endva = KERNBASE + KERNend;
			while (va < endva) {
				pte = vtopte(va);
				if (*pte)
					*pte |= pgeflag;
				va += PAGE_SIZE;
			}
			invltlb();	/* Insurance */
		}
		/*
		 * We do not need to broadcast the invltlb here, because
		 * each AP does it the moment it is released from the boot
		 * lock.  See ap_init().
		 */
	}
}

void *
pmap_allocf(uma_zone_t zone, int bytes, u_int8_t *flags, int wait)
{
	*flags = UMA_SLAB_PRIV;
	return (void *)kmem_alloc(kernel_map, bytes);
}

/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 *	pmap_init has been enhanced to support in a fairly consistant
 *	way, discontiguous physical memory.
 */
void
pmap_init(phys_start, phys_end)
	vm_offset_t phys_start, phys_end;
{
	int i;
	int initial_pvs;

	/*
	 * object for kernel page table pages
	 */
	kptobj = vm_object_allocate(OBJT_DEFAULT, NKPDE);

	/*
	 * Allocate memory for random pmap data structures.  Includes the
	 * pv_head_table.
	 */

	for(i = 0; i < vm_page_array_size; i++) {
		vm_page_t m;

		m = &vm_page_array[i];
		TAILQ_INIT(&m->md.pv_list);
		m->md.pv_list_count = 0;
	}

	/*
	 * init the pv free list
	 */
	initial_pvs = vm_page_array_size;
	if (initial_pvs < MINPV)
		initial_pvs = MINPV;
	pvzone = uma_zcreate("PV ENTRY", sizeof (struct pv_entry), NULL, NULL, 
	    NULL, NULL, UMA_ALIGN_PTR, UMA_ZONE_VM);
	uma_zone_set_allocf(pvzone, pmap_allocf);
	uma_prealloc(pvzone, initial_pvs);

	/*
	 * Now it is safe to enable pv_table recording.
	 */
	pmap_initialized = TRUE;
}

/*
 * Initialize the address space (zone) for the pv_entries.  Set a
 * high water mark so that the system can recover from excessive
 * numbers of pv entries.
 */
void
pmap_init2()
{
	int shpgperproc = PMAP_SHPGPERPROC;

	TUNABLE_INT_FETCH("vm.pmap.shpgperproc", &shpgperproc);
	pv_entry_max = shpgperproc * maxproc + vm_page_array_size;
	TUNABLE_INT_FETCH("vm.pmap.pv_entries", &pv_entry_max);
	pv_entry_high_water = 9 * (pv_entry_max / 10);
	uma_zone_set_obj(pvzone, &pvzone_obj, pv_entry_max);
}


/***************************************************
 * Low level helper routines.....
 ***************************************************/

#if defined(PMAP_DIAGNOSTIC)

/*
 * This code checks for non-writeable/modified pages.
 * This should be an invalid condition.
 */
static int
pmap_nw_modified(pt_entry_t ptea)
{
	int pte;

	pte = (int) ptea;

	if ((pte & (PG_M|PG_RW)) == PG_M)
		return 1;
	else
		return 0;
}
#endif


/*
 * this routine defines the region(s) of memory that should
 * not be tested for the modified bit.
 */
static PMAP_INLINE int
pmap_track_modified(vm_offset_t va)
{
	if ((va < kmi.clean_sva) || (va >= kmi.clean_eva)) 
		return 1;
	else
		return 0;
}

#ifdef I386_CPU
/*
 * i386 only has "invalidate everything" and no SMP to worry about.
 */
PMAP_INLINE void
pmap_invalidate_page(pmap_t pmap, vm_offset_t va)
{

	if (pmap == kernel_pmap || pmap->pm_active)
		invltlb();
}

PMAP_INLINE void
pmap_invalidate_range(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{

	if (pmap == kernel_pmap || pmap->pm_active)
		invltlb();
}

PMAP_INLINE void
pmap_invalidate_all(pmap_t pmap)
{

	if (pmap == kernel_pmap || pmap->pm_active)
		invltlb();
}
#else /* !I386_CPU */
#ifdef SMP
/*
 * For SMP, these functions have to use the IPI mechanism for coherence.
 */
void
pmap_invalidate_page(pmap_t pmap, vm_offset_t va)
{
	u_int cpumask;
	u_int other_cpus;

	critical_enter();
	/*
	 * We need to disable interrupt preemption but MUST NOT have
	 * interrupts disabled here.
	 * XXX we may need to hold schedlock to get a coherent pm_active
	 */
	if (pmap->pm_active == -1 || pmap->pm_active == all_cpus) {
		invlpg(va);
		smp_invlpg(va);
	} else {
		cpumask = PCPU_GET(cpumask);
		other_cpus = PCPU_GET(other_cpus);
		if (pmap->pm_active & cpumask)
			invlpg(va);
		if (pmap->pm_active & other_cpus)
			smp_masked_invlpg(pmap->pm_active & other_cpus, va);
	}
	critical_exit();
}

void
pmap_invalidate_range(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	u_int cpumask;
	u_int other_cpus;
	vm_offset_t addr;

	critical_enter();
	/*
	 * We need to disable interrupt preemption but MUST NOT have
	 * interrupts disabled here.
	 * XXX we may need to hold schedlock to get a coherent pm_active
	 */
	if (pmap->pm_active == -1 || pmap->pm_active == all_cpus) {
		for (addr = sva; addr < eva; addr += PAGE_SIZE)
			invlpg(addr);
		smp_invlpg_range(sva, eva);
	} else {
		cpumask = PCPU_GET(cpumask);
		other_cpus = PCPU_GET(other_cpus);
		if (pmap->pm_active & cpumask)
			for (addr = sva; addr < eva; addr += PAGE_SIZE)
				invlpg(addr);
		if (pmap->pm_active & other_cpus)
			smp_masked_invlpg_range(pmap->pm_active & other_cpus,
			    sva, eva);
	}
	critical_exit();
}

void
pmap_invalidate_all(pmap_t pmap)
{
	u_int cpumask;
	u_int other_cpus;

#ifdef SWTCH_OPTIM_STATS
	tlb_flush_count++;
#endif
	critical_enter();
	/*
	 * We need to disable interrupt preemption but MUST NOT have
	 * interrupts disabled here.
	 * XXX we may need to hold schedlock to get a coherent pm_active
	 */
	if (pmap->pm_active == -1 || pmap->pm_active == all_cpus) {
		invltlb();
		smp_invltlb();
	} else {
		cpumask = PCPU_GET(cpumask);
		other_cpus = PCPU_GET(other_cpus);
		if (pmap->pm_active & cpumask)
			invltlb();
		if (pmap->pm_active & other_cpus)
			smp_masked_invltlb(pmap->pm_active & other_cpus);
	}
	critical_exit();
}
#else /* !SMP */
/*
 * Normal, non-SMP, 486+ invalidation functions.
 * We inline these within pmap.c for speed.
 */
PMAP_INLINE void
pmap_invalidate_page(pmap_t pmap, vm_offset_t va)
{

	if (pmap == kernel_pmap || pmap->pm_active)
		invlpg(va);
}

PMAP_INLINE void
pmap_invalidate_range(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	vm_offset_t addr;

	if (pmap == kernel_pmap || pmap->pm_active)
		for (addr = sva; addr < eva; addr += PAGE_SIZE)
			invlpg(addr);
}

PMAP_INLINE void
pmap_invalidate_all(pmap_t pmap)
{

	if (pmap == kernel_pmap || pmap->pm_active)
		invltlb();
}
#endif /* !SMP */
#endif /* !I386_CPU */

/*
 * Return an address which is the base of the Virtual mapping of
 * all the PTEs for the given pmap. Note this doesn't say that
 * all the PTEs will be present or that the pages there are valid.
 * The PTEs are made available by the recursive mapping trick.
 * It will map in the alternate PTE space if needed.
 */
static pt_entry_t *
get_ptbase(pmap)
	pmap_t pmap;
{
	pd_entry_t frame;

	/* are we current address space or kernel? */
	if (pmap == kernel_pmap)
		return PTmap;
	frame = pmap->pm_pdir[PTDPTDI] & PG_FRAME;
	if (frame == (PTDpde & PG_FRAME))
		return PTmap;
	/* otherwise, we are alternate address space */
	if (frame != (APTDpde & PG_FRAME)) {
		APTDpde = (pd_entry_t) (frame | PG_RW | PG_V);
		pmap_invalidate_all(kernel_pmap);	/* XXX Bandaid */
	}
	return APTmap;
}

/*
 * Super fast pmap_pte routine best used when scanning
 * the pv lists.  This eliminates many coarse-grained
 * invltlb calls.  Note that many of the pv list
 * scans are across different pmaps.  It is very wasteful
 * to do an entire invltlb for checking a single mapping.
 */

static pt_entry_t * 
pmap_pte_quick(pmap, va)
	register pmap_t pmap;
	vm_offset_t va;
{
	pd_entry_t pde, newpf;
	pde = pmap->pm_pdir[va >> PDRSHIFT];
	if (pde != 0) {
		pd_entry_t frame = pmap->pm_pdir[PTDPTDI] & PG_FRAME;
		unsigned index = i386_btop(va);
		/* are we current address space or kernel? */
		if (pmap == kernel_pmap || frame == (PTDpde & PG_FRAME))
			return PTmap + index;
		newpf = pde & PG_FRAME;
		if (((*PMAP1) & PG_FRAME) != newpf) {
			*PMAP1 = newpf | PG_RW | PG_V;
			pmap_invalidate_page(kernel_pmap, (vm_offset_t)PADDR1);
		}
		return PADDR1 + (index & (NPTEPG - 1));
	}
	return (0);
}

/*
 *	Routine:	pmap_extract
 *	Function:
 *		Extract the physical page address associated
 *		with the given map/virtual_address pair.
 */
vm_offset_t 
pmap_extract(pmap, va)
	register pmap_t pmap;
	vm_offset_t va;
{
	vm_offset_t rtval;	/* XXX FIXME */
	vm_offset_t pdirindex;

	if (pmap == 0)
		return 0;
	pdirindex = va >> PDRSHIFT;
	rtval = pmap->pm_pdir[pdirindex];
	if (rtval != 0) {
		pt_entry_t *pte;
		if ((rtval & PG_PS) != 0) {
			rtval &= ~(NBPDR - 1);
			rtval |= va & (NBPDR - 1);
			return rtval;
		}
		pte = get_ptbase(pmap) + i386_btop(va);
		rtval = ((*pte & PG_FRAME) | (va & PAGE_MASK));
		return rtval;
	}
	return 0;

}

/***************************************************
 * Low level mapping routines.....
 ***************************************************/

/*
 * Add a wired page to the kva.
 * Note: not SMP coherent.
 */
PMAP_INLINE void 
pmap_kenter(vm_offset_t va, vm_offset_t pa)
{
	pt_entry_t *pte;

	pte = vtopte(va);
	*pte = pa | PG_RW | PG_V | pgeflag;
}

/*
 * Remove a page from the kernel pagetables.
 * Note: not SMP coherent.
 */
PMAP_INLINE void
pmap_kremove(vm_offset_t va)
{
	pt_entry_t *pte;

	pte = vtopte(va);
	*pte = 0;
}

/*
 *	Used to map a range of physical addresses into kernel
 *	virtual address space.
 *
 *	The value passed in '*virt' is a suggested virtual address for
 *	the mapping. Architectures which can support a direct-mapped
 *	physical to virtual region can return the appropriate address
 *	within that region, leaving '*virt' unchanged. Other
 *	architectures should map the pages starting at '*virt' and
 *	update '*virt' with the first usable address after the mapped
 *	region.
 */
vm_offset_t
pmap_map(vm_offset_t *virt, vm_offset_t start, vm_offset_t end, int prot)
{
	vm_offset_t va, sva;

	va = sva = *virt;
	while (start < end) {
		pmap_kenter(va, start);
		va += PAGE_SIZE;
		start += PAGE_SIZE;
	}
	pmap_invalidate_range(kernel_pmap, sva, va);
	*virt = va;
	return (sva);
}


/*
 * Add a list of wired pages to the kva
 * this routine is only used for temporary
 * kernel mappings that do not need to have
 * page modification or references recorded.
 * Note that old mappings are simply written
 * over.  The page *must* be wired.
 * Note: SMP coherent.  Uses a ranged shootdown IPI.
 */
void
pmap_qenter(vm_offset_t sva, vm_page_t *m, int count)
{
	vm_offset_t va;

	va = sva;
	while (count-- > 0) {
		pmap_kenter(va, VM_PAGE_TO_PHYS(*m));
		va += PAGE_SIZE;
		m++;
	}
	pmap_invalidate_range(kernel_pmap, sva, va);
}

/*
 * This routine tears out page mappings from the
 * kernel -- it is meant only for temporary mappings.
 * Note: SMP coherent.  Uses a ranged shootdown IPI.
 */
void
pmap_qremove(vm_offset_t sva, int count)
{
	vm_offset_t va;

	va = sva;
	while (count-- > 0) {
		pmap_kremove(va);
		va += PAGE_SIZE;
	}
	pmap_invalidate_range(kernel_pmap, sva, va);
}

static vm_page_t
pmap_page_lookup(vm_object_t object, vm_pindex_t pindex)
{
	vm_page_t m;

retry:
	m = vm_page_lookup(object, pindex);
	if (m && vm_page_sleep_busy(m, FALSE, "pplookp"))
		goto retry;
	return m;
}

/*
 * Create the kernel stack (including pcb for i386) for a new thread.
 * This routine directly affects the fork perf for a process and
 * create performance for a thread.
 */
void
pmap_new_thread(struct thread *td)
{
	int i;
	vm_page_t ma[KSTACK_PAGES];
	vm_object_t ksobj;
	vm_page_t m;
	vm_offset_t ks;

	/*
	 * allocate object for the kstack
	 */
	ksobj = vm_object_allocate(OBJT_DEFAULT, KSTACK_PAGES);
	td->td_kstack_obj = ksobj;

	/* get a kernel virtual address for the kstack for this thread */
#ifdef KSTACK_GUARD
	ks = kmem_alloc_nofault(kernel_map, (KSTACK_PAGES + 1) * PAGE_SIZE);
	if (ks == 0)
		panic("pmap_new_thread: kstack allocation failed");
	if (*vtopte(ks) != 0)
		pmap_qremove(ks, 1);
	ks += PAGE_SIZE;
	td->td_kstack = ks;
#else
	/* get a kernel virtual address for the kstack for this thread */
	ks = kmem_alloc_nofault(kernel_map, KSTACK_PAGES * PAGE_SIZE);
	if (ks == 0)
		panic("pmap_new_thread: kstack allocation failed");
	td->td_kstack = ks;
#endif
	/* 
	 * For the length of the stack, link in a real page of ram for each
	 * page of stack.
	 */
	for (i = 0; i < KSTACK_PAGES; i++) {
		/*
		 * Get a kernel stack page
		 */
		m = vm_page_grab(ksobj, i,
		    VM_ALLOC_NORMAL | VM_ALLOC_RETRY | VM_ALLOC_WIRED);
		ma[i] = m;

		vm_page_wakeup(m);
		vm_page_flag_clear(m, PG_ZERO);
		m->valid = VM_PAGE_BITS_ALL;
	}
	pmap_qenter(ks, ma, KSTACK_PAGES);
}

/*
 * Dispose the kernel stack for a thread that has exited.
 * This routine directly impacts the exit perf of a process and thread.
 */
void
pmap_dispose_thread(td)
	struct thread *td;
{
	int i;
	vm_object_t ksobj;
	vm_offset_t ks;
	vm_page_t m;

	ksobj = td->td_kstack_obj;
	ks = td->td_kstack;
	pmap_qremove(ks, KSTACK_PAGES);
	for (i = 0; i < KSTACK_PAGES; i++) {
		m = vm_page_lookup(ksobj, i);
		if (m == NULL)
			panic("pmap_dispose_thread: kstack already missing?");
		vm_page_lock_queues();
		vm_page_busy(m);
		vm_page_unwire(m, 0);
		vm_page_free(m);
		vm_page_unlock_queues();
	}
	/*
	 * Free the space that this stack was mapped to in the kernel
	 * address map.
	 */
#ifdef KSTACK_GUARD
	kmem_free(kernel_map, ks - PAGE_SIZE, (KSTACK_PAGES + 1) * PAGE_SIZE);
#else
	kmem_free(kernel_map, ks, KSTACK_PAGES * PAGE_SIZE);
#endif
	vm_object_deallocate(ksobj);
}

/*
 * Allow the Kernel stack for a thread to be prejudicially paged out.
 */
void
pmap_swapout_thread(td)
	struct thread *td;
{
	int i;
	vm_object_t ksobj;
	vm_offset_t ks;
	vm_page_t m;

	ksobj = td->td_kstack_obj;
	ks = td->td_kstack;
	pmap_qremove(ks, KSTACK_PAGES);
	for (i = 0; i < KSTACK_PAGES; i++) {
		m = vm_page_lookup(ksobj, i);
		if (m == NULL)
			panic("pmap_swapout_thread: kstack already missing?");
		vm_page_lock_queues();
		vm_page_dirty(m);
		vm_page_unwire(m, 0);
		vm_page_unlock_queues();
	}
}

/*
 * Bring the kernel stack for a specified thread back in.
 */
void
pmap_swapin_thread(td)
	struct thread *td;
{
	int i, rv;
	vm_page_t ma[KSTACK_PAGES];
	vm_object_t ksobj;
	vm_offset_t ks;
	vm_page_t m;

	ksobj = td->td_kstack_obj;
	ks = td->td_kstack;
	for (i = 0; i < KSTACK_PAGES; i++) {
		m = vm_page_grab(ksobj, i, VM_ALLOC_NORMAL | VM_ALLOC_RETRY);
		if (m->valid != VM_PAGE_BITS_ALL) {
			rv = vm_pager_get_pages(ksobj, &m, 1, 0);
			if (rv != VM_PAGER_OK)
				panic("pmap_swapin_thread: cannot get kstack for proc: %d\n", td->td_proc->p_pid);
			m = vm_page_lookup(ksobj, i);
			m->valid = VM_PAGE_BITS_ALL;
		}
		ma[i] = m;
		vm_page_lock_queues();
		vm_page_wire(m);
		vm_page_wakeup(m);
		vm_page_unlock_queues();
	}
	pmap_qenter(ks, ma, KSTACK_PAGES);
}

/***************************************************
 * Page table page management routines.....
 ***************************************************/

/*
 * This routine unholds page table pages, and if the hold count
 * drops to zero, then it decrements the wire count.
 */
static int 
_pmap_unwire_pte_hold(pmap_t pmap, vm_page_t m)
{

	while (vm_page_sleep_busy(m, FALSE, "pmuwpt"))
		;

	if (m->hold_count == 0) {
		vm_offset_t pteva;
		/*
		 * unmap the page table page
		 */
		pmap->pm_pdir[m->pindex] = 0;
		--pmap->pm_stats.resident_count;
		if ((pmap->pm_pdir[PTDPTDI] & PG_FRAME) ==
		    (PTDpde & PG_FRAME)) {
			/*
			 * Do a invltlb to make the invalidated mapping
			 * take effect immediately.
			 */
			pteva = VM_MAXUSER_ADDRESS + i386_ptob(m->pindex);
			pmap_invalidate_page(pmap, pteva);
		}

		if (pmap->pm_ptphint == m)
			pmap->pm_ptphint = NULL;

		/*
		 * If the page is finally unwired, simply free it.
		 */
		--m->wire_count;
		if (m->wire_count == 0) {

			vm_page_flash(m);
			vm_page_busy(m);
			vm_page_free_zero(m);
			--cnt.v_wire_count;
		}
		return 1;
	}
	return 0;
}

static PMAP_INLINE int
pmap_unwire_pte_hold(pmap_t pmap, vm_page_t m)
{
	vm_page_unhold(m);
	if (m->hold_count == 0)
		return _pmap_unwire_pte_hold(pmap, m);
	else
		return 0;
}

/*
 * After removing a page table entry, this routine is used to
 * conditionally free the page, and manage the hold/wire counts.
 */
static int
pmap_unuse_pt(pmap_t pmap, vm_offset_t va, vm_page_t mpte)
{
	unsigned ptepindex;
	if (va >= VM_MAXUSER_ADDRESS)
		return 0;

	if (mpte == NULL) {
		ptepindex = (va >> PDRSHIFT);
		if (pmap->pm_ptphint &&
			(pmap->pm_ptphint->pindex == ptepindex)) {
			mpte = pmap->pm_ptphint;
		} else {
			mpte = pmap_page_lookup(pmap->pm_pteobj, ptepindex);
			pmap->pm_ptphint = mpte;
		}
	}

	return pmap_unwire_pte_hold(pmap, mpte);
}

void
pmap_pinit0(pmap)
	struct pmap *pmap;
{
	pmap->pm_pdir =
		(pd_entry_t *)kmem_alloc_pageable(kernel_map, PAGE_SIZE);
	pmap_kenter((vm_offset_t)pmap->pm_pdir, (vm_offset_t)IdlePTD);
#ifndef I386_CPU
	invlpg((vm_offset_t)pmap->pm_pdir);
#else
	invltlb();
#endif
	pmap->pm_ptphint = NULL;
	pmap->pm_active = 0;
	TAILQ_INIT(&pmap->pm_pvlist);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);
	LIST_INSERT_HEAD(&allpmaps, pmap, pm_list);
}

/*
 * Initialize a preallocated and zeroed pmap structure,
 * such as one in a vmspace structure.
 */
void
pmap_pinit(pmap)
	register struct pmap *pmap;
{
	vm_page_t ptdpg;

	/*
	 * No need to allocate page table space yet but we do need a valid
	 * page directory table.
	 */
	if (pmap->pm_pdir == NULL)
		pmap->pm_pdir =
			(pd_entry_t *)kmem_alloc_pageable(kernel_map, PAGE_SIZE);

	/*
	 * allocate object for the ptes
	 */
	if (pmap->pm_pteobj == NULL)
		pmap->pm_pteobj = vm_object_allocate(OBJT_DEFAULT, PTDPTDI + 1);

	/*
	 * allocate the page directory page
	 */
	ptdpg = vm_page_grab(pmap->pm_pteobj, PTDPTDI,
			VM_ALLOC_NORMAL | VM_ALLOC_RETRY | VM_ALLOC_WIRED);
	vm_page_flag_clear(ptdpg, PG_MAPPED | PG_BUSY); /* not usually mapped*/
	ptdpg->valid = VM_PAGE_BITS_ALL;

	pmap_qenter((vm_offset_t) pmap->pm_pdir, &ptdpg, 1);
	if ((ptdpg->flags & PG_ZERO) == 0)
		bzero(pmap->pm_pdir, PAGE_SIZE);

	LIST_INSERT_HEAD(&allpmaps, pmap, pm_list);
	/* Wire in kernel global address entries. */
	/* XXX copies current process, does not fill in MPPTDI */
	bcopy(PTD + KPTDI, pmap->pm_pdir + KPTDI, nkpt * PTESIZE);
#ifdef SMP
	pmap->pm_pdir[MPPTDI] = PTD[MPPTDI];
#endif

	/* install self-referential address mapping entry */
	pmap->pm_pdir[PTDPTDI] =
		VM_PAGE_TO_PHYS(ptdpg) | PG_V | PG_RW | PG_A | PG_M;

	pmap->pm_active = 0;
	pmap->pm_ptphint = NULL;
	TAILQ_INIT(&pmap->pm_pvlist);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);
}

/*
 * Wire in kernel global address entries.  To avoid a race condition
 * between pmap initialization and pmap_growkernel, this procedure
 * should be called after the vmspace is attached to the process
 * but before this pmap is activated.
 */
void
pmap_pinit2(pmap)
	struct pmap *pmap;
{
	/* XXX: Remove this stub when no longer called */
}

static int
pmap_release_free_page(pmap_t pmap, vm_page_t p)
{
	pd_entry_t *pde = pmap->pm_pdir;
	/*
	 * This code optimizes the case of freeing non-busy
	 * page-table pages.  Those pages are zero now, and
	 * might as well be placed directly into the zero queue.
	 */
	if (vm_page_sleep_busy(p, FALSE, "pmaprl"))
		return 0;

	vm_page_lock_queues();
	vm_page_busy(p);

	/*
	 * Remove the page table page from the processes address space.
	 */
	pde[p->pindex] = 0;
	pmap->pm_stats.resident_count--;

	if (p->hold_count)  {
		panic("pmap_release: freeing held page table page");
	}
	/*
	 * Page directory pages need to have the kernel
	 * stuff cleared, so they can go into the zero queue also.
	 */
	if (p->pindex == PTDPTDI) {
		bzero(pde + KPTDI, nkpt * PTESIZE);
#ifdef SMP
		pde[MPPTDI] = 0;
#endif
		pde[APTDPTDI] = 0;
		pmap_kremove((vm_offset_t) pmap->pm_pdir);
	}

	if (pmap->pm_ptphint && (pmap->pm_ptphint->pindex == p->pindex))
		pmap->pm_ptphint = NULL;

	p->wire_count--;
	cnt.v_wire_count--;
	vm_page_free_zero(p);
	vm_page_unlock_queues();
	return 1;
}

/*
 * this routine is called if the page table page is not
 * mapped correctly.
 */
static vm_page_t
_pmap_allocpte(pmap, ptepindex)
	pmap_t	pmap;
	unsigned ptepindex;
{
	vm_offset_t pteva, ptepa;	/* XXXPA */
	vm_page_t m;

	/*
	 * Find or fabricate a new pagetable page
	 */
	m = vm_page_grab(pmap->pm_pteobj, ptepindex,
			VM_ALLOC_ZERO | VM_ALLOC_RETRY);

	KASSERT(m->queue == PQ_NONE,
		("_pmap_allocpte: %p->queue != PQ_NONE", m));

	if (m->wire_count == 0)
		cnt.v_wire_count++;
	m->wire_count++;

	/*
	 * Increment the hold count for the page table page
	 * (denoting a new mapping.)
	 */
	m->hold_count++;

	/*
	 * Map the pagetable page into the process address space, if
	 * it isn't already there.
	 */

	pmap->pm_stats.resident_count++;

	ptepa = VM_PAGE_TO_PHYS(m);
	pmap->pm_pdir[ptepindex] =
		(pd_entry_t) (ptepa | PG_U | PG_RW | PG_V | PG_A | PG_M);

	/*
	 * Set the page table hint
	 */
	pmap->pm_ptphint = m;

	/*
	 * Try to use the new mapping, but if we cannot, then
	 * do it with the routine that maps the page explicitly.
	 */
	if ((m->flags & PG_ZERO) == 0) {
		if ((pmap->pm_pdir[PTDPTDI] & PG_FRAME) ==
		    (PTDpde & PG_FRAME)) {
			pteva = VM_MAXUSER_ADDRESS + i386_ptob(ptepindex);
			bzero((caddr_t) pteva, PAGE_SIZE);
		} else {
			pmap_zero_page(m);
		}
	}

	m->valid = VM_PAGE_BITS_ALL;
	vm_page_flag_clear(m, PG_ZERO);
	vm_page_wakeup(m);

	return m;
}

static vm_page_t
pmap_allocpte(pmap_t pmap, vm_offset_t va)
{
	unsigned ptepindex;
	pd_entry_t ptepa;
	vm_page_t m;

	/*
	 * Calculate pagetable page index
	 */
	ptepindex = va >> PDRSHIFT;

	/*
	 * Get the page directory entry
	 */
	ptepa = (vm_offset_t) pmap->pm_pdir[ptepindex];

	/*
	 * This supports switching from a 4MB page to a
	 * normal 4K page.
	 */
	if (ptepa & PG_PS) {
		pmap->pm_pdir[ptepindex] = 0;
		ptepa = 0;
		pmap_invalidate_all(kernel_pmap);
	}

	/*
	 * If the page table page is mapped, we just increment the
	 * hold count, and activate it.
	 */
	if (ptepa) {
		/*
		 * In order to get the page table page, try the
		 * hint first.
		 */
		if (pmap->pm_ptphint &&
			(pmap->pm_ptphint->pindex == ptepindex)) {
			m = pmap->pm_ptphint;
		} else {
			m = pmap_page_lookup(pmap->pm_pteobj, ptepindex);
			pmap->pm_ptphint = m;
		}
		m->hold_count++;
		return m;
	}
	/*
	 * Here if the pte page isn't mapped, or if it has been deallocated.
	 */
	return _pmap_allocpte(pmap, ptepindex);
}


/***************************************************
* Pmap allocation/deallocation routines.
 ***************************************************/

/*
 * Release any resources held by the given physical map.
 * Called when a pmap initialized by pmap_pinit is being released.
 * Should only be called if the map contains no valid mappings.
 */
void
pmap_release(pmap_t pmap)
{
	vm_page_t p,n,ptdpg;
	vm_object_t object = pmap->pm_pteobj;
	int curgeneration;

#if defined(DIAGNOSTIC)
	if (object->ref_count != 1)
		panic("pmap_release: pteobj reference count != 1");
#endif
	
	ptdpg = NULL;
	LIST_REMOVE(pmap, pm_list);
retry:
	curgeneration = object->generation;
	for (p = TAILQ_FIRST(&object->memq); p != NULL; p = n) {
		n = TAILQ_NEXT(p, listq);
		if (p->pindex == PTDPTDI) {
			ptdpg = p;
			continue;
		}
		while (1) {
			if (!pmap_release_free_page(pmap, p) &&
				(object->generation != curgeneration))
				goto retry;
		}
	}

	if (ptdpg && !pmap_release_free_page(pmap, ptdpg))
		goto retry;
}

static int
kvm_size(SYSCTL_HANDLER_ARGS)
{
	unsigned long ksize = VM_MAX_KERNEL_ADDRESS - KERNBASE;

	return sysctl_handle_long(oidp, &ksize, 0, req);
}
SYSCTL_PROC(_vm, OID_AUTO, kvm_size, CTLTYPE_LONG|CTLFLAG_RD, 
    0, 0, kvm_size, "IU", "Size of KVM");

static int
kvm_free(SYSCTL_HANDLER_ARGS)
{
	unsigned long kfree = VM_MAX_KERNEL_ADDRESS - kernel_vm_end;

	return sysctl_handle_long(oidp, &kfree, 0, req);
}
SYSCTL_PROC(_vm, OID_AUTO, kvm_free, CTLTYPE_LONG|CTLFLAG_RD, 
    0, 0, kvm_free, "IU", "Amount of KVM free");

/*
 * grow the number of kernel page table entries, if needed
 */
void
pmap_growkernel(vm_offset_t addr)
{
	struct pmap *pmap;
	int s;
	vm_offset_t ptppaddr;
	vm_page_t nkpg;
	pd_entry_t newpdir;

	s = splhigh();
	if (kernel_vm_end == 0) {
		kernel_vm_end = KERNBASE;
		nkpt = 0;
		while (pdir_pde(PTD, kernel_vm_end)) {
			kernel_vm_end = (kernel_vm_end + PAGE_SIZE * NPTEPG) & ~(PAGE_SIZE * NPTEPG - 1);
			nkpt++;
		}
	}
	addr = (addr + PAGE_SIZE * NPTEPG) & ~(PAGE_SIZE * NPTEPG - 1);
	while (kernel_vm_end < addr) {
		if (pdir_pde(PTD, kernel_vm_end)) {
			kernel_vm_end = (kernel_vm_end + PAGE_SIZE * NPTEPG) & ~(PAGE_SIZE * NPTEPG - 1);
			continue;
		}

		/*
		 * This index is bogus, but out of the way
		 */
		nkpg = vm_page_alloc(kptobj, nkpt,
				     VM_ALLOC_SYSTEM | VM_ALLOC_WIRED);
		if (!nkpg)
			panic("pmap_growkernel: no memory to grow kernel");

		nkpt++;

		pmap_zero_page(nkpg);
		ptppaddr = VM_PAGE_TO_PHYS(nkpg);
		newpdir = (pd_entry_t) (ptppaddr | PG_V | PG_RW | PG_A | PG_M);
		pdir_pde(PTD, kernel_vm_end) = newpdir;

		LIST_FOREACH(pmap, &allpmaps, pm_list) {
			*pmap_pde(pmap, kernel_vm_end) = newpdir;
		}
		kernel_vm_end = (kernel_vm_end + PAGE_SIZE * NPTEPG) & ~(PAGE_SIZE * NPTEPG - 1);
	}
	splx(s);
}


/***************************************************
 * page management routines.
 ***************************************************/

/*
 * free the pv_entry back to the free list
 */
static PMAP_INLINE void
free_pv_entry(pv_entry_t pv)
{
	pv_entry_count--;
	uma_zfree(pvzone, pv);
}

/*
 * get a new pv_entry, allocating a block from the system
 * when needed.
 * the memory allocation is performed bypassing the malloc code
 * because of the possibility of allocations at interrupt time.
 */
static pv_entry_t
get_pv_entry(void)
{
	pv_entry_count++;
	if (pv_entry_high_water &&
		(pv_entry_count > pv_entry_high_water) &&
		(pmap_pagedaemon_waken == 0)) {
		pmap_pagedaemon_waken = 1;
		wakeup (&vm_pages_needed);
	}
	return uma_zalloc(pvzone, M_NOWAIT);
}

/*
 * This routine is very drastic, but can save the system
 * in a pinch.
 */
void
pmap_collect()
{
	int i;
	vm_page_t m;
	static int warningdone = 0;

	if (pmap_pagedaemon_waken == 0)
		return;

	if (warningdone < 5) {
		printf("pmap_collect: collecting pv entries -- suggest increasing PMAP_SHPGPERPROC\n");
		warningdone++;
	}

	for(i = 0; i < vm_page_array_size; i++) {
		m = &vm_page_array[i];
		if (m->wire_count || m->hold_count || m->busy ||
		    (m->flags & (PG_BUSY | PG_UNMANAGED)))
			continue;
		pmap_remove_all(m);
	}
	pmap_pagedaemon_waken = 0;
}
	

/*
 * If it is the first entry on the list, it is actually
 * in the header and we must copy the following entry up
 * to the header.  Otherwise we must search the list for
 * the entry.  In either case we free the now unused entry.
 */

static int
pmap_remove_entry(pmap_t pmap, vm_page_t m, vm_offset_t va)
{
	pv_entry_t pv;
	int rtval;
	int s;

	s = splvm();
	if (m->md.pv_list_count < pmap->pm_stats.resident_count) {
		TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
			if (pmap == pv->pv_pmap && va == pv->pv_va) 
				break;
		}
	} else {
		TAILQ_FOREACH(pv, &pmap->pm_pvlist, pv_plist) {
			if (va == pv->pv_va) 
				break;
		}
	}

	rtval = 0;
	if (pv) {
		rtval = pmap_unuse_pt(pmap, va, pv->pv_ptem);
		TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);
		m->md.pv_list_count--;
		if (TAILQ_FIRST(&m->md.pv_list) == NULL)
			vm_page_flag_clear(m, PG_MAPPED | PG_WRITEABLE);

		TAILQ_REMOVE(&pmap->pm_pvlist, pv, pv_plist);
		free_pv_entry(pv);
	}
			
	splx(s);
	return rtval;
}

/*
 * Create a pv entry for page at pa for
 * (pmap, va).
 */
static void
pmap_insert_entry(pmap_t pmap, vm_offset_t va, vm_page_t mpte, vm_page_t m)
{

	int s;
	pv_entry_t pv;

	s = splvm();
	pv = get_pv_entry();
	pv->pv_va = va;
	pv->pv_pmap = pmap;
	pv->pv_ptem = mpte;

	TAILQ_INSERT_TAIL(&pmap->pm_pvlist, pv, pv_plist);
	TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_list);
	m->md.pv_list_count++;

	splx(s);
}

/*
 * pmap_remove_pte: do the things to unmap a page in a process
 */
static int
pmap_remove_pte(pmap_t pmap, pt_entry_t *ptq, vm_offset_t va)
{
	pt_entry_t oldpte;
	vm_page_t m;

	oldpte = atomic_readandclear_int(ptq);
	if (oldpte & PG_W)
		pmap->pm_stats.wired_count -= 1;
	/*
	 * Machines that don't support invlpg, also don't support
	 * PG_G.
	 */
	if (oldpte & PG_G)
		pmap_invalidate_page(kernel_pmap, va);
	pmap->pm_stats.resident_count -= 1;
	if (oldpte & PG_MANAGED) {
		m = PHYS_TO_VM_PAGE(oldpte);
		if (oldpte & PG_M) {
#if defined(PMAP_DIAGNOSTIC)
			if (pmap_nw_modified((pt_entry_t) oldpte)) {
				printf(
	"pmap_remove: modified page not writable: va: 0x%x, pte: 0x%x\n",
				    va, oldpte);
			}
#endif
			if (pmap_track_modified(va))
				vm_page_dirty(m);
		}
		if (oldpte & PG_A)
			vm_page_flag_set(m, PG_REFERENCED);
		return pmap_remove_entry(pmap, m, va);
	} else {
		return pmap_unuse_pt(pmap, va, NULL);
	}

	return 0;
}

/*
 * Remove a single page from a process address space
 */
static void
pmap_remove_page(pmap_t pmap, vm_offset_t va)
{
	register pt_entry_t *ptq;

	/*
	 * if there is no pte for this address, just skip it!!!
	 */
	if (*pmap_pde(pmap, va) == 0) {
		return;
	}

	/*
	 * get a local va for mappings for this pmap.
	 */
	ptq = get_ptbase(pmap) + i386_btop(va);
	if (*ptq) {
		(void) pmap_remove_pte(pmap, ptq, va);
		pmap_invalidate_page(pmap, va);
	}
	return;
}

/*
 *	Remove the given range of addresses from the specified map.
 *
 *	It is assumed that the start and end are properly
 *	rounded to the page size.
 */
void
pmap_remove(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	register pt_entry_t *ptbase;
	vm_offset_t pdnxt;
	pd_entry_t ptpaddr;
	vm_offset_t sindex, eindex;
	int anyvalid;

	if (pmap == NULL)
		return;

	if (pmap->pm_stats.resident_count == 0)
		return;

	/*
	 * special handling of removing one page.  a very
	 * common operation and easy to short circuit some
	 * code.
	 */
	if ((sva + PAGE_SIZE == eva) && 
	    ((pmap->pm_pdir[(sva >> PDRSHIFT)] & PG_PS) == 0)) {
		pmap_remove_page(pmap, sva);
		return;
	}

	anyvalid = 0;

	/*
	 * Get a local virtual address for the mappings that are being
	 * worked with.
	 */
	ptbase = get_ptbase(pmap);

	sindex = i386_btop(sva);
	eindex = i386_btop(eva);

	for (; sindex < eindex; sindex = pdnxt) {
		unsigned pdirindex;

		/*
		 * Calculate index for next page table.
		 */
		pdnxt = ((sindex + NPTEPG) & ~(NPTEPG - 1));
		if (pmap->pm_stats.resident_count == 0)
			break;

		pdirindex = sindex / NPDEPG;
		ptpaddr = pmap->pm_pdir[pdirindex];
		if ((ptpaddr & PG_PS) != 0) {
			pmap->pm_pdir[pdirindex] = 0;
			pmap->pm_stats.resident_count -= NBPDR / PAGE_SIZE;
			anyvalid++;
			continue;
		}

		/*
		 * Weed out invalid mappings. Note: we assume that the page
		 * directory table is always allocated, and in kernel virtual.
		 */
		if (ptpaddr == 0)
			continue;

		/*
		 * Limit our scan to either the end of the va represented
		 * by the current page table page, or to the end of the
		 * range being removed.
		 */
		if (pdnxt > eindex) {
			pdnxt = eindex;
		}

		for (; sindex != pdnxt; sindex++) {
			vm_offset_t va;
			if (ptbase[sindex] == 0) {
				continue;
			}
			va = i386_ptob(sindex);
			
			anyvalid++;
			if (pmap_remove_pte(pmap,
				ptbase + sindex, va))
				break;
		}
	}

	if (anyvalid)
		pmap_invalidate_all(pmap);
}

/*
 *	Routine:	pmap_remove_all
 *	Function:
 *		Removes this physical page from
 *		all physical maps in which it resides.
 *		Reflects back modify bits to the pager.
 *
 *	Notes:
 *		Original versions of this routine were very
 *		inefficient because they iteratively called
 *		pmap_remove (slow...)
 */

static void
pmap_remove_all(vm_page_t m)
{
	register pv_entry_t pv;
	pt_entry_t *pte, tpte;
	int s;

#if defined(PMAP_DIAGNOSTIC)
	/*
	 * XXX this makes pmap_page_protect(NONE) illegal for non-managed
	 * pages!
	 */
	if (!pmap_initialized || (m->flags & PG_FICTITIOUS)) {
		panic("pmap_page_protect: illegal for unmanaged page, va: 0x%x", VM_PAGE_TO_PHYS(m));
	}
#endif

	s = splvm();
	while ((pv = TAILQ_FIRST(&m->md.pv_list)) != NULL) {
		pv->pv_pmap->pm_stats.resident_count--;

		pte = pmap_pte_quick(pv->pv_pmap, pv->pv_va);

		tpte = atomic_readandclear_int(pte);
		if (tpte & PG_W)
			pv->pv_pmap->pm_stats.wired_count--;

		if (tpte & PG_A)
			vm_page_flag_set(m, PG_REFERENCED);

		/*
		 * Update the vm_page_t clean and reference bits.
		 */
		if (tpte & PG_M) {
#if defined(PMAP_DIAGNOSTIC)
			if (pmap_nw_modified((pt_entry_t) tpte)) {
				printf(
	"pmap_remove_all: modified page not writable: va: 0x%x, pte: 0x%x\n",
				    pv->pv_va, tpte);
			}
#endif
			if (pmap_track_modified(pv->pv_va))
				vm_page_dirty(m);
		}
		pmap_invalidate_page(pv->pv_pmap, pv->pv_va);

		TAILQ_REMOVE(&pv->pv_pmap->pm_pvlist, pv, pv_plist);
		TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);
		m->md.pv_list_count--;
		pmap_unuse_pt(pv->pv_pmap, pv->pv_va, pv->pv_ptem);
		free_pv_entry(pv);
	}

	vm_page_flag_clear(m, PG_MAPPED | PG_WRITEABLE);

	splx(s);
}

/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 */
void
pmap_protect(pmap_t pmap, vm_offset_t sva, vm_offset_t eva, vm_prot_t prot)
{
	register pt_entry_t *ptbase;
	vm_offset_t pdnxt;
	pd_entry_t ptpaddr;
	vm_offset_t sindex, eindex;
	int anychanged;

	if (pmap == NULL)
		return;

	if ((prot & VM_PROT_READ) == VM_PROT_NONE) {
		pmap_remove(pmap, sva, eva);
		return;
	}

	if (prot & VM_PROT_WRITE)
		return;

	anychanged = 0;

	ptbase = get_ptbase(pmap);

	sindex = i386_btop(sva);
	eindex = i386_btop(eva);

	for (; sindex < eindex; sindex = pdnxt) {

		unsigned pdirindex;

		pdnxt = ((sindex + NPTEPG) & ~(NPTEPG - 1));

		pdirindex = sindex / NPDEPG;
		ptpaddr = pmap->pm_pdir[pdirindex];
		if ((ptpaddr & PG_PS) != 0) {
			pmap->pm_pdir[pdirindex] &= ~(PG_M|PG_RW);
			pmap->pm_stats.resident_count -= NBPDR / PAGE_SIZE;
			anychanged++;
			continue;
		}

		/*
		 * Weed out invalid mappings. Note: we assume that the page
		 * directory table is always allocated, and in kernel virtual.
		 */
		if (ptpaddr == 0)
			continue;

		if (pdnxt > eindex) {
			pdnxt = eindex;
		}

		for (; sindex != pdnxt; sindex++) {

			pt_entry_t pbits;
			vm_page_t m;

			pbits = ptbase[sindex];

			if (pbits & PG_MANAGED) {
				m = NULL;
				if (pbits & PG_A) {
					m = PHYS_TO_VM_PAGE(pbits);
					vm_page_flag_set(m, PG_REFERENCED);
					pbits &= ~PG_A;
				}
				if (pbits & PG_M) {
					if (pmap_track_modified(i386_ptob(sindex))) {
						if (m == NULL)
							m = PHYS_TO_VM_PAGE(pbits);
						vm_page_dirty(m);
						pbits &= ~PG_M;
					}
				}
			}

			pbits &= ~PG_RW;

			if (pbits != ptbase[sindex]) {
				ptbase[sindex] = pbits;
				anychanged = 1;
			}
		}
	}
	if (anychanged)
		pmap_invalidate_all(pmap);
}

/*
 *	Insert the given physical page (p) at
 *	the specified virtual address (v) in the
 *	target physical map with the protection requested.
 *
 *	If specified, the page will be wired down, meaning
 *	that the related pte can not be reclaimed.
 *
 *	NB:  This is the only routine which MAY NOT lazy-evaluate
 *	or lose information.  That is, this routine must actually
 *	insert this page into the given map NOW.
 */
void
pmap_enter(pmap_t pmap, vm_offset_t va, vm_page_t m, vm_prot_t prot,
	   boolean_t wired)
{
	vm_offset_t pa;
	register pt_entry_t *pte;
	vm_offset_t opa;
	pt_entry_t origpte, newpte;
	vm_page_t mpte;

	if (pmap == NULL)
		return;

	va &= PG_FRAME;
#ifdef PMAP_DIAGNOSTIC
	if (va > VM_MAX_KERNEL_ADDRESS)
		panic("pmap_enter: toobig");
	if ((va >= UPT_MIN_ADDRESS) && (va < UPT_MAX_ADDRESS))
		panic("pmap_enter: invalid to pmap_enter page table pages (va: 0x%x)", va);
#endif

	mpte = NULL;
	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
	if (va < VM_MAXUSER_ADDRESS) {
		mpte = pmap_allocpte(pmap, va);
	}
#if 0 && defined(PMAP_DIAGNOSTIC)
	else {
		pd_entry_t *pdeaddr = pmap_pde(pmap, va);
		origpte = *pdeaddr;
		if ((origpte & PG_V) == 0) { 
			panic("pmap_enter: invalid kernel page table page, pdir=%p, pde=%p, va=%p\n",
				pmap->pm_pdir[PTDPTDI], origpte, va);
		}
	}
#endif

	pte = pmap_pte(pmap, va);

	/*
	 * Page Directory table entry not valid, we need a new PT page
	 */
	if (pte == NULL) {
		panic("pmap_enter: invalid page directory, pdir=%p, va=0x%x\n",
			(void *)pmap->pm_pdir[PTDPTDI], va);
	}

	pa = VM_PAGE_TO_PHYS(m) & PG_FRAME;
	origpte = *(vm_offset_t *)pte;
	opa = origpte & PG_FRAME;

	if (origpte & PG_PS)
		panic("pmap_enter: attempted pmap_enter on 4MB page");

	/*
	 * Mapping has not changed, must be protection or wiring change.
	 */
	if (origpte && (opa == pa)) {
		/*
		 * Wiring change, just update stats. We don't worry about
		 * wiring PT pages as they remain resident as long as there
		 * are valid mappings in them. Hence, if a user page is wired,
		 * the PT page will be also.
		 */
		if (wired && ((origpte & PG_W) == 0))
			pmap->pm_stats.wired_count++;
		else if (!wired && (origpte & PG_W))
			pmap->pm_stats.wired_count--;

#if defined(PMAP_DIAGNOSTIC)
		if (pmap_nw_modified((pt_entry_t) origpte)) {
			printf(
	"pmap_enter: modified page not writable: va: 0x%x, pte: 0x%x\n",
			    va, origpte);
		}
#endif

		/*
		 * Remove extra pte reference
		 */
		if (mpte)
			mpte->hold_count--;

		if ((prot & VM_PROT_WRITE) && (origpte & PG_V)) {
			if ((origpte & PG_RW) == 0) {
				*pte |= PG_RW;
				pmap_invalidate_page(pmap, va);
			}
			return;
		}

		/*
		 * We might be turning off write access to the page,
		 * so we go ahead and sense modify status.
		 */
		if (origpte & PG_MANAGED) {
			if ((origpte & PG_M) && pmap_track_modified(va)) {
				vm_page_t om;
				om = PHYS_TO_VM_PAGE(opa);
				vm_page_dirty(om);
			}
			pa |= PG_MANAGED;
		}
		goto validate;
	} 
	/*
	 * Mapping has changed, invalidate old range and fall through to
	 * handle validating new mapping.
	 */
	if (opa) {
		int err;
		err = pmap_remove_pte(pmap, pte, va);
		if (err)
			panic("pmap_enter: pte vanished, va: 0x%x", va);
	}

	/*
	 * Enter on the PV list if part of our managed memory. Note that we
	 * raise IPL while manipulating pv_table since pmap_enter can be
	 * called at interrupt time.
	 */
	if (pmap_initialized && 
	    (m->flags & (PG_FICTITIOUS|PG_UNMANAGED)) == 0) {
		pmap_insert_entry(pmap, va, mpte, m);
		pa |= PG_MANAGED;
	}

	/*
	 * Increment counters
	 */
	pmap->pm_stats.resident_count++;
	if (wired)
		pmap->pm_stats.wired_count++;

validate:
	/*
	 * Now validate mapping with desired protection/wiring.
	 */
	newpte = (vm_offset_t) (pa | pte_prot(pmap, prot) | PG_V);

	if (wired)
		newpte |= PG_W;
	if (va < VM_MAXUSER_ADDRESS)
		newpte |= PG_U;
	if (pmap == kernel_pmap)
		newpte |= pgeflag;

	/*
	 * if the mapping or permission bits are different, we need
	 * to update the pte.
	 */
	if ((origpte & ~(PG_M|PG_A)) != newpte) {
		*pte = newpte | PG_A;
		/*if (origpte)*/ {
			pmap_invalidate_page(pmap, va);
		}
	}
}

/*
 * this code makes some *MAJOR* assumptions:
 * 1. Current pmap & pmap exists.
 * 2. Not wired.
 * 3. Read access.
 * 4. No page table pages.
 * 5. Tlbflush is deferred to calling procedure.
 * 6. Page IS managed.
 * but is *MUCH* faster than pmap_enter...
 */

static vm_page_t
pmap_enter_quick(pmap_t pmap, vm_offset_t va, vm_page_t m, vm_page_t mpte)
{
	pt_entry_t *pte;
	vm_offset_t pa;

	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
	if (va < VM_MAXUSER_ADDRESS) {
		unsigned ptepindex;
		pd_entry_t ptepa;

		/*
		 * Calculate pagetable page index
		 */
		ptepindex = va >> PDRSHIFT;
		if (mpte && (mpte->pindex == ptepindex)) {
			mpte->hold_count++;
		} else {
retry:
			/*
			 * Get the page directory entry
			 */
			ptepa = pmap->pm_pdir[ptepindex];

			/*
			 * If the page table page is mapped, we just increment
			 * the hold count, and activate it.
			 */
			if (ptepa) {
				if (ptepa & PG_PS)
					panic("pmap_enter_quick: unexpected mapping into 4MB page");
				if (pmap->pm_ptphint &&
					(pmap->pm_ptphint->pindex == ptepindex)) {
					mpte = pmap->pm_ptphint;
				} else {
					mpte = pmap_page_lookup(pmap->pm_pteobj, ptepindex);
					pmap->pm_ptphint = mpte;
				}
				if (mpte == NULL)
					goto retry;
				mpte->hold_count++;
			} else {
				mpte = _pmap_allocpte(pmap, ptepindex);
			}
		}
	} else {
		mpte = NULL;
	}

	/*
	 * This call to vtopte makes the assumption that we are
	 * entering the page into the current pmap.  In order to support
	 * quick entry into any pmap, one would likely use pmap_pte_quick.
	 * But that isn't as quick as vtopte.
	 */
	pte = vtopte(va);
	if (*pte) {
		if (mpte)
			pmap_unwire_pte_hold(pmap, mpte);
		return 0;
	}

	/*
	 * Enter on the PV list if part of our managed memory. Note that we
	 * raise IPL while manipulating pv_table since pmap_enter can be
	 * called at interrupt time.
	 */
	if ((m->flags & (PG_FICTITIOUS|PG_UNMANAGED)) == 0)
		pmap_insert_entry(pmap, va, mpte, m);

	/*
	 * Increment counters
	 */
	pmap->pm_stats.resident_count++;

	pa = VM_PAGE_TO_PHYS(m);

	/*
	 * Now validate mapping with RO protection
	 */
	if (m->flags & (PG_FICTITIOUS|PG_UNMANAGED))
		*pte = pa | PG_V | PG_U;
	else
		*pte = pa | PG_V | PG_U | PG_MANAGED;

	return mpte;
}

/*
 * Make a temporary mapping for a physical address.  This is only intended
 * to be used for panic dumps.
 */
void *
pmap_kenter_temporary(vm_offset_t pa, int i)
{
	vm_offset_t va;

	va = (vm_offset_t)crashdumpmap + (i * PAGE_SIZE);
	pmap_kenter(va, pa);
#ifndef I386_CPU
	invlpg(va);
#else
	invltlb();
#endif
	return ((void *)crashdumpmap);
}

#define MAX_INIT_PT (96)
/*
 * pmap_object_init_pt preloads the ptes for a given object
 * into the specified pmap.  This eliminates the blast of soft
 * faults on process startup and immediately after an mmap.
 */
void
pmap_object_init_pt(pmap_t pmap, vm_offset_t addr,
		    vm_object_t object, vm_pindex_t pindex,
		    vm_size_t size, int limit)
{
	vm_offset_t tmpidx;
	int psize;
	vm_page_t p, mpte;
	int objpgs;

	if (pmap == NULL || object == NULL)
		return;

	/*
	 * This code maps large physical mmap regions into the
	 * processor address space.  Note that some shortcuts
	 * are taken, but the code works.
	 */
	if (pseflag && (object->type == OBJT_DEVICE) &&
	    ((addr & (NBPDR - 1)) == 0) && ((size & (NBPDR - 1)) == 0)) {
		int i;
		vm_page_t m[1];
		unsigned int ptepindex;
		int npdes;
		pd_entry_t ptepa;

		if (pmap->pm_pdir[ptepindex = (addr >> PDRSHIFT)])
			return;

retry:
		p = vm_page_lookup(object, pindex);
		if (p && vm_page_sleep_busy(p, FALSE, "init4p"))
			goto retry;

		if (p == NULL) {
			p = vm_page_alloc(object, pindex, VM_ALLOC_NORMAL);
			if (p == NULL)
				return;
			m[0] = p;

			if (vm_pager_get_pages(object, m, 1, 0) != VM_PAGER_OK) {
				vm_page_lock_queues();
				vm_page_free(p);
				vm_page_unlock_queues();
				return;
			}

			p = vm_page_lookup(object, pindex);
			vm_page_wakeup(p);
		}

		ptepa = VM_PAGE_TO_PHYS(p);
		if (ptepa & (NBPDR - 1)) {
			return;
		}

		p->valid = VM_PAGE_BITS_ALL;

		pmap->pm_stats.resident_count += size >> PAGE_SHIFT;
		npdes = size >> PDRSHIFT;
		for(i = 0; i < npdes; i++) {
			pmap->pm_pdir[ptepindex] =
			    ptepa | PG_U | PG_RW | PG_V | PG_PS;
			ptepa += NBPDR;
			ptepindex += 1;
		}
		vm_page_flag_set(p, PG_MAPPED);
		pmap_invalidate_all(kernel_pmap);
		return;
	}

	psize = i386_btop(size);

	if ((object->type != OBJT_VNODE) ||
	    ((limit & MAP_PREFAULT_PARTIAL) && (psize > MAX_INIT_PT) &&
	     (object->resident_page_count > MAX_INIT_PT))) {
		return;
	}

	if (psize + pindex > object->size) {
		if (object->size < pindex)
			return;
		psize = object->size - pindex;
	}

	mpte = NULL;
	/*
	 * if we are processing a major portion of the object, then scan the
	 * entire thing.
	 */
	if (psize > (object->resident_page_count >> 2)) {
		objpgs = psize;

		for (p = TAILQ_FIRST(&object->memq);
		    ((objpgs > 0) && (p != NULL));
		    p = TAILQ_NEXT(p, listq)) {

			if (p->pindex < pindex || p->pindex - pindex >= psize) {
				continue;
			}
			tmpidx = p->pindex - pindex;
			/*
			 * don't allow an madvise to blow away our really
			 * free pages allocating pv entries.
			 */
			if ((limit & MAP_PREFAULT_MADVISE) &&
			    cnt.v_free_count < cnt.v_free_reserved) {
				break;
			}
			vm_page_lock_queues();
			if (((p->valid & VM_PAGE_BITS_ALL) == VM_PAGE_BITS_ALL) &&
				(p->busy == 0) &&
			    (p->flags & (PG_BUSY | PG_FICTITIOUS)) == 0) {
				if ((p->queue - p->pc) == PQ_CACHE)
					vm_page_deactivate(p);
				vm_page_busy(p);
				vm_page_unlock_queues();
				mpte = pmap_enter_quick(pmap, 
					addr + i386_ptob(tmpidx), p, mpte);
				vm_page_lock_queues();
				vm_page_flag_set(p, PG_MAPPED);
				vm_page_wakeup(p);
			}
			vm_page_unlock_queues();
			objpgs -= 1;
		}
	} else {
		/*
		 * else lookup the pages one-by-one.
		 */
		for (tmpidx = 0; tmpidx < psize; tmpidx += 1) {
			/*
			 * don't allow an madvise to blow away our really
			 * free pages allocating pv entries.
			 */
			if ((limit & MAP_PREFAULT_MADVISE) &&
			    cnt.v_free_count < cnt.v_free_reserved) {
				break;
			}
			p = vm_page_lookup(object, tmpidx + pindex);
			if (p == NULL)
				continue;
			vm_page_lock_queues();
			if ((p->valid & VM_PAGE_BITS_ALL) == VM_PAGE_BITS_ALL &&
				(p->busy == 0) &&
			    (p->flags & (PG_BUSY | PG_FICTITIOUS)) == 0) {
				if ((p->queue - p->pc) == PQ_CACHE)
					vm_page_deactivate(p);
				vm_page_busy(p);
				vm_page_unlock_queues();
				mpte = pmap_enter_quick(pmap, 
					addr + i386_ptob(tmpidx), p, mpte);
				vm_page_lock_queues();
				vm_page_flag_set(p, PG_MAPPED);
				vm_page_wakeup(p);
			}
			vm_page_unlock_queues();
		}
	}
	return;
}

/*
 * pmap_prefault provides a quick way of clustering
 * pagefaults into a processes address space.  It is a "cousin"
 * of pmap_object_init_pt, except it runs at page fault time instead
 * of mmap time.
 */
#define PFBAK 4
#define PFFOR 4
#define PAGEORDER_SIZE (PFBAK+PFFOR)

static int pmap_prefault_pageorder[] = {
	-PAGE_SIZE, PAGE_SIZE,
	-2 * PAGE_SIZE, 2 * PAGE_SIZE,
	-3 * PAGE_SIZE, 3 * PAGE_SIZE
	-4 * PAGE_SIZE, 4 * PAGE_SIZE
};

void
pmap_prefault(pmap, addra, entry)
	pmap_t pmap;
	vm_offset_t addra;
	vm_map_entry_t entry;
{
	int i;
	vm_offset_t starta;
	vm_offset_t addr;
	vm_pindex_t pindex;
	vm_page_t m, mpte;
	vm_object_t object;

	if (!curthread || (pmap != vmspace_pmap(curthread->td_proc->p_vmspace)))
		return;

	object = entry->object.vm_object;

	starta = addra - PFBAK * PAGE_SIZE;
	if (starta < entry->start) {
		starta = entry->start;
	} else if (starta > addra) {
		starta = 0;
	}

	mpte = NULL;
	for (i = 0; i < PAGEORDER_SIZE; i++) {
		vm_object_t lobject;
		pt_entry_t *pte;

		addr = addra + pmap_prefault_pageorder[i];
		if (addr > addra + (PFFOR * PAGE_SIZE))
			addr = 0;

		if (addr < starta || addr >= entry->end)
			continue;

		if ((*pmap_pde(pmap, addr)) == NULL) 
			continue;

		pte = vtopte(addr);
		if (*pte)
			continue;

		pindex = ((addr - entry->start) + entry->offset) >> PAGE_SHIFT;
		lobject = object;
		for (m = vm_page_lookup(lobject, pindex);
		    (!m && (lobject->type == OBJT_DEFAULT) && (lobject->backing_object));
		    lobject = lobject->backing_object) {
			if (lobject->backing_object_offset & PAGE_MASK)
				break;
			pindex += (lobject->backing_object_offset >> PAGE_SHIFT);
			m = vm_page_lookup(lobject->backing_object, pindex);
		}

		/*
		 * give-up when a page is not in memory
		 */
		if (m == NULL)
			break;
		vm_page_lock_queues();
		if (((m->valid & VM_PAGE_BITS_ALL) == VM_PAGE_BITS_ALL) &&
			(m->busy == 0) &&
		    (m->flags & (PG_BUSY | PG_FICTITIOUS)) == 0) {

			if ((m->queue - m->pc) == PQ_CACHE) {
				vm_page_deactivate(m);
			}
			vm_page_busy(m);
			vm_page_unlock_queues();
			mpte = pmap_enter_quick(pmap, addr, m, mpte);
			vm_page_lock_queues();
			vm_page_flag_set(m, PG_MAPPED);
			vm_page_wakeup(m);
		}
		vm_page_unlock_queues();
	}
}

/*
 *	Routine:	pmap_change_wiring
 *	Function:	Change the wiring attribute for a map/virtual-address
 *			pair.
 *	In/out conditions:
 *			The mapping must already exist in the pmap.
 */
void
pmap_change_wiring(pmap, va, wired)
	register pmap_t pmap;
	vm_offset_t va;
	boolean_t wired;
{
	register pt_entry_t *pte;

	if (pmap == NULL)
		return;

	pte = pmap_pte(pmap, va);

	if (wired && !pmap_pte_w(pte))
		pmap->pm_stats.wired_count++;
	else if (!wired && pmap_pte_w(pte))
		pmap->pm_stats.wired_count--;

	/*
	 * Wiring is not a hardware characteristic so there is no need to
	 * invalidate TLB.
	 */
	pmap_pte_set_w(pte, wired);
}



/*
 *	Copy the range specified by src_addr/len
 *	from the source map to the range dst_addr/len
 *	in the destination map.
 *
 *	This routine is only advisory and need not do anything.
 */

void
pmap_copy(pmap_t dst_pmap, pmap_t src_pmap, vm_offset_t dst_addr, vm_size_t len,
	  vm_offset_t src_addr)
{
	vm_offset_t addr;
	vm_offset_t end_addr = src_addr + len;
	vm_offset_t pdnxt;
	pd_entry_t src_frame, dst_frame;
	vm_page_t m;

	if (dst_addr != src_addr)
		return;

	src_frame = src_pmap->pm_pdir[PTDPTDI] & PG_FRAME;
	if (src_frame != (PTDpde & PG_FRAME))
		return;

	dst_frame = dst_pmap->pm_pdir[PTDPTDI] & PG_FRAME;
	for (addr = src_addr; addr < end_addr; addr = pdnxt) {
		pt_entry_t *src_pte, *dst_pte;
		vm_page_t dstmpte, srcmpte;
		pd_entry_t srcptepaddr;
		unsigned ptepindex;

		if (addr >= UPT_MIN_ADDRESS)
			panic("pmap_copy: invalid to pmap_copy page tables\n");

		/*
		 * Don't let optional prefaulting of pages make us go
		 * way below the low water mark of free pages or way
		 * above high water mark of used pv entries.
		 */
		if (cnt.v_free_count < cnt.v_free_reserved ||
		    pv_entry_count > pv_entry_high_water)
			break;
		
		pdnxt = ((addr + PAGE_SIZE*NPTEPG) & ~(PAGE_SIZE*NPTEPG - 1));
		ptepindex = addr >> PDRSHIFT;

		srcptepaddr = src_pmap->pm_pdir[ptepindex];
		if (srcptepaddr == 0)
			continue;
			
		if (srcptepaddr & PG_PS) {
			if (dst_pmap->pm_pdir[ptepindex] == 0) {
				dst_pmap->pm_pdir[ptepindex] = srcptepaddr;
				dst_pmap->pm_stats.resident_count += NBPDR / PAGE_SIZE;
			}
			continue;
		}

		srcmpte = vm_page_lookup(src_pmap->pm_pteobj, ptepindex);
		if ((srcmpte == NULL) ||
		    (srcmpte->hold_count == 0) || (srcmpte->flags & PG_BUSY))
			continue;

		if (pdnxt > end_addr)
			pdnxt = end_addr;

		/*
		 * Have to recheck this before every avtopte() call below
		 * in case we have blocked and something else used APTDpde.
		 */
		if (dst_frame != (APTDpde & PG_FRAME)) {
			APTDpde = dst_frame | PG_RW | PG_V;
			pmap_invalidate_all(kernel_pmap); /* XXX Bandaid */
		}
		src_pte = vtopte(addr);
		dst_pte = avtopte(addr);
		while (addr < pdnxt) {
			pt_entry_t ptetemp;
			ptetemp = *src_pte;
			/*
			 * we only virtual copy managed pages
			 */
			if ((ptetemp & PG_MANAGED) != 0) {
				/*
				 * We have to check after allocpte for the
				 * pte still being around...  allocpte can
				 * block.
				 */
				dstmpte = pmap_allocpte(dst_pmap, addr);
				if ((*dst_pte == 0) && (ptetemp = *src_pte)) {
					/*
					 * Clear the modified and
					 * accessed (referenced) bits
					 * during the copy.
					 */
					m = PHYS_TO_VM_PAGE(ptetemp);
					*dst_pte = ptetemp & ~(PG_M | PG_A);
					dst_pmap->pm_stats.resident_count++;
					pmap_insert_entry(dst_pmap, addr,
						dstmpte, m);
	 			} else {
					pmap_unwire_pte_hold(dst_pmap, dstmpte);
				}
				if (dstmpte->hold_count >= srcmpte->hold_count)
					break;
			}
			addr += PAGE_SIZE;
			src_pte++;
			dst_pte++;
		}
	}
}	

#ifdef SMP

/*
 *	pmap_zpi_switchin*()
 *
 *	These functions allow us to avoid doing IPIs alltogether in certain
 *	temporary page-mapping situations (page zeroing).  Instead to deal
 *	with being preempted and moved onto a different cpu we invalidate
 *	the page when the scheduler switches us in.  This does not occur
 *	very often so we remain relatively optimal with very little effort.
 */
static void
pmap_zpi_switchin12(void)
{
	invlpg((u_int)CADDR1);
	invlpg((u_int)CADDR2);
}

static void
pmap_zpi_switchin2(void)
{
	invlpg((u_int)CADDR2);
}

static void
pmap_zpi_switchin3(void)
{
	invlpg((u_int)CADDR3);
}

#endif

/*
 *	pmap_zero_page zeros the specified hardware page by mapping 
 *	the page into KVM and using bzero to clear its contents.
 */
void
pmap_zero_page(vm_page_t m)
{
	vm_offset_t phys;

	phys = VM_PAGE_TO_PHYS(m);
	if (*CMAP2)
		panic("pmap_zero_page: CMAP2 busy");
	*CMAP2 = PG_V | PG_RW | phys | PG_A | PG_M;
#ifdef I386_CPU
	invltlb();
#else
#ifdef SMP
	curthread->td_switchin = pmap_zpi_switchin2;
#endif
	invlpg((u_int)CADDR2);
#endif
#if defined(I686_CPU)
	if (cpu_class == CPUCLASS_686)
		i686_pagezero(CADDR2);
	else
#endif
		bzero(CADDR2, PAGE_SIZE);
#ifdef SMP
	curthread->td_switchin = NULL;
#endif
	*CMAP2 = 0;
}

/*
 *	pmap_zero_page_area zeros the specified hardware page by mapping 
 *	the page into KVM and using bzero to clear its contents.
 *
 *	off and size may not cover an area beyond a single hardware page.
 */
void
pmap_zero_page_area(vm_page_t m, int off, int size)
{
	vm_offset_t phys;

	phys = VM_PAGE_TO_PHYS(m);
	if (*CMAP2)
		panic("pmap_zero_page: CMAP2 busy");
	*CMAP2 = PG_V | PG_RW | phys | PG_A | PG_M;
#ifdef I386_CPU
	invltlb();
#else
#ifdef SMP
	curthread->td_switchin = pmap_zpi_switchin2;
#endif
	invlpg((u_int)CADDR2);
#endif
#if defined(I686_CPU)
	if (cpu_class == CPUCLASS_686 && off == 0 && size == PAGE_SIZE)
		i686_pagezero(CADDR2);
	else
#endif
		bzero((char *)CADDR2 + off, size);
#ifdef SMP
	curthread->td_switchin = NULL;
#endif
	*CMAP2 = 0;
}

/*
 *	pmap_zero_page_idle zeros the specified hardware page by mapping 
 *	the page into KVM and using bzero to clear its contents.  This
 *	is intended to be called from the vm_pagezero process only and
 *	outside of Giant.
 */
void
pmap_zero_page_idle(vm_page_t m)
{
	vm_offset_t phys;

	phys = VM_PAGE_TO_PHYS(m);
	if (*CMAP3)
		panic("pmap_zero_page: CMAP3 busy");
	*CMAP3 = PG_V | PG_RW | phys | PG_A | PG_M;
#ifdef I386_CPU
	invltlb();
#else
#ifdef SMP
	curthread->td_switchin = pmap_zpi_switchin3;
#endif
	invlpg((u_int)CADDR3);
#endif
#if defined(I686_CPU)
	if (cpu_class == CPUCLASS_686)
		i686_pagezero(CADDR3);
	else
#endif
		bzero(CADDR3, PAGE_SIZE);
#ifdef SMP
	curthread->td_switchin = NULL;
#endif
	*CMAP3 = 0;
}

/*
 *	pmap_copy_page copies the specified (machine independent)
 *	page by mapping the page into virtual memory and using
 *	bcopy to copy the page, one machine dependent page at a
 *	time.
 */
void
pmap_copy_page(vm_page_t src, vm_page_t dst)
{

	if (*CMAP1)
		panic("pmap_copy_page: CMAP1 busy");
	if (*CMAP2)
		panic("pmap_copy_page: CMAP2 busy");
	*CMAP1 = PG_V | VM_PAGE_TO_PHYS(src) | PG_A;
	*CMAP2 = PG_V | PG_RW | VM_PAGE_TO_PHYS(dst) | PG_A | PG_M;
#ifdef I386_CPU
	invltlb();
#else
#ifdef SMP
	curthread->td_switchin = pmap_zpi_switchin12;
#endif
	invlpg((u_int)CADDR1);
	invlpg((u_int)CADDR2);
#endif
	bcopy(CADDR1, CADDR2, PAGE_SIZE);
#ifdef SMP
	curthread->td_switchin = NULL;
#endif
	*CMAP1 = 0;
	*CMAP2 = 0;
}


/*
 *	Routine:	pmap_pageable
 *	Function:
 *		Make the specified pages (by pmap, offset)
 *		pageable (or not) as requested.
 *
 *		A page which is not pageable may not take
 *		a fault; therefore, its page table entry
 *		must remain valid for the duration.
 *
 *		This routine is merely advisory; pmap_enter
 *		will specify that these pages are to be wired
 *		down (or not) as appropriate.
 */
void
pmap_pageable(pmap, sva, eva, pageable)
	pmap_t pmap;
	vm_offset_t sva, eva;
	boolean_t pageable;
{
}

/*
 * Returns true if the pmap's pv is one of the first
 * 16 pvs linked to from this page.  This count may
 * be changed upwards or downwards in the future; it
 * is only necessary that true be returned for a small
 * subset of pmaps for proper page aging.
 */
boolean_t
pmap_page_exists_quick(pmap, m)
	pmap_t pmap;
	vm_page_t m;
{
	pv_entry_t pv;
	int loops = 0;
	int s;

	if (!pmap_initialized || (m->flags & PG_FICTITIOUS))
		return FALSE;

	s = splvm();

	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		if (pv->pv_pmap == pmap) {
			splx(s);
			return TRUE;
		}
		loops++;
		if (loops >= 16)
			break;
	}
	splx(s);
	return (FALSE);
}

#define PMAP_REMOVE_PAGES_CURPROC_ONLY
/*
 * Remove all pages from specified address space
 * this aids process exit speeds.  Also, this code
 * is special cased for current process only, but
 * can have the more generic (and slightly slower)
 * mode enabled.  This is much faster than pmap_remove
 * in the case of running down an entire address space.
 */
void
pmap_remove_pages(pmap, sva, eva)
	pmap_t pmap;
	vm_offset_t sva, eva;
{
	pt_entry_t *pte, tpte;
	vm_page_t m;
	pv_entry_t pv, npv;
	int s;

#ifdef PMAP_REMOVE_PAGES_CURPROC_ONLY
	if (!curthread || (pmap != vmspace_pmap(curthread->td_proc->p_vmspace))) {
		printf("warning: pmap_remove_pages called with non-current pmap\n");
		return;
	}
#endif

	s = splvm();
	for (pv = TAILQ_FIRST(&pmap->pm_pvlist); pv; pv = npv) {

		if (pv->pv_va >= eva || pv->pv_va < sva) {
			npv = TAILQ_NEXT(pv, pv_plist);
			continue;
		}

#ifdef PMAP_REMOVE_PAGES_CURPROC_ONLY
		pte = vtopte(pv->pv_va);
#else
		pte = pmap_pte_quick(pv->pv_pmap, pv->pv_va);
#endif
		tpte = *pte;

		if (tpte == 0) {
			printf("TPTE at %p  IS ZERO @ VA %08x\n",
							pte, pv->pv_va);
			panic("bad pte");
		}

/*
 * We cannot remove wired pages from a process' mapping at this time
 */
		if (tpte & PG_W) {
			npv = TAILQ_NEXT(pv, pv_plist);
			continue;
		}

		m = PHYS_TO_VM_PAGE(tpte);
		KASSERT(m->phys_addr == (tpte & PG_FRAME),
		    ("vm_page_t %p phys_addr mismatch %08x %08x",
		    m, m->phys_addr, tpte));

		KASSERT(m < &vm_page_array[vm_page_array_size],
			("pmap_remove_pages: bad tpte %x", tpte));

		pv->pv_pmap->pm_stats.resident_count--;

		*pte = 0;

		/*
		 * Update the vm_page_t clean and reference bits.
		 */
		if (tpte & PG_M) {
			vm_page_dirty(m);
		}

		npv = TAILQ_NEXT(pv, pv_plist);
		TAILQ_REMOVE(&pv->pv_pmap->pm_pvlist, pv, pv_plist);

		m->md.pv_list_count--;
		TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);
		if (TAILQ_FIRST(&m->md.pv_list) == NULL) {
			vm_page_flag_clear(m, PG_MAPPED | PG_WRITEABLE);
		}

		pmap_unuse_pt(pv->pv_pmap, pv->pv_va, pv->pv_ptem);
		free_pv_entry(pv);
	}
	splx(s);
	pmap_invalidate_all(pmap);
}

/*
 * pmap_testbit tests bits in pte's
 * note that the testbit/changebit routines are inline,
 * and a lot of things compile-time evaluate.
 */
static boolean_t
pmap_testbit(m, bit)
	vm_page_t m;
	int bit;
{
	pv_entry_t pv;
	pt_entry_t *pte;
	int s;

	if (!pmap_initialized || (m->flags & PG_FICTITIOUS))
		return FALSE;

	if (TAILQ_FIRST(&m->md.pv_list) == NULL)
		return FALSE;

	s = splvm();

	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		/*
		 * if the bit being tested is the modified bit, then
		 * mark clean_map and ptes as never
		 * modified.
		 */
		if (bit & (PG_A|PG_M)) {
			if (!pmap_track_modified(pv->pv_va))
				continue;
		}

#if defined(PMAP_DIAGNOSTIC)
		if (!pv->pv_pmap) {
			printf("Null pmap (tb) at va: 0x%x\n", pv->pv_va);
			continue;
		}
#endif
		pte = pmap_pte_quick(pv->pv_pmap, pv->pv_va);
		if (*pte & bit) {
			splx(s);
			return TRUE;
		}
	}
	splx(s);
	return (FALSE);
}

/*
 * this routine is used to modify bits in ptes
 */
static __inline void
pmap_changebit(vm_page_t m, int bit, boolean_t setem)
{
	register pv_entry_t pv;
	register pt_entry_t *pte;
	int s;

	if (!pmap_initialized || (m->flags & PG_FICTITIOUS))
		return;

	s = splvm();

	/*
	 * Loop over all current mappings setting/clearing as appropos If
	 * setting RO do we need to clear the VAC?
	 */
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		/*
		 * don't write protect pager mappings
		 */
		if (!setem && (bit == PG_RW)) {
			if (!pmap_track_modified(pv->pv_va))
				continue;
		}

#if defined(PMAP_DIAGNOSTIC)
		if (!pv->pv_pmap) {
			printf("Null pmap (cb) at va: 0x%x\n", pv->pv_va);
			continue;
		}
#endif

		pte = pmap_pte_quick(pv->pv_pmap, pv->pv_va);

		if (setem) {
			*pte |= bit;
			pmap_invalidate_page(pv->pv_pmap, pv->pv_va);
		} else {
			pt_entry_t pbits = *pte;
			if (pbits & bit) {
				if (bit == PG_RW) {
					if (pbits & PG_M) {
						vm_page_dirty(m);
					}
					*pte = pbits & ~(PG_M|PG_RW);
				} else {
					*pte = pbits & ~bit;
				}
				pmap_invalidate_page(pv->pv_pmap, pv->pv_va);
			}
		}
	}
	splx(s);
}

/*
 *      pmap_page_protect:
 *
 *      Lower the permission for all mappings to a given page.
 */
void
pmap_page_protect(vm_page_t m, vm_prot_t prot)
{
	if ((prot & VM_PROT_WRITE) == 0) {
		if (prot & (VM_PROT_READ | VM_PROT_EXECUTE)) {
			pmap_changebit(m, PG_RW, FALSE);
		} else {
			pmap_remove_all(m);
		}
	}
}

vm_offset_t
pmap_phys_address(ppn)
	int ppn;
{
	return (i386_ptob(ppn));
}

/*
 *	pmap_ts_referenced:
 *
 *	Return a count of reference bits for a page, clearing those bits.
 *	It is not necessary for every reference bit to be cleared, but it
 *	is necessary that 0 only be returned when there are truly no
 *	reference bits set.
 *
 *	XXX: The exact number of bits to check and clear is a matter that
 *	should be tested and standardized at some point in the future for
 *	optimal aging of shared pages.
 */
int
pmap_ts_referenced(vm_page_t m)
{
	register pv_entry_t pv, pvf, pvn;
	pt_entry_t *pte;
	int s;
	int rtval = 0;

	if (!pmap_initialized || (m->flags & PG_FICTITIOUS))
		return (rtval);

	s = splvm();

	if ((pv = TAILQ_FIRST(&m->md.pv_list)) != NULL) {

		pvf = pv;

		do {
			pvn = TAILQ_NEXT(pv, pv_list);

			TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);

			TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_list);

			if (!pmap_track_modified(pv->pv_va))
				continue;

			pte = pmap_pte_quick(pv->pv_pmap, pv->pv_va);

			if (pte && (*pte & PG_A)) {
				*pte &= ~PG_A;

				pmap_invalidate_page(pv->pv_pmap, pv->pv_va);

				rtval++;
				if (rtval > 4) {
					break;
				}
			}
		} while ((pv = pvn) != NULL && pv != pvf);
	}
	splx(s);

	return (rtval);
}

/*
 *	pmap_is_modified:
 *
 *	Return whether or not the specified physical page was modified
 *	in any physical maps.
 */
boolean_t
pmap_is_modified(vm_page_t m)
{
	return pmap_testbit(m, PG_M);
}

/*
 *	Clear the modify bits on the specified physical page.
 */
void
pmap_clear_modify(vm_page_t m)
{
	pmap_changebit(m, PG_M, FALSE);
}

/*
 *	pmap_clear_reference:
 *
 *	Clear the reference bit on the specified physical page.
 */
void
pmap_clear_reference(vm_page_t m)
{
	pmap_changebit(m, PG_A, FALSE);
}

/*
 * Miscellaneous support routines follow
 */

static void
i386_protection_init()
{
	register int *kp, prot;

	kp = protection_codes;
	for (prot = 0; prot < 8; prot++) {
		switch (prot) {
		case VM_PROT_NONE | VM_PROT_NONE | VM_PROT_NONE:
			/*
			 * Read access is also 0. There isn't any execute bit,
			 * so just make it readable.
			 */
		case VM_PROT_READ | VM_PROT_NONE | VM_PROT_NONE:
		case VM_PROT_READ | VM_PROT_NONE | VM_PROT_EXECUTE:
		case VM_PROT_NONE | VM_PROT_NONE | VM_PROT_EXECUTE:
			*kp++ = 0;
			break;
		case VM_PROT_NONE | VM_PROT_WRITE | VM_PROT_NONE:
		case VM_PROT_NONE | VM_PROT_WRITE | VM_PROT_EXECUTE:
		case VM_PROT_READ | VM_PROT_WRITE | VM_PROT_NONE:
		case VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE:
			*kp++ = PG_RW;
			break;
		}
	}
}

/*
 * Map a set of physical memory pages into the kernel virtual
 * address space. Return a pointer to where it is mapped. This
 * routine is intended to be used for mapping device memory,
 * NOT real memory.
 */
void *
pmap_mapdev(pa, size)
	vm_offset_t pa;
	vm_size_t size;
{
	vm_offset_t va, tmpva, offset;

	offset = pa & PAGE_MASK;
	size = round_page(offset + size);
	pa = trunc_page(pa);

	/* We have a 1MB direct mapped region at KERNBASE */
	if (pa < 0x00100000 && pa + size <= 0x00100000)
		return (void *)(pa + KERNBASE);

	GIANT_REQUIRED;
	va = kmem_alloc_pageable(kernel_map, size);
	if (!va)
		panic("pmap_mapdev: Couldn't alloc kernel virtual memory");

	for (tmpva = va; size > 0; ) {
		pmap_kenter(tmpva, pa);
		size -= PAGE_SIZE;
		tmpva += PAGE_SIZE;
		pa += PAGE_SIZE;
	}
	pmap_invalidate_range(kernel_pmap, va, tmpva);
	return (void *)(va + offset);
}

void
pmap_unmapdev(va, size)
	vm_offset_t va;
	vm_size_t size;
{
	vm_offset_t base, offset, tmpva;
	pt_entry_t *pte;

	base = va & PG_FRAME;
	offset = va & PAGE_MASK;
	size = round_page(offset + size);
	if (base >= KERNBASE && va + size <= KERNBASE + 0x00100000)
		return;		/* direct mapped */
	for (tmpva = base; tmpva < (base + size); tmpva += PAGE_SIZE) {
		pte = vtopte(tmpva);
		*pte = 0;
	}
	pmap_invalidate_range(kernel_pmap, va, tmpva);
	kmem_free(kernel_map, base, size);
}

/*
 * perform the pmap work for mincore
 */
int
pmap_mincore(pmap, addr)
	pmap_t pmap;
	vm_offset_t addr;
{
	pt_entry_t *ptep, pte;
	vm_page_t m;
	int val = 0;
	
	ptep = pmap_pte(pmap, addr);
	if (ptep == 0) {
		return 0;
	}

	if ((pte = *ptep) != 0) {
		vm_offset_t pa;

		val = MINCORE_INCORE;
		if ((pte & PG_MANAGED) == 0)
			return val;

		pa = pte & PG_FRAME;

		m = PHYS_TO_VM_PAGE(pa);

		/*
		 * Modified by us
		 */
		if (pte & PG_M)
			val |= MINCORE_MODIFIED|MINCORE_MODIFIED_OTHER;
		/*
		 * Modified by someone
		 */
		else if (m->dirty || pmap_is_modified(m))
			val |= MINCORE_MODIFIED_OTHER;
		/*
		 * Referenced by us
		 */
		if (pte & PG_A)
			val |= MINCORE_REFERENCED|MINCORE_REFERENCED_OTHER;

		/*
		 * Referenced by someone
		 */
		else if ((m->flags & PG_REFERENCED) || pmap_ts_referenced(m)) {
			val |= MINCORE_REFERENCED_OTHER;
			vm_page_flag_set(m, PG_REFERENCED);
		}
	} 
	return val;
}

void
pmap_activate(struct thread *td)
{
	struct proc *p = td->td_proc;
	pmap_t	pmap;
	u_int32_t  cr3;

	pmap = vmspace_pmap(td->td_proc->p_vmspace);
#if defined(SMP)
	pmap->pm_active |= PCPU_GET(cpumask);
#else
	pmap->pm_active |= 1;
#endif
	cr3 = vtophys(pmap->pm_pdir);
	/* XXXKSE this is wrong.
	 * pmap_activate is for the current thread on the current cpu
	 */
	if (p->p_flag & P_KSES) {
		/* Make sure all other cr3 entries are updated. */
		/* what if they are running?  XXXKSE (maybe abort them) */
		FOREACH_THREAD_IN_PROC(p, td) {
			td->td_pcb->pcb_cr3 = cr3;
		}
	} else {
		td->td_pcb->pcb_cr3 = cr3;
	}
	load_cr3(cr3);
#ifdef SWTCH_OPTIM_STATS
	tlb_flush_count++;
#endif
}

vm_offset_t
pmap_addr_hint(vm_object_t obj, vm_offset_t addr, vm_size_t size)
{

	if ((obj == NULL) || (size < NBPDR) || (obj->type != OBJT_DEVICE)) {
		return addr;
	}

	addr = (addr + (NBPDR - 1)) & ~(NBPDR - 1);
	return addr;
}


#if defined(PMAP_DEBUG)
pmap_pid_dump(int pid)
{
	pmap_t pmap;
	struct proc *p;
	int npte = 0;
	int index;

	sx_slock(&allproc_lock);
	LIST_FOREACH(p, &allproc, p_list) {
		if (p->p_pid != pid)
			continue;

		if (p->p_vmspace) {
			int i,j;
			index = 0;
			pmap = vmspace_pmap(p->p_vmspace);
			for (i = 0; i < NPDEPG; i++) {
				pd_entry_t *pde;
				pt_entry_t *pte;
				vm_offset_t base = i << PDRSHIFT;
				
				pde = &pmap->pm_pdir[i];
				if (pde && pmap_pde_v(pde)) {
					for (j = 0; j < NPTEPG; j++) {
						vm_offset_t va = base + (j << PAGE_SHIFT);
						if (va >= (vm_offset_t) VM_MIN_KERNEL_ADDRESS) {
							if (index) {
								index = 0;
								printf("\n");
							}
							sx_sunlock(&allproc_lock);
							return npte;
						}
						pte = pmap_pte_quick(pmap, va);
						if (pte && pmap_pte_v(pte)) {
							pt_entry_t pa;
							vm_page_t m;
							pa = *pte;
							m = PHYS_TO_VM_PAGE(pa);
							printf("va: 0x%x, pt: 0x%x, h: %d, w: %d, f: 0x%x",
								va, pa, m->hold_count, m->wire_count, m->flags);
							npte++;
							index++;
							if (index >= 2) {
								index = 0;
								printf("\n");
							} else {
								printf(" ");
							}
						}
					}
				}
			}
		}
	}
	sx_sunlock(&allproc_lock);
	return npte;
}
#endif

#if defined(DEBUG)

static void	pads(pmap_t pm);
void		pmap_pvdump(vm_offset_t pa);

/* print address space of pmap*/
static void
pads(pm)
	pmap_t pm;
{
	int i, j;
	vm_offset_t va;
	pt_entry_t *ptep;

	if (pm == kernel_pmap)
		return;
	for (i = 0; i < NPDEPG; i++)
		if (pm->pm_pdir[i])
			for (j = 0; j < NPTEPG; j++) {
				va = (i << PDRSHIFT) + (j << PAGE_SHIFT);
				if (pm == kernel_pmap && va < KERNBASE)
					continue;
				if (pm != kernel_pmap && va > UPT_MAX_ADDRESS)
					continue;
				ptep = pmap_pte_quick(pm, va);
				if (pmap_pte_v(ptep))
					printf("%x:%x ", va, *ptep);
			};

}

void
pmap_pvdump(pa)
	vm_offset_t pa;
{
	pv_entry_t pv;
	vm_page_t m;

	printf("pa %x", pa);
	m = PHYS_TO_VM_PAGE(pa);
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		printf(" -> pmap %p, va %x", (void *)pv->pv_pmap, pv->pv_va);
		pads(pv->pv_pmap);
	}
	printf(" ");
}
#endif
