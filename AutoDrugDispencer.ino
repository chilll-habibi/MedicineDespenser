/* =====================================================================
   EE-490 Project I - Component bring-up test, Arduino UNO R3
   Tests one part at a time from a keypad menu

   MISSING Pins: LEDs.
   Kept: LCD, keypad, speaker, servo, stepper, Bluetooth.

   PIN MAP (Uno)
     LCD 16x2 I2C ....... A4 (SDA), A5 (SCL)   address LCD_ADDR
     Keypad rows ........ A0, A1, A2, A3 (used as digital pins)
     Keypad columns ..... D2, D3, D4, D5
     Servo (lid) ........ D6   (power from separate 5 V supply)
     Speaker/buzzer ..... D7
     Stepper (ULN2003) .. IN1 D8, IN2 D9, IN3 D10, IN4 D11
                          (power from separate 5 V supply)
     HM-10 TXD .......... D12 (Arduino RX)
     HM-10 RXD .......... D13 (Arduino TX, through 5 V -> 3.3 V divider)
     D0/D1 stay free for USB. HM-10 and Arduino grounds must be common.
   All grounds (Arduino, servo supply, stepper supply) must be common.

   LIBRARIES (Library Manager): Keypad (Stanly & Brevig), LiquidCrystal (Brabander),
     Adafruit PCF8574; Servo, Stepper, Wire, SoftwareSerial come with the IDE.

   TESTING MENU:  1 LCD   2 Keypad   3 Speaker   4 Servo   5 Stepper
                 6 Bluetooth (USB serial bridge; ~ or keypad # exits)
   ===================================================================== */

#include <Wire.h>
#include <Servo.h>
#include <Stepper.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_PCF8574.h>
#include <SoftwareSerial.h>

// ------------------------------ configuration ------------------------
#define LCD_ADDR          0x27     // try 0x3F if the screen stays blank
#define STEPS_PER_REV     2048     // 28BYJ-48 in full-step mode
#define STEPS_PER_SLOT    (STEPS_PER_REV / 4)
#define LID_CLOSED_ANGLE  0
#define LID_OPEN_ANGLE    90
#define BLUETOOTH_BAUD    9600    // must match the HM-10's stored UART baud rate

// ------------------------------ pins ---------------------------------
const uint8_t PIN_SERVO   = 6;
const uint8_t PIN_SPEAKER = 7;
const uint8_t STEP_PINS[4] = {8, 9, 10, 11};
const uint8_t PIN_BT_RX = 12;     // connected to HM-10 TXD
const uint8_t PIN_BT_TX = 13;     // connected to HM-10 RXD through divider

// ------------------------------ hardware objects ---------------------
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Servo lid;
Stepper carousel(STEPS_PER_REV, 8, 10, 9, 11);   // IN1, IN3, IN2, IN4
SoftwareSerial bluetooth(PIN_BT_RX, PIN_BT_TX);

const byte ROWS = 4, COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {A0, A1, A2, A3};
byte colPins[COLS] = {2, 3, 4, 5};
Keypad keypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS);

Adafruit_PCF8574 pcf8574;
bool pcfReady = false;
bool bluetoothTestActive = false;
// ====================================================================
//                            helpers
// ====================================================================
void show(const __FlashStringHelper* l1, const __FlashStringHelper* l2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);  
}

void lcdText(uint8_t row, const char* s) {
  char b[17];
  snprintf(b, sizeof(b), "%-16s", s);
  lcd.setCursor(0, row);
  lcd.print(b);
}

void printMenu() {
  Serial.println(F("\n1 LCD | 2 Keypad | 3 Speaker | 4 Servo | 5 Stepper | 6 Bluetooth"));
}

// Poll once so other work can continue while the menu is idle.
char readMenuKey() {
  char k = keypad.getKey();
  if (k) return k;
  if (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') return c;
  }
  return 0;
}

void servicePCF() {
  static uint32_t lastPoll = 0;
  if (!pcfReady || millis() - lastPoll < 100UL) return;
  lastPoll = millis();
  bool buttonState = pcf8574.digitalRead(0);
  pcf8574.digitalWrite(7, !buttonState);
}

uint8_t countBits(uint16_t v) {
  uint8_t c = 0;
  while (v) { c += v & 1; v >>= 1; }
  return c;
}

int keyIndex(char k) {
  for (uint8_t r = 0; r < ROWS; r++)
    for (uint8_t c = 0; c < COLS; c++)
      if (keyMap[r][c] == k) return r * COLS + c;
  return -1;
}

void releaseStepper() {
}

// ====================================================================
//                              TESTS
// ====================================================================
// 1) LCD: full-width pattern + backlight blink
void testLCD() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("0123456789ABCDEF"));
  lcd.setCursor(0, 1); lcd.print(F("LCD OK  16x2    "));
  for (uint8_t i = 0; i < 3; i++) {
    lcd.noBacklight(); delay(250);
    lcd.backlight();   delay(250);
  }
  Serial.println(F("LCD: all 32 characters should be visible"));
  delay(1500);
}

// 2) Keypad: press all 16 keys; ends when all seen or after 10 s idle
void testKeypad() {
  uint16_t seen = 0;
  show(F("Press all 16"), F("keys..."));
  uint32_t last = millis();
  while (seen != 0xFFFF && millis() - last<10000UL){
    char k = keypad.getKey();
    if (!k) continue;
    last = millis();
    int idx = keyIndex(k);
    if (idx >= 0) seen |= (1U << idx);
    Serial.print(F("Key: "));
    Serial.println(k);
    char b[17];
    snprintf(b, sizeof(b), "Key %c seen %d/16", k, countBits(seen));
    lcdText(1, b);
  }
  if (seen == 0xFFFF) {
    show(F("ALL 16 KEYS OK"), F(""));
    Serial.println(F("Keypad: all 16 keys OK"));
  } else {
    show(F("KEYPAD TIMEOUT"), F("see Serrial Mon."));
    Serial.print(F("Keypad: never saw:"));
    for (uint8_t i = 0; i < 16; i++)
      if (!((seen >> i) & 1)) {
        Serial.print(' ');
        Serial.print(keyMap[i / 4][i % 4]);
      }
    Serial.println();
  }
  delay(2000);
}

// 3) Speaker: the two category tones used by the dispenser
void testSpeaker() {
}

// 4) Servo (lid): slow sweep closed -> open -> closed
void testServo() {
}

// 5) Stepper (carousel): 1/4 turn forward, 1/4 turn back, then 1 full turn
void testStepper() {
}

// 6) Manual AT commands when disconnected; short text messages when connected.
// No AT commands or persistent configuration changes are sent automatically.
void testBluetooth() {
  bluetooth.listen();
  bluetoothTestActive = true;
  Serial.print(F("\nHM-10 bridge active: module UART at "));
  Serial.println(BLUETOOTH_BAUD);
  Serial.println(F("Serial Monitor: 115200 baud, No line ending."));
  Serial.println(F("Disconnect phone, then send AT; expect OK."));
  Serial.println(F("Connect a BLE app to exchange short text messages."));
  Serial.println(F("Exit: send ~ over USB or press # on the keypad."));
}

void serviceBluetooth() {
  // A keypad escape still works when the module is absent or silent.
  if (keypad.getKey() == '#') {
    bluetoothTestActive = false;
  }
  if (bluetoothTestActive && Serial.available()) {
    char c = Serial.read();
    if (c == '~') bluetoothTestActive = false;
    else bluetooth.write(c);
  }
  while (bluetooth.available()) {
    Serial.write(bluetooth.read());
  }
  if (bluetooth.overflow()) {
    Serial.println(F("\nBluetooth receive overflow: resend a shorter message."));
  }
  if (!bluetoothTestActive) printMenu();
}

// ====================================================================
//                           setup / loop
// ====================================================================
void setup() {
  Serial.begin(115200);
  bluetooth.begin(BLUETOOTH_BAUD);
  Wire.begin();

  pinMode(PIN_SPEAKER, OUTPUT);
  for (uint8_t i = 0; i < 4; i++) pinMode(STEP_PINS[i], OUTPUT);

  while (!Serial) {delay(10);}
  Serial.println("PCF Initialization");
  pcfReady = pcf8574.begin(0x20, &Wire);
  if (pcfReady) {
    Serial.println(F("PCF Initialization successful"));
    pcf8574.pinMode(0, INPUT_PULLUP);
    pcf8574.pinMode(7, OUTPUT);
  } else {
    Serial.println(F("PCF not found; PCF test disabled. Bluetooth remains available."));
  }
  printMenu();
}

void loop() {
  servicePCF();
  if (bluetoothTestActive) {
    serviceBluetooth();
    return;
  }

  char k = readMenuKey();
  switch (k) {
    case '1': testLCD();     break;
    case '2': testKeypad();  break;
    case '3': testSpeaker(); break;
    case '4': testServo();   break;
    case '5': testStepper(); break;
    case '6': testBluetooth(); return;
    default:  break;
  }
  if (k >= '1' && k <= '5') printMenu();
}
