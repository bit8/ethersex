/*
* frequency counter
* main measurement functions
*
* Copyright (c) 2011 by Gerd v. Egidy <gerd@egidy.de>
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 3
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
* For more information on the GPL, please go to:
* http://www.gnu.org/copyleft/gpl.html
*/

#include <avr/io.h>
#include <avr/interrupt.h>

#include "config.h"
#include "core/bit-macros.h"
#include "core/debug.h"
#include "core/periodic.h"

#include "freqcount.h"
#include "freqcount_internal.h"

// define them static to allow the compiler inlining them
static void start_measure(void);
static void measure_done(void);
static void check_measure_timeout(void);

// variables to communicate between pin change ISR and control
volatile tick24bit_t freqcount_start;
volatile tick24bit_t freqcount_freq_end;
#ifdef FREQCOUNT_DUTY_SUPPORT
volatile tick24bit_t freqcount_on_end;
#endif
volatile freqcount_state_t freqcount_state = FC_DISABLED;

void freqcount_mainloop(void)
{
    if (freqcount_state==FC_DISABLED)
        start_measure();
    else if (freqcount_state==FC_DONE)
    {
        measure_done();
        // for now: automatically restart
        start_measure();
    }
    
    check_measure_timeout();
}

static void start_measure(void)
{
    freqcount_state=FC_BEFORE_START;
    
    // trigger on rising edge
    TCCR1B |= _BV(ICES1);

    // clear timer interrupt capture flag by writing a 1:
    // required after changing the trigger direction
    TIFR1 = _BV(ICF1);

    // disable the noise canceler
    TCCR1B &= ~(_BV(ICNC1));

    // disable triggering timer interrupt capture from the analog comparator
    // this makes sure the ICP1 pin is used
    ACSR &= ~(_BV(ACIC));
    
    // enable timer input capture interrupt
    TIMSK1 |= _BV(ICIE1);
}

static void measure_done(void)
{
    // disable timer input capture interrupt
    TIMSK1 &= ~(_BV(ICIE1));
    freqcount_state=FC_DISABLED;
    
    // convert from 24 bit to 32 to do the math
    
    uint32_t t1=0, t2=0;
 
#ifndef FREQCOUNT_NOSLOW_SUPPORT
    *(((uint8_t*)(&t1))+2)=freqcount_start.high;
    *(((uint8_t*)(&t2))+2)=freqcount_freq_end.high;
#endif    
    *((uint16_t*)(&t1))=freqcount_start.low;
    *((uint16_t*)(&t2))=freqcount_freq_end.low;

    // handle overflow with different sized types
    if (t1 > t2)
    {
#ifdef FREQCOUNT_NOSLOW_SUPPORT
        *(((uint8_t*)(&t2))+2)=1;
#else
        *(((uint8_t*)(&t2))+3)=1;
#endif
    }
    
    // calc frequency
    uint32_t freqcount_ticks=t2-t1;
    
#ifdef FREQCOUNT_DUTY_SUPPORT
    // calc on cycle tickts
    // end of frequency measurement is start of on-cyle
#ifndef FREQCOUNT_NOSLOW_SUPPORT
    *(((uint8_t*)(&t1))+2)=freqcount_on_end.high;
#endif    
    *((uint16_t*)(&t1))=freqcount_on_end.low;

    // handle overflow with different sized types
    if (t2 > t1)
    {
#ifdef FREQCOUNT_NOSLOW_SUPPORT
        *(((uint8_t*)(&t1))+2)=1;
#else
        *(((uint8_t*)(&t1))+3)=1;
#endif
    }
    
    uint8_t freqcount_duty=((t1-t2)<<8)/freqcount_ticks;

    freqcount_average_results(freqcount_ticks, freqcount_duty);
#else
    freqcount_average_results(freqcount_ticks);
#endif
}

static void check_measure_timeout(void)
{
    static freqcount_state_t last_reset_state=FC_DISABLED;

    if (freqcount_state != last_reset_state)
    {
        overflows_since_freq_start=0;
        last_reset_state=freqcount_state;
    }
    else if (overflows_since_freq_start > 1 && 
        (freqcount_state==FC_BEFORE_START ||
         freqcount_state==FC_FREQ ||
         freqcount_state==FC_ON_CYCLE))
    {
        // we are inside a measurement, but we had two overflows
        // -> we can't get reliable data anymore

        // disable timer input capture interrupt
        TIMSK1 &= ~(_BV(ICIE1));

        freqcount_state=FC_DISABLED;
        last_reset_state=FC_DISABLED;
        overflows_since_freq_start=0;
        
#ifdef FREQCOUNT_DUTY_SUPPORT
        freqcount_average_results(0,0);
#else
        freqcount_average_results(0);
#endif
    }
}

ISR(TIMER1_CAPT_vect)
{
    if (freqcount_state==FC_BEFORE_START)
    {
        // start to measure frequency
        freqcount_start.low=ICR1;
#ifndef FREQCOUNT_NOSLOW_SUPPORT
        freqcount_start.high=timer_overflows;
#endif
        freqcount_state=FC_FREQ;
    }
    else if (freqcount_state==FC_FREQ)
    {
        // next rising edge -> we have the frequency
        freqcount_freq_end.low=ICR1;
#ifndef FREQCOUNT_NOSLOW_SUPPORT
        freqcount_freq_end.high=timer_overflows;
#endif

#ifndef FREQCOUNT_DUTY_SUPPORT
        freqcount_state=FC_DONE;
#else /* FREQCOUNT_DUTY_SUPPORT */
        // start to measure the on-cycle
        // trigger on falling edge
        TCCR1B &= ~(_BV(ICES1));
        
        // clear timer interrupt capture flag by writing a 1:
        // required after changing the trigger direction
        TIFR1 = _BV(ICF1);
        
        freqcount_state=FC_ON_CYCLE;
    }
    else if (freqcount_state==FC_ON_CYCLE)
    {
        freqcount_on_end.low=ICR1;
#ifndef FREQCOUNT_NOSLOW_SUPPORT
        freqcount_on_end.high=timer_overflows;
#endif /* FREQCOUNT_NOSLOW_SUPPORT */
        freqcount_state=FC_DONE;
#endif /* FREQCOUNT_DUTY_SUPPORT */
    }
}

/*
  -- Ethersex META --
  header(services/freqcount/freqcount.h)
*/
