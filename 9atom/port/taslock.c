#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "../port/edf.h"

#pragma profile 0

long maxlockcycles;
long maxilockcycles;
long cumlockcycles;
long cumilockcycles;
uintptr maxlockpc;
uintptr maxilockpc;

struct
{
	ulong	locks;
	ulong	glare;
	ulong	inglare;
} lockstats;

void
lockloop(Lock *l, uintptr pc)
{
	Proc *p;

	p = l->p;
	print("lock %#p loop key %#p pc %#p held by pc %#p proc %lud\n",
		l, l->key, pc, l->pc, p ? p->pid : 0);
	dumpaproc(up);
	if(p != nil)
		dumpaproc(p);
}

int
lock(Lock *l)
{
	int i;
	uintptr pc;

	pc = getcallerpc(&l);

	lockstats.locks++;
	if(up)
		up->nlocks++;		/* prevent being scheded */
	if(tas(&l->key) == 0){
		if(up)
			up->lastlock = l;
		l->pc = pc;
		l->p = up;
		l->isilock = 0;
#ifdef LOCKCYCLES
		l->lockcycles = -lcycles();
#endif
		return 0;
	}
	if(up)
		up->nlocks--;

	lockstats.glare++;
	for(;;){
		lockstats.inglare++;
		i = 0;
		while(l->key){
			if(conf.nmach < 2 && up && up->edf && (up->edf->flags & Admitted)){
				/*
				 * Priority inversion, yield on a uniprocessor; on a
				 * multiprocessor, the other processor will unlock
				 */
				print("inversion %#p pc %#p proc %lud held by pc %#p proc %lud\n",
					l, pc, up ? up->pid : 0, l->pc, l->p ? l->p->pid : 0);
				up->edf->d = todget(nil);	/* yield to process with lock */
			}
			if(i++ > 100000000){
				i = 0;
				lockloop(l, pc);
			}
		}
		if(up)
			up->nlocks++;
		if(tas(&l->key) == 0){
			if(up)
				up->lastlock = l;
			l->pc = pc;
			l->p = up;
			l->isilock = 0;
#ifdef LOCKCYCLES
			l->lockcycles = -lcycles();
#endif
			return 1;
		}
		if(up)
			up->nlocks--;
	}
}

void
ilock(Lock *l)
{
	ulong x;
	uintptr pc;

	pc = getcallerpc(&l);
	if(l == nil)
		panic("ilock nil %#p", pc);
	lockstats.locks++;

	x = splhi();
	if(tas(&l->key) != 0){
		lockstats.glare++;
		/*
		 * Cannot also check l->pc, l->m, or l->isilock here
		 * because they might just not be set yet, or
		 * (for pc and m) the lock might have just been unlocked.
		 */
		for(;;){
			lockstats.inglare++;
			splx(x);
			while(l->key)
				;
			x = splhi();
			if(tas(&l->key) == 0)
				goto acquire;
		}
	}
acquire:
	m->ilockdepth++;
	if(up)
		up->lastilock = l;
	l->sr = x;
	l->pc = pc;
	l->p = up;
	l->isilock = 1;
	l->m = MACHP(m->machno);
#ifdef LOCKCYCLES
	l->lockcycles = -lcycles();
#endif
}

int
canlock(Lock *l)
{
	if(up)
		up->nlocks++;
	if(tas(&l->key)){
		if(up)
			up->nlocks--;
		return 0;
	}

	if(up)
		up->lastlock = l;
	l->pc = getcallerpc(&l);
	l->p = up;
	l->m = MACHP(m->machno);
	l->isilock = 0;
#ifdef LOCKCYCLES
	l->lockcycles = -lcycles();
#endif
	return 1;
}

void
unlock(Lock *l)
{
#ifdef LOCKCYCLES
	l->lockcycles += lcycles();
	cumlockcycles += l->lockcycles;
	if(l->lockcycles > maxlockcycles){
		maxlockcycles = l->lockcycles;
		maxlockpc = l->pc;
	}
#endif
	if(l->key == 0)
		print("unlock: not locked: pc %#p\n", getcallerpc(&l));
	if(l->isilock)
		print("unlock of ilock: pc %#p, held by %#p\n", getcallerpc(&l), l->pc);
	if(l->p != up)
		print("unlock: up changed: pc %#p, acquired at pc %#p, lock p %#p, unlock up %#p\n", getcallerpc(&l), l->pc, l->p, up);
	l->m = nil;
	l->key = 0;
	coherence();

	if(up && --up->nlocks == 0 && up->delaysched && islo()){
		/*
		 * Call sched if the need arose while locks were held
		 * But, don't do it from interrupt routines, hence the islo() test
		 */
		sched();
	}
}

#ifdef LOCKCYCLES
uintptr ilockpcs[0x100] = { [0xff] = 1 };
static int n;
#endif

void
iunlock(Lock *l)
{
	ulong sr;

#ifdef LOCKCYCLES
	l->lockcycles += lcycles();
	cumilockcycles += l->lockcycles;
	if(l->lockcycles > maxilockcycles){
		maxilockcycles = l->lockcycles;
		maxilockpc = l->pc;
	}
	if(l->lockcycles > 2400)
		ilockpcs[n++ & 0xff]  = l->pc;
#endif
	if(l->key == 0)
		print("iunlock: not locked: pc %#p\n", getcallerpc(&l));
	if(!l->isilock)
		print("iunlock of lock: pc %#p, held by %#p\n", getcallerpc(&l), l->pc);
	if(islo())
		print("iunlock while lo: pc %#p, held by %#p\n", getcallerpc(&l), l->pc);
	if(l->m != MACHP(m->machno)){
		iprint("iunlock by mach%d, locked by mach%d: pc %#p, held by %#p\n",
			m->machno, l->m->machno, getcallerpc(&l), l->pc);
	}

	sr = l->sr;
	l->m = nil;
	l->key = 0;
	coherence();
	m->ilockdepth--;
	if(up)
		up->lastilock = nil;
	splx(sr);
}

int
ownlock(Lock *l)
{
	return l->m == MACHP(m->machno);
}

uintptr
lockgetpc(Lock *l)
{
	if(l != nil)
		return l->pc;
	return 0;
}

void
locksetpc(Lock *l, uintptr pc)
{
	l->pc = pc;
}
