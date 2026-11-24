/* =====================================================================
   EE-490 Project I - Component bring-up test, Arduino UNO R3
   Tests one part at a time from a keypad menu (or the Serial Monitor,
   115200 baud, if the keypad is not working yet).

   MISSING Pins: Bluetooth module, LEDs.
   Kept: LCD, keypad, RTC, speaker, servo, stepper.

   PIN MAP (Uno)
     LCD 16x2 I2C ....... A4 (SDA), A5 (SCL)   address LCD_ADDR
     DS3231 RTC ......... A4 (SDA), A5 (SCL)   address 0x68
     Keypad rows ........ A0, A1, A2, A3 (used as digital pins)
     Keypad columns ..... D2, D3, D4, D5
     Servo (lid) ........ D6   (power from separate 5 V supply)
     Speaker/buzzer ..... D7
     Stepper (ULN2003) .. IN1 D8, IN2 D9, IN3 D10, IN4 D11
                          (power from separate 5 V supply)
     Spare .............. D12, D13  (D0/D1 stay free for USB)
   All grounds (Arduino, servo supply, stepper supply) must be common.

   LIBRARIES (Library Manager): Keypad, LiquidCrystal_I2C (F. Brabander),
     RTClib (Adafruit). Servo, Stepper, Wire come with the IDE.

   MENU:  1 LCD   2 Keypad   3 RTC   4 Speaker   5 Servo   6 Stepper
   ===================================================================== */

#include <Wire.h>
#include <Servo.h>
#include <Stepper.h>
#include <Keypad.h>
#include <LiquidCrystal.h>
//#include <RTClib.h>

// ------------------------------ configuration ------------------------
#define LCD_ADDR          0x27     // try 0x3F if the screen stays blank
#define STEPS_PER_REV     2048     // 28BYJ-48 in full-step mode
#define STEPS_PER_SLOT    (STEPS_PER_REV / 4)
#define LID_CLOSED_ANGLE  0
#define LID_OPEN_ANGLE    90

// ------------------------------ pins ---------------------------------
const uint8_t PIN_SERVO   = 6;
const uint8_t PIN_SPEAKER = 7;
const uint8_t STEP_PINS[4] = {8, 9, 10, 11};

// ------------------------------ hardware objects ---------------------
//LiquidCrystal_I2C
//RTC
Servo lid;
Stepper carousel(STEPS_PER_REV, 8, 10, 9, 11);   // IN1, IN3, IN2, IN4

const byte ROWS = 4, COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {A0, A1, A2, A3};
byte colPins[COLS] = {2, 3, 4, 5};
//Keypad keypad

// ====================================================================
//                            helpers
// ====================================================================
// Two lines of text from flash (no RAM used).
void show(const __FlashStringHelper* l1, const __FlashStringHelper* l2) {
}

// One padded line of text from RAM (for values that change).
void lcdText(uint8_t row, const char* s) {
  char b[17];
  snprintf(b, sizeof(b), "%-16s", s);
}

// Waits for a keypad press; a character typed in the Serial Monitor works too.
char waitKey() {
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

// ====================================================================
//                      I2C scan (runs at power-up)
// ====================================================================
void i2cScan() {
  bool lcdFound = false, rtcFound = false;
  uint8_t n = 0;
  Serial.println(F("I2C scan:"));
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x"));
      Serial.println(a, HEX);
      if (a == LCD_ADDR) lcdFound = true;
      if (a == 0x68) rtcFound = true;
      n++;
    }
  }
  if (!lcdFound) Serial.println(F("  LCD not at LCD_ADDR - try 0x3F"));
  if (!rtcFound) Serial.println(F("  RTC (0x68) not found"));
  // (0x57 is the RTC board's own EEPROM - normal.)

  char b[17];
  snprintf(b, sizeof(b), "I2C devices: %d", n);
  lcdText(0, b);
  snprintf(b, sizeof(b), "LCD:%c RTC:%c", lcdFound ? 'Y' : 'N', rtcFound ? 'Y' : 'N');
  lcdText(1, b);
  delay(2500);
}

// ====================================================================
//                              TESTS
// ====================================================================
// 1) LCD: full-width pattern + backlight blink
void testLCD() {
}

// 2) Keypad: press all 16 keys; ends when all seen or after 10 s idle
void testKeypad() {
}

// 3) RTC: shows date, time and chip temperature until a key is pressed
void testRTC() {
}

// 4) Speaker: the two category tones used by the dispenser
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

// 5) Servo (lid): slow sweep closed -> open -> closed
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
  lid.detach();                       // stops jitter between tests
  show(F("Servo done"), F("Adjust angles?"));
  Serial.println(F("Servo: tune LID_OPEN_ANGLE / LID_CLOSED_ANGLE at the top"));
  delay(1500);
}

// 6) Stepper (carousel): 1/4 turn forward, 1/4 turn back, then 1 full turn
void testStepper() {
  show(F("Stepper test"), F("Ext 5V? Any key"));
  waitKey();
  carousel.setSpeed(10);                         // rpm
  show(F("Fwd 1/4 turn"), F("512 steps"));
  carousel.step(STEPS_PER_SLOT);
  releaseStepper();
  delay(600);
  show(F("Back 1/4 turn"), F("512 steps"));
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

// ====================================================================
//                           setup / loop
// ====================================================================
void setup() {
  Serial.begin(115200);
  Wire.begin();

  pinMode(PIN_SPEAKER, OUTPUT);
  for (uint8_t i = 0; i < 4; i++) pinMode(STEP_PINS[i], OUTPUT);
  releaseStepper();

  i2cScan();
}

void loop() {
  show(F("1LCD 2Key 3RTC"), F("4Spk 5Srv 6Stp"));
  char k = waitKey();
  switch (k) {
    case '1': testLCD();     break;
    case '2': testKeypad();  break;
    case '3': testRTC();     break;
    case '4': testSpeaker(); break;
    case '5': testServo();   break;
    case '6': testStepper(); break;
    default:  break;
  }
}
