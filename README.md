# PICO TNC

PICO TNC is the Terminal Node Controler for Amateur Packet Radio powered by Raspberry Pi Pico. This is a fork of that project. I have removed all of the major features of the code except for the modulator/demodulator section and inserted a z80 emulator from my TNCEMU project that emulates a Heathkit HK21 Pocket Packet. This is still expermental for testing.

## PIC TNC features

- Encode and decode Bell 202 AFSK signal without modem chip
- Support both USB serial and UART serial interface
- Emulated Support of full tnc and tnc commands including pbbs
- Kiss support at the flip of a switch.

## Todo
- Monitor Emulated TNC SIO uart registers for baud rate changes and update PICO serial port baud accordingly.
- Add 1600 1800 hz modem for HF Modem support.
- Investigate if FX.25 can be added especially for hf.
  
## How to build
```
git clone https://github.com/pfiliberti/pico_tnc.git
cd pico_tnc
mkdir build
cd build
cmake ..
make -j4
(flash 'pico_tnc/pico_tnc.uf2' file to your Pico)
```
![bell202-wave](bell202-wave.png)
![terminal-scrren](command.png)
[![schemantic](schematic.jpg)](schematic.png)
