#pragma once

// Improv WiFi over the USB serial port (https://www.improv-wifi.com/serial/),
// so the web flasher (ESP Web Tools) can offer "Connect to Wi-Fi" straight
// after installing: it lists the networks the board can see, sends the one
// chosen, and links to the configuration page once the board is on it.
//
// Packets share the port with the log. Improv clients look for their
// "IMPROV" header and skip everything else, and a packet goes out in one
// write, so log lines can't land inside it.
//
// Only on boards whose USB port is the chip's USB Serial/JTAG (the Tab5);
// a no-op elsewhere.
void improv_start(void);
