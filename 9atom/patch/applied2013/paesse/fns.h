#include "../port/portfns.h"

void	aamloop(int);
Dirtab*	addarchfile(char*, int, long(*)(Chan*,void*,long,vlong), long(*)(Chan*,void*,long,vlong));
void	archinit(void);
void	bootargs(void*);
usize	cankaddr(uintmem);
void	clockintr(Ureg*, void*);
#define	cmpswap(a, b, c) cmpswap486(a, b, c)
int	cmpswap486(long*, long, long);
void	(*coherence)(void);
void	cpuid(int, Cpuidreg*);
void	cpuid1(Cpuidreg*);
int	cpuidentify(void);
void	cpuidprint(void);
void	cycles(uvlong*);
void	delay(int);
#define	evenaddr(x)				/* x86 doesn't care */
void	fpclear(void);
void	fpinit(void);
void	fpoff(void);
void	fprestore(FPsave*);
void	fpsave(FPsave*);
u32int	getcr0(void);
u32int	getcr2(void);
u32int	getcr3(void);
u32int	getcr4(void);
char*	getconf(char*);
void	guesscpuhz(int);
void	halt(void);
int	i8042auxcmd(int);
int	i8042auxcmds(uchar*, int);
void	i8042auxenable(void (*)(int, int));
void	i8042reset(void);
void	i8250console(void);
void*	i8250alloc(int, int, int);
void	i8250mouse(char*, int (*)(Queue*, int), int);
void	i8250setmouseputc(char*, int (*)(Queue*, int));
void	i8253enable(void);
void	i8253init(void);
void	i8253link(void);
uvlong	i8253read(uvlong*);
void	i8253timerset(uvlong);
int	i8259disable(Vctl*);
int	i8259enable(Vctl*);
void	i8259init(void);
int	i8259isr(int);
void	i8259on(void);
void	i8259off(void);
void	idle(void);
void	idlehands(void);
int	inb(int);
void	insb(int, void*, int);
ushort	ins(int);
void	inss(int, void*, int);
ulong	inl(int);
void	insl(int, void*, int);
int	intrdisable(int, void (*)(Ureg *, void *), void*, int, char*);
int	intrenable(int, void (*)(Ureg*, void*), void*, int, char*);
void	introff(void);
void	intron(void);
void	invlpg(uintptr);
void	iofree(int);
void	ioinit(void);
int	iounused(int, int);
int	ioalloc(int, int, int, char*);
int	ioreserve(int, int, int, char*);
int	iprint(char*, ...);
int	isaconfig(char*, int, ISAConf*);
void*	kaddr(uintmem);
void	kbdenable(void);
void	kbdinit(void);
#define	kmapinval()
void	lgdt(ushort[3]);
void	lidt(ushort[3]);
void	links(void);
void	ltr(ulong);
void	mach0init(void);
void	mathinit(void);
void	mb586(void);
void	meminit(void);
void	memorysummary(void);
void	mfence(void);
#define mmuflushtlb() putcr3(PADDR(m->pdpt))
void	mmuidle(void);
void	mmuinit(void);
PTE*	mmuwalk(PTE*, uintptr, int, int);
uchar	nvramread(int);
void	nvramwrite(int, uchar);
void	outb(int, int);
void	outsb(int, void*, int);
void	outs(int, ushort);
void	outss(int, void*, int);
void	outl(int, u32int);
void	outsl(int, void*, int);
uintmem	paddr(void*);
int	pcicap(Pcidev*, int);
int	pcicfgr8(Pcidev*, int);
int	pcicfgr16(Pcidev*, int);
int	pcicfgr32(Pcidev*, int);
void	pcicfgw8(Pcidev*, int, int);
void	pcicfgw16(Pcidev*, int, int);
void	pcicfgw32(Pcidev*, int, int);
void	pciclrbme(Pcidev*);
void	pciclrioe(Pcidev*);
void	pciclrmwi(Pcidev*);
int	pcigetpms(Pcidev*);
void	pcihinv(Pcidev*);
uchar	pciipin(Pcidev*, uchar);
Pcidev* pcimatch(Pcidev*, int, int);
Pcidev* pcimatchtbdf(int);
void	pcireset(void);
int	pciscan(int, Pcidev**);
void	pcisetbme(Pcidev*);
void	pcisetioe(Pcidev*);
void	pcisetmwi(Pcidev*);
int	pcisetpms(Pcidev*, int);
void	pcmcisread(PCMslot*);
int	pcmcistuple(int, int, int, void*, int);
PCMmap*	pcmmap(int, ulong, int, int);
int	pcmspecial(char*, ISAConf*);
int	(*_pcmspecial)(char *, ISAConf *);
void	pcmspecialclose(int);
void	(*_pcmspecialclose)(int);
void	pcmunmap(int, PCMmap*);
int	pdbmap(PTE*, uintmem, uintptr, int);
void	pdptinit(PTE*, PTE*);
void	procrestore(Proc*);
void	procsave(Proc*);
void	procsetup(Proc*);
void	putcr0(u32int);
void	putcr3(u32int);
void	putcr4(u32int);
void*	rampage(void);
void	rdmsr(int, vlong*);
void	realmode(Ureg*);
void	screeninit(void);
void	(*screenputs)(char*, int);
void	sfence(void);
int	strtotbdf(char*, char**, int);
void	syncclock(void);
void	syscallfmt(int syscallno, uintptr pc, va_list list);
void	sysretfmt(int syscallno, va_list list, long ret, uvlong start, uvlong stop);
void*	tmpmap(Page*);
void	tmpunmap(void*);
void	touser(void*);
void	trapenable(int, void (*)(Ureg*, void*), void*, char*);
void	trapinit(void);
void	trapinit0(void);
int	tas(void*);
uvlong	tscticks(uvlong*);
ulong	umbmalloc(ulong, int, int);
void	umbfree(ulong, int);
ulong	umbrwmalloc(ulong, int, int);
void	umbrwfree(ulong, int);
ulong	upaalloc(int, int);
void	upafree(ulong, int);
void	upareserve(ulong, int);
#define	userureg(ur) (((ur)->cs & 0xFFFF) == UESEL)
void	vectortable(void);
void*	vmap(uintmem, usize);
void*	vmappat(uintmem, usize, int);
int	vmapsync(uintptr);
void	vunmap(void*, usize);
void	wbinvd(void);
void	wrmsr(int, vlong);
int	xchgw(ushort*, int);

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))
#define	KADDR(a)	kaddr(a)
#define PADDR(a)	paddr((void*)(a))

#define	dcflush(a, b)

#define PTR2UINT(p)	((uintptr)(p))
#define UINT2PTR(i)	((void*)(i))

#define BIOSSEG(a)	KADDR(((uint)(a))<<4)
