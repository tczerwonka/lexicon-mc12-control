# lexicon-mc12-control

This is a small ESP32 project that connects to a Lexicon MC-12HD serial
port and in addition to displaying the unit's front panel information, 
allows minimal control of the unit functions.

This uses a commonly-available ESP32-C3 mini, RS232 to TTL converter
and DE9 male plug.  It is powered via the ESP32 USB-C port.  Other ESP32
boards will probably work as well.

[ESP32-C3 -- amazon.com/dp/B0D1FX3QGN](https://www.amazon.com/dp/B0D1FX3QGN)

[RS232 to TTL converter -- amazon.com/dp/B00LUDCAXQ](https://www.amazon.com/dp/B00LUDCAXQ)

ESP32 GPIO 21 is data out - this pin connects to the TTL input of the converter
board and the RS232 output goes to pin 3 on the DE9.

ESP32 GPIO 10 is data in - it connects to the TTL output of the converter
board and the RS232 input goes to pin 2 on the DE9.

Ground to pin 5 on the DE9.

The converter board is powered with the ESP32 3.3v and ground pins.  See
picture below for further information.

This has only been tested on my Lexicon MC12-HD.  The code is currently
modified slightly in the input drop-down for the external hardware on my
Lexicon.  I have not tested it on any other models. 
This only has control for the main output, and not Zone 2 or Record.  I
don't use either of those so I did not include them.

There are two fold out menus - for the menu config to set unit parameters
and also for debugging information.  Both can be ignored under normal use.

This uses the tzapu/WiFiManager library to handle the WiFi / SSID setup.
On initial startup, the unit has a "Lexicon" SSID with ip address of
192.168.4.1 - a modern Android phone on initial connection will typically
automatically connect and send you to the UI for connecting to your own
home access point.  The blue LED turns on when WiFi is connected.  The
red LED is power.  Ideally WiFi setup is only done once.

After connection the unit uses mDNS and with a correctly configured network
and PC / phone, your web browser -may- find it at [http://lexicon.local](http://lexicon.local) .  

Additional debugging output is available on the ESP32 serial port, 115200
baud - which includes MAC address, IP address, additional debug output, etc.

![startup](startup.png)
Unit startup.  The Menu pull out has a 'power' button that provides
the ability to turn the unit on, but not off.

![normal operation](running.png)
Un-expanded menus during normal operation.  Here I have the Input
drop-down menus customized for both the front label and what I have
connected.

![menu control](menu.png)
Expanded menu for unit setup.

![debug output](debug.png)
All menus expanded including debugging output.  The "Reset WiFi" button
erases the stored ssid/password information and prompts you to connect 
to a new AP on the next reset.

![raw unit](unit.jpg)
The unpackaged unit - ESP32-C3 and level converter connected to serial
port 1.
