#if LAB >= 2
/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/kclock.h>
#if LAB >= 3
#include <kern/env.h>
#endif

// These variables are set by i386_detect_memory()
static physaddr_t maxpa;	// Maximum physical address
size_t npage;			// Amount of physical memory (in pages)
static size_t basemem;		// Amount of base memory (in bytes)
static size_t extmem;		// Amount of extended memory (in bytes)

// These variables are set in i386_vm_init()
pde_t* boot_pgdir;		// Virtual address of boot time page directory
physaddr_t boot_cr3;		// Physical address of boot time page directory
static char* boot_freemem;	// Pointer to next byte of free mem

struct Page* pages;		// Virtual address of physical page array
static struct Page_list page_free_list;	// Free list of physical pages

// Global descriptor table.
//
// The kernel and user segments are identical (except for the DPL).
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
struct Segdesc gdt[] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// 0x28 - tss, initialized in idt_init()
	[GD_TSS >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

static int
nvram_read(int r)
{
	return mc146818_read(r) | (mc146818_read(r + 1) << 8);
}

void
i386_detect_memory(void)
{
	// CMOS tells us how many kilobytes there are
	basemem = ROUNDDOWN(nvram_read(NVRAM_BASELO)*1024, PGSIZE);
	extmem = ROUNDDOWN(nvram_read(NVRAM_EXTLO)*1024, PGSIZE);

	// Calculate the maximum physical address based on whether
	// or not there is any extended memory.  See comment in <inc/mmu.h>.
	if (extmem)
		maxpa = EXTPHYSMEM + extmem;
	else
		maxpa = basemem;

	npage = maxpa / PGSIZE;

	cprintf("Physical memory: %dK available, ", (int)(maxpa/1024));
	cprintf("base = %dK, extended = %dK\n", (int)(basemem/1024), (int)(extmem/1024));
}

// --------------------------------------------------------------
// Set up initial memory mappings and turn on MMU.
// --------------------------------------------------------------

static void check_boot_pgdir(void);

//
// Allocate n bytes of physical memory aligned on an 
// align-byte boundary.  Align must be a power of two.
// Return kernel virtual address.  Returned memory is uninitialized.
//
// If we're out of memory, boot_alloc should panic.
// This function may ONLY be used during initialization,
// before the page_free_list has been set up.
// 
static void*
boot_alloc(uint32_t n, uint32_t align)
{
	extern char end[];
	void *v;

	// Initialize boot_freemem if this is the first time.
	// 'end' is a magic symbol automatically generated by the linker,
	// which points to the end of the kernel's bss segment -
	// i.e., the first virtual address that the linker
	// did _not_ assign to any kernel code or global variables.
	if (boot_freemem == 0)
		boot_freemem = end;

	// LAB 2: Your code here:
	//	Step 1: round boot_freemem up to be aligned properly
	//	Step 2: save current value of boot_freemem as allocated chunk
	//	Step 3: increase boot_freemem to record allocation
	//	Step 4: return allocated chunk

#if SOL >= 2
	boot_freemem = (char*) ROUNDUP(boot_freemem, align);
	if (boot_freemem + n < boot_freemem || boot_freemem + n > (char*) (maxpa + KERNBASE))
		panic("out of memory during i386_vm_init");
	v = boot_freemem;
	boot_freemem += n;
	return v;
#else
	return NULL;
#endif /* SOL >= 2 */
}

//
// Given pgdir, a pointer to a page directory,
// walk the 2-level page table structure to find
// the page table entry (PTE) for linear address la.
// Return a pointer to this PTE.
//
// If the relevant page table doesn't exist in the page directory:
//	- If create == 0, return 0.
//	- Otherwise allocate a new page table, install it into pgdir,
//	  and return a pointer into it.
//        (Questions: What data should the new page table contain?
//	  And what permissions should the new pgdir entry have?
//	  Note that we use the 486-only "WP" feature of %cr0, which
//	  affects the way supervisor-mode writes are checked.)
//
// This function abstracts away the 2-level nature of
// the page directory by allocating new page tables
// as needed.
// 
// boot_pgdir_walk may ONLY be used during initialization,
// before the page_free_list has been set up.
// It should panic on failure.  (Note that boot_alloc already panics
// on failure.)
//
static pte_t*
boot_pgdir_walk(pde_t *pgdir, uintptr_t la, int create)
{
#if SOL >= 2
	pde_t *pde;
	pte_t *pgtab;

	pde = &pgdir[PDX(la)];
	if (*pde & PTE_P)
		pgtab = (pte_t*) KADDR(PTE_ADDR(*pde));
	else {
		if (!create)
			return 0;

		// Must set PTE_U in directory entry so that we can define
		// user segments with boot_map_segment.  If the
		// pages really aren't meant to be user readable,
		// the page table entries will reflect this.

		pgtab = boot_alloc(PGSIZE, PGSIZE);
		memset(pgtab, 0, PGSIZE);
		*pde = PADDR(pgtab) | PTE_P | PTE_U | PTE_W;
	}
	return &pgtab[PTX(la)];
#else
	return 0;
#endif /* SOL >= 2 */
}

//
// Map [la, la+size) of linear address space to physical [pa, pa+size)
// in the page table rooted at pgdir.  Size is a multiple of PGSIZE.
// Use permission bits perm|PTE_P for the entries.
//
// This function may ONLY be used during initialization,
// before the page_free_list has been set up.
//
static void
boot_map_segment(pde_t *pgdir, uintptr_t la, size_t size, physaddr_t pa, int perm)
{
#if SOL >= 2
	size_t i;

	for (i = 0; i < size; i += PGSIZE)
		*boot_pgdir_walk(pgdir, la + i, 1) = (pa + i) | perm | PTE_P;
#endif /* SOL >= 2 */
}

// Set up a two-level page table:
//    boot_pgdir is its linear (virtual) address of the root
//    boot_cr3 is the physical adresss of the root
// Then turn on paging.  Then effectively turn off segmentation.
// (i.e., the segment base addrs are set to zero).
// 
// This function only sets up the kernel part of the address space
// (ie. addresses >= UTOP).  The user part of the address space
// will be setup later.
//
// From UTOP to ULIM, the user is allowed to read but not write.
// Above ULIM the user cannot read (or write). 
void
i386_vm_init(void)
{
	pde_t* pgdir;
	uint32_t cr0;
	size_t n;

#if SOL >= 2
#else
	// Remove this line when you're ready to test this function.
	panic("i386_vm_init: This function is not finished\n");
#endif

	//////////////////////////////////////////////////////////////////////
	// create initial page directory.
	pgdir = boot_alloc(PGSIZE, PGSIZE);
	memset(pgdir, 0, PGSIZE);
	boot_pgdir = pgdir;
	boot_cr3 = PADDR(pgdir);

	//////////////////////////////////////////////////////////////////////
	// Recursively insert PD in itself as a page table, to form
	// a virtual page table at virtual address VPT.
	// (For now, you don't have understand the greater purpose of the
	// following two lines.)

	// Permissions: kernel RW, user NONE
	pgdir[PDX(VPT)] = PADDR(pgdir)|PTE_W|PTE_P;

	// same for UVPT
	// Permissions: kernel R, user R 
	pgdir[PDX(UVPT)] = PADDR(pgdir)|PTE_U|PTE_P;

	//////////////////////////////////////////////////////////////////////
	// Map the kernel stack (symbol name "bootstack").  The complete VA
	// range of the stack, [KSTACKTOP-PTSIZE, KSTACKTOP), breaks into two
	// pieces:
	//     * [KSTACKTOP-KSTKSIZE, KSTACKTOP) -- backed by physical memory
	//     * [KSTACKTOP-PTSIZE, KSTACKTOP-KSTKSIZE) -- not backed => faults
	//     Permissions: kernel RW, user NONE
	// Your code goes here:
#if SOL >= 2
	boot_map_segment(pgdir, KSTACKTOP - KSTKSIZE, KSTKSIZE,
		PADDR(bootstack), PTE_W);
#endif

	//////////////////////////////////////////////////////////////////////
	// Map all of physical memory at KERNBASE. 
	// Ie.  the VA range [KERNBASE, 2^32) should map to
	//      the PA range [0, 2^32 - KERNBASE)
	// We might not have 2^32 - KERNBASE bytes of physical memory, but
	// we just set up the mapping anyway.
	// Permissions: kernel RW, user NONE
	// Your code goes here: 
#if SOL >= 2
	boot_map_segment(pgdir, KERNBASE, -KERNBASE, 0, PTE_W);
#endif

	//////////////////////////////////////////////////////////////////////
	// Make 'pages' point to an array of size 'npage' of 'struct Page'.
	// The kernel uses this structure to keep track of physical pages;
	// 'npage' equals the number of physical pages in memory.  User-level
	// programs get read-only access to the array as well.
	// You must allocate the array yourself.
	// Map this array read-only by the user at linear address UPAGES
	// (ie. perm = PTE_U | PTE_P)
	// Permissions:
	//    - pages -- kernel RW, user NONE
	//    - the read-only version mapped at UPAGES -- kernel R, user R
	// Your code goes here: 
#if SOL >= 2
	n = npage*sizeof(struct Page);
	pages = boot_alloc(n, PGSIZE);
	memset(pages, 0, n);
	boot_map_segment(pgdir, UPAGES, n, PADDR(pages), PTE_U);
#endif
#if LAB >= 3

	//////////////////////////////////////////////////////////////////////
	// Make 'envs' point to an array of size 'NENV' of 'struct Env'.
	// Map this array read-only by the user at linear address UENVS
	// (ie. perm = PTE_U | PTE_P).
	// Permissions:
	//    - envs itself -- kernel RW, user NONE
	//    - the image of envs mapped at UENVS  -- kernel R, user R
	
	// LAB 3: Your code here.
#if SOL >= 3
	n = NENV*sizeof(struct Env);
	envs = boot_alloc(n, PGSIZE);
	memset(envs, 0, n);
	boot_map_segment(pgdir, UENVS, n, PADDR(envs), PTE_U);
#endif	// SOL >= 3
#endif	// LAB >= 3

	// Check that the initial page directory has been set up correctly.
	check_boot_pgdir();

	//////////////////////////////////////////////////////////////////////
	// On x86, segmentation maps a VA to a LA (linear addr) and
	// paging maps the LA to a PA.  I.e. VA => LA => PA.  If paging is
	// turned off the LA is used as the PA.  Note: there is no way to
	// turn off segmentation.  The closest thing is to set the base
	// address to 0, so the VA => LA mapping is the identity.

	// Current mapping: VA KERNBASE+x => PA x.
	//     (segmentation base=-KERNBASE and paging is off)

	// From here on down we must maintain this VA KERNBASE + x => PA x
	// mapping, even though we are turning on paging and reconfiguring
	// segmentation.

	// Map VA 0:4MB same as VA KERNBASE, i.e. to PA 0:4MB.
	// (Limits our kernel to <4MB)
	pgdir[0] = pgdir[PDX(KERNBASE)];

	// Install page table.
	lcr3(boot_cr3);

	// Turn on paging.
	cr0 = rcr0();
	cr0 |= CR0_PE|CR0_PG|CR0_AM|CR0_WP|CR0_NE|CR0_TS|CR0_EM|CR0_MP;
	cr0 &= ~(CR0_TS|CR0_EM);
	lcr0(cr0);

	// Current mapping: KERNBASE+x => x => x.
	// (x < 4MB so uses paging pgdir[0])

	// Reload all segment registers.
	asm volatile("lgdt gdt_pd");
	asm volatile("movw %%ax,%%gs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%es" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" :: "a" (GD_KD));
	asm volatile("ljmp %0,$1f\n 1:\n" :: "i" (GD_KT));  // reload cs
	asm volatile("lldt %%ax" :: "a" (0));

	// Final mapping: KERNBASE+x => KERNBASE+x => x.

	// This mapping was only used after paging was turned on but
	// before the segment registers were reloaded.
	pgdir[0] = 0;

	// Flush the TLB for good measure, to kill the pgdir[0] mapping.
	lcr3(boot_cr3);
}

//
// Checks that the kernel part of virtual address space
// has been setup roughly correctly(by i386_vm_init()).
//
// This function doesn't test every corner case,
// in fact it doesn't test the permission bits at all,
// but it is a pretty good sanity check. 
//
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va);

static void
check_boot_pgdir(void)
{
	uint32_t i, n;
	pde_t *pgdir;

	pgdir = boot_pgdir;

	// check pages array
	n = ROUNDUP(npage*sizeof(struct Page), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
		assert(check_va2pa(pgdir, UPAGES + i) == PADDR(pages) + i);
	
#if LAB >= 3
	// check envs array (new test for lab 3)
	n = ROUNDUP(NENV*sizeof(struct Env), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
		assert(check_va2pa(pgdir, UENVS + i) == PADDR(envs) + i);
#endif

	// check phys mem
	for (i = 0; KERNBASE + i != 0; i += PGSIZE)
		assert(check_va2pa(pgdir, KERNBASE + i) == i);

	// check kernel stack
	for (i = 0; i < KSTKSIZE; i += PGSIZE)
		assert(check_va2pa(pgdir, KSTACKTOP - KSTKSIZE + i) == PADDR(bootstack) + i);

	// check for zero/non-zero in PDEs
	for (i = 0; i < NPDENTRIES; i++) {
		switch (i) {
		case PDX(VPT):
		case PDX(UVPT):
		case PDX(KSTACKTOP-1):
		case PDX(UPAGES):
#if LAB >= 3
		case PDX(UENVS):
#endif
			assert(pgdir[i]);
			break;
		default:
			if (i >= PDX(KERNBASE))
				assert(pgdir[i]);
			else
				assert(pgdir[i] == 0);
			break;
		}
	}
	cprintf("check_boot_pgdir() succeeded!\n");
}

// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pgdir'.  The hardware normally performs
// this functionality for us!  We define our own version to help check
// the check_boot_pgdir() function; it shouldn't be used elsewhere.

static physaddr_t
check_va2pa(pde_t *pgdir, uintptr_t va)
{
	pte_t *p;

	pgdir = &pgdir[PDX(va)];
	if (!(*pgdir & PTE_P))
		return ~0;
	p = (pte_t*) KADDR(PTE_ADDR(*pgdir));
	if (!(p[PTX(va)] & PTE_P))
		return ~0;
	return PTE_ADDR(p[PTX(va)]);
}
		
// --------------------------------------------------------------
// Tracking of physical pages.
// The 'pages' array has one 'struct Page' entry per physical page.
// Pages are reference counted, and free pages are kept on a linked list.
// --------------------------------------------------------------

//  
// Initialize page structure and memory free list.
// After this point, ONLY use the functions below
// to allocate and deallocate physical memory via the page_free_list,
// and NEVER use boot_alloc() or the related boot-time functions above.
//
void
page_init(void)
{
#if SOL >= 2
	int i, inuse;

	LIST_INIT(&page_free_list);

	// Align boot_freemem to page boundary.
	boot_alloc(0, PGSIZE);

	for (i = 0; i < npage; i++) {
		// Off-limits until proven otherwise.
		inuse = 1;

		// The bottom basemem bytes are free except page 0.
		if (i != 0 && i < basemem / PGSIZE)
			inuse = 0;

		// The IO hole and the kernel abut.

		// The memory past the kernel is free.
		if (i >= PADDR(boot_freemem) / PGSIZE)
			inuse = 0;

		pages[i].pp_ref = inuse;
		if (!inuse)
			LIST_INSERT_HEAD(&page_free_list, &pages[i], pp_link);
	}
	
#else /* not SOL >= 2 */
	// The example code here marks all pages as free.
	// However this is not truly the case.  What memory is free?
	//  1) Mark page 0 as in use.
	//     This way we preserve the real-mode IDT and BIOS structures
	//     in case we ever need them.  (Currently we don't, but...)
	//  2) Mark the rest of base memory as free.
	//  3) Then comes the IO hole [IOPHYSMEM, EXTPHYSMEM).
	//     Mark it as in use so that it can never be allocated.      
	//  4) Then extended memory [EXTPHYSMEM, ...).
	//     Some of it is in use, some is free. Where is the kernel?
	//     Which pages are used for page tables and other data structures?
	//
	// Change the code to reflect this.
	int i;
	LIST_INIT(&page_free_list);
	for (i = 0; i < npage; i++) {
		pages[i].pp_ref = 0;
		LIST_INSERT_HEAD(&page_free_list, &pages[i], pp_link);
	}
#endif /* not SOL >= 2 */
}

//
// Initialize a Page structure.
// The result has null links and 0 refcount.
// Note that the corresponding physical page is NOT initialized!
//
static void
page_initpp(struct Page *pp)
{
	memset(pp, 0, sizeof(*pp));
}

//
// Allocates a physical page.
// Does NOT set the contents of the physical page to zero -
// the caller must do that if necessary.
//
// *pp_store -- is set to point to the Page struct of the newly allocated
// page
//
// RETURNS 
//   0 -- on success
//   -E_NO_MEM -- otherwise 
//
// Hint: use LIST_FIRST, LIST_REMOVE, and page_initpp
// Hint: pp_ref should not be incremented 
int
page_alloc(struct Page **pp_store)
{
	// Fill this function in
#if SOL >= 2
	*pp_store = LIST_FIRST(&page_free_list);
	if (*pp_store) {
		LIST_REMOVE(*pp_store, pp_link);
		page_initpp(*pp_store);
		return 0;
	}

	//warn("page_alloc() can't find memory");
#endif /* not SOL >= 2 */
	return -E_NO_MEM;
}

//
// Return a page to the free list.
// (This function should only be called when pp->pp_ref reaches 0.)
//
void
page_free(struct Page *pp)
{
#if SOL >= 2
	if (pp->pp_ref) {
		warn("page_free: attempt to free mapped page");
		return;		/* be conservative and assume page is still used */
	}
	LIST_INSERT_HEAD(&page_free_list, pp, pp_link);
	pp->pp_ref = 0;
#else /* not SOL >= 2 */
	// Fill this function in
#endif /* not SOL >= 2 */
}

//
// Decrement the reference count on a page,
// freeing it if there are no more refs.
//
void
page_decref(struct Page* pp)
{
	if (--pp->pp_ref == 0)
		page_free(pp);
}

// Given 'pgdir', a pointer to a page directory, pgdir_walk returns
// a pointer to the page table entry (PTE) for linear address 'va'.
// This requires walking the two-level page table structure.
//
// If the relevant page table doesn't exist in the page directory, then:
//    - If create == 0, pgdir_walk returns NULL.
//    - Otherwise, pgdir_walk tries to allocate a new page table
//	with page_alloc.  If this fails, pgdir_walk returns NULL.
//    - Otherwise, pgdir_walk returns a pointer into the new page table.
//
// This is boot_pgdir_walk, but using page_alloc() instead of boot_alloc().
// Unlike boot_pgdir_walk, pgdir_walk can fail.
//
// Hint: you can turn a Page * into the physical address of the
// page it refers to with page2pa() from kern/pmap.h.
pte_t *
pgdir_walk(pde_t *pgdir, const void *va, int create)
{
#if SOL >= 2
	int r;
	struct Page *pp;
	pde_t *pde;
	pte_t *pgtab;

	pde = &pgdir[PDX(va)];
	if (*pde & PTE_P)
		pgtab = (pte_t*) KADDR(PTE_ADDR(*pde));
	else if (!create || (r = page_alloc(&pp)) < 0)
		return NULL;
	else {
		pp->pp_ref++;
		pgtab = (pte_t*)page2kva(pp);

		// Make sure all those PTE_P bits are zero.
		memset(pgtab, 0, PGSIZE);

		// The permissions here are overly generous, but they can
		// be further restricted by the permissions in the page table 
		// entries, if necessary.
		*pde = page2pa(pp) | PTE_P | PTE_W | PTE_U;
	}

	return &pgtab[PTX(va)];
#else /* not SOL >= 2 */
	// Fill this function in
	return NULL;
#endif /* not SOL >= 2 */
}

//
// Map the physical page 'pp' at virtual address 'va'.
// The permissions (the low 12 bits) of the page table
//  entry should be set to 'perm|PTE_P'.
//
// Details
//   - If there is already a page mapped at 'va', it is page_remove()d.
//   - If necessary, on demand, allocates a page table and inserts it into
//     'pgdir'.
//   - pp->pp_ref should be incremented if the insertion succeeds.
//   - The TLB must be invalidated if a page was formerly present at 'va'.
//
// RETURNS: 
//   0 on success
//   -E_NO_MEM, if page table couldn't be allocated
//
// Hint: The TA solution is implemented using pgdir_walk, page_remove,
// and page2pa.
//
int
page_insert(pde_t *pgdir, struct Page *pp, void *va, int perm) 
{
#if SOL >= 2
	pte_t* pte;

	if ((pte = pgdir_walk(pgdir, va, 1)) == NULL)
		return -E_NO_MEM;

	// We must increment pp_ref before page_remove, so that
	// if pp is already mapped at va (we're just changing perm),
	// we don't lose the page when we decref in page_remove.
	pp->pp_ref++;

	if (*pte & PTE_P)
		page_remove(pgdir, va);

	*pte = page2pa(pp) | perm | PTE_P;
	return 0;
#else /* not SOL >= 2 */
	// Fill this function in
	return 0;
#endif /* not SOL >= 2 */
}

//
// Return the page mapped at virtual address 'va'.
// If pte_store is not zero, then we store in it the address
// of the pte for this page.  This is used by page_remove
// but should not be used by other callers.
//
// Return 0 if there is no page mapped at va.
//
// Hint: the TA solution uses pgdir_walk and pa2page.
//
struct Page *
page_lookup(pde_t *pgdir, void *va, pte_t **pte_store)
{
#if SOL >= 2
	int r;
	pte_t *pte = pgdir_walk(pgdir, va, 0);

	if (pte == 0 || *pte == 0)
		return 0;
	if (pte_store)
		*pte_store = pte;
	if (!(*pte & PTE_P) || PPN(PTE_ADDR(*pte)) >= npage) {
		warn("page_lookup: found bogus PTE 0x%08lx at pgdir %p va %p",
			*pte, pgdir, va);
		return 0;
	}

	return pa2page(PTE_ADDR(*pte));
#else /* not SOL >= 2 */
	// Fill this function in
	return NULL;
#endif /* not SOL >= 2 */
}

//
// Unmaps the physical page at virtual address 'va'.
// If there is no physical page at that address, silently does nothing.
//
// Details:
//   - The ref count on the physical page should decrement.
//   - The physical page should be freed if the refcount reaches 0.
//   - The pg table entry corresponding to 'va' should be set to 0.
//     (if such a PTE exists)
//   - The TLB must be invalidated if you remove an entry from
//     the pg dir/pg table.
//
// Hint: The TA solution is implemented using page_lookup,
// 	tlb_invalidate, and page_decref.
//
void
page_remove(pde_t *pgdir, void *va)
{
#if SOL >= 2
	struct Page *pp;
	pte_t *pte;

	if ((pp = page_lookup(pgdir, va, &pte)) == 0)
		return;
	*pte = 0;
	tlb_invalidate(pgdir, va);
	page_decref(pp);
#else /* not SOL >= 2 */
	// Fill this function in
#endif /* not SOL >= 2 */
}

//
// Invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
//
void
tlb_invalidate(pde_t *pgdir, void *va)
{
	// Flush the entry only if we're modifying the current address space.
#if LAB >= 4
	if (!curenv || curenv->env_pgdir == pgdir)
		invlpg(va);
#else
	// For now, there is only one address space, so always invalidate.
	invlpg(va);
#endif
}

#if LAB >= 3
static uintptr_t user_mem_check_addr;

//
// Check that an environment is allowed to access the range of memory
// [va, va+len) with permissions 'perm | PTE_P'.
// Normally 'perm' will contain PTE_U at least, but this is not required.
// 'va' and 'len' need not be page-aligned; you must test every page that
// contains any of that range.  You will test either 'len/PGSIZE',
// 'len/PGSIZE + 1', or 'len/PGSIZE + 2' pages.
//
// A user program can access a virtual address if (1) the address is below
// ULIM, and (2) the page table gives it permission.  These are exactly
// the tests you should implement here.
//
// If there is an error, set the 'user_mem_check_addr' variable to the first
// erroneous virtual address.
//
// Returns 0 if the user program can access this range of addresses,
// and -E_FAULT otherwise.
//
// Hint: The TA solution uses pgdir_walk.
//
int
user_mem_check(struct Env *env, const void *va, size_t len, int perm)
{
#if SOL >= 3
	const void *endva = (const void *) ((uintptr_t) va + len);
	pte_t *ptep;
	
	// Prevent access to kernel memory or wrapped access
	if ((uintptr_t) endva >= ULIM || va > endva) {
		user_mem_check_addr = (uintptr_t) va;
		return -E_FAULT;
	}

	// Check page table entries in relevant range
	while (va < endva) {
		ptep = pgdir_walk(env->env_pgdir, va, 0);
		if (!ptep || (*ptep & (perm | PTE_P)) != (perm | PTE_P)) {
			user_mem_check_addr = (uintptr_t) va;
			return -E_FAULT;
		}
		va = ROUNDUP(va + 1, PGSIZE);
	}
#else
	// LAB 3: Your code here. 
#endif

	return 0;
}

//
// Checks that environment 'env' is allowed to access the range
// of memory [va, va+len) with permissions 'perm | PTE_U'.
// If it can, then the function simply returns.
// If it cannot, 'env' is destroyed.
//
void
user_mem_assert(struct Env *env, const void *va, size_t len, int perm)
{
	if (user_mem_check(env, va, len, perm | PTE_U) < 0) {
		cprintf("[%08x] user_mem_check assertion failure for "
			"va %08x\n", curenv->env_id, user_mem_check_addr);
		env_destroy(env);	// may not return
	}
}

#endif
void
page_check(void)
{
	struct Page *pp, *pp0, *pp1, *pp2;
	struct Page_list fl;
	pte_t *ptep, *ptep1;
	void *va;
	int i;

	// poison the pages on the free list 
	// to sniff out problems in the future.
	// ideally we'd memset the entire page
	// but it takes too long using bochs.
	LIST_FOREACH(pp0, &page_free_list, pp_link)
		memset(page2kva(pp0), 0x97, 128);

	// should be able to allocate three pages
	pp0 = pp1 = pp2 = 0;
	assert(page_alloc(&pp0) == 0);
	assert(page_alloc(&pp1) == 0);
	assert(page_alloc(&pp2) == 0);

	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);

	// temporarily steal the rest of the free pages
	fl = page_free_list;
	LIST_INIT(&page_free_list);

	// should be no free memory
	assert(page_alloc(&pp) == -E_NO_MEM);

	// there is no page allocated at address 0
	assert(page_lookup(boot_pgdir, (void *) 0x0, &ptep) == NULL);

	// there is no free memory, so we can't allocate a page table 
	assert(page_insert(boot_pgdir, pp1, 0x0, 0) < 0);

	// free pp0 and try again: pp0 should be used for page table
	page_free(pp0);
	assert(page_insert(boot_pgdir, pp1, 0x0, 0) == 0);
	assert(PTE_ADDR(boot_pgdir[0]) == page2pa(pp0));
	assert(check_va2pa(boot_pgdir, 0x0) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp0->pp_ref == 1);

	// should be able to map pp2 at PGSIZE because pp0 is already allocated for page table
	assert(page_insert(boot_pgdir, pp2, (void*) PGSIZE, 0) == 0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);

	// should be no free memory
	assert(page_alloc(&pp) == -E_NO_MEM);

	// should be able to map pp2 at PGSIZE because it's already there
	assert(page_insert(boot_pgdir, pp2, (void*) PGSIZE, 0) == 0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);

	// pp2 should NOT be on the free list
	// could happen in ref counts are handled sloppily in page_insert
	assert(page_alloc(&pp) == -E_NO_MEM);

	// check that pgdir_walk returns a pointer to the pte
	ptep = KADDR(PTE_ADDR(boot_pgdir[PDX(PGSIZE)]));
	assert(pgdir_walk(boot_pgdir, (void*)PGSIZE, 0) == ptep+PTX(PGSIZE));

	// should be able to change permissions too.
	assert(page_insert(boot_pgdir, pp2, (void*) PGSIZE, PTE_U) == 0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);
	assert(*pgdir_walk(boot_pgdir, (void*) PGSIZE, 0) & PTE_U);
	
	// should not be able to map at PTSIZE because need free page for page table
	assert(page_insert(boot_pgdir, pp0, (void*) PTSIZE, 0) < 0);

	// insert pp1 at PGSIZE (replacing pp2)
	assert(page_insert(boot_pgdir, pp1, (void*) PGSIZE, 0) == 0);

	// should have pp1 at both 0 and PGSIZE, pp2 nowhere, ...
	assert(check_va2pa(boot_pgdir, 0) == page2pa(pp1));
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp1));
	// ... and ref counts should reflect this
	assert(pp1->pp_ref == 2);
	assert(pp2->pp_ref == 0);

	// pp2 should be returned by page_alloc
	assert(page_alloc(&pp) == 0 && pp == pp2);

	// unmapping pp1 at 0 should keep pp1 at PGSIZE
	page_remove(boot_pgdir, 0x0);
	assert(check_va2pa(boot_pgdir, 0x0) == ~0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp2->pp_ref == 0);

	// unmapping pp1 at PGSIZE should free it
	page_remove(boot_pgdir, (void*) PGSIZE);
	assert(check_va2pa(boot_pgdir, 0x0) == ~0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == ~0);
	assert(pp1->pp_ref == 0);
	assert(pp2->pp_ref == 0);

	// so it should be returned by page_alloc
	assert(page_alloc(&pp) == 0 && pp == pp1);

	// should be no free memory
	assert(page_alloc(&pp) == -E_NO_MEM);
	
	// should be able to page_insert to change a page
	// and see the new data immediately.
	memset(page2kva(pp1), 1, PGSIZE);
	memset(page2kva(pp2), 2, PGSIZE);
	page_insert(boot_pgdir, pp1, 0x0, 0);
	assert(pp1->pp_ref == 1);
	assert(*(int*)0 == 0x01010101);
	page_insert(boot_pgdir, pp2, 0x0, 0);
	assert(*(int*)0 == 0x02020202);
	assert(pp2->pp_ref == 1);
	assert(pp1->pp_ref == 0);
	page_remove(boot_pgdir, 0x0);
	assert(pp2->pp_ref == 0);

	// forcibly take pp0 back
	assert(PTE_ADDR(boot_pgdir[0]) == page2pa(pp0));
	boot_pgdir[0] = 0;
	assert(pp0->pp_ref == 1);
	pp0->pp_ref = 0;
	
	// check pointer arithmetic in pgdir_walk
	page_free(pp0);
	va = (void*)(PGSIZE * NPDENTRIES + PGSIZE);
	ptep = pgdir_walk(boot_pgdir, va, 1);
	ptep1 = KADDR(PTE_ADDR(boot_pgdir[PDX(va)]));
	assert(ptep == ptep1 + PTX(va));
	boot_pgdir[PDX(va)] = 0;
	pp0->pp_ref = 0;
	
	// check that new page tables get cleared
	memset(page2kva(pp0), 0xFF, PGSIZE);
	page_free(pp0);
	pgdir_walk(boot_pgdir, 0x0, 1);
	ptep = page2kva(pp0);
	for(i=0; i<NPTENTRIES; i++)
		assert((ptep[i] & PTE_P) == 0);
	boot_pgdir[PDX(va)] = 0;
	pp0->pp_ref = 0;

	// give free list back
	page_free_list = fl;

	// free the pages we took
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);
	
	cprintf("page_check() succeeded!\n");
}

#endif /* LAB >= 2 */
