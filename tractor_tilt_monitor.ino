/*
 * Tractor Tilt Monitor  --  Kubota EK1-261
 * -----------------------------------------
 * Bezpecnostny monitor naklonu pre traktor.
 *
 * Hardware:
 *   - ESP-WROOM-32 (dev board, CH340, USB-C)
 *   - GY-521 / MPU-6050 (akcelerometer + gyroskop, I2C @ 0x68)
 *   - OLED SSD1306 128x64 (I2C @ 0x3C)
 *   - Piezobzucak PK-20A38WQ spinany cez BC337 (NPN)
 *   - LM2596 step-down: 12V traktor -> 5V ESP32
 *
 * Meranie: komplementarny filter (gyro + akcelerometer), aby vibracie a
 * zrychlenie/brzdenie traktora nerozhadzali uhol. Cisty akcelerometer by
 * na pohybujucom sa stroji "skakal".
 *
 * !!! BEZPECNOST !!!
 * Toto je POMOCNY varovny system, nie nahrada za bezpecnu jazdu ani za ROPS.
 * Prahy nizsie su konzervativny odhad -- MUSIS si ich overit a doladit na
 * realnom stroji na bezpecnom, kontrolovanom svahu. Kubota EK1-261 je maly
 * traktor s vysokym tazistom; prevratenie sa deje rychlo a zradne, hlavne
 * do BOKU (roll).
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- Konfiguracia ----------------

// Prahy naklonu v stupnoch (od zvislice). Doladit na stroji!
const float TILT_WARN   = 12.0;   // zlta zona: zacni davat pozor
const float TILT_DANGER = 20.0;   // cervena zona: hrozi prevratenie

// Piny
const int PIN_BUZZER = 23;   // -> 1k odpor -> baza BC337 (D23, dolny rad DevKitu)
const int PIN_ZERO   = 0;    // onboard BOOT tlacidlo = "vynuluj na rovine"

// I2C
const int PIN_SDA = 21;
const int PIN_SCL = 22;

// MPU-6050
const uint8_t MPU_ADDR = 0x68;

// Komplementarny filter: 0.98 gyro / 0.02 akcel (typicke)
const float ALPHA = 0.98;

// --- Bzucak: obal cez ledc, funguje na ESP32 core 2.x aj 3.x ---
void buzzerBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2400, 8);
#else
  ledcSetup(0, 2400, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
}
void buzzerTone(uint32_t freq) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, freq);
#else
  ledcWriteTone(0, freq);
#endif
}

// OLED
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ---------------- Stav ----------------

float roll = 0, pitch = 0;          // odfiltrovane uhly [deg]
float rollZero = 0, pitchZero = 0;  // offset z kalibracie na rovine
float gxBias = 0, gyBias = 0;       // bias gyroskopu [deg/s]
unsigned long lastMicros = 0;

// Bzucak (neblokujuci)
unsigned long beepTimer = 0;
bool beepOn = false;

// ---------------- MPU-6050 nizka uroven ----------------

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// precita ax,ay,az,gx,gy,gz do g resp. deg/s
void mpuRead(float &ax, float &ay, float &az,
             float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);                  // ACCEL_XOUT_H
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)14);

  int16_t rax = (Wire.read() << 8) | Wire.read();
  int16_t ray = (Wire.read() << 8) | Wire.read();
  int16_t raz = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();          // teplota - preskoc
  int16_t rgx = (Wire.read() << 8) | Wire.read();
  int16_t rgy = (Wire.read() << 8) | Wire.read();
  int16_t rgz = (Wire.read() << 8) | Wire.read();

  ax = rax / 16384.0;  ay = ray / 16384.0;  az = raz / 16384.0;   // +-2g
  gx = rgx / 131.0;    gy = rgy / 131.0;    gz = rgz / 131.0;      // +-250 dps
}

// Zmeria bias gyroskopu (traktor musi 2s stat)
void calibrateGyro() {
  float sx = 0, sy = 0, dummy;
  const int N = 400;
  for (int i = 0; i < N; i++) {
    float gx, gy, gz;
    mpuRead(dummy, dummy, dummy, gx, gy, gz);
    sx += gx;  sy += gy;
    delay(3);
  }
  gxBias = sx / N;
  gyBias = sy / N;
}

// Nastavi aktualny naklon ako "nulu" (montujes nakrivo? na rovine stlac tlacidlo)
void setZero() {
  rollZero  += roll;   // pripocitame, lebo roll/pitch uz su po odcitani stareho zero
  pitchZero += pitch;
}

// ---------------- Alarm ----------------

void updateBuzzer(float tilt) {
  if (tilt >= TILT_DANGER) {
    // trvaly ton
    if (!beepOn) { buzzerTone(2400); beepOn = true; }
  } else if (tilt >= TILT_WARN) {
    // preryvane pipanie ~3 Hz
    if (millis() - beepTimer > 160) {
      beepTimer = millis();
      beepOn = !beepOn;
      buzzerTone(beepOn ? 2000 : 0);
    }
  } else {
    if (beepOn) { buzzerTone(0); beepOn = false; }
  }
}

// ---------------- Displej ----------------

void draw(float tilt) {
  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("NAKLON TRAKTORA");

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print("R:");
  display.print(roll, 0);
  display.print((char)247);        // stupen
  display.setCursor(0, 36);
  display.print("P:");
  display.print(pitch, 0);
  display.print((char)247);

  // stavovy pruh
  display.setTextSize(2);
  display.setCursor(80, 26);
  if (tilt >= TILT_DANGER)      display.print("!!!");
  else if (tilt >= TILT_WARN)   display.print(" ! ");
  else                          display.print("OK");

  // pruh naklonu dole
  int barW = (int)(tilt / TILT_DANGER * 127);
  if (barW > 127) barW = 127;
  display.drawRect(0, 58, 128, 6, SSD1306_WHITE);
  display.fillRect(0, 58, barW, 6, SSD1306_WHITE);

  display.display();
}

// ---------------- Setup / Loop ----------------

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ZERO, INPUT_PULLUP);

  // bzucak
  buzzerBegin();
  buzzerTone(0);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED nenajdeny!");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Kubota EK1-261");
  display.println("Tilt monitor");
  display.println("Kalibrujem...");
  display.display();

  // MPU-6050 zobud a nastav rozsahy
  mpuWrite(0x6B, 0x00);   // PWR_MGMT_1: wake
  delay(100);
  mpuWrite(0x1C, 0x00);   // ACCEL_CONFIG: +-2g
  mpuWrite(0x1B, 0x00);   // GYRO_CONFIG:  +-250 dps
  mpuWrite(0x1A, 0x03);   // DLPF ~44Hz (potlaci vibracie)
  delay(100);

  calibrateGyro();

  // inicializuj uhol z akcelerometra (aby nezacinal od 0)
  float ax, ay, az, gx, gy, gz;
  mpuRead(ax, ay, az, gx, gy, gz);
  roll  = atan2(ay, az) * 180.0 / PI;
  pitch = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;

  lastMicros = micros();
}

void loop() {
  float ax, ay, az, gx, gy, gz;
  mpuRead(ax, ay, az, gx, gy, gz);

  unsigned long now = micros();
  float dt = (now - lastMicros) / 1000000.0;
  lastMicros = now;

  // uhly z akcelerometra
  float rollAcc  = atan2(ay, az) * 180.0 / PI;
  float pitchAcc = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;

  // komplementarny filter (gyro s odcitanym biasom)
  roll  = ALPHA * (roll  + (gx - gxBias) * dt) + (1 - ALPHA) * rollAcc;
  pitch = ALPHA * (pitch + (gy - gyBias) * dt) + (1 - ALPHA) * pitchAcc;

  float r = roll  - rollZero;
  float p = pitch - pitchZero;

  // celkovy naklon od zvislice
  float tilt = sqrt(r * r + p * p);

  // tlacidlo: vynuluj na rovine
  if (digitalRead(PIN_ZERO) == LOW) {
    delay(50);
    if (digitalRead(PIN_ZERO) == LOW) {
      setZero();
      while (digitalRead(PIN_ZERO) == LOW) delay(10);
    }
  }

  updateBuzzer(tilt);

  static unsigned long lastDraw = 0;
  if (millis() - lastDraw > 100) {   // 10 fps
    lastDraw = millis();
    // prekresli s korigovanymi uhlami
    float sr = roll, sp = pitch;
    roll = r; pitch = p;             // draw() pouziva globaly
    draw(tilt);
    roll = sr; pitch = sp;

    Serial.print("roll="); Serial.print(r, 1);
    Serial.print(" pitch="); Serial.print(p, 1);
    Serial.print(" tilt="); Serial.print(tilt, 1);
    Serial.print(" state=");
    Serial.println(tilt >= TILT_DANGER ? "DANGER" :
                   tilt >= TILT_WARN ? "WARN" : "OK");
  }

  delay(5);
}
