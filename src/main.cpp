// Organic white "breathing" lamp — custom PCB v1
// Board: Seeed XIAO ESP32-S3 (socketed) · Strip: WS2812B
// Controls: brightness pot, speed pot, on/off button, onboard status LED
// Build with PlatformIO (see platformio.ini). Library: FastLED 3.9.16.

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
#define NUM_LEDS        32       // test strip; set to 72 for the final 50 cm 144/m strip
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define MAX_MILLIAMPS   2500     // ~2500 on a 3 A charger; ~500 when powered from a laptop

// ================= DEBUG =================
const bool DEBUG = false;        // true = print knob values (only when a computer is connected)

// ================= CONTROL SETTINGS =================
const bool  INVERT_BRIGHT_POT = false;  // set true if turning clockwise dims the lamp
const bool  INVERT_SPEED_POT  = false;  // set true if turning clockwise slows it down
const float MIN_BRIGHTNESS    = 0.02f;  // knob fully down (0–1)
const float MAX_BRIGHTNESS    = 1.0f;   // knob fully up (0–1)
const float MIN_SPEED         = 0.15f;  // knob fully down: very slow drift
const float MAX_SPEED         = 3.0f;   // knob fully up: lively
const float FADE_TIME         = 1.2f;   // seconds for the on/off fade
const float POT_SMOOTHING     = 0.08f;  // 0–1, lower = smoother but slower to react
const float SPEED_SMOOTHING   = 0.03f;  // extra smoothing so speed changes glide, never jump

// ================= STATUS LED =================
const uint8_t STATUS_ON_LEVEL  = 40;    // 0–255 PWM: soft glow when the lamp is on
const uint8_t STATUS_OFF_LEVEL = 0;     // e.g. 4 for a faint "standby" glow when off

// ================= LOOK & FEEL =================
const float   BREATH_PERIOD = 7.0f;     // seconds per breath at speed 1.0
const uint8_t BREATH_DEPTH  = 90;       // how strongly the whole strip breathes (0–255)
const float   WAVE_SCALE    = 60.0f;    // blob size (use ~25 for the 144/m strip)
const float   DRIFT_SPEED   = 0.25f;    // how fast the waves wander along the strip
const float   DETAIL_MIX    = 0.30f;    // amount of fine ripple on top (0–1)
const float   FLOOR_LEVEL   = 0.025f;   // minimum glow per LED, relative to brightness (0–1)
const float   MIN_LED_LEVEL = 3.0f;     // absolute minimum per LED while on (0–255): LEDs never fully go dark
const float   HYSTERESIS    = 0.7f;     // an LED only changes step when its target moves this far (0.5 = off)
const CRGB    WHITE_TINT    = CRGB(255, 255, 255);  // e.g. CRGB(255, 200, 140) for warmer

const float CONTRAST_LOW  = 0.30f;
const float CONTRAST_HIGH = 0.70f;
const uint16_t FRAME_MS   = 8;          // ~120 fps: smoother motion at high speed
const float MAX_DT        = 0.04f;      // cap per-frame time step so a hiccup never causes a jump

// ================= STATE =================
CRGB  leds[NUM_LEDS];
float potBright = 0, potSpeed = 0;      // filtered knob positions 0–1
float speed = 1.0f;                     // smoothed animation speed
float brightness = 0.7f;                // 0–1
bool  lampOn = true;
float master = 0.0f;                    // 0–1 on/off fade (starts dark, fades in)

// ================= HELPERS =================
inline float smoothstepf(float x) { return x * x * (3.0f - 2.0f * x); }

inline float noise01(float x, float y) {
  // inoise16 works in 16.16 fixed point: 65536 = one noise cell
  return inoise16((uint32_t)x, (uint32_t)y) / 65535.0f;
}

float readPot(int pin, bool invert) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(pin);   // average out ADC noise
  float v = (sum / 8.0f) / 4095.0f;
  v = constrain((v - 0.02f) / 0.96f, 0.0f, 1.0f);        // small dead zones at both ends
  return invert ? 1.0f - v : v;
}

void updateControls(float dt) {
  // Knobs, smoothed against jitter
  potBright += (readPot(POT_BRIGHT_PIN, INVERT_BRIGHT_POT) - potBright) * POT_SMOOTHING;
  potSpeed  += (readPot(POT_SPEED_PIN,  INVERT_SPEED_POT)  - potSpeed)  * POT_SMOOTHING;

  // Squared curve: fine control at the dim end
  brightness = MIN_BRIGHTNESS + (MAX_BRIGHTNESS - MIN_BRIGHTNESS) * potBright * potBright;

  // Exponential curve, then glide toward it so speed never jumps
  float targetSpeed = MIN_SPEED * powf(MAX_SPEED / MIN_SPEED, potSpeed);
  speed += (targetSpeed - speed) * SPEED_SMOOTHING;

  // Button: debounced, toggles on press
  static bool lastStable = HIGH, lastRead = HIGH;
  static uint32_t lastChange = 0;
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastRead) { lastChange = millis(); lastRead = reading; }
  if (millis() - lastChange > 30 && reading != lastStable) {
    lastStable = reading;
    if (lastStable == LOW) lampOn = !lampOn;
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
  if (!DEBUG || !Serial) return;   // never block when no computer is listening
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  Serial.printf("bright %.2f -> %.3f | speed pot %.2f -> %.2f | %s\n",
                potBright, brightness, potSpeed, speed, lampOn ? "on" : "off");
}

// ================= SETUP / LOOP =================
void setup() {
  if (DEBUG) Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);   // full 0–3.3 V range

  potBright = readPot(POT_BRIGHT_PIN, INVERT_BRIGHT_POT);
  potSpeed  = readPot(POT_SPEED_PIN,  INVERT_SPEED_POT);
  speed = MIN_SPEED * powf(MAX_SPEED / MIN_SPEED, potSpeed);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.setDither(false);
  FastLED.setBrightness(255);       // brightness is applied per pixel below, in one step
  FastLED.clear(true);
}

void loop() {
  static uint32_t last = micros();
  uint32_t now = micros();
  float dt = min((now - last) / 1000000.0f, MAX_DT);
  last = now;

  updateControls(dt);
  printDebug();

  // ---- Clocks (in noise cells), wrapped so floats never lose precision ----
  static float tMain = 0, tDetail = 0, tSlow = 0, drift = 0, phase = 0;
  tMain   = fmodf(tMain   + dt * speed * 0.35f, 4096.0f);
  tDetail = fmodf(tDetail + dt * speed * 0.90f, 4096.0f);
  tSlow   = fmodf(tSlow   + dt * 0.05f,         4096.0f);

  // Drift direction wanders on its own, so the pattern never repeats
  float driftVel = (noise01(tSlow * 65536.0f, 1234567.0f) - 0.5f) * 2.0f * DRIFT_SPEED;
  drift = fmodf(drift + dt * speed * driftVel, 4096.0f);

  // ---- Global breath with slightly irregular tempo ----
  float wobble = (noise01(tSlow * 65536.0f, 7777777.0f) - 0.5f) * 0.6f;
  phase += dt * speed * TWO_PI / BREATH_PERIOD * (1.0f + wobble);
  if (phase > TWO_PI) phase -= TWO_PI;
  float breath = smoothstepf(0.5f - 0.5f * cosf(phase));

  float depth     = BREATH_DEPTH / 255.0f;
  float breathMul = (1.0f - depth) + depth * breath;
  float fade      = smoothstepf(master);
  float global    = brightness * fade;                  // knob × on/off fade
  float minLevel  = MIN_LED_LEVEL * fade;               // floor fades out with the lamp
  static uint8_t out[NUM_LEDS] = {0};                   // last value sent to each LED

  // ---- Per-LED organic field ----
  for (int i = 0; i < NUM_LEDS; i++) {
    float x = (i * WAVE_SCALE * 256.0f) + (drift + 8192.0f) * 65536.0f;
    float big    = noise01(x,                    tMain   * 65536.0f);
    float detail = noise01(x * 2.0f + 5.0e6f,    tDetail * 65536.0f + 3.0e6f);
    float n = big * (1.0f - DETAIL_MIX) + detail * DETAIL_MIX;

    n = constrain((n - CONTRAST_LOW) / (CONTRAST_HIGH - CONTRAST_LOW), 0.0f, 1.0f);
    n = smoothstepf(n);

    float level = powf(n * breathMul, 2.2f);          // gamma for even gradients
    level = FLOOR_LEVEL + (1.0f - FLOOR_LEVEL) * level;

    // Pattern × brightness × fade, lifted onto the minimum level (0–255, still a float)
    float target = minLevel + level * global * (255.0f - minLevel);

    // Hysteresis: only step when the target has clearly moved, so an LED sitting
    // between two dim steps doesn't chatter back and forth (the low-brightness flicker)
    if (fabsf(target - out[i]) > HYSTERESIS || (target < 0.5f && out[i] != 0)) {
      out[i] = (uint8_t)constrain(target + 0.5f, 0.0f, 255.0f);
    }

    leds[i] = WHITE_TINT;
    leds[i].nscale8_video(out[i]);
  }

  FastLED.show();
  delay(FRAME_MS);
}
