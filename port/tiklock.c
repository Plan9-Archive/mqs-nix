#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/edf.h"

typedef struct Glares Glares;
typedef struct Glaring Glaring;

struct Glaring
{
	Lock*	l;
	ulong	ticks;
	ulong	lastlock;
	uint	key;
	uintptr	lpc;
	uintptr	pc;
	uint	n;
	int	ok;
	int	shown;
};

struct Glares
{
	Glaring	instance[30];
	uint	next;
	uint	total;
};

static	Glares	glares[MACHMAX];	/* per core, to avoid lock */
static	int	lockdebug;
static	uint	spinbackoff = 50;

#define Thdb		1
#define Ttlb		2
#define Tmagicmask	0xff0000ff

static int
tqisfree(u32int *key)
{
	uchar *p;

	p = (uchar *)key;
	return (p[Ttlb]^p[Thdb]) == 0;
}

/* increment tail and return the old value for user. */
static int
tqtailinc(u32int *keyaddr)
{
	uchar *p;

	p = (uchar *)keyaddr;
	return ainc8(&p[Ttlb]);
}

/* increment head, releasing the lock for the next in line */
static int
tqheadinc(uint *keyaddr)
{
	uchar *p;

	p = (uchar *)keyaddr;
	return ainc8(&p[Thdb]);
}

/*
 * attempt to increment tail by one and maintain head
 * such that tail == head + 1, indicating ownership.
 */
static int
tqcanlock(uint *keyaddr)
{
	u32int key, nkey;
	uchar *tl;

	nkey = key = *keyaddr;
	if(!tqisfree(&key))
		return 0;
	tl = (uchar *)&nkey + Ttlb;
	*tl += 1;
	return cas32((u32int*)keyaddr, key, nkey);
}

static uint
tqmagic(uint key)
{
	return key & Tmagicmask;
}

static uint
tqhead(uint *key)
{
	uchar *p;

	p = (uchar *)key;
	return p[Thdb];
}

static uint
tqtail(uint *key)
{
	uchar *p;

	p = (uchar *)key;
	return p[Ttlb];
}

/*
 * head and tail are really one byte wide, but uchar
 * subtraction math isn't doing the right wrap
 * delta calculation like uints do.
 */
static uint
tqdelta(uint tl, uint hd)
{
	if(tl >= hd)
		return tl - hd;
	return 0x100 - hd + tl;
}

/*
 * Return the page aligned key offset in the Lock.
 */
static void *
tqkey(Lock *l)
{
	uintptr key;

	key = ROUNDUP((uintptr)l, Cachelinesz);
	key += Cachelinesz - sizeof (uint);
	return (void*)key;
}

void
showlockloops(void)
{
	int machno, i, p;
	Glares *mg;
	Glaring *g;

	p = 0;
	for(machno = 0; machno < nelem(glares); machno++){
		mg = glares + machno;
		for(i = 0; i < nelem(mg->instance); i++){
			g = mg->instance + i;
			if(g->l == nil)
				break;
			if(!g->shown){
				g->shown = 0;
				iprint("mach%d: %d: l=%#p lpc=%#p pc=%#p n=%ud ok=%d\n",
					machno, i, g->l, g->lpc, g->pc, g->n, g->ok);
			}
			p++;
		}
	}
	if(p == 0)
		iprint("no loops\n");
}

static void
lockcrash(Lock *l, uintptr pc, char *why)
{
	Proc *p;

	p = l->p;
	if(lockdebug > 1){
		if(up != nil)
			dumpaproc(up);
		if(p != nil)
			dumpaproc(p);
	}
	showlockloops();
	panic("mach%d: %s lock %#p key %#p pc %#p proc %ud held by pc %#p proc %ud\n",
		m->machno, why, l, tqkey(l), pc, up->pid, l->pc, p? p->pid: 0);
}

/*
 * A "lock loop" is excessive delay in obtaining a spin lock:
 * it could be long delay through contention (ie, inefficient but harmless),
 * or a real deadlock (a programming error);
 * record them for later analysis to discover which.
 * Don't print them at the time, or the harmless cases become deadly.
 */
static Glaring*
lockloop(Glaring *og, Lock *l, uintptr pc)
{
	int i;
	Glares *mg;
	Glaring *g;
	Mpl s;

	s = splhi();
	if(l->m == MACHP(m->machno))
		lockcrash(l, pc, "deadlock/abandoned");	/* recovery is impossible */
	mg = &glares[m->machno];
	for(i = 0; i < nelem(mg->instance); i++){
		g = mg->instance + i;
		if(g->l == nil)
			break;
		if(g->l == l && g->lpc == l->pc && g->pc == pc){
			g->ok = 0;
			if(og == g){
				if((long)(sys->ticks - g->lastlock) >=  60*HZ)
					lockcrash(l, pc, "stuck");	/* delay is hopelessly long: we're doomed, i tell ye */
			}else{
				g->lastlock = sys->ticks;
				g->n++;
				g->shown = 0;
			}
			splx(s);
			return g;
		}
	}
	i = mg->next;
	g = mg->instance + i;
	g->ticks = sys->ticks;
	g->lastlock = g->ticks;
	g->l = l;
	g->pc = pc;
	g->lpc = l->pc;
	g->n = 1;
	g->ok = 0;
	g->shown = 0;
	if(++i >= nelem(mg->instance))
		i = 0;
	mg->next = i;
	mg->total++;
	splx(s);
	if(islo() && up != nil)
		print("mach%d: pid %d slow locks: %d\n", m->machno, up->pid, glares[m->machno].total);
	if(lockdebug)
		lockcrash(l, pc, "stuck");
	return g;
}

static int
lock0(Lock *l, uintptr pc)
{
	int i, j;
	uint mytl, hd, n, *key, ct;
	Glaring *g;

	key = tqkey(l);
	i = 0;
	g = nil;
	mytl = tqtailinc(key);
	hd = tqhead(key);
	if(hd == mytl)
		goto out;
//	if(kproflock)
//		kproflock(pc);
	for (;; i++) {
		n = tqdelta(mytl, hd) - 1;
		for (j = 0; j < spinbackoff*n; j++)
			pause();
		if(i > 100*1000*1000) {
			g = lockloop(g, l, pc);
			i = 0;
		}
		lfence();
		ct = *key;
		hd = tqhead(&ct);
		if(hd == mytl)
			break;
		if(tqmagic(ct) != 0) {
			for(;;){
				iprint("lock miskey %ud pc %#p->%#p\n",
					ct, pc, l->pc);
				iprint("%.*H\n", 32, l);
				iprint("%.*H\n", 32, (uchar*)l + 32);
				delay(5000);
			}
		}
	}
out:
	l->pc = pc;
	l->p = up;
	l->isilock = 0;
	l->m = MACHP(m->machno);
	if(g != nil)
		g->ok = 1;	/* recovered */
	return i > 0;
}

int
lockpc(Lock *l, uintptr pc)
{
	int n;

	if(up != nil)
		up->nlocks++;
	n = lock0(l, pc);
	if(up != nil)
		up->lastlock = l;
	return n;
}

int
lock(Lock *l)
{
	return lockpc(l, getcallerpc(&l));
}

void
ilockpc(Lock *l, uintptr pc)
{
	Mpl s;

	s = splhi();
	lock0(l, pc);
	if(up != nil)
		up->lastilock = l;
	m->ilockdepth++;
	l->isilock = 1;
	l->pl = s;
}

void
ilock(Lock *l)
{
	ilockpc(l, getcallerpc(&l));
}

int
canlock(Lock *l)
{
	uintptr pc;

	pc = getcallerpc(&l);

	if(up != nil)
		up->nlocks++;
	if(tqcanlock(tqkey(l)) == 0) {
		if(up != nil)
			up->nlocks--;
		return 0;
	}
	l->pc = pc;
	l->p = up;
	l->m = MACHP(m->machno);
	l->isilock = 0;
	return 1;
}

static void
unlock0(Lock *l, uintptr pc)
{
	uint *key;

	key = tqkey(l);
	if(tqisfree(key))
		panic("unlock: not locked: pc %#p\n", pc);
	if(l->isilock)
		panic("unlock of ilock: pc %#p, held by %#p\n", pc, l->pc);
	if(l->p != up)
		panic("unlock: u changed: pc %#p, acquired at pc %#p, lock p %#p, unlock u %#p\n", pc, l->pc, l->p, up);
	l->m = nil;
	l->p = nil;
	coherence();
	tqheadinc(key);
}

void
unlock(Lock *l)
{
	uintptr pc;

	pc = getcallerpc(&l);
	unlock0(l, pc);
	if(up && --up->nlocks == 0 && up->delaysched && islo()) {
		/*
		 * Call sched if the need arose while locks were held
		 * But, don't do it from interrupt routines, hence the islo() test
		 */
		sched();
	}
}

void
iunlock(Lock *l)
{
	uintptr pc;
	Mpl s;

	pc = getcallerpc(&l);
	if(!l->isilock)
		panic("iunlock of lock: pc %#p, held by %#p\n", pc, l->pc);
	if(islo())
		panic("iunlock while lo: pc %#p, held by %#p\n", pc, l->pc);
	l->isilock = 0;
	s = l->pl;
	unlock0(l, pc);
	m->ilockdepth--;
	if(up)
		up->lastilock = nil;
	splx(s);
}

int
ownlock(Lock *l)
{
	return l->m == MACHP(m->machno);
}

uintptr
lockgetpc(Lock *l)
{
	if(l != nil)
		return l->pc;
	return 0;
}

void
locksetpc(Lock *l, uintptr pc)
{
	l->pc = pc;
}
