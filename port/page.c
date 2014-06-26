#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

enum
{
	Nstartpgs = 32,
	Nminfree = 3,
	Nfreepgs = 512,
};

enum
{
	Punused = 0,
	Pused,
	Pfreed,
};

#define pghash(daddr)	pga.hash[(daddr>>PGSHIFT)&(PGHSIZE-1)]
Pgalloc pga;		/* new allocator */

char*
seprintpagestats(char *s, char *e)
{
	int i;

	lock(&pga);
	for(i = 0; i < m->npgsz; i++)
		if(m->pgsz[i] != 0)
			s = seprint(s, e, "%ud/%d %dK user pages avail\n",
				pga.pgsza[i].freecount,
				pga.pgsza[i].npages.ref, m->pgsz[i]/KiB);
	unlock(&pga);
	return s;
}

/*
 * Preallocate some pages:
 *  some 2M ones will be used by the first process.
 *  some 1G ones will be allocated for each domain so processes may use them.
 */
void
pageinit(void)
{
	int si, i, color;
	Page *pg;

	DBG("pageinit: npgsz = %d\n", m->npgsz);
	/*
	 * Don't pre-allocate 4K pages, we are not using them anymore.
	 */
	for(si = 1; si < m->npgsz; si++){
		for(i = 0; i < Nstartpgs; i++){
			if(si < 2)
				color = -1;
			else
				color = i;
			pg = pgalloc(m->pgsz[si], color);
			if(pg == nil){
				DBG("pageinit: pgalloc failed. breaking.\n");
				break;	/* don't consume more memory */
			}
			DBG("pageinit: alloced pa %#P sz %#ux color %d\n",
				pg->pa, m->pgsz[si], pg->color);
			lock(&pga);
			pg->ref = 0;
			pagechainhead(pg);
			unlock(&pga);
		}
	}
}

int
getpgszi(usize size)
{
	int si;

	for(si = 0; si < m->npgsz; si++)
		if(size == m->pgsz[si])
			return si;
	return -1;
}

Page*
pgalloc(usize size, int color)
{
	Page *pg;
	int si;

	si = getpgszi(size);
	if(si == -1){
		print("pgalloc: getpgszi %lux %d %#p\n", size, color, getcallerpc(&size));
		return nil;
	}
	if((pg = malloc(sizeof(Page))) == nil){
		DBG("pgalloc: malloc failed\n");
		return nil;
	}
	if((pg->pa = physalloc(size, &color, pg)) == 0){
		DBG("pgalloc: physalloc failed: size %#lux color %d\n", size, color);
		free(pg);
		return nil;
	}
	pg->pgszi = si;	/* size index */
	incref(&pga.pgsza[si].npages);
	pg->color = color;
	pg->ref = 1;
	return pg;
}

void
pgfree(Page* pg)
{
	decref(&pga.pgsza[pg->pgszi].npages);
	physfree(pg->pa, m->pgsz[pg->pgszi]);
	free(pg);
}

void
pageunchain(Page *p)
{
	Pgsza *pa;

	if(canlock(&pga))
		panic("pageunchain");
	pa = &pga.pgsza[p->pgszi];
	if(p->prev)
		p->prev->next = p->next;
	else
		pa->head = p->next;
	if(p->next)
		p->next->prev = p->prev;
	else
		pa->tail = p->prev;
	p->prev = p->next = nil;
	pa->freecount--;
}

void
pagechaintail(Page *p)
{
	Pgsza *pa;

	if(canlock(&pga))
		panic("pagechaintail");
	pa = &pga.pgsza[p->pgszi];
	if(pa->tail) {
		p->prev = pa->tail;
		pa->tail->next = p;
	}
	else {
		pa->head = p;
		p->prev = 0;
	}
	pa->tail = p;
	p->next = 0;
	pa->freecount++;
}

void
pagechainhead(Page *p)
{
	Pgsza *pa;

	if(canlock(&pga))
		panic("pagechainhead");
	pa = &pga.pgsza[p->pgszi];
	if(pa->head) {
		p->next = pa->head;
		pa->head->prev = p;
	}
	else {
		pa->tail = p;
		p->next = nil;
	}
	pa->head = p;
	p->prev = nil;
	pa->freecount++;
}

static Page*
findpg(Page *pl, int color)
{
	Page *p;

	for(p = pl; p != nil; p = p->next)
		if(color == NOCOLOR || p->color == color)
			return p;
	return nil;
}
/*
 * can be called with up == nil during boot.
 */
Page*
newpage(int clear, Segment **s, uintptr va, usize size, int color)
{
	Page *p;
	KMap *k;
	Pgsza *pa;
	int i, dontalloc, si;

	si = getpgszi(size);
	if(si == -1)
		panic("newpage: getpgszi %lux %d %#p", size, color, getcallerpc(&size));
	pa = &pga.pgsza[si];

	lock(&pga);
	/*
	 * Beware, new page may enter a loop even if this loop does not
	 * loop more than once, if the segment is lost and fault calls us
	 * again. Either way, we accept any color if we failed a couple of times.
	 */
	for(i = 0;; i++){
		if(i > 3)
			color = NOCOLOR;

		/*
		 * 1. try to reuse a free one.
		 */
		p = findpg(pa->head, color);
		if(p != nil){
			pageunchain(p);
			break;
		}
		unlock(&pga);

		/*
		 * 2. try to allocate a new one from physical memory
		 */
		p = pgalloc(size, color);
		if(p != nil){
			p->va = va;
			p->daddr = ~0;
			goto Clear;
		}

		/*
		 * 3. out of memory, try with the pager.
		 * but release the segment (if any) while in the pager.
		 */

		dontalloc = 0;
		if(s != nil && *s != nil) {
			qunlock(&((*s)->lk));
			*s = nil;
			dontalloc = 1;
		}

		kickpager(si, color);

		/*
		 * If called from fault and we lost the segment from
		 * underneath don't waste time allocating and freeing
		 * a page. Fault will call newpage again when it has
		 * reacquired the segment locks
		 */
		if(dontalloc)
			return nil;

		lock(&pga);
	}

	lock(p);
	if(p->ref != 0)
		panic("newpage pa %#P", p->pa);

	uncachepage(p);
	p->ref++;
	p->va = va;
	p->modref = 0;
	p->daddr = ~0;
	for(i = 0; i < nelem(p->cachectl); i++)
		p->cachectl[i] = PG_NEWCOL;
	unlock(p);
	unlock(&pga);

Clear:
	if(clear) {
		k = kmap(p);
		memset((void*)VA(k), 0, m->pgsz[p->pgszi]);
		kunmap(k);
	}
	DBG("newpage: va %#p pa %#P pgsz %#ux color %d\n",
		p->va, p->pa, m->pgsz[p->pgszi], p->color);

	return p;
}

void
putpage(Page *p)
{
	Pgsza *pa;
	int rlse;

	lock(&pga);
	lock(p);

	if(p->ref == 0)
		panic("putpage");

	if(--p->ref > 0) {
		unlock(p);
		unlock(&pga);
		return;
	}
	rlse = 0;
	if(p->image != nil)
		pagechaintail(p);
	else{
		/*
		 * Free pages if we have plenty in the free list.
		 */
		pa = &pga.pgsza[p->pgszi];
		if(pa->freecount > Nfreepgs)
			rlse = 1;
		else
			pagechainhead(p);
	}
	if(pga.r.p != nil)
		wakeup(&pga.r);
	unlock(p);
	unlock(&pga);
	if(rlse)
		pgfree(p);
}

/*
 * Get an auxiliary page.
 * Don't do so if less than Nminfree pages.
 * Only used by cache.
 * The interface must specify page size.
 */
Page*
auxpage(usize size, int color)
{
	Page *p;
	Pgsza *pa;
	int si;

	si = getpgszi(size);
	if(si == -1)
		panic("auxpage: getpgszi %lux %d %#p", size, color, getcallerpc(&size));
	lock(&pga);
	pa = &pga.pgsza[si];
	p = pa->head;
	if(pa->freecount < Nminfree){
		unlock(&pga);
		return nil;
	}
	pageunchain(p);
	lock(p);
	if(p->ref != 0)
		panic("auxpage");
	p->ref++;
	uncachepage(p);
	unlock(p);
	unlock(&pga);

	return p;
}

void
copypage(Page *f, Page *t)
{
	KMap *ks, *kd;

	if(f->pgszi != t->pgszi || t->pgszi < 0)
		panic("copypage");
	ks = kmap(f);
	kd = kmap(t);
	memmove((void*)VA(kd), (void*)VA(ks), m->pgsz[t->pgszi]);
	kunmap(ks);
	kunmap(kd);
}

void
uncachepage(Page *p)			/* Always called with a locked page */
{
	Page **l, *f;

	if(p->image == nil)
		return;

	lock(&pga.hashlock);
	l = &pghash(p->daddr);
	for(f = *l; f; f = f->hash){
		if(f == p){
			*l = p->hash;
			goto found;
		}
		l = &f->hash;
	}
	panic("uncachpage");
found:
	unlock(&pga.hashlock);
	putimage(p->image);
	p->image = 0;
	p->daddr = ~0;
}

void
cachepage(Page *p, Image *i)
{
	Page **l;

	/* If this ever happens it should be fixed by calling
	 * uncachepage instead of panic. I think there is a race
	 * with pio in which this can happen. Calling uncachepage is
	 * correct - I just wanted to see if we got here.
	 */
	if(p->image)
		panic("cachepage");

	incref(i);
	lock(&pga.hashlock);
	p->image = i;
	l = &pghash(p->daddr);
	p->hash = *l;
	*l = p;
	unlock(&pga.hashlock);
}

Page *
lookpage(Image *i, ulong daddr)
{
	Page *f;

	lock(&pga.hashlock);
	for(f = pghash(daddr); f; f = f->hash){
		if(f->image == i && f->daddr == daddr){
			unlock(&pga.hashlock);

			lock(&pga);
			lock(f);
			if(f->image != i || f->daddr != daddr){
				unlock(f);
				unlock(&pga);
				return 0;
			}
			if(++f->ref == 1)
				pageunchain(f);
			unlock(&pga);
			unlock(f);

			return f;
		}
	}
	unlock(&pga.hashlock);

	return nil;
}

/* Called from imagereclaim, to try to release Images */
uint
pagereclaim(void)
{
	int lg, n;
	usize sz;
	Page *p;

	lock(&pga);
	sz = 0;
	n = 0;
	for(lg = 0; lg < m->npgsz; lg++){
		for(p = pga.pgsza[lg].tail; p != nil; p = p->prev){
			if(p->image != nil && p->ref == 0 && canlock(p)){
				if(p->ref == 0) {
					n++;
					sz += 1<<m->pgszlg2[lg];
					uncachepage(p);
				}
				unlock(p);
			}
			if(sz >= 20*MiB && n>5)
				break;
		}
	}
	unlock(&pga);
	return sz;
}

Pte*
ptecpy(Segment *s, Pte *old)
{
	Pte *new;
	Page **src, **dst;

	new = ptealloc(s);
	dst = &new->pages[old->first-old->pages];
	new->first = dst;
	for(src = old->first; src <= old->last; src++, dst++)
		if(*src != nil){
			lock(*src);
			(*src)->ref++;
			unlock(*src);
			new->last = dst;
			*dst = *src;
		}

	return new;
}

Pte*
ptealloc(Segment *s)
{
	Pte *new;

	new = smalloc(sizeof(Pte) + sizeof(Page*)*s->ptepertab);
	new->first = &new->pages[s->ptepertab];
	new->last = new->pages;
	return new;
}

void
freepte(Segment *s, Pte *p)
{
	int ref;
	void (*fn)(Page*);
	Page *pt, **pg, **ptop;

	switch(s->type&SG_TYPE) {
	case SG_PHYSICAL:
		fn = s->pseg->pgfree;
		ptop = &p->pages[s->ptepertab];
		if(fn) {
			for(pg = p->pages; pg < ptop; pg++) {
				if(*pg == 0)
					continue;
				(*fn)(*pg);
				*pg = 0;
			}
			break;
		}
		for(pg = p->pages; pg < ptop; pg++) {
			pt = *pg;
			if(pt == nil)
				continue;
			lock(pt);
			ref = --pt->ref;
			unlock(pt);
			if(ref == 0)
				free(pt);
		}
		break;
	default:
		for(pg = p->first; pg <= p->last; pg++)
			if(*pg) {
				putpage(*pg);
				*pg = 0;
			}
	}
	free(p);
}
