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
#include "pico/stdlib.h"
#include "hardware/rtc.h"
#include "hardware/watchdog.h"

#include "tnc.h"
#include "z80emu.h"

#include "ax25.h"
#include "flash.h"
#include "tty.h"
#include "send.h"

uint32_t __tnc_time;

tnc_t tnc[PORT_N];

double  cycles;
double  total;

int  timer_int;

/* tnc emulator */
unsigned char flop,oldptt;
unsigned char RxCharIn_Idx=0;
unsigned char ax25rdy=0;
unsigned char feedflag=0;
unsigned char abortflag=0;
unsigned char txundr_count=0;
unsigned short int mycrc;
int rxcnt;

/* Output Buffer */
unsigned char Ax25_Out[BUFLEN];
unsigned int  Ax25_Out_Cnt;
unsigned int  Ax25_In_Dly = 0;

unsigned int PrevbbsMsgNo;
unsigned int clock_address = 0; /* Clock stucture in TNC Ram */
unsigned int bbsmsg_address = 0;

/* Locations in ram where z80 code stores these parameters */
unsigned int ax25_parm_location[NUMKISSPARMS-1]= {0x3FDB, 0x4033, 0x4035, 0x3FD7};

/* The Emulated TNC has 32k of RAM and 32k of ROM.
   Rom is addressed starting at 0 and Ram at 0x8000 */
unsigned char *Rom = &_binary_hk21rom_bin_start;

/* Emulated TNC Ram Space */
unsigned char   Ram[1 << 15];

/* Declare struct vars for SIO channels */
IC_SIO	sioa;
IC_SIO	siob;
Z80_STATE       state;

void tnc_init(void)
{
  int x;
  /* Init ax25 Receive Q pointers */
  ax25_init_Q();

  rtc_init(); // Initialize the RTC

  clock_address = 0x4f0a; /* Where tnc keeps time */
  bbsmsg_address = 0x4f06; /* where tnc stores msg count */

  /* Patch for rom we can manually patch later, Needed? */
//  Rom[0x5032] = Rom[0x5041];
  Rom[0x5032] = 0x3e;

  /* Stuff NOPs to disable strange obfuscation of text */
  for(int x=0; x< 11; x++) Rom[0x47f7+x] = 0;
  Rom[0x47f7+12] = 0;

  /* Read GPIO ? and if set clear ram */
  if(0)
  {
    for(x=0; x<sizeof(Ram); x++)
    {
        Ram[x]=0;
    }
  }
  else // read saved ram memory from flash
  {
      printf("TNCEMU:Reading Ram Data from Flash ");
      int slot = flash_read(Ram, sizeof(Ram));
      if (slot >= 0) 
      {
          printf("slot %d\n", slot);
      } else 
      {
          printf(".\nRead failed!\n");
      }
  }

  char NewBBsMsg[] = DEFAULT_BBS_MSG;
  /* Throw some custom text into eprom for when user logs into bbs */
  /* Replaces "Heath System" */
  RewriteBbsMsg(0x2dad, NewBBsMsg);

  /* initialize the previous bbs msg # to current in memory
  for later comparison to see if a msg was added */
  PrevbbsMsgNo = GetNextBbsMsgNo();

  // Set a dummy time if the RTC is not already set (optional, for testing)
  datetime_t initial_time = {
      .year = 2000,
      .month = 8,
      .day = 6,
      .dotw = 3, // Wednesday
      .hour = 12,
      .min = 0,
      .sec = 0
  };
  rtc_set_datetime(&initial_time);
  Ram[clock_address+5] = 0x20; // Set year in tnc ram

  // filter initialization
  // LPF
  static const filter_param_t flt_lpf = {
      .size = FIR_LPF_N,
      .sampling_freq = SAMPLING_RATE,
      .pass_freq = 0,
      .cutoff_freq = 1200,
  };
  int16_t *lpf_an, *bpf_an;

  lpf_an = filter_coeff(&flt_lpf);

#if 0
  printf("LPF coeffient\n");
  for (int i = 0; i < flt_lpf.size; i++) {
      printf("%d\n", lpf_an[i]);
  }
#endif
  // BPF
  static const filter_param_t flt_bpf = {
      .size = FIR_BPF_N,
      .sampling_freq = SAMPLING_RATE,
      .pass_freq = 900,
      .cutoff_freq = 2500,
  };
  bpf_an = filter_coeff(&flt_bpf);
#if 0
  printf("BPF coeffient\n");
  for (int i = 0; i < flt_bpf.size; i++) {
      printf("%d\n", bpf_an[i]);
  }
#endif
  // PORT initialization
  for (x = 0; x < PORT_N; x++) {
      tnc_t *tp = &tnc[x];

      // Console and station leds set gpio pins
      tp->conled_pin = CON_LED_GPIO;
      tp->staled_pin = STA_LED_GPIO;

      gpio_init(tp->conled_pin);
      gpio_set_dir(tp->conled_pin, GPIO_OUT);
      gpio_init(tp->staled_pin);
      gpio_set_dir(tp->staled_pin, GPIO_OUT);

#ifdef TNC_EMULATING_LED_PIN
      gpio_init(TNC_EMULATING_LED_PIN);
      gpio_set_dir(TNC_EMULATING_LED_PIN, GPIO_OUT);
      gpio_put(TNC_EMULATING_LED_PIN,0);
#endif

      // receive
      tp->port = x;
      tp->state = FLAG;
      filter_init(&tp->lpf, lpf_an, FIR_LPF_N);
      filter_init(&tp->bpf, bpf_an, FIR_BPF_N);

      // send queue
      queue_init(&tp->send_queue, sizeof(uint8_t), SEND_QUEUE_LEN);
      tp->send_state = SP_IDLE;

      tp->cdt = 0;
      tp->ax25_parms[KISS_TXDELAY] = 50;
      tp->ax25_parms[KISS_P] = 63;
      tp->ax25_parms[KISS_SLOT] = 10;
      tp->ax25_parms[KISS_TXTAIL] = 0;
      tp->ax25_parms[KISS_FULLDUPLEX] = 0;

      // calibrate
      tp->do_nrzi = true;
  }

  //printf("%d ports support\n", PORT_N);
  //printf("DELAYED_N = %d\n", DELAYED_N);

  tnc_t *tp = &tnc[0];
  /* Memorize certain ax25 parms to detect any changes later */
  for(x=0; x< NUMKISSPARMS-1; x++) /* start 1 skip txdelay for now */
  {
    tp->ax25_parms[x] = Ram[ax25_parm_location[x]];
  }

    /* Reset emulated SIO state machines */
    SIO_Reset(&sioa); /* Reset Emulated Serial i/o a */
    SIO_Reset(&siob); /* Reset Emulated Serial i/o b */

    /* Reset z80 emulator */
    Z80Reset(&state);

    /* Init z80 cycle time counters */
    total = timer_int = 0;
}

/* Run some cycles of emulated tnc */
void tnc_emulate(void)
{
    bool flashUpdate = false;
    unsigned int x;
    tnc_t *tp = &tnc[0];

    uint32_t ts = time_us_32();
    uint32_t cs = time_us_32();


    #ifdef TNCEMUDEBUG
    printf("PC=%x cycles=%.0f\n",state.pc,total);
    cycles = Z80Emulate(&state, 1);
#endif
    cycles = Z80Emulate(&state, CYCLES_PER_PASS);
    total += cycles;

  /* Every other run do a timer interrupt, we come into the emulator
  roughly ever 10ms not super acurate but it */
  if( timer_int == 0)
  {
    timer_int = 3;
    total += Z80Interrupt (&state, 0x10 );
    cycles = Z80Emulate(&state, CYCLES_PER_INT);
    total += cycles;
  }
  else
  {
    timer_int--;
  }

  // /* Every second update our clock from pico rtc */
  if (tnc_time() - cs >= TIME_1SECOND)
  {
    cs += TIME_1SECOND;

    datetime_t current_time;
    rtc_get_datetime(&current_time);

    // Hear check if our Clock's year matches PICO RTC and if not
    // Update RTC from tncemu's clock.
    x= current_time.year;
    x = x - ((x / 100) * 100);
    if( Ram[clock_address+5] != tobcd(x) )
    {
      // printf("RTC Before %d:%d:%d:%d:%d\n",current_time.year,current_time.month,current_time.day,
      //   current_time.hour,current_time.min);
      current_time.sec = 0;
      current_time.min = frombcd(Ram[clock_address+1]);
      current_time.hour = frombcd(Ram[clock_address+2]);
      current_time.day = frombcd(Ram[clock_address+3]);
      current_time.month = frombcd(Ram[clock_address+4]) + 1;
      current_time.year = frombcd(Ram[clock_address+5]) + 2000;
      rtc_set_datetime(&current_time);
      // printf("RTC Update %d:%d:%d:%d:%d\n",current_time.year,current_time.month,current_time.day,
      //   current_time.hour,current_time.min);
    }
    else
    {
      /* here update tnc time with our pico rtc time */
      x= current_time.sec;
      Ram[clock_address] = tobcd(x);
      x= current_time.min;
      Ram[clock_address+1] = tobcd(x);
      x= current_time.hour;
      Ram[clock_address+2] = tobcd(x);
      x= current_time.day;
      Ram[clock_address+3] = tobcd(x);
      x= current_time.month - 1;
      Ram[clock_address+4] = tobcd(x);
      // Don't need to update years as they match
      // x= current_time.year;
      // x= x - ((x / 100) * 100);
      // Ram[clock_address+5] = tobcd(x);
    }

    /* Check if any new bbs msgs have arrived and if so save ram to disk */
    if( PrevbbsMsgNo != GetNextBbsMsgNo())
    {
        PrevbbsMsgNo = GetNextBbsMsgNo();
        flashUpdate = true;
    }

    /* compare saved kiss parms to ram parms and if chaned update sender parms  */
    for(x=0; x< NUMKISSPARMS-1; x++)
    {
      if(tp->ax25_parms[x] != Ram[ax25_parm_location[x]] )
      {
        tp->ax25_parms[x] = Ram[ax25_parm_location[x]];
        if(x == KISS_TXDELAY) flashUpdate = true; /* only update if this parm is changed */
      }
    }

    if(flashUpdate)
    {
      flashUpdate = false;

      /* Disable watchdog during flash writes */
      watchdog_disable();

      printf("TNCEMU:Saving Ram Data to Flash ");
      int slot = flash_write(Ram, sizeof(Ram));
      if (slot >= 0) 
      {
          printf("slot %d\n", slot);
      } else 
      {
          printf(".\nWrite failed!\n");
      }

      // set watchdog, timeout 1000 ms
      watchdog_enable(1000, true);
    }

    /* Here check SIO dtr values and set gpio's for leds according to status */
    gpio_put(tp->staled_pin, !(sioa.registers[5] & 0x80));
    gpio_put(tp->conled_pin, !(siob.registers[5] & 0x80));
  }

/* These values may need to be adjusted depending on the speed of 
the machine you will be emulating on. */
    if( 1 ) /* on fast x86 */
    {
      flop = flop ^0x01;
      if(flop)
      {

        if(RxCharIn_Idx || ax25rdy)
        {
          if(RxCharIn_Idx)
          {
            while(!state.iff1) cycles = Z80Emulate(&state, CYCLES_PER_INT );
            cycles += Z80Interrupt (&state, siob.registers[2] | 0x0c);   // ax25 char read int
            total += cycles;
          }

          if(ax25rdy)
          {
            while(!state.iff1) cycles = Z80Emulate(&state, CYCLES_PER_INT );
            cycles += Z80Interrupt (&state, siob.registers[2] | 0x0e); // eof int
            total += cycles;
          }
        }
        else
        {
          if(txundr_count)
          {
            txundr_count--;
            if(!txundr_count)
            {
              feedflag = 1; /* txunderrun we can send packet!*/
              if(Ax25_Out_Cnt)
              {
                send_packet(&tnc[0], Ax25_Out, Ax25_Out_Cnt);
                Ax25_Out_Cnt = 0;
              }
            }
          }

          if(feedflag || abortflag )
          {
            while(!state.iff1) cycles = Z80Emulate(&state, CYCLES_PER_INT );
            cycles += Z80Interrupt (&state, siob.registers[2] | 0x0a); // ext stat int
            total += cycles;
          }
          else
          {
            if(siob.registers[1] & 2)
            {
          // while(!state.iff1) total += Z80Emulate(&state, CYCES_PER_INT );
              cycles = Z80Interrupt (&state, siob.registers[2] );
              total += cycles;
            }
          }
        }
      }
      else /* flip */
      {
        if( tty_peek(&tty[0]) || tty_peek(&tty[1] ) )
        {
// This breaks inital autobaud!   if(state.iff1 && (siob.registers[1] & 0x18) )
//      {
          total += Z80Interrupt (&state, siob.registers[2] | 4);
//      }
          tp->active_timeout = DEFAULT_ACTIVITY_COUNT;
        } 
        else total += Z80Interrupt (&state, siob.registers[2] | 8 );
      }

      if(ax25_InQ_HasData() && !RxCharIn_Idx && !ax25rdy && !txundr_count  && !Ax25_In_Dly ) /* do we have a socket */
      {
        RxCharIn_Idx = 1; /* Let everyone know */
        Ax25_In_Dly = 75; /* this is an arbitrary delay amount so emulator can process rx packets */
      }

      if(Ax25_In_Dly && !RxCharIn_Idx && !txundr_count) Ax25_In_Dly--;
    } /* end if sio int */

    if(oldptt != (sioa.registers[5] & 2))
    {
      oldptt = sioa.registers[5] & 2;
#ifdef TNCEMUDEBUG
      printf("ptt=%x\n",oldptt);
#endif
      if(oldptt == 2)
      {
        txundr_count=10;
      }
    }

    /* Here check status of tnc buffers and if work to do set activity */
    if(RxCharIn_Idx > 0 || Ax25_Out_Cnt > 0 )
    {
      //printf("%d-%d\n",RxCharIn_Idx,Ax25_Out_Cnt);
      tp->active_timeout = DEFAULT_ACTIVITY_COUNT;
    }

    /* If activity timer set decrement until 0 */
    if(tp->active_timeout > 0)
    {
      tp->active_timeout--;
#ifdef TNC_EMULATING_LED_PIN
      gpio_put(TNC_EMULATING_LED_PIN,1);
#endif
    }

#ifdef TNCEMUDEBUG
  if (state.status & FLAG_STOP_EMULATION) 
  {
    printf("\n%.0f cycle(s) emulated.\n" 
    "For a Z80 running at %.2fMHz, "
    "that would be %d second(s) or %.2f hour(s).\n",
    total,
    Z80_CPU_SPEED / 1000000.0,
    (int) (total / Z80_CPU_SPEED),
    total / ((double) 3600 * Z80_CPU_SPEED));
  }
#endif

#ifdef TNC_EMULATING_LED_PIN
    if(tp->active_timeout == 0) gpio_put(TNC_EMULATING_LED_PIN,0);
#endif
}

/*************************************************************************
  Here we emulate as best we can the I/O of the Toshiba TMPZ84C015 CPU
  Integrated CPU/IO I/C. The following is the portmap of the i/o

  Internal Halt Mode Setting Registers
  This controls how the ic internal osc operates during certain cpu 
  halt modes. Also watchdog is in hee. Probably can ignore since we are emulating.

  Halt Mode Settings Register 0xF0 
  Code writes an 0x7B Putting All devices in Powered up Run Mode.

  Halt Mode Control Register 0XF1
  Code writes an 0xB1 to disable watchdog timer since wdog enable bit in 0xF0 is off

  Interrupt Priority Register 0xF4 Bits 1,2,0 as follows
  HIGH  TO  LOWEST PRIORITY
  CTC - SIO - PIO 0 0 0 
  SIO - CTC - PIO 0 0 1 (THIS IS THE ONE THE CODE SELECTS)
  CTC - PIO - SIO 0 1 0
  PIO - SIO - CTC 0 1 1
  PIO - CTC - SIO 1 0 0
  SIO - PIO - CTC 1 0 1
  
  CTC Timer I/O Map
  0x10 = Chan 0
  0x11 = Chan 1
  0x12 = Chan 2
  0x13 = Chan 3

  SIO Serial Device I/O Map 
  0x18 = Chan A Data
  0x19 = Chan A Command
  0x1A = Chan B Data
  0x1B = Chan B Command

  PIO I/O Map
  0x1C = Port A Data
  0x1D = Port A Command
  0x1E = Port B Data
  0x1F = Port B Command

*/

int IO_in (int port)
{
  int x=0;
  port &= 255;

  switch (port)
  {
    case 0x10: // CTC Chan0
      break;

    case 0x11: // CTC Chan1
      break;

    case 0x12: // CTC Chan2
      break;

    case 0x13: // CTC Chan3
      break;

    case 0x18: // SIOA Data
      x=0xff;
      if(RxCharIn_Idx) 
      {
        x = Ax25_In_Q[Ax25_In_Tail].data[RxCharIn_Idx-1];
//printf("%x\n",x);
        RxCharIn_Idx++;
        if(--Ax25_In_Q[Ax25_In_Tail].count == 0) 
        {
          RxCharIn_Idx = 0;
          ax25_InQ_Remove();
          ax25rdy=1;
        }
      }
      break;

    case 0x19: // SIOA Cmd
      x = SIO_Cmd_Read( &sioa );
      break;

    case 0x1A: // SIOB Data
      if( !tty_getch(&tty[0], &x) )
      {
        if( !tty_getch(&tty[1], &x) )
        {
          x = 0xff;
        }
      }
      /* some key translations are they needed? */
      if(x == 0x0a) x=0x0d;
      if(x == 0x7f) x=0x08;
#ifdef TNCEMUDEBUG
      if(x == '&') RxCharIn_Idx=1; // Trigger to inject test ax25 packet
#endif
      break;

    case 0x1B: // SIOB Cmd
      x = SIO_Cmd_Read( &siob );
      break;

    case 0x1C: // PIOA Data
      break;

    case 0x1D: // PIOA Cmd
      break;

    case 0x1E: // PIOB Data
      break;

    case 0x1F: // PIOB Cmd
      break;

    default:  // All else do nothing
      break;

  }

//    printf("IO In from port %x = %x:%x\n",port,x,state.pc);
  return (x);
}

void IO_out (int port, int x)
{
  port &= 255;

 // printf("IO out %x to port %x\n",x,port);

  switch (port)
  {
    case 0x10: // CTC Chan0
      break;

    case 0x11: // CTC Chan1
      break;

    case 0x12: // CTC Chan2
      break;

    case 0x13: // CTC Chan3
      break;

    case 0x18: // SIOA Data
      Ax25_Out[Ax25_Out_Cnt++] = x;
      txundr_count=10; /* reset tx underrun */
      break;

    case 0x19: // SIOA Cmd
      SIO_Cmd_Write( &sioa, x);
      break;

    case 0x1A: // SIOB Data
      tty_write_char(&tty[0], x);
      tty_write_char(&tty[1], x);
      tnc[0].active_timeout = DEFAULT_ACTIVITY_COUNT;
      break;

    case 0x1B: // SIOB Cmd
      SIO_Cmd_Write( &siob, x);
      break;

    case 0x1C: // PIOA Data
      break;

    case 0x1D: // PIOA Cmd
      break;

    case 0x1E: // PIOB Data
      break;

    case 0x1F: // PIOB Cmd
      break;

    default:  // All else do nothing
    break;

  }


}

/* SIO functions are here to handle emulation of SIO Channels */

/* Reset SIO registers and cmd ptr */
void SIO_Reset( IC_SIO *sio )
{
  sio->state = 0; // Set state for cmd reg
  sio->cmd_ptr = 0; // Set cmd ptr to reg 0
  sio->registers[0] = 0; 
}

/* Handle Writes to SIO Command Port */
void SIO_Cmd_Write( IC_SIO *sio, unsigned char x)
{
  if(sio->state) /* write to actual reg */
  {
//    if(abortflag && sio->cmd_ptr == 5 && !(x & 2)) 
//      printf("PC=%x\n",state.pc);

    sio->registers[sio->cmd_ptr] = x; 
    sio->cmd_ptr = 0; /* after a write it sets back to 0 */
    sio->state = 0; /* next state is command */
  }
  else /* set write register */
  {
    if(x & 0x20) feedflag=0;
    if(x == 8)  abortflag=1;/* Abort Seq SDLC */
    if(x == 0x18 ) /* handle special reset case */
    {
      sio->cmd_ptr = 0; /* after a write it sets back to 0 */
      sio->state = 0; /* next state is command */
    }
    else 
    {
      if(!(x & 0x38)) 
      {
        sio->cmd_ptr = x & 0x07; /* lsb 3 bits select reg for next write/read */
        sio->state = 1; /* flip state */
      }
    }
  }

}

/* Handle Reads from SIO Command Port */
int SIO_Cmd_Read( IC_SIO *sio )
{

int val = 0;

  switch ( sio->cmd_ptr )
  {
    case 0: /* Reg Indicates the rx/tx buffer state & pins state */
            /* MSB -> BRK/ABORT, UNDRRUN, CTS, SYNC/HUNT, DCD, TBUF_EMPTY
               INT_PENDING, RX_CHAR_RDY <-LSB */
      if(sio == &siob )
      {
        val = 0x2c; /* set CTS, DCD, TBUF_EMPTY always */
        if(tty_peek(&tty[0])) val |=1; /* if keys in buffer set flag we have rx chars */  
        if(tty_peek(&tty[1])) val |=1; /* if keys in buffer set flag we have rx chars */  
      }
      else /* handle sioa */
      {
        val = val | 4; /* TBUF_EMPTY */
        if( sio->registers[5] & 2 ) val |= 0x20; /* cts is wired to rts so it follows it */
        if(abortflag) 
        {
          val |= 0x10; /* set Sync/Hunt */
          val &= 0xFB; /* Clear TFBUF_EMPTY emulating crc going out in uart */
          if(!(val & 0x20)) abortflag=0;
        }
        if( RxCharIn_Idx ) val |= 0x09; /* DCD & RX_CHAR_RDY */
        if(feedflag) val |= 0x40; 
      }
      break;
     
    case 1: /* Reg Indicates error status and end of frame code */
            /* MSB -> EOF_FRAM, FRAME_ERR, RX_OVRRUN, PARITY_ERROR
               NONE, FRACTION, NONE, TX_EMPTY or always 1 in SYNC MODE */
      if(sio == &siob )
      {
        val = 0x01	; /* TX Empty always! */
      }
      else /* chan a */
      {
        val= 0x01;
        if(ax25rdy)
        {
          val |= 0x86; /* set eof detected and correct fraction bits! */
          ax25rdy = 0;
          /* If input queue has more data retrigger to process next packet */
          /* scratch that, this is done abocve after a delay period now.*/
          //if(Ax25_In_HasData()) RxCharIn_Idx=1;
        }
      }

      break;

    case 2: /* Returns int vector but only for port b but we do both */
      if(sio == &siob )
        {
          val = sio->registers[2];
        } else val=0; /* no int vec on chan a! */

      break;

    default:
      break;

  }

  sio->state = 0;
  sio->cmd_ptr = 0;
//  sio->state = sio->state ^ 0x01; /* flip state */

  return val;
}

/* Memory Access Functions go here */

unsigned int Memory_Read_Byte(unsigned int address)
{
  if(address > 0x7FFF) return Ram[address & 0x7fff];
  else return Rom[address];
}

unsigned int Memory_Read_Word(unsigned int address)
{
  if(address > 0x7FFF) 
    return Ram[address & 0x7fff] | ( Ram[ (address+1) & 0x7fff ] << 8 );
  else
    return Rom[address] | ( Rom[ address+1 ] << 8 );
}

void Memory_Write_Byte(unsigned int address, unsigned int data)
{
  if(address > 0x7FFF) Ram[address & 0x7fff] = data & 0xff;
}

void Memory_Write_Word(unsigned int address, unsigned int data)
{
  if(address > 0x7FFF) 
  {
    Ram[address & 0x7fff] = data & 0xff; 
    if( ((address+1) & 0xffff) > 0x7fff)                                	
    Ram[(address + 1) & 0x7fff] = data >> 8; 
  }
}

// Convert int to bcd 
char tobcd(unsigned int val)
{
    if (val < 0 || val > 99) {
        return 0; // out of range
    }
    return (char)(((val / 10) << 4) | (val % 10));
}

// Convert from BCD
char frombcd(unsigned int bcd)
{
  return (char)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

void RewriteBbsMsg(int addr, char *txt )
{
  int x;

  for(x=0; x<22; x++)
  {
    Rom[addr+x] = 0x20; // space char
  }

  Rom[addr+12] = 0; // Plant terminator

  for(x=0; x<12; x++)
  {
    if(*txt == 0) break;
    Rom[addr+x] = *txt++;
  }

  if(*txt == 0) return;

  for(x=13; x<22; x++)
  {
    if(*txt == 0) break;
    Rom[addr+x] = *txt++;
  }
}

unsigned int GetNextBbsMsgNo(void)
{
  int msg = 0;
  msg = Ram[bbsmsg_address] + Ram[bbsmsg_address+1] * 256;
  return msg;
}
