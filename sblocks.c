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
#define MAX(X,Y)   ((X) > (Y) ? (X) : (Y))
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
#define setSigHandler(s, h)\
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
	int toJoin;
	int idx;
} Thrd;
#endif /* THR */

/* function declarations */
static void blksToStext(void);
static void closeDpy(void);
static void onQuit(int s);
static void openDpy(void);
static void (*printStext)(void);
static void printStdout(void);
static void run(void);
static void setRoot(void);
static void sigHan(int s);
static void sigSetup(void);
static void Sleep();
static void tsDiff(struct timespec *res, const struct timespec *a, const struct timespec *b);
static int updateAll(int t);
static void updateBlk(int i);
#ifdef THR
static void asyncUpdateBlk(int i);
static void initThrds(void);
static void syncUpdatedBlks(void);
static void *thrdUpdateBlk(void *arg);
#endif /* THR */

/* include configuration file before variable declerations */
#include "config.h"

/* variables */
char *argv0;
static char blkStr[BLKN][BLKLEN];
static char sText[STSLEN];
static int loops;
static int restart = 0;
static int running = 1;
static int lastSignal = 0;
static struct timespec nextTs[1] = { { 0, 0 } };
static struct timespec currTs[1] = { { 0, 0 } };
static struct timespec sleepTs[1] = { { 0, 0 } };
/* X11 specific */
static Display *dpy;
static Window root;
#ifdef THR
static Thrd thrds[BLKN];
#endif /* THR */

/* function implementations */
void
blksToStext(void)
{
	int i, e;

	for (i = e = 0; i < BLKN; i++) {
		e += sprintf(sText + e, "%s%s%s",
		     blks[i].strBefore, blkStr[i], blks[i].strAfter);
	}
}

void
closeDpy(void)
{
	XCloseDisplay(dpy);
}

void
onQuit(int s)
{
	running = 0;
	if (s == SIGHUP)
		restart = 1;
}

void
openDpy(void)
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
	if (printStext == setRoot)
		openDpy();

	while (running) {
		clock_gettime(CLOCK, currTs);
		nextTs->tv_sec = currTs->tv_sec + SEC;
		nextTs->tv_nsec = currTs->tv_nsec + NSEC;

		if (updateAll(loops++)) {
			blksToStext();
			printStext();
		}
		Sleep();
	}

	if (printStext == setRoot)
		closeDpy();
}

void
setRoot(void)
{
	XStoreName(dpy, root, sText);
	XFlush(dpy);
}

void
sigHan(int s)
{
	lastSignal = s;
}

void
sigSetup(void)
{
	int i, s;

	setSigHandler(SIGINT, onQuit);
	setSigHandler(SIGTERM, onQuit);
	setSigHandler(SIGHUP, onQuit);

	for (i = 0; i < BLKN; i++) {
		if ((s = blks[i].sig))
			setSigHandler(s, sigHan);
	}
}

void
Sleep(void)
{
	int i;

	if (!running)
		return;
	if (lastSignal) {
		for (i = 0; i < BLKN; i++) {
			if (blks[i].sig == lastSignal)
#ifdef THR
				asyncUpdateBlk(i);
#else
				updateBlk(i);
#endif /* THR */
		}
#ifdef THR
		syncUpdatedBlks();
#endif /* THR */
		blksToStext();
		printStext();
		lastSignal = 0;
	}

	clock_gettime(CLOCK, currTs);
	tsDiff(sleepTs, nextTs, currTs);
	debugts(sleepTs);
	if (nanosleep(sleepTs, NULL))
		Sleep();
}

void
printStdout(void)
{
	puts(sText);
	fflush(stdout);
}

void
tsDiff(struct timespec *res, const struct timespec *a, const struct timespec *b)
{
	res->tv_sec = a->tv_sec - b->tv_sec - (a->tv_nsec < b->tv_nsec);
	res->tv_nsec = a->tv_nsec - b->tv_nsec +
	               (a->tv_nsec < b->tv_nsec) * 1E9;
	/* needed after continiously signaling */
	res->tv_nsec = MAX(res->tv_nsec, MIN_NSEC);
	res->tv_sec = MAX(res->tv_sec, 0);
}

int
updateAll(int t)
{
	int i, per, u;

	for (i = u = 0; i < BLKN; i++) {
		per = blks[i].period;
		if ((per != 0 && t % per == 0) || t == 0) {
#ifdef THR
			asyncUpdateBlk(i);
#else
			updateBlk(i);
#endif /* THR */
			u = 1;
		}
	}
#ifdef THR
	syncUpdatedBlks();
#endif /* THR */

	return u;
}

#ifdef THR
void
asyncUpdateBlk(int i)
{
	pthread_create(&thrds[i].t, NULL, thrdUpdateBlk, &thrds[i].idx);
	thrds[i].toJoin = 1;
}

void
initThrds(void)
{
	int i;
	for (i = 0; i < BLKN; i++)
		thrds[i].idx = i;
}

void
syncUpdatedBlks(void)
{
	int i;
	for (i = 0; i < BLKN; i++) {
		if (thrds[i].toJoin) {
			pthread_join(thrds[i].t, NULL);
			thrds[i].toJoin = 0;
		}
	}
}

void *
thrdUpdateBlk(void *arg)
{
	int i = *(int*)arg;

	debugf("in: blk[%d]\n", i);
	updateBlk(i);
	debugf("out: blk[%d]\n", i);

	return NULL;
}
#endif /* THR */

void
updateBlk(int i)
{
	FILE *cmdout;

	cmdout = popen(blks[i].cmd, "r");
	if (!fgets(blkStr[i], BLKLEN, cmdout) && !lastSignal)
		blkStr[i][0] = '\0';
	pclose(cmdout);
}

int
main(int argc, char *argv[])
{
	char **argvsave = argv;

	printStext = setRoot;

	ARGBEGIN {
	case 'o':
		printStext = printStdout;
		break;
	default:
		eprintf("usage: %s [-o]\n", argv0);
		exit(1);
	} ARGEND

#ifdef THR
	initThrds();
#endif /* THR */
	sigSetup();
	run();

	if (restart)
		execvp(argv0, argvsave);

	return 0;
}
