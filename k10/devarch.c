#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ureg.h"

typedef struct IOMap IOMap;
struct IOMap
{
	IOMap	*next;
	char	tag[16+1];
	uint	start;
	uint	end;
};

static struct
{
	Lock;
	IOMap	*map;
	IOMap	*free;
	IOMap	maps[32];		// some initial free maps

	QLock	ql;			// lock for reading map
} iomap;

enum {
	Qdir = 0,
	Qioalloc = 1,
	Qiob,
	Qiow,
	Qiol,
	Qbase,

	Qmax = 32,
};

typedef long Rdwrfn(Chan*, void*, long, vlong);

static Rdwrfn *readfn[Qmax];
static Rdwrfn *writefn[Qmax];

static Dirtab archdir[Qmax] = {
	".",		{ Qdir, 0, QTDIR },	0,	0555,
	"ioalloc",	{ Qioalloc, 0 },	0,	0444,
	"iob",		{ Qiob, 0 },		0,	0660,
	"iow",		{ Qiow, 0 },		0,	0660,
	"iol",		{ Qiol, 0 },		0,	0660,
};
Lock archwlock;	/* the lock is only for changing archdir */
int narchdir = Qbase;

/*
 * Add a file to the #P listing.  Once added, you can't delete it.
 * You can't add a file with the same name as one already there,
 * and you get a pointer to the Dirtab entry so you can do things
 * like change the Qid version.  Changing the Qid path is disallowed.
 */
Dirtab*
addarchfile(char *name, int perm, Rdwrfn *rdfn, Rdwrfn *wrfn)
{
	int i;
	Dirtab d;
	Dirtab *dp;

	memset(&d, 0, sizeof d);
	strcpy(d.name, name);
	d.perm = perm;

	lock(&archwlock);
	if(narchdir >= Qmax){
		unlock(&archwlock);
		return nil;
	}

	for(i=0; i<narchdir; i++)
		if(strcmp(archdir[i].name, name) == 0){
			unlock(&archwlock);
			return nil;
		}

	d.qid.path = narchdir;
	archdir[narchdir] = d;
	readfn[narchdir] = rdfn;
	writefn[narchdir] = wrfn;
	dp = &archdir[narchdir++];
	unlock(&archwlock);

	return dp;
}

void
ioinit(void)
{
	int i;

	for(i = 0; i < nelem(iomap.maps)-1; i++)
		iomap.maps[i].next = &iomap.maps[i+1];
	iomap.maps[i].next = nil;
	iomap.free = iomap.maps;
}

/*
 * alloc some io port space and remember who it was
 * alloced to.  if port < 0, find a free region.
 */
int
ioalloc(int port, int size, int /*align*/, char *tag)
{
	IOMap *map, **l;

	lock(&iomap);
	assert(port >= 0);
	assert(port + size <= 0x10000);	/* 64k io space */
	// see if the space clashes with previously allocated ports
	for(l = &iomap.map; *l; l = &(*l)->next){
		map = *l;
		if(map->end <= port)
			continue;
		if(map->start >= port+size)
			break;
		unlock(&iomap);
		return -1;
	}
	map = iomap.free;
	if(map == nil)
		panic("ioalloc: out of maps");
	iomap.free = map->next;
	map->next = *l;
	map->start = port;
	map->end = port + size;
	strncpy(map->tag, tag, sizeof(map->tag));
	map->tag[sizeof(map->tag)-1] = 0;
	*l = map;

	archdir[0].qid.vers++;

	unlock(&iomap);
	return map->start;
}

void
iofree(int port)
{
	IOMap *map, **l;

	lock(&iomap);
	for(l = &iomap.map; *l; l = &(*l)->next){
		if((*l)->start == port){
			map = *l;
			*l = map->next;
			map->next = iomap.free;
			iomap.free = map;
			break;
		}
		if((*l)->start > port)
			break;
	}
	archdir[0].qid.vers++;
	unlock(&iomap);
}

int
iounused(int start, int end)
{
	IOMap *map;

	for(map = iomap.map; map; map = map->next){
		if(start >= map->start && start < map->end
		|| start <= map->start && end > map->start)
			return 0; 
	}
	return 1;
}

static void
checkport(int start, int end)
{
	/* standard vga regs are OK */
	if(start >= 0x2b0 && end <= 0x2df+1)
		return;
	if(start >= 0x3c0 && end <= 0x3da+1)
		return;

	if(iounused(start, end))
		return;
	error(Eperm);
}

static Chan*
archattach(char* spec)
{
	return devattach('P', spec);
}

Walkqid*
archwalk(Chan* c, Chan *nc, char** name, int nname)
{
	return devwalk(c, nc, name, nname, archdir, narchdir, devgen);
}

static long
archstat(Chan* c, uchar* dp, long n)
{
	return devstat(c, dp, n, archdir, narchdir, devgen);
}

static Chan*
archopen(Chan* c, int omode)
{
	return devopen(c, omode, archdir, narchdir, devgen);
}

static void
archclose(Chan*)
{
}

enum
{
	Linelen= 31,
};

static long
archread(Chan *c, void *a, long n, vlong offset)
{
	char *buf, *p;
	int port;
	u16int *sp;
	u32int *lp;
	IOMap *map;
	Rdwrfn *fn;

	switch((uint)c->qid.path){

	case Qdir:
		return devdirread(c, a, n, archdir, narchdir, devgen);

	case Qiob:
		port = offset;
		checkport(offset, offset+n);
		for(p = a; port < offset+n; port++)
			*p++ = inb(port);
		return n;

	case Qiow:
		if(n & 1)
			error(Ebadarg);
		checkport(offset, offset+n);
		sp = a;
		for(port = offset; port < offset+n; port += 2)
			*sp++ = ins(port);
		return n;

	case Qiol:
		if(n & 3)
			error(Ebadarg);
		checkport(offset, offset+n);
		lp = a;
		for(port = offset; port < offset+n; port += 4)
			*lp++ = inl(port);
		return n;

	case Qioalloc:
		break;

	default:
		if(c->qid.path < narchdir && (fn = readfn[c->qid.path]))
			return fn(c, a, n, offset);
		error(Eperm);
		break;
	}

	if((buf = malloc(n)) == nil)
		error(Enomem);
	p = buf;
	n = n/Linelen;
	offset = offset/Linelen;

	switch((uint)c->qid.path){
	case Qioalloc:
		lock(&iomap);
		for(map = iomap.map; n > 0 && map != nil; map = map->next){
			if(offset-- > 0)
				continue;
			sprint(p, "%#8ux %#8ux %-16.16s\n", map->start, map->end-1, map->tag);
			p += Linelen;
			n--;
		}
		unlock(&iomap);
		break;
	}

	n = p - buf;
	memmove(a, buf, n);
	free(buf);

	return n;
}

static long
archwrite(Chan *c, void *a, long n, vlong offset)
{
	char *p;
	int port;
	u16int *sp;
	u32int *lp;
	Rdwrfn *fn;

	switch((uint)c->qid.path){

	case Qiob:
		p = a;
		checkport(offset, offset+n);
		for(port = offset; port < offset+n; port++)
			outb(port, *p++);
		return n;

	case Qiow:
		if(n & 1)
			error(Ebadarg);
		checkport(offset, offset+n);
		sp = a;
		for(port = offset; port < offset+n; port += 2)
			outs(port, *sp++);
		return n;

	case Qiol:
		if(n & 3)
			error(Ebadarg);
		checkport(offset, offset+n);
		lp = a;
		for(port = offset; port < offset+n; port += 4)
			outl(port, *lp++);
		return n;

	default:
		if(c->qid.path < narchdir && (fn = writefn[c->qid.path]))
			return fn(c, a, n, offset);
		error(Eperm);
		break;
	}
	return 0;
}

Dev archdevtab = {
	'P',
	"arch",

	devreset,
	devinit,
	devshutdown,
	archattach,
	archwalk,
	archstat,
	archopen,
	devcreate,
	archclose,
	archread,
	devbread,
	archwrite,
	devbwrite,
	devremove,
	devwstat,
};

static long
cputyperead(Chan*, void *a, long n, vlong off)
{
	char buf[512], *e;

	e = buf+sizeof buf;
	seprint(buf, e, "%s %ud\n", "AMD64", m->cpumhz);
	return readstr(off, a, n, buf);
}

static long
archcfgread(Chan*, void *a, long n, vlong off)
{
	char buf[512], *p, *e;

	p = buf;
	e = buf+sizeof buf;

	/* builtin devices may be missing. */
	p = seprint(p, e, "legacy	%d\n", !sys->nolegacyprobe);
	p = seprint(p, e, "i8042kbd	%d\n", !sys->noi8042kbd);
	p = seprint(p, e, "vga	%d\n", !sys->novga);
	p = seprint(p, e, "msi	%d\n", !sys->nomsi);
	p = seprint(p, e, "msi-x	%d\n", !sys->nomsix);
	p = seprint(p, e, "cmos	%d\n", !sys->nocmos);

	p = seprint(p, e, "monitor	%d\n", sys->monitor);

	USED(p);

	return readstr(off, a, n, buf);
}

/* need to catch #GP */
static long
msrread(Chan*, void *a, long n, vlong o)
{
	char buf[32];
	u32int msr;
	uvlong v;

	if(o != (uint)o)
		error(Egreg);
	msr = (uint)o;
	v = rdmsr(msr);
	snprint(buf, sizeof buf, "%#.16llux\n", v);
	return readstr(0, a, n, buf);
}

static long
msrwrite(Chan*, void *a, long n, vlong o)
{
	char buf[32];
	u32int msr;
	uvlong v;

	if(o != (uint)o || n > 31)
		error(Egreg);
	memmove(buf, a, n);
	buf[n] = 0;
	v = strtoull(buf, nil, 0);
	msr = (uint)o;
	wrmsr(msr, v);
	return n;
}

void
archinit(void)
{
	addarchfile("cputype", 0444, cputyperead, nil);
	addarchfile("archcfg", 0444, archcfgread, nil);
	addarchfile("msr", 0664, msrread, msrwrite);
}

void
archreset(void)
{
	int i;

	/*
	 * The reset register (0xcf9) is usually in one of the bridge chips.
	 * The actual location and sequence could be extracted from
	 * ACPI.
	 */
	i = inb(0xcf9);					/* ICHx reset control */
	i &= 0x06;
	outb(0xcf9, i|0x02);				/* SYS_RST */
	delay(1);
	outb(0xcf9, i|0x06);				/* RST_CPU transition */

	ndnr();
}

/*
 *  return value and speed of timer
 */
uvlong
fastticks(uvlong* hz)
{
	if(hz != nil)
		*hz = m->cpuhz;
	return rdtsc();
}

uint
µs(void)
{
	return fastticks2us(rdtsc());
}

/*
 *  set next timer interrupt
 */
void
timerset(uvlong x)
{
	extern void lapictimerset(uvlong);

	lapictimerset(x);
}

void
cycles(uvlong* t)
{
	*t = rdtsc();
}

/*  
 *  performance measurement ticks.  must be low overhead.
 *  doesn't have to count over a second.
 */
uvlong
perfticks(void)
{
	return rdtsc();
}
