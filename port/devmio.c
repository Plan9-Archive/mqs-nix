#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

enum {
	Chunkbytes	= 4,
	Maxchunk	= (1ull<<Chunkbytes*8)-1,

	Qzero		= 0,
	Qtopdir,
	Qtopstat,
	Qtopclone,
	Qconvdir,
	Qconvdata,
	Qconvctl,
};

#define Type(q)		((uint)q.path & 0xf)
#define Conv(q)		((q).path>>5)
#define QID(n, t)	((n)<<5 | (t))

static char *names[] = {
[Qzero]		"#~",
[Qtopdir]	"mio",
[Qtopstat]	"status",
[Qtopclone]	"clone",
[Qconvdir]	"*gok*",
[Qconvdata]	"data",
[Qconvctl]	"ctl"
};

typedef	struct	Mio	Mio;
struct Mio {
	Queue	*q;
	Chan	*c;

	char	user[28];

	/*
	 * accounting is slightly wrong, since qio can't tell us if
	 * a bwrite blocked and we check qfull outside of q locking.
	 */
	int	block;
	uvlong	blocks;
	uvlong	blockfastticks;
};

void
miorproc(void *v)
{
	uvlong t;
	Block *b;
	Mio *m;

	m = v;
	if(waserror()){
		/* channel was closed or the queue was closed */
		pexit(up->errstr, 1);
	}
	t = 0;	/* silence compiler */
	for(;;){
		b = m->c->dev->bread(m->c, 10000000, 0);
		if(b == nil)
			continue;
		if(qfull(m->q)){
			m->block = 1;
			m->blocks++;
			t = fastticks(nil);
		}
		qbwrite(m->q, b);
		if(m->block){
			m->blockfastticks += t - fastticks(nil);
			m->block = 0;
		}
	}
//	poperror();	/* silence compiler */
}

Chan*
mioattach(char *spec)
{
	Chan *c;

	if(spec != nil && *spec != 0)
		error(Enodev);
	c = devattach('~', spec);
	mkqid(&c->qid, Qzero, 0, QTDIR);
	return c;
}

static int
·gen(Chan *c, char*, Dirtab *, int, int s, Dir *d)
{
	Qid q;

	q = (Qid){0, 0, QTFILE};
	switch(Type(c->qid)){
	case Qtopdir:
		if(s == DEVDOTDOT){
			q.path = QID(0, Qtopdir);
			q.type = QTDIR;
			devdir(c, q, "#~", 0, eve, 0555, d);
			return 1;
		}
		switch(s){
		case 0:
		case 1:
			q.path = QID(0, Qtopdir+1+s);
			devdir(c, c->qid, names[Qtopdir+1+s], 0, eve, 0660, d);
			return 1;
		}
		s -= 2;
		if(s > 0)
			return -1;
		q.path = QID(0, Qconvdir);
		q.type = QTDIR;
		devdir(c, q, "zotty", 0, eve, 0555, d);
		return 1;
	case Qconvdir:
		if(s == DEVDOTDOT){
			q.path = QID(0, Qconvdir);
			q.type = QTDIR;
			devdir(c, q, "mio", 0, eve, 0555, d);
			return 1;
		}
		if(s < 0 || s > 1)
			return -1;
		q.path = QID(Conv(c->qid), Qconvdir+s+1);
		devdir(c, q, names[Qconvdir+s+1], 0, "user", 0660, d);
		return 1;
	default:
		devdir(c, c->qid, names[Type(c->qid)], 0, "user", 0660, d);
		return 1;
	}
}

static int
miogen(Chan *c, char*, Dirtab*, int, int s, Dir *d)
{
	int type;
	Qid q;

	switch(type = Type(c->qid)){
	case Qzero:
		switch(s){
		case DEVDOTDOT:
			q = (Qid){QID(0, Qtopdir), 0, QTDIR};
			devdir(c, q, names[Qtopdir], 0, eve, 0555, d);
			return 1;
		case 0:
print("topdir+s = %d\n", Qtopdir+s);
			q = (Qid){QID(0, Qtopdir+s), 0, QTFILE};
q.path = Qtopdir+s;
			devdir(c, c->qid, names[Qtopdir+s], 0, eve, 0660, d);
			return 1;
		default:
			return -1;
		}
	case Qtopdir:
print("Qtopdirchild\n");
		switch(s){
		case DEVDOTDOT:
			q = (Qid){QID(0, type-1), 0, QTDIR};
			devdir(c, q, names[type-1], 0, eve, 0555, d);
			return 1;
		case 0:
		case 1:
			devdir(c, c->qid, names[type+s+1], 0, eve, 0660, d);
			return 1;
		default:
			return -1;
		}
	default:
print("def case %llux\n", c->qid.path);
		devdir(c, c->qid, names[Type(c->qid)], 0, "user", 0660, d);
		return 1;
	}
}

static Walkqid*
miowalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, miogen);
}

static long
miostat(Chan *c, uchar *db, long n)
{
	return devstat(c, db, n, nil, 0, miogen);
}

static Chan*
mioopen(Chan *c, int omode)
{
	return devopen(c, omode, nil, 0, miogen);
}

static void
miocreate(Chan*, char*, int, int)
{
	error(Eperm);
}

static void
mioclose(Chan*)
{
}

static long
mioread(Chan *c, void *buf, long n, vlong offset)
{
	if(c->qid.type & QTDIR)
		return devdirread(c, buf, n, nil, 0, miogen);

	USED(buf, n, offset);
	error("not done");
	return -1;
}

static long
miowrite(Chan *c, void* buf, long n, vlong offset)
{
	USED(c, buf, n, offset);
	error("not done");
	return -1;
}

Dev miodevtab = {
	'~',
	"mio",

	devreset,
	devinit,
	devshutdown,
	mioattach,
	miowalk,
	miostat,
	mioopen,
	miocreate,
	mioclose,
	mioread,
	devbread,
	miowrite,
	devbwrite,
	devremove,
	devwstat,
};
