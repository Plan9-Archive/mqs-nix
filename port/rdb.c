#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "ureg.h"

#undef DBG
#define DBG	if(0)scrprint
#pragma varargck argpos scrprint 1
static Ureg ureg;

static void
scrprint(char *fmt, ...)
{
	char buf[128];
	va_list va;
	int n;

	va_start(va, fmt);
	n = vseprint(buf, buf+sizeof buf, fmt, va)-buf;
	va_end(va);
	putstrn(buf, n);
}

static char*
getline(void)
{
	static char buf[128];
	int i, c;

	for(;;){
		for(i=0; i<nelem(buf) && (c=uartgetc()) != '\n'; i++){
			DBG("%c...", c);
			buf[i] = c;
		}

		if(i < nelem(buf)){
			buf[i] = 0;
			return buf;
		}
	}
}

static void*
addr(char *s, Ureg *ureg, char **p)
{
	uintptr a;

	a = strtoull(s, p, 16);
	if(a < sizeof(Ureg))
		return ((uchar*)ureg)+a;
	return UINT2PTR(a);
}

static void
talkrdb(Ureg *ureg)
{
	uintptr *a;
	char *p, *req;

	for(;;){
		req = getline();
		switch(*req){
		case 'r':
			a = addr(req+1, ureg, nil);
			DBG("read %#p\n", a);
			iprint("R%p %.*H", a, sizeof(uintptr), a);
			break;

		case 'w':
			a = addr(req+1, ureg, &p);
			*a = strtoull(p, nil, 16);
			iprint("W\n");
			break;
/*
 *		case Tmput:
			n = min[4];
			if(n > 4){
				mesg(Rerr, Ecount);
				break;
			}
			a = addr(min+0);
			scrprint("mput %p\n", a);
			memmove(a, min+5, n);
			mesg(Rmput, mout);
			break;
 *
 */
		default:
			DBG("unknown %c\n", *req);
			iprint("Eunknown message\n");
			break;
		}
	}
}

void
rdb(void)
{
	extern int encodefmt(Fmt*);

	fmtinstall('H', encodefmt);
	synccons();
	splhi();
	iprint("rdb...");
	callwithureg(talkrdb);
}
