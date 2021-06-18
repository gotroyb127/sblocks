#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/select.h>
#include <X11/Xlib.h>

#include "arg.h"

/* macros */
#define BLKLEN     256
#define BLKN       LENGTH(blks)
#define CLOCK      CLOCK_MONOTONIC
#define LENGTH(X)  (sizeof X / sizeof X[0])
#define MAX(X,Y)   ((X) > (Y) ? (X) : (Y))
#define STSLEN     256 /* dwm uses 256 */

#ifdef DEBUG
#define debugf(...) eprintf(__VA_ARGS__)
#else
#define debugf(...)
#endif /* DEBUG */

#define debugTs(t)  debugf("%s: %ld.%09ld\n", #t, t->tv_sec, t->tv_nsec);
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
	char *fmt;
	char *cmd;
	unsigned int period;
	unsigned int sig;
	struct timespec ts;
} Blk;

#ifdef THR
typedef struct {
	pthread_t t;
	int toJoin;
	int idx;
	int pfd[2]; /* pipe's fd */
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
static void _sleep();
static void tsDiff(struct timespec *res, const struct timespec *a, const struct timespec *b);
static int updateAll(int t);
static void updateBlk(int i);

#ifdef THR
static void asyncUpdateBlk(int i);
static void initThrds(void);
static int syncUpdatedBlks(void);
static void *thrdUpdateBlk(void *arg);
#endif /* THR */

/* include configuration file before variable declerations */
#include "config.h"

/* variables */
char *argv0;
static char blkStr[BLKN][BLKLEN];
static char sText[STSLEN];
static int cycles;
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
	int i;
	char *s, *e;

	s = sText;
	e = sText + sizeof sText;
	for (i = 0; i < BLKN && s < e; i++)
		s += snprintf(s, e - s, blks[i].fmt, blkStr[i]);
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

	clock_gettime(CLOCK, nextTs);
	while (running) {
		nextTs->tv_sec += SEC;
		nextTs->tv_nsec += NSEC;

		if (updateAll(cycles++)) {
			blksToStext();
			printStext();
		}
		_sleep();
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
_sleep(void)
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
	/* happens when signaled continiously */
	while (sleepTs->tv_sec < 0) {
		nextTs->tv_sec += 1;
		sleepTs->tv_sec += 1;
	}
	debugTs(currTs);
	debugTs(sleepTs);
	if (nanosleep(sleepTs, NULL))
		_sleep();
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
	return syncUpdatedBlks();
#endif /* THR */
	return u;
}

#ifdef THR
void
asyncUpdateBlk(int i)
{
	Thrd *th;

	/* a previous command is still running */
	if ((th = &thrds[i])->toJoin)
		return;

	pipe(th->pfd);
	pthread_create(&th->t, NULL, thrdUpdateBlk, &th->idx);
	th->toJoin = 1;
}

void
initThrds(void)
{
	int i;
	for (i = 0; i < BLKN; i++)
		thrds[i].idx = i;
}

int
syncUpdatedBlks(void)
{
	int i, n, u;
	Thrd *th;
	fd_set rfds;

	for (i = 0; i < BLKN; i++) {
		if (!(th = &thrds[i])->toJoin)
			continue;

		FD_ZERO(&rfds);
		FD_SET(th->pfd[0], &rfds);

		n = pselect(th->pfd[0] + 1, &rfds, NULL, NULL,
		            &blks[i].ts, NULL);
		if (n == 0) {
			debugf("blk[%d] hasn't completed.\n", i);
			continue;
		} else if (n > 0)
			close(th->pfd[0]);
		else
			debugf("pselect: %s\n", strerror(errno));

		pthread_join(th->t, NULL);
		th->toJoin = 0;
		u = 1;
	}
	return u;
}

void *
thrdUpdateBlk(void *arg)
{
	int i = *(int*)arg;

	debugf("in: blk[%d]\n", i);
	updateBlk(i);
	debugf("out: blk[%d]\n", i);

	write(thrds[i].pfd[1], "\n", 1);
	close(thrds[i].pfd[1]);

	return NULL;
}
#endif /* THR */

void
updateBlk(int i)
{
	FILE *cmdout;

	cmdout = popen(blks[i].cmd, "r");
	if (!fgets(blkStr[i], BLKLEN, cmdout))
		blkStr[i][0] = '\0';
	pclose(cmdout);
}

int
main(int argc, char *_argv[])
{
	char **argv = _argv;

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
		execvp(argv0, _argv);

	return 0;
}
