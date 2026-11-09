# Medicine Dispenser

Senior Design Project

An automated medicine dispenser controlled by an
Arduino microcontroller.

## HM-10 Bluetooth test

The sketch targets an Arduino Uno R3 and the DSD TECH HM-10 four-pin
base board. It uses SoftwareSerial (included with the Arduino AVR core).
The other sketch libraries are still required: Keypad, LiquidCrystal_I2C,
Adafruit PCF8574 and its dependencies, Servo, Stepper, and Wire.

### Wiring (with power disconnected)

| HM-10 pin | Arduino connection |
| --- | --- |
| VCC | 5 V only if the base board is labeled 3.6-6 V |
| GND | GND, shared with the Arduino and other supplies |
| TXD | D12 (Arduino receives) |
| RXD | D13 through the voltage divider below |

Do not connect the Uno's 5 V transmit signal directly to HM-10 RXD.
The bare HM-10 is a 3.3 V device; the base board's supply rating does not
establish 5 V tolerance for its serial input.

```text
Uno D13 --- 1 kOhm ---+--- HM-10 RXD
                     |
                   2 kOhm
                     |
                    GND
```

### Test procedure

1. Upload the sketch for Uno R3. If Arduino IDE requests a matching sketch
   folder, use `AutoDrugDispencer/AutoDrugDispencer.ino`.
2. Open Serial Monitor at **115200 baud**, with **No line ending**.
3. Send `6`, or press `6` on the keypad, to enter the Bluetooth bridge.
   Wait for the bridge instructions before sending another message.
4. With the phone disconnected, send `AT`. The expected reply is `OK`.
   Send `AT+VERS?` to check firmware, or `AT+NAME?` to check the module name.
   Send each command separately and wait for its response.
5. Connect using a BLE app such as LightBlue. With default HM-10 settings,
   select service FFE0, characteristic FFE1, and enable notifications.
   Write `hello` as text to FFE1; it should appear in Serial Monitor.
   Send `hello phone` from Serial Monitor and check the app's notifications.
6. Send `~` from Serial Monitor, or press `#` on the physical keypad, to
   return to the component menu. USB `~` is reserved and is not forwarded.
   A `#` sent through USB is ordinary data.

The module UART uses `BLUETOOTH_BAUD` (9600 by default); the USB Serial
Monitor uses 115200. Initializing SoftwareSerial does not change the
module's saved baud rate. If `AT` gets no response, check TX/RX wiring,
common ground, power, line endings, phone disconnection, and the module's
configured baud rate. The sketch does not automatically send AT commands
or change the module's persistent settings.

This is a manual bridge for short messages, active only in menu option 6.
It forwards bytes rather than interpreting dispensing commands. Exit back
to the menu before running other component tests. Those tests can still
block, so this is not yet an always-on Bluetooth interface. A receive-buffer
overflow prints a diagnostic; SoftwareSerial cannot transmit and receive
simultaneously, so send messages in one direction at a time.

The PCF8574 is optional for this test: failure to detect it prints a message
and disables PCF access instead of halting startup. When present, its button
and output are serviced every 100 ms while idle or in Bluetooth mode.

References:

- [DSD TECH four-pin HM-10 board](https://www.deshide.com/product-details_HM-10-1.html)
- [HM-10 datasheet](https://people.ece.cornell.edu/land/courses/ece4760/PIC32/uart/HM10/DSD%20TECH%20HM-10%20datasheet.pdf)
- [Arduino SoftwareSerial](https://docs.arduino.cc/learn/built-in-libraries/software-serial/)
