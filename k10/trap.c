#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	<tos.h>
#include	"ureg.h"

#include	"io.h"
#include	"apic.h"
#include	"amd64.h"

extern int notify(Ureg*);

static void debugbpt(Ureg*, void*);
static void faultamd64(Ureg*, void*);
static void doublefault(Ureg*, void*);
static void unexpected(Ureg*, void*);
static void expected(Ureg*, void*);
static void dumpstackwithureg(Ureg*);

static Lock vctllock;
static Vctl *vctl[MACHMAX][256];

void
vctlinit(Vctl *v)
{
	memset(v, 0, sizeof *v);
	v->type = "unset";
	v->affinity = -1;
}

void*
vintrenable(Vctl *v, char *name)
{
	int vno;
	Vctl *old;

	if(v->f == nil)
		panic("intrenable: %s: no handler for %d, tbdf %#T", name, v->irq, v->tbdf);
	v->isintr = 1;
	strncpy(v->name, name, KNAMELEN-1);
	v->name[KNAMELEN-1] = 0;

	ilock(&vctllock);
	vno = ioapicintrenable(v);
	if(vno == -1){
		iunlock(&vctllock);
		print("vintrenable: %s: couldn't enable irq %d, %T\n",
			v->name, v->irq, v->tbdf);
		free(v);
		return nil;
	}
	if((old = vctl[v->affinity][vno]) != nil){
		if(old->isr != v->isr || old->eoi != v->eoi)
			panic("vintrenable: handler: %s %s %#p %#p %#p %#p",
				old->name, v->name, old->isr, v->isr, old->eoi, v->eoi);
		if(old->f == v->f && old->a == v->a){
			iprint("vintrenable: duplicate intr fn %#p data %#p", v->f, v->a);
		}
	}
	v->vno = vno;
	v->next = vctl[v->affinity][vno];
	vctl[v->affinity][vno] = v;

	iunlock(&vctllock);

	if(v->mask)
		v->mask(v, 0);
	return v;
}

void*
intrenable(int irq, void (*f)(Ureg*, void*), void* a, int tbdf, char *name)
{
	Vctl *v;

	v = malloc(sizeof(Vctl));
	vctlinit(v);
	v->isintr = 1;
	v->irq = irq;
	v->tbdf = tbdf;
	v->f = f;
	v->a = a;
	if(f == timerintr)
		v->affinity = m->machno;		/* hack! */
	return vintrenable(v, name);
}

void
trapenable(int vno, void (*f)(Ureg*, void*), void* a, char *name)
{
	Vctl *v;

	v = malloc(sizeof(Vctl));
	vctlinit(v);
	v->type = "trap";
	v->tbdf = BUSUNKNOWN;
	v->f = f;
	v->a = a;
	v->affinity = m->machno;
	strncpy(v->name, name, KNAMELEN);
	v->name[KNAMELEN-1] = 0;

	ilock(&vctllock);
	v->next = vctl[m->machno][vno];
	vctl[m->machno][vno] = v;
	iunlock(&vctllock);
}

int
intrdisable(void* vector)
{
	Vctl *v, *x, **ll;

	if(vector == nil)
		return 0;
	ilock(&vctllock);
	v = vector;
	if(v == nil || vctl[v->affinity][v->vno] != v)
		panic("intrdisable: v %#p", v);
	for(ll = vctl[v->affinity]+v->vno; x = *ll; ll = &x->next)
		if(v == x)
			break;
	if(x != v)
		panic("intrdisable: v %#p", v);
	if(v->mask)
		v->mask(v, 1);
	v->f(nil, v->a);
	*ll = v->next;
	if(strcmp(v->type, "ioapic") == 0)
		ioapicintrdisable(v->vno);
	else
		iprint("intrdisable: not disabling %s type %s\n", v->name, v->type);
	iunlock(&vctllock);

	free(v);
	return 0;
}

static long
irqallocread(Chan*, void *vbuf, long n, vlong offset)
{
	char *buf, vnam[11+1], *p, str[2*(11+1)+2*(20+1)+(KNAMELEN+1)+(8+1)+1];
	int m, vno, af;
	long oldn;
	Vctl *v;

	if(n < 0 || offset < 0)
		error(Ebadarg);

	oldn = n;
	buf = vbuf;
	for(vno=0; vno<256; vno++)
	for(af = 0; af < sys->nmach; af++)
	for(v=vctl[af][vno]; v; v=v->next){
		snprint(vnam, sizeof vnam, "%d.%d", vno, v->affinity);
		m = snprint(str, sizeof str, "%11s %11d %20llud %20llud %-*.*s %.*s\n",
			vnam, v->irq, v->count, v->cycles, 8, 8, v->type, KNAMELEN, v->name);
		if(m <= offset)	/* if do not want this, skip entry */
			offset -= m;
		else{
			/* skip offset bytes */
			m -= offset;
			p = str+offset;
			offset = 0;

			/* write at most max(n,m) bytes */
			if(m > n)
				m = n;
			memmove(buf, p, m);
			n -= m;
			buf += m;

			if(n == 0)
				return oldn;
		}
	}
	return oldn - n;
}

void
interruptsummary(void)
{
	char vnam[11+1];
	int vno, af;
	Vctl *v;

	for(vno=65; vno<256; vno++)
	for(af = 0; af < sys->nmach; af++)
	for(v=vctl[af][vno]; v; v=v->next){
		snprint(vnam, sizeof vnam, "%d.%d", vno, v->affinity);
		print("%11s %11d %20llud %20llud %-*.*s %.*s\n",
			vnam, v->irq, v->count, v->cycles, 8, 8, v->type, KNAMELEN, v->name);
	}
}

void
trapinit(void)
{
	/*
	 * Need to set BPT interrupt gate - here or in vsvminit?
	 */
	trapenable(VectorBPT, debugbpt, nil, "#BP");
	trapenable(VectorPF, faultamd64, nil, "#PF");
	trapenable(Vector2F, doublefault, nil, "#DF");
	trapenable(Vector15, unexpected, nil, "#15");
	trapenable(IdtIPI, expected, nil, "#IPI");

	if(m->machno == 0){
		nmienable();
		addarchfile("irqalloc", 0444, irqallocread, nil);
	}
}

static char* excname[32] = {
	"#DE",					/* Divide-by-Zero Error */
	"#DB",					/* Debug */
	"#NMI",					/* Non-Maskable-Interrupt */
	"#BP",					/* Breakpoint */
	"#OF",					/* Overflow */
	"#BR",					/* Bound-Range */
	"#UD",					/* Invalid-Opcode */
	"#NM",					/* Device-Not-Available */
	"#DF",					/* Double-Fault */
	"#9 (reserved)",
	"#TS",					/* Invalid-TSS */
	"#NP",					/* Segment-Not-Present */
	"#SS",					/* Stack */
	"#GP",					/* General-Protection */
	"#PF",					/* Page-Fault */
	"#15 (reserved)",
	"#MF",					/* x87 FPE-Pending */
	"#AC",					/* Alignment-Check */
	"#MC",					/* Machine-Check */
	"#XF",					/* SIMD Floating-Point */
	"#VE",					/* virtualization exception */
	"#21 (reserved)",
	"#22 (reserved)",
	"#23 (reserved)",
	"#24 (reserved)",
	"#25 (reserved)",
	"#26 (reserved)",
	"#27 (reserved)",
	"#28 (reserved)",
	"#29 (reserved)",
	"#30 (reserved)",
	"#31 (reserved)",
};

/*
 *  keep interrupt service times and counts
 */
void
intrtime(Vctl *v)
{
	uvlong diff, x;

	x = rdtsc();
	diff = x - m->perf.intrts;
	m->perf.intrts = x;

	m->perf.inintr += diff;
	if(up == nil && m->perf.inidle > diff)
		m->perf.inidle -= diff;
	for(; v != nil; v = v->next){
		v->cycles += diff;
		v->count++;
	}
}

/* go to user space */
void
kexit(Ureg*)
{
 	uvlong t;
	Tos *tos;

	/*
	 * precise time accounting, kernel exit
	 * initialized in exec, sysproc.c
	 */
	tos = (Tos*)(USTKTOP-sizeof(Tos));
	cycles(&t);
	tos->kcycles += t - up->kentry;
	tos->pcycles = up->pcycles;
	tos->pid = up->pid;
	tos->machno = m->machno;	
	/*
	 * The process may change its mach.
	 * Be sure it has the right cyclefreq.
	 */
	tos->cyclefreq = m->cyclefreq;
}

/*
 *  All traps come here.  It is slower to have all traps call trap()
 *  rather than directly vectoring the handler.  However, this avoids a
 *  lot of code duplication and possible bugs.  The only exception is
 *  VectorSYSCALL.
 *  Trap is called with interrupts disabled via interrupt-gates.
 */
void
trap(Ureg* ureg)
{
	int clockintr, vno, user;
	char buf[ERRMAX];
	Proc *oup;
	Vctl *ctl, *v;

	vno = ureg->type;

	m->perf.intrts = rdtsc();
	user = userureg(ureg);
	if(user){
		up->dbgreg = ureg;
		cycles(&up->kentry);
	}

	clockintr = 0;

	if(ctl = vctl[m->machno][vno]){
		if(ctl->isintr){
			m->intr++;
			m->lastintr = ctl->irq;
	
			oup = up;
			up = nil;
			if(ctl->isr)
				ctl->isr(vno);
			for(v = ctl; v != nil; v = v->next){
				if(v->f)
					v->f(ureg, v->a);
			}
			if(ctl->eoi)
				ctl->eoi(vno);
			up = oup;
			intrtime(ctl);
			if(ctl->irq == IrqCLOCK || ctl->irq == IrqTIMER)
				clockintr = 1;
	
			if(up && !clockintr)
				preempted();
		}else{
			if(up)
				up->nqtrap++;
			for(v = ctl; v != nil; v = v->next){
				if(v->f)
					v->f(ureg, v->a);
			}
		}
	}
	else if(vno < nelem(excname) && user){
		spllo();
		snprint(buf, sizeof buf, "sys: trap: %s", excname[vno]);
		postnote(up, 1, buf, NDebug);
	}
	else if(vno >= VectorPIC && vno != VectorSYSCALL){
		static ulong last;
		static uvlong count[MACHMAX];

		/* spurious interrupt. */
		lapiceoi(vno);
		if(sys->ticks - last > 20*HZ){
			last = sys->ticks;
			iprint("cpu%d: spurious interrupt %d, last %d %lld\n",
				m->machno, vno, m->lastintr, count[m->machno]);
		}
		count[m->machno]++;
	//	intrtime(vno);
		if(user)
			kexit(ureg);
		return;
	}
	else{
		if(vno == VectorNMI){
			if(m->machno != 0){
				iprint("cpu%d: nmi pc %#p\n",
					m->machno, ureg->ip);
				ndnr();
			}
		}
		dumpregs(ureg);
		if(vno < nelem(excname))
			panic("%s", excname[vno]);
		panic("unknown trap/intr: %d", vno);
	}
	splhi();

	/* delaysched set because we held a lock or because our quantum ended */
	if(clockintr && up && up->nlocks == 0 && up->delaysched){
		ainc(&sys->preempts);
		sched();
		splhi();
	}

	if(user){
		if(up && up->procctl || up->nnote)
			notify(ureg);
		kexit(ureg);
	}
}

/*
 * Dump general registers.
 */
static void
dumpgpr(Ureg* ureg)
{
	if(up != nil)
		iprint("cpu%d: registers for %s %d\n",
			m->machno, up->text, up->pid);
	else
		iprint("cpu%d: registers for kernel\n", m->machno);

   	iprint("ax\t%#16.16llux   	", ureg->ax);
   	iprint("bx\t%#16.16llux\n", ureg->bx);
   	iprint("cx\t%#16.16llux   	", ureg->cx);
   	iprint("dx\t%#16.16llux\n", ureg->dx);
   	iprint("di\t%#16.16llux   	", ureg->di);
   	iprint("si\t%#16.16llux\n", ureg->si);
   	iprint("bp\t%#16.16llux   	", ureg->bp);
   	iprint("r8\t%#16.16llux\n", ureg->r8);
   	iprint("r9\t%#16.16llux   	", ureg->r9);
   	iprint("r10\t%#16.16llux\n", ureg->r10);
   	iprint("r11\t%#16.16llux   	", ureg->r11);
   	iprint("r12\t%#16.16llux\n", ureg->r12);
   	iprint("r13\t%#16.16llux   	", ureg->r13);
   	iprint("r14\t%#16.16llux\n", ureg->r14);
   	iprint("r15\t%#16.16llux\n", ureg->r15);
	iprint("ds  %#4.4ux   es  %#4.4ux   fs  %#4.4ux   gs  %#4.4ux\n",
		ureg->ds, ureg->es, ureg->fs, ureg->gs);
	iprint("ureg fs\t%#ux	", *(u32int*)&ureg->ds);
	iprint("type\t%#llux	", ureg->type);
	iprint("error\t%#llux\n", ureg->error);
	iprint("pc\t%#llux\n", ureg->ip);
	iprint("cs\t%#llux	", ureg->cs);
	iprint("flags\t%#llux\n", ureg->flags);
	iprint("sp\t%#llux	", ureg->sp);
	iprint("ss\t%#llux\n", ureg->ss);
	iprint("type\t%#llux\n", ureg->type);
	iprint("fs\t%#llux\n", rdmsr(FSbase));
	iprint("gs\t%#llux\n", rdmsr(GSbase));

	iprint("m\t%#16.16p	up\t%#16.16p\n", m, up);
}

void
dumpregs(Ureg* ureg)
{
	dumpgpr(ureg);

	/*
	 * Processor control registers.
	 * If machine check exception, time stamp counter, page size extensions
	 * or enhanced virtual 8086 mode extensions are supported, there is a
	 * CR4. If there is a CR4 and machine check extensions, read the machine
	 * check address and machine check type registers if RDMSR supported.
	 */
	iprint("cr0\t%#16.16llux\n", getcr0());
	iprint("cr2\t%#P\n", getcr2());
	iprint("cr3\t%#16.16llux\n", getcr3());

//	archdumpregs();
}

/*
 * Fill in enough of Ureg to get a stack trace, and call a function.
 * Used by debugging interface rdb.
 */
void
callwithureg(void (*fn)(Ureg*))
{
	Ureg ureg;
	ureg.ip = getcallerpc(&fn);
	ureg.sp = PTR2UINT(&fn);
	fn(&ureg);
}

static void
dumpstackwithureg(Ureg* ureg)
{
	char *s;
	uintptr l, v, i, estack;
	extern ulong etext;

	if((s = getconf("*nodumpstack")) != nil && atoi(s) != 0){
		iprint("dumpstack disabled\n");
		return;
	}
	iprint("dumpstack\n");

	iprint("ktrace 9%s %#p %#p\n", strrchr(conffile, '/')+1, ureg->ip, ureg->sp);
	if(up != nil
//	&& (uintptr)&l >= (uintptr)up->kstack
	&& (uintptr)&l <= (uintptr)up->kstack+KSTACK)
		estack = (uintptr)up->kstack+KSTACK;
	else if((uintptr)&l >= m->stack && (uintptr)&l <= m->stack+MACHSTKSZ)
		estack = m->stack+MACHSTKSZ;
	else{
		if(up != nil)
			iprint("&up->kstack %#p &l %#p\n", up->kstack, &l);
		else
			iprint("&m %#p &l %#p\n", m, &l);
		return;
	}
	i = 0;
	iprint("estackx %#p\n", estack);
	for(l = (uintptr)&l; l < estack; l += sizeof(uintptr)){
		v = *(uintptr*)l;
		if((KTZERO < v && v < (uintptr)&etext)
		|| ((uintptr)&l < v && v < estack) || estack-l < 256){
			iprint("%#16.16p=%#16.16p ", l, v);
			if(i++&1)
				iprint("\n");
		}
	}
	if(i)
		iprint("\n");
}

void
dumpstack(void)
{
	callwithureg(dumpstackwithureg);
}

static void
debugbpt(Ureg* ureg, void*)
{
	char buf[ERRMAX];

	if(up == nil)
		panic("kernel bpt");
	/* restore pc to instruction that caused the trap */
	ureg->ip--;
	snprint(buf, sizeof buf, "sys: breakpoint");
	postnote(up, 1, buf, NDebug);
}

static void
doublefault(Ureg*, void*)
{
	panic("double fault");
}

static void
unexpected(Ureg* ureg, void*)
{
	iprint("unexpected trap %llud; ignoring\n", ureg->type);
}

static void
expected(Ureg*, void*)
{
}

uintptr globalpc[MACHMAX];

static void
faultamd64(Ureg* ureg, void*)
{
	u64int addr;
	int read, user, insyscall;
	char buf[ERRMAX];

	addr = getcr2();
	user = userureg(ureg);
	if(!user && vmapsync(addr) == 0)
		return;

	/*
	 * There must be a user context.
	 * If not, the usual problem is causing a fault during
	 * initialisation before the system is fully up.
	 */
	if(up == nil){
		if(m->online == 0)
			dumpregs(ureg);
		panic("fault with up == nil; pc %#p addr %#p",
			ureg->ip, addr);
	}
	read = !(ureg->error & 2);

	insyscall = up->insyscall;
	up->insyscall = 1;
globalpc[m->machno] = ureg->ip;
	if(fault(addr, read) < 0){

		/*
		 * It is possible to get here with !user if, for example,
		 * a process was in a system call accessing a shared
		 * segment but was preempted by another process which shrunk
		 * or deallocated the shared segment; when the original
		 * process resumes it may fault while in kernel mode.
		 * No need to panic this case, post a note to the process
		 * and unwind the error stack. There must be an error stack
		 * (up->nerrlab != 0) if this is a system call, if not then
		 * the game's a bogey.
		 */
		if(!user && (!insyscall || up->nerrlab == 0))
			panic("fault: %#p pc %#p", addr, ureg->ip);
		print("%ud: %s: %s: fault addr=%#p kpc=%#p\n",
			up->pid, up->text, up->user, addr, ureg->ip);
		snprint(buf, sizeof buf, "sys: trap: fault %s addr=%#p",
			read? "read": "write", addr);
		postnote(up, 1, buf, NDebug);
		if(insyscall)
			error(buf);
	}
	up->insyscall = insyscall;
}

/*
 *  return the userpc the last exception happened at
 */
uintptr
userpc(Ureg* ureg)
{
	if(ureg == nil)
		ureg = up->dbgreg;
	return ureg->ip;
}

/* This routine must save the values of registers the user is not permitted
 * to write from devproc and then restore the saved values before returning.
 * TODO: fix this because the segment registers are wrong for 64-bit mode. 
 */
void
setregisters(Ureg* ureg, char* pureg, char* uva, int n)
{
	u64int cs, flags, ss;
	u16int ds, es, fs, gs;

	ss = ureg->ss;
	flags = ureg->flags;
	cs = ureg->cs;
	gs = ureg->cs;
	fs = ureg->cs;
	es = ureg->cs;
	ds = ureg->cs;
	memmove(pureg, uva, n);
	ureg->ds = ds;
	ureg->es = es;
	ureg->fs = fs;
	ureg->gs = gs;
	ureg->cs = cs;
	ureg->flags = (ureg->flags & 0x00ff) | (flags & 0xff00);
	ureg->ss = ss;
}

/* Give enough context in the ureg to produce a kernel stack for
 * a sleeping process
 */
void
setkernur(Ureg* ureg, Proc* p)
{
	ureg->ip = p->sched.pc;
	ureg->sp = p->sched.sp+BY2SE;
	ureg->r14 = (uvlong)p;
	ureg->r15 = (uvlong)m;		/* well, better than nothing */
}

uintptr
dbgpc(Proc *p)
{
	Ureg *ureg;

	ureg = p->dbgreg;
	if(ureg == 0)
		return 0;

	return ureg->ip;
}
