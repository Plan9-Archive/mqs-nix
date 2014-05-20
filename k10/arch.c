/*
 * EPISODE 12B
 * How to recognise different types of trees from quite a long way away.
 * NO. 1
 * THE LARCH
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

void
procrestore(Proc *p)
{
	uvlong t;

	if(p->kp)
		return;
	cycles(&t);
	p->pcycles -= t;

	fpuprocrestore(p);
}

/*
 *  Save the mach dependent part of the process state.
 *  NB: the caller should mmuflushtlb after procsave().
 *  procsave/procrestore don't touch the mmu, they
 *  care about fpu, mostly.
 */
void
procsave(Proc *p)
{
	uvlong t;

	fpuprocsave(p);
	mmuflushtlb(m->pml4->pa);

	cycles(&t);
	p->pcycles += t;
}

static void
linkproc(void)
{
	spllo();
	up->kpfun(up->kparg);
	pexit("kproc dying", 0);
}

void
kprocchild(Proc* p, void (*func)(void*), void* arg)
{
	/*
	 * gotolabel() needs a word on the stack in
	 * which to place the return PC used to jump
	 * to linkproc().
	 */
	p->sched.pc = PTR2UINT(linkproc);
	p->sched.sp = PTR2UINT(p->kstack+KSTACK-BY2SE);
	p->sched.sp = STACKALIGN(p->sched.sp);

	p->kpfun = func;
	p->kparg = arg;
}

/*
 *  put the processor in the halt state if we've no processes to run.
 *  an interrupt will get us going again.
 */
void
idlehands(void)
{
	if(m->machno != 0)
		halt();
}
