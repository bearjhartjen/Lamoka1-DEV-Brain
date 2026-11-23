#include <Arduino.h>
#include <FastLED.h>
#include "pico/bootrom.h"

/*
 * ================================================================
 * LAMOKA1 DEV BRAIN — PORTAL INTERFACE ENGINE V1.8
 * ================================================================
 * Full ten-LED DEV-Brain firmware.
 *
 * MAIN MENU
 * 0 = BLUE STATUS BALL
 * 1 = FIREPLACE
 * 2 = LIGHTSABER
 * 3 = RAINBOW
 * 4 = GAMES
 * 5 = SETTINGS
 *
 * Input: SW1 on GPIO0, wired active-low with INPUT_PULLUP.
 * LED output: GPIO11, ten WS2812B LEDs, GRB order.
 *
 * Button rules:
 * - short tap: one event after a debounced release
 * - hold: one event at 500 ms (or on release if held past the threshold)
 *
 * These are the only two gestures. There is no double-tap, long-hold, or
 * very-long-hold behavior. One physical press can never create both events.
 * No min() / max() library calls are used.
 */

/* ================================================================
 * HARDWARE / TIMING
 * ================================================================ */
#define LED_PIN             11
#define NUM_LEDS            10
#define BUTTON_PIN          0
#define COLOR_ENTROPY_PIN   5
#define ADC0                26
#define ADC1                27
#define ADC2                28
#define ADC3                29

#define DEFAULT_BRIGHTNESS  6
#define DEFAULT_SPEED       100
#define FRAME_MS            16UL
#define MENU_TIMEOUT_MS     16000UL

#define BUTTON_DEBOUNCE_MS  25UL
#define BUTTON_TAP_MIN_MS   30UL
#define BUTTON_HOLD_MS      500UL

CRGB leds[NUM_LEDS];

/* ================================================================
 * COLORS
 * ================================================================ */
const CRGB COLOR_CACTUS   = CRGB(0, 90, 18);
const CRGB COLOR_HOT_PINK = CRGB(255, 40, 160);
const CRGB COLOR_GAMES    = CRGB(255, 90, 0);
const CRGB COLOR_SETTINGS = CRGB(0, 50, 100);

const CRGB FIRE_GREEN  = CRGB(0, 180, 40);
const CRGB FIRE_RED    = CRGB(150, 0, 0);
const CRGB FIRE_ORANGE = CRGB(255, 45, 0);
const CRGB FIRE_HOT    = CRGB(255, 130, 10);
const CRGB FIRE_WHITE  = CRGB(255, 255, 255);

/* ================================================================
 * ENUMERATIONS
 * ================================================================ */
enum Gesture : uint8_t {
  G_NONE,
  G_TAP,
  G_HOLD
};

enum UIState : uint8_t {
  UI_MODE,
  UI_MENU_MAIN,
  UI_MENU_GAMES,
  UI_MENU_SETTINGS,
  UI_BRIGHTNESS,
  UI_SPEED,
  UI_INFO,
  UI_IN_GAME,
  UI_SLEEP
};

/* ================================================================
 * GLOBAL SETTINGS / UI STATE
 * ================================================================ */
uint8_t globalBrightness = DEFAULT_BRIGHTNESS;
uint8_t globalSpeed = DEFAULT_SPEED;

uint8_t mainCursor = 0;
uint8_t gamesCursor = 0;
uint8_t settingsCursor = 0;
uint8_t currentMainMode = 0;
uint8_t settingsReturnCursor = 0;
uint32_t menuLastActivity = 0;
UIState uiState = UI_MODE;

/* ================================================================
 * LED HELPERS
 * ================================================================ */
void applyBrightness() {
  uint8_t brightness = globalBrightness;
  if (brightness < 6) brightness = 6;
  FastLED.setBrightness(brightness);
}

void clearLeds() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}

CRGB blendColor(const CRGB &a, const CRGB &b, uint8_t amount) {
  return CRGB(
    lerp8by8(a.r, b.r, amount),
    lerp8by8(a.g, b.g, amount),
    lerp8by8(a.b, b.b, amount)
  );
}

void renderCursor(uint8_t index) {
  if (index < NUM_LEDS) leds[index] = COLOR_HOT_PINK;
}

/* ================================================================
 * ENTROPY SERVICE
 * ================================================================ */
class EntropyService {
  uint32_t s = 1;

  void mix() {
    const uint32_t a = analogRead(ADC0);
    const uint32_t b = analogRead(ADC1);
    const uint32_t c = analogRead(ADC2);
    const uint32_t d = analogRead(ADC3);
    s ^= (a << 16);
    s ^= b;
    s ^= (c << 8);
    s ^= d;
    s ^= micros();
    s ^= ((uint32_t)digitalRead(COLOR_ENTROPY_PIN) << 23);
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    s += 0x9E3779B9UL;
  }

public:
  void begin() {
    pinMode(ADC0, INPUT);
    pinMode(ADC1, INPUT);
    pinMode(ADC2, INPUT);
    pinMode(ADC3, INPUT);
    pinMode(COLOR_ENTROPY_PIN, INPUT);
    s ^= micros();
    for (uint16_t i = 0; i < 300; i++) mix();
  }

  void update() { mix(); }

  uint32_t next() {
    mix();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }

  uint8_t next8() { return (uint8_t)(next() & 255UL); }

  uint16_t range(uint16_t maximum) {
    if (maximum == 0) return 0;
    return (uint16_t)(next() % ((uint32_t)maximum + 1UL));
  }

  bool coin() { return (next() & 1UL) != 0; }
};

EntropyService entropy;

/* ================================================================
 * BUTTON SERVICE — ONE EVENT PER PHYSICAL PRESS
 * ================================================================ */
class ButtonService {
  bool lastRaw = HIGH;
  bool stableRaw = HIGH;
  bool buttonPressed = false;
  bool holdSent = false;
  bool ignoreUntilRelease = false;
  uint32_t rawChangedAt = 0;
  uint32_t pressStart = 0;

public:
  void begin() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // SW1 is active-low: idle is HIGH, a press connects GPIO0 to ground.
    lastRaw = digitalRead(BUTTON_PIN);
    stableRaw = lastRaw;
    rawChangedAt = millis();
    buttonPressed = false;
    holdSent = false;

    // Do not turn a button held during power-up into a phantom hold event.
    ignoreUntilRelease = (stableRaw == LOW);
  }

  Gesture poll() {
    const bool raw = digitalRead(BUTTON_PIN);
    const uint32_t now = millis();

    // Record every raw edge, then wait for it to remain stable.
    if (raw != lastRaw) {
      lastRaw = raw;
      rawChangedAt = now;
    }
    if (now - rawChangedAt < BUTTON_DEBOUNCE_MS) return G_NONE;

    // Apply a debounced transition.
    if (raw != stableRaw) {
      stableRaw = raw;

      if (stableRaw == LOW) {
        if (ignoreUntilRelease) return G_NONE;
        buttonPressed = true;
        holdSent = false;
        pressStart = rawChangedAt;
        return G_NONE;
      }

      // Stable HIGH: the button has been released.
      if (ignoreUntilRelease) {
        ignoreUntilRelease = false;
        buttonPressed = false;
        holdSent = false;
        return G_NONE;
      }
      if (!buttonPressed) return G_NONE;

      buttonPressed = false;
      const uint32_t heldMs = rawChangedAt - pressStart;

      // If the hold threshold was crossed, it was (or is now) one hold.
      if (heldMs >= BUTTON_HOLD_MS) {
        const bool alreadySent = holdSent;
        holdSent = false;
        return alreadySent ? G_NONE : G_HOLD;
      }

      holdSent = false;
      if (heldMs >= BUTTON_TAP_MIN_MS) return G_TAP;
      return G_NONE;
    }

    // Emit a hold once while the electrical input is still LOW. Checking raw
    // prevents a release that is being debounced from becoming a false hold.
    if (buttonPressed && raw == LOW && !holdSent &&
        now - pressStart >= BUTTON_HOLD_MS) {
      holdSent = true;
      return G_HOLD;
    }

    return G_NONE;
  }
};
ButtonService button;

/* ================================================================
 * BASE MODE INTERFACE
 * ================================================================ */
class Mode {
public:
  virtual ~Mode() {}
  virtual void enter() {}
  virtual void update(Gesture) {}
  virtual void render() {}
};

/* ================================================================
 * STATUS — BLUE BOUNCING BALL
 * ================================================================ */
class ModeStatus : public Mode {
  int8_t position = 0;
  int8_t direction = 1;
  uint32_t lastStep = 0;

public:
  void enter() override {
    position = 0;
    direction = 1;
    lastStep = millis();
  }

  void render() override {
    clearLeds();
    const uint16_t interval = (uint16_t)(28 + 8000 / (globalSpeed + 50));
    if (millis() - lastStep >= interval) {
      lastStep = millis();
      position += direction;
      if (position >= NUM_LEDS - 1) {
        position = NUM_LEDS - 1;
        direction = -1;
      }
      if (position <= 0) {
        position = 0;
        direction = 1;
      }
    }

    for (int trail = 0; trail < 3; trail++) {
      const int led = position - direction * trail;
      if (led >= 0 && led < NUM_LEDS) {
        leds[led] = CRGB(0, 25 + trail * 35, 70 + trail * 55);
      }
    }
    leds[position] = CRGB(0, 70, 255);
  }
};

/* ================================================================
 * FIREPLACE — REALISTIC HEAT MODEL
 * ================================================================ */
class ModeFire : public Mode {
  uint8_t heat[NUM_LEDS];
  int16_t source = 4;
  int16_t target = 4;
  int8_t sourceVelocity = 0;
  uint32_t lastMove = 0;
  uint8_t flare = 0;

  void chooseTarget() {
    target = (int16_t)(1 + entropy.range(6));
  }

public:
  void enter() override {
    memset(heat, 0, sizeof(heat));
    source = target = 4;
    sourceVelocity = 0;
    lastMove = millis();
    flare = 0;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      heat[i] = (uint8_t)(30 + entropy.range(50));
    }
  }

  void update(Gesture gesture) override {
    if (gesture == G_TAP) flare = 160;
  }

  void render() override {
    const uint32_t now = millis();

    if (now - lastMove > 180UL + entropy.range(220)) {
      chooseTarget();
      lastMove = now;
      sourceVelocity = (int8_t)((int)entropy.range(3) - 1);
    }

    if (source < target) source++;
    else if (source > target) source--;
    source += sourceVelocity;
    if (source < 1) { source = 1; sourceVelocity = 0; }
    if (source > 8) { source = 8; sourceVelocity = 0; }
    if (sourceVelocity) {
      sourceVelocity = sourceVelocity > 0 ? sourceVelocity - 1 : sourceVelocity + 1;
    }

    const uint8_t wind1 = inoise8(now / 70);
    const uint8_t wind2 = inoise8(now / 140 + 50);
    const int8_t drift = (int8_t)(((int)wind1 - 128) / 28 + ((int)wind2 - 128) / 40);

    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      const uint8_t cooling = (uint8_t)(2 + entropy.range(3) + (i * 3) / 8);
      heat[i] = heat[i] > cooling ? heat[i] - cooling : 0;
    }

    int16_t rawSource = source + drift;
    if (rawSource < 0) rawSource = 0;
    if (rawSource > NUM_LEDS - 1) rawSource = NUM_LEDS - 1;
    const uint8_t sourcePosition = (uint8_t)rawSource;

    const uint8_t fuel = (uint8_t)(120 + entropy.range(70) + (flare ? 40 : 0));
    heat[sourcePosition] = qadd8(heat[sourcePosition], fuel);
    if (sourcePosition > 0) heat[sourcePosition - 1] = qadd8(heat[sourcePosition - 1], fuel / 2);
    if (sourcePosition < NUM_LEDS - 1) heat[sourcePosition + 1] = qadd8(heat[sourcePosition + 1], fuel / 3);

    uint8_t oldHeat[NUM_LEDS];
    memcpy(oldHeat, heat, sizeof(heat));
    for (uint8_t i = 1; i < NUM_LEDS - 1; i++) {
      const uint16_t average = (uint16_t)oldHeat[i - 1] + (oldHeat[i] * 2) + oldHeat[i + 1];
      heat[i] = lerp8by8(oldHeat[i], (uint8_t)(average / 4), 160);
    }

    for (int i = NUM_LEDS - 1; i >= 1; i--) {
      uint16_t rising = (uint16_t)heat[i - 1] * 3 / 2;
      if (i >= 2) rising += heat[i - 2] / 2;
      const uint8_t risingHeat = (uint8_t)(rising > 255 ? 255 : rising);
      heat[i] = lerp8by8(heat[i], risingHeat, 180);
    }

    if (entropy.next8() > 200) {
      int q = sourcePosition + (int)entropy.range(4) - 1;
      if (q < 1) q = 1;
      if (q > 8) q = 8;
      const uint8_t sparkHeat = (uint8_t)(70 + entropy.range(120));
      heat[q] = qadd8(heat[q], sparkHeat);
      if (q < 9) heat[q + 1] = qadd8(heat[q + 1], sparkHeat / 2);
      if (q < 8 && entropy.coin()) heat[q + 2] = qadd8(heat[q + 2], sparkHeat / 4);
    }

    if (entropy.next8() > 245) {
      const uint8_t spark = (uint8_t)(5 + entropy.range(4));
      heat[spark] = qadd8(heat[spark], (uint8_t)(100 + entropy.range(80)));
    }

    if (flare) {
      heat[sourcePosition] = qadd8(heat[sourcePosition], flare);
      if (sourcePosition < NUM_LEDS - 1) {
        heat[sourcePosition + 1] = qadd8(heat[sourcePosition + 1], flare * 2 / 3);
      }
      if (sourcePosition < 8) heat[sourcePosition + 2] = qadd8(heat[sourcePosition + 2], flare / 3);
      flare = flare > 9 ? flare - 9 : 0;
    }

    if (heat[0] < 40) heat[0] = (uint8_t)(40 + entropy.range(20));

    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      const uint8_t noise = inoise8(now / 32 + i * 41);
      heat[i] = scale8(heat[i], (uint8_t)(210 + (noise >> 4)));
      const uint8_t h = heat[i];

      if (h < 20) {
        leds[i] = blendColor(CRGB(4, 0, 0), CRGB(30, 1, 0), (uint8_t)(h * 12));
      } else if (h < 55) {
        leds[i] = blendColor(CRGB(30, 1, 0), CRGB(120, 8, 0), scale8((uint8_t)(h - 20), 255));
      } else if (h < 100) {
        leds[i] = blendColor(CRGB(120, 8, 0), CRGB(255, 40, 0), scale8((uint8_t)(h - 55), 255));
      } else if (h < 155) {
        leds[i] = blendColor(CRGB(255, 40, 0), CRGB(255, 140, 10), scale8((uint8_t)(h - 100), 255));
      } else if (h < 210) {
        leds[i] = blendColor(CRGB(255, 140, 10), CRGB(255, 220, 60), scale8((uint8_t)(h - 155), 255));
      } else {
        leds[i] = blendColor(CRGB(255, 220, 60), CRGB(255, 250, 200), scale8((uint8_t)(h - 210), 255));
      }
    }
  }
};

/* ================================================================
 * LIGHTSABER
 * ================================================================ */
class ModeSaber : public Mode {
  uint8_t hue = 0;
  uint8_t lastHue = 255;
  uint8_t saturation = 220;
  uint8_t pattern = 0;
  uint8_t instability = 30;
  uint8_t power = 0;
  uint8_t clash = 0;
  bool extending = true;
  uint32_t ignition = 0;

  void launch() {
    do { hue = entropy.next8(); } while (hue == lastHue);
    lastHue = hue;
    saturation = (uint8_t)(175 + entropy.range(65));
    pattern = (uint8_t)entropy.range(6);
    instability = (uint8_t)(20 + entropy.range(80));
    power = 0;
    extending = true;
    ignition = millis();
  }

public:
  void enter() override { launch(); }

  void update(Gesture gesture) override {
    if (gesture == G_TAP) {
      if (extending) extending = false;
      else launch();
    }
  }

  void render() override {
    clearLeds();
    uint8_t rate = (uint8_t)(7 + globalSpeed / 30);
    if (pattern == 1) rate += 4;
    if (pattern == 3 && rate > 2) rate--;

    if (extending) power = qadd8(power, rate);
    else power = qsub8(power, 10);

    const uint8_t length = scale8(power, 10);
    for (uint8_t i = 0; i < length; i++) {
      uint8_t brightness = qsub8(255, (uint8_t)(i * 11));
      if (pattern == 3 && entropy.next8() < instability) brightness = scale8(brightness, 120);
      if (pattern == 5 && entropy.next8() > 220) brightness = 255;
      if (pattern == 4) brightness = scale8(brightness, beatsin8(12, 145, 255));
      if (pattern == 2) brightness = scale8(brightness, beatsin8(9, 185, 255));
      if (pattern == 1 && i == 0) brightness = 255;

      leds[i] = CHSV(hue, saturation, brightness);
      if (i < 2) leds[i] = blendColor(leds[i], CRGB::White, pattern == 1 ? 95 : 75);
    }

    if (extending && millis() - ignition < 170 && length) {
      leds[0] = blendColor(leds[0], CRGB::White, 180);
    }

    if (clash) {
      for (uint8_t i = 0; i < length; i++) {
        if (entropy.next8() > 90) leds[i] = blendColor(leds[i], CRGB::White, 210);
      }
      clash--;
    }

    if (length && entropy.next8() > 220) {
      const uint8_t p = entropy.next8() % length;
      leds[p] = blendColor(leds[p], CRGB::White, 150);
    }
  }
};

/* ================================================================
 * RAINBOW
 * ================================================================ */
class ModeRainbow : public Mode {
  uint8_t hue = 0;
  uint8_t speed = 3;

public:
  void enter() override { hue = entropy.next8(); }

  void update(Gesture gesture) override {
    if (gesture == G_TAP) speed = (uint8_t)(speed % 10 + 1);
  }

  void render() override {
    uint8_t step = (uint8_t)(speed * (globalSpeed + 40) / 75);
    if (!step) step = 1;
    hue += step;
    fill_rainbow(leds, NUM_LEDS, hue, 16);
  }
};

/* ================================================================
 * MODE INSTANCES
 * ================================================================ */
ModeStatus modeStatus;
ModeFire modeFire;
ModeSaber modeSaber;
ModeRainbow modeRainbow;

Mode *mainModes[] = {
  &modeStatus,
  &modeFire,
  &modeSaber,
  &modeRainbow
};
/* ================================================================
 * FIREBALL GAME
 * ================================================================ */
class AppFireball {
  static const int TARGET_LED = 0;
  static const int START_SPEED = 115;
  static const int SPEEDUP = 10;
  static const int MIN_SPEED = 30;

  enum GameState : uint8_t {
    GAME_RUNNING,
    GAME_BLAST,
    GAME_SCORE,
    GAME_MISSED,
    GAME_OVER
  };

  GameState state = GAME_RUNNING;
  uint16_t score = 0;
  uint8_t lives = 3;
  int firePosition = NUM_LEDS - 1;
  int fireSpeed = START_SPEED;
  uint32_t lastMove = 0;
  uint32_t stateStart = 0;

  uint8_t displayedScore() const {
    return score > NUM_LEDS ? NUM_LEDS : (uint8_t)score;
  }

  void drawGame() {
    clearLeds();
    leds[TARGET_LED] = FIRE_GREEN;
    if (firePosition >= 0 && firePosition < NUM_LEDS) leds[firePosition] = FIRE_WHITE;
    if (firePosition + 1 < NUM_LEDS) leds[firePosition + 1] = FIRE_HOT;
    if (firePosition + 2 < NUM_LEDS) leds[firePosition + 2] = FIRE_ORANGE;
    if (firePosition + 3 < NUM_LEDS) leds[firePosition + 3] = FIRE_RED;
  }

  void beginRound() {
    firePosition = NUM_LEDS - 1;
    lastMove = millis();
    state = GAME_RUNNING;
  }

  void startBlast() {
    stateStart = millis();
    state = GAME_BLAST;
  }

  void catchFireball() {
    score++;
    fireSpeed -= SPEEDUP;
    if (fireSpeed < MIN_SPEED) fireSpeed = MIN_SPEED;
    startBlast();
  }

  void missFireball() {
    if (lives > 0) lives--;
    stateStart = millis();
    state = (lives == 0) ? GAME_OVER : GAME_MISSED;
  }

  void renderBlast() {
    clearLeds();
    uint32_t elapsed = millis() - stateStart;
    uint8_t frame = (uint8_t)(elapsed / 45UL);
    if (frame > 7) frame = 7;
    for (int i = 0; i < NUM_LEDS; i++) {
      const int distance = abs(i - TARGET_LED);
      if (distance <= frame) {
        int level = 255 - distance * 50 - frame * 18;
        if (level < 10) level = 10;
        leds[i] = CRGB((uint8_t)level, (uint8_t)(level / 2), (uint8_t)(level / 12));
      }
    }
  }

  void renderScore() {
    clearLeds();
    const uint8_t shown = displayedScore();
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      if (shown & (1U << i)) leds[i] = COLOR_CACTUS;
    }
  }

public:
  void enter() {
    score = 0;
    lives = 3;
    fireSpeed = START_SPEED;
    firePosition = NUM_LEDS - 1;
    lastMove = millis();
    state = GAME_RUNNING;
    stateStart = millis();
  }

  void update(Gesture gesture) {
    if (state == GAME_OVER) {
      if (gesture == G_TAP) enter();
      return;
    }
    if (state == GAME_BLAST || state == GAME_SCORE || state == GAME_MISSED) return;
    if (state == GAME_RUNNING && gesture == G_TAP && firePosition == TARGET_LED) {
      catchFireball();
    }
  }

  void render() {
    if (state == GAME_RUNNING) {
      const uint32_t now = millis();
      if (now - lastMove >= (uint32_t)fireSpeed) {
        lastMove = now;
        firePosition--;
        if (firePosition < TARGET_LED) missFireball();
      }
      if (state == GAME_RUNNING) drawGame();
      return;
    }

    if (state == GAME_BLAST) {
      const uint32_t elapsed = millis() - stateStart;
      renderBlast();
      if (elapsed >= 360) {
        state = GAME_SCORE;
        stateStart = millis();
      }
      return;
    }

    if (state == GAME_SCORE) {
      renderScore();
      if (millis() - stateStart >= 750) beginRound();
      return;
    }

    if (state == GAME_MISSED) {
      clearLeds();
      const uint32_t elapsed = millis() - stateStart;
      if (elapsed < 70) fill_solid(leds, NUM_LEDS, FIRE_RED);
      if (elapsed >= 140) beginRound();
      return;
    }

    if (state == GAME_OVER) {
      clearLeds();
      const uint8_t pulse = beatsin8(4, 30, 180);
      fill_solid(leds, NUM_LEDS, CRGB(pulse, 0, 0));
      const uint8_t shown = displayedScore();
      for (uint8_t i = 0; i < NUM_LEDS; i++) {
        if (shown & (1U << i)) leds[i] = COLOR_CACTUS;
      }
    }
  }
};

AppFireball appFireball;

/* ================================================================
 * MENU HELPERS
 * ================================================================ */
void touchMenu() {
  menuLastActivity = millis();
}

bool inAnyMenu() {
  return uiState == UI_MENU_MAIN ||
         uiState == UI_MENU_GAMES ||
         uiState == UI_MENU_SETTINGS ||
         uiState == UI_BRIGHTNESS ||
         uiState == UI_SPEED ||
         uiState == UI_INFO;
}

/* ================================================================
 * MAIN MENU
 * ================================================================ */
void renderMainMenu() {
  clearLeds();
  for (uint8_t i = 0; i < 4; i++) leds[i] = COLOR_CACTUS;
  leds[4] = COLOR_GAMES;
  leds[5] = COLOR_SETTINGS;
  renderCursor(mainCursor);
}

/* ================================================================
 * GAMES MENU
 * ================================================================ */
void renderGamesMenu() {
  clearLeds();
  leds[0] = CRGB(255, 100, 0);
  leds[1] = CRGB(255, 0, 0);
  renderCursor(gamesCursor);
}

/* ================================================================
 * SETTINGS MENU
 * ================================================================ */
const uint8_t SETTINGS_COUNT = 6;

void renderSettingsMenu() {
  clearLeds();
  leds[0] = COLOR_SETTINGS;
  leds[1] = COLOR_CACTUS;
  leds[2] = COLOR_CACTUS;
  leds[3] = COLOR_CACTUS;
  leds[4] = CRGB(180, 120, 0);
  leds[5] = CRGB(255, 0, 0);
  renderCursor(settingsCursor);
}

/* ================================================================
 * BRIGHTNESS
 * ================================================================ */
const uint8_t brightVals[10] = {6, 12, 20, 30, 42, 56, 72, 88, 106, 125};
uint8_t brightLevel = 0;

void renderBrightness() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    leds[i] = (i <= brightLevel) ? COLOR_HOT_PINK : COLOR_CACTUS;
  }
}

/* ================================================================
 * SPEED
 * ================================================================ */
const uint8_t speedVals[10] = {40, 55, 70, 85, 100, 120, 145, 170, 200, 240};
uint8_t speedLevel = 4;

void renderSpeed() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    leds[i] = (i <= speedLevel) ? COLOR_HOT_PINK : COLOR_CACTUS;
  }
}

/* ================================================================
 * INFO
 * ================================================================ */
uint8_t infoPage = 0;

void renderInfo() {
  clearLeds();
  if (infoPage == 0) {
    fill_solid(leds, NUM_LEDS, COLOR_CACTUS);
    leds[0] = COLOR_HOT_PINK;
  } else if (infoPage == 1) {
    const uint8_t count = (uint8_t)((millis() / 1000UL) % (NUM_LEDS + 1));
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      leds[i] = (i < count) ? COLOR_CACTUS : CRGB::Black;
    }
  } else {
    fill_solid(leds, NUM_LEDS, COLOR_CACTUS);
  }
}

/* ================================================================
 * SHUTDOWN
 * ================================================================ */
bool sleepWaitRelease = true;
uint32_t sleepStart = 0;

void enterShutdown() {
  uiState = UI_SLEEP;
  sleepWaitRelease = true;
  sleepStart = 0;
  clearLeds();
  FastLED.setBrightness(0);
  FastLED.show();
}

void updateShutdown() {
  clearLeds();
  FastLED.setBrightness(0);
  FastLED.show();

  if (sleepWaitRelease) {
    if (digitalRead(BUTTON_PIN) == HIGH) sleepWaitRelease = false;
    return;
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!sleepStart) sleepStart = millis();
    if (millis() - sleepStart >= BUTTON_HOLD_MS) {
      sleepStart = 0;
      applyBrightness();
      uiState = UI_MODE;
      currentMainMode = 0;
      mainCursor = 0;
      modeStatus.enter();
      sleepWaitRelease = true;
    }
  } else {
    sleepStart = 0;
  }
}

/* ================================================================
 * BOOTSEL
 * ================================================================ */
void enterBootsel() {
  clearLeds();
  FastLED.setBrightness(0);
  FastLED.show();
  delay(40);
  reset_usb_boot(0, 0);
  while (true) delay(1000);
}

/* ================================================================
 * RETURN TO DEFAULT IDLE
 * ================================================================ */
void returnToIdle() {
  uiState = UI_MODE;
  currentMainMode = 0;
  mainCursor = 0;
  modeStatus.enter();
}

/* ================================================================
 * UI HANDLER
 * ================================================================ */
void handleUI(Gesture gesture) {
  if (gesture == G_NONE) return;
  if (inAnyMenu()) touchMenu();

  // FIREBALL: tap attempts a catch/restarts Game Over; hold exits the game.
  if (uiState == UI_IN_GAME) {
    if (gesture == G_HOLD) {
      uiState = UI_MENU_GAMES;
      touchMenu();
      return;
    }
    appFireball.update(gesture);
    return;
  }

  // NORMAL MODE: hold opens the main menu. Status has no tap animation action,
  // so a short tap there also opens the menu. Other modes use tap for effects.
  if (uiState == UI_MODE) {
    if (gesture == G_HOLD ||
        (gesture == G_TAP && currentMainMode == 0)) {
      mainCursor = currentMainMode;
      uiState = UI_MENU_MAIN;
      touchMenu();
      return;
    }
    mainModes[currentMainMode]->update(gesture);
    return;
  }

  // HOME MENU: short tap moves forward; hold selects.
  if (uiState == UI_MENU_MAIN) {
    if (gesture == G_TAP) {
      mainCursor = (uint8_t)((mainCursor + 1) % 6);
      return;
    }
    if (gesture == G_HOLD) {
      switch (mainCursor) {
        case 0:
          currentMainMode = 0;
          modeStatus.enter();
          uiState = UI_MODE;
          return;
        case 1:
          currentMainMode = 1;
          modeFire.enter();
          uiState = UI_MODE;
          return;
        case 2:
          currentMainMode = 2;
          modeSaber.enter();
          uiState = UI_MODE;
          return;
        case 3:
          currentMainMode = 3;
          modeRainbow.enter();
          uiState = UI_MODE;
          return;
        case 4:
          uiState = UI_MENU_GAMES;
          touchMenu();
          return;
        case 5:
          uiState = UI_MENU_SETTINGS;
          touchMenu();
          return;
      }
    }
    return;
  }

  // GAMES MENU: short tap changes item; hold selects.
  if (uiState == UI_MENU_GAMES) {
    if (gesture == G_TAP) {
      gamesCursor = (uint8_t)((gamesCursor + 1) % 2);
      return;
    }
    if (gesture == G_HOLD) {
      if (gamesCursor == 0) {
        uiState = UI_MENU_MAIN;
        mainCursor = 4;
        touchMenu();
        return;
      }
      appFireball.enter();
      uiState = UI_IN_GAME;
      touchMenu();
      return;
    }
    return;
  }

  // SETTINGS MENU: short tap changes item; hold selects.
  if (uiState == UI_MENU_SETTINGS) {
    if (gesture == G_TAP) {
      settingsCursor = (uint8_t)((settingsCursor + 1) % SETTINGS_COUNT);
      return;
    }
    if (gesture == G_HOLD) {
      if (settingsCursor == 0) {
        uiState = UI_MENU_MAIN;
        mainCursor = 5;
        touchMenu();
        return;
      }
      if (settingsCursor == 1) {
        settingsReturnCursor = 1;
        uint8_t best = 255;
        for (uint8_t i = 0; i < 10; i++) {
          const uint8_t difference = (uint8_t)abs((int)brightVals[i] - (int)globalBrightness);
          if (difference < best) {
            best = difference;
            brightLevel = i;
          }
        }
        uiState = UI_BRIGHTNESS;
        touchMenu();
        return;
      }
      if (settingsCursor == 2) {
        settingsReturnCursor = 2;
        uint8_t best = 255;
        for (uint8_t i = 0; i < 10; i++) {
          const uint8_t difference = (uint8_t)abs((int)speedVals[i] - (int)globalSpeed);
          if (difference < best) {
            best = difference;
            speedLevel = i;
          }
        }
        uiState = UI_SPEED;
        touchMenu();
        return;
      }
      if (settingsCursor == 3) {
        settingsReturnCursor = 3;
        infoPage = 0;
        uiState = UI_INFO;
        touchMenu();
        return;
      }
      if (settingsCursor == 4) {
        enterBootsel();
        return;
      }
      if (settingsCursor == 5) {
        enterShutdown();
        return;
      }
    }
    return;
  }

  // BRIGHTNESS: tap advances level; hold returns to Settings.
  if (uiState == UI_BRIGHTNESS) {
    if (gesture == G_TAP) {
      brightLevel = (uint8_t)((brightLevel + 1) % 10);
      globalBrightness = brightVals[brightLevel];
      applyBrightness();
      touchMenu();
      return;
    }
    if (gesture == G_HOLD) {
      uiState = UI_MENU_SETTINGS;
      settingsCursor = settingsReturnCursor;
      touchMenu();
      return;
    }
  }

  // SPEED: tap advances level; hold returns to Settings.
  if (uiState == UI_SPEED) {
    if (gesture == G_TAP) {
      speedLevel = (uint8_t)((speedLevel + 1) % 10);
      globalSpeed = speedVals[speedLevel];
      touchMenu();
      return;
    }
    if (gesture == G_HOLD) {
      uiState = UI_MENU_SETTINGS;
      settingsCursor = settingsReturnCursor;
      touchMenu();
      return;
    }
  }

  // INFO: tap advances page; hold returns to Settings.
  if (uiState == UI_INFO) {
    if (gesture == G_TAP) {
      infoPage = (uint8_t)((infoPage + 1) % 3);
      touchMenu();
      return;
    }
    if (gesture == G_HOLD) {
      uiState = UI_MENU_SETTINGS;
      settingsCursor = settingsReturnCursor;
      touchMenu();
      return;
    }
  }
}

/* ================================================================
 * RENDER UI
 * ================================================================ */
void renderUI() {
  switch (uiState) {
    case UI_MODE:          mainModes[currentMainMode]->render(); break;
    case UI_MENU_MAIN:     renderMainMenu(); break;
    case UI_MENU_GAMES:    renderGamesMenu(); break;
    case UI_MENU_SETTINGS: renderSettingsMenu(); break;
    case UI_BRIGHTNESS:    renderBrightness(); break;
    case UI_SPEED:         renderSpeed(); break;
    case UI_INFO:          renderInfo(); break;
    case UI_IN_GAME:       appFireball.render(); break;
    case UI_SLEEP:         clearLeds(); break;
  }
}

/* ================================================================
 * SETUP
 * ================================================================ */
void setup() {
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setDither(0);
  applyBrightness();
  FastLED.clear(true);

  entropy.begin();
  button.begin();

  // Startup sweep
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(i * 25, 230, 210);
    FastLED.show();
    delay(18);
  }
  delay(70);
  clearLeds();
  FastLED.show();

  currentMainMode = 0;
  mainCursor = 0;
  uiState = UI_MODE;
  modeStatus.enter();
}

/* ================================================================
 * LOOP
 * ================================================================ */
void loop() {
  static uint32_t lastFrame = 0;
  const uint32_t now = millis();

  if (uiState == UI_SLEEP) {
    updateShutdown();
    delay(5);
    return;
  }

  entropy.update();

  if (inAnyMenu() && now - menuLastActivity > MENU_TIMEOUT_MS) {
    returnToIdle();
  }

  const Gesture gesture = button.poll();
  if (gesture != G_NONE) handleUI(gesture);

  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    renderUI();
    FastLED.show();
  }
}
