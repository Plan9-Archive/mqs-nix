#include	<u.h>
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"../port/edf.h"
#include	"errstr.h"
#include	<trace.h>

enum
{
	Scaling		= 2,
	Shortburst	= HZ,		/* 1 second */
	Migratedelay	= 500*1000,	/* 500µs — very little affinity */

	/*
	 * number of schedulers used.
	 * 1 uses just one, which is the behavior of Plan 9.
	 */
	Nsched		= 4,
	LoadCalcFreq    = HZ*3, /* How often to calculate global load */
};
Ref	noteidalloc;

static	Ref	pidalloc;
static	uvlong	affdelay;

/*
 * Many multiprocessor machines are NUMA
 * try to use a different scheduler per color
 */
//Sched run[Nsched];

struct Procalloc procalloc;

extern Proc* psalloc(void);
extern void pshash(Proc*);
extern void psrelease(Proc*);
extern void psunhash(Proc*);

static int reprioritize(Proc*);
static void updatecpu(Proc*);

static void rebalance(void);
static Mach *findmach(void);

int schedsteals = 1;
ulong lastloadavg;

char *statename[Nprocstate] =
{	/* BUG: generate automatically */
	"Dead",
	"Moribund",
	"Ready",
	"Scheding",
	"Running",
	"Queueing",
	"QueueingR",
	"QueueingW",
	"Wakeme",
	"Broken",
	"Stopped",
	"Rendez",
	"Waitrelease",
	"Semdown",
};

/*
void
setmachsched(Mach *mp)
{
	int color;

	color = machcolor(mp);
	if(color < 0){
		iprint("mach%d: unknown color\n", mp->machno);
		color = 0;
	}
//	mp->sch = &run[color%Nsched];
}
*/

Sched*
procsched(Proc *p)
{
	Mach *pm;

	pm = p->mp;
	if(pm == nil)
		pm = m;
	return &pm->sch;
}

/*
 * Always splhi()'ed.
 */
void
schedinit(void)		/* never returns */
{
	Edf *e;

	affdelay = fastticks2ns(Migratedelay);
	if(affdelay == 0)
		affdelay++;

	m->inidle = 1;
	ainc(&m->sch.nmach);

	setlabel(&m->sched);
	if(up) {
		if((e = up->edf) && (e->flags & Admitted))
			edfrecord(up);
		m->proc = nil;
		switch(up->state) {
		case Running:
			ready(up);
		default:
			up->mach = nil;
			updatecpu(up);
			up = nil;
			break;
		case Moribund:
			up->state = Dead;
			edfstop(up);
			if (up->edf)
				free(up->edf);
			up->edf = nil;

			/*
			 * Holding locks from pexit:
			 * 	procalloc
			 *	pga
			 */
			mmurelease(up);
			unlock(&pga);

			psrelease(up);
			up->mach = nil;
			updatecpu(up);

			unlock(&procalloc);
			up = nil;

			break;
		}
	}
	sched();
}

/*
 * Check if up's stack has more than 4*KiB free.
 *
 * warning: this assumes that all processes always use their kstack
 * when in the kernel.  this is not true e.g. fork k10 when using
 * ist stacks.  see ../k10/vsvm.c.
 */
static void
upkstackok(void)
{
	char dummy, *p;

	p = &dummy;
	if(p < up->kstack + 4*KiB || p > up->kstack + KSTACK)
		panic("mach%d: up->stack bounds %s:%s %#p %#p",
			m->machno, up->text, statename[up->state], p, up->kstack);
}

/*
 *  If changing this routine, look also at sleep().  It
 *  contains a copy of the guts of sched().
 */
void
sched(void)
{
	Proc *p;
	Sched *sch;

	sch = &m->sch;
	if(m->ilockdepth)
		panic("mach%d: ilockdepth %d, last lock %#p at %#p, sched called from %#p",
			m->machno,
			m->ilockdepth,
			up? up->lastilock: nil,
			(up && up->lastilock)? lockgetpc(up->lastilock): m->ilockpc,
			getcallerpc(&p+2));

	if(up){
		/*
		 * Delay the sched until the process gives up the locks
		 * it is holding.  This avoids dumb lock loops.
		 * Don't delay if the process is Moribund.
		 * It called sched to die.
		 */
		if(up->nlocks && up->state != Moribund){
			up->delaysched++;
 			sch->delayedscheds++;
			return;
		}
		up->delaysched = 0;

		splhi();

		/* statistics */
		if(up->nqtrap == 0 && up->nqsyscall == 0)
			up->nfullq++;
		m->cs++;

		upkstackok();

		procsave(up);
		if(setlabel(&up->sched)){
			procrestore(up);
			spllo();
			return;
		}
		gotolabel(&m->sched);
	}
	m->inidle = 1;
	p = runproc();
	m->inidle = 0;
	if(!p->edf){
		updatecpu(p);
		p->priority = reprioritize(p);
	}
	if(p != m->readied)
		m->schedticks = m->ticks + HZ/10;
	m->readied = nil;
	up = p;
	up->nqtrap = 0;
	up->nqsyscall = 0;
	up->state = Running;
	up->mach = m;
	m->proc = up;
	mmuswitch(up);

	assert(up->wired == nil || up->wired == MACHP(m->machno));
	gotolabel(&up->sched);
}

int
anyready(void)
{
	return m->sch.runvec;
}

int
anyhigher(void)
{
	return m->sch.runvec & ~((1<<(m->proc->priority+1))-1);
}

/*
 *  here once per clock tick to see if we should resched
 */
void
hzsched(void)
{
	/* once a second, rebalance will reprioritize ready procs
	 *
	 * each mach must now call rebalance(), unlike the previous implementation
	 * where only mach 0 called it.
	 */
	rebalance();

	/* unless preempted, get to run for at least 100ms */
	if(anyhigher()
	|| (!m->proc->fixedpri && tickscmp(m->ticks, m->schedticks) >= 0 && anyready())){
		m->readied = nil;	/* avoid cooperative scheduling */
		m->proc->delaysched++;
	}
}

/*
 *  here at the end of non-clock interrupts to see if we should preempt the
 *  current process.  Returns 1 if preempted, 0 otherwise.
 */
int
preempted(void)
{
	if(m->ilockdepth == 0 && up->nlocks == 0)
	if(up->state == Running)
	if(up->preempted == 0)
	if(anyhigher())
	if(!active.exiting){
		m->readied = nil;	/* avoid cooperative scheduling */
		up->preempted = 1;
		sched();
		splhi();
		up->preempted = 0;
		return 1;
	}
	return 0;
}

/*
 * Update the cpu time average for this particular process,
 * which is about to change from up -> not up or vice versa.
 * p->lastupdate is the last time an updatecpu happened.
 *
 * The cpu time average is a decaying average that lasts
 * about D clock ticks.  D is chosen to be approximately
 * the cpu time of a cpu-intensive "quick job".  A job has to run
 * for approximately D clock ticks before we home in on its
 * actual cpu usage.  Thus if you manage to get in and get out
 * quickly, you won't be penalized during your burst.  Once you
 * start using your share of the cpu for more than about D
 * clock ticks though, your p->cpu hits 1000 (1.0) and you end up
 * below all the other quick jobs.  Interactive tasks, because
 * they basically always use less than their fair share of cpu,
 * will be rewarded.
 *
 * If the process has not been running, then we want to
 * apply the filter
 *
 *	cpu = cpu * (D-1)/D
 *
 * n times, yielding
 *
 *	cpu = cpu * ((D-1)/D)^n
 *
 * but D is big enough that this is approximately
 *
 * 	cpu = cpu * (D-n)/D
 *
 * so we use that instead.
 *
 * If the process has been running, we apply the filter to
 * 1 - cpu, yielding a similar equation.  Note that cpu is
 * stored in fixed point (* 1000).
 *
 * Updatecpu must be called before changing up, in order
 * to maintain accurate cpu usage statistics.  It can be called
 * at any time to bring the stats for a given proc up-to-date.
 */
static void
updatecpu(Proc *p)
{
	int D, n, t, ocpu;

	if(p->edf)
		return;

	t = sys->ticks*Scaling + Scaling/2;
	n = t - p->lastupdate;
	p->lastupdate = t;

	if(n == 0)
		return;
	if(&m->sch == nil)	/* may happen during boot */
		return;
//	D = m->sch.schedgain*HZ*Scaling;
	D = Shortburst*Scaling;
	if(n > D)
		n = D;

	ocpu = p->cpu;
	if(p != up)
		p->cpu = (ocpu*(D-n))/D;
	else{
		t = 1000 - ocpu;
		t = (t*(D-n))/D;
		p->cpu = 1000 - t;
	}

//iprint("pid %d %s for %d cpu %d -> %d\n", p->pid, p==up? "active": "inactive", n, ocpu, p->cpu);
}

/*
 * On average, p has used p->cpu of a cpu recently.
 * Its fair share is sys->nmach/m->load of a cpu.  If it has been getting
 * too much, penalize it.  If it has been getting not enough, reward it.
 * I don't think you can get much more than your fair share that
 * often, so most of the queues are for using less.  Having a priority
 * of 3 means you're just right.  Having a higher priority (up to p->basepri)
 * means you're not using as much as you could.
 */
static int
reprioritize(Proc *p)
{
	int fairshare, n, load, ratio;

	load = sys->load;
	if(load == 0)
		return p->basepri;

	/*
	 *  fairshare = 1.000 * sys->nmach * 1.000/load,
	 * except the decimal point is moved three places
	 * on both load and fairshare.
	 */
	fairshare = (sys->nmach*1000*1000)/load;
	n = p->cpu;
	if(n == 0)
		n = 1;
	ratio = (fairshare+n/2) / n;
	if(ratio > p->basepri)
		ratio = p->basepri;
	if(ratio < 0)
		panic("reprioritize");
//iprint("pid %d cpu %d load %d fair %d pri %d\n", p->pid, p->cpu, load, fairshare, ratio);
	return ratio;
}

/* called in clock intr ctx */
void
pushproc(Mach *target)
{
	Sched *srcsch;
	Sched *dstsch;
	Schedq *dstrq, *rq;
	Proc *p;
	int pri;

	srcsch = &m->sch;
	dstsch = &target->sch; 

	/* then we shouldn't be here in the first place, nothing to push */
	if(m->runvec == 0)
		return;

	lock(srcsch);

	/* Find a process to push */
	for(rq = &srcsch->runq[Nrq-1]; rq >= srcsch->runq; rq--){
		if ((p = rq->head) == nil)
			continue;
		if(p->mp == nil || p->mp == m
				|| p->wired == nil && fastticks2ns(fastticks(nil) - p->readytime) >= Migratedelay) {
			splhi();
			/* dequeue the first (head) process of this rq */
			if(p->rnext == nil)
				rq->tail = nil;
			rq->head = p->rnext;
			if(rq->head == nil)
				srcsch->runvec &= ~(1<<(rq-srcsch->runq));
			rq->n--;
			srcsch->nrdy--;
			if(p->state != Ready)
				iprint("pushproc %s %d %s\n", p->text, p->pid, statename[p->state]);
			break;
		}
	}
	unlock(srcsch);
	
	if(p == nil)
		return;
	
	/* We have our proc, stick it in the target runqueue 
	 * will have to:
	 * force the target mach to schedule() 
	 * reprioritize it? The next hzclock will do this in rebalance()
	 */
	lock(dstsch);
	pri = reprioritize(p);
	p->priority = pri;
	dstrq = &dstsch->runq[pri];
	p->readytime = fastticks(nil);
	p->state = Ready;

	/* guts of queueproc without the initial lock(sch) */
	p->rnext = 0;
	if(dstrq->tail)
		dstrq->tail->rnext = p;
	else
		dstrq->head = p;
	dstrq->tail = p;
	dstrq->n++;
	dstsch->nrdy++;
	dstsch->runvec |= 1<<pri;

	unlock(dstsch);
}

/*
 * add a process to a scheduling queue
 */
static void
queueproc(Sched *sch, Schedq *rq, Proc *p)
{
	int pri;

	pri = rq - sch->runq;
	lock(sch);
	p->priority = pri;
	p->rnext = 0;
	if(rq->tail)
		rq->tail->rnext = p;
	else
		rq->head = p;
	rq->tail = p;
	rq->n++;
	sch->nrdy++;
	sch->runvec |= 1<<pri;
	unlock(sch);
}

/*
 *  try to remove target process tp from 
 *  scheduling queue rq (called splhi)
 */
Proc*
dequeueproc(Sched *sch, Schedq *rq, Proc *tp)
{
	Proc *l, *p;
/*
	if(!canlock(sch))
		return nil;
*/
	lock(sch);

	/*
	 *  the queue may have changed before we locked runq,
	 *  refind the target process.
	 */
	l = 0;
	for(p = rq->head; p; p = p->rnext){
		if(p == tp)
			break;
		l = p;
	}

	/*
	 *  p->mach == nil only when process state is saved
	 */
	if(p == nil || !procsaved(p)){
		unlock(sch);
		return nil;
	}
	if(p->rnext == nil)
		rq->tail = l;
	if(l)
		l->rnext = p->rnext;
	else
		rq->head = p->rnext;
	if(rq->head == nil)
		sch->runvec &= ~(1<<(rq-sch->runq));
	rq->n--;
	sch->nrdy--;
	if(p->state != Ready)
		iprint("dequeueproc %s %d %s\n", p->text, p->pid, statename[p->state]);

	unlock(sch);
	return p;
}

static void
schedready(Sched *sch, Proc *p)
{
	Mpl pl;
	int pri;
	Schedq *rq;

	pl = splhi();
	if(edfready(p)){
		splx(pl);
		return;
	}

	/*
	 * BUG: if schedready is called to rebalance the scheduler,
	 * for another core, then this is wrong.
	 */
	if(m->proc != p && (p->wired == nil || p->wired == MACHP(m->machno)))
		m->readied = p;	/* group scheduling */

	updatecpu(p);
	pri = reprioritize(p);
	p->priority = pri;
	rq = &sch->runq[pri];
	p->readytime = fastticks(nil);
	p->state = Ready;
	queueproc(sch, rq, p);
	if(p->trace)
		proctrace(p, SReady, 0);
	splx(pl);
}

/*
 *  ready(p) picks a new priority for a process and sticks it in the
 *  runq for that priority.
 */
void
ready(Proc *p)
{
	schedready(procsched(p), p);
}

/*
 * choose least loaded runqueue for newly forked process
 */
void
forkready(Proc *p)
{
	if (p->wired != nil) {
		/* procsched will return p->mp, which was set to 
		 * p->wired upon proc creation 
		 */
		ready(p);
	} else{
		Mach *laziest;

		laziest = findmach();
		schedready(&laziest->sch, p);
	}
}
/*
 *  yield the processor and drop our priority
 */
void
yield(void)
{
	if(anyready()){
		/* pretend we just used 1/2 tick */
		up->lastupdate -= Scaling/2;
		sched();
	}
}

/*
 *  recalculate priorities once a second.  We need to do this
 *  since priorities will otherwise only be recalculated when
 *  the running process blocks.
 */
static void
rebalance(void)
{
	Mpl pl;
	int pri, npri, t;
	Sched *sch;
	Schedq *rq;
	Proc *p;

	sch = &m->sch;
	t = m->ticks;
	if(t - sch->balancetime < HZ)
		return;
	sch->balancetime = t;

	for(pri=0, rq=sch->runq; pri<Npriq; pri++, rq++){
another:
		p = rq->head;
		if(p == nil)
			continue;
		if(p->mp != m)
			continue;
		if(pri == p->basepri)
			continue;
		updatecpu(p);
		npri = reprioritize(p);
		if(npri != pri){
			pl = splhi();
			p = dequeueproc(sch, rq, p);
			if(p != nil)
				queueproc(sch, &sch->runq[npri], p);
			splx(pl);
			goto another;
		}
	}
}

#define softaffinity(p)	((p)->mp == nil || (p)->mp == m)
#define hardaffinity(p)	((p)->wired == nil || (p)->wired == MACHP(m->machno))

/*
 *  pick a process to run
 */
Proc*
runproc(void)
{
	Schedq *rq;
	Sched *sch;
	Proc *p, *l;
	uvlong start, now;
	int i, skip;

	skip = 0;
	start = perfticks();
	sch = &m->sch;
	/* cooperative scheduling until the clock ticks */
	if((p=m->readied) != nil && procsaved(p) && p->state == Ready
	&& (sch->runvec & (1<<Nrq-1 | 1<< Nrq-2)) == 0
	&& hardaffinity(p)){
		sch->skipscheds++;
		skip = 1;
		rq = &sch->runq[p->priority];
		goto skipsched;
	}

	sch->preempts++;

loop:
	spllo();
	for(i = 0; ; i++){
		for(rq = &sch->runq[Nrq-1]; rq >= sch->runq; rq--){
			if ((p = rq->head) == nil)
				continue;
/*			if(p->mp == nil || p->mp == m
					|| p->wired == nil && fastticks2ns(fastticks(nil) - p->readytime) >= Migratedelay) { */
				splhi();
				lock(sch);
				/* dequeue the first (head) process of this rq */
				if(p->rnext == nil)
					rq->tail = nil;
				rq->head = p->rnext;
				if(rq->head == nil)
					sch->runvec &= ~(1<<(rq-sch->runq));
				rq->n--;
				sch->nrdy--;
				if(p->state != Ready)
					iprint("dequeueproc %s %d %s\n", p->text, p->pid, statename[p->state]);
	if(sch->rqn == 0) 
		sch->rqn = 1;
	sch->readytimeavg = ((sch->readytimeavg * sch->rqn) + (fastticks(nil) - p->readytime)) / sch->rqn++; 
				unlock(sch);
				goto found;
		}

		while(monmwait((int*)&sch->runvec, 0) == 0)
			;

		/* remember how much time we're here */
		now = perfticks();
		m->perf.inidle += now-start;
		start = now;
	}

skipsched:
	splhi();
	p = dequeueproc(sch, rq, p);
	if(p == nil){
		if(skip){
			skip = 0;
			sch->skipscheds--;
		}
		sch->loop++;
		goto loop;
	}

//stolen:
found:
	p->state = Scheding;
	p->mp = m;

	if(edflock(p)){
		edfrun(p, rq == &sch->runq[PriEdf]);	/* start deadline timer and do admin */
		edfunlock();
	}
	if(p->trace)
		proctrace(p, SRun, 0);
	return p;
}

int
canpage(Proc *p)
{
	int ok;
	Sched *sch;
	Mpl pl;

	pl = splhi();
	sch = procsched(p);
	lock(sch);
	/* Only reliable way to see if we are Running */
	if(procsaved(p)){
		p->newtlb = 1;
		ok = 1;
	}
	else
		ok = 0;
	unlock(sch);
	splx(pl);

	return ok;
}

Proc*
newproc(void)
{
	Proc *p;

	p = psalloc();

	p->state = Scheding;
	p->psstate = "New";
	p->mach = nil;
	p->qnext = nil;
	p->nchild = 0;
	p->nwait = 0;
	p->waitq = nil;
	p->parent = nil;
	p->pgrp = nil;
	p->egrp = nil;
	p->fgrp = nil;
	p->rgrp = nil;
	p->pdbg = 0;
	p->kp = 0;
	if(up != nil && up->procctl == Proc_tracesyscall)
		p->procctl = Proc_tracesyscall;
	else
		p->procctl = 0;
	p->syscalltrace = nil;
	p->notepending = 0;
	p->nnote = 0;
	p->ureg = nil;
	p->privatemem = 0;
	p->noswap = 0;
	p->errstr = p->errbuf0;
	p->syserrstr = p->errbuf1;
	p->errbuf0[0] = '\0';
	p->errbuf1[0] = '\0';
	p->nlocks = 0;
	p->delaysched = 0;
	p->trace = 0;
	kstrdup(&p->user, "*nouser");
	kstrdup(&p->text, "*notext");
	kstrdup(&p->args, "");
	p->nargs = 0;
	p->setargs = 0;
	memset(p->seg, 0, sizeof p->seg);
	p->pid = incref(&pidalloc);
	pshash(p);
	p->noteid = incref(&noteidalloc);
	if(p->pid <= 0 || p->noteid <= 0)
		panic("pidalloc");
	if(p->kstack == nil)
		p->kstack = smalloc(KSTACK);

	/* sched params */
	p->mp = nil;
	p->wired = nil;
	procpriority(p, PriNormal, 0);
	p->cpu = 0;
	p->lastupdate = sys->ticks*Scaling;
	p->edf = nil;

	p->ntrap = 0;
	p->nintr = 0;
	p->nsyscall = 0;
	p->nfullq = 0;
	memset(&p->PMMU, 0, sizeof p->PMMU);
	return p;
}

/*
 * wire this proc to a machine
 */
void
procwired(Proc *p, int bm)
{
	Proc *pp;
	int i;
	char nwired[MACHMAX];
	Mach *wm;

	if(bm < 0){
		/* pick a machine to wire to */
		memset(nwired, 0, sizeof(nwired));
		for(i=0; (pp = psincref(i)) != nil; i++){
			wm = pp->wired;
			if(wm && pp->pid && pp != p)
				nwired[wm->machno]++;
			psdecref(pp);
		}
		bm = 0;
		for(i=0; i<sys->nmach; i++)
			if(nwired[i] < nwired[bm])
				bm = i;
	} else {
		/* use the virtual machine requested */
		bm = bm % sys->nmach;
	}

	p->wired = sys->machptr[bm];
	p->mp = p->wired;

	/*
	 * adjust our color to the new domain.
	 */
	if(up == nil || p != up)
		return;
	up->color = machcolor(up->mp);
	qlock(&up->seglock);
	for(i = 0; i < NSEG; i++)
		if(up->seg[i])
			up->seg[i]->color = up->color;
	qunlock(&up->seglock);

	if(m->machno != bm)
		sched();
	if(m->machno != bm)
		panic("procwired: %d != %d", m->machno, bm);
}

void
procpriority(Proc *p, int pri, int fixed)
{
	if(pri >= Npriq)
		pri = Npriq - 1;
	else if(pri < 0)
		pri = 0;
	p->basepri = pri;
	p->priority = pri;
	p->fixedpri = fixed;
}

/*
 *  sleep if a condition is not true.  Another process will
 *  awaken us after it sets the condition.  When we awaken
 *  the condition may no longer be true.
 *
 *  we lock both the process and the rendezvous to keep r->p
 *  and p->r synchronized.
 */
void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	Mpl pl;

	pl = splhi();

	if(up->nlocks)
		print("process %d sleeps with %d locks held, last lock %#p locked at pc %#p, sleep called from %#p\n",
			up->pid, up->nlocks, up->lastlock, lockgetpc(up->lastlock), getcallerpc(&r));
	lock(r);
	lock(&up->rlock);
	if(r->p){
		print("double sleep called from %#p, %d %d\n",
			getcallerpc(&r), r->p->pid, up->pid);
		dumpstack();
	}

	/*
	 *  Wakeup only knows there may be something to do by testing
	 *  r->p in order to get something to lock on.
	 *  Flush that information out to memory in case the sleep is
	 *  committed.
	 */
	r->p = up;

	if((*f)(arg) || up->notepending){
		/*
		 *  if condition happened or a note is pending
		 *  never mind
		 */
		r->p = nil;
		unlock(&up->rlock);
		unlock(r);
	} else {
		/*
		 *  now we are committed to
		 *  change state and call scheduler
		 */
		if(up->trace)
			proctrace(up, SSleep, 0);
		up->state = Wakeme;
		up->r = r;

		/* statistics */
		m->cs++;

		procsave(up);
		if(setlabel(&up->sched)) {
			/*
			 *  here when the process is awakened
			 */
			procrestore(up);
			spllo();
		} else {
			/*
			 *  here to go to sleep (i.e. stop Running)
			 */
			unlock(&up->rlock);
			unlock(r);
			gotolabel(&m->sched);
		}
	}

	if(up->notepending) {
		up->notepending = 0;
		splx(pl);
		if(up->procctl == Proc_exitme && up->closingfgrp)
			forceclosefgrp();
		error(Eintr);
	}

	splx(pl);
}

static int
tfn(void *arg)
{
	return up->trend == nil || up->tfn(arg);
}

void
twakeup(Ureg*, Timer *t)
{
	Proc *p;
	Rendez *trend;

	p = t->ta;
	trend = p->trend;
	p->trend = 0;
	if(trend)
		wakeup(trend);
}

void
tsleep(Rendez *r, int (*fn)(void*), void *arg, long ms)
{
	if (up->tt){
		print("tsleep: timer active: mode %d, tf %#p\n",
			up->tmode, up->tf);
		timerdel(up);
	}
	up->tns = MS2NS(ms);
	up->tf = twakeup;
	up->tmode = Trelative;
	up->ta = up;
	up->trend = r;
	up->tfn = fn;
	timeradd(up);

	if(waserror()){
		timerdel(up);
		nexterror();
	}
	sleep(r, tfn, arg);
	if (up->tt)
		timerdel(up);
	up->twhen = 0;
	poperror();
}

/*
 *  Expects that only one process can call wakeup for any given Rendez.
 *  We hold both locks to ensure that r->p and p->r remain consistent.
 *  Richard Miller has a better solution that doesn't require both to
 *  be held simultaneously, but I'm a paranoid - presotto.
 */
Proc*
wakeup(Rendez *r)
{
	Mpl pl;
	Proc *p;

	pl = splhi();

	lock(r);
	p = r->p;

	if(p != nil){
		lock(&p->rlock);
		if(p->state != Wakeme || p->r != r)
			panic("wakeup: state");
		r->p = nil;
		p->r = nil;
		ready(p);
		unlock(&p->rlock);
	}
	unlock(r);

	splx(pl);

	return p;
}

/*
 *  if waking a sleeping process, this routine must hold both
 *  p->rlock and r->lock.  However, it can't know them in
 *  the same order as wakeup causing a possible lock ordering
 *  deadlock.  We break the deadlock by giving up the p->rlock
 *  lock if we can't get the r->lock and retrying.
 */
int
postnote(Proc *p, int dolock, char *n, int flag)
{
	Mpl pl;
	int ret;
	Rendez *r;
	Proc *d, **l;

	if(dolock)
		qlock(&p->debug);

	if(flag != NUser && (p->notify == nil || p->notified))
		p->nnote = 0;

	ret = 0;
	if(p->nnote < NNOTE) {
		strcpy(p->note[p->nnote].msg, n);
		p->note[p->nnote++].flag = flag;
		ret = 1;
	}
	p->notepending = 1;

	if(dolock)
		qunlock(&p->debug);

	/* this loop is to avoid lock ordering problems. */
	for(;;){
		pl = splhi();
		lock(&p->rlock);
		r = p->r;

		/* waiting for a wakeup? */
		if(r == nil)
			break;	/* no */

		/* try for the second lock */
		if(canlock(r)){
			if(p->state != Wakeme || r->p != p)
				panic("postnote: state %d %d %d", r->p != p, p->r != r, p->state);
			p->r = nil;
			r->p = nil;
			ready(p);
			unlock(r);
			break;
		}

		/* give other process time to get out of critical section and try again */
		unlock(&p->rlock);
		splx(pl);
		sched();
	}
	unlock(&p->rlock);
	splx(pl);

	if(p->state != Rendezvous){
		if(p->state == Semdown)
			ready(p);
		return ret;
	}
	/* Try and pull out of a rendezvous */
	lock(p->rgrp);
	if(p->state == Rendezvous) {
		p->rendval = ~0;
		l = &REND(p->rgrp, p->rendtag);
		for(d = *l; d; d = d->rendhash) {
			if(d == p) {
				*l = p->rendhash;
				break;
			}
			l = &d->rendhash;
		}
		ready(p);
	}
	unlock(p->rgrp);
	return ret;
}

/*
 * weird thing: keep at most NBROKEN around
 */
#define	NBROKEN 4
struct
{
	QLock;
	int	n;
	Proc	*p[NBROKEN];
}broken;

void
addbroken(Proc *p)
{
	qlock(&broken);
	if(broken.n == NBROKEN) {
		ready(broken.p[0]);
		memmove(&broken.p[0], &broken.p[1], sizeof(Proc*)*(NBROKEN-1));
		--broken.n;
	}
	broken.p[broken.n++] = p;
	qunlock(&broken);

	edfstop(up);
	p->state = Broken;
	p->psstate = 0;
	sched();
}

void
unbreak(Proc *p)
{
	int b;

	qlock(&broken);
	for(b=0; b < broken.n; b++)
		if(broken.p[b] == p) {
			broken.n--;
			memmove(&broken.p[b], &broken.p[b+1],
					sizeof(Proc*)*(NBROKEN-(b+1)));
			ready(p);
			break;
		}
	qunlock(&broken);
}

int
freebroken(void)
{
	int i, n;

	qlock(&broken);
	n = broken.n;
	for(i=0; i<n; i++) {
		ready(broken.p[i]);
		broken.p[i] = 0;
	}
	broken.n = 0;
	qunlock(&broken);
	return n;
}

void
pexit(char *exitstr, int freemem)
{
	Proc *p;
	Segment **s, **es;
	Waitq *wq, *f, *next;
	Fgrp *fgrp;
	Egrp *egrp;
	Rgrp *rgrp;
	Pgrp *pgrp;

	Chan *dot;
	if(0 && up->nfullq > 0)
		iprint(" %s=%d", up->text, up->nfullq);
	free(up->syscalltrace);
	up->syscalltrace = nil;

	if(up->alarm != 0)
		procalarm(0);
	if(up->tt)
		timerdel(up);
	if(up->trace)
		proctrace(up, SDead, 0);

	/* nil out all the resources under lock (free later) */
	qlock(&up->debug);
	fgrp = up->fgrp;
	up->fgrp = nil;
	egrp = up->egrp;
	up->egrp = nil;
	rgrp = up->rgrp;
	up->rgrp = nil;
	pgrp = up->pgrp;
	up->pgrp = nil;
	dot = up->dot;
	up->dot = nil;
	qunlock(&up->debug);

	if(fgrp)
		closefgrp(fgrp);
	if(egrp)
		closeegrp(egrp);
	if(rgrp)
		closergrp(rgrp);
	if(dot)
		cclose(dot);
	if(pgrp)
		closepgrp(pgrp);

	/*
	 * if not a kernel process and have a parent,
	 * do some housekeeping.
	 */
	if(up->kp == 0) {
		p = up->parent;
		if(p == nil) {
			if(exitstr == nil)
				exitstr = "unknown";
			panic("boot process died: %s", exitstr);
		}

		while(waserror())
			;

		wq = smalloc(sizeof(Waitq));
		poperror();

		wq->w.pid = up->pid;
		wq->w.time[TUser] = tk2ms(up->time[TUser]);
		wq->w.time[TSys] = tk2ms(up->time[TSys]);
		wq->w.time[TReal] = tk2ms(sys->ticks - up->time[TReal]);
		if(exitstr && exitstr[0])
			snprint(wq->w.msg, sizeof(wq->w.msg), "%s %d: %s",
				up->text, up->pid, exitstr);
		else
			wq->w.msg[0] = '\0';

		lock(&p->exl);
		/*
		 * Check that parent is still alive.
		 */
		if(p->pid == up->parentpid && p->state != Broken) {
			p->nchild--;
			/*
			 * If there would be more than 128 wait records
			 * processes for my parent, then don't leave a wait
			 * record behind.  This helps prevent badly written
			 * daemon processes from accumulating lots of wait
			 * records.
		 	 */
			if(p->nwait < 128) {
				wq->next = p->waitq;
				p->waitq = wq;
				p->nwait++;
				wq = nil;
				wakeup(&p->waitr);
			}
		}
		unlock(&p->exl);
		if(wq)
			free(wq);
	}

	if(!freemem)
		addbroken(up);

	qlock(&up->seglock);
	es = &up->seg[NSEG];
	for(s = up->seg; s < es; s++) {
		if(*s) {
			putseg(*s);
			*s = 0;
		}
	}
	qunlock(&up->seglock);

	lock(&up->exl);		/* Prevent my children from leaving waits */
	psunhash(up);
	up->pid = 0;
	wakeup(&up->waitr);
	unlock(&up->exl);

	for(f = up->waitq; f; f = next) {
		next = f->next;
		free(f);
	}

	/* release debuggers */
	qlock(&up->debug);
	if(up->pdbg) {
		wakeup(&up->pdbg->sleep);
		up->pdbg = 0;
	}
	qunlock(&up->debug);

	/* Sched must not loop for these locks */
	lock(&procalloc);
	lock(&pga);

	edfstop(up);
	up->state = Moribund;
	sched();
	panic("pexit");
}

int
haswaitq(void *x)
{
	Proc *p;

	p = (Proc *)x;
	return p->waitq != 0;
}

int
pwait(Waitmsg *w)
{
	int cpid;
	Waitq *wq;

	if(!canqlock(&up->qwaitr))
		error(Einuse);

	if(waserror()) {
		qunlock(&up->qwaitr);
		nexterror();
	}

	lock(&up->exl);
	if(up->nchild == 0 && up->waitq == nil) {
		unlock(&up->exl);
		error(Enochild);
	}
	unlock(&up->exl);

	sleep(&up->waitr, haswaitq, up);

	lock(&up->exl);
	wq = up->waitq;
	up->waitq = wq->next;
	up->nwait--;
	unlock(&up->exl);

	qunlock(&up->qwaitr);
	poperror();

	if(w)
		memmove(w, &wq->w, sizeof(Waitmsg));
	cpid = wq->w.pid;
	free(wq);

	return cpid;
}

void
dumpaproc(Proc *p)
{
	uintptr bss;
	char *s;

	if(p == nil)
		return;

	bss = 0;
	if(p->seg[HSEG])
		bss = p->seg[HSEG]->top;
	else if(p->seg[BSEG])
		bss = p->seg[BSEG]->top;

	s = p->psstate;
	if(s == nil)
		s = statename[p->state];
	print("%3d:%10s pc %#p dbgpc %#p  %8s (%s) ut %ld st %ld bss %#p qpc %#p nl %d nd %ud lpc %#p pri %ud\n",
		p->pid, p->text, p->pc, dbgpc(p), s, statename[p->state],
		p->time[0], p->time[1], bss, p->qpc, p->nlocks,
		p->delaysched, lockgetpc(p->lastlock), p->priority);
}

void
procdump(void)
{
	int i;
	Proc *p;

	if(up != nil)
		print("up %d\n", up->pid);
	else
		print("no current process\n");
	for(i=0; (p = psincref(i)) != nil; i++) {
		if(p->state != Dead)
			dumpaproc(p);
		psdecref(p);
	}
}

/*
 *  wait till all processes have flushed their mmu
 *  state about segement s
 */
void
procflushseg(Segment *s)
{
	int i, ns, nm, nwait;
	Proc *p;

	/*
	 *  tell all processes with this
	 *  segment to flush their mmu's
	 */
	nwait = 0;
	for(i=0; (p = psincref(i)) != nil; i++) {
		if(p->state == Dead){
			psdecref(p);
			continue;
		}
		for(ns = 0; ns < NSEG; ns++){
			if(p->seg[ns] == s){
				p->newtlb = 1;
				for(nm = 0; nm < sys->nmach; nm++){
					if(sys->machptr[nm]->proc == p){
						sys->machptr[nm]->mmuflush = 1;
						nwait++;
					}
				}
				break;
			}
		}
		psdecref(p);
	}

	if(nwait == 0)
		return;

	/*
	 *  wait for all processors to take a clock interrupt
	 *  and flush their mmu's.
	 */
	for(nm = 0; nm < sys->nmach; nm++)
		if(sys->machptr[nm] != m)
			while(sys->machptr[nm]->mmuflush)
				sched();
}

char*
schedstats(char *s, char *e)
{
	int n;
	Sched *sch;
/*
	for(n = 0; n < Nsched; n++){
		sch = run+n;
		if(sch->nmach == 0)
			continue;
		s = seprint(s, e, "sch%d: nrdy	%d\n", n, sch->nrdy);
		s = seprint(s, e, "sch%d: delayed	%d\n", n, sch->delayedscheds);
		s = seprint(s, e, "sch%d: preempt	%d\n", n, sch->preempts);
		s = seprint(s, e, "sch%d: skip	%d\n", n, sch->skipscheds);
		s = seprint(s, e, "sch%d: loop	%d\n", n, sch->loop);
		s = seprint(s, e, "sch%d: balancet	%ld\n", n, sch->balancetime);
		s = seprint(s, e, "sch%d: runvec	%#b\n", n, sch->runvec);
		s = seprint(s, e, "sch%d: nmach	%d\n", n, sch->nmach);
		s = seprint(s, e, "sch%d: nrun	%d\n", n, sch->nrun);
	}
*/
	return s;
}

void
scheddump(void)
{
	Proc *p;
	Sched *sch;
	Schedq *rq;
	Mach *mp;
	int i;

	/* 
	 * for each cpu's Sched struct, loop over their runq 
	 */
	for(i = 0; i < MACHMAX; i++) {
		mp = sys->machptr[i];
		if (mp == nil || mp->online == 0 || &mp->sch == nil)
			continue;
		sch = &mp->sch;
		print("sch for mach %d: nrdy %d: nrun: %d\n", mp->machno, 
				sch->nrdy, sch->nrun);
		print("\tnmach %d: delayedscheds %d:\tskipscheds %d\n", sch->nmach, 
				sch->delayedscheds, sch->skipscheds);
		print("\tpreempts %d:\tloop %d\n: balancetime %ld\n", sch->preempts, 
				sch->loop, sch->balancetime);

		for(rq = &sch->runq[Nrq-1]; rq >= sch->runq; rq--) {
			if(rq->head == nil)
			    continue;
			print("sch%ld rq%ld n%ld:", sch, rq-sch->runq, rq->n);
			for(p = rq->head; p; p = p->rnext)
				iprint(" %d(%lludµs)", p->pid, fastticks2us(fastticks(nil) - p->readytime));
			iprint("\n");
		}
	}
}

void
kproc(char *name, void (*func)(void *), void *arg)
{
	Proc *p;
	static Pgrp *kpgrp;

	while(waserror())
		;
	p = newproc();
	poperror();

	p->psstate = 0;
	p->procmode = 0640;
	p->kp = 1;
	p->noswap = 1;

	p->scallnr = up->scallnr;
	memmove(p->arg, up->arg, sizeof(up->arg));
	p->nerrlab = 0;
	p->slash = up->slash;
	p->dot = up->dot;
	if(p->dot)
		incref(p->dot);

	memmove(p->note, up->note, sizeof(p->note));
	p->nnote = up->nnote;
	p->notified = 0;
	p->lastnote = up->lastnote;
	p->notify = up->notify;
	p->ureg = nil;
	p->dbgreg = nil;

	procpriority(p, PriKproc, 0);

	kprocchild(p, func, arg);

	kstrdup(&p->user, eve);
	kstrdup(&p->text, name);
	if(kpgrp == nil)
		kpgrp = newpgrp();
	p->pgrp = kpgrp;
	incref(kpgrp);

	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = sys->ticks;
	ready(p);
}

/*
 *  called splhi() by notify().  See comment in notify for the
 *  reasoning.
 */
void
procctl(Proc *p)
{
	Mpl pl;
	char *state;

	switch(p->procctl) {
	case Proc_exitbig:
		spllo();
		pexit("Killed: Insufficient physical memory", 1);

	case Proc_exitme:
		spllo();		/* pexit has locks in it */
		pexit("Killed", 1);

	case Proc_traceme:
		if(p->nnote == 0)
			return;
		/* No break */

	case Proc_stopme:
		p->procctl = 0;
		state = p->psstate;
		p->psstate = "Stopped";
		/* free a waiting debugger */
		pl = spllo();
		qlock(&p->debug);
		if(p->pdbg) {
			wakeup(&p->pdbg->sleep);
			p->pdbg = 0;
		}
		qunlock(&p->debug);
		splhi();
		p->state = Stopped;
		sched();
		p->psstate = state;
		splx(pl);
		return;
	}
}

void
error(char *err)
{
	spllo();

	assert(up->nerrlab < NERR);
	kstrcpy(up->errstr, err, ERRMAX);
	setlabel(&up->errlab[NERR-1]);
	nexterror();
}

void
nexterror(void)
{
	assert(up->nerrlab > 0);
	gotolabel(&up->errlab[--up->nerrlab]);
}

void
exhausted(char *resource)
{
	char buf[ERRMAX];

	snprint(buf, sizeof buf, "no free %s", resource);
	iprint("%s\n", buf);
	error(buf);
}

void
killbig(char *why)
{
	int i, x;
	Segment *s;
	uintptr l, max;
	Proc *p, *kp;

	max = 0;
	kp = nil;
	for(x = 0; (p = psincref(x)) != nil; x++) {
		if(p->state == Dead || p->kp){
			psdecref(p);
			continue;
		}
		l = 0;
		for(i=1; i<NSEG; i++) {
			s = p->seg[i];
			if(s != 0)
				l += s->top - s->base;
		}
		if(l > max && ((p->procmode&0222) || strcmp(eve, p->user)!=0)) {
			if(kp != nil)
				psdecref(kp);
			kp = p;
			max = l;
		}
		else
			psdecref(p);
	}
	if(kp == nil)
		return;

	print("%d: %s killed: %s\n", kp->pid, kp->text, why);
	for(x = 0; (p = psincref(x)) != nil; x++) {
		if(p->state == Dead || p->kp){
			psdecref(p);
			continue;
		}
		if(p != kp && p->seg[BSEG] && p->seg[BSEG] == kp->seg[BSEG])
			p->procctl = Proc_exitbig;
		psdecref(p);
	}

	kp->procctl = Proc_exitbig;
	for(i = 0; i < NSEG; i++) {
		s = kp->seg[i];
		if(s != 0 && canqlock(&s->lk)) {
			mfreeseg(s, s->base, s->top);
			qunlock(&s->lk);
		}
	}
	psdecref(kp);
}

/*
 *  change ownership to 'new' of all processes owned by 'old'.  Used when
 *  eve changes.
 */
void
renameuser(char *old, char *new)
{
	int i;
	Proc *p;

	for(i = 0; (p = psincref(i)) != nil; i++){
		if(p->user!=nil && strcmp(old, p->user)==0)
			kstrdup(&p->user, new);
		psdecref(p);
	}
}

/*
 *  time accounting called by clock() splhi'd
 *  load is calculated per-mach
 */
void
accounttime(void)
{
	Sched *sch;
	Proc *p;
	uvlong n, per;
	ulong now;
	int i, globalnrdy, globalnrun;
	Mach *mach;

	sch = &m->sch;
	p = m->proc;
	if(p != nil){
		sch->nrun++;
		p->time[p->insyscall]++;
	}

	/* calculate decaying duty cycles */
	n = perfticks();
	per = n - m->perf.last;
	m->perf.last = n;
	per = (m->perf.period*(HZ-1) + per)/HZ;
	if(per != 0)
		m->perf.period = per;

	m->perf.avg_inidle = (m->perf.avg_inidle*(HZ-1)+m->perf.inidle)/HZ;
	m->perf.inidle = 0;

	m->perf.avg_inintr = (m->perf.avg_inintr*(HZ-1)+m->perf.inintr)/HZ;
	m->perf.inintr = 0;

	/*
	 * calculate decaying load average.
	 * if we decay by (n-1)/n then it takes
	 * n clock ticks to go from load L to .36 L once
	 * things quiet down.  it takes about 5 n clock
	 * ticks to go to zero.  so using HZ means this is
	 * approximately the load over the last second,
	 * with a tail lasting about 5 seconds.
	 */
	n = sch->nrun;
	sch->nrun = 0;
	n = (sch->nrdy+n)*1000;
	m->load = (m->load*(HZ-1)+n)/HZ;

	/* Global load calculation */
	if (m->machno != 0)
		return;

	now = perfticks();
	if(now >= (lastloadavg+LoadCalcFreq)) {
		lastloadavg = now;
		for(i = 0; i < sys->nmach; i++) {
			mach = sys->machptr[i];
			globalnrdy += mach->sch.nrdy;
			globalnrun += mach->sch.nrun;
		}
		n = globalnrun;
		globalnrun = 0;
		n = (globalnrdy+n)*1000;
		sys->load = (sys->load*(HZ-1)+n)/HZ;
	}
}

Mach* 
findmach(void)
{
	int i, min_load;
	Mach *mp, *laziest;

	min_load = m->load;
	laziest = m;

	if(min_load == 0 || m->runvec == 0)
		return laziest;

	for(i = 0; i < NDIM; i++) {
		if((mp = m->neighbors[i])->load == 0)
			laziest = mp;

		if((mp->runvec == 0))
			laziest = mp;
		/*if((mp = m->neighbors[i])->load < min_load)
			laziest = mp;*/
	}

	return laziest;
}


void
halt(void)
{
	if(m->sch.nrdy != 0)
		return;
	hardhalt();
}
