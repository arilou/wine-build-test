/*
 * MIDI driver for OSS (PE-side)
 *
 * Copyright 1994       Martin Ayotte
 * Copyright 1998       Luiz Otavio L. Zorzella (init procedures)
 * Copyright 1998, 1999 Eric POUECH
 * Copyright 2022       Huw Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* TODO:
 *    + use better instrument definition for OPL/2 (midiPatch.c) or
 *      use existing instrument definition (from playmidi or kmid)
 *      with a .winerc option 
 *    + have a look at OPL/3 ?
 *    + implement asynchronous playback of MidiHdr
 *    + implement STREAM'ed MidiHdr (question: how shall we share the
 *      code between the midiStream functions in MMSYSTEM/WINMM and
 *      the code for the low level driver)
 *    + use a more accurate read mechanism than the one of snooping on
 *      timers (like select on fd)
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/soundcard.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "mmddk.h"
#include "audioclient.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(midi);

static WINE_MIDIIN *MidiInDev;

/* this is the total number of MIDI out devices found */
static	int 		MIDM_NumDevs = 0;

static	int		numStartedMidiIn = 0;

static int rec_cancel_pipe[2];
static HANDLE hThread;

/*======================================================================*
 *                  Low level MIDI implementation			*
 *======================================================================*/

static int midiOpenSeq(void);
static int midiCloseSeq(int);

static int MIDI_loadcount;
/**************************************************************************
 * 			OSS_MidiInit				[internal]
 *
 * Initializes the MIDI devices information variables
 */
static LRESULT OSS_MidiInit(void)
{
    struct midi_init_params params;
    UINT err;

    TRACE("(%i)\n", MIDI_loadcount);
    if (MIDI_loadcount++)
        return 1;

    TRACE("Initializing the MIDI variables.\n");

    params.err = &err;
    OSS_CALL(midi_init, &params);

    if (!err)
    {
        MidiInDev = params.srcs;
        MIDM_NumDevs = params.num_srcs;
    }
    return err;
}

/**************************************************************************
 * 			OSS_MidiExit				[internal]
 *
 * Release the MIDI devices information variables
 */
static LRESULT OSS_MidiExit(void)
{
    TRACE("(%i)\n", MIDI_loadcount);

    if (--MIDI_loadcount)
        return 1;

    MidiInDev = NULL;
    MIDM_NumDevs = 0;

    return 0;
}

static void notify_client(struct notify_context *notify)
{
    TRACE("dev_id = %d msg = %d param1 = %04lX param2 = %04lX\n",
          notify->dev_id, notify->msg, notify->param_1, notify->param_2);

    DriverCallback(notify->callback, notify->flags, notify->device, notify->msg,
                   notify->instance, notify->param_1, notify->param_2);
}

/**************************************************************************
 * 			MIDI_NotifyClient			[internal]
 */
static void MIDI_NotifyClient(UINT wDevID, WORD wMsg,
			      DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    DWORD_PTR 		dwCallBack;
    UINT 		uFlags;
    HANDLE		hDev;
    DWORD_PTR 		dwInstance;

    TRACE("wDevID = %04X wMsg = %d dwParm1 = %04lX dwParam2 = %04lX\n",
	  wDevID, wMsg, dwParam1, dwParam2);

    switch (wMsg) {
    case MIM_OPEN:
    case MIM_CLOSE:
    case MIM_DATA:
    case MIM_LONGDATA:
    case MIM_ERROR:
    case MIM_LONGERROR:
    case MIM_MOREDATA:
	if (wDevID > MIDM_NumDevs) return;

	dwCallBack = MidiInDev[wDevID].midiDesc.dwCallback;
	uFlags = MidiInDev[wDevID].wFlags;
	hDev = MidiInDev[wDevID].midiDesc.hMidi;
	dwInstance = MidiInDev[wDevID].midiDesc.dwInstance;
	break;
    default:
	ERR("Unsupported MSW-MIDI message %u\n", wMsg);
	return;
    }

    DriverCallback(dwCallBack, uFlags, hDev, wMsg, dwInstance, dwParam1, dwParam2);
}

/**************************************************************************
 * 			midiOpenSeq				[internal]
 */
static int midiOpenSeq(void)
{
    struct midi_seq_open_params params;

    params.close = 0;
    params.fd = -1;
    OSS_CALL(midi_seq_open, &params);

    return params.fd;
}

/**************************************************************************
 * 			midiCloseSeq				[internal]
 */
static int midiCloseSeq(int fd)
{
    struct midi_seq_open_params params;

    params.close = 1;
    params.fd = fd;
    OSS_CALL(midi_seq_open, &params);

    return 0;
}

static void handle_midi_data(unsigned char *buffer, unsigned int len)
{
    struct midi_handle_data_params params;

    params.buffer = buffer;
    params.len = len;
    OSS_CALL(midi_handle_data, &params);
}

static DWORD WINAPI midRecThread(void *arg)
{
    int fd = (int)(INT_PTR)arg;
    unsigned char buffer[256];
    int len;
    struct pollfd pollfd[2];

    pollfd[0].fd = rec_cancel_pipe[0];
    pollfd[0].events = POLLIN;
    pollfd[1].fd = fd;
    pollfd[1].events = POLLIN;

    while (1)
    {
	/* Check if an event is present */
	if (poll(pollfd, ARRAY_SIZE(pollfd), -1) <= 0)
	    continue;

	if (pollfd[0].revents & POLLIN) /* cancelled */
	    break;

	len = read(fd, buffer, sizeof(buffer));

	if (len > 0 && len % 4 == 0)
	    handle_midi_data(buffer, len);
    }
    return 0;
}

/**************************************************************************
 * 			midOpen					[internal]
 */
static DWORD midOpen(WORD wDevID, LPMIDIOPENDESC lpDesc, DWORD dwFlags)
{
    int fd;

    TRACE("(%04X, %p, %08X);\n", wDevID, lpDesc, dwFlags);

    if (lpDesc == NULL) {
	WARN("Invalid Parameter !\n");
	return MMSYSERR_INVALPARAM;
    }

    /* FIXME :
     *	how to check that content of lpDesc is correct ?
     */
    if (wDevID >= MIDM_NumDevs) {
	WARN("wDevID too large (%u) !\n", wDevID);
	return MMSYSERR_BADDEVICEID;
    }
    if (MidiInDev[wDevID].state == -1) {        
        WARN("device disabled\n");
        return MIDIERR_NODEVICE;
    }
    if (MidiInDev[wDevID].midiDesc.hMidi != 0) {
	WARN("device already open !\n");
	return MMSYSERR_ALLOCATED;
    }
    if ((dwFlags & MIDI_IO_STATUS) != 0) {
	WARN("No support for MIDI_IO_STATUS in dwFlags yet, ignoring it\n");
	dwFlags &= ~MIDI_IO_STATUS;
    }
    if ((dwFlags & ~CALLBACK_TYPEMASK) != 0) {
	FIXME("Bad dwFlags\n");
	return MMSYSERR_INVALFLAG;
    }

    fd = midiOpenSeq();
    if (fd < 0) {
	return MMSYSERR_ERROR;
    }

    if (numStartedMidiIn++ == 0) {
	pipe(rec_cancel_pipe);
	hThread = CreateThread(NULL, 0, midRecThread, (void *)(INT_PTR)fd, 0, NULL);
	if (!hThread) {
	    close(rec_cancel_pipe[0]);
	    close(rec_cancel_pipe[1]);
	    numStartedMidiIn = 0;
	    WARN("Couldn't create thread for midi-in\n");
	    midiCloseSeq(fd);
	    return MMSYSERR_ERROR;
	}
        SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);
	TRACE("Created thread for midi-in\n");
    }

    MidiInDev[wDevID].wFlags = HIWORD(dwFlags & CALLBACK_TYPEMASK);

    MidiInDev[wDevID].lpQueueHdr = NULL;
    MidiInDev[wDevID].midiDesc = *lpDesc;
    MidiInDev[wDevID].state = 0;
    MidiInDev[wDevID].incLen = 0;
    MidiInDev[wDevID].startTime = 0;
    MidiInDev[wDevID].fd = fd;

    MIDI_NotifyClient(wDevID, MIM_OPEN, 0L, 0L);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 			midClose				[internal]
 */
static DWORD midClose(WORD wDevID)
{
    int		ret = MMSYSERR_NOERROR;

    TRACE("(%04X);\n", wDevID);

    if (wDevID >= MIDM_NumDevs) {
	WARN("wDevID too big (%u) !\n", wDevID);
	return MMSYSERR_BADDEVICEID;
    }
    if (MidiInDev[wDevID].midiDesc.hMidi == 0) {
	WARN("device not opened !\n");
	return MMSYSERR_ERROR;
    }
    if (MidiInDev[wDevID].lpQueueHdr != 0) {
	return MIDIERR_STILLPLAYING;
    }

    if (MidiInDev[wDevID].fd == -1) {
	WARN("ooops !\n");
	return MMSYSERR_ERROR;
    }
    if (--numStartedMidiIn == 0) {
	TRACE("Stopping thread for midi-in\n");
	write(rec_cancel_pipe[1], "x", 1);
	if (WaitForSingleObject(hThread, 5000) != WAIT_OBJECT_0) {
	    WARN("Thread end not signaled, force termination\n");
	    TerminateThread(hThread, 0);
	}
	close(rec_cancel_pipe[0]);
	close(rec_cancel_pipe[1]);
    	TRACE("Stopped thread for midi-in\n");
    }
    midiCloseSeq(MidiInDev[wDevID].fd);
    MidiInDev[wDevID].fd = -1;

    MIDI_NotifyClient(wDevID, MIM_CLOSE, 0L, 0L);
    MidiInDev[wDevID].midiDesc.hMidi = 0;
    return ret;
}

/*======================================================================*
 *                  	    MIDI entry points 				*
 *======================================================================*/

/**************************************************************************
 * 			midMessage (WINEOSS.@)
 */
DWORD WINAPI OSS_midMessage(UINT wDevID, UINT wMsg, DWORD_PTR dwUser,
			    DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    struct midi_in_message_params params;
    struct notify_context notify;
    UINT err;

    TRACE("(%04X, %04X, %08lX, %08lX, %08lX);\n",
	  wDevID, wMsg, dwUser, dwParam1, dwParam2);
    switch (wMsg) {
    case DRVM_INIT:
        return OSS_MidiInit();
    case DRVM_EXIT:
        return OSS_MidiExit();
    case MIDM_OPEN:
	return midOpen(wDevID, (LPMIDIOPENDESC)dwParam1, dwParam2);
    case MIDM_CLOSE:
	return midClose(wDevID);
    }

    params.dev_id = wDevID;
    params.msg = wMsg;
    params.user = dwUser;
    params.param_1 = dwParam1;
    params.param_2 = dwParam2;
    params.err = &err;
    params.notify = &notify;

    do
    {
        OSS_CALL(midi_in_message, &params);
        if ((!err || err == ERROR_RETRY) && notify.send_notify) notify_client(&notify);
    } while (err == ERROR_RETRY);

    return err;
}

/**************************************************************************
 * 				modMessage (WINEOSS.@)
 */
DWORD WINAPI OSS_modMessage(UINT wDevID, UINT wMsg, DWORD_PTR dwUser,
			    DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    struct midi_out_message_params params;
    struct notify_context notify;
    UINT err;

    TRACE("(%04X, %04X, %08lX, %08lX, %08lX);\n",
	  wDevID, wMsg, dwUser, dwParam1, dwParam2);

    switch (wMsg) {
    case DRVM_INIT:
        return OSS_MidiInit();
    case DRVM_EXIT:
        return OSS_MidiExit();
    }

    params.dev_id = wDevID;
    params.msg = wMsg;
    params.user = dwUser;
    params.param_1 = dwParam1;
    params.param_2 = dwParam2;
    params.err = &err;
    params.notify = &notify;

    OSS_CALL(midi_out_message, &params);

    if (!err && notify.send_notify) notify_client(&notify);

    return err;
}

static DWORD WINAPI notify_thread(void *p)
{
    struct midi_notify_wait_params params;
    struct notify_context notify;
    BOOL quit;

    params.notify = &notify;
    params.quit = &quit;

    while (1)
    {
        OSS_CALL(midi_notify_wait, &params);
        if (quit) break;
        if (notify.send_notify) notify_client(&notify);
    }
    return 0;
}

/**************************************************************************
 * 				DriverProc (WINEOSS.1)
 */
LRESULT CALLBACK OSS_DriverProc(DWORD_PTR dwDevID, HDRVR hDriv, UINT wMsg,
                                LPARAM dwParam1, LPARAM dwParam2)
{
     TRACE("(%08lX, %p, %08X, %08lX, %08lX)\n",
           dwDevID, hDriv, wMsg, dwParam1, dwParam2);

    switch(wMsg) {
    case DRV_LOAD:
        CloseHandle(CreateThread(NULL, 0, notify_thread, NULL, 0, NULL));
        return 1;
    case DRV_FREE:
        OSS_CALL(midi_release, NULL);
        return 1;
    case DRV_OPEN:
    case DRV_CLOSE:
    case DRV_ENABLE:
    case DRV_DISABLE:
    case DRV_QUERYCONFIGURE:
    case DRV_CONFIGURE:
        return 1;
    case DRV_INSTALL:
    case DRV_REMOVE:
        return DRV_SUCCESS;
    default:
	return 0;
    }
}
