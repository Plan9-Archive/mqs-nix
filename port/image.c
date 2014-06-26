#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#define IHASHSIZE	67
#define ihash(s)	imagealloc.hash[s%IHASHSIZE]

static struct Imagealloc
{
	Lock;
	Image	*mru;			/* head of LRU list */
	Image	*lru;			/* tail of LRU list */
	Image	*hash[IHASHSIZE];
	QLock	ireclaim;		/* mutex on reclaiming free images */
} imagealloc;

static struct {
	int	attachimage;		/* number of attach images */
	int	found;			/* number of images found */
	int	reclaims;			/* times imagereclaim was called */
	uvlong	ticks;			/* total time in the main loop */
	uvlong	maxt;			/* longest time in main loop */
} irstats;

char*
imagestats(char *p, char *e)
{
	p = seprint(p, e, "image reclaims: %d\n", irstats.reclaims);
	p = seprint(p, e, "image µs: %lld\n", fastticks2us(irstats.ticks));
	p = seprint(p, e, "image max µs: %lld\n", fastticks2us(irstats.maxt));
	p = seprint(p, e, "image attachimage: %d\n", irstats.attachimage);
	p = seprint(p, e, "image found: %d\n", irstats.found);
	return p;
}

/*
 * imagealloc and i must be locked.
 */
static void
imageunused(Image *i)
{
	if(i->prev != nil)
		i->prev->next = i->next;
	else
		imagealloc.mru = i->next;
	if(i->next != nil)
		i->next->prev = i->prev;
	else
		imagealloc.lru = i->prev;
	i->next = i->prev = nil;
}

/*
 * imagealloc and i must be locked.
 */
static void
imageused(Image *i)
{
	imageunused(i);
	i->next = imagealloc.mru;
	i->next->prev = i;
	imagealloc.mru = i;
	if(imagealloc.lru == nil)
		imagealloc.lru = i;
}

/*
 * imagealloc must be locked.
 */
static Image*
lruimage(void)
{
	Image *i;

	for(i = imagealloc.lru; i != nil; i = i->prev)
		if(i->c == nil){
			/*
			 * i->c will be set before releasing the
			 * lock on imagealloc, which means it's in use.
			 */
			return i;
		}
	return nil;
}

void
initimage(void)
{
	Image *i, *ie;

	DBG("initimage: %ud images\n", sys->nimage);
	imagealloc.mru = malloc(sys->nimage*sizeof(Image));
	if(imagealloc.mru == nil)
		panic("imagealloc: no memory");
	ie = &imagealloc.mru[sys->nimage];
	for(i = imagealloc.mru; i < ie; i++){
		i->c = nil;
		i->ref = 0;
		i->prev = i-1;
		i->next = i+1;
	}
	imagealloc.mru[0].prev = nil;
	imagealloc.mru[sys->nimage-1].next = nil;
	imagealloc.lru = &imagealloc.mru[sys->nimage-1];
}

/*
 * images may hang around because they have stale pages referring
 * to them.  call pagerreclaim() to do the dirty work.
 */
static uint
imagereclaim(void)
{
	uint sz;
	uvlong ticks0, ticks;

	if(!canqlock(&imagealloc.ireclaim))
		return 0;
	irstats.reclaims++;
	ticks0 = fastticks(nil);

	sz = pagereclaim();

	ticks = fastticks(nil) - ticks0;
	irstats.ticks += ticks;
	if(ticks > irstats.maxt)
		irstats.maxt = ticks;
	DBG("imagereclaim %lludµs\n", fastticks2us(ticks));
	qunlock(&imagealloc.ireclaim);
	return sz;
}

Image*
attachimage(int type, Chan *c, int color, uintptr base, uintptr top)
{
	Image *i, **l;

	lock(&imagealloc);
	irstats.attachimage++;

	/*
	 * Search the image cache for remains of the text from a previous
	 * or currently running incarnation
	 */
	for(i = ihash(c->qid.path); i; i = i->hash) {
		if(c->qid.path == i->qid.path) {
			lock(i);
			if(eqqid(c->qid, i->qid) &&
			   eqqid(c->mqid, i->mqid) &&
			   c->mchan == i->mchan &&
			   c->dev->dc == i->dc) {
				irstats.found++;
				goto found;
			}
			unlock(i);
		}
	}

	/*
	 * imagereclaim dumps pages from the free list which are cached by image
	 * structures. This should free some image structures.
	 */
	while(!(i = lruimage())) {
		unlock(&imagealloc);
		imagereclaim();
		sched();
		lock(&imagealloc);
	}

	lock(i);
	incref(c);
	i->c = c;
	i->dc = c->dev->dc;
	i->qid = c->qid;
	i->mqid = c->mqid;
	i->mchan = c->mchan;
	i->color = color;
	l = &ihash(c->qid.path);
	i->hash = *l;
	*l = i;
found:
	imageused(i);
	unlock(&imagealloc);

	if(i->s == 0) {
		/* Disaster after commit in exec */
		if(waserror()) {
			unlock(i);
			pexit(Enovmem, 1);
		}
		i->s = newseg(type, base, top);
		i->s->image = i;
		i->s->color = color;
		incref(i);
		poperror();
	}
	else
		incref(i->s);

	return i;
}

void
putimage(Image *i)
{
	Chan *c;
	Image *f, **l;

	if(i->notext)
		return;

	if(decref(i) == 0){
		lock(i);
		l = &ihash(i->qid.path);
		mkqid(&i->qid, ~0, ~0, QTFILE);
		unlock(i);
		c = i->c;

		lock(&imagealloc);
		for(f = *l; f; f = f->hash) {
			if(f == i) {
				*l = i->hash;
				break;
			}
			l = &f->hash;
		}
		i->c = nil;		/* flag as unused in lru list */
		unlock(&imagealloc);

		ccloseq(c);		/* won't block */
	}
}
