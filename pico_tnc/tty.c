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
#include <string.h>
#include "pico/stdlib.h"
#include "class/cdc/cdc_device.h"
#include "pico/sync.h"
#include "hardware/uart.h"

#include "tnc.h"
#include "usb_output.h"
#include "usb_input.h"
#include "serial.h"

// usb echo flag
//uint8_t usb_echo = 1; // on

// tty info
tty_t tty[TTY_N];

//static uint8_t cmd_buf[CMD_LEN + 1];
//static int cmd_idx = 0;

static const enum TTY_MODE tty_mode[] = {
    TTY_TERMINAL,
    TTY_TERMINAL,
    TTY_GPS,
};

static const enum TTY_SERIAL tty_serial[] = {
    TTY_USB,
    TTY_UART0,
    TTY_UART1,
};

void tty_init(void)
{
    for (int i = 0; i < TTY_N; i++) {
        tty_t *ttyp = &tty[i];

        ttyp->num = i;

        ttyp->tty_mode = tty_mode[i];
        ttyp->tty_serial = tty_serial[i];
    }
}

void tty_write(tty_t *ttyp, uint8_t const *data, int len)
{
    if (ttyp->tty_serial == TTY_USB) {
        usb_write(data, len);
        return;
    }

    if (ttyp->tty_serial == TTY_UART0) serial_write(data, len);
}

void tty_write_char(tty_t *ttyp, uint8_t ch)
{
    if (ttyp->tty_serial == TTY_USB) {
        usb_write_char(ch);
        return;
    }

    if (ttyp->tty_serial == TTY_UART0) serial_write_char(ch);
}

void tty_write_str(tty_t *ttyp, uint8_t const *str)
{
    int len = strlen(str);

    tty_write(ttyp, str, len);
}

#define CAL_DATA_MAX 3

static const uint8_t calibrate_data[CAL_DATA_MAX] = {
    0x00, 0xff, 0x55,
};

static const char *calibrate_str[CAL_DATA_MAX] = {
    "send space (2200Hz)\r\n",
    "send mark  (1200Hz)\r\n",
    "send 0x55  (1200/2200Hz)\r\n",
};

void tty_input(tty_t *ttyp, int ch)
{
    ttyp->input_buf[ttyp->inp_head++] = ch;
    ttyp->inp_head &= 0xFF;
}

bool tty_getch(tty_t *ttyp, int *ch)
{
    if(ttyp->inp_head == ttyp->inp_tail)
        return(false);

    *ch = ttyp->input_buf[ttyp->inp_tail++];
    ttyp->inp_tail &= 0xFF;
    return(true);
}

bool tty_peek(tty_t *ttyp)
{
    if(ttyp->inp_head == ttyp->inp_tail)
        return(false);
    else return true;
}
