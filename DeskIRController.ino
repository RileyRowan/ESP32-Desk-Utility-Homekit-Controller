/*
  ESP32 Desk Utility HomeKit Controller
  ------------------------------------------------------------------------------
  HomeKit bridge (HomeSpan) for my desk: controls a Samsung TV over IR, drives a
  WS2812B monitor backlight, and reads one capacitive touch pad.

  BUILD
    Arduino IDE, ESP32 core 2.0.14, HomeSpan 1.9.1, IRremoteESP8266 2.9.0
    LEDs use HomeSpan's built-in Pixel class (RMT driven, safe alongside WiFi)

  HARDWARE
    GPIO18  TTP223B touch sensor
    GPIO4   IR LED
    GPIO5   WS2812B data. 12V strip, 114 LEDs wired 3 per IC = 38 pixels.
            Strip has its own 12V supply, ground shared with the ESP32.

  SETUP (Serial Monitor, 115200 baud)
    W     enter WiFi credentials
    E     erase stored data (WiFi + HomeKit pairing)
    ?     full HomeSpan command list
    @W    save the Monitor Light's current color as the warm white (kept in flash)
    HomeKit setup code: 466-37-726

  HOMEKIT TILES
    Desk Switch        ON:  light fades up warm, TV power IR sent 5s later
                       OFF: light fades out (cancels the IR if not sent yet)
    TV Power, Volume Up, Volume Down, Mode
                       momentary buttons that send the Samsung IR code
    Monitor Light      color bulb (on, brightness, hue, saturation) for the strip
    Chase Red / Blue   red or blue wave drifting over the Monitor Light color
    Color Cycle        red/green/blue wheel rotating around the monitor, 10s loop
    Fireflies          warm base at 10%, random pixels swell up and back down

    Effect tiles act like radio buttons; turning the active one off returns to
    solid. Brightness dims every mode. Mode changes crossfade over FADE_MS.
    Turning the light on from off (any path) resets to warm white at 50%.

  TOUCH COMMANDS

      +-------------+------------------------------------------------------------+
      | Gesture     | Action                                                     |
      +-------------+------------------------------------------------------------+
      | 1 tap       | TV Volume Up                                               |
      | 2 taps      | TV Volume Down                                             |
      | 3 taps      | TV Mode                                                    |
      | 4 taps      | Monitor Light on/off (on = warm white @ 50%)               |
      | 5 taps      | Next effect: Solid>Chase Red>Chase Blue>Cycle>Fireflies    |
      | Hold 1s     | Desk Switch ON (light fades up warm, TV power IR in 5s)    |
      | Hold 3s     | Desk Switch OFF (light fades out)                          |
      +-------------+------------------------------------------------------------+
      Taps must land within 400ms of each other (TAP_WINDOW).
*/

#include <Arduino.h>
#include <HomeSpan.h>
#include "extras/Pixel.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <Preferences.h>

// -------------------- PINS --------------------
#define TOUCH_PIN 18
#define IR_PIN 4
#define LED_PIN 5

#define NUM_PIXELS 38          // 38 addressable groups (114 LEDs, 3 per IC)

IRsend irsend(IR_PIN);

// -------------------- IR CODES --------------------
const uint32_t IR_POWER    = 0x34347887;
const uint32_t IR_VOL_UP   = 0x3434E817;
const uint32_t IR_VOL_DOWN = 0x34346897;
const uint32_t IR_MODE     = 0x343451AE;

// -------------------- LED MODES --------------------
enum LedMode {
  MODE_SOLID,        // single HomeKit-set color on all pixels
  MODE_CHASE_RED,    // red chases over the Monitor Light background color
  MODE_CHASE_BLUE,   // blue chases over the Monitor Light background color
  MODE_ANCHOR,       // rotating red/green/blue color wheel ("Color Cycle")
  MODE_FIREFLY,      // static warm white with random intensity swells
  MODE_COUNT
};

const char *MODE_NAMES[MODE_COUNT] = { "Solid", "Chase Red", "Chase Blue", "Color Cycle", "Fireflies" };

// -------------------- LED TUNING --------------------
const unsigned long FADE_MS   = 1500;   // crossfade length between modes
const unsigned long FRAME_MS  = 33;     // ~30 fps
const float POWER_FADE_STEP   = 0.033;  // master fade step per frame (~1s full on/off)

// Warm white factory default (R=100%, G=35%, B=0%), also the firefly color.
// '@W' overwrites the live values below and saves them to flash; they reload at boot.
const float WARM_HUE_DEFAULT = 21, WARM_SAT_DEFAULT = 100;

float   warmHue = WARM_HUE_DEFAULT, warmSat = WARM_SAT_DEFAULT;
uint8_t warmR = 255, warmG = 89, warmB = 0;    // kept in sync with warmHue/warmSat

Preferences prefs;

// Chase colors
const uint8_t COL_RED[3]  = { 255, 0,   0 };
const uint8_t COL_BLUE[3] = {   0, 0, 255 };

// Chase: one full sine wave around the loop, so the chase color's peak and the
// pure background color sit 180 degrees apart -- both visible at once, drifting.
const float CHASE_SPEED = 0.8;      // radians/sec drift
const float CHASE_WAVES = 1.0;      // waves along the strip

// Color Cycle: every pixel sits on a circular 0..65535 wheel through the anchor
// colors, and the wheel rotates over time.
const uint8_t ANCHORS[3][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 } };  // red, green, blue
const float ANCHOR_ROT_SPEED = -6554.0;   // 65536 units / 10s = one loop every 10s; sign = direction
const float ANCHOR_GAMMA = 2.8;

#define MAX_FIREFLIES 6
const float FIREFLY_BASE = 0.10;    // dim warm base level; swells rise to 1.0 and dim back down

// -------------------- STRIP COLOR ORDER --------------------
// This strip's ICs take channels in RBG order, so green and blue are swapped
// right before the driver. Everything else in the sketch works in normal RGB.
Pixel::Color stripColor(uint8_t r, uint8_t g, uint8_t b) {
  return Pixel::Color().RGB(r, b, g);   // RBG profile: swap G and B
}

// -------------------- HSV -> RGB --------------------
void hsv2rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
  s /= 100.0; v /= 100.0;
  float c = v * s;
  float x = c * (1 - fabsf(fmodf(h / 60.0, 2) - 1));
  float m = v - c;
  float rf, gf, bf;
  if      (h <  60) { rf = c; gf = x; bf = 0; }
  else if (h < 120) { rf = x; gf = c; bf = 0; }
  else if (h < 180) { rf = 0; gf = c; bf = x; }
  else if (h < 240) { rf = 0; gf = x; bf = c; }
  else if (h < 300) { rf = x; gf = 0; bf = c; }
  else              { rf = c; gf = 0; bf = x; }
  *r = (rf + m) * 255;
  *g = (gf + m) * 255;
  *b = (bf + m) * 255;
}

// -------------------- LED ENGINE --------------------
// Renders the active mode every frame. After a mode change it also renders the
// previous mode and crossfades the two over FADE_MS, so effects never snap.
// Power on/off is a separate master brightness ramp (~1s fade).
struct LedEngine {

  Pixel *pixel;

  LedMode mode = MODE_SOLID;
  LedMode prevMode = MODE_SOLID;
  unsigned long modeChangeTime = 0;

  float master = 0;         // current master brightness 0..1
  float masterTarget = 0;   // where the master fade is headed

  // Solid-mode color, set from HomeKit (defaults to warm white at 50%)
  float hue = WARM_HUE_DEFAULT, sat = WARM_SAT_DEFAULT, val = 50;

  unsigned long lastFrame = 0;

  uint8_t bufCur[NUM_PIXELS][3];
  uint8_t bufPrev[NUM_PIXELS][3];

  struct Firefly {
    bool active = false;
    int idx = 0;
    unsigned long start = 0;
    unsigned long dur = 0;
  };
  Firefly flies[MAX_FIREFLIES];

  uint8_t gammaT[256];   // gamma 2.8 lookup for the Color Cycle wheel

  void begin() {
    pixel = new Pixel(LED_PIN);
    for (int i = 0; i < 256; i++)
      gammaT[i] = (uint8_t)(255.0 * powf(i / 255.0, ANCHOR_GAMMA) + 0.5);
    clear();
  }

  void clear() {
    Pixel::Color colors[NUM_PIXELS];
    for (int i = 0; i < NUM_PIXELS; i++) colors[i] = stripColor(0, 0, 0);
    pixel->set(colors, NUM_PIXELS);
  }

  void setMode(LedMode m) {
    if (m == mode) return;
    prevMode = mode;
    mode = m;
    modeChangeTime = millis();
    Serial.print("LED mode -> ");
    Serial.println(MODE_NAMES[m]);
  }

  void setPower(bool on) {
    masterTarget = on ? 1.0 : 0.0;
  }

  bool isOn() {
    return masterTarget > 0.5;
  }

  // ---- per-mode renderers ----

  // Renderers work at full value; brightness (val) is applied in tick().
  void renderSolid(uint8_t buf[][3]) {
    uint8_t r, g, b;
    hsv2rgb(hue, sat, 100, &r, &g, &b);
    for (int i = 0; i < NUM_PIXELS; i++) { buf[i][0] = r; buf[i][1] = g; buf[i][2] = b; }
  }

  // Chase color drifts over the Monitor Light color. One sine wave around the
  // loop puts the chase peak and the pure background 180 degrees apart.
  void renderChase(uint8_t buf[][3], const uint8_t *chase, float t) {
    uint8_t br, bg, bb;
    hsv2rgb(hue, sat, 100, &br, &bg, &bb);
    for (int i = 0; i < NUM_PIXELS; i++) {
      float k = 0.5 + 0.5 * sinf(i * (2 * PI * CHASE_WAVES / NUM_PIXELS) + t * CHASE_SPEED);
      buf[i][0] = br + (chase[0] - br) * k;
      buf[i][1] = bg + (chase[1] - bg) * k;
      buf[i][2] = bb + (chase[2] - bb) * k;
    }
  }

  // pos(i) = offset + i * 65536 / NUM_PIXELS spreads the wheel evenly around the
  // loop, including the wrap seam. Colors blend between anchors, then gamma.
  void renderAnchor(uint8_t buf[][3], float t) {
    uint16_t offset = (uint16_t)(int32_t)(t * ANCHOR_ROT_SPEED);
    for (int i = 0; i < NUM_PIXELS; i++) {
      uint16_t pos = offset + (uint32_t)i * 65536UL / NUM_PIXELS;
      uint32_t scaled  = (uint32_t)pos * 3;
      uint8_t  segment = scaled / 65536UL;
      uint16_t frac    = scaled % 65536UL;
      const uint8_t *c0 = ANCHORS[segment];
      const uint8_t *c1 = ANCHORS[(segment + 1) % 3];
      buf[i][0] = gammaT[c0[0] + ((int32_t)(c1[0] - c0[0]) * frac) / 65536];
      buf[i][1] = gammaT[c0[1] + ((int32_t)(c1[1] - c0[1]) * frac) / 65536];
      buf[i][2] = gammaT[c0[2] + ((int32_t)(c1[2] - c0[2]) * frac) / 65536];
    }
  }

  void renderFirefly(uint8_t buf[][3]) {
    unsigned long now = millis();

    // occasionally spawn a new firefly in a free slot
    for (int f = 0; f < MAX_FIREFLIES; f++) {
      if (!flies[f].active && random(100) < 2) {          // ~2% chance per slot per frame
        flies[f].active = true;
        flies[f].idx = random(NUM_PIXELS);
        flies[f].start = now;
        flies[f].dur = random(2000, 4500);                // 2 - 4.5s swell
      }
    }

    // static warm base
    for (int i = 0; i < NUM_PIXELS; i++) {
      buf[i][0] = warmR * FIREFLY_BASE;
      buf[i][1] = warmG * FIREFLY_BASE;
      buf[i][2] = warmB * FIREFLY_BASE;
    }

    // active swells: smooth rise and fall up to full warm brightness
    for (int f = 0; f < MAX_FIREFLIES; f++) {
      if (!flies[f].active) continue;
      float p = (float)(now - flies[f].start) / flies[f].dur;
      if (p >= 1.0) { flies[f].active = false; continue; }
      float swell = sinf(PI * p);                          // 0 -> 1 -> 0
      float level = FIREFLY_BASE + (1.0 - FIREFLY_BASE) * swell;
      int i = flies[f].idx;
      buf[i][0] = warmR * level;
      buf[i][1] = warmG * level;
      buf[i][2] = warmB * level;
    }
  }

  void render(LedMode m, float t, uint8_t buf[][3]) {
    switch (m) {
      case MODE_SOLID:      renderSolid(buf); break;
      case MODE_CHASE_RED:  renderChase(buf, COL_RED, t); break;
      case MODE_CHASE_BLUE: renderChase(buf, COL_BLUE, t); break;
      case MODE_ANCHOR:     renderAnchor(buf, t); break;
      case MODE_FIREFLY:    renderFirefly(buf); break;
      default: break;
    }
  }

  // ---- frame tick: call every pass through the main loop ----
  void tick() {
    unsigned long now = millis();
    if (now - lastFrame < FRAME_MS) return;
    lastFrame = now;

    // master power ramp (soft on/off)
    if (master < masterTarget) master = min(masterTarget, master + POWER_FADE_STEP);
    if (master > masterTarget) master = max(masterTarget, master - POWER_FADE_STEP);

    // skip rendering entirely once fully dark
    if (master <= 0.0 && masterTarget <= 0.0) {
      clear();
      return;
    }

    // HomeKit brightness acts as a global dimmer over every mode
    float scale = master * (val / 100.0);

    float t = now / 1000.0;
    render(mode, t, bufCur);

    // crossfade with the previous mode while inside the fade window
    float blend = 1.0;
    if (now - modeChangeTime < FADE_MS) {
      blend = (float)(now - modeChangeTime) / FADE_MS;
      render(prevMode, t, bufPrev);
    }

    Pixel::Color colors[NUM_PIXELS];
    for (int i = 0; i < NUM_PIXELS; i++) {
      float r = bufCur[i][0], g = bufCur[i][1], b = bufCur[i][2];
      if (blend < 1.0) {
        r = bufPrev[i][0] + (r - bufPrev[i][0]) * blend;
        g = bufPrev[i][1] + (g - bufPrev[i][1]) * blend;
        b = bufPrev[i][2] + (b - bufPrev[i][2]) * blend;
      }
      colors[i] = stripColor(r * scale, g * scale, b * scale);
    }
    pixel->set(colors, NUM_PIXELS);
  }
};

LedEngine leds;

// Forward declarations for cross-object updates
void syncEffectTiles(LedMode activeMode);
void setLightTile(bool on);
void resetToWarmDefaults();

// -------------------- TOUCH / GESTURE STATE --------------------
bool touchActive = false;
unsigned long touchStart = 0;
bool hold1Fired = false;
bool hold2Fired = false;

const unsigned long HOLD1_MS = 1000;   // hold to turn desk switch ON
const unsigned long HOLD2_MS = 3000;   // hold to turn desk switch OFF

// -------------------- TAP STATE --------------------
int tapCount = 0;
unsigned long lastTapTime = 0;
const unsigned long TAP_WINDOW = 400;

// -------------------- HOMEKIT: DESK SWITCH --------------------
struct DeskSceneSwitch : Service::Switch {

  SpanCharacteristic *power;

  bool pendingIr = false;
  unsigned long irTime = 0;

  DeskSceneSwitch() : Service::Switch() {
    power = new Characteristic::On(false);
  }

  // HomeKit toggled the switch
  boolean update() override {
    applyState(power->getNewVal());
    return true;
  }

  // HomeSpan calls this every poll; handles the non-blocking 5s IR delay
  void loop() override {
    if (pendingIr && millis() >= irTime) {
      irsend.sendSAMSUNG(IR_POWER, 32);
      Serial.println("POWER sent");
      pendingIr = false;
    }
  }

  // Touch sensor drives the switch; also updates the HomeKit tile
  void setFromTouch(bool state) {
    power->setVal(state, true);
    applyState(state);
  }

  // Shared by HomeKit and touch
  void applyState(bool state) {
    Serial.print("Desk Switch -> ");
    Serial.println(state ? "ON" : "OFF");

    if (state) {
      pendingIr = true;
      irTime = millis() + 5000;
      Serial.println("Will send POWER IR in 5s...");

      // Monitor coming on -> light defaults to warm white at 50%
      resetToWarmDefaults();
      leds.setPower(true);
      setLightTile(true);
    } else {
      pendingIr = false;   // cancel a pending send if switch is turned off before the 5s elapses

      // Desk going off -> fade the light out too
      leds.setPower(false);
      setLightTile(false);
    }
  }
};

DeskSceneSwitch *deskSwitch;

// -------------------- HOMEKIT: MOMENTARY REMOTE BUTTON --------------------
// One remote key as a HomeKit switch that turns itself back off after 500ms,
// so it acts like a button in the Home app and Siri.
struct RemoteButton : Service::Switch {

  SpanCharacteristic *power;
  const char *label;
  uint32_t irCode;

  bool pendingOff = false;
  unsigned long offTime = 0;

  RemoteButton(const char *label, uint32_t irCode) : Service::Switch() {
    this->label = label;
    this->irCode = irCode;
    power = new Characteristic::On(false);
  }

  boolean update() override {
    if (power->getNewVal()) {
      Serial.print(label);
      Serial.println(" pressed via HomeKit");

      irsend.sendSAMSUNG(irCode, 32);

      pendingOff = true;
      offTime = millis() + 500;   // brief on-flash, then auto-reset
    }
    return true;
  }

  void loop() override {
    if (pendingOff && millis() >= offTime) {
      power->setVal(false, true);
      pendingOff = false;
    }
  }

  // Lets the touch sensor trigger the same button/IR code
  void pressFromTouch() {
    irsend.sendSAMSUNG(irCode, 32);
    power->setVal(true, true);
    pendingOff = true;
    offTime = millis() + 500;
  }
};

RemoteButton *powerButton;
RemoteButton *volUpButton;
RemoteButton *volDownButton;
RemoteButton *modeButton;

// -------------------- HOMEKIT: MONITOR LIGHT (color bulb) --------------------
struct MonitorLight : Service::LightBulb {

  SpanCharacteristic *power;
  SpanCharacteristic *level;
  SpanCharacteristic *hue;
  SpanCharacteristic *saturation;

  MonitorLight() : Service::LightBulb() {
    power      = new Characteristic::On(false);
    level      = new Characteristic::Brightness(50);
    hue        = new Characteristic::Hue(warmHue);
    saturation = new Characteristic::Saturation(warmSat);
    level->setRange(1, 100, 1);
  }

  boolean update() override {

    if (power->updated()) {
      // Turning on from off comes up warm white at 50%. If the same request
      // also carries a color or brightness (a scene), it's applied below.
      if (power->getNewVal() && !leds.isOn())
        resetToWarmDefaults();
      leds.setPower(power->getNewVal());
      Serial.print("Monitor Light -> ");
      Serial.println(power->getNewVal() ? "ON" : "OFF");
    }

    // Brightness dims every mode; it doesn't stop a running effect
    if (level->updated())
      leds.val = level->getNewVal<float>();

    // Color sets the solid color, which is also the chase background.
    // A running effect keeps running.
    if (hue->updated())
      leds.hue = hue->getNewVal<float>();
    if (saturation->updated())
      leds.sat = saturation->getNewVal<float>();

    return true;
  }
};

MonitorLight *monitorLight;

// -------------------- HOMEKIT: EFFECT SWITCH (radio-button style) --------------------
struct EffectSwitch : Service::Switch {

  SpanCharacteristic *power;
  const char *label;
  LedMode effectMode;

  EffectSwitch(const char *label, LedMode m) : Service::Switch() {
    this->label = label;
    this->effectMode = m;
    power = new Characteristic::On(false);
  }

  boolean update() override {
    if (power->getNewVal()) {
      Serial.print("Effect via HomeKit: ");
      Serial.println(label);

      // From off: reset to warm white at 50% first, then start the effect
      if (!leds.isOn())
        resetToWarmDefaults();

      leds.setMode(effectMode);
      leds.setPower(true);
      setLightTile(true);
      syncEffectTiles(effectMode);  // turn the other effect tiles off
    } else if (leds.mode == effectMode) {
      // switching the active effect off -> back to solid color
      leds.setMode(MODE_SOLID);
      Serial.println("Effect off -> back to Solid");
    }
    return true;
  }
};

EffectSwitch *effectSwitches[MODE_COUNT];   // indexed by LedMode; MODE_SOLID entry stays null

// Reflect the currently-active mode onto the HomeKit effect tiles
void syncEffectTiles(LedMode activeMode) {
  for (int m = 0; m < MODE_COUNT; m++) {
    if (!effectSwitches[m]) continue;
    bool shouldBeOn = (m == activeMode);
    if (effectSwitches[m]->power->getVal() != shouldBeOn)
      effectSwitches[m]->power->setVal(shouldBeOn, true);
  }
}

void setLightTile(bool on) {
  if (monitorLight->power->getVal() != on)
    monitorLight->power->setVal(on, true);
}

// Solid warm white at 50%, mirrored to the HomeKit tiles. Called whenever the
// light turns on from off, whatever it was doing before.
void resetToWarmDefaults() {
  leds.hue = warmHue;
  leds.sat = warmSat;
  leds.val = 50;
  leds.setMode(MODE_SOLID);
  monitorLight->hue->setVal(warmHue, true);
  monitorLight->saturation->setVal(warmSat, true);
  monitorLight->level->setVal(50, true);
  syncEffectTiles(MODE_SOLID);
}

// -------------------- WARM WHITE SAVE / LOAD --------------------
void recomputeWarmRgb() {
  hsv2rgb(warmHue, warmSat, 100, &warmR, &warmG, &warmB);
}

void loadWarmWhite() {
  prefs.begin("deskled");
  warmHue = prefs.getFloat("warmH", WARM_HUE_DEFAULT);
  warmSat = prefs.getFloat("warmS", WARM_SAT_DEFAULT);
  recomputeWarmRgb();
  Serial.printf("Warm white loaded: hue=%.0f sat=%.0f -> RGB(%d,%d,%d)\n",
                warmHue, warmSat, warmR, warmG, warmB);
}

// '@W' in the Serial Monitor: set the color in the Home app first, then type @W
// to save it as the warm white.
void saveWarmWhite(const char *buf) {
  warmHue = leds.hue;
  warmSat = leds.sat;
  recomputeWarmRgb();
  prefs.putFloat("warmH", warmHue);
  prefs.putFloat("warmS", warmSat);
  Serial.printf("Warm white saved: hue=%.0f sat=%.0f -> RGB(%d,%d,%d)\n",
                warmHue, warmSat, warmR, warmG, warmB);
}

// Cycle to the next LED mode (used by the 5-tap gesture)
void cycleLedMode() {
  LedMode next = (LedMode)((leds.mode + 1) % MODE_COUNT);
  leds.setMode(next);
  leds.setPower(true);
  setLightTile(true);
  syncEffectTiles(next);
}

// -------------------- SETUP --------------------
void setup() {

  Serial.begin(115200);
  delay(500);

  pinMode(TOUCH_PIN, INPUT);

  irsend.begin();
  leds.begin();

  // Load warm white before building accessories so the light tile starts on it
  loadWarmWhite();
  leds.hue = warmHue;
  leds.sat = warmSat;

  // Multiple accessories -> HomeSpan requires a leading bare Bridge accessory
  homeSpan.begin(Category::Bridges, "Desk IR Controller");

  new SpanAccessory();                              // Bridge accessory (aid=1)
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Desk IR Bridge");

  new SpanAccessory();                              // Desk Switch
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Desk Switch");
    deskSwitch = new DeskSceneSwitch();

  new SpanAccessory();                              // TV Power button
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("TV Power");
    powerButton = new RemoteButton("Power", IR_POWER);

  new SpanAccessory();                              // Volume Up button
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Volume Up");
    volUpButton = new RemoteButton("Volume Up", IR_VOL_UP);

  new SpanAccessory();                              // Volume Down button
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Volume Down");
    volDownButton = new RemoteButton("Volume Down", IR_VOL_DOWN);

  new SpanAccessory();                              // Mode button
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Mode");
    modeButton = new RemoteButton("Mode", IR_MODE);

  new SpanAccessory();                              // Monitor Light (color bulb)
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Monitor Light");
    monitorLight = new MonitorLight();

  new SpanAccessory();                              // Effect: Chase Red
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Chase Red");
    effectSwitches[MODE_CHASE_RED] = new EffectSwitch("Chase Red", MODE_CHASE_RED);

  new SpanAccessory();                              // Effect: Chase Blue
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Chase Blue");
    effectSwitches[MODE_CHASE_BLUE] = new EffectSwitch("Chase Blue", MODE_CHASE_BLUE);

  new SpanAccessory();                              // Effect: Color Cycle
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Color Cycle");
    effectSwitches[MODE_ANCHOR] = new EffectSwitch("Color Cycle", MODE_ANCHOR);

  new SpanAccessory();                              // Effect: Fireflies
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Fireflies");
    effectSwitches[MODE_FIREFLY] = new EffectSwitch("Fireflies", MODE_FIREFLY);

  new SpanUserCommand('W', " - save the Monitor Light's current color as warm white (persists)", saveWarmWhite);

  Serial.println("System Ready");
  Serial.println("Tap: 1=Vol+, 2=Vol-, 3=Mode, 4=Light On/Off, 5=Cycle Light | Hold 1s=Desk ON, Hold 3s=Desk OFF");
}

// -------------------- TAP PROCESSOR --------------------
void processTaps() {

  if (tapCount == 0) return;
  if (millis() - lastTapTime < TAP_WINDOW) return;

  if (tapCount == 1) {
    Serial.println("1 TAP -> VOLUME UP");
    volUpButton->pressFromTouch();
  }
  else if (tapCount == 2) {
    Serial.println("2 TAPS -> VOLUME DOWN");
    volDownButton->pressFromTouch();
  }
  else if (tapCount == 3) {
    Serial.println("3 TAPS -> MODE");
    modeButton->pressFromTouch();
  }
  else if (tapCount == 4) {
    bool newState = !leds.isOn();
    Serial.print("4 TAPS -> LIGHT ");
    Serial.println(newState ? "ON" : "OFF");
    if (newState)
      resetToWarmDefaults();   // always come up in warm white at 50%
    leds.setPower(newState);
    setLightTile(newState);
  }
  else if (tapCount == 5) {
    Serial.println("5 TAPS -> CYCLE LIGHT MODE");
    cycleLedMode();
  }

  tapCount = 0;
}

// -------------------- LOOP --------------------
void loop() {

  homeSpan.poll();

  leds.tick();

  bool touched = digitalRead(TOUCH_PIN);

  // TOUCH START
  if (touched && !touchActive) {
    touchActive = true;
    touchStart = millis();
    hold1Fired = false;
    hold2Fired = false;
  }

  // WHILE HELD -> check hold thresholds
  if (touched && touchActive) {
    unsigned long held = millis() - touchStart;

    // Only act if the state would change. A 3s hold-to-off passes the 1s mark;
    // without the guard it would re-fire ON and re-arm the TV power IR.
    if (!hold1Fired && held >= HOLD1_MS) {
      hold1Fired = true;
      if (!deskSwitch->power->getVal()) {
        Serial.println("Hold 1s -> Desk Switch ON");
        deskSwitch->setFromTouch(true);
      }
    }

    if (!hold2Fired && held >= HOLD2_MS) {
      hold2Fired = true;
      if (deskSwitch->power->getVal()) {
        Serial.println("Hold 3s -> Desk Switch OFF");
        deskSwitch->setFromTouch(false);
      }
    }
  }

  // TOUCH END
  if (!touched && touchActive) {
    touchActive = false;

    // Only count as a tap if no hold gesture fired during this press
    if (!hold1Fired && !hold2Fired) {
      tapCount++;
      lastTapTime = millis();
      Serial.print("Tap registered: ");
      Serial.println(tapCount);
    }
  }

  processTaps();
}
