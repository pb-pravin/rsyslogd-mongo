/* threads.c
 *
 * This file implements threading support helpers (and maybe the thread object)
 * for rsyslog.
 * 
 * File begun on 2007-12-14 by RGerhards
 *
 * Copyright 2007, 2009 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>

#include "rsyslog.h"
#include "dirty.h"
#include "linkedlist.h"
#include "threads.h"
#include "srUtils.h"

/* linked list of currently-known threads */
static linkedList_t llThrds;

/* methods */

/* Construct a new thread object
 */
static rsRetVal
thrdConstruct(thrdInfo_t **ppThis)
{
	DEFiRet;
	thrdInfo_t *pThis;

	assert(ppThis != NULL);

	CHKmalloc(pThis = calloc(1, sizeof(thrdInfo_t)));
	pthread_mutex_init(&pThis->mutThrd, NULL);
	pthread_cond_init(&pThis->condThrdTerm, NULL);
	*ppThis = pThis;

finalize_it:
	RETiRet;
}


/* Destructs a thread object. The object must not be linked to the
 * linked list of threads. Please note that the thread should have been
 * stopped before. If not, we try to do it.
 */
static rsRetVal thrdDestruct(thrdInfo_t *pThis)
{
	DEFiRet;
	assert(pThis != NULL);

	if(pThis->bIsActive == 1) {
		thrdTerminate(pThis);
	}
	pthread_mutex_destroy(&pThis->mutThrd);
	pthread_cond_destroy(&pThis->condThrdTerm);
	free(pThis);

	RETiRet;
}


/* terminate a thread via the non-cancel interface
 * This is a separate function as it involves a bit more of code.
 * rgerhads, 2009-10-15
 */
static inline rsRetVal
thrdTerminateNonCancel(thrdInfo_t *pThis)
{
	struct timespec tTimeout;
	int ret;
	DEFiRet;
	assert(pThis != NULL);

	DBGPRINTF("request term via SIGTTIN for input thread 0x%x\n", (unsigned) pThis->thrdID);
	pThis->bShallStop = TRUE;
	do {
		d_pthread_mutex_lock(&pThis->mutThrd);
		pthread_kill(pThis->thrdID, SIGTTIN);
		timeoutComp(&tTimeout, 10); /* a fixed 10ms timeout, do after lock (may take long!) */
		ret = d_pthread_cond_timedwait(&pThis->condThrdTerm, &pThis->mutThrd, &tTimeout);
		d_pthread_mutex_unlock(&pThis->mutThrd);
		if(Debug) {
			if(ret == ETIMEDOUT) {
				dbgprintf("input thread term: had a timeout waiting on thread termination\n");
			} else if(ret == 0) {
				dbgprintf("input thread term: thread returned normally and is terminated\n");
			} else {
				char errStr[1024];
				int err = errno;
				rs_strerror_r(err, errStr, sizeof(errStr));
				dbgprintf("input thread term: cond_wait returned with error %d: %s\n",
					  err, errStr);
			}
		}
	} while(pThis->bIsActive);
	DBGPRINTF("non-cancel input thread termination succeeded for thread 0x%x\n", (unsigned) pThis->thrdID);

	RETiRet;
}


/* terminate a thread gracefully.
 */
rsRetVal thrdTerminate(thrdInfo_t *pThis)
{
	DEFiRet;
	assert(pThis != NULL);
	
	if(pThis->bNeedsCancel) {
		DBGPRINTF("request term via canceling for input thread 0x%x\n", (unsigned) pThis->thrdID);
		pthread_cancel(pThis->thrdID);
		pThis->bIsActive = 0;
	} else {
		thrdTerminateNonCancel(pThis);
	}
	pthread_join(pThis->thrdID, NULL); /* wait for input thread to complete */

	/* call cleanup function, if any */
	if(pThis->pAfterRun != NULL)
		pThis->pAfterRun(pThis);
	
	RETiRet;
}


/* terminate all known threads gracefully.
 */
rsRetVal thrdTerminateAll(void)
{
	DEFiRet;
	llDestroy(&llThrds);
	RETiRet;
}


/* This is an internal wrapper around the user thread function. Its
 * purpose is to handle all the necessary housekeeping stuff so that the
 * user function needs not to be aware of the threading calls. The user
 * function call has just "normal", non-threading semantics.
 * rgerhards, 2007-12-17
 */
static void* thrdStarter(void *arg)
{
	DEFiRet;
	thrdInfo_t *pThis = (thrdInfo_t*) arg;

	assert(pThis != NULL);
	assert(pThis->pUsrThrdMain != NULL);

	/* block all signals */
	sigset_t sigSet;
	sigfillset(&sigSet);
	pthread_sigmask(SIG_BLOCK, &sigSet, NULL);

	/* but ignore SIGTTN, which we (ab)use to signal the thread to shutdown -- rgerhards, 2009-07-20 */
	sigemptyset(&sigSet);
	sigaddset(&sigSet, SIGTTIN);
	pthread_sigmask(SIG_UNBLOCK, &sigSet, NULL);

	/* setup complete, we are now ready to execute the user code. We will not
	 * regain control until the user code is finished, in which case we terminate
	 * the thread.
	 */
	iRet = pThis->pUsrThrdMain(pThis);

	dbgprintf("thrdStarter: usrThrdMain 0x%lx returned with iRet %d, exiting now.\n", (unsigned long) pThis->thrdID, iRet);

	/* signal master control that we exit (we do the mutex lock mostly to 
	 * keep the thread debugger happer, it would not really be necessary with
	 * the logic we employ...)
	 */
	pThis->bIsActive = 0;
	d_pthread_mutex_lock(&pThis->mutThrd);
	pthread_cond_signal(&pThis->condThrdTerm);
	d_pthread_mutex_unlock(&pThis->mutThrd);

	ENDfunc
	pthread_exit(0);
}

/* Start a new thread and add it to the list of currently
 * executing threads. It is added at the end of the list.
 * rgerhards, 2007-12-14
 */
rsRetVal thrdCreate(rsRetVal (*thrdMain)(thrdInfo_t*), rsRetVal(*afterRun)(thrdInfo_t *), sbool bNeedsCancel)
{
	DEFiRet;
	thrdInfo_t *pThis;
	int i;

	assert(thrdMain != NULL);

	CHKiRet(thrdConstruct(&pThis));
	pThis->bIsActive = 1;
	pThis->pUsrThrdMain = thrdMain;
	pThis->pAfterRun = afterRun;
	pThis->bNeedsCancel = bNeedsCancel;
	i = pthread_create(&pThis->thrdID, NULL, thrdStarter, pThis);
	CHKiRet(llAppend(&llThrds, NULL, pThis));

finalize_it:
	RETiRet;
}


/* initialize the thread-support subsystem
 * must be called once at the start of the program
 */
rsRetVal thrdInit(void)
{
	DEFiRet;
	iRet = llInit(&llThrds, thrdDestruct, NULL, NULL);
	RETiRet;
}


/* de-initialize the thread subsystem
 * must be called once at the end of the program
 */
rsRetVal thrdExit(void)
{
	DEFiRet;
	iRet = llDestroy(&llThrds);
	RETiRet;
}


/* vi:set ai:
 */
