/* =====================================================================
   EE-490 Project I - Component bring-up test, Arduino UNO R3
   Tests one part at a time from a keypad menu

   MISSING Pins: Bluetooth module, LEDs.
   Kept: LCD, keypad, speaker, servo, stepper.

   PIN MAP (Uno)
     I2C bus (A4 = SDA, A5 = SCL) shared by all I2C devices
        LCD backpack ......... 0x27 (LCD_ADDR)
        PCF8574 expander ..... 0x20 (KEYPAD_EXP_ADDR)  -> keypad
        DS3231 RTC ........... 0x68 
     D2  Bluetooth RX
     D3 LED green
     D4 LEG amber
     D5 LED red
     D6 Servo (lid)
     D7 Speaker/buzzer
     D8-D11 Stepper IN1-IN4 (ULN2003)
     D12 Bluetooth TX
     A0 IR Sensor AO
     Spare: D13, A1, A2, A3
   All grounds (Arduino, servo supply, stepper supply) must be common.

   LIBRARIES (Library Manager): I2CKeyPad (Tillaart), 
     LiquidCrystal (Brabander), Servo, Stepper, Wire.

   TESTING MENU:  1 LCD   2 Keypad   3 Speaker   4 Servo   5 Stepper
   ===================================================================== */

#include <Wire.h>
#include <Servo.h>
#include <Stepper.h>
#include <I2CKeyPad.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// ------------------------------ configuration ------------------------
#define LCD_ADDR          0x27
#define KEYPAD_EXP_ADDR   0x20
#define RTC_ADDR          0x68
#define STEPS_PER_REV     2048     // 28BYJ-48 in full-step mode
#define NUM_SLOTS         4
#define STEPS_PER_SLOT    (STEPS_PER_REV / 4)
#define LID_CLOSED_ANGLE  0
#define LID_OPEN_ANGLE    90
#define IR_MIN_DIFF       60
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

// I2CKeyPad (Tillaart) talks to the PCF8574 itself: it only needs the I2C
// address. Rows/cols are fixed by the PCF8574 pins (P0-P3 / P4-P7).
// getKey() returns an index 0..15 (16 = no key, 17 = fail), so the key map
// is a flat array indexed by that number.
I2CKeyPad keypad(KEYPAD_EXP_ADDR);
const char keyMap[17] = "123A456B789C*0#D";

//Projected IR
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

// Waits for a keypad press; a character typed in the Serial Monitor works too.
// Returns one char per key press (0 = nothing new). getKey() keeps reporting
// a key while it is held, so remember the last index to only report new presses.
char readKeypad() {
  static uint8_t lastIdx = I2C_KEYPAD_NOKEY;
  uint8_t idx = keypad.getKey();
  if (idx == I2C_KEYPAD_THRESHOLD) return 0;      // debounce: ignore
  if (idx > 15) {                                  // NOKEY (16) or FAIL (17)
    if (idx == I2C_KEYPAD_NOKEY) lastIdx = idx;    // key released
    return 0;
  }
  if (idx == lastIdx) return 0;                    // still held
  lastIdx = idx;
  return keyMap[idx];
}

char pollKey() {
  char k = readKeypad();
  if (k)
    return k;
  if (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r')
      return c;
  }
  return 0;
}

char waitKey() {
  char k;
  while (!(k = pollKey())) {

  }
  return k;
}

uint8_t countBits(uint16_t v) {
  uint8_t c = 0;
  while (v) { c += v & 1; v >>= 1; }
  return c;
}

int keyIndex(char k) {
  for (uint8_t i = 0; i < 16; i++)
    if (keyMap[i] == k) return i;
  return -1;
}

void releaseStepper() {
  for (uint8_t i = 0; i < 4; i++) digitalWrite(STEP_PINS[i], LOW);
}

int irRead() {
  long s = 0;
  for (uint8_t i = 0; i < 8; i++) {
    s += analogRead(PIN_IR);
    delay(2);
  }
  return (int)(s / 8);
}

// Reading is "pill present" if it is closer to the calibrated pill value
bool irIsPill(int v) {
  return abs(v - irPill) < abs(v - irEmpty);
}

// Scans the I2C bus and prints every address that answers
void i2cScan() {
  Serial.println(F("I2C scan:"));
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x"));
      Serial.println(addr, HEX);
    }
  }
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
    char k = readKeypad();
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
    show(F("KEYPAD TIMEOUT"), F("see Serial Mon."));
    Serial.print(F("Keypad: never saw:"));
    for (uint8_t i = 0; i < 16; i++)
      if (!((seen >> i) & 1)) {
        Serial.print(' ');
        Serial.print(keyMap[i]);
      }
    Serial.println();
  }
  delay(2000);
}

// 3) Speaker: the two category tones used by the dispenser
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

// 4) Servo (lid): slow sweep closed -> open -> closed
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

// 5) Stepper (carousel): 1/4 turn forward, 1/4 turn back, then 1 full turn
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

//6) RTC
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

// 7) LEDs
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

// 8) IR Live readout
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

// 9) IR sensor- calibration, in place, taken
void testIR() {
  // calibration
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
  // wait for a pill to be in place
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
  // "pill taken"
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

// 10) Bluetooth
void testBluetooth(){
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

  while (!Serial) {delay(10);}

  // Keypad expander (PCF8574 @ 0x20). begin() must come after Wire.begin().
  if (!keypad.begin()) {
    Serial.println(F("Error: can't find keypad PCF8574 at 0x20"));
    while (1);
  }
  Serial.println(F("Keypad expander found"));

  lcd.init();
  lcd.backlight();
  show(F("Component test"), F("Uno + expander"));
  delay(1200);

  i2cScan();
}

void loop() {
  uint8_t shownPage = 255;
  char k = 0;
  while (!k) {
    uint8_t page = (millis()/3000UL) % 2;
    if (page != shownPage) {
      shownPage = page;
      if (page == 0)
        show(F("1LCD 2Key 3Spk"), F("4Srv 5Stp 6RTC"));
      else
        show(F("7LED 8LIV 9IR"), F("0Bluetooth"));
    }
    k = pollKey();
  }
  switch (k) {
    case '1': testLCD();     break;
    case '2': testKeypad();  break;
    case '3': testSpeaker(); break;
    case '4': testServo();   break;
    case '5': testStepper(); break;
    case '6': testRTC(); break;
    case '7': testLEDs(); break;
    case '8': waitHashLive(F("IR live  #/* exit")); break;
    case '9': testIR(); break;
    case '0': testBluetooth(); break;
    default:  break;
  }
}