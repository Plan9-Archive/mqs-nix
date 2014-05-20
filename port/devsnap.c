/*
 * Copyright (c) 2013, Coraid, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Coraid nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CORAID BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

typedef struct Snap Snap;
typedef struct Store Store;
typedef struct SDB SDB;

enum {
	Magic = 1000006,

	SecSize = 512,
	PperSec = SecSize/sizeof(uvlong),
	BlkSize = 1024*1024,
	PperBlk = BlkSize/sizeof(uvlong),
	MaxStores = 128,
	MaxSnap = 1024,

	QTdot = 0,
	QTctl,
	QTdata,

	CMbind = 0,
	CMream,
	CMrevert,
	CMsnap,
	CMunbind,

	SFvalid = 1<<0,
	SFronly = 1<<1,
};

struct Snap {
	QLock;
	Qid qid;
	Store *st;
	char *name;
	uvlong flags;
	uvlong size;
	uvlong inuse;
	uvlong alpha;
	uvlong beta;
	uvlong nmap;
	uvlong next;
	uvlong mapmap[1024];
	uvlong mblk;
	uvlong *pi;
};

struct Store {
	Chan *dchan;
	char *bpath;
	int nsnaps;
	Snap *snaps;
};

struct SDB {
	uvlong magic;
	uvlong flags;
	uvlong size;
	uvlong inuse;
	uvlong alpha;
	uvlong beta;
	uvlong nmap;
	uvlong next;
	char name[128];
	uvlong mapmap[1024];	/* Good for 128TB */
};

enum {
	SDBSec = (sizeof(SDB) + SecSize - 1) / SecSize,
};

static Store stores[MaxStores];

static Qid dotqid = {QTdot, 0, QTDIR};
static Qid ctlqid = {QTctl, 0, QTFILE};
static Cmdtab snapcmd[] = {
	{CMbind, "bind", 2},
	{CMream, "ream", 3},
	{CMrevert, "revert", 3},
	{CMsnap, "snap", 3},
	{CMunbind, "unbind", 2},
};

static void
writesdb(Snap *s)
{
	SDB *sblk;

	sblk = malloc(SDBSec * SecSize);
	if(waserror()) {
		free(sblk);
		nexterror();
	}
	sblk->magic = Magic;
	sblk->flags = s->flags;
	sblk->size = s->size;
	sblk->inuse = s->inuse;
	sblk->alpha = s->alpha;
	sblk->beta = s->beta;
	sblk->nmap = s->nmap;
	sblk->next = s->next;
	memmove(sblk->mapmap, s->mapmap, nelem(s->mapmap) * sizeof(uvlong));
	strncpy(sblk->name, s->name, 127);
	s->st->dchan->dev->write(s->st->dchan,sblk, SDBSec * SecSize, s->alpha * BlkSize);
	poperror();
	free(sblk);
}

static uvlong
updmap(Snap *s, uvlong blkno, uvlong mblk)
{
	uvlong *msec;
	uvlong idx, sec;

	idx = blkno % PperBlk;
	if(mblk < s->alpha) {
		mblk = s->beta;
		s->mblk = mblk;
		++s->beta;
		s->mapmap[idx/PperBlk] = mblk;
		s->pi[idx] = s->beta;
		s->st->dchan->dev->write(s->st->dchan,
			s->pi, BlkSize, mblk * BlkSize);
	}
	else {
		s->pi[idx] = s->beta;
		sec = idx / PperSec;
		msec = s->pi + sec * PperSec;
		s->st->dchan->dev->write(s->st->dchan,
			msec, SecSize, mblk * BlkSize + sec * SecSize);
	}
	return mblk;
}

static uvlong
l2p(Snap *s, uvlong l, int doalloc)
{
	uchar *tblk;
	uvlong blkno, mblk, p;

	blkno = l / BlkSize;
	mblk = s->mapmap[blkno/PperBlk];
	qlock(s);
	if(waserror()) {
		qunlock(s);
		nexterror();
	}
	if(s->pi == nil)
		s->pi = malloc(BlkSize);
	if(mblk == 0) {
		if(doalloc) {
			mblk = s->beta;
			s->mapmap[blkno/PperBlk] = mblk;
			s->mblk = mblk;
			++s->beta;
			memset(s->pi, 0, BlkSize);
		}
		else {
			poperror();
			qunlock(s);
			return 0;
		}
	}
	if(mblk != s->mblk) {
		s->st->dchan->dev->read(s->st->dchan,s->pi, BlkSize, mblk * BlkSize);
		s->mblk = mblk;
	}
	p = s->pi[blkno % PperBlk];
	if(p == 0) {
		if(!doalloc) {
			poperror();
			qunlock(s);
			return 0;
		}
		updmap(s, blkno, mblk);
		p = s->beta;
		++s->beta;
		++s->inuse;
		writesdb(s);
	}
	else if(p < s->alpha && doalloc) {
		/*
		 * Do COW
		 */
		updmap(s, blkno, mblk);
		tblk = malloc(BlkSize);
		if(waserror()) {
			free(tblk);
			nexterror();
		}
		s->st->dchan->dev->read(s->st->dchan,tblk, BlkSize, p * BlkSize);
		s->st->dchan->dev->write(s->st->dchan,tblk, BlkSize, s->beta * BlkSize);
		poperror();
		free(tblk);
		p = s->beta;
		++s->beta;
		writesdb(s);
	}
	poperror();
	qunlock(s);
	p *= BlkSize;
	p += l % BlkSize;
	return p;
}

static int
snapgen(Chan *c, char *name, Dirtab *, int, int i, Dir *dp)
{
	int j, mode;

	if(i == DEVDOTDOT)
		devdir(c, dotqid, ".", 0, eve, 0777, dp);
	else {
		if(name) {
			if(strcmp(name, "ctl") == 0)
				devdir(c, ctlqid, "ctl", 0, eve, 0664, dp);
			else {
				for(j = 0; j < stores[0].nsnaps && strcmp(name, stores[0].snaps[j].name) != 0; ++j) ;
				if(j >= stores[0].nsnaps)
					return -1;
				if(stores[0].snaps[j].flags & SFronly)
					mode = 0444;
				else
					mode = 0664;
				devdir(c, stores[0].snaps[j].qid, stores[0].snaps[j].name,
					stores[0].snaps[j].size, eve, mode, dp);
			}
		}
		else {
			++i;
			if(i == 1)
				devdir(c, ctlqid, "ctl", 0, eve, 0664, dp);
			else {
				i -= QTdata;
				if(i >= stores[0].nsnaps)
					return -1;
				if(!(stores[0].snaps[i].flags & SFvalid))
					return 0;
				if(stores[0].snaps[i].flags & SFronly)
					mode = 0444;
				else
					mode = 0664;
				devdir(c, stores[0].snaps[i].qid, stores[0].snaps[i].name,
					stores[0].snaps[i].size, eve, mode, dp);
			}
		}
	}
	return 1;
}

static Chan *
snapattach(char *spec)
{
	return devattach(L'ℙ', spec);
}

static Walkqid *
snapwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, snapgen);
}

static long
snapstat(Chan *c, uchar *dp, long n)
{
	return devstat(c, dp, n, nil, 0, snapgen);
}

static Chan *
snapopen(Chan *c, int omode)
{
	return devopen(c, omode, nil, 0, snapgen);
}

static void
snapclose(Chan *)
{
}

static long
snaprctl(char *a, long n, vlong off)
{
	char *p, *e;
	int i;

	if(off)
		return 0;
	p = a;
	e = a + n;
	for(i = 0; i < stores[0].nsnaps; ++i)
		p = seprint(p, e, "%d %s %s %ullx %ulld %ulld %ulld\n", i,
			stores[0].snaps[i].name, stores[0].bpath, stores[0].snaps[i].flags,
			stores[0].snaps[i].inuse, stores[0].snaps[i].alpha, stores[0].snaps[i].beta);
	return p-a;
}

static long
snapread(Chan *c, void *a, long n, vlong off)
{
	Chan *sc;
	char *p;
	uvlong qpath, poff, loff;
	long tot, m;

	qpath = c->qid.path;
	if(qpath == QTdot)
		return devdirread(c, a, n, nil, 0, snapgen);
	if(qpath == QTctl)
		return snaprctl(a, n, off);
	qpath -= QTdata;
	if(qpath >= stores[0].nsnaps)
		error(Enonexist);
	if(!(stores[0].snaps[qpath].flags & SFvalid))
		error(Enonexist);
	if(off >= stores[0].snaps[qpath].size)
		return 0;
	if(n + off >= stores[0].snaps[qpath].size)
		n = stores[0].snaps[qpath].size - off;
	sc = stores[0].dchan;
	p = a;
	loff = off;
	tot = 0;
	while(loff < off + n) {
		m = BlkSize - (loff % BlkSize);
		if(m > n - tot)
			m = n - tot;
		poff = l2p(&stores[0].snaps[qpath], loff, 0);
		if(poff == 0)
			memset(p, 0, m);
		else
			m = sc->dev->read(sc,p, m, poff);
		if(m == 0)
			break;
		tot += m;
		loff += m;
		p += m;
	}
	return tot;
}

static void
snapbind(Cmdbuf *cb)
{
	Chan *c;
	SDB *s;
	Store *st;
	uchar *buf;
	uvlong sb;

	if(stores[0].nsnaps != 0)
		error("multiple binds not yet supported");
	c = namec(cb->f[1], Aopen, ORDWR, 0);
	buf = malloc(SDBSec * SecSize);
	if(waserror()) {
		cclose(c);
		free(buf);
		nexterror();
	}
	st = &stores[0];
	st->dchan = c;
	st->bpath = malloc(strlen(cb->f[1]) + 1);
	strcpy(st->bpath, cb->f[1]);
	st->nsnaps = 0;
	st->snaps = malloc(MaxSnap * sizeof(Snap));
	sb = 0;
	do {
		c->dev->read(c,buf, SDBSec * SecSize, sb * BlkSize);
		s = (SDB *)buf;
		if(s->magic != Magic)
			break;
		if(s->flags & SFvalid) {
			st->snaps[st->nsnaps].st = st;
			st->snaps[st->nsnaps].name = malloc(strlen(s->name) + 1);
			strcpy(st->snaps[st->nsnaps].name, s->name);
			st->snaps[st->nsnaps].flags = s->flags;
			st->snaps[st->nsnaps].size = s->size;
			st->snaps[st->nsnaps].inuse = s->inuse;
			st->snaps[st->nsnaps].alpha = s->alpha;
			st->snaps[st->nsnaps].beta = s->beta;
			st->snaps[st->nsnaps].nmap = s->nmap;
			st->snaps[st->nsnaps].next = s->next;
			memmove(st->snaps[st->nsnaps].mapmap, s->mapmap,
				nelem(s->mapmap) * sizeof(uvlong));
			st->snaps[st->nsnaps].qid.path = st->nsnaps + QTdata;
			st->snaps[st->nsnaps].qid.vers = 0;
			st->snaps[st->nsnaps].qid.type = QTFILE;
			st->snaps[st->nsnaps].pi = nil;
			st->snaps[st->nsnaps].mblk = 0;
			++st->nsnaps;
		}
		sb = s->next;
	} while(sb != 0);
	poperror();
	free(buf);
}

static void
snapream(Cmdbuf *cb)
{
	Dir d;
	Chan *c;
	Store *st;
	uchar *buf;
	int i;

	if(stores[0].nsnaps != 0)
		error("multiple binds not yet supported");
	/* Open the channel */
	st = &stores[0];
	st->snaps = malloc(MaxSnap * sizeof(Snap));
	c = st->dchan = namec(cb->f[2], Aopen, ORDWR, 0);
	buf = malloc(65536);
	if(waserror()) {
		cclose(c);
		free(buf);
		nexterror();
	}
	/* Get the length */
	i = c->dev->stat(c,buf, 65536);
	convM2D(buf, i, &d, nil);
	/* Set the internal store struct */
	st->snaps[0].st = st;
	st->snaps[0].name = malloc(strlen(cb->f[1]) + 1);
	strcpy(st->snaps[0].name, cb->f[1]);
	st->bpath = malloc(strlen(cb->f[2]) + 1);
	strcpy(st->bpath, cb->f[2]);
	st->snaps[0].qid.path = QTdata;
	st->snaps[0].qid.vers = 0;
	st->snaps[0].qid.type = QTFILE;
	st->snaps[0].flags = SFvalid;
	st->snaps[0].size = d.length;
	st->snaps[0].alpha = 0;
	st->snaps[0].nmap = ((st->snaps[0].size + BlkSize - 1)/ BlkSize + PperBlk - 1) / PperBlk;
	st->snaps[0].beta = 1;
	st->snaps[0].inuse = st->snaps[0].beta;
	memset(st->snaps[0].mapmap, 0, nelem(st->snaps[0].mapmap) * sizeof(uvlong));
	writesdb(&st->snaps[0]);
	poperror();
	free(buf);
	st->nsnaps = 1;
}

static void
snaprevert(Cmdbuf *cb)
{
	Store *st;
	int i, j, k, l;

	st = &stores[0];
	for(i = 0; i < st->nsnaps && strcmp(st->snaps[i].name, cb->f[1]) != 0; ++i) ;
	for(j = 0; j < st->nsnaps && strcmp(st->snaps[j].name, cb->f[2]) != 0; ++j) ;
	if(i >= st->nsnaps || j >= st->nsnaps)
		error(Enonexist);
	qlock(&st->snaps[i]);
	qlock(&st->snaps[j]);
	if(waserror()) {
		qunlock(&st->snaps[i]);
		qunlock(&st->snaps[j]);
		nexterror();
	}
	for(k = i; ; ) {
		st->snaps[k].next = 0;
		st->snaps[k].flags &= ~SFvalid;
		writesdb(&st->snaps[k]);
		if(k == j)
			break;
		for(l = 0; l < st->nsnaps && st->snaps[l].next != st->snaps[k].alpha; ++l) ;
		if(l >= st->nsnaps)
			break;
		k = l;
	}
	st->snaps[i].inuse = st->snaps[j].inuse;
	st->snaps[i].alpha = st->snaps[j].alpha;
	st->snaps[i].beta = st->snaps[j].beta;
	memmove(st->snaps[i].mapmap, st->snaps[j].mapmap,
		nelem(st->snaps[i].mapmap) * sizeof(uvlong));
	st->snaps[i].mblk = 0;
	st->snaps[i].flags = SFvalid;
	writesdb(&st->snaps[i]);
	poperror();
	qunlock(&st->snaps[j]);
	qunlock(&st->snaps[i]);
}

static void
snapsnap(Cmdbuf *cb)
{
	Store *st;
	int i, slot;

	st = &stores[0];
	for(i = 0; i < st->nsnaps && strcmp(st->snaps[i].name, cb->f[1]) != 0; ++i) ;
	if(i >= st->nsnaps)
		error(Enonexist);
	for(slot = 0; slot < st->nsnaps && (st->snaps[slot].flags & SFvalid); ++slot) ;
	qlock(&st->snaps[i]);
	qlock(&st->snaps[slot]);
	if(waserror()) {
		qunlock(&st->snaps[i]);
		qunlock(&st->snaps[slot]);
		nexterror();
	}
	/* make a new store */
	st->snaps[slot].st = st;
	st->snaps[slot].qid.path = slot + QTdata;
	st->snaps[slot].qid.vers = 0;
	st->snaps[slot].qid.type = QTFILE;
	free(st->snaps[slot].name);
	st->snaps[slot].name = malloc(strlen(cb->f[2]) + 1);
	strcpy(st->snaps[slot].name, cb->f[2]);
	st->snaps[slot].inuse = st->snaps[i].inuse;
	st->snaps[slot].size = st->snaps[i].inuse * BlkSize;
	st->snaps[slot].flags = st->snaps[i].flags | SFronly;
	st->snaps[slot].alpha = st->snaps[i].alpha;
	st->snaps[slot].beta = st->snaps[i].beta;
	st->snaps[slot].nmap = st->snaps[i].nmap;
	st->snaps[slot].next = st->snaps[i].beta;
	memmove(st->snaps[slot].mapmap, st->snaps[i].mapmap,
		nelem(st->snaps[i].mapmap) * sizeof(uvlong));
	st->snaps[slot].mblk = 0;
	free(st->snaps[slot].pi);
	st->snaps[slot].pi = nil;
	/* adjust the original to point to the new space */
	st->snaps[i].alpha = st->snaps[i].beta;
	st->snaps[i].beta = st->snaps[i].alpha + 1;
	/* write the sdb blocks */
	writesdb(&st->snaps[i]);
	writesdb(&st->snaps[slot]);
	poperror();
	qunlock(&stores[0].snaps[i]);
	qunlock(&stores[0].snaps[slot]);
	if(slot == st->nsnaps)
		++st->nsnaps;
}

static void
snapunbind(Cmdbuf *cb)
{
	Store *st;
	int i;

	for(i = 0; i < MaxStores && (stores[i].bpath == nil || strcmp(stores[i].bpath, cb->f[1]) !=0); ++i) ;
	if(i >= MaxStores)
		error(Enonexist);
	st = &stores[i];
	cclose(st->dchan);
	free(st->bpath);
	for(i = 0; i < st->nsnaps; ++i) {
		free(st->snaps[i].name);
		free(st->snaps[i].pi);
	}
	free(st->snaps);
	st->nsnaps = 0;
}

static long
snapwctl(char *a, long n)
{
	Cmdbuf *cb;
	Cmdtab *ct;

	cb = parsecmd(a, n);
	ct = lookupcmd(cb, snapcmd, nelem(snapcmd));
	switch(ct->index) {
	case CMbind:
		snapbind(cb);
		break;
	case CMream:
		snapream(cb);
		break;
	case CMrevert:
		snaprevert(cb);
		break;
	case CMsnap:
		snapsnap(cb);
		break;
	case CMunbind:
		snapunbind(cb);
		break;
	}
	return n;
}

static long
snapwrite(Chan *c, void *a, long n, vlong off)
{
	Chan *sc;
	Store *st;
	char *p;
	uvlong qpath, poff, loff;
	long tot, m;

	qpath = c->qid.path;
	if(qpath == QTdot)
		error(Eisdir);
	if(qpath == QTctl)
		return snapwctl(a, n);
	qpath -= QTdata;
	if(qpath >= stores[0].nsnaps)
		error(Enonexist);
	st = &stores[0];
	if(!(st->snaps[qpath].flags & SFvalid))
		error(Enonexist);
	if(st->snaps[qpath].flags & SFronly)
		error(Eperm);
	if(off >= st->snaps[qpath].size)
		return 0;
	if(n + off >= st->snaps[qpath].size)
		n = st->snaps[qpath].size - off;
	sc = st->dchan;
	p = a;
	loff = off;
	tot = 0;
	while(loff < off + n) {
		m = BlkSize - (loff % BlkSize);
		if(m > n - tot)
			m = n - tot;
		poff = l2p(&st->snaps[qpath], loff, 1);
		if(poff == 0)
			error("no space");
		m = sc->dev->write(sc,p, m, poff);
		if(m == 0)
			break;
		tot += m;
		loff += m;
		p += m;
	}
	return tot;
}

Dev snapdevtab = {
	L'ℙ',
	"snap",

	devreset,
	devinit,
	devshutdown,
	snapattach,
	snapwalk,
	snapstat,
	snapopen,
	devcreate,
	snapclose,
	snapread,
	devbread,
	snapwrite,
	devbwrite,
	devremove,
	devwstat,
};
