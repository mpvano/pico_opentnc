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

#include "tnc.h"
#include "z80emu.h"

#include "ax25.h"
#include "flash.h"

uint32_t __tnc_time;

tnc_t tnc[PORT_N];

double  cycles;
double  total;
double  timer_int,sio_int;

/* tnc emulator */
unsigned int key;
unsigned char keybuf[16];
unsigned char keyhead = 0;
unsigned char keytail = 0;
unsigned char flop,oldptt;
unsigned char RxCharIn_Idx=0;
unsigned char ax25rdy=0;
unsigned char feedflag=0;
unsigned char abortflag=0;
unsigned char txundr_count=0;
int activity,activity2;
unsigned short int mycrc;
int rxcnt;

/* Output Buffer */
unsigned char Ax25_Out[BUFLEN];
unsigned int  Ax25_In_Cnt, Ax25_Out_Cnt;

/* Input Queue to stack multiple incoming packets */
struct inQueue       Ax25_In_Q[AX25_IN_MAXSIZE];
unsigned int  Ax25_In_Head = 0;
unsigned int  Ax25_In_Tail = 0;
unsigned int  Ax25_In_Dly = 0;


param_t param = {
    .mycall = { 0, 0, },
    .unproto = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, },
    .myalias = { 0, 0 },
    .btext = "",
    .txdelay = 100,
    .echo = 1,
    .gps = 0,
    .trace = 0,
    .mon = 0,
    .digi = 0,
    .beacon = 0,
};

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
    for (int i = 0; i < PORT_N; i++) {
        tnc_t *tp = &tnc[i];

        // receive
        tp->port = i;
        tp->state = FLAG;
        filter_init(&tp->lpf, lpf_an, FIR_LPF_N);
        filter_init(&tp->bpf, bpf_an, FIR_BPF_N);

        // send queue
        queue_init(&tp->send_queue, sizeof(uint8_t), SEND_QUEUE_LEN);
        tp->send_state = SP_IDLE;

        tp->cdt = 0;
        tp->kiss_txdelay = 50;
        tp->kiss_p = 63;
        tp->kiss_slottime = 10;
        tp->kiss_fullduplex = 0;

        // calibrate
        tp->do_nrzi = true;
    }

    //printf("%d ports support\n", PORT_N);
    //printf("DELAYED_N = %d\n", DELAYED_N);

    // read flash
    flash_read(&param, sizeof(param));

    // set kiss txdelay
    if (param.txdelay > 0) {
        tnc[0].kiss_txdelay = param.txdelay * 2 / 3;
    }

    /* for now clear ram */
    for(int x=0; x<sizeof(Ram); x++)
    {
        Ram[x]=0;
    }

    /* Reset emulated SIO state machines */
    SIO_Reset(&sioa); /* Reset Emulated Serial i/o a */
    SIO_Reset(&siob); /* Reset Emulated Serial i/o b */
	sio_int = 0;

    /* Reset z80 emulator */
    Z80Reset(&state);

    /* Init z80 cycle time counters */
    total = timer_int = sio_int =  0.0;

    /* Init some activity timers that control sleep */
    activity = 0;
    activity2 = 0;

}

/* Run some cycles of emulated tnc */
void tnc_emulate(void)
{
#ifdef TNCEMUDEBUG
    printf("PC=%x cycles=%.0f\n",state.pc,total);
    cycles = Z80Emulate(&state, 1);
#endif
    cycles = Z80Emulate(&state, CYCLES_PER_STEP);
    total += cycles;
    timer_int += cycles;
    sio_int += cycles;

/* Every so many cycles do a timer interrupt, highly inacurate but it
doesn't matter since we don't rely on it anymore. This could be done
better but for now it works */
    if( timer_int > 2750200 )
    {
      timer_int = 0;
      total += Z80Interrupt (&state, 0x10 );

      /* here update tnc time with our time if we can */
    //   if( clock_address > 0 )
    //   {
    //     time(&rawtime);
    //     timeinfo = localtime (&rawtime);
    //     x= timeinfo->tm_sec;
    //     Ram[clock_address] = tobcd(x);
    //     x= timeinfo->tm_min;
    //     Ram[clock_address+1] = tobcd(x);
    //     x= timeinfo->tm_hour;
    //     Ram[clock_address+2] = tobcd(x);
    //     x= timeinfo->tm_mday;
    //     Ram[clock_address+3] = tobcd(x);
    //     x= timeinfo->tm_mon;
    //     Ram[clock_address+4] = tobcd(x+1);
    //     x= timeinfo->tm_year;
    //     x= x - ((x / 100) * 100);
    //     Ram[clock_address+5] = tobcd(x);
    //   }

      /* Check if any new bbs msgs have arrived and if so save ram to disk */
    //   if( PrevbbsMsgNo != GetNextBbsMsgNo())
    //   {
    //       PrevbbsMsgNo = GetNextBbsMsgNo();
    //       WriteRamfile();
    //   }
    }

/* These values may need to be adjusted depending on the speed of 
the machine you will be emulating on. */
#ifdef SPEED_PI
    if( sio_int > 115004 ) /* on pi3 */
#else
    if( sio_int > 215004 ) /* on fast x86 */
#endif
    {
      flop = flop ^0x01;
      sio_int = 0;
      if(flop)
      {

        if(RxCharIn_Idx || ax25rdy)
        {
          if(RxCharIn_Idx)
          {
            while(!state.iff1) cycles = Z80Emulate(&state, CYCLES_PER_INT );
            cycles += Z80Interrupt (&state, siob.registers[2] | 0x0c);   // ax25 char read int
            total += cycles;
            sio_int += cycles;
          }

          if(ax25rdy)
          {
            while(!state.iff1) cycles = Z80Emulate(&state, CYCLES_PER_INT );
            cycles += Z80Interrupt (&state, siob.registers[2] | 0x0e); // eof int
            total += cycles;
            sio_int += cycles;
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
            //     mycrc = compute_crc(Ax25_Out, Ax25_Out_Cnt);
            //     Ax25_Out[Ax25_Out_Cnt++] = mycrc & 0xFF;
            //     Ax25_Out[Ax25_Out_Cnt++] = mycrc >> 8;

            //     if ((x = sendto(sock_out, Ax25_Out, Ax25_Out_Cnt, 0,
            //         servinfo->ai_addr, servinfo->ai_addrlen)) == -1)
            //       die("Socket Xmit Error!\n");
               }
            }
          }

          if(feedflag || abortflag )
          {
            while(!state.iff1) cycles = Z80Emulate(&state, CYCLES_PER_INT );
            cycles += Z80Interrupt (&state, siob.registers[2] | 0x0a); // ext stat int
            total += cycles;
            sio_int += cycles;
          }
          else
          {
            if(siob.registers[1] & 2)
            {
          // while(!state.iff1) total += Z80Emulate(&state, CYCES_PER_INT );
              cycles = Z80Interrupt (&state, siob.registers[2] );
              total += cycles;
              sio_int += cycles;
            }
          }
        }
      }
      else /* flip */
      {
        if(keyhead != keytail)
        {
// This breaks inital autobaud!   if(state.iff1 && (siob.registers[1] & 0x18) )
//      {
          total += Z80Interrupt (&state, siob.registers[2] | 4);
//      }
        } 
        else total += Z80Interrupt (&state, siob.registers[2] | 8 );
      }

      if(Ax25_In_HasData() && !RxCharIn_Idx && !ax25rdy && !txundr_count  && !Ax25_In_Dly ) /* do we have a socket */
      {
        RxCharIn_Idx = 1; /* Let everyone know */
        Ax25_In_Dly = 75; /* this is an arbitrary delay amount so emulator can process rx packets */
      }

      if(Ax25_In_Dly && !RxCharIn_Idx && !txundr_count) Ax25_In_Dly--;

    /* here check if we are in the middle of processing the previous 
       socket by checking the RxCharIn_Idx & ax25rdy flags! */
      if(Ax25_In_HasRoom()) /* do we have a socket and room in Ax25 input Queue */
      { 
        /* Here we check if there is any incoming data */
        if (0/*Incoming data from modem */)
        {
          activity2 = 3000; /* we have activity so set counter for sleep algo */
          for(int x=0; x < rxcnt; x++) 
          Ax25_In_Q[Ax25_In_Head].data[x] = 0; /*Socket_Data_In[x]; */

          //mycrc = compute_crc(Ax25_In_Q[Ax25_In_Head].data, rxcnt-2);
#ifdef TNCEMUDEBUG
          printf("CRC=%2x RxCRC=%x%x head=%d tail=%d\n",mycrc,Ax25_In_Q[Ax25_In_Head].data[rxcnt-1],Ax25_In_Q[Ax25_In_Head].data[rxcnt-2],
          Ax25_In_Head, Ax25_In_Tail);
#endif
          if( mycrc == ( (Ax25_In_Q[Ax25_In_Head].data[rxcnt-1] << 8) + Ax25_In_Q[Ax25_In_Head].data[rxcnt-2] ) ) /* crc check */
          {
          Ax25_In_Q[Ax25_In_Head].count = rxcnt-1; /* adjust to get total # of bytes in buffer */
          Ax25_In_Insert();
          }
        }
      } /* end if socket active */
    } /* end if sio int */

    // here check and handle console keyboard input
    if(kbhit())
    {
      activity2 = 100;
      key=getchar();
      if(key == 0x0a) key=0x0d;
      if(key == 0x7f) key=0x08;
#ifdef TNCEMUDEBUG
      if(key == '&') RxCharIn_Idx=1; // Trigger to inject test ax25 packet
      else
      {
#endif
        // add to key buffer
        keybuf[keyhead] = key;
        keyhead++;
        keyhead &= 0x0f;
#ifdef TNCEMUDEBUG
        printf("got key %c\n",(char) key);
      }
#endif
    }

    if(oldptt != (sioa.registers[5] & 2))
    {
      oldptt = sioa.registers[5] & 2;
#ifdef TNCEMUDEBUG
      printf("ptt=%x\n",oldptt);
#endif
      if(oldptt == 2)
      {
        txundr_count=10; 
        Ax25_Out_Cnt=0;
      }
    }

/* Here check activity and if none sleep so we're not a cpu hog */
    if(Ax25_In_HasData() || RxCharIn_Idx || ax25rdy || feedflag || abortflag ) activity = 0;
    if(txundr_count ) activity = 0;
    if(keyhead != keytail) activity = 0;

    //if(activity > 10000) usleep(10000);
/*                else activity++; */
/* Enable to stop even more cpu use*/
    else
    { 
      //if(activity > 1) usleep(2000);
      if(activity2) activity2--;
        else activity++;
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
        activity = 0;
        x = Ax25_In_Q[Ax25_In_Tail].data[RxCharIn_Idx-1];
//printf("%x\n",x);
        RxCharIn_Idx++;
        if(--Ax25_In_Q[Ax25_In_Tail].count == 0) 
        {
          RxCharIn_Idx = 0;
          Ax25_In_Remove();
          ax25rdy=1;
        }
      }
      break;

    case 0x19: // SIOA Cmd
      x = SIO_Cmd_Read( &sioa );
      break;

    case 0x1A: // SIOB Data
      x=0xff;
      if(keyhead != keytail)
      {
        x=keybuf[keytail];
        keytail++;
        keytail &= 0x0f;
      }
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
      printf("%c",x);
      activity = 0;
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
        if(keyhead != keytail) val |=1; /* if keys in buffer set flag we have rx chars */  
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

int kbhit()
{

}

// Convert int to bcd 
char tobcd(unsigned int val)
{

char result;

  val &= 0xFF; // only doing 2 digits

  result = val /10;
  result <<= 4;
  result |= val % 10;
  return result;
}

bool Ax25_In_HasRoom(void)
{
  int next = Ax25_In_Head + 1;
  if(next >= AX25_IN_MAXSIZE) 
    next = 0;

  if(next == Ax25_In_Tail)
    return false;
  else
    return true;
}

void Ax25_In_Insert(void)
{
  if(++Ax25_In_Head >= AX25_IN_MAXSIZE) 
    Ax25_In_Head = 0;
}

void Ax25_In_Remove(void)
{
  if(++Ax25_In_Tail >= AX25_IN_MAXSIZE) 
    Ax25_In_Tail = 0;
}

bool Ax25_In_HasData(void)
{
  if(Ax25_In_Head != Ax25_In_Tail)
    return(true);
  else
    return(false);
}

