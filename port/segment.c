#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

uintmem
segppn(Segment *s, uintmem pa)
{
	uintmem pgsz;

	pgsz = m->pgsz[s->pgszi];
	pa &= ~(pgsz-1);
	return pa;
}

static int
segshift(uintptr base, uintptr top)
{
	int i;
	uintptr v;

	if(m->npgsz == 0)
		panic("segpgsizes");
	v = base|top;
	for(i = m->npgsz-1; i >= 0; i--)
		if((v & m->pgszmask[i]) == 0)
			return m->pgszlg2[i];
	return PGSHIFT;
}

/*
 * if base is aligned to 1G and size is >= 1G and we support 1G pages.
 * this is exceptionally lame.  
 */
int okbigseg[SG_TYPE+1] = {
[SG_BSS]	1,
[SG_SHARED]	1,
[SG_PHYSICAL]	1,
[SG_STACK] 1,
};

Segment*
newseg(int type, uintptr base, uintptr top)
{
	Segment *s;
	int mapsize, t;
	uint pgshift;
	uintptr sz;	/* ptrdiff */

	sz = top - base;
	if((base|top) & (PGSZ-1))
		panic("newseg %#p %#p", base, top);
	t = type&SG_TYPE;
	if(okbigseg[t]){
		pgshift = segshift(base, top);
//		print("newseg: %d: big seg %#p %#p [%d]\n", t, base, top, 1<<pgshift);
	}
	else
//		pgshift = PGSHIFT;
		pgshift = BIGPGSHIFT;		/* so wrong */

	s = smalloc(sizeof(Segment));
	s->ref = 1;
	s->type = type;
	s->base = base;
	s->ptepertab = PTEMAPMEM>>pgshift;
	s->top = top;
	s->size = sz>>pgshift;
	s->pgszi = getpgszi(1<<pgshift);
	if(s->pgszi < 0)
		panic("newseg: getpgszi %d", 1<<pgshift);
	s->sema.prev = &s->sema;
	s->sema.next = &s->sema;
	s->color = NOCOLOR;

	mapsize = HOWMANY(s->size, s->ptepertab);
	DBG("newseg %d %#p %#p; pgshift = %d; pages %ld; mapsize %d\n",
		type, base, top, pgshift, s->size, mapsize);
	if(mapsize > nelem(s->ssegmap)){
		mapsize *= 2;
		s->map = smalloc(mapsize*sizeof(Pte*));
		s->mapsize = mapsize;
	}
	else{
		s->map = s->ssegmap;
		s->mapsize = nelem(s->ssegmap);
	}

	return s;
}

void
putseg(Segment *s)
{
	int n;
	Pte **pp, **emap;
	Image *i;

	if(s == 0)
		return;

	i = s->image;
	if(i != 0) {
		lock(i);
		lock(s);
		n = decref(s);
		if(i->s == s && n == 0)
			i->s = 0;
		unlock(i);
		unlock(s);
	}
	else{
		lock(s);
		n = decref(s);
		unlock(s);
	}

	if(n != 0)
		return;

	qlock(&s->lk);
	if(i)
		putimage(i);

	emap = &s->map[s->mapsize];
	for(pp = s->map; pp < emap; pp++)
		if(*pp)
			freepte(s, *pp);

	qunlock(&s->lk);
	if(s->map != s->ssegmap)
		free(s->map);
	if(s->profile != 0)
		free(s->profile);
//	memset(s, 0x22, sizeof *s);
	free(s);
}

void
relocateseg(Segment *s, uintptr offset)
{
	Page **pg, *x;
	Pte *pte, **p, **endpte;

	endpte = &s->map[s->mapsize];
	for(p = s->map; p < endpte; p++) {
		if(*p == 0)
			continue;
		pte = *p;
		for(pg = pte->first; pg <= pte->last; pg++) {
			if(x = *pg)
				x->va += offset;
		}
	}
}

Segment*
dupseg(Segment **seg, int segno, int share)
{
	int i, size;
	Pte *pte;
	Segment *n, *s;

	SET(n);
	s = seg[segno];

	qlock(&s->lk);
	if(waserror()){
		qunlock(&s->lk);
		nexterror();
	}
	switch(s->type&SG_TYPE) {
	case SG_TEXT:		/* New segment shares pte set */
	case SG_SHARED:
	case SG_PHYSICAL:
		goto sameseg;

	case SG_STACK:
		n = newseg(s->type, s->base, s->top);
		break;

	case SG_BSS:		/* Just copy on write */
		if(share)
			goto sameseg;
		n = newseg(s->type, s->base, s->top);
		break;

	case SG_DATA:		/* Copy on write plus demand load info */
		if(segno == TSEG){
			poperror();
			qunlock(&s->lk);
			return data2txt(s);
		}

		if(share)
			goto sameseg;
		n = newseg(s->type, s->base, s->top);

		incref(s->image);
		n->image = s->image;
		n->fstart = s->fstart;
		n->flen = s->flen;
		n->pgszi = s->pgszi;
		n->color = s->color;
		n->ptepertab = s->ptepertab;
		break;
	}
	size = s->mapsize;
	for(i = 0; i < size; i++)
		if(pte = s->map[i])
			n->map[i] = ptecpy(n, pte);

	n->flushme = s->flushme;
	if(s->ref > 1)
		procflushseg(s);
	poperror();
	qunlock(&s->lk);
	return n;

sameseg:
	incref(s);
	poperror();
	qunlock(&s->lk);
	return s;
}

void
segpage(Segment *s, Page *p)
{
	Pte **pte;
	uintptr soff;
	uintmem pgsz;
	Page **pg;

	if(s->pgszi < 0)
		s->pgszi = p->pgszi;
	if(s->color == NOCOLOR)
		s->color = p->color;
	if(s->pgszi != p->pgszi)
		panic("segpage: s->pgszi != p->pgszi; %d %d", s->pgszi, p->pgszi);

	if(p->va < s->base || p->va >= s->top)
		panic("segpage: p->va < s->base || p->va >= s->top");

	soff = p->va - s->base;
	pte = &s->map[soff/PTEMAPMEM];
	if(*pte == 0)
		*pte = ptealloc(s);
	pgsz = m->pgsz[s->pgszi];
	pg = &(*pte)->pages[(soff&(PTEMAPMEM-1))/pgsz];
	*pg = p;
	if(pg < (*pte)->first)
		(*pte)->first = pg;
	if(pg > (*pte)->last)
		(*pte)->last = pg;
}

/*
 *  called with s->lk locked
 */
void
mfreeseg(Segment *s, uintptr start, uintptr end)
{
	int i, j, size, pages;
	uintptr soff;
	uintmem pgsz;
	Page *pg;
	Page *list;

	pgsz = m->pgsz[s->pgszi];
	pages = (end - start)/pgsz;
	soff = start-s->base;
	j = (soff&(PTEMAPMEM-1))/pgsz;

	size = s->mapsize;
	list = nil;
	for(i = soff/PTEMAPMEM; i < size; i++) {
		if(pages <= 0)
			break;
		if(s->map[i] == 0) {
			pages -= s->ptepertab-j;
			j = 0;
			continue;
		}
		while(j < s->ptepertab) {
			pg = s->map[i]->pages[j];
			/*
			 * We want to zero s->map[i]->page[j] and putpage(pg),
			 * but we have to make sure other processors flush the
			 * entry from their TLBs before the page is freed.
			 * We construct a list of the pages to be freed, zero
			 * the entries, then (below) call procflushseg, and call
			 * putpage on the whole list.
			 */
			if(pg){
				pg->next = list;
				list = pg;
				s->map[i]->pages[j] = nil;
			}
			if(--pages == 0)
				goto out;
			j++;
		}
		j = 0;
	}
out:
	/* flush this seg in all other processes */
	if(s->ref > 1)
		procflushseg(s);

	/* free the pages */
	for(pg = list; pg != nil; pg = list){
		list = list->next;
		putpage(pg);
	}
}

Segment*
isoverlap(Proc* p, uintptr va, usize len)
{
	int i;
	Segment *ns;
	uintptr newtop;

	newtop = va+len;
	for(i = 0; i < NSEG; i++) {
		ns = p->seg[i];
		if(ns == 0)
			continue;
		if((newtop > ns->base && newtop <= ns->top) ||
		   (va >= ns->base && va < ns->top))
			return ns;
	}
	return nil;
}

void
segclock(uintptr pc)
{
	Segment *s;

	s = up->seg[TSEG];
	if(s == nil || s->profile == 0)
		return;

	s->profile[0] += TK2MS(1);
	if(pc >= s->base && pc < s->top) {
		pc -= s->base;
		s->profile[pc>>LRESPROF] += TK2MS(1);
	}
}
