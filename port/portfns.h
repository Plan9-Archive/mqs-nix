void		accounttime(void);
void		addbootfile(char*, uchar*, ulong);
Timer*		addclock0link(void (*)(void), int);
int		addconsdev(Queue*, void (*fn)(char*,int), int, int);
int		addkbdq(Queue*, int);
int		addphysseg(Physseg*);
void		addwaitstat(uintptr pc, uvlong t0, int type);
void		addwatchdog(Watchdog*);
int		adec(int*);
Block*		adjustblock(Block*, int);
int		ainc(int*);
void		alarmkproc(void*);
Block*		allocb(int);
char*		allocbstats(char*, char*);
int		anyhigher(void);
int		anyready(void);
void		_assert(char*);
Image*		attachimage(int, Chan*, int, uintptr, uintptr);
Page*		auxpage(usize);
Block*		bl2mem(uchar*, Block*, int);
int		blocklen(Block*);
void		bootlinks(void);
void		cachedel(Image*, ulong);
void		cachepage(Page*, Image*);
void		callwithureg(void (*)(Ureg*));
int		canlock(Lock*);
int		canpage(Proc*);
int		canqlock(QLock*);
int		canrlock(RWlock*);
Chan*		cclone(Chan*);
void		cclose(Chan*);
void		ccloseq(Chan*);
void		chanfree(Chan*);
char*		chanpath(Chan*);
void		checkalarms(void);
void		checkb(Block*, char*);
void		clearwaitstats(void);
void		closeegrp(Egrp*);
void		closefgrp(Fgrp*);
void		closepgrp(Pgrp*);
void		closergrp(Rgrp*);
void		cmderror(Cmdbuf*, char*);
int		cmount(Chan**, Chan*, int, char*);
Block*		concatblock(Block*);
char*		confenv(char*, char*);
int		consactive(void);
void		(*consdebug)(void);
void		(*consputs)(char*, int);
Block*		copyblock(Block*, int);
void		copypage(Page*, Page*);
void		cunmount(Chan*, Chan*);
Segment*	data2txt(Segment*);
uintptr		dbgpc(Proc*);
int		decref(Ref*);
void		delay(int);
Proc*		dequeueproc(Sched*, Schedq*, Proc*);
Chan*		devattach(int, char*);
Block*		devbread(Chan*, long, vlong);
long		devbwrite(Chan*, Block*, vlong);
Chan*		devclone(Chan*);
int		devconfig(int, char *, DevConf *);
void		devcreate(Chan*, char*, int, int);
void		devdir(Chan*, Qid, char*, vlong, char*, long, Dir*);
long		devdirread(Chan*, char*, long, Dirtab*, int, Devgen*);
Devgen		devgen;
void		devinit(void);
Chan*		devopen(Chan*, int, Dirtab*, int, Devgen*);
void		devpermcheck(char*, int, int);
void		devpower(int);
void		devremove(Chan*);
void		devreset(void);
void		devshutdown(void);
long		devstat(Chan*, uchar*, long, Dirtab*, int, Devgen*);
Dev*		devtabget(int, int);
void		devtabinit(void);
long		devtabread(Chan*, void*, long, vlong);
void		devtabreset(void);
void		devtabshutdown(void);
Walkqid*		devwalk(Chan*, Chan*, char**, int, Dirtab*, int, Devgen*);
long		devwstat(Chan*, uchar*, long);
void		drawactive(int);
void		drawcmap(void);
void		dumpaproc(Proc*);
void		dumpregs(Ureg*);
void		dumpstack(void);
Fgrp*		dupfgrp(Fgrp*);
int		duppage(Page*);
Segment*	dupseg(Segment**, int, int);
void		dupswap(Page*);
char*		edfadmit(Proc*);
void		edfinit(Proc*);
int		edfready(Proc*);
void		edfrecord(Proc*);
void		edfrun(Proc*, int);
void		edfstop(Proc*);
void		edfyield(void);
int		emptystr(char*);
void		envcpy(Egrp*, Egrp*);
int		eqchanddq(Chan*, int, uint, Qid, int);
int		eqqid(Qid, Qid);
void		error(char*);
void		exhausted(char*);
void		exit(int);
uvlong		fastticks(uvlong*);
uvlong		fastticks2ns(uvlong);
uvlong		fastticks2us(uvlong);
int		fault(uintptr, int);
void		fdclose(int, int);
Chan*		fdtochan(int, int, int, int);
int		findmount(Chan**, Mhead**, int, uint, Qid);
int		fixfault(Segment*, uintptr, int, int, int);
void		fmtinit(void);
void		forceclosefgrp(void);
void		free(void*);
void		freeb(Block*);
void		freeblist(Block*);
int		freebroken(void);
void		freepte(Segment*, Pte*);
int		getpgszi(usize);
void		gotolabel(Label*);
int		haswaitq(void*);
void		hnputl(void*, uint);
void		hnputs(void*, ushort);
void		hnputv(void*, uvlong);
long		hostdomainwrite(char*, long);
long		hostownerwrite(char*, long);
void		hzsched(void);
Block*		iallocb(int);
void		iallocsummary(void);
void		ilock(Lock*);
Mach* 		imbalance(void);
int		incref(Ref*);
void		initimage(void);
void		interruptsummary(void);
int		iprint(char*, ...);
void		isdir(Chan*);
int		iseve(void);
int		islo(void);
Segment*	isoverlap(Proc*, uintptr, usize);
int		isphysseg(char*);
void		iunlock(Lock*);
void		ixsummary(void);
int		kbdcr2nl(Queue*, int);
Kbscan*		kbdnewscan(void (*)(char*));
void		kbdfreescan(Kbscan*);
void		kbdputsc(int c, Kbscan*);
int		kbdputc(Queue*, int);
void		kbdputmap(ushort, ushort, Rune);
void		kickpager(int, int);
void		killbig(char*);
void		kproc(char*, void(*)(void*), void*);
void		kprocchild(Proc*, void (*)(void*), void*);
void		(*kproftimer)(uintptr);
void		ksetenv(char*, char*, int);
void		kstrcpy(char*, char*, int);
void		kstrdup(char**, char*);
void 		loadbalance(void);
long		latin1(Rune*, int);
int		lock(Lock*);
uintptr		lockgetpc(Lock*);
void		locksetpc(Lock*, uintptr);
void		log(Log*, int, char*, ...);
int		log2ceil(uintmem);
void		log2init(void);
void		logclose(Log*);
char*		logctl(Log*, int, char**, Logflag*);
void		logn(Log*, int, void*, int);
void		logopen(Log*);
long		logread(Log*, void*, ulong, long);
Page*		lookpage(Image*, ulong);
Cmdtab*		lookupcmd(Cmdbuf*, Cmdtab*, int);
int		machcolor(Mach*);
void		mallocinit(void);
char*		mallocstats(char*, char*);
void		mallocsummary(void);
Block*		mem2bl(uchar*, int);
void		(*mfcinit)(void);
void		(*mfcopen)(Chan*);
int		(*mfcread)(Chan*, uchar*, int, vlong);
void		(*mfcupdate)(Chan*, uchar*, int, vlong);
void		(*mfcwrite)(Chan*, uchar*, int, vlong);
void		mfreeseg(Segment*, uintptr, uintptr);
void		microdelay(int);
uvlong		mk64fract(uvlong, uvlong);
void		mkqid(Qid*, vlong, ulong, int);
void		mmuflush(void);
void		mmuput(uintptr, Page*, uint);
void		mmurelease(Proc*);
void		mmuswitch(Proc*);
Chan*		mntauth(Chan*, char*);
usize		mntversion(Chan*, u32int, char*, usize);
void		mountfree(Mount*);
uvlong		ms2fastticks(ulong);
#define		MS2NS(n) (((vlong)(n))*1000000LL)
ulong		ms2tk(ulong);
void		mul64fract(uvlong*, uvlong, uvlong);
void		muxclose(Mnt*);
Chan*		namec(char*, int, int, int);
void		nameerror(char*, char*);
Chan*		newchan(void);
int		newfd(Chan*);
Mhead*		newmhead(Chan*);
Mount*		newmount(Mhead*, Chan*, int, char*);
Page*		newpage(int, Segment **, uintptr, usize, int);
Path*		newpath(char*);
Pgrp*		newpgrp(void);
Proc*		newproc(void);
Rgrp*		newrgrp(void);
Segment*	newseg(int, uintptr, uintptr);
void		nexterror(void);
uint		nhgetl(void*);
ushort		nhgets(void*);
uvlong		nhgetv(void*);
int		nrand(int);
uvlong		ns2fastticks(uvlong);
int		okaddr(uintptr, long, int);
int		openmode(int);
int		ownlock(Lock*);
Block*		packblock(Block*);
Block*		padblock(Block*, int);
void		pagechainhead(Page*);
void		pageinit(void);
ulong		pagenumber(Page*);
uvlong		pagereclaim(Image*);
void		pagersummary(void);
void		pageunchain(Page*);
void		panic(char*, ...);
Cmdbuf*		parsecmd(char *a, int n);
void		pathclose(Path*);
uvlong		perfticks(void);
void		pexit(char*, int);
Page*		pgalloc(usize, int);
void		pgfree(Page*);
void		pgrpcpy(Pgrp*, Pgrp*);
void		pgrpnote(ulong, char*, long, int);
uintmem		physalloc(u64int, int*, void*);
void		physdump(void);
void		physfree(uintmem, u64int);
void		physinit(uintmem, u64int, int);
void*		phystag(uintmem);
void		pio(Segment*, uintptr, usize, Page**, int);
#define		poperror()		up->nerrlab--
int		postnote(Proc*, int, char*, int);
int		pprint(char*, ...);
int		preempted(void);
void		prflush(void);
void		printinit(void);
ulong		procalarm(ulong);
void		procctl(Proc*);
void		procdump(void);
int		procfdprint(Chan*, int, int, char*, int);
void		procflushseg(Segment*);
void		procpriority(Proc*, int, int);
void		procrestore(Proc*);
void		procsave(Proc*);
#define		procsaved(p)	((p)->mach == nil)
void		(*proctrace)(Proc*, int, vlong);
void		proctracepid(Proc*);
void		procwired(Proc*, int);
void		psdecref(Proc*);
Proc*		psincref(int);
int		psindex(int);
void		psinit(int);
Pte*		ptealloc(Segment*);
Pte*		ptecpy(Segment*,Pte*);
int		pullblock(Block**, int);
Block*		pullupblock(Block*, int);
Block*		pullupqueue(Queue*, int);
void		putimage(Image*);
void		putmhead(Mhead*);
void		putpage(Page*);
void		putseg(Segment*);
void		putstrn(char*, int);
void		putswap(Page*);
int		pwait(Waitmsg*);
void		qaddlist(Queue*, Block*);
Block*		qbread(Queue*, int);
long		qbwrite(Queue*, Block*);
Queue*		qbypass(void (*)(void*, Block*), void*);
int		qcanread(Queue*);
void		qclose(Queue*);
int		qconsume(Queue*, void*, int);
Block*		qcopy(Queue*, int, ulong);
int		qdiscard(Queue*, int);
void		qflush(Queue*);
void		qfree(Queue*);
int		qfull(Queue*);
Block*		qget(Queue*);
void		qhangup(Queue*, char*);
char*		qiostats(char*, char*);
int		qisclosed(Queue*);
int		qiwrite(Queue*, void*, int);
int		qlen(Queue*);
void		qlock(QLock*);
void		qnoblock(Queue*, int);
Queue*		qopen(int, int, void (*)(void*), void*);
int		qpass(Queue*, Block*);
int		qpassnolim(Queue*, Block*);
int		qproduce(Queue*, void*, int);
void		qputback(Queue*, Block*);
long		qread(Queue*, void*, int);
Block*		qremove(Queue*);
void		qreopen(Queue*);
void		qsetlimit(Queue*, int);
void		qunlock(QLock*);
int		qwindow(Queue*);
int		qwrite(Queue*, void*, int);
int		rand(void);
void		randominit(void);
ulong		randomread(void*, ulong);
void		rdb(void);
int		readnum(ulong, char*, ulong, ulong, int);
long		readstr(long, char*, long, char*);
void		ready(Proc*);
void		forkready(Proc *);
void		reboot(void*, void*, usize);
void		rebootcmd(int, char**);
void		relocateseg(Segment*, uintptr);
void		renameuser(char*, char*);
void		resched(char*);
void		resrcwait(char*);
int		return0(void*);
void		rlock(RWlock*);
long		rtctime(void);
void		runlock(RWlock*);
Proc*		runproc(void);
void		sched(void);
void		scheddump(void);
void		schedinit(void);
char*		schedstats(char*, char*);
long		seconds(void);
Segment*	seg(Proc*, uintptr, int);
void		segclock(uintptr);
Sem*		segmksem(Segment*, int*);
void		segpage(Segment*, Page*);
uintmem		segppn(Segment*, uintmem);
char*		seprintpagestats(char*, char*);
char*		seprintphysstats(char*, char*);
void		setkernur(Ureg*, Proc*);
int		setlabel(Label*);
void		setregisters(Ureg*, char*, char*, int);
char*		skipslash(char*);
void		sleep(Rendez*, int (*)(void*), void*);
void*		smalloc(usize);
void		spldone(void);
Mpl		splhi(void);
Mpl		spllo(void);
void		splx(Mpl);
char*		srvname(Chan*);
void		startwaitstats(int);
int		swapcount(ulong);
void		swapinit(void);
void		synccons(void);
void		syscallfmt(int, va_list list);
void		sysretfmt(int, va_list, Ar0*, uvlong, uvlong);
void*		sysexecregs(uintptr, uint, uint);
uintptr		sysexecstack(uintptr, int);
void		sysrforkchild(Proc*, Proc*);
void		sysprocsetup(Proc*);
#define		tickscmp(a, b)  ((long)((a)-(b)))
void		timeradd(Timer*);
void		timerdel(Timer*);
void		timerintr(Ureg*, void*);
void		timerset(uvlong);
void		timersinit(void);
ulong		tk2ms(ulong);
#define		TK2MS(x) ((x)*(1000/HZ))
uvlong		tod2fastticks(vlong);
vlong		todget(vlong*);
void		todinit(void);
void		todset(vlong, vlong, int);
void		todsetfreq(vlong);
Block*		trimblock(Block*, int, int);
void		tsleep(Rendez*, int (*)(void*), void*, long);
Uart*		uartconsole(int, char*);
int		uartctl(Uart*, char*);
int		uartconsconf(char**);
int		uartgetc(void);
void		uartkick(void*);
void		uartmouse(int, int (*)(Queue*, int), char*);
void		uartputc(int);
void		uartputs(char*, int);
void		uartrecv(Uart*, char);
void		uartsetmouseputc(int, int (*)(Queue*, int));
int		uartstageoutput(Uart*);
void		unbreak(Proc*);
void		uncachepage(Page*);
void		unlock(Lock*);
void		userinit(void);
uintptr		userpc(Ureg*);
long		userwrite(char*, long);
void*		validaddr(void*, long, int);
void		validname(char*, int);
char*		validnamedup(char*, int);
void		validstat(uchar*, usize);
void*		vmemchr(void*, int, int);
Proc*		wakeup(Rendez*);
int		walk(Chan**, char**, int, int, int*);
void		wlock(RWlock*);
void		wunlock(RWlock*);
void		yield(void);
uint		µs(void);

#pragma		varargck	argpos	iprint		1
#pragma		varargck	argpos	panic		1
#pragma		varargck	argpos	pprint		1
