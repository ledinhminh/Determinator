/* See COPYRIGHT for copyright information. */

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/main.h>
#include <kern/monitor.h>
#include <kern/console.h>
#include <kern/mem.h>
#include <kern/cpu.h>
#include <kern/mp.h>
#include <kern/trap.h>
#if LAB >= 2
#include <kern/pmap.h>
#include <kern/kclock.h>
#if LAB >= 3
#include <kern/env.h>
#if LAB >= 4
#include <kern/sched.h>
#include <kern/picirq.h>
#if LAB >= 6
#include <kern/time.h>
#include <kern/pci.h>
#endif  // LAB >= 6
#endif	// LAB >= 4
#endif	// LAB >= 3
#endif	// LAB >= 2

#include <dev/lapic.h>



#if LAB >= 2	// ...then leave this code out.
#elif LAB >= 1
// Test the stack backtrace function (lab 1 only)
void
test_backtrace(int x)
{
	cprintf("entering test_backtrace %d\n", x);
	if (x > 0)
		test_backtrace(x-1);
	else
		mon_backtrace(0, 0, 0);
	cprintf("leaving test_backtrace %d\n", x);
}
#endif

// Called from entry.S, only on the bootstrap processor.
// PIOS conventional: all '_init' functions get called
// only at bootstrap and only on the bootstrap processor.
void
init(void)
{
	extern char edata[], end[];

	// Before doing anything else, complete the ELF loading process.
	// Clear the uninitialized global data (BSS) section of our program.
	// This ensures that all static/global variables start out zero.
	memset(edata, 0, end - edata);

	// Initialize the console.
	// Can't call cprintf until after we do this!
	cons_init();

	cprintf("1234 decimal is %o octal!\n", 1234);

	// Initialize and load the bootstrap CPU's GDT, TSS, and IDT.
	cpu_init(&bootcpu);
	assert(cpu_cur() == &bootcpu);	// cpu_cur() should now work
	trap_init();

	cpu_startup();		// load GDT, TSS
	trap_startup();		// load IDT
	lapic_startup();	// setup boot CPU's local APIC

	// Find and init other processors in a multiprocessor system
	mp_init();

	// Physical memory detection/initialization.
	// Can't call mem_alloc until after we do this!
	mem_init();

#if LAB >= 2
	// Lab 2 memory management initialization functions
	pmem_init();
	i386_vm_init();
#endif

#if LAB >= 3
	// Lab 3 user environment initialization functions
	env_init();
	trap_init();
#endif

#if LAB >= 4
	// Lab 4 multitasking initialization functions
	pic_init();
	kclock_init();
#endif

#if LAB >= 6
	time_init();
	pci_init();

#endif
#if LAB >= 7
	// Should always have an idle process as first one.
	ENV_CREATE(user_idle);

	// Start fs.
	ENV_CREATE(fs_fs);

#if !defined(TEST_NO_NS)
	// Start ns.
	ENV_CREATE(net_ns);
#endif

	// Start init
#if defined(TEST)
	// Don't touch -- used by grading script!
	ENV_CREATE2(TEST, TESTSIZE);
#else
	// Touch all you want.
	ENV_CREATE(user_icode);
	// ENV_CREATE(user_pipereadeof);
	// ENV_CREATE(user_pipewriteeof);
#endif
#elif LAB >= 6
	// Should always have an idle process as first one.
	ENV_CREATE(user_idle);

	// Start fs.
	ENV_CREATE(fs_fs);

#if !defined(TEST_NO_NS)
	// Start ns.
	ENV_CREATE(net_ns);
#endif

	// Start init
#if defined(TEST)
	// Don't touch -- used by grading script!
	ENV_CREATE2(TEST, TESTSIZE);
#else
	// Touch all you want.
	// ENV_CREATE(user_echosrv);
	// ENV_CREATE(user_httpd);
#endif
#elif LAB >= 5
	// Should always have an idle process as first one.
	ENV_CREATE(user_idle);

	// Start fs.
	ENV_CREATE(fs_fs);

	// Start init
#if defined(TEST)
	// Don't touch -- used by grading script!
	ENV_CREATE2(TEST, TESTSIZE);
#else
	// Touch all you want.
	// ENV_CREATE(user_writemotd);
	// ENV_CREATE(user_testfsipc);
	// ENV_CREATE(user_icode);
#endif
#elif LAB >= 4
	// Should always have an idle process as first one.
	ENV_CREATE(user_idle);

#if defined(TEST)
	// Don't touch -- used by grading script!
	ENV_CREATE2(TEST, TESTSIZE)
#else
	// Touch all you want.
	ENV_CREATE(user_primes);
#endif // TEST*
#elif LAB >= 3
	// Temporary test code specific to LAB 3
#if defined(TEST)
	// Don't touch -- used by grading script!
	ENV_CREATE2(TEST, TESTSIZE);
#else
	// Touch all you want.
	ENV_CREATE(user_hello);
#endif // TEST*
#endif // LAB5, LAB4, LAB3

#if LAB >= 7
	// Should not be necessary - drain keyboard because interrupt has given up.
	kbd_intr();

#endif

#if LAB >= 4
	// Schedule and run the first user environment!
	sched_yield();
#else
#if LAB >= 3
	// We only have one user environment for now, so just run it.
	env_run(&envs[0]);
#endif
#endif

	cpu_bootothers();	// Get other processors started

	startup();		// Continue with per-CPU startup

#if LAB >= 99
#if LAB >= 2

#else
	// Test the stack backtrace function (lab 1 only)
	test_backtrace(5);
#endif

#if LAB <= 2
	// Drop into the kernel monitor.
	while (1)
		monitor(NULL);
#endif
#endif
}


// Called after bootstrap initialization on ALL processors,
// to initialize each CPU's private state and start it doing work.
// PIOS convention: all '_startup' functions get called on every processor.
void
startup(void)
{
	if (cpu_cur() != &bootcpu) {	// already done by init() on boot CPU
		cpu_startup();
		trap_startup();
		lapic_startup();
	}

	cprintf("CPU %d has booted\n", cpu_cur()->id);
	while (1)
		;
}


/*
 * Variable panicstr contains argument to first call to panic; used as flag
 * to indicate that the kernel has already called panic.
 */
static const char *panicstr;

/*
 * Panic is called on unresolvable fatal errors.
 * It prints "panic: mesg", and then enters the kernel monitor.
 */
void
_panic(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	if (panicstr)
		goto dead;
	panicstr = fmt;

	va_start(ap, fmt);
	cprintf("kernel panic at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);

dead:
	/* break into the kernel monitor */
	while (1)
		monitor(NULL);
}

/* like panic, but don't */
void
_warn(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	va_start(ap, fmt);
	cprintf("kernel warning at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);
}
