/* =====================================================================
   EE-490 Project I - Component bring-up test, Arduino UNO R3
   ALL parts included: LCD, keypad (via PCF8574), RTC, speaker, LEDs,
   servo, stepper, Bluetooth, and an IR pill sensor.
   Nothing is skipped. Only ONE PCF8574 is needed (it carries the 8-wire
   keypad). Everything else fits on the Uno's own pins.

   PIN MAP
     I2C bus (A4 = SDA, A5 = SCL), shared by three devices:
        LCD backpack ........ 0x27  (LCD_ADDR)
        PCF8574 expander .... 0x20  (KEYPAD_EXP_ADDR)  -> keypad
        DS3231 RTC .......... 0x68
     Expander P0..P3 -> keypad rows 1-4, P4..P7 -> keypad columns 1-4
     D2  Bluetooth RX (from module TX)
     D3  LED green        D4  LED amber        D5  LED red
         (each LED: pin -> 220 ohm -> LED -> GND)
     D6  Servo (lid)      D7  Speaker/buzzer
     D8-D11 Stepper IN1-IN4 (ULN2003)
     D12 Bluetooth TX (to module RX through a 5 V -> 3.3 V divider)
     A0  IR sensor analog output
     Spare: D13, A1, A2, A3
   Servo and stepper run from a separate 5 V supply, common ground.

   ADDRESS WARNING: many PCF8574 breakout boards ship as 0x27, the same
   as the LCD backpack. Bridge the A0/A1/A2 pads to GND on the expander
   board to make it 0x20 (check the scan output at boot).

   LIBRARIES (Library Manager): Keypad, Keypad_I2C (Joe Young),
     LiquidCrystal_I2C (F. Brabander), RTClib (Adafruit).
     Servo, Stepper, SoftwareSerial, Wire come with the IDE.

   MENU (alternates between two pages every 3 s; press a digit any time)
     1 LCD   2 Keypad   3 RTC   4 Speaker   5 LEDs   6 Servo
     7 Stepper   8 IR sensor (calibrate + "pill taken" test)
     9 IR slot scan   0 Bluetooth
   ===================================================================== */

#include <Wire.h>
#include <Servo.h>
#include <Stepper.h>
#include <Keypad.h>
#include <Keypad_I2C.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <SoftwareSerial.h>

// ------------------------------ configuration ------------------------
#define LCD_ADDR          0x27     // try 0x3F if the screen stays blank
#define KEYPAD_EXP_ADDR   0x20     // PCF8574 (PCF8574A boards: 0x38-0x3F)
#define RTC_ADDR          0x68
#define STEPS_PER_REV     2048     // 28BYJ-48 in full-step mode
#define NUM_SLOTS         4
#define STEPS_PER_SLOT    (STEPS_PER_REV / NUM_SLOTS)
#define LID_CLOSED_ANGLE  0
#define LID_OPEN_ANGLE    90
#define IR_MIN_DIFF       60       // min ADC difference, pill vs empty
#define TAKEN_TIMEOUT_MS  15000UL
#define TAKEN_DEBOUNCE_MS 500UL

// ------------------------------ pins ---------------------------------
const uint8_t PIN_BT_RX   = 2;
const uint8_t PIN_BT_TX   = 12;
const uint8_t PIN_LED_G   = 3;
const uint8_t PIN_LED_A   = 4;
const uint8_t PIN_LED_R   = 5;
const uint8_t PIN_SERVO   = 6;
const uint8_t PIN_SPEAKER = 7;
const uint8_t PIN_IR      = A0;
const uint8_t STEP_PINS[4] = {8, 9, 10, 11};

// ------------------------------ hardware objects ---------------------
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
RTC_DS3231 rtc;
Servo lid;
Stepper carousel(STEPS_PER_REV, 8, 10, 9, 11);   // IN1, IN3, IN2, IN4
SoftwareSerial BT(PIN_BT_RX, PIN_BT_TX);

const byte ROWS = 4, COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {0, 1, 2, 3};   // expander pins P0-P3
byte colPins[COLS] = {4, 5, 6, 7};   // expander pins P4-P7
Keypad_I2C keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS,
                  KEYPAD_EXP_ADDR, PCF8574);

// IR calibration (set by test 8, used by test 9)
int  irEmpty = 0;
int  irPill  = 0;
bool irCal   = false;

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

// Non-blocking: returns a key from the keypad or Serial Monitor, else 0.
char pollKey() {
  char k = keypad.getKey();
  if (k) return k;
  if (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') return c;
  }
  return 0;
}

char waitKey() {
  char k;
  while (!(k = pollKey())) { }
  return k;
}

void releaseStepper() {
  for (uint8_t i = 0; i < 4; i++) digitalWrite(STEP_PINS[i], LOW);
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

// Averaged IR reading (0-1023)
int irRead() {
  long s = 0;
  for (uint8_t i = 0; i < 8; i++) { s += analogRead(PIN_IR); delay(2); }
  return (int)(s / 8);
}

// Polarity-agnostic: a reading counts as "pill" if it is closer to the
// calibrated pill value than to the calibrated empty value.
bool irIsPill(int v) {
  return abs(v - irPill) < abs(v - irEmpty);
}

// ====================================================================
//                      I2C scan (runs at power-up)
// ====================================================================
void i2cScan() {
  bool lcdFound = false, expFound = false, rtcFound = false;
  uint8_t n = 0;
  Serial.println(F("I2C scan:"));
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x"));
      Serial.println(a, HEX);
      if (a == LCD_ADDR)        lcdFound = true;
      if (a == KEYPAD_EXP_ADDR) expFound = true;
      if (a == RTC_ADDR)        rtcFound = true;
      n++;
    }
  }
  if (!lcdFound) Serial.println(F("  LCD not at LCD_ADDR - try 0x3F"));
  if (!expFound) Serial.println(F("  Expander not at KEYPAD_EXP_ADDR (address clash with LCD?)"));
  if (!rtcFound) Serial.println(F("  RTC (0x68) not found"));
  // (0x57 is the RTC board's own EEPROM - normal.)

  char b[17];
  snprintf(b, sizeof(b), "I2C devices: %d", n);
  lcdText(0, b);
  snprintf(b, sizeof(b), "L:%c K:%c R:%c", lcdFound ? 'Y' : 'N',
           expFound ? 'Y' : 'N', rtcFound ? 'Y' : 'N');
  lcdText(1, b);
  delay(2500);
}

// ====================================================================
//                              TESTS
// ====================================================================
// 1) LCD
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

// 2) Keypad (through the PCF8574)
void testKeypad() {
  uint16_t seen = 0;
  show(F("Press all 16"), F("keys..."));
  uint32_t last = millis();
  while (seen != 0xFFFF && millis() - last < 10000UL) {
    char k = keypad.getKey();
    if (!k) continue;
    last = millis();
    int idx = keyIndex(k);
    if (idx >= 0) seen |= (1U << idx);
    Serial.print(F("Key: "));
    Serial.println(k);
    char b[17];
    snprintf(b, sizeof(b), "Key %c  seen %d/16", k, countBits(seen));
    lcdText(1, b);
  }
  if (seen == 0xFFFF) {
    show(F("ALL 16 KEYS OK"), F(""));
    Serial.println(F("Keypad: all 16 keys OK"));
  } else {
    show(F("KEYPAD TIMEOUT"), F("see Serial Mon."));
    Serial.print(F("Keypad: never saw:"));
    for (uint8_t i = 0; i < 16; i++)
      if (!((seen >> i) & 1)) { Serial.print(' '); Serial.print(keyMap[i / 4][i % 4]); }
    Serial.println();
  }
  delay(2000);
}

// 3) RTC
void testRTC() {
  if (!rtc.begin()) {
    show(F("RTC NOT FOUND"), F("Check SDA/SCL"));
    Serial.println(F("RTC: not found"));
    waitKey();
    return;
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));   // set to build time
    show(F("RTC was unset"), F("set to build time"));
    Serial.println(F("RTC: was unset, set to compile time"));
    delay(1500);
  }
  show(F("RTC running"), F(""));
  while (!pollKey()) {
    DateTime n = rtc.now();
    char b[17], t[8];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", n.year(), n.month(), n.day());
    lcdText(0, b);
    dtostrf(rtc.getTemperature(), 4, 1, t);
    snprintf(b, sizeof(b), "%02d:%02d:%02d %sC", n.hour(), n.minute(), n.second(), t);
    lcdText(1, b);
    delay(250);
  }
}

// 4) Speaker
void testSpeaker() {
  show(F("Cat A: 1 beep"), F("1500 Hz"));
  tone(PIN_SPEAKER, 1500, 200);
  delay(1500);
  show(F("Cat B: 2 beeps"), F("2200 Hz"));
  tone(PIN_SPEAKER, 2200, 200);
  delay(400);
  tone(PIN_SPEAKER, 2200, 200);
  delay(1500);
  show(F("Heard both?"), F("Any key = menu"));
  waitKey();
}

// 5) LEDs
void testLEDs() {
  const uint8_t leds[3] = {PIN_LED_G, PIN_LED_A, PIN_LED_R};
  show(F("LED test"), F("G -> A -> R"));
  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(leds[i], HIGH);
    delay(700);
    digitalWrite(leds[i], LOW);
  }
  show(F("LED test"), F("All blink x3"));
  for (uint8_t n = 0; n < 3; n++) {
    for (uint8_t i = 0; i < 3; i++) digitalWrite(leds[i], HIGH);
    delay(300);
    for (uint8_t i = 0; i < 3; i++) digitalWrite(leds[i], LOW);
    delay(300);
  }
  show(F("LEDs done"), F(""));
  delay(1000);
}

// 6) Servo (lid)
void testServo() {
  show(F("Servo test"), F("Ext 5V? Any key"));
  waitKey();
  show(F("Closing lid..."), F(""));
  lid.attach(PIN_SERVO);
  lid.write(LID_CLOSED_ANGLE);
  delay(800);
  show(F("Opening lid..."), F(""));
  for (int a = LID_CLOSED_ANGLE; a <= LID_OPEN_ANGLE; a += 3) { lid.write(a); delay(25); }
  delay(800);
  show(F("Closing lid..."), F(""));
  for (int a = LID_OPEN_ANGLE; a >= LID_CLOSED_ANGLE; a -= 3) { lid.write(a); delay(25); }
  delay(500);
  lid.detach();
  show(F("Servo done"), F("Adjust angles?"));
  Serial.println(F("Servo: tune LID_OPEN_ANGLE / LID_CLOSED_ANGLE at the top"));
  delay(1500);
}

// 7) Stepper (carousel)
void testStepper() {
  show(F("Stepper test"), F("Ext 5V? Any key"));
  waitKey();
  carousel.setSpeed(10);
  show(F("Fwd 1 slot"), F("512 steps"));
  carousel.step(STEPS_PER_SLOT);
  releaseStepper();
  delay(600);
  show(F("Back 1 slot"), F("512 steps"));
  carousel.step(-STEPS_PER_SLOT);
  releaseStepper();
  delay(600);
  show(F("Full turn fwd"), F("2048 steps"));
  carousel.step(STEPS_PER_REV);
  releaseStepper();
  show(F("Done. Shaft back"), F("at start? Any key"));
  Serial.println(F("Stepper: wrong direction? Use Stepper(STEPS_PER_REV, 11, 9, 10, 8)"));
  waitKey();
}

// Shows a prompt plus the live IR value. '#' = true, '*' = false.
bool waitHashLive(const __FlashStringHelper* prompt) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(prompt);
  while (true) {
    char k = pollKey();
    if (k == '#') return true;
    if (k == '*') return false;
    char b[17];
    snprintf(b, sizeof(b), "IR=%4d #ok *esc", irRead());
    lcdText(1, b);
  }
}

// 8) IR sensor: calibrate, then run the "was the pill taken?" check
void testIR() {
  // --- calibration ---
  if (!waitHashLive(F("EMPTY slot, #"))) {
    if (!irCal) { show(F("Not calibrated"), F("Run test 8 again")); delay(1500); return; }
    show(F("Using old calib."), F("")); delay(1000);
  } else {
    irEmpty = irRead();
    if (!waitHashLive(F("PILL in slot, #"))) { show(F("Cancelled"), F("")); delay(1000); return; }
    irPill = irRead();
    int diff = abs(irPill - irEmpty);
    Serial.print(F("IR empty=")); Serial.print(irEmpty);
    Serial.print(F(" pill="));    Serial.print(irPill);
    Serial.print(F(" diff="));    Serial.println(diff);
    char b[17];
    snprintf(b, sizeof(b), "diff=%d", diff);
    if (diff < IR_MIN_DIFF) {
      show(F("WEAK SIGNAL"), F(""));
      lcdText(1, b);
      Serial.println(F("IR: pill vs empty too close - move/aim the sensor"));
      irCal = false;
      delay(3000);
      return;
    }
    irCal = true;
    show(F("Calibrated OK"), F(""));
    lcdText(1, b);
    delay(1500);
  }

  // --- wait for a pill to be in place ---
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Put pill in slot"));
  while (true) {
    if (pollKey() == '*') return;
    int v = irRead();
    char b[17];
    snprintf(b, sizeof(b), "IR=%4d %s", v, irIsPill(v) ? "PILL " : "empty");
    lcdText(1, b);
    if (irIsPill(v)) break;
  }
  delay(800);

  // --- "pill taken" check: absent for TAKEN_DEBOUNCE_MS in a row ---
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("TAKE the pill..."));
  uint32_t start = millis(), absentSince = 0;
  while (millis() - start < TAKEN_TIMEOUT_MS) {
    if (pollKey() == '*') return;
    int v = irRead();
    bool pill = irIsPill(v);
    char b[17];
    snprintf(b, sizeof(b), "IR=%4d %s", v, pill ? "PILL " : "empty");
    lcdText(1, b);
    if (pill) {
      absentSince = 0;
    } else {
      if (absentSince == 0) absentSince = millis();
      if (millis() - absentSince >= TAKEN_DEBOUNCE_MS) {
        char t[17];
        snprintf(t, sizeof(t), "in %lu.%lu s",
                 (millis() - start) / 1000UL, ((millis() - start) / 100UL) % 10UL);
        show(F("PILL TAKEN"), F(""));
        lcdText(1, t);
        Serial.println(F("IR: pill taken detected"));
        delay(2500);
        return;
      }
    }
  }
  show(F("NOT TAKEN"), F("(timed out)"));
  Serial.println(F("IR: timed out, pill still present"));
  delay(2500);
}

// 9) IR slot scan: checks the sensor sees every compartment
void testSlotScan() {
  if (!irCal) {
    show(F("Calibrate first"), F("Run test 8"));
    delay(1800);
    return;
  }
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Slot 1 at sensor"));
  lcd.setCursor(0, 1); lcd.print(F("#=go  *=cancel"));
  while (true) {
    char k = waitKey();
    if (k == '#') break;
    if (k == '*') return;
  }
  carousel.setSpeed(10);
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    delay(300);
    int v = irRead();
    bool pill = irIsPill(v);
    char b[17];
    snprintf(b, sizeof(b), "Slot %d: %s", s + 1, pill ? "PILL" : "empty");
    lcdText(0, b);
    snprintf(b, sizeof(b), "IR=%d", v);
    lcdText(1, b);
    Serial.print(F("Slot ")); Serial.print(s + 1);
    Serial.print(pill ? F(": PILL  IR=") : F(": empty IR="));
    Serial.println(v);
    delay(1500);
    carousel.step(STEPS_PER_SLOT);     // 4 moves = one full turn, back at slot 1
    releaseStepper();
  }
  show(F("Scan done"), F("Back at slot 1"));
  delay(1500);
}

// 0) Bluetooth: pings out, shows whatever the phone sends
void testBluetooth() {
  BT.begin(9600);
  show(F("Bluetooth test"), F("Pair + open app"));
  Serial.println(F("BT: pair the phone, open a serial terminal at 9600 baud"));
  delay(1500);

  char win[17];
  memset(win, ' ', 16);
  win[16] = 0;
  uint16_t txCount = 0, rxCount = 0;
  uint32_t lastTx = 0;

  while (!pollKey()) {
    if (millis() - lastTx > 2000) {
      lastTx = millis();
      BT.print(F("PING "));
      BT.println(++txCount);
      char b[17];
      snprintf(b, sizeof(b), "TX:%u RX:%u", txCount, rxCount);
      lcdText(0, b);
    }
    bool got = false;
    while (BT.available()) {
      char c = BT.read();
      Serial.write(c);
      if (c >= 32 && c < 127) {
        memmove(win, win + 1, 15);
        win[15] = c;
      }
      rxCount++;
      got = true;
    }
    if (got) {
      char b[17];
      snprintf(b, sizeof(b), "TX:%u RX:%u", txCount, rxCount);
      lcdText(0, b);
      lcdText(1, win);
    }
  }
}

// ====================================================================
//                           setup / loop
// ====================================================================
void setup() {
  Serial.begin(115200);
  Wire.begin();

  pinMode(PIN_SPEAKER, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_A, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  for (uint8_t i = 0; i < 4; i++) pinMode(STEP_PINS[i], OUTPUT);
  releaseStepper();

  lcd.init();
  lcd.backlight();
  show(F("Component test"), F("Uno + expander"));
  delay(1200);

  keypad.begin(makeKeymap(keyMap));
  i2cScan();
}

void loop() {
  uint8_t shownPage = 255;
  char k = 0;
  while (!k) {
    uint8_t page = (millis() / 3000UL) % 2;
    if (page != shownPage) {
      shownPage = page;
      if (page == 0) show(F("1LCD 2Key 3RTC"), F("4Spk 5LED 6Srv"));
      else           show(F("7Stp 8IR 9Scan"), F("0:Bluetooth"));
    }
    k = pollKey();
  }
  switch (k) {
    case '1': testLCD();       break;
    case '2': testKeypad();    break;
    case '3': testRTC();       break;
    case '4': testSpeaker();   break;
    case '5': testLEDs();      break;
    case '6': testServo();     break;
    case '7': testStepper();   break;
    case '8': testIR();        break;
    case '9': testSlotScan();  break;
    case '0': testBluetooth(); break;
    default:  break;
  }
}
