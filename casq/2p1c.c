#define THREEINCH
#include "stuff.h"
#include <stdio.h>

#define NENTS 20
#define QLIMIT 256*1024
#define ENTSIZE 64

long sleepsem = 0;
Proc *up;

void main(void)
{
	int pid, val, i, sleep_i;
	char c;
	char src[ENTSIZE];
	char dest[ENTSIZE];
	Block *b; 
//	print("2p1c starting\n");
	CasQueue *q = casqopen(QLIMIT);

	switch(pid = rfork(RFPROC|RFMEM)){
	case -1: /* fork failed */
		fprintf(stderr, "ABORT!\n");
		abort();
	case 0: /* child: consumer */
		for(i = 0; i < NENTS * 2; i++){
			b = casqget(q);
			if (b == nil)
				i--;
//			else
//				fprintf(stderr, "dest: %s", b->rp);
		}
//		fprintf(stderr, "consumer cputime: %f\n", cputime());
		break;
	default: /* parent: producer */
		switch(pid = rfork(RFPROC|RFMEM)){
		case -1: /* fork failed */
			fprintf(stderr, "ABORT!\n");
			abort();
		case 0: /* child: producer */
			for(i = 0; i < NENTS; i++){
				snprint(src, 40, "P1: this is the contents of block %d\n", i);
				b = allocb(ENTSIZE);
				memmove(b->wp, src, ENTSIZE);
				b->wp += ENTSIZE;
				casqput(q, b);
			}
//			fprintf(stderr, "P1 cputime: %f\n", cputime());
			break;
		default: /* parent: producer */
			for(i = 0; i < NENTS; i++){
				snprint(src, 40, "P2: this is the contents of block %d\n", i);
				b = allocb(ENTSIZE);
				memmove(b->wp, src, ENTSIZE);
				b->wp += ENTSIZE;
				casqput(q, b);
			}
//			fprintf(stderr, "P2 cputime before wait: %f\n", cputime());
			waitpid();
			waitpid();
//			fprintf(stderr, "P2 cputime after wait: %f\n", cputime());
//			print("2p1c complete\n");
		}
	}
}

