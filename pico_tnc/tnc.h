/**
 * Copyright (c) 2021 JN1DFF
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "pico/util/queue.h"

#include "filter.h"
#include "ax25.h"
//#include "cmd.h"

#include "sio.h"

// number of ports
#define PORT_N 1    // number of ports, 1..3

#define BAUD_RATE 1200
#define SAMPLING_N 11
//#define DELAY_N 3
//#define SAMPLING_RATE ((1000000*DELAY_N+DELAY_US/2)/DELAY_US)
#define SAMPLING_RATE (BAUD_RATE * SAMPLING_N)
#define DELAY_US 446 // 446us
#define DELAYED_N ((SAMPLING_RATE * DELAY_US + 500000) / 1000000)

#define ADC_SAMPLING_RATE (SAMPLING_RATE * PORT_N)

#define DATA_LEN 1024                   // packet receive buffer size

#define FIR_LPF_N 27
#define FIR_BPF_N 25

#define ADC_BIT 8       // adc bits 8 or 12
//#define ADC_BIT 12      // adc bits 8 or 12

//#define BELL202_SYNC 1  // sync decode
#define DECODE_PLL 1    // use PLL

#define CONTROL_N 10
#define DAC_QUEUE_LEN 64
#define DAC_BLOCK_LEN (DAC_QUEUE_LEN + 1)

#define SEND_QUEUE_LEN (1024 * 16)

#define AX25_FLAG 0x7e

#define BUSY_PIN 22

#define BEACON_PORT 0

#define TTY_N 3                             // number of serial
#define CMD_BUF_LEN 255

#define VERSION_INFO "1.0"

/* Additional Z80_STATE status flag to request emulation termination. */
#define FLAG_STOP_EMULATION     (1 << 31)
//#define TNCEMUDEBUG 1

/* Emulation Defines */
#define Z80_CPU_SPEED           8195200   /* In Hz. */
#define CYCLES_PER_PASS         (Z80_CPU_SPEED / 400)
#define CYCLES_PER_INT		    (CYCLES_PER_PASS / 10) /*Cycles to run for each int processing */
#define DEFAULT_BBS_MSG "Happy if u post msg"
#define TIMER_TIME_10MS (10 * 1000)    // 10 ms = 10 * 1000 us
#define TIME_1SECOND 1000000 // 1 million us = 1 second

/* Rom image is externally linked in. */
extern unsigned char _binary_hk21rom_bin_start;
extern unsigned char _binary_hk21rom_bin_end;
extern unsigned char _binary_hk21rom_bin_size;

/* Rom Image for Tnc Emulator */
extern unsigned char *Rom;

enum STATE {
	FLAG,
	DATA
};

typedef struct {
  int low_i;
  int low_q;
  int high_i;
  int high_q;
} values_t;

typedef struct TTY tty_t;

typedef struct TNC {
    uint8_t port;

    // receive

    // demodulator
    uint8_t bit;

    // decode_bit
    uint16_t data_cnt;
    uint8_t data[DATA_LEN];
    uint8_t state;
    uint8_t flag;
    uint8_t data_byte;
    uint8_t data_bit_cnt;

    // decode
    uint8_t edge;
    
    // decode2
    int32_t pll_counter;
    uint8_t pval;
    uint8_t nrzi;

    // bell202_decode
    int delayed[DELAYED_N];
    int delay_idx;
    int cdt;
    int cdt_lvl;
    int avg;
    uint8_t cdt_pin;

    // bell202_decode2
    int sum_low_i;
    int sum_low_q;
    int sum_high_i;
    int sum_high_q;
    int low_idx;
    int high_idx;
    values_t values[SAMPLING_N];
    int values_idx;
    filter_t lpf;
    filter_t bpf;

    // send

/* Kiss Parameter Offset defines */
#define NUMKISSPARMS 5
#define KISS_TXDELAY 0
#define KISS_P 1
#define KISS_SLOT 2
#define KISS_TXTAIL 3
#define KISS_FULLDUPLEX 4

    uint8_t ax25_parms[NUMKISSPARMS];

    // dac queue
    queue_t dac_queue;

    // DAC, PTT pin
    uint8_t ptt_pin;
    uint8_t pwm_pin;
    uint8_t pwm_slice;

    // DMA channels
    uint8_t ctrl_chan;
    uint8_t data_chan;
    uint32_t data_chan_mask;
    uint8_t busy;

    // Bell202 wave generator
    int next;
    int phase;
    int level;
    int cnt_one;

    // wave buffer for DMA
    uint32_t const *dma_blocks[DAC_BLOCK_LEN][CONTROL_N + 1];

    // send data queue
    queue_t send_queue;
    int send_time;
    int send_len;
    int send_state;
    int send_data;

    // calibrate
    uint8_t cal_data;
    bool do_nrzi;
    uint32_t cal_time;
    tty_t *ttyp;

    // station and console leds
    uint8_t conled_pin;
    uint8_t staled_pin;

} tnc_t;

extern tnc_t tnc[];
extern uint32_t __tnc_time;

void tnc_init(void);
void tnc_emulate(void);
int IO_in (int);
void IO_out (int, int);
void SIO_Reset( IC_SIO *);
int SIO_Cmd_Read( IC_SIO *);
unsigned int Memory_Read_Byte(unsigned int);
unsigned int Memory_Read_Word(unsigned int);
void Memory_Write_Byte(unsigned int, unsigned int);
void Memory_Write_Word(unsigned int, unsigned int);
int kbhit(void);
char tobcd(unsigned int);
void RewriteBbsMsg(int addr, char *txt );
unsigned int GetNextBbsMsgNo(void);

inline uint32_t tnc_time(void)
{
    return __tnc_time;
}

// TNC command
enum MONITOR {
    MON_ALL = 0,
    MON_OFF,
};

// tty
enum TTY_MODE {
    TTY_TERMINAL = 0,
    TTY_GPS,
};

enum TTY_SERIAL {
    TTY_USB = 0,
    TTY_UART0,
    TTY_UART1,
};

typedef struct TTY {
    uint8_t input_buf[CMD_BUF_LEN + 1];
    int inp_head;
    int inp_tail;

    uint8_t num;        // index of tty[]

    uint8_t tty_mode;   // terminal or GPS
    uint8_t tty_serial; // USB, UART0, UART1

    // Decode Monitor
    uint8_t montype;

    tnc_t *tp;          // input/output port No.
} tty_t;

extern tty_t tty[];

// send process state
enum SEND_STATE {
    SP_IDLE = 0,
    SP_WAIT_CLR_CH,
    SP_P_PERSISTENCE,
    SP_WAIT_SLOTTIME,
    SP_PTT_ON,
    SP_SEND_FLAGS,
    SP_DATA_START,
    SP_DATA,
    SP_ERROR,
    SP_CALIBRATE,
    SP_CALIBRATE_OFF,
};
