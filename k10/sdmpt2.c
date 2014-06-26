/*
 * LSI Fusion-MPT SAS 2.0 SCSI Host Adapter.
 * not 64-bit clean
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

#include "../port/sd.h"
#include <fis.h>
#include "../port/sdfis.h"

#define B2W(x)		((x) / BIT32SZ)
#define W2B(x)		((x) * BIT32SZ)

extern SDifc sdmpt2ifc;

static char Ebadunit[] = "invalid unit";
static char Enoreqs[] = "no free requests";
static char Etimeout[] = "request timeout";

/* System Interface Register Set */
enum {
	Doorbell			= 0x00,
	WriteSequence			= 0x04,
	HostDiagnostic			= 0x08,
	DiagRwData			= 0x10,
	DiagRwAddressLow		= 0x14,
	DiagRwAddressHigh		= 0x18,
	HostInterruptStatus		= 0x30,
	HostInterruptMask		= 0x34,
	DcrData				= 0x38,
	DcrAddress			= 0x3c,
	ReplyFreeHostIndex		= 0x48,
	ReplyPostHostIndex		= 0x6c,
	HcbSize				= 0x74,
	HcbAddressLow			= 0x78,
	HcbAddressHigh			= 0x7c,
	RequestDescriptorPostLow	= 0xc0,
	RequestDescriptorPostHigh	= 0xc4,
};

/* Doorbell */
enum {
	WhoInitNone			= 0x0<<24,
	WhoInitSystemBios		= 0x1<<24,
	WhoInitRomBios			= 0x2<<24,
	WhoInitPciPeer			= 0x3<<24,
	WhoInitHostDriver		= 0x4<<24,
	WhoInitManufacturing		= 0x5<<24,
	IocMessageUnitReset		= 0x40<<24,
	Handshake			= 0x42<<24,
	DoorbellUsed			= 1<<27,
	StateReset			= 0x0<<28,
	StateReady			= 0x1<<28,
	StateOperational		= 0x2<<28,
	StateFault			= 0x4<<28,
	StateMask			= 0xf<<28,
};

/* HostDiagnostic */
enum {
	HoldIocReset			= 1<<1,
	ResetAdapter			= 1<<2,
	DiagRwEnable			= 1<<4,
	ResetHistory			= 1<<5,
	FlashBadSignature		= 1<<6,
	DiagWriteEnable			= 1<<7,
	HcbMode				= 1<<8,
	ForceHcbOnReset			= 1<<9,
	ClearFlashBadSignature		= 1<<10,
	BootDeviceFlash			= 0<<11,
	BootDeviceHcdw			= 1<<11,
};

/* HostInterruptStatus */
enum {
	Ioc2SysDbStatus			= 1<<0,
	ReplyDescriptorInterrupt	= 1<<3,
	Sys2IocDbStatus			= 1<<31,
};

/* HostInterruptMask */
enum {
	Ioc2SysDbMask			= 1<<0,
	ReplyIntMask			= 1<<3,
	ResetIrqMask			= 1<<30,
};

/* Reply Descriptors */
enum {
	ScsiIoSuccess			= 0x0,
	AddressReply			= 0x1,
};

/* Messages */
enum {
	DefaultReplyLength		= 5,
	IocInit				= 0x02,
	IocFacts			= 0x03,
	PortEnable			= 0x06,
	EventNotification		= 0x07,
	EventAck			= 0x08,
	ScsiIoRequest			= 0x00,
	ScsiTaskManagement		= 0x01,
	ScsiEnclosureProcessor		= 0x18,
	SasIoUnitControl		= 0x1b,
	SataPassthrough			= 0x1c,
	Config				= 0x04,
};

/* Events */
enum {
	LogData				= 0x0001,
	StateChange			= 0x0002,
	EventChange			= 0x000a,
	LogEntryAdded			= 0x0021,
	GpioInterrupt			= 0x0023,
	TempThreshold			= 0x0027,
	HostMessage			= 0x0028,
	PowerPerformanceChange		= 0x0029,
	HardResetReceived		= 0x0005,
	SasDeviceStatusChange		= 0x000f,
	IrOperationStatus		= 0x0014,
	IrVolume			= 0x001e,
	IrPhysicalDisk			= 0x001f,
	IrConfigurationChangeList	= 0x0020,
	SasDiscovery			= 0x0016,
	SasBroadcastPrimitive		= 0x0017,
	SasNotifyPrimitive		= 0x0026,
	SasInitDeviceStatusChange	= 0x0018,
	SasInitTableOverflow		= 0x0019,
	SasTopologyChangeList		= 0x001c,
	SasEnclDeviceStatusChange	= 0x001d,
	SasPhyCounter			= 0x0022,
	SasQuiesce			= 0x0025,
	HbdPhyEvent			= 0x0024,
};

enum {
	Nenclosure	= 1,
	Nunitperenc	= 20,
	Nunit		= Nenclosure * Nunitperenc,	/* should limit to 16 but i have more */

	Offline		= 0,
	Online,
};

typedef struct Unit Unit;
struct Unit {
	Sfisx;

	int		state;
	u16int		devh;
	uchar		link;
	u32int		info;
	u16int		flags;
	u64int		wwn;

	int		slot;
	uint		exph;
	uint		ench;
};

typedef struct Req Req;
struct Req {
	Rendez;
	u16int		smid;
	u32int		*req;
	SDreq		*sdreq;
	int		done;
	Req		*next;
};

typedef struct Ctlr Ctlr;
struct Ctlr {
	Lock;
	int		ioc;
	char		name[8];
	int		port;
	Pcidev		*pcidev;
	SDev		*sdev;
	int		enabled;
	Rendez		reset;
	RWlock		resetlock;
	Lock		doorlock;
	RWlock		tasklock;

	Unit		unit[Nunit];

	uchar		numports;
	ushort		maxtargs;

	u32int		*req;
	Req		*reqfree;
	Lock		reqfreelock;
	Lock		reqpostlock;
	u16int		reqcredit;
	u16int		iocreqfsz;
	u16int		reqfsz;

	u32int		*reply;
	u32int		*replyfree;
	u32int		*replypost;
	uchar		replyfsz;
	u16int		replyq;
	u16int		replyfreeq;
	u16int		replypostq;
	u16int		replypostmax;
	u32int		replyfreep;
	u32int		replypostp;

	char		allphysenc;
	char		direct;

	Queue		*eventq;
	void		*vector;
};

static u32int
iocread(Ctlr *ctlr, int reg)
{
	return inl(ctlr->port + reg);
}

static void
iocwrite(Ctlr *ctlr, int reg, u32int val)
{
	outl(ctlr->port + reg, val);
}

static u32int
iocwait(Ctlr *ctlr, int reg, u32int mask, int on)
{
	ulong t;
	u32int val;

	t = sys->ticks;
	for(;;){
		val = iocread(ctlr, reg);
		if(on)
		if(val & mask)
			return val;
		if(!on)
		if(!(val & mask))
			return val;
		if(TK2MS(sys->ticks - t) > 10*1000)	/* PG §3.7.1 */
			panic("iocwait: %s: wedge reg %#.2ux val %#.8ux",
			    ctlr->name, reg, val);
		microdelay(500);
	}
}

static void
ckstate(Ctlr *ctlr, int state)
{
	u32int val;

	val = iocread(ctlr, Doorbell);
	if(val & StateMask != state)
		panic("ckstate: %s: bad state %#.8ux", ctlr->name, val);
}

static int
ckstatus(Ctlr *ctlr, u32int *reply)
{
	ushort status;

	/*
	 * IOC Status is provided in every reply, which may
	 * be used to determine the success of a given function
	 * independent of message content.  In the event an
	 * unexpected status is returned, a panic is issued.
	 */
	status = reply[3]>>16 & 0x7fff;		/* IOCStatus */
	if(status == 0x0000)	/* SUCCESS */
		return 1;

	/*
	 * Some functions support nominal failure modes; rather
	 * than panic, we allow the caller to determine the
	 * severity of the condition.
	 */
	switch(reply[0]>>24){			/* Function */
	case ScsiIoRequest:
	case SataPassthrough:
		switch(status){
		case 0x0003:	/* INVALID_SGL (user command with r/w bits mixed up) */
		case 0x0045:	/* SCSI_DATA_UNDERRUN */
		case 0x0046:	/* SCSI_IO_DATA_ERROR */
		case 0x0047:	/* SCSI_PROTOCOL_ERROR */
			return 0;
		}
		break;
	case ScsiTaskManagement:
		switch(status){
		case 0x0040:	/* SCSI_RECOVERED_ERROR */
		case 0x0042:	/* SCSI_INVALID_DEVHANDLE */
		case 0x0043:	/* SCSI_DEVICE_NOT_THERE */
		case 0x0044:	/* SCSI_DATA_OVERRUN */
		case 0x0045:	/* SCSI_DATA_UNDERRUN */
		case 0x0046:	/* SCSI_IO_DATA_ERROR */
		case 0x0047:	/* SCSI_PROTOCOL_ERROR */
		case 0x0048:	/* SCSI_TASK_TERMINATED */
		case 0x0049:	/* SCSI_RESIDUAL_MISMATCH */
		case 0x004a:	/* SCSI_TASK_MGMT_FAILED */
		case 0x004b:	/* SCSI_IOC_TERMINATED */
		case 0x004c:	/* SCSI_EXT_TERMINATED */
			return 0;
		}
		break;
	case Config:
		switch(status){
		case 0x0020:	/* CONFIG_INVALID_ACTION */
		case 0x0021:	/* CONFIG_INVALID_TYPE */
		case 0x0022:	/* CONFIG_INVALID_PAGE */
		case 0x0023:	/* CONFIG_INVALID_DATA */
		case 0x0024:	/* CONFIG_NO_DEFAULTS */
		case 0x0025:	/* CONFIG_CANT_COMMIT */
			return 0;
		}
		break;
	}
	panic("ckstatus: %s: fn %.2ux, bad status %#.04ux",
		ctlr->name, reply[0]>>24, status);
	return -1;	/* not reached */
}

static int
doorbell(Ctlr *ctlr, u32int *req, int nwords)
{
	int i;
	u16int *reply;

	ilock(&ctlr->doorlock);
	iocwait(ctlr, Doorbell, DoorbellUsed, 0);
	iocwrite(ctlr, HostInterruptStatus, 0);

	iocwrite(ctlr, Doorbell, Handshake | nwords<<16);

	iocwait(ctlr, HostInterruptStatus, Ioc2SysDbStatus, 1);
	iocwrite(ctlr, HostInterruptStatus, 0);

	iocwait(ctlr, HostInterruptStatus, Sys2IocDbStatus, 0);

	for(i = 0; i < nwords; ++i){
		iocwrite(ctlr, Doorbell, req[i]);
		iocwait(ctlr, HostInterruptStatus, Sys2IocDbStatus, 0);
	}

	/*
	 * We do something sneaky here; replies are written back
	 * into the request buffer during handshake.  Buffers must
	 * be sized to accomodate the larger of the two messages.
	 *
	 * Doorbell reads yield 16 bits at a time; upper bits are
	 * considered reserved.  The reply MsgLength is located
	 * in the first 32-bit word; a reply will always contain
	 * at least DefaultReplyLength words.
	 */
	reply = (u16int *)req;
	nwords = DefaultReplyLength;
	for(i = 0; i < nwords * BIT16SZ; ++i){
		iocwait(ctlr, HostInterruptStatus, Ioc2SysDbStatus, 1);
		reply[i] = iocread(ctlr, Doorbell);
		iocwrite(ctlr, HostInterruptStatus, 0);
		if(i == 1)
			nwords = reply[i] & 0xff;	/* MsgLength */
	}

	iocwait(ctlr, HostInterruptStatus, Ioc2SysDbStatus, 1);
	iocwrite(ctlr, HostInterruptStatus, 0);

	iocwait(ctlr, Doorbell, DoorbellUsed, 0);
	iunlock(&ctlr->doorlock);
	return nwords;
}

#define UNIT(ctlr, n)	((ctlr)->unit + (n))

static int
getunitcaps(Unit *u, char *p, int l)
{
	char *o, *e;
	int i;
	static char *caps[] = {
		[4]	"fua",		/* SATA FUA */
		[5]	"ncq",		/* SATA NCQ */
		[6]	"smart",	/* SATA SMART */
		[7]	"lba48",	/* SATA 48-bit LBA */
		[9]	"ssp",		/* SATA Software Settings Preservation */
		[10]	"async",	/* SATA Asynchronous Notification */
		[11]	"partial",	/* Partial Power Management Mode */
		[12]	"slumber",	/* Slumber Power Management Mode */
		[13]	"fp",		/* Fast Path */
	};

	o = p;
	e = p + l;
	for(i = 0; i < nelem(caps); ++i)
		if(caps[i])
		if(u->flags & 1<<i){
			if(p != o)
				p = seprint(p, e, " ");
			p = seprint(p, e, "%s", caps[i]);
		}
	if(p == o)
		p = seprint(p, e, "none");
	return p - o;
}

static char *
unittype(Unit *u)
{
	if(u->info & 1<<7)			/* Device Type */
		return "sata";
	return "sas";
}

static char *
unitlink(Unit *u)
{
	switch(u->link>>4){			/* Current Link Rate */
	case 0x8:
		return "1.5Gb/s";
	case 0x9:
		return "3.0Gb/s";
	case 0xa:
		return "6.0Gb/s";
	case 0xb:
		return "12.0Gb/s";
	default:
		return "unknown";
	}
}

static char *
unitstate(Unit *u)
{
	switch(u->state){
	case Online:
		return "online";
	default:
		return "offline";
	}
}

#define REQ(ctlr, n)	((Req *)((ctlr)->req + (n)*(ctlr)->reqfsz + \
			    (ctlr)->iocreqfsz))

static u32int *
reallocreq(Ctlr *ctlr)
{
	ushort n;

	free(ctlr->req);

	/*
	 * System Request Message Frames must be allocated
	 * contiguously, aligned on a 16-byte boundary, and be a
	 * multiple of 16 bytes in length.
	 */
	n = W2B(ctlr->iocreqfsz) + ROUNDUP(sizeof(Req), 16);
	ctlr->reqfsz = B2W(n);
	ctlr->req = mallocalign(ctlr->reqcredit * n, 16, 0, 0);
	if(ctlr->req == nil)
		print("reallocreq: %s: out of memory\n", ctlr->name);
	return ctlr->req;
}

static Req *
nextreq(Ctlr *ctlr)
{
	Req *r;

	lock(&ctlr->reqfreelock);
	if(r = ctlr->reqfree)
		ctlr->reqfree = r->next;
	unlock(&ctlr->reqfreelock);
	return r;
}

static void
freereq(Ctlr *ctlr, Req *r)
{
	lock(&ctlr->reqfreelock);
	r->next = ctlr->reqfree;
	ctlr->reqfree = r;
	unlock(&ctlr->reqfreelock);
}

static int
reqdone(void *arg)
{
	Req *r;

	r = arg;
	return r->done;
}

static void
postreq(Ctlr *ctlr, Req *r, u32int *desc, int ms)
{
	r->done = 0;

	ilock(&ctlr->reqpostlock);
	iocwrite(ctlr, RequestDescriptorPostLow, desc[0]);
	iocwrite(ctlr, RequestDescriptorPostHigh, desc[1]);
	iunlock(&ctlr->reqpostlock);

	while(waserror())
		;
	tsleep(r, reqdone, r, ms);
	poperror();
	if(!r->done)
		error(Etimeout);
}

static void
postio(Ctlr *ctlr, Req *r, u32int *desc, int ms)
{
	rlock(&ctlr->tasklock);
	if(waserror()){
		runlock(&ctlr->tasklock);
		nexterror();
	}
	postreq(ctlr, r, desc, ms);
	poperror();
	runlock(&ctlr->tasklock);
}

static void
posttask(Ctlr *ctlr, Req *r, u32int *desc, int ms)
{
	wlock(&ctlr->tasklock);
	if(waserror()){
		wunlock(&ctlr->tasklock);
		nexterror();
	}
	postreq(ctlr, r, desc, ms);
	poperror();
	wunlock(&ctlr->tasklock);
}

/* need to fix assumptions of caller */
static void
mksge64(u32int *sgl, void *data, int len, int write)
{
	int flags;

	flags = 1<<7;				/* LastElement */
	flags |= 1<<6;				/* EndOfBuffer */
	flags |= 1<<4;				/* ElementType (Simple Element) */
	if(write)
		flags |= 1<<2;			/* Direction (Write) */
	flags |= 1<<1;				/* AddressSize (64-bit) */
	flags |= 1<<0;				/* EndOfList */

	sgl[0] = flags<<24;			/* Flags */
	sgl[0] |= len & 0xffffff;		/* Length */
	if(data){
		sgl[1] = Pciwaddrl(data);	/* Address */
		sgl[2] = Pciwaddrh(data);	/* Address */
	}
	else{
		sgl[1] = ~0;
		sgl[2] = ~0;
	}
}

static void
mksge32(u32int *sgl, void *data, int len, int write)
{
	int flags;

	flags = 1<<7;				/* LastElement */
	flags |= 1<<6;				/* EndOfBuffer */
	flags |= 1<<4;				/* ElementType (Simple Element) */
	if(write)
		flags |= 1<<2;			/* Direction (Write) */
	flags |= 0<<1;				/* AddressSize (32-bit) */
	flags |= 1<<0;				/* EndOfList */

	sgl[0] = flags<<24;			/* Flags */
	sgl[0] |= len & 0xffffff;		/* Length */
	if(data)
		sgl[1] = PCIWADDR(data);	/* Address */
	else
		sgl[1] = 0;
}

static void
iocfacts(Ctlr *ctlr)
{
	u32int buf[16];

	memset(buf, 0, W2B(3));
	buf[0] = IocFacts<<24;			/* Function */

	doorbell(ctlr, buf, 3);
	ckstatus(ctlr, buf);

	ctlr->numports = buf[5]>>16 & 0xff;	/* NumberOfPorts */
	ctlr->reqcredit = buf[6] & 0xffff;	/* RequestCredit */
	ctlr->iocreqfsz = buf[9] & 0xffff;	/* IOCRequestFrameSize */
	ctlr->maxtargs = buf[10]>>16;		/* MaxTargets */
	ctlr->replyfsz = buf[12]>>16 & 0xff;	/* ReplyFrameSize */
	ctlr->replypostmax = buf[13] & 0xffff;	/* MaxReplyDescriptorPostQueueDepth */
}

static void
iocinit(Ctlr *ctlr)
{
	u32int buf[18];

	memset(buf, 0, W2B(18));
	buf[0] = IocInit<<24;			/* Function */
	buf[6] = ctlr->reqfsz<<16;		/* SystemRequestFrameSize */
	buf[7] = ctlr->replyfreeq<<16;		/* ReplyFreeQueueDepth */
	buf[7] |= ctlr->replypostq;		/* ReplyDescriptorPostQueueDepth */
	buf[10] = Pciwaddrl(ctlr->req);		/* SystemRequestFrameBaseAddress */
	buf[11] = Pciwaddrh(ctlr->req);
	buf[12] = Pciwaddrl(ctlr->replypost);	/* ReplyDescriptorPostQueueAddress */
	buf[13] = Pciwaddrh(ctlr->replypost);
	buf[14] = Pciwaddrl(ctlr->replyfree);	/* ReplyFreeQueueAddress */
	buf[15] = Pciwaddrh(ctlr->replyfree);

	doorbell(ctlr, buf, 18);
	ckstatus(ctlr, buf);
	ckstate(ctlr, StateOperational);
}

static void
event(u32int *buf, u32int event)
{
	buf[event/32] &= ~(1 << event%32);
}

static void
eventnotify(Ctlr *ctlr)
{
	u32int buf[11];

	memset(buf, 0, W2B(11));
	buf[0] = EventNotification<<24;		/* Function */

	/*
	 * Event notifications are masked using the bit identified
	 * by the value of the event; see MPI §8.4.  PG §3.7.4
	 * suggests a number of SAS events required for proper
	 * host mapping, however the SAS_TOPOLOGY_CHANGE_LIST
	 * event is merely sufficient.
	 */
	memset(buf+5, 0xff, 4*4);
	event(buf+5, SasTopologyChangeList);
	event(buf+5, SasEnclDeviceStatusChange);

	doorbell(ctlr, buf, 11);
	ckstatus(ctlr, buf);
}

static void
eventack(Ctlr *ctlr, u16int event, u32int context)
{
	Req *r;
	u32int desc[2];

	r = nextreq(ctlr);
	if(r == nil)
		error(Enoreqs);
	memset(r->req, 0, W2B(5));
	r->req[0] = EventAck<<24;		/* Function */
	r->req[3] = event;			/* Event */
	r->req[4] = context;			/* EventContext */

	desc[0] = r->smid<<16 | 0x4<<1;		/* Default Request */
	desc[1] = 0;
	postreq(ctlr, r, desc, 5*1000);		/* PG §3.7.4 */
	freereq(ctlr, r);
}

static void
portenable(Ctlr *ctlr)
{
	Req *r;
	u32int desc[2];

	/*
	 * The Port Enable message is posted using the Request
	 * Descriptor Post Queue for reliable delivery of events.
	 * Use of the System Doorbell will miss events when used
	 * on a uniprocessor.
	 */
	r = nextreq(ctlr);
	if(r == nil)
		error(Enoreqs);
	memset(r->req, 0, W2B(3));
	r->req[0] = PortEnable<<24;		/* Function */

	desc[0] = r->smid<<16 | 0x4<<1;		/* Default Request */
	desc[1] = 0;
	postreq(ctlr, r, desc, 300*1000);	/* PG §3.7.1 */
	freereq(ctlr, r);
}

enum {
	/* page types */
	IO_UNIT_PAGE		= 0x0,
	IOC_PAGE		= 0x1,
	BIOS_PAGE		= 0x2,
	RAID_VOL_PAGE	= 0x8,
	MFG_PAGE		= 0x9,
	RAID_DISK_PAGE	= 0xa,
	EXT_PAGE		= 0xf,

	/* extended page types */
	SAS_IO_UNIT		= 0x10,
	SAS_EXPANDER	= 0x11,
	SAS_DEVICE		= 0x12,
	SAS_PHY		= 0x13,
	LOG			= 0x14,
	ENCLOSURE		= 0x15,
	RAID_CONFIG		= 0x16,
	DRIVER_MAPPING	= 0x17,
	SAS_PORT		= 0x18,
	ETHERNET		= 0x19,
	EXT_MFG		= 0x1a,
};

static int
getpage(Ctlr *ctlr, uint devh, int ptype, int exptype, int pnum, u32int *page, int npage)
{
	u32int buf[7+2];

	memset(buf, 0, W2B(7));
	buf[0] = Config<<24;			/* Function */
	buf[0] |= 0x00;				/* Action (PAGE_HEADER) */
	buf[1] = exptype<<16;			/* ExtPageType */
	buf[5] = ptype<<24;			/* PageType (Extended) */
	buf[5] |= pnum<<16;			/* PageNumber */
	mksge32(buf+7, nil, 0, 0);			/* PageBufferSGE (empty) */

	doorbell(ctlr, buf, 7+2);
	ckstatus(ctlr, buf);

	buf[0] |= 0x01;				/* Action (READ_CURRENT) */
	buf[6] = 0x2<<28 | devh;			/* PageAddress (HANDLE) */
	mksge32(buf+7, page, npage, 0);		/* PageBufferSGE */

	doorbell(ctlr, buf, 7+2);
	if(!ckstatus(ctlr, buf))
		return -1;
	return 0;
}

static int
devpage(Ctlr *ctlr, uint devh, u32int page[14])
{
	return getpage(ctlr, devh, EXT_PAGE, SAS_DEVICE, 0, page, 4*14);
}

static int
·devpage(Ctlr *ctlr, uint devh, u32int page[14])
{
	u32int buf[7+2];

	/*
	 * Unit configuration is pulled from SAS Device Page 0.
	 * The DeviceInfo and Flags fields provide device
	 * interconnect and capabilities.  We obtain configuration
	 * via the System Doorbell to simplify access to the page
	 * header.
	 */
	getpage(ctlr, devh, EXT_PAGE, SAS_DEVICE, 0, buf, sizeof buf);

	memset(buf, 0, W2B(7));
	buf[0] = Config<<24;			/* Function */
	buf[0] |= 0x00;				/* Action (PAGE_HEADER) */
	buf[1] = 0x12<<16;			/* ExtPageType (SAS_DEVICE) */
	buf[5] = 0xf<<24;			/* PageType (Extended) */
	buf[5] |= 0<<16;			/* PageNumber */
	mksge32(buf+7, nil, 0, 0);		/* PageBufferSGE (empty) */

	doorbell(ctlr, buf, 7+2);
	ckstatus(ctlr, buf);

	buf[0] |= 0x01;				/* Action (READ_CURRENT) */
	buf[6] = 0x2<<28 | devh;		/* PageAddress (HANDLE) */
	mksge32(buf+7, page, 14*4, 0);		/* PageBufferSGE */

	doorbell(ctlr, buf, 7+2);
	if(!ckstatus(ctlr, buf))
		return -1;
	return 0;
}

static void
unitconfig(Ctlr *ctlr, Unit *u, uint devh)
{
	u32int page[14];

	if(devpage(ctlr, devh, page) == -1)
		error(Ebadunit);
	u->devh = devh;
	u->info = page[7];			/* DeviceInfo */
	u->flags = page[8] & 0xffff;		/* Flags */
	u->wwn = getle((uchar*)(page+9), 8);
	u->slot = page[2] & 0xffff;
//	print("parent devh %ux phyno %d physport %d\n", page[5]&0xffff, page[5]>>16 & 0xff, page[8]>>24&0xff);
}

static void
unitremove(Ctlr *ctlr, Unit *u)
{
	Req *r;
	u32int desc[2];

	r = nextreq(ctlr);
	if(r == nil)
		error(Enoreqs);
	memset(r->req, 0, W2B(11));
	r->req[0] = SasIoUnitControl<<24;	/* Function */
	r->req[0] |= 0x0d;			/* Operation (REMOVE_DEVICE) */
	r->req[1] = u->devh;			/* DevHandle */

	desc[0] = r->smid<<16 | 0x4<<1;		/* Default Request */
	desc[1] = 0;
	postreq(ctlr, r, desc, 5*1000);		/* PG §3.7.4 */
	freereq(ctlr, r);
}

static void
unitreset(Ctlr *ctlr, Unit *u)
{
	Req *r;
	u32int desc[2];

	r = nextreq(ctlr);
	if(r == nil)
		error(Enoreqs);
	memset(r->req, 0, W2B(13));
	r->req[0] = ScsiTaskManagement<<24;	/* Function */
	r->req[0] |= u->devh;			/* DevHandle */
	r->req[1] = 0x03<<8;			/* TaskType (Target Reset) */

	desc[0] = r->smid<<16 | 0x3<<1;		/* High Priority Request */
	desc[1] = 0;
	posttask(ctlr, r, desc, 30*1000);	/* PG §3.7.3 */
	freereq(ctlr, r);
}

static void
scsiabort(Ctlr *ctlr, Unit *u, Req *s)
{
	Req *r;
	u32int desc[2];

	r = nextreq(ctlr);
	if(r == nil)
		error(Enoreqs);
	memset(r->req, 0, W2B(13));
	r->req[0] = ScsiTaskManagement<<24;	/* Function */
	r->req[0] |= u->devh;			/* DevHandle */
	r->req[1] = 0x01<<8;			/* TaskType (Abort Task) */
	r->req[12] = s->smid;			/* TaskMID */

	desc[0] = r->smid<<16 | 0x3<<1;		/* High Priority Request */
	desc[1] = 0;
	posttask(ctlr, r, desc, 5*1000);	/* PG §3.7.3 */
	freereq(ctlr, r);
}

static void
scsiio(Ctlr *ctlr, Unit *u, SDreq *sdreq)
{
	Req *r;
	u32int desc[2];

	r = nextreq(ctlr);
	if(r == nil){
		sdreq->status = SDbusy;
		return;
	}
	r->sdreq = sdreq;
	memset(r->req, 0, W2B(24));
	r->req[0] = ScsiIoRequest<<24;		/* Function */
	r->req[0] |= u->devh;			/* DevHandle */
	r->req[3] = PCIWADDR(sdreq->sense);	/* SenseBufferLowAddress */
	r->req[4] = sizeof sdreq->sense<<16;	/* SenseBufferLength */
	r->req[5] = 24;				/* SGLOffset0 */
	r->req[7] = sdreq->dlen;		/* DataLength */
	r->req[9] = sdreq->clen;		/* CDBLength */
	if(sdreq->write)
		r->req[15] = 0x1<<24;		/* Data Direction (Write) */
	else
		r->req[15] = 0x2<<24;		/* Data Direction (Read) */

	memmove(r->req+16, sdreq->cmd, sdreq->clen);
	mksge32(r->req+24, sdreq->data, sdreq->dlen, sdreq->write);

	desc[0] = r->smid<<16 | 0x0<<1;		/* SCSI IO Request */
	desc[1] = u->devh<<16;
	if(waserror()){
		/*
		 * SCSI Task Management Requests are guaranteed to
		 * block until the IOC has replied to outstanding
		 * messages; see MPI §9.5.
		 */
		if(!r->done)
			scsiabort(ctlr, u, r);
		if(!r->done)
			unitreset(ctlr, u);
		if(!r->done)
			nexterror();
	}else{
		postio(ctlr, r, desc, 10*1000);
		poperror();
	}
	freereq(ctlr, r);
}

static int
fisreqchk(Sfis *f, SDreq *r)
{
	uchar *c;

	if((r->ataproto & Pprotom) == Ppkt)
		return SDnostatus;
	if(r->clen != 16)
		error("bad command length");
	c = r->cmd;
	if(c[0] == 0xf0){
		sigtofis(f, r->cmd);
		return r->status = SDok;
	}
//	c[0] = H2dev;
//	c[1] = Fiscmd;
//	c[7] |= Ataobs;
	return SDnostatus;
}

static void
ataio(Ctlr *ctlr, Unit *u, SDreq *sdreq)
{
	Req *r;
	int ms;
	u32int desc[2];

print("ataio %.2ux %.2ux %.2ux %.2ux clen=%d\n", sdreq->cmd[0], sdreq->cmd[1], sdreq->cmd[2], sdreq->cmd[3],
	sdreq->clen);
	if((sdreq->status = fisreqchk(u, sdreq)) != SDnostatus)
		return; /* sdreq->status;*/

	/*
	 * SATA passthrough is supported for PIO protocol ATA commands
	 * such as IDENTIFY DEVICE.  Only Register Host to Device
	 * and Register Device to Host command FISes are supported.
	 */
	r = nextreq(ctlr);
	if (r == nil) {
		sdreq->status = SDbusy;
		return;
	}
	r->sdreq = sdreq;
	memset(r->req, 0, W2B(12));
	r->req[0] = SataPassthrough<<24;	/* Function */
	r->req[0] |= u->devh;			/* DevHandle */
	r->req[1] = 1<<4;			/* PIO */
	if (sdreq->write)
		r->req[1] |= 1<<1;		/* WRITE */
	else
		r->req[1] |= 1<<0;		/* READ */
	r->req[6] = sdreq->dlen;			/* DataLength */
	memmove(r->req+7, sdreq->cmd, sdreq->clen);
	mksge32(r->req+12, sdreq->data, sdreq->dlen, sdreq->write);

	desc[0] = r->smid<<16 | 0x4<<1;		/* Default Request */
	desc[1] = 0;

	/*
	 * Pernicious hack to ensure certain commands are
	 * able to run to completion.
	 */
	switch (sdreq->cmd[2]) {
	case 0x92:	/* DOWNLOAD MICROCODE */
	case 0xf4:	/* SECURITY ERASE */
	case 0xf5:	/* SECURITY FREEZE LOCK */
		ms = 180*1000;
		break;
	default:
		ms = 10*1000;
	}
	postio(ctlr, r, desc, ms);
	freereq(ctlr, r);
}

#define REPLY(ctlr, n)		((ctlr)->reply + (n)*1)
#define REPLYPOST(ctlr, n)	((ctlr)->replypost + (n)*2)

static u32int *
reallocreply(Ctlr *ctlr)
{
	free(ctlr->reply);

	/*
	 * System Reply Message Frames are less disciplined; they
	 * do not have to be contiguous, must be aligned on a
	 * 4-byte boundary, and must be a multiple of 4 bytes in
	 * length.
	 */
	ctlr->replyq = ctlr->reqcredit + 32;	/* PG §3.1.3 */
	ctlr->reply = mallocz(ctlr->replyq * W2B(ctlr->replyfsz), 0);
	if(ctlr->reply == nil)
		print("reallocreply: %s: out of memory\n", ctlr->name);
	return ctlr->reply;
}

static u32int *
reallocreplyfree(Ctlr *ctlr)
{
	free(ctlr->replyfree);

	/*
	 * The Reply Free Queue must be allocated contiguously,
	 * aligned on a 16-byte boundary, and must have a depth
	 * that is a multiple of 16 bytes.
	 */
	ctlr->replyfreeq = ROUNDUP(ctlr->replyq + 1, 16);
	ctlr->replyfree = mallocalign(ctlr->replyfreeq * W2B(1), 16, 0, 0);
	if(ctlr->replyfree == nil)
		print("reallocreplyfree: %s: out of memory\n", ctlr->name);
	return ctlr->replyfree;
}

static u32int *
reallocreplypost(Ctlr *ctlr)
{
	free(ctlr->replypost);

	/*
	 * The Reply Descriptor Post Queue must be allocated
	 * contiguously and aligned on a 16-byte boundary.  The
	 * depth must not exceed MaxReplyDescriptorPostQueueDepth
	 * returned in the IOC Facts reply.
	 */
	ctlr->replypostq = MIN(ctlr->replypostmax,
	    ROUNDUP(ctlr->replyq + ctlr->reqcredit + 1, 16));
	ctlr->replypost = mallocalign(ctlr->replypostq * W2B(2), 16, 0, 0);
	if(ctlr->replypost == nil)
		print("reallocreplypost: %s: out of memory\n", ctlr->name);
	return ctlr->replypost;
}

static void
freereply(Ctlr *ctlr, u32int *reply)
{
	ctlr->replyfree[ctlr->replyfreep] = PCIWADDR(reply);
	ctlr->replyfreep++;
	if(ctlr->replyfreep == ctlr->replyfreeq)
		ctlr->replyfreep = 0;
}

static u32int *
nextreplypost(Ctlr *ctlr)
{
	u32int *desc;

	desc = REPLYPOST(ctlr, ctlr->replypostp);
	if(desc[0] == 0xffffffff)
	if(desc[1] == 0xffffffff)
		return nil;
	return desc;
}

static void
freereplypost(Ctlr *ctlr, u32int *desc)
{
	desc[0] = 0xffffffff;
	desc[1] = 0xffffffff;
	ctlr->replypostp++;
	if(ctlr->replypostp == ctlr->replypostq)
		ctlr->replypostp = 0;
}

static void
scsiok(Ctlr *ctlr, u16int smid)
{
	Req *r;

	r = REQ(ctlr, smid);
	r->sdreq->status = SDok;
	r->sdreq->rlen = r->sdreq->dlen;
	r->done++;
	wakeup(r);
}

static void
scsierror(Ctlr *ctlr, u16int smid, u32int *reply)
{
	Req *r;

	ckstatus(ctlr, reply);
	r = REQ(ctlr, smid);
	r->sdreq->status = reply[3] & 0xff;	/* SCSIStatus */
	if(reply[3]>>8 & 1<<0)			/* SCSIState (Sense Valid) */
		r->sdreq->flags |= SDvalidsense;
	r->sdreq->rlen = reply[5];		/* TransferCount */
	r->done++;
	wakeup(r);
}

static void
doreply(Ctlr *ctlr, u16int smid, u32int *reply)
{
	Req *r;

	ckstatus(ctlr, reply);
	r = REQ(ctlr, smid);
	r->done++;
	wakeup(r);
}

/*
 * temporary hack
 */
static int
port2unit(Ctlr *ctlr, uint devh, uint /*exph*/, uint ench)
{
	int unitidx, encidx;
	u32int slot, page[14];

	/* hack: ignore direct ports when all phys externally mapped */
	if(ctlr->allphysenc && ctlr->direct == -1)
		encidx = ench - 2;
	else
		encidx = ench - 1;
	if(encidx < 0 || encidx >= Nenclosure)
		return -1;

	if(devpage(ctlr, devh, page) == -1)
		return -1;
	slot = page[2] & 0xffff;
	if(slot >= Nunitperenc)
		return -1;
	unitidx = encidx*Nunitperenc + slot;

	if(unitidx >= Nunit)
		return -1;

	return unitidx;
}

/* sleeze; try to detect when no direct connect */
static int
anyconnected(u32int *data, int e)
{
	int i;
	u32int *p, reason;

	for(i = 0; i < e; i++){
		p = data + i;
		reason = *p>>24 & 0xf;
		if(reason == 1 || reason == 2)
			return 1;
	}
	return 0;
}

static void
topoevent(Ctlr *ctlr, u32int *data)
{
	u32int *p;
	int i, b, e, rate, reason, devh, unitno, exph, ench, num, start;
	Unit *u;

	exph = data[0] >> 16;
	ench = data[0] & 0xffff;
	start = data[2]>>8 & 0xff;
	num = data[2]>>0 & 0xff;
	print("%s: topoevent exp/enc %d/%d port %d-%d #phys %d\n", ctlr->name, 
		exph, ench, start, start+num, data[1]);

	/*
	 * SAS topology changes are handled in two phases;  we
	 * first obtain identifying information from event data.
	 * New units require additional configuration information
	 * and missing units must have resources released.
	 */
	b = data[2]>>8 & 0xff;
	e = data[2] & 0xff;			/* NumEntries */

	if(exph == 0 && ench == 1){
		if(!anyconnected(data + 3, e)){
			if(ctlr->direct == 0)
				ctlr->direct = -1;
			return;
		}
		if(ctlr->direct == 0)
			ctlr->direct = 1;
	}

	for(i = 0; i < e; i++){
		p = data + 3 + i;
		rate = p[0]>>16 & 0xff;
		devh = p[0] & 0xffff;
		reason = *p>>24 & 0xf;
		if(devh == 1 || devh == 0)
			continue;		/* ignore self; empty */
		unitno = port2unit(ctlr, devh, exph, ench);
		if(unitno == -1 || unitno >= ctlr->sdev->nunit){
			print("   entry %d exph %d ench %d unitno %d\n", i, exph, ench, unitno);
			continue;
		}
		u = UNIT(ctlr, unitno);
		switch(reason){		/* PhyStatus (Reason Code) */
		case 1:			/* online */
			unitconfig(ctlr, u, devh);
			u->exph = exph;
			u->ench = ench;
			print("unit %d→%d slot %d:%d devh %.4ux  rate %x stat %x online wwn %llux\n",
				i+b, unitno, ench, u->slot, devh, rate, *p>>28, u->wwn);
			u->state = Online;
			break;
		case 2:			/* offline */
			print("unit %d devh %.4ux  rate %x offline\n", i+b, devh, rate);
			u->state = Offline;
			unitreset(ctlr, u);
			unitremove(ctlr, u);
			break;
		default:
			print("unit %d devh %.4ux  rate %x ?? = %d\n", i+b, devh, rate, rate);
			break;
		}
		u->link = rate;	/* LinkRate */
	}
}

static void
encevent(Ctlr *ctlr, u32int *data)
{
	char *reason;
	int s, n, phymap;

	phymap = data[4];
	switch(data[0]>>16 & 0xff){
	default:
		reason = "*gok*";
		break;
	case 1:
		/* hack: assume 8 phys */
		if(phymap == 0xff)
			ctlr->allphysenc++;
		reason = "added";
		break;
	case 2:
		reason = "gone";
		break;
	}
	s = data[3] >> 16;
	n = data[3] & 0xffff;
	print("mpt2: enc %d %s: port %d wwn %llux slots %d-%d phy %b\n", data[0]>>0 & 0xffff, reason,
		data[0]>>24, (uvlong)data[2]<<32 | data[1], s, s+n, phymap);
}

static void
doevent(Ctlr *ctlr, u32int *reply)
{
	ushort event;
	u32int context;
	u32int *data;

	event = reply[5] & 0xffff;		/* Event */
	context = reply[6];			/* EventContext */
	data = reply + 7;			/* EventData */
	switch(event){
	case SasTopologyChangeList:
		topoevent(ctlr, data);
		break;
	case SasEnclDeviceStatusChange:
		encevent(ctlr, data);
		break;
	default:
		print("sdmpt2: doevent: %s: bad event %#.4ux", ctlr->name, event);
		break;
	}
	if(reply[1]>>16 & 0xff)			/* AckRequired */
		eventack(ctlr, event, context);
}

static void
qevent(Ctlr *ctlr, u32int *reply)
{
	int n;
	Block *bp;

	n = W2B(reply[0]>>16 & 0xff);		/* MsgLength */
	bp = iallocb(n);
	if(bp == nil)
		panic("qevent: %s: out of memory", ctlr->name);
	memmove(bp->wp, reply, n);
	bp->wp += n;
	qpassnolim(ctlr->eventq, bp);
}

static void
satareply(Ctlr *ctlr, u16int smid, u32int *reply)
{
	Req *r;
	SDreq *sr;

	r = REQ(ctlr, smid);
	sr = r->sdreq;
	if(ckstatus(ctlr, reply) == 1){
		if ((reply[3]>>8 & 0xff) == 0x00)	/* SASStatus (SUCCESS) */
			sr->status = SDok;
		else
			sr->status = SDretry;
		memmove(sr->data, reply+5, W2B(5));	/* StatusFIS */
		sr->rlen = W2B(5);
		sr->dlen = reply[11];		/* TransferCount */
	} else
		sr->status = SDretry;

	r->done++;
	wakeup(r);
}

static void
addressreply(Ctlr *ctlr, u16int smid, u32int *reply)
{
	uchar fn;

	fn = reply[0]>>24;			/* Function */
	switch(fn){
	case PortEnable:
	case ScsiTaskManagement:
	case SasIoUnitControl:
		doreply(ctlr, smid, reply);
		break;
	case SataPassthrough:
		satareply(ctlr, smid, reply);
		break;
	case ScsiIoRequest:
		/*
		 * Address Replies to SCSI IO Requests always
		 * indicate an error; see MPI §9.4.
		 */
		scsierror(ctlr, smid, reply);
		break;
	case EventNotification:
		/*
		 * We queue events for handling in a separate
		 * process to ensure sanity when the IOC
		 * requires synchronization via acknowledgement.
		 */
		qevent(ctlr, reply);
		break;
	default:
		panic("addressreply: %s: bad function %#.02ux", ctlr->name, fn);
	}

	/*
	 * To simplify handing a System Reply Message Frame back
	 * to the IOC, we update the ReplyFreeHostIndex register
	 * immediately.  Unfortunately, this requires additional
	 * coherence.
	 */
	freereply(ctlr, reply);
	coherence();
	iocwrite(ctlr, ReplyFreeHostIndex, ctlr->replyfreep);
}

static void
freectlr(Ctlr *ctlr)
{
	iofree(ctlr->port);
	qfree(ctlr->eventq);
	free(ctlr->req);
	free(ctlr->reply);
	free(ctlr->replyfree);
	free(ctlr->replypost);
	free(ctlr->sdev);
	free(ctlr);
}

static void
resetctlr(Ctlr *ctlr)
{
	u32int val;

	iocwrite(ctlr, WriteSequence, 0);	/* flush */
	iocwrite(ctlr, WriteSequence, 0xf);
	iocwrite(ctlr, WriteSequence, 0x4);
	iocwrite(ctlr, WriteSequence, 0xb);
	iocwrite(ctlr, WriteSequence, 0x2);
	iocwrite(ctlr, WriteSequence, 0x7);
	iocwrite(ctlr, WriteSequence, 0xd);

	val = iocread(ctlr, HostDiagnostic);
	val |= ResetAdapter;
	iocwrite(ctlr, HostDiagnostic, val);

	delay(50);	/* don't touch! */

	val = iocwait(ctlr, HostDiagnostic, ResetAdapter, 0);
	if(val & HcbMode)
		panic("resetctlr: %s: host boot not supported", ctlr->name);
	val &= ~HoldIocReset;
	iocwrite(ctlr, HostDiagnostic, val);
	iocwrite(ctlr, WriteSequence, 0xf);	/* disable */

	iocwait(ctlr, Doorbell, StateMask, 1);
	ckstate(ctlr, StateReady);
}

static int
initctlr(Ctlr *ctlr)
{
	int i;
	Req *r;
	u32int *p;

	memset(ctlr->unit, 0, sizeof ctlr->unit);

	/*
	 * Each time the controller is reset, an IOC Facts reponse
	 * may return different values.  We err on the side of
	 * caution and reallocate resources prior to issuing an
	 * IOC Init request.
	 */
	iocfacts(ctlr);
	if(reallocreq(ctlr))
	if(reallocreply(ctlr))
	if(reallocreplyfree(ctlr))
	if(reallocreplypost(ctlr))
		goto init;
	return -1;
init:
	iocinit(ctlr);

	/*
	 * Initialize System Request Message Frames and associated
	 * structures.  A SMID is written once to avoid headaches
	 * constructing messages in the I/O path.  A SMID of 0 must
	 * be initialized and is considered reserved; it may not be
	 * placed on the free list or used by the host in any way.
	 */
	ctlr->reqfree = nil;
	for(i = 0; i < ctlr->reqcredit; ++i)
		if(i > 0){
			r = REQ(ctlr, i);
			r->smid = i;
			r->req = (u32int *)r - ctlr->iocreqfsz;
			freereq(ctlr, r);
		}

	/*
	 * The Reply Descriptor Post Queue must be initialized with
	 * the unused descriptor type for each entry.  This is
	 * slightly reordered to take advantage of coherence
	 * required when updating the ReplyFreeHostIndex register
	 * below.
	 */
	ctlr->replypostp = 0;
	for(i = 0; i < ctlr->replypostq; ++i){
		p = REPLYPOST(ctlr, i);
		freereplypost(ctlr, p);
	}

	/*
	 * The Reply Free Queue is initialized with the lower
	 * 32 bits of the PADDR for each System Reply Frame.  The
	 * ReplyFreeHostIndex register is initialized with the
	 * next (unused) entry.
	 */
	ctlr->replyfreep = 0;
	for(i = 0; i < ctlr->replyq; ++i){
		p = REPLY(ctlr, i);
		freereply(ctlr, p);
	}
	coherence();
	iocwrite(ctlr, ReplyFreeHostIndex, ctlr->replyfreep);

	/*
	 * Enable event notifications.
	 */
	qreopen(ctlr->eventq);
	eventnotify(ctlr);
	return 0;
}

static void
enablectlr(Ctlr *ctlr)
{
	u32int val;

	val = iocread(ctlr, HostInterruptMask);
	val &= ~ReplyIntMask;
	iocwrite(ctlr, HostInterruptMask, val);
}

static void
disablectlr(Ctlr *ctlr)
{
	u32int val;

	val = iocread(ctlr, HostInterruptMask);
	val |= ReplyIntMask;
	iocwrite(ctlr, HostInterruptMask, val);
}

static int
ctlrunits(Ctlr *ctlr)
{
	char *s;
	int units, n;

	/* start with 1:1 direct mapping */
	units = ctlr->numports;

	/* manually configured expander? */
	if((s = getconf("*mpt2expander")) != nil)
	if((n = atoi(s)) > 0)
		units = n;

	/* autodetected expander? (guess based on a single port) */
	if(units == 1 || units > Nunit)
		units = Nunit;

	return units;
}

static SDev *
mpt2pnp(void)
{
	Pcidev *p;
	SDev *sdev;
	int ioc;
	char name[8];
	Ctlr *ctlr;
	static int iocs;

	p = nil;
	while(p = pcimatch(p, 0x1000, 0)){
		switch(p->did){
		case 0x0070:	/* LSISAS2004 */
		case 0x0072:	/* LSISAS2008 */
		case 0x0074:	/* LSISAS2108 */
		case 0x0076:
		case 0x0077:
		case 0x0064:	/* LSISAS2116 */
		case 0x0065:
		case 0x0080:	/* LSISAS2208 */
		case 0x0081:
		case 0x0082:
		case 0x0083:
		case 0x0084:
		case 0x0085:
		case 0x0086:	/* LSISAS2308 */
		case 0x0087:
			break;
		default:
			continue;
		}
		ioc = iocs++;
		snprint(name, sizeof name, "ioc%d", ioc);
		ctlr = malloc(sizeof(Ctlr));
		if(ctlr == nil){
			print("mpt2pnp: %s: out of memory\n", name);
			continue;
		}
		ctlr->ioc = ioc;
		strncpy(ctlr->name, name, sizeof(ctlr->name));
		ctlr->port = ioalloc(p->mem[0].bar & ~1, p->mem[0].size, 0, "mpt2");
		if(ctlr->port < 0){
			print("mpt2pnp: %s: ioalloc failed\n", name);
			freectlr(ctlr);
			continue;
		}
		pcisetbme(p);
		ctlr->pcidev = p;
		ctlr->eventq = qopen(0, Qmsg | Qclosed, nil, nil);
		if(ctlr->eventq == nil){
			print("mpt2pnp: %s: qopen failed\n", name);
			freectlr(ctlr);
			continue;
		}
		resetctlr(ctlr);
		if(initctlr(ctlr) < 0){
			print("mpt2pnp: %s: initctlr failed\n", name);
			freectlr(ctlr);
			continue;
		}
		sdev = malloc(sizeof(SDev));
		if(sdev == nil){
			print("mpt2pnp: %s: out of memory\n", name);
			freectlr(ctlr);
			continue;
		}
		sdev->ifc = &sdmpt2ifc;
		sdev->ctlr = ctlr;
		sdev->idno = 'M';
		sdev->nunit = ctlrunits(ctlr);
		ctlr->sdev = sdev;
		sdadddevs(sdev);
		print("#S/%s: %s: mpt2 sas-2 with %d ports, %d max targets\n",
		    sdev->name, name, ctlr->numports, ctlr->maxtargs);
	}
	return nil;
}

static void
mpt2interrupt(Ureg *, void *arg)
{
	Ctlr *ctlr;
	u32int val, *desc, *reply;
	u16int smid;

	ctlr = arg;
	ilock(ctlr);
	val = iocread(ctlr, HostInterruptStatus);
	if(!(val & ReplyDescriptorInterrupt)){
		iunlock(ctlr);
		return;
	}
	for(;;){
		desc = nextreplypost(ctlr);
		if(desc == nil)
			break;
		switch(desc[0] & 0xf){		/* ReplyFlags */
		case ScsiIoSuccess:
			smid = desc[0]>>16;	/* SMID */
			scsiok(ctlr, smid);
			break;
		case AddressReply:
			smid = desc[0]>>16;	/* SMID */
			reply = KADDR(desc[1]);	/* ReplyFrameAddress */
			addressreply(ctlr, smid, reply);
			break;
		default:
			panic("mpt2interrupt: %s: bad reply %#.8ux %#.8ux",
			    ctlr->name, desc[0], desc[1]);
		}
		freereplypost(ctlr, desc);
	}
	iunlock(ctlr);	/* coherence */
	iocwrite(ctlr, ReplyPostHostIndex, ctlr->replypostp);
}

static void
mpt2reset(void *arg)
{
	Ctlr *ctlr;

	ctlr = arg;
	for(;;){
		while(waserror())
			;
		sleep(&ctlr->reset, return0, nil);
		poperror();

		qclose(ctlr->eventq);

		wlock(&ctlr->resetlock);
		ilock(ctlr);
		disablectlr(ctlr);
		resetctlr(ctlr);
		if(initctlr(ctlr) < 0){
			iunlock(ctlr);
			wunlock(&ctlr->resetlock);
			print("mpt2reset: %s: initctlr failed\n", ctlr->name);
			continue;
		}
		enablectlr(ctlr);
		iunlock(ctlr);
		wunlock(&ctlr->resetlock);

		if(waserror())
			print("mpt2reset: %s: %s\n",ctlr->name, up->errstr);
		else{
			portenable(ctlr);
			poperror();
		}
	}
}

static void
mpt2event(void *arg)
{
	Ctlr *ctlr;
	Block *bp;
	u32int *reply;

	/*
	 * For the unwary, a pending reset will first close the
	 * eventq, which will cause qbread to eventually error.
	 * The control structure below yields to the reset kproc
	 * until the eventq is reopened and sanity is restored.
	 */
	ctlr = arg;
	while(waserror())
		yield();
	for(;;){
		rlock(&ctlr->resetlock);
		if(waserror()){
			runlock(&ctlr->resetlock);
			nexterror();
		}
		bp = qbread(ctlr->eventq, 0);
		if(bp)
		if(waserror())
			wakeup(&ctlr->reset);
		else{
			reply = (u32int *)bp->rp;
			doevent(ctlr, reply);
			poperror();
		}
		freeb(bp);
		poperror();
		runlock(&ctlr->resetlock);
	}
}

/* hotpluggable.  report all the ports (or slots) we've got */
static int
mpt2verify(SDunit*)
{
//	scsiverify(u);
	return 1;
}

static int
mpt2enable(SDev *sdev)
{
	Ctlr *ctlr;
	Pcidev *p;
	char name[32];

	ctlr = sdev->ctlr;
	p = ctlr->pcidev;
	snprint(name, sizeof name, "%s (%s)", sdev->name, sdev->ifc->name);
	intrenable(p->intl, mpt2interrupt, ctlr, p->tbdf, name);

	ilock(ctlr);
	enablectlr(ctlr);
	iunlock(ctlr);

	if(!ctlr->enabled++){
		kproc("mpt2reset", mpt2reset, ctlr);
		kproc("mpt2event", mpt2event, ctlr);
	}
	if(waserror())
		print("mpt2enable: %s: %s\n", ctlr->name, up->errstr);
	else{
		portenable(ctlr);
		poperror();
	}
	return 1;
}

static int
mpt2disable(SDev *sdev)
{
	Ctlr *ctlr;

	ctlr = sdev->ctlr;
	ilock(ctlr);
	disablectlr(ctlr);
	iunlock(ctlr);

	intrdisable(ctlr->vector);
	return 1;
}

static int
mpt2rio(SDreq *sdreq)
{
	Ctlr *ctlr;
	Unit *u;

	ctlr = sdreq->unit->dev->ctlr;
	u = UNIT(ctlr, sdreq->unit->subno);
	rlock(&ctlr->resetlock);
	if(waserror()){
		runlock(&ctlr->resetlock);
		return SDeio;
	}
	if(u->state != Online)
		error(Ebadunit);
	if(waserror()){
		wakeup(&ctlr->reset);
		nexterror();
	}
	scsiio(ctlr, u, sdreq);
	poperror();
	poperror();
	runlock(&ctlr->resetlock);
	return sdreq->status;
}

static int
mpt2ataio(SDreq *sdreq)
{
	Ctlr *ctlr;
	Unit *u;

	ctlr = sdreq->unit->dev->ctlr;
	u = UNIT(ctlr, sdreq->unit->subno);
	rlock(&ctlr->resetlock);
	if(waserror()){
		runlock(&ctlr->resetlock);
		return SDeio;
	}
	if(u->state != Online)
		error(Ebadunit);
	if(waserror()){
		wakeup(&ctlr->reset);
		nexterror();
	}
	ataio(ctlr, u, sdreq);
	poperror();
	poperror();
	runlock(&ctlr->resetlock);
	return sdreq->status;
}

static int
mpt2rctl(SDunit *unit, char *p, int l)
{
	Ctlr *ctlr;
	Unit *u;
	char *o, *e;
	char buf[48];

	ctlr = unit->dev->ctlr;
	if(ctlr == nil)
		return 0;
	o = p;
	e = p + l;
	u = UNIT(ctlr, unit->subno);
	getunitcaps(u, buf, sizeof buf);
	p = seprint(p, e, "capabilities %s\n", buf);
	p = seprint(p, e, "wwn %.16llux\n", u->wwn);
	p = seprint(p, e, "type %s\n", unittype(u));
	p = seprint(p, e, "link %s\n", unitlink(u));
	p = seprint(p, e, "state %s\n", unitstate(u));
	p = seprint(p, e, "geometry %llud %ud\n",
	    unit->sectors, unit->secsize);
	p = seprint(p, e, "slot %d:%d\n", u->ench, u->slot);

	return p - o;
}

static char *
mpt2rtopctl(SDev *sdev, char *p, char *e)
{
	Ctlr *ctlr;

	ctlr = sdev->ctlr;
	return seprint(p, e, "%s mpt2 %s port %ux irq %d\n",
	    sdev->name, ctlr->name, ctlr->port, ctlr->pcidev->intl);
}

static int
mpt2online(SDunit *unit)
{
	int r;
	Ctlr *ctlr;
	Unit *u;

	ctlr = unit->dev->ctlr;
	u = ctlr->unit + unit->subno;

	/* short circuit if state has not changed */
	if(u->state == Online ^ unit->sectors == 0)
		return 1;
	u->drivechange = 0;
	r = scsionlinex(unit, u) == SDok;
	if(r){
		unit->sectors = u->sectors;
		unit->secsize = u->secsize;
	}
	return r;
}

static long
mpt2bio(SDunit *unit, int lun, int write, void *data, long count, uvlong lba)
{
	Ctlr *ctlr;
	Unit *u;

	ctlr = unit->dev->ctlr;
	u = ctlr->unit + unit->subno;
//	if(u->feat & Datapi || u->type == Sas)
		return scsibiox(unit, u, lun, write, data, count, lba);
//	return atabio(unit, u, lun, write, data, count, lba);
}

SDifc sdmpt2ifc = {
	"mpt2",				/* name */

	mpt2pnp,			/* pnp */
	nil,				/* legacy */
	mpt2enable,			/* enable */
	mpt2disable,			/* disable */

	mpt2verify,			/* verify */
	mpt2online,			/* online */
	mpt2rio,			/* rio */
	mpt2rctl,			/* rctl */
	nil,				/* wctl */

	mpt2bio,			/* bio */
	nil,				/* probe */
	nil,				/* clear */
	mpt2rtopctl,			/* rtopctl */
	nil,				/* wtopctl */
	mpt2ataio,			/* ataio */
};
