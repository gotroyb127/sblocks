#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <pthread.h>

#include "arg.h"

/* macros */
#define BLKLEN     256
#define BLKN       LENGTH(blks)
#define CLOCK      CLOCK_MONOTONIC
#define LENGTH(X)  (sizeof X / sizeof X[0])
#define MAX(X,Y)   (X > Y ? (X) : (Y))
#define SEC        1
#define NSEC       0
#define MIN_NSEC   1E6
#define STSLEN     256 /* dwm uses 256 */

#ifdef DEBUG
#define debugf(...) eprintf(__VA_ARGS__)
#else
#define debugf(...)
#endif /* DEBUG */
#define debugts(t)  debugf("%s: %ld.%09ld\n", #t, t->tv_sec, t->tv_nsec);
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define esigaction(sig, act, oldact)\
	do {\
		if (sigaction(sig, act, oldact))\
			eprintf("%s[%d]: sigaction: '%s' on signal %d\n",\
			        argv0, __LINE__, strerror(errno), sig);\
	} while (0)
#define setsighandler(s, h)\
	esigaction(s, &(struct sigaction){.sa_handler = h}, NULL)

typedef struct {
	char *strBefore;
	char *cmd;
	char *strAfter;
	unsigned int period;
	unsigned int sig;
} Blk;

#ifdef THR
typedef struct {
	pthread_t t;
	int tojoin;
	int idx;
} Thrd;
#endif /* THR */

/* function declarations */
static void blkstostext(void);
static void closedpy(void);
static void onquit(int s);
static void opendpy(void);
static void (*printstext)(void);
static void printstdout(void);
static void run(void);
static void setroot(void);
static void sighan(int s);
static void sigsetup(void);
static void xsleep();
static void tsdiff(struct timespec *res, const struct timespec *a, const struct timespec *b);
static int updateall(int t);
static void updateblk(int i);
#ifdef THR
static void asyncupdateblk(int i);
static void initthrds(void);
static void syncupdatedblks(void);
static void *thrdupdateblk(void *arg);
#endif /* THR */

/* include configuration file before variable declerations */
#include "config.h"

/* variables */
char *argv0;
static char blkstr[BLKN][BLKLEN];
static char stext[STSLEN];
static int loops;
static int restart = 0;
static int running = 1;
static int lastsignal = 0;
static struct timespec nextts[1] = { { 0, 0 } };
static struct timespec currts[1] = { { 0, 0 } };
static struct timespec sleepts[1] = { { 0, 0 } };
/* X11 specific */
static Display *dpy;
static Window root;
#ifdef THR
static Thrd thrds[BLKN];
#endif /* THR */

/* function implementations */
void
blkstostext(void)
{
	int i, e;

	for (i = e = 0; i < BLKN; i++) {
		e += sprintf(stext + e, "%s%s%s",
		     blks[i].strBefore, blkstr[i], blks[i].strAfter);
	}
}

void
closedpy(void)
{
	XCloseDisplay(dpy);
}

void
onquit(int s)
{
	running = 0;
	if (s == SIGHUP)
		restart = 1;
}

void
opendpy(void)
{
	if (!(dpy = XOpenDisplay(NULL))) {
		eprintf("%s: cannot open display\n", argv0);
		exit(1);
	}

	root = RootWindow(dpy, DefaultScreen(dpy));
}

void
run(void)
{
	if (printstext == setroot)
		opendpy();

	while (running) {
		clock_gettime(CLOCK, currts);
		nextts->tv_sec = currts->tv_sec + SEC;
		nextts->tv_nsec = currts->tv_nsec + NSEC;
		if (updateall(loops++)) {
			blkstostext();
			printstext();
		}
		xsleep();
	}

	if (printstext == setroot)
		closedpy();
}

void
setroot(void)
{
	XStoreName(dpy, root, stext);
	XFlush(dpy);
}

void
sighan(int s)
{
	lastsignal = s;
}

void
sigsetup(void)
{
	int i, s;

	setsighandler(SIGINT, onquit);
	setsighandler(SIGTERM, onquit);
	setsighandler(SIGHUP, onquit);

	for (i = 0; i < BLKN; i++) {
		if ((s = blks[i].sig))
			setsighandler(s, sighan);
	}
}

void
xsleep(void)
{
	int i;

	if (!running)
		return;
	if (lastsignal) {
		for (i = 0; i < BLKN; i++) {
			if (blks[i].sig == lastsignal)
#ifdef THR
				asyncupdateblk(i);
#else
				updateblk(i);
#endif /* THR */
		}
#ifdef THR
		syncupdatedblks();
#endif /* THR */
		blkstostext();
		printstext();
		lastsignal = 0;
	}

	clock_gettime(CLOCK, currts);
	tsdiff(sleepts, nextts, currts);
	debugts(sleepts);
	if (nanosleep(sleepts, NULL))
		xsleep();
}

void
printstdout(void)
{
	puts(stext);
	fflush(stdout);
}

void
tsdiff(struct timespec *res, const struct timespec *a, const struct timespec *b)
{
	res->tv_sec = a->tv_sec - b->tv_sec - (a->tv_nsec < b->tv_nsec);
	res->tv_nsec = a->tv_nsec - b->tv_nsec +
	               (a->tv_nsec < b->tv_nsec) * 1E9;
	/* needed after continiously signaling */
	res->tv_nsec = MAX(res->tv_nsec, MIN_NSEC);
	res->tv_sec = MAX(res->tv_sec, 0);
}

int
updateall(int t)
{
	int i, per, u;

	for (i = u = 0; i < BLKN; i++) {
		per = blks[i].period;
		if ((per != 0 && t % per == 0) || t == 0) {
#ifdef THR
			asyncupdateblk(i);
#else
			updateblk(i);
#endif /* THR */
			u = 1;
		}
	}
#ifdef THR
	syncupdatedblks();
#endif /* THR */

	return u;
}

#ifdef THR
void
asyncupdateblk(int i)
{
	pthread_create(&thrds[i].t, NULL, thrdupdateblk, &thrds[i].idx);
	thrds[i].tojoin = 1;
}

void
initthrds(void)
{
	int i;
	for (i = 0; i < BLKN; i++)
		thrds[i].idx = i;
}

void
syncupdatedblks(void)
{
	int i;
	for (i = 0; i < BLKN; i++) {
		if (thrds[i].tojoin) {
			pthread_join(thrds[i].t, NULL);
			thrds[i].tojoin = 0;
		}
	}
}

void *
thrdupdateblk(void *arg)
{
	int i = *(int*)arg;

	debugf("in: blk[%d]\n", i);
	updateblk(i);
	debugf("out: blk[%d]\n", i);

	return NULL;
}
#endif /* THR */

void
updateblk(int i)
{
	FILE *cmdout;

	cmdout = popen(blks[i].cmd, "r");
	if (!fgets(blkstr[i], BLKLEN, cmdout) && !lastsignal)
		blkstr[i][0] = '\0';
	pclose(cmdout);
}

int
main(int argc, char *argv[])
{
	char **argvsave = argv;

	printstext = setroot;

	ARGBEGIN {
	case 'o':
		printstext = printstdout;
		break;
	default:
		eprintf("usage: %s [-o]\n", argv0);
		exit(1);
	} ARGEND

#ifdef THR
	initthrds();
#endif /* THR */
	sigsetup();
	run();

	if (restart)
		execvp(argv0, argvsave);

	return 0;
}
