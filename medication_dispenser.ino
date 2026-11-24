/* =====================================================================
   EE-490 Senior Design - Project I: Automatic Medication Dispenser
   Target board : Arduino Mega 2560

   MECHANICAL MODEL (assumed)
     - A carousel with NUM_EVENTS compartments, turned by a 28BYJ-48
       stepper (ULN2003 driver). Compartment i is pre-loaded with the
       medication category (A or B) that schedule event i calls for.
     - A servo-driven lid covers the single opening in the housing.
       To "present" a dose: close lid -> rotate carousel -> open lid.

   WIRING (Mega 2560)
     LCD 16x2 I2C ........ SDA 20, SCL 21  (address LCD_ADDR)
     DS3231 RTC module ... SDA 20, SCL 21  (shares the I2C bus)
     4x4 keypad .......... rows 22-25, cols 26-29
     Stepper (ULN2003) ... IN1 30, IN2 31, IN3 32, IN4 33
     Lid servo ........... signal 9 (power from a separate 5 V supply,
                           common ground with the Arduino)
     Buzzer/speaker ...... pin 8 (through a transistor or resistor)
     LEDs ................ green 5, amber 6, red 7 (220 ohm each)
     Bluetooth (HC-05/06). module TX -> pin 19 (RX1), module RX <- pin 18
                           (TX1) through a 5 V -> 3.3 V divider
     Home switch (opt.) .. pin 34 to GND when slot 1 is aligned

   LIBRARIES (Library Manager): Keypad, LiquidCrystal_I2C (F. Brabander),
     RTClib (Adafruit). Servo, Stepper, EEPROM, Wire ship with the IDE.

   KEYPAD (idle screen)
     A = edit schedule   B = history   C = set clock   D = setup/test
     During menus: digits enter numbers, # = OK, * = cancel, C = backspace
     During a dose alert: # = acknowledge ("I took it")

   BLUETOOTH (text lines ending in newline, 9600 baud)
     In : STATUS | HIST | TIME,yyyy,mm,dd,hh,mm,ss
          SET,slot(1-N),hh,mm,A|B,enabled(0/1) | TIMEOUT,seconds
     Out: TAKEN,slot,cat,hh:mm | MISSED,slot,cat,hh:mm | FAULT,slot,cat,hh:mm
   ===================================================================== */

#include <Wire.h>
#include <EEPROM.h>
#include <Servo.h>
#include <Stepper.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// ------------------------------ configuration ------------------------
#define NUM_EVENTS        4       // compartments / daily events (4 or 8)
#define STEPS_PER_REV     2048    // 28BYJ-48 in full-step mode
#define STEPS_PER_SLOT    (STEPS_PER_REV / NUM_EVENTS)
#define HISTORY_LEN       10
#define LATE_GRACE_MIN    15      // still dispense if up to this late
#define ACK_MIN_SEC       10
#define ACK_MAX_SEC       300     // 2 x 300 s must stay < LATE_GRACE_MIN
#define DEFAULT_ACK_SEC   60
#define USE_HOME_SWITCH   0       // 1 = home carousel at boot with switch
#define LCD_ADDR          0x27    // try 0x3F if the screen stays blank
#define BT                Serial1
#define BT_BAUD           9600
#define LID_CLOSED_ANGLE  0
#define LID_OPEN_ANGLE    90
#define STORE_MAGIC       (0x4D00 | NUM_EVENTS)

// ------------------------------ pins ---------------------------------
const uint8_t PIN_BUZZER = 8;
const uint8_t PIN_SERVO  = 9;
const uint8_t PIN_LED_G  = 5;
const uint8_t PIN_LED_A  = 6;
const uint8_t PIN_LED_R  = 7;
const uint8_t PIN_HOME   = 34;
const uint8_t STEP_PINS[4] = {30, 31, 32, 33};

// ------------------------------ hardware objects ---------------------
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
RTC_DS3231 rtc;
Servo lid;
Stepper carousel(STEPS_PER_REV, 30, 32, 31, 33);  // IN1, IN3, IN2, IN4

const byte ROWS = 4, COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {22, 23, 24, 25};
byte colPins[COLS] = {26, 27, 28, 29};
Keypad keypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS);

// ------------------------------ persistent data ----------------------
struct Event {
  uint8_t hour;
  uint8_t minute;
  uint8_t category;   // 0 = A, 1 = B
  uint8_t enabled;
};

struct HistEntry {
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t eventIdx;
  char    status;     // 'T' taken, 'M' missed, 'F' fault
};

struct Store {
  uint16_t  magic;
  Event     events[NUM_EVENTS];
  uint16_t  ackTimeoutSec;
  uint8_t   currentSlot;
  uint8_t   histHead;
  uint8_t   histCount;
  HistEntry hist[HISTORY_LEN];
  uint16_t  testPass;
  uint16_t  testTotal;
};

Store S;

// ------------------------------ runtime state ------------------------
enum State { ST_IDLE, ST_ALERT1, ST_ALERT2, ST_RESULT };
State    state = ST_IDLE;
int      activeEvent = -1;
uint32_t stateStart = 0;
uint32_t resultUntil = 0;
uint32_t lastAlertDraw = 0;
uint32_t firedDay[NUM_EVENTS];
bool     buzzerOn = false;
bool     idleForce = true;
bool     rtcPresent = false;
bool     clockOK = false;
bool     motorFault = false;
DateTime g_now;

// ====================================================================
//                              storage
// ====================================================================
void saveStore() { EEPROM.put(0, S); }

void setDefaults() {
  static const uint8_t defH[8] = {8, 12, 18, 22, 10, 14, 16, 20};
  memset(&S, 0, sizeof(S));
  S.magic = STORE_MAGIC;
  for (uint8_t i = 0; i < NUM_EVENTS; i++) {
    S.events[i].hour = defH[i % 8];
    S.events[i].minute = 0;
    S.events[i].category = i % 2;    // A, B, A, B ...
    S.events[i].enabled = 1;
  }
  S.ackTimeoutSec = DEFAULT_ACK_SEC;
  S.currentSlot = 0;
  saveStore();
}

void loadStore() {
  EEPROM.get(0, S);
  if (S.magic != STORE_MAGIC || S.currentSlot >= NUM_EVENTS ||
      S.ackTimeoutSec < ACK_MIN_SEC || S.ackTimeoutSec > ACK_MAX_SEC ||
      S.histHead >= HISTORY_LEN || S.histCount > HISTORY_LEN) {
    setDefaults();
  }
}

// ====================================================================
//                       small output helpers
// ====================================================================
void lcdLine(uint8_t row, const char* s) {
  char b[17];
  snprintf(b, sizeof(b), "%-16s", s);
  lcd.setCursor(0, row);
  lcd.print(b);
}

void flash(const char* msg) {
  lcdLine(0, msg);
  lcdLine(1, "");
  delay(1000);
}

void setLeds(bool g, bool a, bool r) {
  digitalWrite(PIN_LED_G, g);
  digitalWrite(PIN_LED_A, a);
  digitalWrite(PIN_LED_R, r);
}

void buzz(bool on, uint16_t freq) {
  if (on && !buzzerOn) { tone(PIN_BUZZER, freq); buzzerOn = true; }
  else if (!on && buzzerOn) { noTone(PIN_BUZZER); buzzerOn = false; }
}

void beep(uint16_t freq, uint16_t ms) { tone(PIN_BUZZER, freq, ms); }

char lastStatus() {
  if (S.histCount == 0) return '-';
  return S.hist[(S.histHead + HISTORY_LEN - 1) % HISTORY_LEN].status;
}

void showStatusLeds() {
  char s = lastStatus();
  setLeds(s == 'T', false, s == 'M' || s == 'F');
}

// ====================================================================
//                               clock
// ====================================================================
void refreshClock() {
  if (clockOK) g_now = rtc.now();
}

void resetFired() {
  for (uint8_t i = 0; i < NUM_EVENTS; i++) firedDay[i] = 0xFFFFFFFFUL;
}

int findDueEvent() {
  if (!clockOK) return -1;
  uint32_t today = g_now.unixtime() / 86400UL;
  int nowMin = g_now.hour() * 60 + g_now.minute();
  for (uint8_t i = 0; i < NUM_EVENTS; i++) {
    if (!S.events[i].enabled) continue;
    if (firedDay[i] == today) continue;
    int sched = S.events[i].hour * 60 + S.events[i].minute;
    if (nowMin >= sched && (nowMin - sched) < LATE_GRACE_MIN) return i;
  }
  return -1;
}

int nextEvent() {
  int nowMin = g_now.hour() * 60 + g_now.minute();
  int best = -1, bestDelta = 9999;
  for (uint8_t i = 0; i < NUM_EVENTS; i++) {
    if (!S.events[i].enabled) continue;
    int d = S.events[i].hour * 60 + S.events[i].minute - nowMin;
    if (d <= 0) d += 1440;
    if (d < bestDelta) { bestDelta = d; best = i; }
  }
  return best;
}

// ====================================================================
//                          history / caregiver
// ====================================================================
void logEvent(uint8_t idx, char status) {
  HistEntry &h = S.hist[S.histHead];
  h.month = g_now.month();
  h.day = g_now.day();
  h.hour = g_now.hour();
  h.minute = g_now.minute();
  h.eventIdx = idx;
  h.status = status;
  S.histHead = (S.histHead + 1) % HISTORY_LEN;
  if (S.histCount < HISTORY_LEN) S.histCount++;
  saveStore();
}

void notifyCaregiver(const char* kind, uint8_t idx) {
  char b[40];
  snprintf(b, sizeof(b), "%s,%d,%c,%02d:%02d", kind, idx + 1,
           'A' + S.events[idx].category,
           S.events[idx].hour, S.events[idx].minute);
  BT.println(b);
  Serial.println(b);
}

// ====================================================================
//                         motors (carousel + lid)
// ====================================================================
void releaseStepper() {
  for (uint8_t i = 0; i < 4; i++) digitalWrite(STEP_PINS[i], LOW);
}

void setLid(int angle) {
  lid.write(angle);
  delay(700);
}

// Always rotates forward only, so gear backlash is the same every time.
// If the carousel turns the wrong way, swap IN2/IN3 or negate the step count.
void rotateToSlot(uint8_t target) {
  uint8_t slots = (target + NUM_EVENTS - S.currentSlot) % NUM_EVENTS;
  if (slots == 0) return;
  carousel.step((int)(slots * STEPS_PER_SLOT));
  releaseStepper();
  S.currentSlot = target;
  saveStore();
}

bool homeCarousel() {
#if USE_HOME_SWITCH
  pinMode(PIN_HOME, INPUT_PULLUP);
  int steps = 0;
  while (digitalRead(PIN_HOME) == HIGH && steps < STEPS_PER_REV + 64) {
    carousel.step(4);
    steps += 4;
  }
  releaseStepper();
  if (digitalRead(PIN_HOME) == HIGH) return false;   // never found home
  S.currentSlot = 0;
  saveStore();
#endif
  return true;
}

// Slot index == event index. Returns false (and presents nothing) on fault.
bool presentMedication(uint8_t idx) {
  if (motorFault) return false;
  setLid(LID_CLOSED_ANGLE);
  rotateToSlot(idx);
  setLid(LID_OPEN_ANGLE);
  return true;
}

// ====================================================================
//                           keypad helpers
// ====================================================================
void pollBluetooth();   // defined below

// Blocking key read for menus. Returns 0 if a dose becomes due, so every
// menu unwinds and the main loop can start the alert.
char waitKey() {
  while (true) {
    refreshClock();
    pollBluetooth();
    if (findDueEvent() >= 0) return 0;
    char k = keypad.getKey();
    if (k) return k;
  }
}

// Returns the entered number, or -1 if cancelled.
int readNumber(const char* title, int lo, int hi) {
  char buf[6];
  uint8_t len = 0;
  buf[0] = 0;
  lcdLine(0, title);
  char l2[17];
  while (true) {
    snprintf(l2, sizeof(l2), ">%s_  #ok *esc", buf);
    lcdLine(1, l2);
    char k = waitKey();
    if (k == 0 || k == '*') return -1;
    if (k >= '0' && k <= '9') {
      if (len < 4) { buf[len++] = k; buf[len] = 0; }
    } else if (k == 'C') {
      if (len) buf[--len] = 0;
    } else if (k == '#') {
      if (len) {
        int v = atoi(buf);
        if (v >= lo && v <= hi) return v;
      }
      beep(300, 200);
      len = 0;
      buf[0] = 0;
    }
  }
}

// ====================================================================
//                               menus
// ====================================================================
void editEvent(uint8_t i) {
  int h = readNumber("Hour (0-23)", 0, 23);   if (h < 0) return;
  int m = readNumber("Minute (0-59)", 0, 59); if (m < 0) return;
  int c = readNumber("Med 1=A 2=B", 1, 2);    if (c < 0) return;
  int e = readNumber("On? 1=yes 0=no", 0, 1); if (e < 0) return;
  S.events[i].hour = h;
  S.events[i].minute = m;
  S.events[i].category = c - 1;
  S.events[i].enabled = e;
  saveStore();
  flash("Saved");
}

void menuSchedule() {
  uint8_t i = 0;
  char l1[17], l2[17];
  while (true) {
    snprintf(l1, sizeof(l1), "Ev%d %02d:%02d Med %c", i + 1,
             S.events[i].hour, S.events[i].minute, 'A' + S.events[i].category);
    snprintf(l2, sizeof(l2), "%s D=nxt #=edit", S.events[i].enabled ? "ON " : "OFF");
    lcdLine(0, l1);
    lcdLine(1, l2);
    char k = waitKey();
    if (k == 0 || k == '*') return;
    if (k == 'D') i = (i + 1) % NUM_EVENTS;
    else if (k == '#') editEvent(i);
  }
}

void menuHistory() {
  if (S.histCount == 0) { flash("No history yet"); return; }
  int pos = 0;   // 0 = newest
  char l1[17], l2[17];
  while (true) {
    int idx = (S.histHead - 1 - pos + 2 * HISTORY_LEN) % HISTORY_LEN;
    HistEntry &h = S.hist[idx];
    const char* st = (h.status == 'T') ? "TAKEN" : (h.status == 'M') ? "MISSED" : "FAULT";
    snprintf(l1, sizeof(l1), "#%d %02d/%02d %02d:%02d", pos + 1,
             h.month, h.day, h.hour, h.minute);
    snprintf(l2, sizeof(l2), "Ev%d Med %c %s", h.eventIdx + 1,
             'A' + S.events[h.eventIdx].category, st);
    lcdLine(0, l1);
    lcdLine(1, l2);
    char k = waitKey();
    if (k == 0 || k == '*') return;
    if (k == 'A' && pos < S.histCount - 1) pos++;      // older
    else if (k == 'B' && pos > 0) pos--;               // newer
  }
}

void menuSetTime() {
  if (!rtcPresent) { flash("RTC missing"); return; }
  int y  = readNumber("Year (2024-2099)", 2024, 2099); if (y < 0) return;
  int mo = readNumber("Month (1-12)", 1, 12);          if (mo < 0) return;
  int d  = readNumber("Day (1-31)", 1, 31);            if (d < 0) return;
  int h  = readNumber("Hour (0-23)", 0, 23);           if (h < 0) return;
  int mi = readNumber("Minute (0-59)", 0, 59);         if (mi < 0) return;
  rtc.adjust(DateTime(y, mo, d, h, mi, 0));
  clockOK = true;
  resetFired();
  refreshClock();
  flash("Clock set");
}

void runTest() {
  if (motorFault) { flash("Motor fault"); return; }
  uint8_t slot = (S.currentSlot + 1) % NUM_EVENTS;
  lcdLine(0, "TEST: moving...");
  lcdLine(1, "");
  presentMedication(slot);
  lcdLine(0, "Right slot open?");
  lcdLine(1, "1=Yes 0=No *=skip");
  char k;
  do { k = waitKey(); } while (k != '1' && k != '0' && k != '*' && k != 0);
  setLid(LID_CLOSED_ANGLE);
  if (k == '1' || k == '0') {
    S.testTotal++;
    if (k == '1') S.testPass++;
    saveStore();
  }
}

void showTestStats() {
  char l1[17], l2[17];
  while (true) {
    int pct = S.testTotal ? (int)((100UL * S.testPass) / S.testTotal) : 0;
    snprintf(l1, sizeof(l1), "Pass %u/%u", S.testPass, S.testTotal);
    snprintf(l2, sizeof(l2), "%d%% C=clr *=ex", pct);
    lcdLine(0, l1);
    lcdLine(1, l2);
    char k = waitKey();
    if (k == 0 || k == '*') return;
    if (k == 'C') { S.testPass = 0; S.testTotal = 0; saveStore(); }
  }
}

// Manual alignment: jog until slot 1 sits under the opening, then press #.
void menuJog() {
  lcdLine(0, "Jog 4=back 6=fwd");
  lcdLine(1, "# = slot 1 here");
  while (true) {
    char k = waitKey();
    if (k == 0 || k == '*') return;
    if (k == '4') { carousel.step(-24); releaseStepper(); }
    else if (k == '6') { carousel.step(24); releaseStepper(); }
    else if (k == '#') {
      S.currentSlot = 0;
      saveStore();
      flash("Slot 1 set");
      return;
    }
  }
}

void menuSettings() {
  while (true) {
    lcdLine(0, "1:Ack 2:Test");
    lcdLine(1, "3:Stat 4:Jog *Ex");
    char k = waitKey();
    if (k == 0 || k == '*') return;
    if (k == '1') {
      int v = readNumber("Ack sec 10-300", ACK_MIN_SEC, ACK_MAX_SEC);
      if (v > 0) { S.ackTimeoutSec = v; saveStore(); flash("Saved"); }
    }
    else if (k == '2') runTest();
    else if (k == '3') showTestStats();
    else if (k == '4') menuJog();
  }
}

void handleMenuKey(char k) {
  switch (k) {
    case 'A': menuSchedule(); break;
    case 'B': menuHistory();  break;
    case 'C': menuSetTime();  break;
    case 'D': menuSettings(); break;
  }
}

// ====================================================================
//                              bluetooth
// ====================================================================
char btBuf[48];
uint8_t btLen = 0;

void sendStatus() {
  char b[56];
  if (!clockOK) { BT.println("STATUS,CLOCK_NOT_SET"); return; }
  snprintf(b, sizeof(b), "STATUS,%04d-%02d-%02d %02d:%02d,last=%c,fault=%d",
           g_now.year(), g_now.month(), g_now.day(), g_now.hour(),
           g_now.minute(), lastStatus(), motorFault ? 1 : 0);
  BT.println(b);
  int n = nextEvent();
  if (n >= 0) {
    snprintf(b, sizeof(b), "NEXT,%d,%c,%02d:%02d", n + 1,
             'A' + S.events[n].category, S.events[n].hour, S.events[n].minute);
    BT.println(b);
  }
}

void sendHistory() {
  char b[48];
  for (uint8_t pos = 0; pos < S.histCount; pos++) {
    int idx = (S.histHead - 1 - pos + 2 * HISTORY_LEN) % HISTORY_LEN;
    HistEntry &h = S.hist[idx];
    snprintf(b, sizeof(b), "H,%d,%02d/%02d %02d:%02d,%d,%c,%c", pos + 1,
             h.month, h.day, h.hour, h.minute, h.eventIdx + 1,
             'A' + S.events[h.eventIdx].category, h.status);
    BT.println(b);
  }
  BT.println("END");
}

void handleCommand(char* line) {
  char* cmd = strtok(line, ",");
  if (!cmd) return;
  for (char* p = cmd; *p; p++) *p = toupper(*p);
  char* tok[7];
  uint8_t n = 0;
  char* t;
  while (n < 7 && (t = strtok(NULL, ","))) tok[n++] = t;

  if (!strcmp(cmd, "STATUS")) {
    sendStatus();
  } else if (!strcmp(cmd, "HIST")) {
    sendHistory();
  } else if (!strcmp(cmd, "TIME") && n >= 6) {
    if (!rtcPresent) { BT.println("ERR,RTC"); return; }
    rtc.adjust(DateTime(atoi(tok[0]), atoi(tok[1]), atoi(tok[2]),
                        atoi(tok[3]), atoi(tok[4]), atoi(tok[5])));
    clockOK = true;
    resetFired();
    refreshClock();
    BT.println("OK");
  } else if (!strcmp(cmd, "SET") && n >= 5) {
    int i = atoi(tok[0]) - 1;
    int h = atoi(tok[1]), m = atoi(tok[2]);
    int c = (toupper(tok[3][0]) == 'B') ? 1 : 0;
    int e = atoi(tok[4]);
    if (i < 0 || i >= NUM_EVENTS || h < 0 || h > 23 || m < 0 || m > 59) {
      BT.println("ERR,RANGE");
      return;
    }
    S.events[i].hour = h;
    S.events[i].minute = m;
    S.events[i].category = c;
    S.events[i].enabled = e ? 1 : 0;
    saveStore();
    BT.println("OK");
  } else if (!strcmp(cmd, "TIMEOUT") && n >= 1) {
    int v = atoi(tok[0]);
    if (v < ACK_MIN_SEC || v > ACK_MAX_SEC) { BT.println("ERR,RANGE"); return; }
    S.ackTimeoutSec = v;
    saveStore();
    BT.println("OK");
  } else {
    BT.println("ERR,CMD");
  }
}

void pollBluetooth() {
  while (BT.available()) {
    char c = BT.read();
    if (c == '\n' || c == '\r') {
      if (btLen) {
        btBuf[btLen] = 0;
        handleCommand(btBuf);
        btLen = 0;
      }
    } else if (btLen < sizeof(btBuf) - 1) {
      btBuf[btLen++] = c;
    }
  }
}

// ====================================================================
//                     dose event state machine
// ====================================================================
void finishEvent(char status) {
  buzz(false, 0);
  setLid(LID_CLOSED_ANGLE);                 // never leave a dose exposed
  logEvent(activeEvent, status);

  const Event &e = S.events[activeEvent];
  char l2[17];
  if (status == 'T') {
    snprintf(l2, sizeof(l2), "Med %c taken", 'A' + e.category);
    lcdLine(0, "Dose recorded");
    lcdLine(1, l2);
    notifyCaregiver("TAKEN", activeEvent);
    beep(1800, 150);
  } else if (status == 'M') {
    snprintf(l2, sizeof(l2), "Med %c not taken", 'A' + e.category);
    lcdLine(0, "MISSED DOSE");
    lcdLine(1, l2);
    notifyCaregiver("MISSED", activeEvent);
    beep(400, 800);
  } else {
    lcdLine(0, "SYSTEM FAULT");
    lcdLine(1, "Contact caregiver");
    notifyCaregiver("FAULT", activeEvent);
    beep(300, 1500);
  }
  showStatusLeds();
  state = ST_RESULT;
  resultUntil = millis() + 5000;
}

void startEvent(int idx) {
  activeEvent = idx;
  firedDay[idx] = g_now.unixtime() / 86400UL;
  lcdLine(0, "Preparing dose...");
  lcdLine(1, "Please wait");
  if (!presentMedication(idx)) {            // fault: never claim success
    finishEvent('F');
    return;
  }
  state = ST_ALERT1;
  stateStart = millis();
  lastAlertDraw = 0;
}

void drawAlert(bool urgent, uint32_t elapsedMs) {
  char l1[17], l2[17];
  char cat = 'A' + S.events[activeEvent].category;
  long remain = (long)S.ackTimeoutSec - (long)(elapsedMs / 1000);
  if (remain < 0) remain = 0;
  if (urgent) snprintf(l1, sizeof(l1), "REMINDER! MED %c", cat);
  else        snprintf(l1, sizeof(l1), "TAKE MED %c (Ev%d)", cat, activeEvent + 1);
  snprintf(l2, sizeof(l2), "Press # (%lds)", remain);
  lcdLine(0, l1);
  lcdLine(1, l2);
}

// Two notification channels: audible (beeps) + visual (LEDs and LCD).
// Beep count and pitch identify the category (A = 1 beep low,
// B = 2 beeps high) so it is usable without reading the screen.
void alertOutputs(bool urgent) {
  uint8_t cat = S.events[activeEvent].category;
  uint8_t beeps = cat + 1;
  uint16_t freq = cat ? 2200 : 1500;
  uint16_t period = urgent ? 1200 : 3000;
  uint16_t phase = (millis() - stateStart) % period;
  bool on = (phase < (uint16_t)(beeps * 400)) && ((phase % 400) < 200);
  buzz(on, freq);
  bool redBlink = urgent && ((millis() / 250) % 2);
  setLeds(false, true, redBlink);
}

void handleAlert() {
  bool urgent = (state == ST_ALERT2);
  uint32_t elapsed = millis() - stateStart;

  char k = keypad.getKey();
  if (k == '#') { finishEvent('T'); return; }   // acknowledged

  if (elapsed >= (uint32_t)S.ackTimeoutSec * 1000UL) {
    if (state == ST_ALERT1) {                   // second reminder
      state = ST_ALERT2;
      stateStart = millis();
      lastAlertDraw = 0;
      return;
    }
    finishEvent('M');                           // no response: missed
    return;
  }

  alertOutputs(urgent);
  if (millis() - lastAlertDraw > 500) {
    lastAlertDraw = millis();
    drawAlert(urgent, elapsed);
  }
}

void drawIdle(bool force) {
  static uint32_t lastDraw = 0;
  if (!force && millis() - lastDraw < 500) return;
  lastDraw = millis();

  if (!rtcPresent) { lcdLine(0, "RTC NOT FOUND"); lcdLine(1, "Check wiring"); return; }
  if (!clockOK)    { lcdLine(0, "CLOCK NOT SET"); lcdLine(1, "Press C to set"); return; }

  char l1[17], l2[17];
  snprintf(l1, sizeof(l1), "%02d:%02d:%02d  Last:%c",
           g_now.hour(), g_now.minute(), g_now.second(), lastStatus());
  lcdLine(0, l1);

  uint8_t page = (millis() / 3000) % 3;
  if (motorFault) {
    lcdLine(1, "MOTOR FAULT");
  } else if (page == 0) {
    int n = nextEvent();
    if (n < 0) lcdLine(1, "No events set");
    else {
      snprintf(l2, sizeof(l2), "Next %02d:%02d Med %c",
               S.events[n].hour, S.events[n].minute, 'A' + S.events[n].category);
      lcdLine(1, l2);
    }
  } else if (page == 1) {
    lcdLine(1, "A:Sched B:Hist");
  } else {
    lcdLine(1, "C:Clock D:Setup");
  }
}

void handleIdle() {
  int due = findDueEvent();
  if (due >= 0) { startEvent(due); return; }

  char k = keypad.getKey();
  if (k) {
    handleMenuKey(k);
    idleForce = true;
  }
  drawIdle(idleForce);
  idleForce = false;
}

// ====================================================================
//                           setup / loop
// ====================================================================
void setup() {
  Serial.begin(115200);
  BT.begin(BT_BAUD);

  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_A, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  for (uint8_t i = 0; i < 4; i++) pinMode(STEP_PINS[i], OUTPUT);
  releaseStepper();

  lcd.init();
  lcd.backlight();
  lcdLine(0, "Med Dispenser");
  lcdLine(1, "Starting...");

  // LED + buzzer power-on self test
  setLeds(true, false, false);  delay(250);
  setLeds(false, true, false);  delay(250);
  setLeds(false, false, true);  delay(250);
  setLeds(false, false, false);
  beep(1500, 120);

  loadStore();
  resetFired();

  Wire.begin();
  rtcPresent = rtc.begin();
  clockOK = rtcPresent && !rtc.lostPower();
  refreshClock();

  lid.attach(PIN_SERVO);
  lid.write(LID_CLOSED_ANGLE);
  delay(500);

  carousel.setSpeed(10);                 // rpm; 28BYJ-48 is happy at ~10-15
  if (!homeCarousel()) {
    motorFault = true;
    lcdLine(0, "MOTOR FAULT");
    lcdLine(1, "Home not found");
    beep(300, 1500);
    delay(2000);
  }

  showStatusLeds();
  idleForce = true;
}

void loop() {
  refreshClock();
  pollBluetooth();

  switch (state) {
    case ST_IDLE:
      handleIdle();
      break;
    case ST_ALERT1:
    case ST_ALERT2:
      handleAlert();
      break;
    case ST_RESULT:
      if ((long)(millis() - resultUntil) >= 0) {
        state = ST_IDLE;
        idleForce = true;
      }
      break;
  }
}
