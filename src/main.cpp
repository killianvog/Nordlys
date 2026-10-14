// Organic white "breathing" lamp — custom PCB v1
// Board: Seeed XIAO ESP32-S3 (socketed) · Strip: WS2812B 144/m, 50 cm (72 LEDs)
// Controls: brightness pot, speed pot, on/off button, onboard status LED
// Build with PlatformIO (see platformio.ini). Library: FastLED.

#include <Arduino.h>
#include <FastLED.h>

// ================= PIN MAP (matches the schematic) =================
//  XIAO pin | GPIO | Function
//  D0       | 1    | Brightness pot wiper (RV1)
//  D1       | 2    | Speed pot wiper (RV2)
//  D3       | 4    | On/off button (SW1, to GND)
//  D4       | 5    | Status LED (D3 via R4)
//  D5       | 6    | Strip data -> level shifter U2 -> R3 -> J2
#define POT_BRIGHT_PIN  D0
#define POT_SPEED_PIN   D1
#define BUTTON_PIN      D3
#define STATUS_LED_PIN  D4
#define DATA_PIN        D5

// ================= STRIP =================
#define NUM_LEDS        32
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define MAX_MILLIAMPS   2500     // keep ~2500 on a 3 A charger; use ~500 when powered from a laptop

// ================= CONTROL SETTINGS =================
const bool  INVERT_BRIGHT_POT = false;  // set true if turning clockwise dims the lamp
const bool  INVERT_SPEED_POT  = false;  // set true if turning clockwise slows it down
const uint8_t MIN_BRIGHTNESS  = 4;      // knob fully down (never fully black while "on")
const uint8_t MAX_BRIGHTNESS  = 255;    // knob fully up
const float MIN_SPEED         = 0.15f;  // knob fully down: very slow drift
const float MAX_SPEED         = 3.0f;   // knob fully up: lively
const float FADE_TIME         = 1.2f;   // seconds for the on/off fade
const float POT_SMOOTHING     = 0.08f;  // 0–1, lower = smoother but slower to react

// ================= STATUS LED =================
const uint8_t STATUS_ON_LEVEL  = 40;    // 0–255 PWM: soft glow when the lamp is on
const uint8_t STATUS_OFF_LEVEL = 0;     // set e.g. 4 for a faint "standby" glow when off

// ================= LOOK & FEEL =================
const float   BREATH_PERIOD = 7.0f;     // seconds per breath at speed 1.0
const uint8_t BREATH_DEPTH  = 90;       // how strongly the whole strip breathes (0–255)
const float   WAVE_SCALE    = 25.0f;    // blob size (lowered from 60 for the denser 144/m strip)
const float   DRIFT_SPEED   = 0.25f;    // how fast the waves wander along the strip
const float   DETAIL_MIX    = 0.30f;    // amount of fine ripple on top (0–1)
const uint8_t FLOOR_LEVEL   = 6;        // minimum glow per LED (0–255)
const CRGB    WHITE_TINT    = CRGB(255, 255, 255);  // e.g. CRGB(255, 200, 140) for warmer

const float CONTRAST_LOW  = 0.30f;
const float CONTRAST_HIGH = 0.70f;
const uint16_t FRAME_MS   = 16;         // ~60 fps

// ================= STATE =================
CRGB    leds[NUM_LEDS];
float   potBright = 0, potSpeed = 0;    // filtered 0–1
float   speed = 1.0f;
uint8_t brightness = 180;
bool    lampOn = true;
float   master = 0.0f;                  // 0–1 on/off fade (starts dark, fades in)

// ================= HELPERS =================
inline float smoothstepf(float x) { return x * x * (3.0f - 2.0f * x); }

inline float noise01(float x, float y) {
  // inoise16 works in 16.16 fixed point: 65536 = one noise cell
  return inoise16((uint32_t)x, (uint32_t)y) / 65535.0f;
}

float readPot(int pin, bool invert) {
  float v = analogRead(pin) / 4095.0f;
  v = constrain((v - 0.02f) / 0.96f, 0.0f, 1.0f);  // small dead zones at both ends
  return invert ? 1.0f - v : v;
}

void updateControls(float dt) {
  // Knobs, smoothed against jitter
  potBright += (readPot(POT_BRIGHT_PIN, INVERT_BRIGHT_POT) - potBright) * POT_SMOOTHING;
  potSpeed  += (readPot(POT_SPEED_PIN,  INVERT_SPEED_POT)  - potSpeed)  * POT_SMOOTHING;

  // Squared curve: fine control at the dim end
  brightness = MIN_BRIGHTNESS + (MAX_BRIGHTNESS - MIN_BRIGHTNESS) * potBright * potBright;
  // Exponential curve: every part of the knob feels equally useful
  speed = MIN_SPEED * powf(MAX_SPEED / MIN_SPEED, potSpeed);

  // Button: debounced, toggles on press
  static bool lastStable = HIGH, lastRead = HIGH;
  static uint32_t lastChange = 0;
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastRead) { lastChange = millis(); lastRead = reading; }
  if (millis() - lastChange > 30 && reading != lastStable) {
    lastStable = reading;
    if (lastStable == LOW) {
      lampOn = !lampOn;
      Serial.printf("Lamp %s\n", lampOn ? "ON" : "OFF");
    }
  }

  // Smooth fade toward on/off
  float target = lampOn ? 1.0f : 0.0f;
  float step = dt / FADE_TIME;
  if (master < target)      master = min(master + step, target);
  else if (master > target) master = max(master - step, target);

  // Status LED follows the fade
  float s = STATUS_OFF_LEVEL + (STATUS_ON_LEVEL - STATUS_OFF_LEVEL) * smoothstepf(master);
  analogWrite(STATUS_LED_PIN, (int)s);
}

void printDebug() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  Serial.printf("bright pot %.2f -> %3u | speed pot %.2f -> %.2f | %s\n",
                potBright, brightness, potSpeed, speed, lampOn ? "on" : "off");
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);   // full 0–3.3 V range

  potBright = readPot(POT_BRIGHT_PIN, INVERT_BRIGHT_POT);
  potSpeed  = readPot(POT_SPEED_PIN,  INVERT_SPEED_POT);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.setDither(true);
  FastLED.clear(true);
}

void loop() {
  static uint32_t last = millis();
  uint32_t now = millis();
  float dt = (now - last) / 1000.0f;
  last = now;

  updateControls(dt);
  printDebug();

  // ---- Clocks (in noise cells) ----
  static float tMain = 0, tDetail = 0, tSlow = 0, drift = 0, phase = 0;
  tMain   += dt * speed * 0.35f;
  tDetail += dt * speed * 0.90f;
  tSlow   += dt * 0.05f;

  // Drift direction wanders on its own, so the pattern never repeats
  float driftVel = (noise01(tSlow * 65536.0f, 1234567.0f) - 0.5f) * 2.0f * DRIFT_SPEED;
  drift += dt * speed * driftVel;

  // ---- Global breath with slightly irregular tempo ----
  float wobble = (noise01(tSlow * 65536.0f, 7777777.0f) - 0.5f) * 0.6f;
  phase += dt * speed * TWO_PI / BREATH_PERIOD * (1.0f + wobble);
  if (phase > TWO_PI) phase -= TWO_PI;
  float breath = smoothstepf(0.5f - 0.5f * cosf(phase));

  float depth     = BREATH_DEPTH / 255.0f;
  float breathMul = (1.0f - depth) + depth * breath;
  float floorF    = FLOOR_LEVEL / 255.0f;

  // ---- Per-LED organic field ----
  for (int i = 0; i < NUM_LEDS; i++) {
    float x = (i * WAVE_SCALE * 256.0f) + drift * 65536.0f + 1000000.0f;
    float big    = noise01(x,               tMain   * 65536.0f);
    float detail = noise01(x * 2.0f + 5e6f, tDetail * 65536.0f + 3e6f);
    float n = big * (1.0f - DETAIL_MIX) + detail * DETAIL_MIX;

    n = constrain((n - CONTRAST_LOW) / (CONTRAST_HIGH - CONTRAST_LOW), 0.0f, 1.0f);
    n = smoothstepf(n);

    float level = powf(n * breathMul, 2.2f);          // gamma for even gradients
    level = floorF + (1.0f - floorF) * level;

    leds[i] = WHITE_TINT;
    leds[i].nscale8_video((uint8_t)(level * 255.0f));
  }

  // Knob brightness × eased on/off fade
  FastLED.setBrightness((uint8_t)(brightness * smoothstepf(master)));
  FastLED.delay(FRAME_MS);
}
