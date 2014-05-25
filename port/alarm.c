#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

static Alarms	alarms;
static Rendez	alarmr;

void
alarmkproc(void*)
{
	Proc *rp;
	ulong now;

	for(;;){
		now = sys->ticks;
		qlock(&alarms);
		for(rp = alarms.head; rp != nil; rp = rp->palarm){
			if(rp->alarm == 0)
				continue;
			if((long)(now - rp->alarm) < 0)
				break;
			if(canqlock(&rp->debug))
				break;
			if(rp->alarm != 0)
				postnote(rp, 0, "alarm", NUser);
			rp->alarm = 0;
			qunlock(&rp->debug);
		}
		alarms.head = rp;
		qunlock(&alarms);

		sleep(&alarmr, return0, 0);
	}
}

/*
 *  called every clock tick
 */
void
checkalarms(void)
{
	Proc *p;
	ulong now;

	p = alarms.head;
	now = sys->ticks;

	if(p != nil)
	if(p->alarm == 0 || (long)(now - p->alarm) >= 0)
		wakeup(&alarmr);
}

ulong
procalarm(ulong time)
{
	Proc **l, *f;
	ulong when, old;

	if(up->alarm)
		old = tk2ms(up->alarm - sys->ticks);
	else
		old = 0;
	if(time == 0) {
		up->alarm = 0;
		return old;
	}
	when = ms2tk(time)+sys->ticks;
	if(when == 0)
		when = 1;

	qlock(&alarms);
	l = &alarms.head;
	for(f = *l; f; f = f->palarm) {
		if(up == f){
			*l = f->palarm;
			break;
		}
		l = &f->palarm;
	}
	up->palarm = 0;
	l = &alarms.head;
	for(f = *l; f != nil; f = f->palarm) {
		time = f->alarm;
		if(time != 0 && (long)(time - when) >= 0)
			break;
		l = &f->palarm;
	}
	up->palarm = f;
	*l = up;
	up->alarm = when;
	qunlock(&alarms);

	return old;
}
