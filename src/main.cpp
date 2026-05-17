#include <Wire.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>

// ════════════════════════════════════════════════════════
//   BLYNK CONFIG  ← συμπλήρωσε τα δικά σου στοιχεία
// ════════════════════════════════════════════════════════
#define BLYNK_TEMPLATE_ID   "TMPL4XmSQac7Q"
#define BLYNK_TEMPLATE_NAME "gate"
#define BLYNK_AUTH_TOKEN    "-yBI1zKh00tYdjzArlafDmm_LObfQOot"

#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

char ssid[] = "Wokwi-GUEST";
char pass[] = "";
// Virtual Pins
#define VP_GATE_CONTROL  V0   // Blynk → ESP32 : άνοιγμα/κλείσιμο
#define VP_GATE_STATUS   V1   // ESP32 → Blynk : κατάσταση πύλης
#define VP_DISTANCE      V2   // ESP32 → Blynk : απόσταση cm
#define VP_LAST_EVENT    V3   // ESP32 → Blynk : log τελευταίας ενέργειας
#define VP_OBSTACLE      V4   // ESP32 → Blynk : 1=εμπόδιο, 0=ελεύθερο

// ════════════════════════════════════════════════════════
//   ΕΞΥΠΝΗ ΠΥΛΗ ΕΙΣΟΔΟΥ ΚΤΙΡΙΟΥ
//   Hardware: ESP32, Keypad 4x4, Servo, HC-SR04,
//             LCD I2C, Buzzer, RGB LED (common anode), Relay
// ════════════════════════════════════════════════════════

// ─── SERVO ───────────────────────────────────────────────
#define SERVO_PIN 32
Servo gateServo;

// ─── ULTRASONIC HC-SR04 ───────────────────────────────────
#define TRIG_PIN 13
#define ECHO_PIN 14

// ─── BUZZER ───────────────────────────────────────────────
#define BUZZER_PIN 16

// ─── RGB LED (Common Anode → COM στο 3.3V) ───────────────
#define LED_R  2
#define LED_G 25
#define LED_B 26

// ─── RELAY ───────────────────────────────────────────────
#define RELAY_PIN 5

// ─── LCD I2C ─────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─── KEYPAD 4×4 ──────────────────────────────────────────
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {23, 19, 18, 17};
byte colPins[COLS] = {33,  12, 15, 27};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ─── ACCESS DATA ─────────────────────────────────────────
const String CORRECT_PIN = "1234";

// ─── STATE ───────────────────────────────────────────────
String enteredPin = "";
bool gateOpen = false;
unsigned long gateOpenTime = 0;
const unsigned long GATE_OPEN_DURATION = 8000;

// Blynk timer για ανανέωση δεδομένων
BlynkTimer timer;

// ─── PROTOTYPES ──────────────────────────────────────────
void checkKeypad();
void grantAccess(String method);
void denyAccess(String reason);
void openGate();
void closeGate();
void showIdle();
long getDistance();
void buzzerOK();
void buzzerFail();
void setRGB(bool r, bool g, bool b);
void sendSensorData();
void logEvent(String event);

// ════════════════════════════════════════════════════════
//   BLYNK: Λήψη εντολής από dashboard (V0)
//   0 = κλείσιμο, 1 = άνοιγμα
// ════════════════════════════════════════════════════════
BLYNK_WRITE(VP_GATE_CONTROL) {
  int val = param.asInt();
  if (val == 1 && !gateOpen) {
    grantAccess("Blynk App");
  } else if (val == 0 && gateOpen) {
    closeGate();
    logEvent("Remote CLOSE");
  }
}

// ════════════════════════════════════════════════════════
//   SETUP
// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nSYSTEM STARTING...");

  // Servo
  gateServo.attach(SERVO_PIN, 500, 2400);
  gateServo.write(0);
  Serial.println("Servo READY - Gate CLOSED");

  // Ultrasonic
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // RGB
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setRGB(false, false, false);

  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" SMART GATE ");
  lcd.setCursor(0, 1);
  lcd.print(" Connecting...");

  // WiFi + Blynk
  Serial.println("Connecting to WiFi + Blynk...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  Serial.println("Blynk CONNECTED");

  // Self-test
  setRGB(false, false, true);
  buzzerOK();
  delay(400);
  setRGB(false, false, false);

  // Timer: στέλνε sensor data κάθε 2 δευτερόλεπτα
  timer.setInterval(2000L, sendSensorData);

  showIdle();
  logEvent("System START");
  Serial.println("SYSTEM READY");
}

// ════════════════════════════════════════════════════════
//   LOOP
// ════════════════════════════════════════════════════════
void loop() {
  Blynk.run();   // ← απαραίτητο για Blynk
  timer.run();   // ← απαραίτητο για BlynkTimer

  // Auto-close logic
  if (gateOpen && (millis() - gateOpenTime >= GATE_OPEN_DURATION)) {
    long distance = getDistance();
    Serial.print("Distance: ");
    Serial.print(distance);
    Serial.println(" cm");

    if (distance > 20 || distance >= 999) {
      closeGate();
    } else {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("OBSTACLE FOUND!");
      lcd.setCursor(0, 1);
      lcd.print("Waiting...");
      Serial.println("Obstacle! Waiting...");
      Blynk.virtualWrite(VP_OBSTACLE, 1);
      logEvent("Obstacle detected");
      gateOpenTime = millis();
    }
  }

  checkKeypad();
}

// ════════════════════════════════════════════════════════
//   BLYNK: Στέλνει δεδομένα στο dashboard κάθε 2s
// ════════════════════════════════════════════════════════
void sendSensorData() {
  long dist = getDistance();
  Blynk.virtualWrite(VP_DISTANCE, dist >= 999 ? 0 : dist);
  Blynk.virtualWrite(VP_GATE_STATUS, gateOpen ? 1 : 0);
  Blynk.virtualWrite(VP_OBSTACLE, 0);  // reset obstacle flag
}

// ════════════════════════════════════════════════════════
//   BLYNK: Log γεγονότος
// ════════════════════════════════════════════════════════
void logEvent(String event) {
  Serial.println("EVENT: " + event);
  Blynk.virtualWrite(VP_LAST_EVENT, event);
}

// ════════════════════════════════════════════════════════
//   KEYPAD CHECK
// ════════════════════════════════════════════════════════
void checkKeypad() {
  char key = keypad.getKey();
  if (!key) return;

  Serial.print("Key pressed: ");
  Serial.println(key);

  if (key == '#') {
    Serial.print("PIN entered: ");
    Serial.println(enteredPin);

    if (enteredPin == CORRECT_PIN) {
      grantAccess("PIN OK");
    } else {
      denyAccess("WRONG PIN");
    }
    enteredPin = "";

  } else if (key == '*') {
    enteredPin = "";
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PIN CLEARED");
    delay(800);
    showIdle();

  } else {
    if (enteredPin.length() < 8) {
      enteredPin += key;
    }
    lcd.setCursor(0, 0);
    lcd.print("ENTER PIN:      ");
    lcd.setCursor(0, 1);
    for (size_t i = 0; i < enteredPin.length(); i++) lcd.print('*');
    for (size_t i = enteredPin.length(); i < 16; i++) lcd.print(' ');
  }
}

// ════════════════════════════════════════════════════════
//   ACCESS GRANTED
// ════════════════════════════════════════════════════════
void grantAccess(String method) {
  Serial.println("ACCESS GRANTED - " + method);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" ACCESS OK!");
  lcd.setCursor(0, 1);
  lcd.print(method);

  logEvent("GRANTED: " + method);

  setRGB(false, true, false);
  digitalWrite(RELAY_PIN, HIGH);
  openGate();
  buzzerOK();
}

// ════════════════════════════════════════════════════════
//   ACCESS DENIED
// ════════════════════════════════════════════════════════
void denyAccess(String reason) {
  Serial.println("ACCESS DENIED - " + reason);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ACCESS DENIED");
  lcd.setCursor(0, 1);
  lcd.print(reason);

  logEvent("DENIED: " + reason);

  setRGB(true, false, false);
  buzzerFail();
  delay(1500);
  setRGB(false, false, false);
  showIdle();
}

// ════════════════════════════════════════════════════════
//   GATE OPEN / CLOSE
// ════════════════════════════════════════════════════════
void openGate() {
  Serial.println("GATE OPENING...");
  gateServo.write(90);
  gateOpen = true;
  gateOpenTime = millis();
  Blynk.virtualWrite(VP_GATE_STATUS, 1);
  Blynk.virtualWrite(VP_GATE_CONTROL, 1);  // συγχρονισμός button στο app
  Serial.println("GATE OPEN - auto close in 8s");
}

void closeGate() {
  Serial.println("GATE CLOSING...");
  gateServo.write(0);
  gateOpen = false;
  digitalWrite(RELAY_PIN, LOW);
  setRGB(false, false, false);
  Blynk.virtualWrite(VP_GATE_STATUS, 0);
  Blynk.virtualWrite(VP_GATE_CONTROL, 0);  // συγχρονισμός button στο app
  Blynk.virtualWrite(VP_OBSTACLE, 0);
  logEvent("Gate CLOSED");
  Serial.println("GATE CLOSED");
  showIdle();
}

// ════════════════════════════════════════════════════════
//   ULTRASONIC DISTANCE (cm). Returns 999 on timeout.
// ════════════════════════════════════════════════════════
long getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 999;
  return (long)(duration * 0.0343 / 2.0);
}

// ════════════════════════════════════════════════════════
//   RGB LED
// ════════════════════════════════════════════════════════
void setRGB(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

// ════════════════════════════════════════════════════════
//   BUZZER SOUNDS
// ════════════════════════════════════════════════════════
void buzzerOK() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(80);
  }
}

void buzzerFail() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(500);
  digitalWrite(BUZZER_PIN, LOW);
}

// ════════════════════════════════════════════════════════
//   IDLE SCREEN
// ════════════════════════════════════════════════════════
void showIdle() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enter PIN + #");
  lcd.setCursor(0, 1);
  lcd.print("(* to clear)");
}