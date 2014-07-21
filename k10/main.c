#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "init.h"
#include "io.h"
#include "apic.h"

#include "amd64.h"
#include "reboot.h"

Sys*	sys;
char	dbgflg[256];

static uintptr sp;		/* XXX - must go - user stack of init proc */
static int maxmach = MACHMAX;

extern	void	options(void);
//extern	void	setmachsched(Mach*);

void
squidboy(int apicno)
{
	sys->machptr[m->machno] = m;

	m->perf.period = 1;
	m->cpuhz = sys->machptr[0]->cpuhz;
	m->cyclefreq = m->cpuhz;
	m->cpumhz = sys->machptr[0]->cpumhz;

	DBG("Hello Squidboy %d %d\n", apicno, m->machno);

	trapinit();
	vsvminit(MACHSTKSZ);
	apmmuinit();
	if(!lapiconline())
		ndnr();
	fpuinit();
	m->splpc = 0;
	m->online = 1;

	DBG("Wait for the thunderbirds!\n");
	while(!active.thunderbirdsarego)
		monmwait(&active.thunderbirdsarego, 0);
	wrmsr(0x10, sys->epoch);
	m->rdtsc = rdtsc();

	DBG("cpu%d color %d tsc %lld\n",
		m->machno, machcolor(m), m->rdtsc);

	/*
	 * enable interrupts.
	 */
	lapicpri(0);

	timersinit();
	adec(&active.nbooting);
	ainc(&active.nonline);

	schedinit();
	panic("squidboy returns");
}

/*
 * Wait for other cores to be initialized and sync tsc counters.
 * Assume other cores have had time to set active.online=1.
 */
static void
nixsquids(void)
{
	int i;
	uvlong now, start;
	Mach *mp;

	for(i = 1; i < MACHMAX; i++)
		if((mp = sys->machptr[i]) != nil && mp->online != 0){
			sys->nmach++;
			ainc(&active.nbooting);
		}
	sys->epoch = rdtsc();
	coherence();
	wrmsr(0x10, sys->epoch);
	m->rdtsc = rdtsc();
	active.thunderbirdsarego = 1;
	start = fastticks2us(fastticks(nil));
	do{
		now = fastticks2us(fastticks(nil));
	}while(active.nbooting > 0 && now - start < 1000000);
	if(active.nbooting > 0)
		print("cpu0: %d maches couldn't start\n", active.nbooting);
	active.nbooting = 0;
}

static void
torusinit(void)
{
	int i,j;
	for(i = 0; i < sys->nmach; i++) {
		for(j = 0; j < NDIM; j++)
			sys->machptr[i]->neighbors[j] = sys->machptr[(i+j+1)%sys->nmach];
		sys->machptr[i]->rqn = 1;
		sys->machptr[i]->readytimeavg = 0;
	}
}

void
sysconfinit(void)
{
	sys->nproc = 2000;
	sys->nimage = 200;
}

void
main(void)
{
	vlong hz;
	struct Account sched_stats;

	memset(edata, 0, end - edata);
	cgapost(sizeof(uintptr)*8);
	memset(m, 0, sizeof(Mach));

	sched_stats.qn = 1;
	sched_stats.queuetimeavg = 0;

	m->machno = 0;
	m->online = 1;
	sys->machptr[m->machno] = &sys->mach;
	m->stack = PTR2UINT(sys->machstk);
	m->vsvm = sys->vsvmpage;
	up = nil;
	active.nonline = 1;
	active.exiting = 0;
	active.nbooting = 0;
	log2init();
	adrinit();
	sysconfinit();
	options();

	/*
	 * Need something for initial delays
	 * until a timebase is worked out.
	 */
	m->cpuhz = 2000000000ll;
	m->cpumhz = 2000;
	wrmsr(0x10, 0);				/* reset tsc */

	cgainit();
	i8250console();
	consputs = cgaconsputs;

	vsvminit(MACHSTKSZ);
	sys->nmach = 1;
	fmtinit();
	print("\nnix\n");

	m->perf.period = 1;
	hz = archhz();
	m->cpuhz = hz;
	m->cyclefreq = hz;
	m->cpumhz = hz/1000000ll;

	/* Mmuinit before meminit because it flushes the TLB via m->pml4->pa.  */
	mmuinit();

	ioinit();
	kbdinit();
	meminit();
	archinit();
	mallocinit();
	archpciinit();

	i8259init(32);
	if(getconf("*maxmach") != nil)
		maxmach = atoi(getconf("*maxmach"));
	mpsinit(maxmach);		/* acpi */

	umeminit();
	trapinit();
	printinit();

	lapiconline();
	ioapiconline();
	sipi();

	timersinit();
	kbdenable();
	i8042kbdenable();
	fpuinit();
	psinit(sys->nproc);
	initimage();
	links();
	devtabreset();
	pageinit();
	swapinit();
	userinit();
	nixsquids();
	torusinit();
	schedinit();
}

static void
init0(void)
{
	char buf[2*KNAMELEN];

	up->nerrlab = 0;

	spllo();

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	up->slash = namec("#/", Atodir, 0, 0);
	pathclose(up->slash->path);
	up->slash->path = newpath("/");
	up->dot = cclone(up->slash);

	devtabinit();

	if(!waserror()){
		snprint(buf, sizeof(buf), "%s %s", "AMD64", conffile);
		ksetenv("terminal", buf, 0);
		ksetenv("cputype", "amd64", 0);
		if(cpuserver)
			ksetenv("service", "cpu", 0);
		else
			ksetenv("service", "terminal", 0);
		confsetenv();
		poperror();
	}
	kproc("alarm", alarmkproc, 0);
	touser(sp);
}

#include <tos.h>
char*
stkadj(uintptr base, char *p)
{
	uintptr a;

	a = PTR2UINT(p) - base;		/* offset in page */
	a += USTKTOP - BIGPGSZ;	/* + base address */
	return (char*)UINT2PTR(a);
}

static void
bootargs(uintptr base, int argc, char **argv)
{
	int i, len;
	usize ssize;
	char **av, *p, *q, *e;

	len = 0;
	for(i = 0; i < argc; i++)
		len += strlen(argv[i] + 1);

	/*
	 * Push the boot args onto the stack.
	 * Make sure the validaddr check in syscall won't fail
	 * because there are fewer than the maximum number of
	 * args by subtracting sizeof(up->arg).
	 */
	p = UINT2PTR(STACKALIGN(base + BIGPGSZ - sizeof(up->arg) - sizeof(Tos) - len));
	av = (char**)(p - (argc+2)*sizeof(char*));
	ssize = base + BIGPGSZ - PTR2UINT(av);
	sp = USTKTOP - ssize;

	q = p;
	e = q + len;
	av[0] = (char*)argc;
	for(i = 0; i < argc; i++){
		av[i+1] = stkadj(base, argv[i]);
		q = seprint(q, e, "%s", argv[i]);
		*q++ = 0;
	}
	av[i+1] = nil;
}

void
userinit(void)
{
	Proc *p;
	Segment *s;
	KMap *k;
	Page *pg;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = smalloc(sizeof(Egrp));
	p->egrp->ref = 1;
	p->fgrp = dupfgrp(nil);
	p->rgrp = newrgrp();
	p->procmode = 0640;

	kstrdup(&eve, "");
	kstrdup(&p->text, "*init*");
	kstrdup(&p->user, eve);

	/*
	 * Kernel Stack
	 *
	 * N.B. make sure there's enough space for syscall to check
	 * for valid args and space for gotolabel's return PC
	 */
	p->sched.pc = PTR2UINT(init0);
	p->sched.sp = PTR2UINT(p->kstack+KSTACK-sizeof(up->arg)-sizeof(uintptr));
	p->sched.sp = STACKALIGN(p->sched.sp);

	/*
	 * User Stack
	 *
	 * Technically, newpage can't be called here because it
	 * should only be called when in a user context as it may
	 * try to sleep if there are no pages available, but that
	 * shouldn't be the case here.
	 */
	s = newseg(SG_STACK, USTKTOP-USTKSIZE, USTKTOP);
	p->seg[SSEG] = s;

	pg = newpage(1, 0, USTKTOP-m->pgsz[s->pgszi], m->pgsz[s->pgszi], -1);
	segpage(s, pg);
	k = kmap(pg);
	bootargs(VA(k), 0, nil);
	kunmap(k);

	/*
	 * Text
	 */
	s = newseg(SG_TEXT, UTZERO, UTZERO+BIGPGSZ);
	s->flushme++;
	p->seg[TSEG] = s;
	pg = newpage(1, 0, UTZERO, m->pgsz[s->pgszi], -1);
	memset(pg->cachectl, PG_TXTFLUSH, sizeof(pg->cachectl));
	segpage(s, pg);
	k = kmap(s->map[0]->pages[0]);
	memmove(UINT2PTR(VA(k)), initcode, sizeof initcode);
	kunmap(k);

	ready(p);
}

void
apshut(void *v)
{
	int i;

	i = (int)(uintptr)v;
	procwired(up, i);
	splhi();
	lapicpri(0xff);
	m->online = 0;
	adec(&active.nonline);
	adec((int*)&sys->nmach);
	ndnr();
}

#define REBOOTADDR (0x11000)

void
reboot(void *entry, void *code, usize size)
{
	int i;
	uintptr a;
	void (*f)(uintmem, uintmem, usize);

	writeconf();
	synccons();
	procwired(up, 0);

	/*
	 * assumptions abound that the set of processors is contiguous
	 * cf. procflushseg
	 */
	for(i = active.nonline - 1; i > 0; i--){
		kproc("apshut", apshut, (void*)i);
		while((a=active.nonline) == i)
			monmwait(&active.nonline, a);
	}

	devtabshutdown();
	pcireset();
	splhi();
	lapicpri(0xff);

	if(up != nil){
		m->proc = nil;
		mmurelease(up);
		up = nil;
	}

	mmumap(0, 0, 6*1024*1024, PteP|PteRW);
	putcr3(m->pml4->pa);

	f = UINT2PTR(REBOOTADDR);
	memmove(f, rebootcode, sizeof(rebootcode));
	print("warp32...\n");
	f(PTR2UINT(entry), PADDR(code), size);
}

void
exit(int ispanic)
{
	int ms;

	synccons();
	/* accounting */
	if(!m->online)
		archreset();	/* exiting a second time; must not hang */
	m->online = 0;
	iprint("cpu%d: exiting\n", m->machno);
	adec(&active.nonline);
	ainc(&active.exiting);

	/* wait (terminals wait forever) */
	if(ispanic && !cpuserver)
		ndnr();
	for(ms = 0; ms < 5000; ms += 1){
		if(active.nonline == 0)
			break;
		delay(1);
	}
	archreset();
}
