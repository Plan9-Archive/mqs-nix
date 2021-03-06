#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

/*
 *  mouse types
 */
enum
{
	Mouseother=	0,
	Mouseserial=	1,
	MousePS2=	2,
};

extern int mouseshifted;

static QLock mousectlqlock;
static int mousetype;
static int intellimouse;
static int packetsize;
static int resolution;
static int accelerated;
static int mousehwaccel;
static char mouseport[5];
static int dztab[2][7] = {
	0, 1, 2, 3, 4, 5, 6,
	0, 1, 1, 3, 6, 9, 12,
};
static char Emouseset[] = "mouse type already set";

enum
{
	CMaccelerated,
	CMhwaccel,
	CMintellimouse,
	CMlinear,
	CMps2,
	CMps2intellimouse,
	CMres,
	CMreset,
	CMserial,
};

static Cmdtab mousectlmsg[] =
{
	CMaccelerated,		"accelerated",		0,
	CMhwaccel,		"hwaccel",		2,
	CMintellimouse,		"intellimouse",		1,
	CMlinear,		"linear",		1,
	CMps2,			"ps2",			1,
	CMps2intellimouse,	"ps2intellimouse",	1,
	CMres,			"res",			0,
	CMreset,		"reset",		1,
	CMserial,		"serial",		0,
};

/*
 *  ps/2 mouse message is three bytes
 *
 *	byte 0 -	0 0 SDY SDX 1 M R L
 *	byte 1 -	DX
 *	byte 2 -	DY
 *
 *  shift & right button is the same as middle button
 *
 * Intellimouse and AccuPoint with extra buttons deliver
 *	byte 3 -	00 or 01 or FF according to extra button state.
 * extra buttons are mapped in this code to buttons 4 and 5.
 * AccuPoint generates repeated events for these buttons;
*  it and Intellimouse generate 'down' events only, so
 * user-level code is required to generate button 'up' events
 * if they are needed by the application.
 * Also on laptops with AccuPoint AND external mouse, the
 * controller may deliver 3 or 4 bytes according to the type
 * of the external mouse; code must adapt.
 *
 * On the NEC Versa series (and perhaps others?) we seem to
 * lose a byte from the packet every once in a while, which
 * means we lose where we are in the instruction stream.
 * To resynchronize, if we get a byte more than two seconds
 * after the previous byte, we assume it's the first in a packet.
 */

/* 2:1 scaling can fail with some hardware.  do it yourself. */
static int
scale(int x)
{
	if(x >= 0 && x < 7)
		return dztab[accelerated][x];
	if(accelerated)
		return x*2;
	return x;
}

static void resetmouse(void);
static void
ps2mouseputc(int c)
{
	static short msg[4];
	static int nb, aa;
	static uchar b[] = {0, 1, 4, 5, 2, 3, 6, 7, 0, 1, 2, 3, 2, 3, 6, 7 };
	static ulong lasttick;
	ulong m;
	int shift, buttons, dx, dy;

	if(c == 0xaa){
		aa = 1;
		return;
	}
	if(aa && c == 0){
		resetmouse();
		aa = 0;
		return;
	}
	aa = 0;

	/*
	 * non-ps2 keyboards might not set shift
	 * but still set mouseshifted.
	 */
	shift = mouseshifted;
	/*
	 * Resynchronize in stream with timing; see comment above.
	 */
	m = sys->ticks;
	if(TK2SEC(m - lasttick) > 2)
		nb = 0;
	lasttick = m;

	/* 
	 *  check byte 0 for consistency
	 */
	if(nb==0 && (c&0xc8)!=0x08)
		if(intellimouse && (c==0x00 || c==0x01 || c==0xFF)){
			/* last byte of 4-byte packet */
			packetsize = 4;
			return;
		}

	msg[nb] = c;
	if(++nb == packetsize){
		nb = 0;
		if(msg[0] & 0x10)
			msg[1] |= 0xFF00;
		if(msg[0] & 0x20)
			msg[2] |= 0xFF00;

		buttons = b[(msg[0]&7) | (shift ? 8 : 0)];
		if(intellimouse && packetsize==4){
			if((msg[3]&0xc8) == 0x08){
				/* first byte of 3-byte packet */
				packetsize = 3;
				msg[0] = msg[3];
				nb = 1;
				/* fall through to emit previous packet */
			}else{
				/* The AccuPoint on the Toshiba 34[48]0CT
				 * encodes extra buttons as 4 and 5. They repeat
				 * and don't release, however, so user-level
				 * timing code is required. Furthermore,
				 * intellimice with 3buttons + scroll give a
				 * two's complement number in the lower 4 bits
				 * (bit 4 is sign extension) that describes
				 * the amount the scroll wheel has moved during
				 * the last sample. Here we use only the sign to
				 * decide whether the wheel is moving up or down
				 * and generate a single button 4 or 5 click
				 * accordingly.
				 */
				if((msg[3] >> 3) & 1) 
					buttons |= 1<<3;
				else if(msg[3] & 0x7) 
					buttons |= 1<<4;
			}
		}
		dx = scale(msg[1]);
		dy = -scale(msg[2]);
		mousetrack(dx, dy, buttons, TK2MS(sys->ticks));
	}
}

/*
 *  set up a ps2 mouse
 */
static void
ps2mouse(void)
{
	if(mousetype == MousePS2)
		return;

	i8042auxenable(ps2mouseputc);
	/* make mouse streaming, enabled */
//	i8042auxcmd(0xEA);
//	i8042auxcmd(0xF4);

	mousetype = MousePS2;
	packetsize = 3;
	mousehwaccel = 1;
	resetmouse();
}

/*
 * The PS/2 Trackpoint multiplexor on the IBM Thinkpad T23 ignores
 * acceleration commands.  It is supposed to pass them on
 * to the attached device, but my Logitech mouse is simply
 * not behaving any differently.  For such devices, we allow
 * the user to use "hwaccel off" to tell us to back off to
 * software acceleration even if we're using the PS/2 port.
 * (Serial mice are always software accelerated.)
 * For more information on the Thinkpad multiplexor, see
 * http://wwwcssrv.almaden.ibm.com/trackpoint/
 */
static void
setaccelerated(int x)
{
	accelerated = x;
	if(mousehwaccel){
		switch(mousetype){
		case MousePS2:
			/* scale in software; hw scaling isn't always supported. */
//			i8042auxcmd(0xF5);	/* disable */
//			i8042auxcmd(0xE7);
//			i8042auxcmd(0xF4);	/* enabled */
			return;
		}
	}
	mouseaccelerate(x);
}

static void
setlinear(void)
{
	accelerated = 0;
	if(mousehwaccel){
		switch(mousetype){
		case MousePS2:
			i8042auxcmd(0xF5);	/* disable */
			i8042auxcmd(0xE6);
			i8042auxcmd(0xF4);	/* enabled */
			return;
		}
	}
	mouseaccelerate(0);
}

static void
setres(int n)
{
	resolution = n;
	switch(mousetype){
	case MousePS2:
		i8042auxcmd(0xF5);	/* disable */
		i8042auxcmd(0xE8);
		i8042auxcmd(n);
		i8042auxcmd(0xF4);	/* enabled */
		break;
	}
}

static void
setintellimouse(void)
{
	intellimouse = 1;
	packetsize = 4;
	switch(mousetype){
	case MousePS2:
		i8042auxcmd(0xF5);	/* disable */
		i8042auxcmd(0xF3);	/* set sample */
		i8042auxcmd(0xC8);
		i8042auxcmd(0xF3);	/* set sample */
		i8042auxcmd(0x64);
		i8042auxcmd(0xF3);	/* set sample */
		i8042auxcmd(0x50);
		i8042auxcmd(0xF4);	/* enabled */
		break;
	case Mouseserial:
		uartsetmouseputc(atoi(mouseport), m5mouseputc);
		break;
	}
}

static void
resetmouse(void)
{
	packetsize = 3;
	switch(mousetype){
	case MousePS2:
		i8042auxcmd(0xF5);	/* disable */
		i8042auxcmd(0xF6);
		i8042auxcmd(0xEA);	/* streaming */
		i8042auxcmd(0xE8);	/* set resolution */
		i8042auxcmd(3);
		i8042auxcmd(0xE6);	/* 1:1 scaling */
		i8042auxcmd(0xF4);	/* enabled */
		break;
	}
	if(accelerated)
		setaccelerated(accelerated);
	if(resolution)
		setres(resolution);
	if(intellimouse)
		setintellimouse();
}

void
mousectl(Cmdbuf *cb)
{
	char *p;
	int i;
	Cmdtab *ct;

	qlock(&mousectlqlock);
	if(waserror()){
		qunlock(&mousectlqlock);
		nexterror();
	}

	ct = lookupcmd(cb, mousectlmsg, nelem(mousectlmsg));
	switch(ct->index){
	case CMaccelerated:
		setaccelerated(cb->nf == 1 ? 1 : atoi(cb->f[1]));
		break;
	case CMintellimouse:
		setintellimouse();
		break;
	case CMlinear:
		setlinear();
		break;
	case CMps2:
		intellimouse = 0;
		ps2mouse();
		break;
	case CMps2intellimouse:
		ps2mouse();
		setintellimouse();
		break;
	case CMres:
		if(cb->nf >= 2)
			setres(atoi(cb->f[1]));
		else
			setres(1);
		break;
	case CMreset:
		resetmouse();
		if(accelerated)
			setaccelerated(accelerated);
		if(resolution)
			setres(resolution);
		if(intellimouse)
			setintellimouse();
		break;
	case CMserial:
		if(mousetype == Mouseserial)
			error(Emouseset);

		i = strtol(cb->f[1], &p, 0);
		if(i < 0 || p == cb->f[1])
			error(Ebadarg);

		if(cb->nf > 2){
			if(strcmp(cb->f[2], "M") == 0)
				uartmouse(i, m3mouseputc, nil);
			else if(strcmp(cb->f[2], "MI") == 0)
				uartmouse(i, m5mouseputc, nil);
			else
				uartmouse(i, mouseputc, cb->nf == 1? "b1200": nil);
		} else
			uartmouse(i, mouseputc, cb->nf == 1? "b1200": nil);

		mousetype = Mouseserial;
		strncpy(mouseport, cb->f[1], sizeof(mouseport)-1);
		packetsize = 3;
		break;
	case CMhwaccel:
		if(strcmp(cb->f[1], "on")==0)
			mousehwaccel = 1;
		else if(strcmp(cb->f[1], "off")==0)
			mousehwaccel = 0;
		else
			cmderror(cb, "bad mouse control message");
	}

	qunlock(&mousectlqlock);
	poperror();
}
