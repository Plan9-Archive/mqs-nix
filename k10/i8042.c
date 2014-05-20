#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

enum {
	Data=		0x60,		/* data port */

	Status=		0x64,		/* status port */
	 Inready=	0x01,		/*  input character ready */
	 Outbusy=	0x02,		/*  output busy */
	 Sysflag=	0x04,		/*  system flag */
	 Cmddata=	0x08,		/*  cmd==0, data==1 */
	 Inhibit=	0x10,		/*  keyboard/mouse inhibited */
	 Minready=	0x20,		/*  mouse character ready */
	 Rtimeout=	0x40,		/*  general timeout */
	 Parity=	0x80,

	Cmd=		0x64,		/* command port (write only) */

	/* controller command byte */
	Cscs1=		1<<6,		/* scan code set 1 */
	Cauxdis=	1<<5,		/* mouse disable */
	Ckbddis=	1<<4,		/* kbd disable */
	Csf=		1<<2,		/* system flag */
	Cauxint=	1<<1,		/* mouse interrupt enable */
	Ckbdint=	1<<0,		/* kbd interrupt enable */
};

extern int mouseshifted;

static void (*auxputc)(int);
static Lock i8042lock;
static uchar ccc;
static Kbscan *scan;

/*
 *  wait for output no longer busy
 */
static int
outready(void)
{
	int tries;

	for(tries = 0; (inb(Status) & Outbusy); tries++){
		if(tries > 500)
			return -1;
		delay(2);
	}
	return 0;
}

/*
 *  wait for input
 */
static int
inready(void)
{
	int tries;

	for(tries = 0; !(inb(Status) & Inready); tries++){
		if(tries > 500)
			return -1;
		delay(2);
	}
	return 0;
}

/*
 *  ask 8042 to reset the machine
 */
void
i8042reset(void)
{
	ushort *s;
	int i, x;

	if(sys->noi8042kbd)
		return;

	s = KADDR(0x472);
	*s = 0x1234;		/* BIOS warm-boot flag */

	/*
	 *  newer reset the machine command
	 */
	outready();
	outb(Cmd, 0xFE);
	outready();

	/*
	 *  Pulse it by hand (old somewhat reliable)
	 */
	x = 0xDF;
	for(i = 0; i < 5; i++){
		x ^= 1;
		outready();
		outb(Cmd, 0xD1);
		outready();
		outb(Data, x);	/* toggle reset */
		delay(100);
	}
}

/*
 *  keyboard interrupt
 */
static void
i8042intr(Ureg*, void*)
{
	int s, c;

	/*
	 *  get status
	 */
	ilock(&i8042lock);
	s = inb(Status);
	if(!(s&Inready)){
		iunlock(&i8042lock);
		return;
	}

	/*
	 *  get the character
	 */
	c = inb(Data);
	iunlock(&i8042lock);

	/*
	 *  if it's the aux port...
	 */
	if(s & Minready){
		if(auxputc != nil)
			auxputc(c);
		return;
	}

	kbdputsc(c, scan);
}

void
i8042auxenable(void (*putc)(int))
{
	char *err = "i8042: aux init failed\n";

	/* enable kbd/aux xfers and interrupts */
	ccc &= ~Cauxdis;
	ccc |= Cauxint;

	ilock(&i8042lock);
	if(outready() < 0)
		iprint(err);
	outb(Cmd, 0x60);			/* write control register */
	if(outready() < 0)
		iprint(err);
	outb(Data, ccc);
	if(outready() < 0)
		iprint(err);
	outb(Cmd, 0xA8);			/* auxiliary device enable */
	if(outready() < 0){
		iunlock(&i8042lock);
		return;
	}
	auxputc = putc;
	intrenable(IrqAUX, i8042intr, 0, BUSUNKNOWN, "kbdaux");
	iunlock(&i8042lock);
}

static char *initfailed = "i8042: kbdinit failed\n";

static int
outbyte(int port, int c)
{
	outb(port, c);
	if(outready() < 0) {
		print(initfailed);
		return -1;
	}
	return 0;
}

int
i8042auxcmd(int cmd)
{
	uint c;
	int tries;

	c = 0;
	tries = 0;

	ilock(&i8042lock);
	do{
		if(tries++ > 2)
			break;
		if(outready() < 0)
			break;
		outb(Cmd, 0xD4);
		if(outready() < 0)
			break;
		outb(Data, cmd);
		if(outready() < 0)
			break;
		if(inready() < 0)
			break;
		c = inb(Data);
	} while(c == 0xFE || c == 0);
	iunlock(&i8042lock);

	if(c != 0xFA){
		print("i8042: %2.2ux returned to the %2.2ux command\n", c, cmd);
		return -1;
	}
	return 0;
}

/*
 * set keyboard's leds for lock states (scroll, numeric, caps).
 * pc keyboards set numeric-lock behaviour to match the led state
 */
static void
setleds(int leds)
{
	if(sys->noi8042kbd)
		return;
	ilock(&i8042lock);
	outready();
	outb(Data, 0xed);		/* talk directly to kbd, not ctlr */
	microdelay(1);
	outready();
	if(inready() == 0)
		inb(Data);
	outb(Data, leds);
	if(inready() == 0)
		inb(Data);

	outready();
	iunlock(&i8042lock);
}

static int
ledmsg(char *msg)
{
	int leds, c;

	leds = 0;
	while(c = *msg++){
		switch(c){
		case 's':
			leds |= 1<<0;
			break;
		case 'n':
			leds |= 1<<1;
			break;
		case 'c':
			leds |= 1<<2;
			break;
		}
	}
	return leds;
}

static void
i8042msg(char *msg)
{
	switch(*msg){
	case 'L':
		setleds(ledmsg(msg+1));
		break;
	}
}

void
failkbd(void)
{
	sys->noi8042kbd = 1;
	iofree(Data);
	iofree(Cmd);
	print(initfailed);
}

void
kbdinit(void)
{
	int c, try;

	if(sys->noi8042kbd)
		return;

	ioalloc(Data, 1, 0, "kbd data");
	ioalloc(Cmd, 1, 0, "kbd cmd");

	/* wait for a quiescent controller */
	try = 1000;
	while(try-- > 0 && (c = inb(Status)) & (Outbusy | Inready)) {
		if(c & Inready)
			inb(Data);
		delay(1);
	}
	if (try <= 0) {
		failkbd();
		return;
	}

	/* get current controller command byte */
	outb(Cmd, 0x20);
	if(inready() < 0){
		print("i8042: kbdinit can't read ccc\n");
		ccc = 0;
	} else
		ccc = inb(Data);

	/* enable kbd xfers and interrupts */
	ccc &= ~Ckbddis;
	ccc |= Csf | Ckbdint | Cscs1;
	if(outready() < 0) {
		failkbd();
		return;
	}

	/* disable mouse */
	if (outbyte(Cmd, 0x60) < 0 || outbyte(Data, ccc) < 0)
		print("i8042: kbdinit mouse disable failed\n");

	/* set typematic rate/delay (0 -> delay=250ms & rate=30cps) */
	if(outbyte(Cmd, 0xf3) < 0 || outbyte(Data, 0) < 0)
		print("i8042: kbdinit set typematic rate failed\n");
}

void
i8042kbdenable(void)
{
	if(!sys->noi8042kbd){
		intrenable(IrqKBD, i8042intr, 0, BUSUNKNOWN, "i8042");
		scan = kbdnewscan(i8042msg);
	}
}
