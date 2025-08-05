# PICO TNC

PICO TNC is the Terminal Node Controler for Amateur Packet Radio powered by Raspberry Pi Pico. This is a fork of that project. I intend to remove all of the major features of the code except for the modulator/demodulator section and insert the z80 emulator from my TNCEMU project to see if it is possible to make a fully functional TNC that supports full connections and has pbbs support.

## PIC TNC features

- Encode and decode Bell 202 AFSK signal without modem chip
- Support both USB serial and UART serial interface

## How to build

```
git clone https://github.com/amedes/pico_tnc.git
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
