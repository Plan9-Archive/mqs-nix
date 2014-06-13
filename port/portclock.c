#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "ureg.h"

struct Timers
{
	Lock;
	Timer	*head;
};

static Timers timers[MACHMAX];

static vlong
tadd(Timers *tt, Timer *nt)
{
	vlong when;
	Timer *t, **last;

	/* Called with tt locked */
	assert(nt->tt == nil);
	switch(nt->tmode){
	default:
		panic("timer");
		break;
	case Trelative:
		if(nt->tns <= 0)
			nt->tns = 1;
		nt->twhen = fastticks(nil) + ns2fastticks(nt->tns);
		break;
	case Tperiodic:
		/*
		 * Periodic timers must have a period of at least 100µs.
		 */
		assert(nt->tns >= 100000);
		if(nt->twhen == 0){
			/*
			 * Look for another timer at the
			 * same frequency for combining.
			 */
			for(t = tt->head; t; t = t->tnext){
				if(t->tmode == Tperiodic && t->tns == nt->tns)
					break;
			}
			if(t)
				nt->twhen = t->twhen;
			else
				nt->twhen = fastticks(nil);
		}

		/*
		 * The new time must be in the future.
		 * ns2fastticks() can return 0 if the tod clock
		 * has been adjusted by, e.g. timesync.
		 */
		when = ns2fastticks(nt->tns);
		if(when == 0)
			when = 1;
		nt->twhen += when;
		break;
	}

	for(last = &tt->head; t = *last; last = &t->tnext){
		if(t->twhen > nt->twhen)
			break;
	}
	nt->tnext = *last;
	*last = nt;
	nt->tt = tt;
	if(last == &tt->head)
		return nt->twhen;
	return 0;
}

static vlong
tdel(Timer *dt)
{
	Timer *t, **last;
	Timers *tt;

	tt = dt->tt;
	if(tt == nil)
		return 0;
	for(last = &tt->head; t = *last; last = &t->tnext){
		if(t == dt){
			assert(dt->tt);
			dt->tt = nil;
			*last = t->tnext;
			break;
		}
	}
	if(last == &tt->head && tt->head)
		return tt->head->twhen;
	return 0;
}

/* add or modify a timer */
void
timeradd(Timer *nt)
{
	Timers *tt;
	vlong when;

	/* Must lock Timer struct before Timers struct */
	ilock(nt);
	if(tt = nt->tt){
		ilock(tt);
		tdel(nt);
		iunlock(tt);
	}
	tt = &timers[m->machno];
	ilock(tt);
	when = tadd(tt, nt);
	if(when)
		timerset(when);
	iunlock(tt);
	iunlock(nt);
}


void
timerdel(Timer *dt)
{
	Timers *tt;
	vlong when;

	ilock(dt);
	if(tt = dt->tt){
		ilock(tt);
		when = tdel(dt);
		if(when && tt == &timers[m->machno])
			timerset(tt->head->twhen);
		iunlock(tt);
	}
	iunlock(dt);
}

void
hzclock(Ureg *ur)
{
	uintptr pc;

	m->ticks++;
	if(m->machno == 0)
		sys->ticks = m->ticks;

	pc = userpc(ur);
	if(m->proc)
		m->proc->pc = pc;

	if(m->mmuflush){
		if(m->proc != nil)
			mmuflush();
		m->mmuflush = 0;
	}

	accounttime();
	loadbalance();
	kmapinval();

	if(kproftimer != nil)
		kproftimer(pc);

	if(m->online == 0)
		return;

	if(active.exiting)
		exit(0);

	if(m->machno == 0)
		checkalarms();
	if(m->proc != nil && m->proc->state == Running)
		hzsched();	/* in proc.c */
}

void 
loadbalance(void)
{
	int now;
	static ulong lastloadbal;
	struct Mach *target;

	now = perfticks();
	if((now >= (lastloadbal + BALANCE_FREQ)) && (target = imbalance()) != nil) {
		lastloadbal = now;
		pushproc(target);
	}
}

Mach*
imbalance(void)
{
	int i;
	int total_nrun, total_nrdy;	
	struct Mach *mp;

	/* If this mach is idle, it shouldn't be doing any pushing */
	if(m->load == 0 || (m->sch.nrun + m->sch.nrdy) == 0)
		return nil;

	total_nrun = 0;
	total_nrdy = 0;
	for(i = 0; i < NDIM; i++) {
		total_nrun += m->neighbors[i]->sch.nrun;
		total_nrdy += m->neighbors[i]->sch.nrdy;
	}

	if(total_nrun + total_nrdy < NDIM + 1)
		return nil;
	
	for(i = 0; i < NDIM; i++) {
		mp = m->neighbors[i];

		/* total_nrun + total_nrdy is > NDIM+self and a neighbor is idling */
		if(mp->load == 0 || (mp->sch.nrdy + mp->sch.nrun == 0)) {
			balance_neighbor_idle++;
			return mp; 
		}

		/*  percentage difference formula is as follows:
		 *  | Load A - Load B | / ((Load A + Load B)/2) x 100%
		 *
		 *  if the result of the above is >= IMBALANCE_THRES%, then
		 *  we should load balance. The inequality above was simplified
		 *  to the conditional below.
		 */
		if((200 * abs(m->load - mp->load)) >= 
				(IMBALANCE_THRES * (m->load + mp->load))) {
			if(m->load < mp->load) /* there is a imbalance but I'm the less loaded one */
				return nil;
			balance_load_imbal++;
			return mp;
		}
	}

	/* No neighbor met the threshold, assume are balanced */
	return nil;
}

void
timerintr(Ureg *u, void*)
{
	Timer *t;
	Timers *tt;
	vlong when, now;
	int callhzclock;

	callhzclock = 0;
	tt = &timers[m->machno];
	now = fastticks(nil);
	ilock(tt);
	while(t = tt->head){
		/*
		 * No need to ilock t here: any manipulation of t
		 * requires tdel(t) and this must be done with a
		 * lock to tt held.  We have tt, so the tdel will
		 * wait until we're done
		 */
		when = t->twhen;
		if(when > now){
			timerset(when);
			break;
		}
		tt->head = t->tnext;
		assert(t->tt == tt);
		t->tt = nil;
		iunlock(tt);
		if(t->tf)
			(*t->tf)(u, t);
		else
			callhzclock = 1;
		ilock(tt);
		if(t->tmode == Tperiodic)
			tadd(tt, t);
	}
	iunlock(tt);
	if(callhzclock)
		hzclock(u);
}

void
timersinit(void)
{
	Timer *t;

	/*
	 * T->tf == nil means the HZ clock for this processor.
	 */
	todinit();
	t = malloc(sizeof(*t));
	t->tmode = Tperiodic;
	t->tt = nil;
	t->tns = 1000000000/HZ;
	t->tf = nil;
	timeradd(t);
}

Timer*
addclock0link(void (*f)(void), int ms)
{
	Timer *nt;
	vlong when;

	/* Synchronize to hztimer if ms is 0 */
	nt = malloc(sizeof(Timer));
	if(ms == 0)
		ms = 1000/HZ;
	nt->tns = (vlong)ms*1000000LL;
	nt->tmode = Tperiodic;
	nt->tt = nil;
	nt->tf = (void (*)(Ureg*, Timer*))f;

	ilock(&timers[0]);
	when = tadd(&timers[0], nt);
	if(when)
		timerset(when);
	iunlock(&timers[0]);
	return nt;
}

/*
 *  This tk2ms avoids overflows that the macro version is prone to.
 *  It is a LOT slower so shouldn't be used if you're just converting
 *  a delta.
 */
ulong
tk2ms(ulong ticks)
{
	uvlong t, hz;

	t = ticks;
	hz = HZ;
	t *= 1000L;
	t = t/hz;
	ticks = t;
	return ticks;
}

ulong
ms2tk(ulong ms)
{
	/* avoid overflows at the cost of precision */
	if(ms >= 1000000000/HZ)
		return (ms/1000)*HZ;
	return (ms*HZ+500)/1000;
}
