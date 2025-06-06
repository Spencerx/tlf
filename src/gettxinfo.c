/*
 * Tlf - contest logging program for amateur radio operators
 * Copyright (C) 2001-2002-2003-2004 Rein Couperus <pa0r@amsat.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/* ------------------------------------------------------------
 *      get trx info
 *
 *--------------------------------------------------------------*/


#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>

#include "bands.h"
#include "cw_utils.h"
#include "err_utils.h"
#include "fldigixmlrpc.h"
#include "gettxinfo.h"
#include "globalvars.h"
#include "hamlib_keyer.h"
#include "tlf.h"
#include "tlf_curses.h"
#include "callinput.h"
#include "changepars.h"

#include <hamlib/rig.h>

#ifdef RIG_PASSBAND_NOCHANGE
#define TLF_DEFAULT_PASSBAND RIG_PASSBAND_NOCHANGE
#else
#define TLF_DEFAULT_PASSBAND RIG_PASSBAND_NORMAL
#endif

/* output frequency to rig or other rig-related request
 *
 * possible values:
 *  0 - poll rig
 *  SETCWMODE
 *  SETSSBMODE
 *  RESETRIT
 *  SETDIGIMODE
 *  else - set rig frequency
 *
 */
static freq_t outfreq = 0;

static pthread_mutex_t outfreq_mutex = PTHREAD_MUTEX_INITIALIZER;

static double get_current_seconds();
static void handle_trx_bandswitch(const freq_t freq);

void set_outfreq(freq_t hertz) {
    if (!trx_control) {
	hertz = 0;      // no rig control, ignore request
    }
    if (!rig_mode_sync
	    && (hertz == SETCWMODE || hertz == SETSSBMODE || hertz == SETDIGIMODE)
       ) {
	hertz = 0;      // no rig mode setting, ignore request
    }
    pthread_mutex_lock(&outfreq_mutex);
    outfreq = hertz;
    pthread_mutex_unlock(&outfreq_mutex);
}

freq_t get_outfreq() {
    return outfreq;
}

static freq_t get_and_reset_outfreq() {
    pthread_mutex_lock(&outfreq_mutex);
    freq_t f = outfreq;
    outfreq = 0.0;
    pthread_mutex_unlock(&outfreq_mutex);
    return f;
}

static rmode_t get_ssb_mode() {
    // LSB below 14 MHz, USB above it
    return (freq < bandcorner[BANDINDEX_20][0] ? RIG_MODE_LSB : RIG_MODE_USB);
}

static pbwidth_t get_cw_bandwidth() {
    // use specified bandwidth (CWBANDWIDTH) if available
    return (cw_bandwidth > 0 ? cw_bandwidth : TLF_DEFAULT_PASSBAND);
}


static void poll_rig_state() {
    static int oldbandinx = -1;
    static double last_freq_time = 0.0;

    freq_t rigfreq = 0.0;

    double now = get_current_seconds();
    if (now < last_freq_time + 0.2) {
	return;   // last read-out was within 200 ms, skip this query
    }
    last_freq_time = now;

    pthread_mutex_lock(&tlf_rig_mutex);
    vfo_t vfo;
    int retval = rig_get_vfo(my_rig, &vfo); /* initialize RIG_VFO_CURR */
    pthread_mutex_unlock(&tlf_rig_mutex);

    if (retval == RIG_OK || retval == -RIG_ENIMPL || retval == -RIG_ENAVAIL) {
	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_get_freq(my_rig, RIG_VFO_CURR, &rigfreq);
	pthread_mutex_unlock(&tlf_rig_mutex);

	if (retval == RIG_OK &&
		(
		    (trxmode == DIGIMODE && (digikeyer == GMFSK || digikeyer == FLDIGI))
		    ||
		    rig_mode_sync
		)
	   ) {
	    pthread_mutex_lock(&tlf_rig_mutex);
	    pbwidth_t bwidth;
	    int retvalmode = rig_get_mode(my_rig, RIG_VFO_CURR, &rigmode, &bwidth);
	    pthread_mutex_unlock(&tlf_rig_mutex);

	    if (retvalmode != RIG_OK) {
		rigmode = RIG_MODE_NONE;
	    }
	}
    }

    if (trxmode == DIGIMODE && (digikeyer == GMFSK || digikeyer == FLDIGI)) {
	rigfreq += (freq_t)fldigi_get_carrier();
	if (rigmode == RIG_MODE_RTTY || rigmode == RIG_MODE_RTTYR) {
	    int fldigi_shift_freq = fldigi_get_shift_freq();
	    if (fldigi_shift_freq != 0) {
		pthread_mutex_lock(&tlf_rig_mutex);
		retval = rig_set_freq(my_rig, RIG_VFO_CURR,
				      ((freq_t)rigfreq + (freq_t)fldigi_shift_freq));
		pthread_mutex_unlock(&tlf_rig_mutex);
	    }
	}
    } else if (trxmode == CWMODE && (rigmode == RIG_MODE_LSB
				     || rigmode == RIG_MODE_USB)) {
	set_trxmode_internally(SSBMODE);
    } else if (trxmode != CWMODE && (rigmode == RIG_MODE_CW
				     || rigmode == RIG_MODE_CWR)) {
	set_trxmode_internally(CWMODE);
    }

    if (retval != RIG_OK || rigfreq < 0.1) {
	freq = 0.0;
	return;
    }

    if (rigfreq >= bandcorner[0][0]) {
	freq = rigfreq; // Hz
    }

    bandinx = freq2bandindex((unsigned int)freq);

    bandfrequency[bandinx] = freq;

    if (bandinx != oldbandinx) {	// band change on trx
	oldbandinx = bandinx;
	handle_trx_bandswitch((int) freq);
    }

    /* read speed from rig */
    if (cwkeyer == HAMLIB_KEYER) {
	int rig_cwspeed;
	retval = hamlib_keyer_get_speed(&rig_cwspeed);

	if (retval == RIG_OK) {
	    if (speed != rig_cwspeed) {
		speed = rig_cwspeed;

		attron(COLOR_PAIR(C_HEADER) | A_STANDOUT);
		mvprintw(0, 14, "%2u", speed);
	    }
	} else {
	    TLF_SHOW_WARN("Problem with rig link: %s", tlf_rigerror(retval));
	}
    }

}

void gettxinfo(void) {

    int retval;

    if (!trx_control)
	return;

    /* CAT PTT wanted, available, inactive, and PTT On requested
     */
    if (rigptt == (CAT_PTT_USE | CAT_PTT_ON)) {
	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_set_ptt(my_rig, RIG_VFO_CURR, RIG_PTT_ON);
	pthread_mutex_unlock(&tlf_rig_mutex);

	/* Set PTT active bit. */
	rigptt |= CAT_PTT_ACTIVE;

	/* Clear PTT On requested bit. */
	rigptt &= ~CAT_PTT_ON;
    }

    /* CAT PTT wanted, available, active and PTT Off requested
     */
    if (rigptt == (CAT_PTT_USE | CAT_PTT_ACTIVE | CAT_PTT_OFF)) {
	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_set_ptt(my_rig, RIG_VFO_CURR, RIG_PTT_OFF);
	pthread_mutex_unlock(&tlf_rig_mutex);

	/* Clear PTT Off requested bit. */
	rigptt &= ~CAT_PTT_OFF;

	/* Clear PTT active bit. */
	rigptt &= ~CAT_PTT_ACTIVE;
    }

    freq_t reqf = get_and_reset_outfreq();  // get actual request

    if (reqf == 0) {

	poll_rig_state();

    } else if (reqf == SETCWMODE) {

	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_set_mode(my_rig, RIG_VFO_CURR, RIG_MODE_CW, get_cw_bandwidth());
	pthread_mutex_unlock(&tlf_rig_mutex);

	if (retval != RIG_OK) {
	    TLF_SHOW_WARN("Problem with rig link: %s", tlf_rigerror(retval));
	}

    } else if (reqf == SETSSBMODE) {

	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_set_mode(my_rig, RIG_VFO_CURR, get_ssb_mode(),
			      TLF_DEFAULT_PASSBAND);
	pthread_mutex_unlock(&tlf_rig_mutex);

	if (retval != RIG_OK) {
	    TLF_SHOW_WARN("Problem with rig link: %s", tlf_rigerror(retval));
	}

    } else if (reqf == SETDIGIMODE) {
	rmode_t new_mode = digi_mode;
	if (new_mode == RIG_MODE_NONE) {
	    if (digikeyer == FLDIGI)
		new_mode = RIG_MODE_USB;
	    else
		new_mode = RIG_MODE_LSB;
	}
	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_set_mode(my_rig, RIG_VFO_CURR, new_mode,
			      TLF_DEFAULT_PASSBAND);
	pthread_mutex_unlock(&tlf_rig_mutex);

	if (retval != RIG_OK) {
	    TLF_SHOW_WARN("Problem with rig link: %s", tlf_rigerror(retval));
	}

    } else if (reqf == RESETRIT) {
	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_set_rit(my_rig, RIG_VFO_CURR, 0);
	pthread_mutex_unlock(&tlf_rig_mutex);

	if (retval != RIG_OK) {
	    TLF_SHOW_WARN("Problem with rig link: %s", tlf_rigerror(retval));
	}

    } else {
	// set rig frequency (or carrier) to `reqf'
	reqf -= fldigi_get_carrier();
	pthread_mutex_lock(&tlf_rig_mutex);
	retval = rig_set_freq(my_rig, RIG_VFO_CURR, (freq_t) reqf);
	pthread_mutex_unlock(&tlf_rig_mutex);

	if (retval != RIG_OK) {
	    TLF_SHOW_WARN("Problem with rig link: set frequency: %s", tlf_rigerror(retval));
	}

    }

}


static double get_current_seconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}


static void handle_trx_bandswitch(const freq_t freq) {

    send_bandswitch(freq);

    if (!rig_mode_sync) {
	return;     // don't touch rig mode
    }

    rmode_t mode = RIG_MODE_NONE;           // default: no change
    pbwidth_t width = TLF_DEFAULT_PASSBAND; // passband width, in Hz

    if (trxmode == SSBMODE) {
	mode = get_ssb_mode();
    } else if (trxmode == DIGIMODE) {
	if ((rigmode & (RIG_MODE_LSB | RIG_MODE_USB | RIG_MODE_RTTY | RIG_MODE_RTTYR))
		!= rigmode) {
	    mode = RIG_MODE_LSB;
	}
    } else {
	mode = RIG_MODE_CW;
	width = get_cw_bandwidth();
    }

    if (mode == RIG_MODE_NONE) {
	return;     // no change was requested
    }

    pthread_mutex_lock(&tlf_rig_mutex);
    int retval = rig_set_mode(my_rig, RIG_VFO_CURR, mode, width);
    pthread_mutex_unlock(&tlf_rig_mutex);

    if (retval != RIG_OK) {
	TLF_SHOW_WARN("Problem with rig link: %s", tlf_rigerror(retval));
    }
}

