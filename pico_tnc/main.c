/*
Copyright (c) 2021, JN1DFF
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright notice, 
  this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice, 
  this list of conditions and the following disclaimer in the documentation 
  and/or other materials provided with the distribution.
* Neither the name of the <organization> nor the names of its contributors 
  may be used to endorse or promote products derived from this software 
  without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/structs/uart.h"
#include "pico/util/queue.h"
#include "hardware/watchdog.h"

#include "tnc.h"
#include "receive.h"
#include "send.h"
#include "ax25.h"

//#include "usb_input.h"
#include "usb_output.h"
#include "serial.h"
#include "tty.h"
#include "kiss.h"

#define TIME_10MS (10 * 1000)    // 10 ms = 10 * 1000 us

// greeting message
static const uint8_t greeting[] =
    "\r\nPico TNCEMU Emulated Z80 TNC V 1.00\r\n";

int main()
{
    bool kiss_flash_state = false;
    uint32_t flash_time = tnc_time();

    stdio_init_all();

    // Initialize the kiss mode slect switch pin
    gpio_init(KISS_SELECT_GPIO);
    // Set the pin as input
    gpio_set_dir(KISS_SELECT_GPIO, GPIO_IN);
    // Enable the internal pull-up resistor
    gpio_pull_up(KISS_SELECT_GPIO);

    // Initialize the kiss mode slect switch pin
    gpio_init(PSAVE_SELECT_GPIO);
    // Set the pin as input
    gpio_set_dir(PSAVE_SELECT_GPIO, GPIO_IN);
    // Enable the internal pull-up resistor
    gpio_pull_up(PSAVE_SELECT_GPIO);

    // Initialize the kiss mode slect switch pin
    gpio_init(OPTION_SELECT_GPIO);
    // Set the pin as input
    gpio_set_dir(OPTION_SELECT_GPIO, GPIO_IN);
    // Enable the internal pull-up resistor
    gpio_pull_up(OPTION_SELECT_GPIO);


    /* check psave input and if set go into program download mode */
    if( gpio_get(PSAVE_SELECT_GPIO) == false) {
        reset_usb_boot(0, 0); // Enter USB bootloader mode
    }

    // create usb output queue
    usb_output_init();

    tty[0].kiss_mode = 0; // default kiss off
    tty[1].kiss_mode = 0;

    // Read it and if 0 set kiss mode.
    if( gpio_get(KISS_SELECT_GPIO) == false) {
        tty[0].kiss_mode = 1; // activate kiss
        tty[1].kiss_mode = 1; // activate kiss
    }
    else
    {
    // Wait 10 seconds for USB CDC serial is connected
        int usbWaitcnt = 1000;
        while (!stdio_usb_connected()) {
            sleep_ms(10);
            if(--usbWaitcnt == 0)
                break;
        }

        if (watchdog_caused_reboot()) {
            printf("Watch Dog Timer Failure\n");
        }
    }

    // initialize tnc
    tnc_init();
    send_init();
    receive_init();
    serial_init();
    tty_init();     // should call after tnc_init()
    //bell202_init();

    // If not in kiss mode print greeting
    if( !tty[0].kiss_mode|| !tty[1].kiss_mode) {
        // output greeting text to both tty's (serial/usb)
        tty_write_str(&tty[0], greeting);
        tty_write_str(&tty[1], greeting);
    }

#ifdef BUSY_PIN
    gpio_init(BUSY_PIN);
    gpio_set_dir(BUSY_PIN, true); // output
#endif

#define SMPS_PIN 23
#if 1
    gpio_init(SMPS_PIN);
    gpio_set_dir(SMPS_PIN, true); // output
    gpio_put(SMPS_PIN, 0);
#endif

    //uint32_t ts = time_us_32();

    // set watchdog, timeout 1000 ms
    watchdog_enable(1000, true);

    // main loop
    while (1) {

        // update watchdog timer
        watchdog_update();

#if 0 /* now done in receive.c receive funtion*/
        // advance tnc time
        if (time_us_32() - ts >= TIME_10MS) {
            ++tnc_time;
            ts += TIME_10MS;
        }
#endif

        /* Test if in Kiss mode and if not emulate */
        if(tty[0].kiss_mode == 0 && tty[1].kiss_mode == 0 ) 
        {
            // Emulate z80 code
            tnc_emulate();
        }
        else /* kiss mode */
        {
            if(ax25_InQ_HasData())
            {
                // incoming KISS frame to serial
                kiss_output(&tty[0],&tnc[0]);
                kiss_output(&tty[1],&tnc[0]);
                ax25_InQ_Remove();
            }

            // incoming KISS frame from serial
            int ch;
            if( !tty_getch(&tty[0], &ch) )
            {
                if( tty_getch(&tty[1], &ch) )
                {
                    kiss_input(&tty[1], ch);
                }
            }
            else
            {
                kiss_input(&tty[0], ch);
            }

            if (tnc_time() - flash_time >= TIME_1SECOND) {
                flash_time = tnc_time();
                kiss_flash_state = ! kiss_flash_state;
                gpio_put(tnc[0].staled_pin, kiss_flash_state);
                gpio_put(tnc[0].conled_pin, !kiss_flash_state);
            }
        }

        // receive packet
        receive();
        // send packet
        send();
        // process uart I/O
        serial_input();
        serial_output();

        // calibrate off
//        calibrate();

#ifdef BUSY_PIN
//        gpio_put(BUSY_PIN, 0);
#endif

    // if not busy wait small time for next interrupt
    if(tty[0].kiss_mode == 1 || tty[1].kiss_mode == 1) // always if in Kiss mode
    {
        __wfi();
    }
    else if(tnc[0].active_timeout == 0)
    {
        __wfi();
    }

#ifdef BUSY_PIN
//        gpio_put(BUSY_PIN, 1);
#endif

    }

    return 0;
}
