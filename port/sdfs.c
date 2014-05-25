/*
 * sd fs emulation driver,
 * copyright © 2014 erik quanstrom
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/sd.h"
#include "../port/netif.h"
#include "../boot/dosfs.h"

extern	char	Echange[];
extern	char	Enotup[];

#define uprint(...)	snprint(up->genbuf, sizeof up->genbuf, __VA_ARGS__);
#define dprint(...)	print(__VA_ARGS__)

enum {
	Maxpath		= 256,
	Devsectsize	= 512,
};

typedef struct Dtab Dtab;
typedef struct Device Device;
typedef struct Ctlr Ctlr;
typedef struct Parse Parse;

struct Parse {
	char	*s;
	char	string[Maxpath];
};

struct Dtab {
	int	c0, c1;
	uvlong	(*size)(Device*);
	int	(*ssize)(Device*);
	void	(*init)(Device*);
	long	(*read)(Device*, void*, long, uvlong);
	long	(*write)(Device*, void*, long, uvlong);
};

struct Device {
	uchar	type;
	uchar	dno;
	uvlong	base;
	uvlong	size;
	int	ssize;

	Device*	link;
	Device*	cat;
	char	path[Maxpath];
	Chan*	c;

	void*	private;
};

struct Ctlr {
	QLock;

	Ctlr	*next;
	SDunit	*unit;

	char	dstring[Maxpath];
	Device*	device;

	uint	vers;
	uchar	drivechange;

	uvlong	fixedsectors;
	uvlong	sectors;
	uint	sectsize;
};

static	Lock	ctlrlock;
static	Ctlr	*head;
static	Ctlr	*tail;

SDifc	sdfsifc;

static	uvlong	asize(Device*);
static	int	assize(Device*);
static	void	ainit(Device*);
static	long	aread(Device*, void*, long, uvlong);
static	long	awrite(Device*, void*, long, uvlong);

static	uvlong	psize(Device*);
static	void	pinit(Device*);
static	long	pread(Device*, void*, long, uvlong);
static	long	pwrite(Device*, void*, long, uvlong);

static	uvlong	mirsize(Device*);
static	int	mirssize(Device*);
static	void	mirinit(Device*);
static	long	mirread(Device*, void*, long, uvlong);
static	long	mirwrite(Device*, void*, long, uvlong);

static	long	intread(Device*, void*, long, uvlong);
static	long	intwrite(Device*, void*, long, uvlong);

static	uvlong	catsize(Device*);
static	long	catread(Device*, void*, long, uvlong);
static	long	catwrite(Device*, void*, long, uvlong);

static	Dtab	dtab[] = {
['a']	'a',	0,	asize,	assize,	ainit,	aread,	awrite,
['p']	'p',	0,	psize,	mirssize,	pinit,	pread,	pwrite,
['{']	'{',	'}',	mirsize,	mirssize,	mirinit,	mirread,	mirwrite,
//['[']	'[',	']',	catsize,	mirssize,	mirinit,	intread,	intwrite,
['(']	'(',	')',	catsize,	mirssize,	mirinit,	catread,	catwrite,
};

static int
Zfmt0(Fmt *f, Device *d)
{
	char c1;
	Device *l;

	if(d == nil)
		return fmtprint(f, "Z");
	fmtprint(f, "%c", d->type);
	switch(d->type){
	case 'p':
		Zfmt0(f, d->cat);
		return fmtprint(f, "\"%s\"", d->path);
	case 'a':
		return fmtprint(f, "%d", d->dno);
	default:
		for(l = d->cat; l != nil; l = l->link)
			Zfmt0(f, l);
		c1 = dtab[d->type].c1;
		if(c1 != 0)
			fmtprint(f, "%c", c1);
		return 0;
	}
}

static int
Zfmt(Fmt *f)
{
	return Zfmt0(f, va_arg(f->args, Device*));
}
#pragma	varargck	type	"Z"	Device*

int
devsecsize(Device *d)
{
	return dtab[d->type].ssize(d);
}

static long
devread(Device *d, void *buf, long n, uvlong o)
{
	return dtab[d->type].read(d, buf, n, o);
}

static long
devwrite(Device *d, void *buf, long n, uvlong o)
{
	return dtab[d->type].read(d, buf, n, o);
}

/*
 * partition ugliness.  the options are
 * 1.  assume things about the devices
 * 2.  integrate partitioning.
 * i'll choose option 2, but i'm not happy.
 */

enum{
	Npart	= 8,
	Ntab	= 8,
	Sblank	= 0,
	Eblank	= 0,

	RBUFSIZE	= 8192,
};

typedef struct{
	char	type;
	uchar	rem;			/* unaligned partition */
	uvlong	start;
	uvlong	end;
	char	name[Maxpath];
}Part;

typedef struct{
	char	devstr[Maxpath];
	int	n;
	Part	tab[Npart];
}Tab;

static Tab	*tab;
static int		ntab;
static int		pdebug = 0;

static void
initpart(void)
{
	static int done;

	if(done++)
		return;
	tab = malloc(Ntab*sizeof *tab);		/* fix me: dynamic */
}

/*
 * each Device has one partition table, even if d->dno is different.
 */
Tab*
devtotab(Device *d, int *new)
{
	char *s, buf[Maxpath];
	int i;
	Tab *t;

	initpart();
	snprint(buf, sizeof buf, "%Z", d);
	for(i = 0; i < Ntab; i++){
		t = tab+i;
		s = t->devstr;
		if(*s == 0){
			memmove(s, buf, sizeof buf);
			*new = 1;
			return t;
		}else if(!strcmp(buf, s))
			return t;
	}
	panic("too many partitioned devices");
	return 0;
}

uvlong
sectofsblkno(Device *d, uvlong sec)
{
	return (sec*devsecsize(d))/RBUFSIZE;
}

uvlong
offtosec(Device *d, uvlong off)
{
	return off/devsecsize(d);
}

Part*
addpart(Device *parent, Device *d, char *s, uvlong a, uvlong b, int sec)
{
	uvlong sperrb;
	Tab *t;
	Part *p;

	dprint("  %Z %s [%lld, %lld) -> ", d, s, a, b);
	t = parent->private;
	if(t->n+1 == Npart){
		print("too many partitions; part %s %lld %lld dropped\n", s, a, b);
		return t->tab+t->n;
	}
	p = t->tab+t->n++;
	sperrb = offtosec(d, RBUFSIZE);
	if(sperrb > 0)
		p->rem = a%sperrb;
	else
		p->rem = 0;
	if(sec){
		p->type = 's';
	//	p->start = sectofsblkno(d, a+sperrb-1)+Sblank;		/* round up */
	//	p->end = sectofsblkno(d, b&~(sperrb-1))-Eblank;	/* round down */
		p->start = ROUNDUP(a, RBUFSIZE/devsecsize(d));
		p->end = ROUNDDN(b, RBUFSIZE/devsecsize(d));
	}else{
		p->type = 'o';
		p->start = a;
		p->end = b;
	}
	if(p->end < p->start)
		print("bad partition %s %lld not < %lld\n", s, p->start, p->end);
	strncpy(p->name, s, Maxpath);
	dprint("[%lld, %lld)\n", p->start, p->end);
	return p;
}

int
secio(Device *d, int write, uvlong sec, void *buf)
{
	long (*io)(Device*, void*, long, uvlong);

	io = write? devwrite: devread;
	return io(d, buf, 512, sec*512) != 512;
}

/*
 * deal with dos partitions placed on odd-sized boundaries.
 * to further our misfortune, byteio0 can't deal with negative
 * offsets.
 */
static Part *findpart(Device*, char*);
int
byteio(Device *d, int write, uvlong byte, ulong l, void *vbuf)
{
	uvlong rem;
	Part *p;
	long (*io)(Device*, void*, long, uvlong);

	if(d->type == 'p'){
		p = findpart(d, d->path);
		rem = p->rem*devsecsize(d);
		if(rem)
			byte -= RBUFSIZE-rem;
		byte -= Sblank*RBUFSIZE;
		byte += d->base*RBUFSIZE;
		d = d->cat;
	}
	io = write? devwrite: devread;
	return io(d, vbuf, l, byte);
}

int
mbrread(Device *d, uvlong sec, void *buf)
{
	uchar *u;

	if(byteio(d, 0, sec*512, 512, buf) != 512)
		return 1;
	u = buf;
	if(u[0x1fe] != 0x55 || u[0x1ff] != 0xaa)
		return 1;
	return 0;
}

static int
p9part(Device *parent, Device *d, uvlong sec, char *buf)
{
	char *field[4], *line[Npart+1];
	uvlong start, end;
	int i, n;

	if(secio(d, 0, sec+1, buf))
		return 1;
	buf[512-1] = '\0';
	if(strncmp(buf, "part ", 5))
		return 1;

	n = getfields(buf, line, Npart+1, 1, "\n");
	dprint("p9part %d lines..", n);
	if(n == 0)
		return -1;
	for(i = 0; i < n; i++){
		if(strncmp(line[i], "part ", 5) != 0)
			break;
		if(getfields(line[i], field, 4, 0, " ") != 4)
			break;
		start = strtoull(field[2], 0, 0);
		end = strtoull(field[3], 0, 0);
		if(start >= end || end > offtosec(d, d->size))
			break;
		addpart(parent, d, field[1], sec+start, sec+end, 1);
	}
	return 0;
}

int
isdos(int t)
{
	return t==FAT12 || t==FAT16 || t==FATHUGE || t==FAT32 || t==FAT32X;
}

int
isextend(int t)
{
	return t==EXTEND || t==EXTHUGE || t==LEXTEND;
}

static int
mbrpart(Device *parent, Device *d, char *mbrbuf, char *partbuf)
{
	char name[10];
	int ndos, i, nplan9;
	ulong sec, start, end;
	ulong firstx, nextx, npart;
	Dospart *dp;
	int (*repart)(Device*, Device*, uvlong, char*);

	sec = 0;
	dp = (Dospart*)&mbrbuf[0x1be];

	/* get the MBR (allowing for DMDDO) */
	if(mbrread(d, sec, mbrbuf))
		return 1;
	for(i=0; i<4; i++)
		if(dp[i].type == DMDDO) {
			dprint("DMDDO %d\n", i);
			sec = 63;
			if(mbrread(d, sec, mbrbuf))
				return 1;
			i = -1;	/* start over */
		}
	/*
	 * Read the partitions, first from the MBR and then
	 * from successive extended partition tables.
	 */
	nplan9 = 0;
	ndos = 0;
	firstx = 0;
	for(npart=0;; npart++) {
		if(mbrread(d, sec, mbrbuf))
			return 1;
		if(firstx)
			print("%Z ext %lud ", d, sec);
		else
			print("%Z mbr ", d);
		nextx = 0;
		for(i=0; i<4; i++) {
			start = sec+GLONG(dp[i].start);
			end = start+GLONG(dp[i].len);
			if(dp[i].type == 0 && start == 0 && end == 0)
				continue;
			dprint("type %x [%ld, %ld)", dp[i].type, start, end);
			repart = 0;
			if(dp[i].type == PLAN9) {
				if(nplan9 == 0)
					strcpy(name, "plan9");
				else
					sprint(name, "plan9.%d", nplan9);
				repart = p9part;
				nplan9++;
			}else if(!ndos && isdos(dp[i].type)){
				ndos = 1;
				strcpy(name, "dos");
			}else
				snprint(name, sizeof name, "%ld", npart);
			if(end != 0){
				dprint(" %s..", name);
				addpart(parent, d, name, start, end, 1);
			}
			if(repart)
				repart(parent, d, start, partbuf);
			
			/* nextx is relative to firstx (or 0), not sec */
			if(isextend(dp[i].type)){
				nextx = start-sec+firstx;
				dprint("link %lud...", nextx);
			}
		}
		dprint("\n");
		if(!nextx)
			break;
		if(!firstx)
			firstx = nextx;
		sec = nextx;
	}	
	return 0;
}

static int
guessparttab(Tab *t)
{
	int i, c;


	for(i = 0; i < t->n; i++){
		c = t->tab[i].type;
		if(c == 's' || c == 'o')
			return 1;
	}
	return 0;
}

static Part*
findpart(Device *d, char *s)
{
	char c;
	int i;
	uvlong l, start, end;
	Part *p;
	Tab *t;

	t = d->private;
	if(s == 0)
		goto mkpart;
	for(i = 0; i < t->n; i++)
		if(!strcmp(t->tab[i].name, s))
			return t->tab+i;
	print("part %Z not found\n", d);
	return nil;
mkpart:
	if(guessparttab(t))
		print("warning: ignoring part table on %Z\n", d->cat);
	if(d->base < 101 && d->size < 101){
		c = '%';
		l = d->cat->size / 100;
		start = d->base*l;
		end = start + d->size*l;
	}else{
		c = 'b';
		start = d->base;
		end = d->size;
	}
	for(i = 0; i < t->n; i++){
		p = t->tab+i;
		if(start == p->start)
		if(end == p->end)
			return p;
	}
	p = addpart(d, d->cat, "", start, end, 0);
	if(c)
		p->type = c;
	snprint(p->name, sizeof p->name, "f%ld%ld", t-tab, p-t->tab);	// BOTCH
	return p;
}

void
partition(Device *parent, Device *d)
{
	char *m, *p;
	int new;
	Part *q;

	new = 0;
	parent->private = devtotab(d, &new);
	if(new){
		m = malloc(RBUFSIZE);
		p = malloc(RBUFSIZE);
		if(!waserror()){
			!mbrpart(parent, d, m, p) || p9part(parent, d, 0, p);
			poperror();
		}
		free(m);
		free(p);
	}
	q = findpart(parent, parent->path);
	if(q == nil){
		uprint("fs: no part %s", d->path);
		error(up->errstr);
	}
	parent->base = devsecsize(parent)*q->start;
	parent->size = devsecsize(parent)*(q->end-q->start);
}

/* end of partion junk */

static void
freedevice(Device *d)
{
	Device *x, *y;

	if(d == nil)
		return;
	if(d->cat)
		freedevice(d->cat);
	for(x = d->link; x != nil; x = y){
		y = x->link;
		x->link = nil;
		if(x->c != nil)
			chanfree(x->c);
		free(x);
	}
	free(d);
}

static Device*
cpdevice(Device *a)
{
	Device *b;

	if(a == nil)
		return nil;
	b = malloc(sizeof *a);
	*b = *a;
	if(b->c)
		b->c = cclone(b->c);
//	b->link = cpdevice(b->link);
	return b;
}

static char*
cstring(Parse *p, char *s0, char *e)
{
	char c, *s;

	s = s0;
	p->s++;
	for(;;){
		if(s == e)
			error("fs: string too long");
		c = *p->s++;
		if(c == '"'){
			if(*p->s != '"')
				break;
			p->s++;
		}
		if(c == 0){
			error("fs: nil in string");
			break;
		}
		*s++ = c;
	}
	*s = 0;
	if(s == s0)
		error("fs: nil string");
	return s;
}

extern long strcspn(char*, char*);

static Device*
parse(Parse *p)
{
	char *s;
	Device d, *t, *v, **dd;

//print("parseenter %s\n", p->s);
	memset(&d, 0, sizeof d);
	d.type = *p->s++;
	switch(d.type){
	default:
		snprint(up->errstr, sizeof up->errstr, "fs: type %c not recognized", d.type);
		error(up->errstr);
	case '/':
	case '#':
		s = p->s + strcspn(p->s, "()[]{} \"");
		snprint(d.path, sizeof d.path, "%.*s", (int)(s-p->s), p->s);
		p->s = s;
//print("parseexit %s «%Z»\n", p->s, &d);
		return cpdevice(&d);
	case 'a':
		s = p->s;
		d.dno = strtoull(s, &p->s, 0);
		if(s == p->s)
			error("fs: a needs digit");
		snprint(d.path, sizeof d.path, "/dev/sdE%d", d.dno);
//print("parseexit %s «%Z»\n", p->s, &d);
		return cpdevice(&d);
	case 'p':
		t = parse(p);
		if(waserror()){
			freedevice(t);
			nexterror();
		}
		cstring(p,  d.path, d.path+sizeof d.path);
		poperror();
		v = cpdevice(&d);
		v->cat = t;
//print("parseexit %s «%Z»\n", p->s, v);
		return v;
	case '{':
//	case '[':
	case '(':
		t = nil;
		if(waserror()){
			freedevice(t);
			nexterror();
		}
		for(dd = &t;; dd = &(*dd)->link){
			*dd = parse(p);
			if(*dd == nil || strchr(")}]", *p->s) != nil)
				break;
		}
		if(*p->s != dtab[d.type].c1)
			error("fs: unmatched grouping");
		p->s++;
		poperror();

		v = cpdevice(&d);
		v->cat = t;
//print("parseexit %s «%Z»\n", p->s, v);
		return v;
	}
}

static uvlong
asize(Device *d)
{
	uchar buf[sizeof(Dir) + 100];
	int n;
	Dir dir;

	if(d->size == 0){
		if(waserror()){
			iprint("sdfs: size: %s\n", up->errstr);
			nexterror();
		}
		if(d->c == nil)
			error("fs: chan not initialized");
		n = d->c->dev->stat(d->c, buf, sizeof buf);
		if(convM2D(buf, n, &dir, nil) == 0)
			error("internal error: stat error in seek");
		poperror();
		d->size = dir.length;
	}
	return d->size;
}

static int
assize(Device *d)
{
	char n[Maxpath], *buf, *p, *f[5];
	Chan *c;

	if(d->ssize != 0)
		return d->ssize;
	d->ssize = 512;

	snprint(n, sizeof n, "%s/ctl", d->path);
	c = namec(n, Aopen, ORDWR, 0);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	buf = malloc(READSTR);
	if(buf == nil)
		error(Enomem);
	if(waserror()){
		free(buf);
		nexterror();
	}
	c->dev->read(c, buf, READSTR-1, 0);
	p = strstr(buf, "\ngeometry");
	if(p != nil && tokenize(p+1, f, nelem(f)) > 2){
		d->ssize = strtoul(f[2], nil, 0);
		if(d->ssize <= 0 || d->ssize & 512-1)
			d->ssize = 512;
	}
	poperror();
	free(buf);
	poperror();
	cclose(c);
	return d->ssize;
}

static void
ainit(Device *d)
{
	char buf[Maxpath];

	if(d->c == nil){
		snprint(buf, sizeof buf, "%s/data", d->path);
print("ainit %Z namec %s\n", d, buf);
		d->c = namec(buf, Aopen, ORDWR, 0);
		d->size = asize(d);
		d->ssize = assize(d);
		if(d->ssize > RBUFSIZE)
			error("sector size too large");
	}
}

static long
aread(Device *d, void *buf, long n, uvlong o)
{
	return d->c->dev->read(d->c, buf, n, o);
}

static long
awrite(Device *d, void *buf, long n, uvlong o)
{
	return d->c->dev->write(d->c, buf, n, o);
}

static uvlong
psize(Device *d)
{
	uchar buf[sizeof(Dir) + 100];
	int n;
	Dir dir;

	if(d->size == 0){
		if(waserror()){
			iprint("sdfs: size: %s\n", up->errstr);
			nexterror();
		}
		if(d->c == nil)
			error("fs: pchan not initialized");
		n = d->c->dev->stat(d->c, buf, sizeof buf);
		if(convM2D(buf, n, &dir, nil) == 0)
			error("internal error: stat error in seek");
		poperror();
		d->size = dir.length;
	}
	return d->size;
}

static void
pinit(Device *d)
{
	dtab[d->cat->type].init(d->cat);
	d->ssize = devsecsize(d);
	partition(d, d->cat);
}

static long
pread(Device *d, void *buf, long n, uvlong o)
{
	uvlong base, size;

	base = d->base;
	size = d->size;
	if(o < size)
		return dtab[d->type].read(d, buf, n, base+o);
	print("pread %lld %lld\n", o, size);
	return 1;
}

static long
pwrite(Device *d, void *buf, long n, uvlong o)
{
	uvlong base, size;
	Device *x;

	base = d->base;
	size = d->size;
	x = d->cat;
	if(o < size)
		return dtab[x->type].write(x, buf, n, base+o);

	print("pwrite %lld %lld\n", o, size);
	return 1;
}

static int
mirssize(Device *d)
{
	int n;
	Device *x;

	if(d->ssize == 0){
		d->ssize = 512;
		for(x = d->cat; x != nil; x = x->link){
			n = dtab[x->type].ssize(x);
			if(n > d->ssize)
				d->ssize = n;
		}
	}
	return d->ssize;
}

static void
mirinit(Device *d)
{
	Device *x;

	for(x = d->cat; x != nil; x = x->link)
		dtab[x->type].init(x);
	d->size = dtab[d->type].size(d);
	d->ssize = devsecsize(d);
	if(d->ssize > RBUFSIZE)
		error("sector size too large");
}

static uvlong
mirsize(Device *d)
{
	uvlong m, t;
	Device *x;

	if(d->size == 0){
		t = 0;
		for(x = d->cat; x != nil; x = x->link){
			m = x->size;
			if(m == 0){
				m = dtab[x->type].size(x);
				x->size = m;
			}
			if(t == 0 || m < t)
				t = m;
		}
		d->size = t;
	}
	return d->size;
}

static long
mirread(Device *d, void *buf, long n, uvlong o)
{
	long r;
	Device *x;

	for(x = d->cat; x != nil; x = x->link){
		if(waserror()){
			print("fs: read error %s @%llud", up->errstr, o);
			continue;
		}
		r = dtab[x->type].read(x, buf, n, o);
		poperror();
		return r;
	}
	error(up->errstr);
	return -1;
}

static long
mirwrite(Device *d, void *buf, long n, uvlong o)
{
	int bad;
	long r;
	Device *x;

	bad = 0;
	for(x = d->cat; x != nil; x = x->link){
		if(waserror()){
			print("fs: read error %s @%llud", up->errstr, o);
			bad = 1;
			continue;
		}
		r = dtab[x->type].write(x, buf, n, o);
		if(r != n)
			error("fs: short read");
		poperror();
	}
	if(bad)
		error(up->errstr);
	return n;
}

static uvlong
catsize(Device *d)
{
	uvlong m, t;
	Device *x;

	if(d->size == 0){
		t = 0;
		for(x = d->cat; x != nil; x = x->link){
			m = x->size;
			if(m == 0){
				m = dtab[x->type].size(x);
				x->size = m;
			}
			t += m;
		}
		d->size = t;
	}
	return d->size;
}

static long
catread(Device *d, void *buf, long n, uvlong o)
{
	uvlong m, l;
	Device *x;

	l = 0;
	for(x = d->cat; x != nil; x = x->link){
		m = x->size;
		if(m  == 0){
			m = dtab[x->type].size(d);
			x->size = m;
		}
		m = x->size;
		if(o < l+m)
			return dtab[x->type].read(x, buf, n, o);
		l += m;
	}
	error("mcatread");
	return -1;
}

static long
catwrite(Device *d, void *buf, long n, uvlong o)
{
	uvlong m, l;
	Device *x;

	l = 0;
	for(x = d->cat; x != nil; x = x->link){
		m = x->size;
		if(m  == 0){
			m = dtab[x->type].size(d);
			x->size = m;
		}
		if(o < l+m)
			return dtab[x->type].write(x, buf, n, o);
		l += m;
	}
	error("mcatwrite");
	return -1;
}

/* must call with c qlocked */
static void
identify(Ctlr *c, SDunit *u)
{
	uvlong s, osectors;

	if(waserror()){
		iprint("sdfs: identify: %s\n", up->errstr);
		nexterror();
	}
	if(c->device == nil)
		error("fs: device uninitialized");
	osectors = c->sectors;
	s = dtab[c->device->type].size(c->device) / c->sectsize;
	poperror();

	memset(u->inquiry, 0, sizeof u->inquiry);
	u->inquiry[2] = 2;
	u->inquiry[3] = 2;
	u->inquiry[4] = sizeof u->inquiry - 4;
	memmove(u->inquiry+8, c->dstring, 40);

	if(osectors == 0 || osectors != s){
		if(c->fixedsectors != 0)
			c->sectors = c->fixedsectors;
		else
			c->sectors = s;
		c->drivechange = 1;
		c->vers++;
	}
}

static Ctlr*
ctlrlookup(char *dstring)
{
	Ctlr *c;

	lock(&ctlrlock);
	for(c = head; c; c = c->next)
		if(strcmp(c->dstring, dstring) == 0)
			break;
	unlock(&ctlrlock);
	return c;
}

static Ctlr*
newctlr(char *dstring)
{
	Ctlr *c;
	Parse p;

	if(ctlrlookup(dstring))
		error(Eexist);
	if((c = malloc(sizeof *c)) == nil)
		error(Enomem);
	if(waserror()){
		free(c);
		nexterror();
	}
	memset(&p, 0, sizeof p);
	kstrcpy(p.string, dstring, sizeof p.string);
	p.s = p.string;
	c->device = parse(&p);
print("Zdev %Z\n", c->device);
	dtab[c->device->type].init(c->device);
	poperror();
	kstrcpy(c->dstring, dstring, sizeof c->dstring);
	lock(&ctlrlock);
	if(head != nil)
		tail->next = c;
	else
		head = c;
	tail = c;
	unlock(&ctlrlock);
	return c;
}

static void
delctlr(Ctlr *c)
{
	Ctlr *x, *prev;

	lock(&ctlrlock);

	for(prev = nil, x = head; x; prev = x, x = c->next)
		if(strcmp(c->dstring, x->dstring) == 0)
			break;
	if(x == nil){
		unlock(&ctlrlock);
		error(Enonexist);
	}

	if(prev)
		prev->next = x->next;
	else
		head = x->next;
	if(x->next == nil)
		tail = prev;
	unlock(&ctlrlock);

	freedevice(x->device);
	free(x);
}

static SDev*
probe(char *path, SDev *s)
{
	char *p, *q;
	uint sectsize;
	uvlong sectors;
	Ctlr *c;

	fmtinstall('Z', Zfmt);
	sectsize = 0;
	sectors = 0;
	if(p = strchr(path, '!')){
		*p++ = 0;
		if((q = strchr(p, '!')) != nil){
			sectors = strtoull(p, nil, 0);
			p = q + 1;
		}
		sectsize = strtoul(p, nil, 0);
	}
	c = newctlr(path);
	c->sectsize = sectsize? sectsize: devsecsize(c->device);
	c->fixedsectors = sectors;
	if(s == nil && (s = malloc(sizeof *s)) == nil)
		return nil;
	s->ctlr = c;
	s->ifc = &sdfsifc;
	s->nunit = 1;
	return s;
}

static char 	*probef[32];
static int 	nprobe;

static int
pnpprobeid(char *s)
{
	int id;

	if(strlen(s) < 2)
		return 0;
	id = 'l';
	if(s[1] == '!')
		id = s[0];
	return id;
}

static SDev*
pnp(void)
{
	int i, id;
	char *p;
	SDev *h, *t, *s;

	if((p = getconf("fsdev")) == 0)
		return nil;
	nprobe = tokenize(p, probef, nelem(probef));
	h = t = 0;
	for(i = 0; i < nprobe; i++){
		id = pnpprobeid(probef[i]);
		if(id == 0)
			continue;
		s = malloc(sizeof *s);
		if(s == nil)
			break;
		s->ctlr = 0;
		s->idno = id;
		s->ifc = &sdfsifc;
		s->nunit = 1;

		if(h)
			t->next = s;
		else
			h = s;
		t = s;
	}
	return h;
}

static Ctlr*
pnpprobe(SDev *s)
{
	char *p;
	static int i;

	if(i > nprobe)
		return nil;
	p = probef[i++];
	if(strlen(p) < 2)
		return nil;
	if(p[1] == '!')
		p += 2;
	return probe(p, s)->ctlr;
}


static int
fsverify(SDunit *u)
{
	SDev *s;
	Ctlr *c;

	s = u->dev;
	c = s->ctlr;
	if(c == nil){
		if(waserror())
			return 0;
		s->ctlr = c = pnpprobe(s);
		poperror();
	}
	c->drivechange = 1;
	return 1;
}

static int
connect(SDunit *u, Ctlr *c)
{
	qlock(c);
	if(waserror()){
		qunlock(c);
		return -1;
	}
	identify(u->dev->ctlr, u);
	qunlock(c);
	poperror();
	return 0;
}

static int
fsonline(SDunit *u)
{
	Ctlr *c;
	int r;

	c = u->dev->ctlr;
	if(c->drivechange){
		if(connect(u, c) == -1)
			return 0;
		r = 2;
		c->drivechange = 0;
		u->sectors = c->sectors;
		u->secsize = c->sectsize;
	} else
		r = 1;
	return r;
}

static long
fsbio(SDunit *u, int, int write, void *a, long count, uvlong lba)
{
	uchar *data;
	int n;
	long (*rio)(Device*, void*, long, uvlong);
	Dtab *t;
	Ctlr *c;

	c = u->dev->ctlr;
	data = a;
	t = dtab + c->device->type;
	if(write)
		rio = t->write;
	else
		rio = t->read;

	if(waserror()){
		if(strcmp(up->errstr, Echange) == 0 ||
		    strcmp(up->errstr, Enotup) == 0)
			u->sectors = 0;
		nexterror();
	}
	n = rio(c->device, data, c->sectsize * count, c->sectsize * lba);
	poperror();
	return n;
}

static int
fsrio(SDreq *r)
{
	int i, count, rw;
	uvlong lba;
	SDunit *u;

	u = r->unit;

	if(r->cmd[0] == 0x35 || r->cmd[0] == 0x91)
		return sdsetsense(r, SDok, 0, 0, 0);

	if((i = sdfakescsi(r)) != SDnostatus)
		return r->status = i;
	if((i = sdfakescsirw(r, &lba, &count, &rw)) != SDnostatus)
		return i;
	r->rlen = fsbio(u, r->lun, rw == SDwrite, r->data, count, lba);
	return r->status = SDok;
}

static int
fsrctl(SDunit *u, char *p, int l)
{
	Ctlr *c;
	char *e, *op;

	if((c = u->dev->ctlr) == nil)
		return 0;
	e = p+l;
	op = p;

	p = seprint(p, e, "device\t%s\n", c->dstring);
	p = seprint(p, e, "Zdev\t%Z\n", c->device);
	p = seprint(p, e, "geometry %llud %d\n", c->sectors, c->sectsize);
	return p - op;
}

static int
fswctl(SDunit *, Cmdbuf *cmd)
{
	cmderror(cmd, Ebadarg);
	return 0;
}

static SDev*
fsprobew(DevConf *c)
{
	char *p;

	p = strchr(c->type, '/');
	if(p == nil || strlen(p) > Maxpath - 1)
		error(Ebadarg);
	p++;
	if(ctlrlookup(p))
		error(Einuse);
	return probe(p, nil);
}

static void
fsclear(SDev *s)
{
	delctlr((Ctlr *)s->ctlr);
}

static char*
fsrtopctl(SDev *s, char *p, char *e)
{
	Ctlr *c;

	c = s->ctlr;
	return seprint(p, e, "%s fs %s\n", s->name, c!=nil? c->dstring: "");
}

static int
fswtopctl(SDev *, Cmdbuf *cmd)
{
	switch(cmd->nf){
	default:
		cmderror(cmd, Ebadarg);
	}
	return 0;
}

SDifc sdfsifc = {
	"fs",

	pnp,
	nil,		/* legacy */
	nil,		/* enable */
	nil,		/* disable */

	fsverify,
	fsonline,
	fsrio,
	fsrctl,
	fswctl,

	fsbio,
	fsprobew,	/* probe */
	fsclear,	/* clear */
	fsrtopctl,
	fswtopctl,
};
