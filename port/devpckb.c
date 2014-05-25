#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

enum {
	Spec=		0xF800,		/* Unicode private space */
	PF=		Spec|0x20,	/* num pad function key */
	View=		Spec|0x00,	/* view (shift window up) */
	KF=		0xF000,		/* function key (begin Unicode private space) */
	Shift=		Spec|0x60,
	Break=		Spec|0x61,
	Ctrl=		Spec|0x62,
	Latin=		Spec|0x63,
	Caps=		Spec|0x64,
	Num=		Spec|0x65,
	Middle=		Spec|0x66,
	Altgr=		Spec|0x67,
	Gui=		Spec|0x68,
	Kmouse=		Spec|0x100,
	No=		0x00,		/* peter */

	Home=		KF|13,		/* failure of vision; collides with f keys */
	Up=		KF|14,
	Pgup=		KF|15,
	Print=		KF|16,
	Left=		KF|17,
	Right=		KF|18,
	End=		KF|24,
	Down=		View,
	Pgdown=		KF|19,
	Ins=		KF|20,
	Del=		0x7F,
	Scroll=		KF|21,

	Nscan=	128,

	Nscans=	5,
};

/*
 * pc keyboard emulation
 */
typedef struct K K;
struct K {
	int	esc1;
	int	esc2;
	int	alt;
	int	altgr;
	int	ctl;
	int	caps;
	int	num;
	int	scroll;
	int	kana;
	int	shift;
	int	lastc;
	int	collecting;
	int	nk;
	Rune	kc[5];
	int	buttons;
	void	(*msg)(char*);
};

struct Kbscan {
	int	use;
	Queue	*q;
	K;
};

/*
 * The codes at 0x79 and 0x7b are produced by the PFU Happy Hacking keyboard.
 * A 'standard' keyboard doesn't produce anything above 0x58.
 */
Rune kbtab[Nscan] =
{
[0x00]	No,	0x1b,	'1',	'2',	'3',	'4',	'5',	'6',
[0x08]	'7',	'8',	'9',	'0',	'-',	'=',	'\b',	'\t',
[0x10]	'q',	'w',	'e',	'r',	't',	'y',	'u',	'i',
[0x18]	'o',	'p',	'[',	']',	'\n',	Ctrl,	'a',	's',
[0x20]	'd',	'f',	'g',	'h',	'j',	'k',	'l',	';',
[0x28]	'\'',	'`',	Shift,	'\\',	'z',	'x',	'c',	'v',
[0x30]	'b',	'n',	'm',	',',	'.',	'/',	Shift,	'*',
[0x38]	Latin,	' ',	Ctrl,	KF|1,	KF|2,	KF|3,	KF|4,	KF|5,
[0x40]	KF|6,	KF|7,	KF|8,	KF|9,	KF|10,	Num,	Scroll,	'7',
[0x48]	'8',	'9',	'-',	'4',	'5',	'6',	'+',	'1',
[0x50]	'2',	'3',	'0',	'.',	No,	No,	No,	KF|11,
[0x58]	KF|12,	No,	No,	No,	No,	No,	No,	No,
[0x60]	No,	No,	No,	No,	KF|13,	KF|14,	KF|15,	KF|16,
[0x68]	KF|17,	KF|18,	KF|19,	KF|20,	KF|21,	KF|22,	KF|23,	KF|24,
[0x70]	No,	No,	No,	No,	No,	No,	No,	No,
[0x78]	No,	View,	No,	Up,	No,	No,	No,	No,
};

Rune kbtabshift[Nscan] =
{
[0x00]	No,	0x1b,	'!',	'@',	'#',	'$',	'%',	'^',
[0x08]	'&',	'*',	'(',	')',	'_',	'+',	'\b',	'\t',
[0x10]	'Q',	'W',	'E',	'R',	'T',	'Y',	'U',	'I',
[0x18]	'O',	'P',	'{',	'}',	'\n',	Ctrl,	'A',	'S',
[0x20]	'D',	'F',	'G',	'H',	'J',	'K',	'L',	':',
[0x28]	'"',	'~',	Shift,	'|',	'Z',	'X',	'C',	'V',
[0x30]	'B',	'N',	'M',	'<',	'>',	'?',	Shift,	'*',
[0x38]	Latin,	' ',	Ctrl,	KF|1,	KF|2,	KF|3,	KF|4,	KF|5,
[0x40]	KF|6,	KF|7,	KF|8,	KF|9,	KF|10,	Num,	Scroll,	'7',
[0x48]	'8',	'9',	'-',	'4',	'5',	'6',	'+',	'1',
[0x50]	'2',	'3',	'0',	'.',	No,	No,	No,	KF|11,
[0x58]	KF|12,	No,	No,	No,	No,	No,	No,	No,
[0x60]	No,	No,	No,	No,	KF|13,	KF|14,	KF|15,	KF|16,
[0x68]	KF|17,	KF|18,	KF|19,	KF|20,	KF|21,	KF|22,	KF|23,	KF|24,
[0x70]	No,	No,	No,	No,	No,	No,	No,	No,
[0x78]	No,	Up,	No,	Up,	No,	No,	No,	No,
};

Rune kbtabesc1[Nscan] =
{
[0x00]	No,	No,	No,	No,	No,	No,	No,	No,
[0x08]	No,	No,	No,	No,	No,	No,	No,	No,
[0x10]	No,	No,	No,	No,	No,	No,	No,	No,
[0x18]	No,	No,	No,	No,	'\n',	Ctrl,	No,	No,
[0x20]	No,	No,	No,	No,	No,	No,	No,	No,
[0x28]	No,	No,	Shift,	No,	No,	No,	No,	No,
[0x30]	No,	No,	No,	No,	No,	'/',	No,	Print,
[0x38]	Altgr,	No,	No,	No,	No,	No,	No,	No,
[0x40]	No,	No,	No,	No,	No,	No,	Break,	Home,
[0x48]	Up,	Pgup,	No,	Left,	No,	Right,	No,	End,
[0x50]	Down,	Pgdown,	Ins,	Del,	No,	No,	No,	No,
[0x58]	No,	No,	No,	Gui,	Gui,	No,	No,	No,
[0x60]	No,	No,	No,	No,	No,	No,	No,	No,
[0x68]	No,	No,	No,	No,	No,	No,	No,	No,
[0x70]	No,	No,	No,	No,	No,	No,	No,	No,
[0x78]	No,	Up,	No,	No,	No,	No,	No,	No,
};

Rune kbtabaltgr[Nscan] =
{
[0x00]	No,	No,	No,	No,	No,	No,	No,	No,
[0x08]	No,	No,	No,	No,	No,	No,	No,	No,
[0x10]	No,	No,	No,	No,	No,	No,	No,	No,
[0x18]	No,	No,	No,	No,	'\n',	Ctrl,	No,	No,
[0x20]	No,	No,	No,	No,	No,	No,	No,	No,
[0x28]	No,	No,	Shift,	No,	No,	No,	No,	No,
[0x30]	No,	No,	No,	No,	No,	'/',	No,	Print,
[0x38]	Altgr,	No,	No,	No,	No,	No,	No,	No,
[0x40]	No,	No,	No,	No,	No,	No,	Break,	Home,
[0x48]	Up,	Pgup,	No,	Left,	No,	Right,	No,	End,
[0x50]	Down,	Pgdown,	Ins,	Del,	No,	No,	No,	No,
[0x58]	No,	No,	No,	No,	No,	No,	No,	No,
[0x60]	No,	No,	No,	No,	No,	No,	No,	No,
[0x68]	No,	No,	No,	No,	No,	No,	No,	No,
[0x70]	No,	No,	No,	No,	No,	No,	No,	No,
[0x78]	No,	Up,	No,	No,	No,	No,	No,	No,
};

Rune kbtabctrl[Nscan] =
{
[0x00]	No,	'', 	'', 	'', 	'', 	'', 	'', 	'', 
[0x08]	'', 	'', 	'', 	'', 	'', 	'', 	'\b',	'\t',
[0x10]	'', 	'', 	'', 	'', 	'', 	'', 	'', 	'\t',
[0x18]	'', 	'', 	'', 	'', 	'\n',	Ctrl,	'', 	'', 
[0x20]	'', 	'', 	'', 	'\b',	'\n',	'', 	'', 	'', 
[0x28]	'', 	No, 	Shift,	'', 	'', 	'', 	'', 	'', 
[0x30]	'', 	'', 	'', 	'', 	'', 	'', 	Shift,	'\n',
[0x38]	Latin,	No, 	Ctrl,	'', 	'', 	'', 	'', 	'', 
[0x40]	'', 	'', 	'', 	'', 	'', 	'', 	'', 	'', 
[0x48]	'', 	'', 	'', 	'', 	'', 	'', 	'', 	'', 
[0x50]	'', 	'', 	'', 	'', 	No,	No,	No,	'', 
[0x58]	'', 	No,	No,	No,	No,	No,	No,	No,
[0x60]	No,	No,	No,	No,	No,	No,	No,	No,
[0x68]	No,	No,	No,	No,	No,	No,	No,	No,
[0x70]	No,	No,	No,	No,	No,	No,	No,	No,
[0x78]	No,	'', 	No,	'\b',	No,	No,	No,	No,
};

static Queue *kbdq;

int mouseshifted;
int kdebug;
void (*kbdmouse)(int);

static Kbscan scans[Nscans];	/* kernel and external scan code state */

static void
kbmsg(Kbscan *s, char *buf, int n)
{
	Block *b;

	if(s->msg != nil){
		s->msg(buf);
		return;
	}
	if(s->q != nil){
		/* drop messages if necessary */
		while((b = qget(s->q)) != nil)
			freeb(b);
			
		if(n > 64)
			n = 64;
		b = allocb(n);
		memmove(b->rp, buf, n);
		b->wp += n;
		qpass(s->q, b);
	}
}

static void
ledmsg(Kbscan *s)
{
	char buf[64];
	int n;

	n = snprint(buf, sizeof buf, "L %c%c%c%c\n",
		s->caps? 'c' : ' ',
		s->num? 'n' : ' ',
		s->scroll? 's' : ' ',
		s->kana? 'k' : ' ');
	kbmsg(s, buf, n);
}

/*
 * Scan code processing
 */
void
kbdputsc(int c, Kbscan *kbscan)
{
	int i, lastc, keyup;

	if(kbscan == nil){
		print("scan is nil from %#p\n", getcallerpc(&c));
		return;
	}

	if(kdebug)
		print("%ld: sc %x ms %d\n", kbscan-scans, c, mouseshifted);
	/*
	 *  e0's is the first of a 2 character sequence, e1 the first
	 *  of a 3 character sequence (on the safari)
	 */
	if(c == 0xe0){
		kbscan->esc1 = 1;
		return;
	} else if(c == 0xe1){
		kbscan->esc2 = 2;
		return;
	}

	keyup = c & 0x80;
	c &= 0x7f;

	if(kbscan->esc1){
		c = kbtabesc1[c];
		kbscan->esc1 = 0;
	} else if(kbscan->esc2){
		kbscan->esc2--;
		return;
	} else if(kbscan->shift)
		c = kbtabshift[c];
	else if(kbscan->altgr)
		c = kbtabaltgr[c];
	else if(kbscan->ctl)
		c = kbtabctrl[c];
	else
		c = kbtab[c];

	if(kbscan->caps && c<='z' && c>='a')
		c += 'A' - 'a';

	/*
	 *  keyup only important for shifts
	 */
	if(keyup){
		switch(c){
		case Latin:
			kbscan->alt = 0;
			break;
		case Shift:
			kbscan->shift = 0;
			mouseshifted = 0;
			break;
		case Ctrl:
			kbscan->ctl = 0;
			break;
		case Altgr:
			kbscan->altgr = 0;
			break;
		case Kmouse|1:
		case Kmouse|2:
		case Kmouse|3:
		case Kmouse|4:
		case Kmouse|5:
			kbscan->buttons &= ~(1<<(c-Kmouse-1));
			if(kbdmouse)
				kbdmouse(kbscan->buttons);
			break;
		}
		return;
	}

	/*
	 *  normal character
	 */
	lastc = kbscan->lastc;
	kbscan->lastc = c;
	if(!(c & (Spec|KF))){
		if(kbscan->ctl)
			if(kbscan->alt && c == Del)
				exit(0);
		if(!kbscan->collecting){
			kbdputc(kbdq, c);
			return;
		}
		kbscan->kc[kbscan->nk++] = c;
		c = latin1(kbscan->kc, kbscan->nk);
		if(c < -1)	/* need more keystrokes */
			return;
		if(c != -1)	/* valid sequence */
			kbdputc(kbdq, c);
		else	/* dump characters */
			for(i=0; i<kbscan->nk; i++)
				kbdputc(kbdq, kbscan->kc[i]);
		kbscan->nk = 0;
		kbscan->collecting = 0;
		return;
	} else {
		switch(c){
		case Caps:
			kbscan->caps ^= 1;
			ledmsg(kbscan);
			return;
		case Scroll:
			kbscan->scroll ^= 1;
			ledmsg(kbscan);
			return;
		case Num:
			kbscan->num ^= 1;
			ledmsg(kbscan);
			return;
		case Shift:
			kbscan->shift = 1;
			mouseshifted = 1;
			return;
		case Latin:
			kbscan->alt = 1;
			/*
			 * VMware and Qemu use Ctl-Alt as the key combination
			 * to make the VM give up keyboard and mouse focus.
			 * Iogear kvm use Ctl followed by Alt as their special key.
			 * This has the unfortunate side effect that when you
			 * come back into focus, Plan 9 thinks you want to type
			 * a compose sequence (you just typed alt). 
			 *
			 * As a clumsy hack around this, we look for ctl-alt or
			 * ctl followed by alt  and don't treat it as the start of a
			 * compose sequence.
			 */
			if(lastc != Ctrl && lastc != Shift && !kbscan->ctl){
				kbscan->collecting = 1;
				kbscan->nk = 0;
			}
			return;
		case Ctrl:
			kbscan->ctl = 1;
			return;
		case Altgr:
			kbscan->altgr = 1;
			return;
		case Kmouse|1:
		case Kmouse|2:
		case Kmouse|3:
		case Kmouse|4:
		case Kmouse|5:
			kbscan->buttons |= 1<<(c-Kmouse-1);
			if(kbdmouse)
				kbdmouse(kbscan->buttons);
			return;
		case KF|11:
			print("kbd debug on, F12 turns it off\n");
			kdebug = 1;
			break;
		case KF|12:
			kdebug = 0;
			break;
		}
	}
	kbdputc(kbdq, c);
}

Kbscan*
kbdnewscan(void (*f)(char*))
{
	int i;
	Kbscan *s;

	for(i = 0; i < nelem(scans); i++){
		s = scans+i;
		if(cas(&s->use, 0, 1)){
			memset(&s->K, 0, sizeof(K));
			s->msg = f;
			if(s->msg == nil){
				if(s->q == nil)
					s->q = qopen(1024, Qmsg, nil, nil);
				else
					qreopen(s->q);
			}
			ledmsg(s);
			return s;
		}
	}
	print("too many keyboards\n");
	return nil;
}

void
kbdfreescan(Kbscan *s)
{
	if(s == nil)
		return;
	assert(s->use == 1);
	if(s->q != nil)
		qclose(s->q);
	s->use = 0;
}

void
kbdputmap(ushort m, ushort scanc, Rune r)
{
	if(scanc >= Nscan)
		error(Ebadarg);
	switch(m) {
	default:
		error(Ebadarg);
	case 0:
		kbtab[scanc] = r;
		break;
	case 1:
		kbtabshift[scanc] = r;
		break;
	case 2:
		kbtabesc1[scanc] = r;
		break;
	case 3:
		kbtabaltgr[scanc] = r;
		break;
	case 4:	
		kbtabctrl[scanc] = r;
		break;
	}
}

int
kbdgetmap(uint offset, int *t, int *sc, Rune *r)
{
	if ((int)offset < 0)
		error(Ebadarg);
	*t = offset/Nscan;
	*sc = offset%Nscan;
	switch(*t) {
	default:
		return 0;
	case 0:
		*r = kbtab[*sc];
		return 1;
	case 1:
		*r = kbtabshift[*sc];
		return 1;
	case 2:
		*r = kbtabesc1[*sc];
		return 1;
	case 3:
		*r = kbtabaltgr[*sc];
		return 1;
	case 4:
		*r = kbtabctrl[*sc];
		return 1;
	}
}

void
kbdenable(void)
{
	kbdq = qopen(4*1024, 0, 0, 0);
	if(kbdq == nil)
		panic("kbdinit");
	qnoblock(kbdq, 1);
	addkbdq(kbdq, -1);
}

enum {
	Qdir,
	Qkbin,
	Qkbmap,
};

Dirtab kbintab[] = {
	".",	{Qdir, 0, QTDIR},	0,	0555,
	"kbin",	{Qkbin, 0},		0,	0600,
	"kbmap",	{Qkbmap, 0},		0,	0600,
};

static Chan *
kbinattach(char *spec)
{
	return devattach(L'Ι', spec);
}

static Walkqid*
kbinwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, kbintab, nelem(kbintab), devgen);
}

static long
kbinstat(Chan *c, uchar *dp, long n)
{
	return devstat(c, dp, n, kbintab, nelem(kbintab), devgen);
}

static Chan*
kbinopen(Chan *c, int omode)
{
	if(!iseve())
		error(Eperm);
	switch((int)c->qid.path){
	case Qkbin:
		c->aux = kbdnewscan(nil);
		if(c->aux == nil)
			error("exhaused");
		break;
	case Qkbmap:
		c->aux = nil;
		break;
	}
	return devopen(c, omode, kbintab, nelem(kbintab), devgen);
}

static void
kbinclose(Chan *c)
{
	switch(c->qid.path){
	case Qkbin:
		kbdfreescan(c->aux);
		break;
	case Qkbmap:
		free(c->aux);
		break;
	}
	c->aux = nil;
}

enum {
	Kblinelen	= 3*NUMSIZE+1,
};
extern char* smprint(char*, ...);
static long
kbinread(Chan *c, void *a, long n, vlong offset)
{
	char *p, buf[Kblinelen+1];
	int t, sc;
	Rune r;
	Kbscan *s;

	if(c->qid.type == QTDIR)
		return devdirread(c, a, n, kbintab, nelem(kbintab), devgen);
	switch(c->qid.path){
	case Qkbin:
		s = c->aux;
		n = qread(s->q, a, n);
		break;
	case Qkbmap:
		if(kbdgetmap(offset/Kblinelen, &t, &sc, &r) == 0){
			n = 0;
			break;
		}
		p = buf;
		p += readnum(0, buf, NUMSIZE, t, NUMSIZE);
		p += readnum(0, p, NUMSIZE, sc, NUMSIZE);
		p += readnum(0, p, NUMSIZE, r, NUMSIZE);
		*p++ = '\n';
		*p = 0;
		n = readstr(offset%Kblinelen, a, n, buf);
		break;
	default:
		error("botch");
	}
	return n;
}

static int
addline(char *line)
{
	char *f[4], *p, arg;
	int m, key;
	Rune r;

	if(*line == 0 || *line == '#')
		return 0;
	if(getfields(line, f, nelem(f), 1, "\t\r\n ") != 3)
		return -1;
	m = strtoul(f[0], nil, 0);
	key = strtoul(f[1], nil, 0);
	r = 0;
	p = f[2];
	if(*p == 0)
		return -1;
	arg = p[1];
	if(*p == '\'' && arg != 0)
		chartorune(&r, p+1);
	else if(*p == '^' && arg != 0){
		chartorune(&r, p+1);
		if(r >= 0x40 && r < 0x60)
			r -= 0x40;
		else
			return -1;
	}else if(*p == 'M' && arg >= '1' && arg <= '5')
		r = 0xF900+p[1]-'0';
	else if(*p>='0' && *p<='9') /* includes 0x... */
		r = strtoul(p, nil, 0);
	else
		return -1;
	kbdputmap(m, key, r);
	return 0;
}

static long
kbinwrite(Chan *c, void *a, long n, vlong)
{
	char *p, *q, buf[100];
	uchar *u;
	int i, l;

	if(c->qid.type == QTDIR)
		error(Eisdir);
	switch((int)c->qid.path){
	case Qkbin:
		u = a;
		for(i = 0; i < n; i++)
			kbdputsc(u[i], c->aux);
		break;
	case Qkbmap:
		p = a;
		l = n;
		q = buf;
		if(c->aux){
			strncpy(buf, c->aux, sizeof buf-1);
			q = buf+strlen(buf);
			free(c->aux);
			c->aux = nil;
		}
		for(; l > 0; l--){
			if(q - buf == sizeof buf)
				error(Ebadarg);
			*q = *p++;
			if(*q == '\n'){
				*q = 0;
				if(addline(buf) == -1)
					error(Ebadarg);
				q = buf;
			}
			q++;
		}
		if(q != buf)
			c->aux = smprint("%.*s", (int)(q-buf), buf);
		break;
	default:
		error(Egreg);
	}
	return n;
}

Dev pckbdevtab = {
	L'Ι',			/* fix.  demanded by usb/kb */
	"kbin",

	devreset,
	devinit,
	devshutdown,
	kbinattach,
	kbinwalk,
	kbinstat,
	kbinopen,
	devcreate,
	kbinclose,
	kbinread,
	devbread,
	kbinwrite,
	devbwrite,
	devremove,
	devwstat,
};
