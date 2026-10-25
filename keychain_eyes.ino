/*
  ESP32-C3 Super Mini - animated keychain eyes
  --------------------------------------------
  Press the button to cycle moods:
    0 Normal     breathing, wandering gaze, natural (double) blinks
    1 Happy      bouncing ^ ^ eyes, giggles, winks, twinkling sparkles
    2 Angry      slanted lids, trembling, periodic rage flare-ups, side glares
    3 Sleepy     drooping lids, head nods, jolts awake, dozes off with floating Zzz
    4 Surprised  big round eyes that "pop", tiny shaking pupils, darting looks, "!"

  How it works
    * Everything that moves is a small spring (see Spring struct), so eyes
      accelerate, overshoot and settle instead of jumping between positions.
    * No delay() calls - the loop runs a ~30 FPS animation clock and keeps
      polling the button the whole time.
    * Changing mood = eyes blink shut, the mood swaps while they are closed,
      then they re-open with a mood-specific "entry kick".

  Wiring (unchanged): SDA = GPIO5, SCL = GPIO4, button = GPIO3 -> GND
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------------------------------------------------------- hardware
#define OLED_WIDTH   128      // change to 128x32 if that is your panel
#define OLED_HEIGHT  64
#define OLED_SDA     3
#define OLED_SCL     4
#define BUTTON_PIN   9
#define OLED_ADDRESS 0x3C
#define DEBOUNCE_MS  35
#define FRAME_MS     33       // target frame time (~30 FPS)

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

enum Mood {
  MOOD_NORMAL = 0,
  MOOD_HAPPY,
  MOOD_ANGRY,
  MOOD_SLEEPY,
  MOOD_SURPRISED,
  MOOD_COUNT
};

// ---------------------------------------------------------------- helpers
static inline int   imin(int a, int b) { return a < b ? a : b; }
static inline int   imax(int a, int b) { return a > b ? a : b; }
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline int iround(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }
// random float in [a, b]
static inline float frand(float a, float b) {
  return a + (b - a) * (random(0, 10001) / 10000.0f);
}

// ---------------------------------------------------------------- springs
// A damped spring chases `target`. Low damping = bouncy, high = smooth.
struct Spring {
  float pos, vel, target, k, c;
  void init(float p, float stiffness, float damping) {
    pos = target = p; vel = 0; k = stiffness; c = damping;
  }
  void step(float dt) {
    float acc = (target - pos) * k - vel * c;
    vel += acc * dt;
    pos += vel * dt;
  }
  void snap() { pos = target; vel = 0; }
};

Spring sW, sH, sGap;            // eye width / height (arch rise when happy) / gap
Spring sLookX, sLookY;          // gaze direction  -1..1
Spring sLidTop;                 // flat top-lid, 0..1 of eye height
Spring sSlant;                  // +1 angry (inner corners down), -1 tired (outer corners down)
Spring sPupil;                  // pupil size multiplier
Spring sRound;                  // corner roundness 0..1
Spring sOffX, sOffY;            // whole-face offset in pixels
Spring sBlink;                  // 0 open .. 1 shut (slightly negative = wide-open overshoot)
Spring sWink;                   // right-eye wink (happy)

Spring* const shapeSprings[] = {
  &sW, &sH, &sGap, &sLookX, &sLookY, &sLidTop, &sSlant,
  &sPupil, &sRound, &sOffX, &sOffY, &sWink
};
const int NUM_SHAPE = sizeof(shapeSprings) / sizeof(shapeSprings[0]);

// ---------------------------------------------------------------- state
int  SW, SH;                    // screen size
float baseW, baseH, baseGap;    // default eye geometry (scales with the screen)

int  mood = MOOD_NORMAL;
int  pendingMood = MOOD_NORMAL;
bool transitioning = false;
unsigned long moodStart = 0;

unsigned long nextBlinkAt = 0, nextLookAt = 0, nextEventAt = 0;
bool doubleBlinkPending = false;
bool glanceBack = false;

// per-frame overlays (set by the mood logic, applied in drawFrame)
float fxX = 0, fxY = 0, fxH = 0;   // shake / bounce / breathing
float pjx = 0, pjy = 0;            // pupil jitter in pixels

// angry
bool  flaring = false;
unsigned long flareEnd = 0;
float shake = 0;

// happy
unsigned long giggleUntil = 0, winkUntil = 0;

// sleepy
enum SleepPhase { SLP_DROOP, SLP_DOZE, SLP_WAKE };
SleepPhase sleepPhase = SLP_DROOP;
unsigned long phaseStart = 0, phaseDur = 3000;
bool zOn = false;

// surprised
unsigned long bangShowAt = 0, bangUntil = 0;

// button
int lastRawButtonState = HIGH;
int stableButtonState  = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long lastFrame = 0;

// ---------------------------------------------------------------- setup of springs
void initSprings() {
  sW.init(baseW, 220, 15);          // underdamped: eyes "pop"
  sH.init(baseH, 220, 15);
  sGap.init(baseGap, 150, 22);
  sLookX.init(0, 300, 30);          // quick, soft-landing eye saccades
  sLookY.init(0, 300, 30);
  sLidTop.init(0, 140, 20);         // slow, smooth lids
  sSlant.init(0, 160, 22);
  sPupil.init(1, 200, 24);
  sRound.init(0.62f, 160, 22);
  sOffX.init(0, 180, 14);           // bouncy
  sOffY.init(0, 180, 14);
  sBlink.init(1, 700, 36);          // very fast, tiny overshoot on opening
  sWink.init(0, 400, 30);
}

// Resting target shape for a mood
void setBase(int m) {
  sLookX.target = 0;  sLookY.target = 0;
  sOffX.target  = 0;  sOffY.target  = 0;
  sLidTop.target = 0; sSlant.target = 0;
  sPupil.target = 1.0f;
  sRound.target = 0.62f;
  sW.target = baseW; sH.target = baseH; sGap.target = baseGap;
  sWink.target = 0;

  switch (m) {
    case MOOD_HAPPY:
      sW.target = baseW * 1.05f;
      sH.target = baseH * 0.50f;          // arch height
      sGap.target = baseGap * 1.3f;
      sPupil.target = 0;
      break;

    case MOOD_ANGRY:
      sW.target = baseW * 1.05f;
      sH.target = baseH * 0.80f;
      sGap.target = baseGap * 0.75f;
      sRound.target = 0.35f;
      sSlant.target = 0.8f;
      sLidTop.target = 0.10f;
      sPupil.target = 0.75f;
      break;

    case MOOD_SLEEPY:
      sH.target = baseH * 0.92f;
      sRound.target = 0.8f;
      sLidTop.target = 0.30f;
      sSlant.target = -0.35f;
      sPupil.target = 0.9f;
      break;

    case MOOD_SURPRISED: {
      float big = SH * 0.68f;
      float cap = SW * 0.36f;
      if (big > cap) big = cap;
      sW.target = big; sH.target = big;
      sGap.target = baseGap * 0.8f;
      sRound.target = 1.0f;
      sPupil.target = 0.5f;
    } break;

    default: break;
  }
}

// (takes an int on purpose: the Arduino IDE auto-generates prototypes above the
//  enum declarations, so a custom enum type in a signature would not compile)
void setSleepPhase(int p, unsigned long now) {
  sleepPhase = (SleepPhase)p;
  phaseStart = now;
  if (p == SLP_DROOP)      phaseDur = random(3000, 4800);
  else if (p == SLP_DOZE)  phaseDur = random(2200, 3600);
  else                     phaseDur = 1400;
}

// Called at the moment the eyes are shut: loads the new mood.
void enterMood(unsigned long now) {
  moodStart = now;
  flaring = false; shake = 0; zOn = false;
  giggleUntil = 0; winkUntil = 0;
  bangShowAt = 0; bangUntil = 0;
  doubleBlinkPending = false; glanceBack = false;
  nextBlinkAt = now + random(1500, 4000);
  nextLookAt  = now + random(600, 2000);
  nextEventAt = now + 2000;

  setBase(mood);
  for (int i = 0; i < NUM_SHAPE; i++) shapeSprings[i]->snap();   // invisible: eyes are shut

  switch (mood) {
    case MOOD_HAPPY:
      sOffY.vel = -70;                 // little hop
      giggleUntil = now + 700;         // instant giggle
      nextEventAt = now + 1400;
      break;
    case MOOD_ANGRY:
      nextEventAt = now + 150;         // flare up right away
      break;
    case MOOD_SLEEPY:
      setSleepPhase(SLP_DROOP, now);
      break;
    case MOOD_SURPRISED:
      sW.pos = sW.target * 0.55f;      // spring pops them wide open
      sH.pos = sH.target * 0.55f;
      sOffY.vel = -40;
      bangShowAt = now + 350;
      bangUntil  = now + 1700;
      nextEventAt = now + 2600;
      nextLookAt  = now + 1000;
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- mood behaviour
void startBlink() { sBlink.target = 1.0f; }

void updateNormal(unsigned long now, float t) {
  fxH = 1.6f * sinf(t * 2.2f);                 // breathing
  fxY = 0.8f * sinf(t * 1.6f + 1.0f);

  if (now >= nextBlinkAt) {
    startBlink();
    if (doubleBlinkPending) {
      doubleBlinkPending = false;
      nextBlinkAt = now + random(2200, 5200);
    } else if (random(100) < 22) {
      doubleBlinkPending = true;               // quick double blink
      nextBlinkAt = now + 260;
    } else {
      nextBlinkAt = now + random(2200, 5200);
    }
  }

  if (now >= nextLookAt) {
    if (glanceBack) {                          // second half of a quick glance
      sLookX.target = -sLookX.target;
      glanceBack = false;
      nextLookAt = now + random(1200, 2800);
    } else {
      int r = random(100);
      if (r < 25) {
        sLookX.target = 0; sLookY.target = 0;
        nextLookAt = now + random(900, 2400);
      } else if (r < 85) {
        sLookX.target = frand(-1.0f, 1.0f);
        sLookY.target = frand(-0.7f, 0.7f);
        nextLookAt = now + random(900, 3200);
      } else {
        sLookX.target = random(2) ? -1.0f : 1.0f;
        sLookY.target = 0;
        glanceBack = true;
        nextLookAt = now + 520;
      }
    }
  }
}

void updateHappy(unsigned long now, float t) {
  fxY = -fabsf(sinf(t * 4.6f)) * 3.2f + 1.2f;  // bouncing
  fxX = sinf(t * 2.3f) * 1.2f;                 // happy sway

  if (now < giggleUntil) {                     // giggle shake that fades out
    float k = (giggleUntil - now) / 700.0f;
    fxX += sinf(t * 42.0f) * 2.6f * k;
  }

  if (now >= nextEventAt) {
    if (random(100) < 50) {
      giggleUntil = now + 700;
    } else {
      winkUntil = now + 420;
      sWink.target = 1.0f;
    }
    nextEventAt = now + random(2200, 4500);
  }
  if (winkUntil && now >= winkUntil) { sWink.target = 0; winkUntil = 0; }
}

void updateAngry(unsigned long now, float t) {
  if (!flaring && now >= nextEventAt) { flaring = true; flareEnd = now + 750; }

  if (flaring) {
    sSlant.target = 1.0f;   sLidTop.target = 0.16f;
    sH.target = baseH * 0.66f;  sW.target = baseW * 1.08f;
    sGap.target = baseGap * 0.55f;
    sPupil.target = 0.5f;
    sLookX.target = 0;      sLookY.target = 0.3f;
    shake = 2.0f;
    if (now >= flareEnd) {
      flaring = false;
      nextEventAt = now + random(2600, 5200);
    }
  } else {
    sSlant.target = 0.8f;   sLidTop.target = 0.10f;
    sH.target = baseH * 0.80f;  sW.target = baseW * 1.05f;
    sGap.target = baseGap * 0.75f;
    sPupil.target = 0.75f;
    shake = 0.45f;

    if (now >= nextLookAt) {                   // side-eye glares
      sLookX.target = (random(3) == 0) ? 0.0f : (random(2) ? -0.9f : 0.9f);
      sLookY.target = 0.2f;
      nextLookAt = now + random(700, 1800);
    }
    if (now >= nextBlinkAt) {                  // rare, slow blink
      startBlink();
      nextBlinkAt = now + random(5000, 9000);
    }
  }

  fxH = 1.2f * sinf(t * 5.0f);                 // heavy breathing
  fxX = frand(-1.0f, 1.0f) * shake;            // trembling
  fxY = frand(-1.0f, 1.0f) * shake * 0.6f;
  pjx = frand(-1.0f, 1.0f) * shake * 0.5f;
  pjy = frand(-1.0f, 1.0f) * shake * 0.5f;
}

void updateSleepy(unsigned long now, float t) {
  float p = clampf((now - phaseStart) / (float)phaseDur, 0.0f, 1.0f);

  switch (sleepPhase) {
    case SLP_DROOP:
      sLidTop.target = 0.30f + 0.52f * p * p;  // slowly sinking lids
      sSlant.target  = -0.35f;
      sOffY.target   = 4.0f * p;               // head sinks
      sLookY.target  = 0.5f;
      sLookX.target  = 0;
      zOn = (p > 0.55f);
      if (p >= 1.0f) {
        if (random(100) < 55) {
          setSleepPhase(SLP_DOZE, now);        // dozes off completely
        } else {
          setSleepPhase(SLP_WAKE, now);        // head-nod jolt
          sOffY.vel = -60;
          sLidTop.vel = -3.0f;
        }
      }
      break;

    case SLP_DOZE:
      sLidTop.target = 0.90f;
      sSlant.target  = 0;
      sOffY.target   = 5.0f;
      sLookY.target  = 0.3f;
      zOn = true;
      fxY = sinf(t * 1.6f) * 1.2f;             // slow sleeping breath
      fxH = sinf(t * 1.6f) * 0.8f;
      if (p >= 1.0f) setSleepPhase(SLP_WAKE, now);   // gentle wake-up
      break;

    case SLP_WAKE:
      sLidTop.target = (p < 0.55f) ? 0.05f : 0.30f;
      sSlant.target  = (p < 0.55f) ? 0.0f : -0.35f;
      sOffY.target   = (p < 0.55f) ? -1.5f : 0.0f;
      sLookY.target  = 0;
      zOn = false;
      if (p >= 1.0f) setSleepPhase(SLP_DROOP, now);
      break;
  }
  fxX += sinf(t * 0.8f) * 1.2f;                // sleepy sway
}

void updateSurprised(unsigned long now, float t) {
  unsigned long el = now - moodStart;

  pjx = frand(-0.8f, 0.8f);                    // shaking pupils
  pjy = frand(-0.8f, 0.8f);
  fxH = 0.5f * sinf(t * 18.0f);                // heartbeat shiver

  if (el < 900) {
    sLookX.target = 0; sLookY.target = 0;      // frozen stare
  } else if (now >= nextLookAt) {              // nervous darting
    sLookX.target = (random(2) ? -1.0f : 1.0f) * frand(0.6f, 1.0f);
    sLookY.target = frand(-0.5f, 0.3f);
    nextLookAt = now + random(450, 1100);
  }

  if (el > 1500 && now >= nextEventAt) {       // double-take: squash, then pop again
    sW.pos = sW.target * 0.8f;
    sH.pos = sH.target * 0.8f;
    sW.vel = 0; sH.vel = 0;
    bangShowAt = now + 300;
    bangUntil  = now + 1300;
    nextEventAt = now + random(2500, 4500);
  }

  if (now >= nextBlinkAt) {                    // rare slow blink
    startBlink();
    nextBlinkAt = now + random(4000, 7000);
  }
}

// ---------------------------------------------------------------- drawing
void drawEye(int cx, int cy, int w, int h, bool left,
             float lookX, float lookY, float pupilScale,
             float lidFrac, float slant, float roundness) {
  if (w < 2 || h < 2) return;
  int x = cx - w / 2;
  int y = cy - h / 2;
  int r = (int)(imin(w, h) * 0.5f * roundness);

  display.fillRoundRect(x, y, w, h, r, SSD1306_WHITE);

  // pupil (travels on an ellipse so it never leaves the eye)
  int pr = iround(w * 0.19f * pupilScale);
  pr = imin(pr, h / 2 - 1);
  if (pr >= 1) {
    float mx = (float)imax(0, w / 2 - pr - 2);
    float my = (float)imax(0, h / 2 - pr - 2);
    float dx = lookX * mx, dy = lookY * my;
    float len = sqrtf((mx > 0 ? (dx * dx) / (mx * mx) : 0) +
                      (my > 0 ? (dy * dy) / (my * my) : 0));
    if (len > 1.0f) { dx /= len; dy /= len; }
    int px = cx + iround(dx + pjx);
    int py = cy + iround(dy + pjy);
    display.fillCircle(px, py, pr, SSD1306_BLACK);
    if (pr >= 3) {
      display.fillCircle(px - pr / 3, py - pr / 3, imax(1, pr / 3), SSD1306_WHITE);
    }
  }

  // eyelids: a black slanted quad laid over the top of the eye
  float flat = lidFrac * h;
  float inner = flat, outer = flat;
  if (slant > 0) inner += slant * h * 0.55f;       // angry
  else           outer += -slant * h * 0.45f;      // tired
  if (inner > 0.5f || outer > 0.5f) {
    int dl = iround(left ? outer : inner);
    int dr = iround(left ? inner : outer);
    dl = imin(dl, h - 2);
    dr = imin(dr, h - 2);
    int x0 = x - 1, x1 = x + w, yT = y - 1;
    display.fillTriangle(x0, yT, x1, yT, x1, y + dr, SSD1306_BLACK);
    display.fillTriangle(x0, yT, x1, y + dr, x0, y + dl, SSD1306_BLACK);
  }
}

// Happy "^" eye: a thick parabola with constant stroke width
void drawHappyEye(int cx, int cy, int w, int rise, int thick) {
  int half = w / 2;
  if (half < 2) return;
  int base = cy - thick / 2 - rise / 2;
  for (int dx = -half; dx <= half; dx++) {
    float u = (float)dx / (float)half;
    int yTop = base + iround(rise * u * u);
    float slope = 2.0f * rise * fabsf(u) / (float)half;
    int tt = iround(thick * sqrtf(1.0f + slope * slope));
    if (tt > thick * 2) tt = thick * 2;
    display.drawFastVLine(cx + dx, yTop - (tt - thick) / 2, tt, SSD1306_WHITE);
  }
}

void drawSparkle(int x, int y, int r) {
  display.drawFastHLine(x - r, y, 2 * r + 1, SSD1306_WHITE);
  display.drawFastVLine(x, y - r, 2 * r + 1, SSD1306_WHITE);
  if (r >= 3) {
    display.drawPixel(x - 1, y - 1, SSD1306_WHITE);
    display.drawPixel(x + 1, y - 1, SSD1306_WHITE);
    display.drawPixel(x - 1, y + 1, SSD1306_WHITE);
    display.drawPixel(x + 1, y + 1, SSD1306_WHITE);
  }
}

void drawSparkles(float t, bool burst) {
  static const float fx[4] = { 0.06f, 0.94f, 0.94f, 0.06f };
  static const float fy[4] = { 0.16f, 0.22f, 0.80f, 0.78f };
  for (int i = 0; i < 4; i++) {
    float s = sinf(t * 3.2f + i * 1.7f);
    if (s <= 0) continue;
    int r = 1 + (int)(s * (burst ? 5.0f : 3.0f));
    drawSparkle(iround(SW * fx[i]), iround(SH * fy[i]), r);
  }
}

// Classic "anger mark": four corner strokes with the corners pointing inward
void drawAngerMark(unsigned long now) {
  int s  = 3 + (int)((now / 110) % 2);
  int g  = 2;
  int cx = SW - 11;
  int cy = imax(9, (int)(SH * 0.17f));
  display.drawFastHLine(cx - g - s, cy - g, s + 1, SSD1306_WHITE);
  display.drawFastVLine(cx - g, cy - g - s, s + 1, SSD1306_WHITE);
  display.drawFastHLine(cx + g, cy - g, s + 1, SSD1306_WHITE);
  display.drawFastVLine(cx + g, cy - g - s, s + 1, SSD1306_WHITE);
  display.drawFastHLine(cx - g - s, cy + g, s + 1, SSD1306_WHITE);
  display.drawFastVLine(cx - g, cy + g, s + 1, SSD1306_WHITE);
  display.drawFastHLine(cx + g, cy + g, s + 1, SSD1306_WHITE);
  display.drawFastVLine(cx + g, cy + g, s + 1, SSD1306_WHITE);
}

void drawBang(unsigned long now) {
  int bob = (int)(fabsf(sinf(now * 0.012f)) * 2.0f);
  int x = SW - 7;
  int y = SH / 2 - 10 - bob;
  display.fillRoundRect(x, y, 3, 12, 1, SSD1306_WHITE);
  display.fillRect(x, y + 15, 3, 3, SSD1306_WHITE);
}

// Floating z Z Z
void drawZs(float t) {
  for (int i = 0; i < 3; i++) {
    float ph = fmodf(t * 0.42f + i * 0.34f, 1.0f);
    if (ph < 0.06f) continue;
    int x = iround(SW * 0.855f + ph * SW * 0.05f + sinf(ph * 9.0f) * 1.2f);
    int y = iround(SH * 0.58f - ph * SH * 0.46f);
    bool big = (ph >= 0.45f);
    display.setTextSize(big ? 2 : 1);
    display.setCursor(x, y);
    display.print(big ? 'Z' : 'z');
  }
  display.setTextSize(1);
}

void drawFrame(unsigned long now) {
  display.clearDisplay();
  const float t = (now - moodStart) / 1000.0f;

  const float blink = clampf(sBlink.pos, -0.25f, 1.0f);
  const float w     = sW.pos * (1.0f + 0.10f * blink);   // eyes widen a touch when squashed
  const float hRaw  = sH.pos + fxH;
  const float gap   = sGap.pos;

  const float lookShiftX = (mood == MOOD_SURPRISED) ? 3.0f : SW * 0.04f;
  const float shiftX = sLookX.pos * lookShiftX + sOffX.pos + fxX;
  const float shiftY = sLookY.pos * (SH * 0.05f) + sOffY.pos + fxY;

  const float cy  = SH * 0.5f + shiftY;
  const float cxL = SW * 0.5f - (gap + w) * 0.5f + shiftX;
  const float cxR = SW * 0.5f + (gap + w) * 0.5f + shiftX;

  if (mood == MOOD_HAPPY) {
    int rise  = imax(2, iround(hRaw * (1.0f - 0.7f * blink)));
    int riseR = imax(1, iround(rise * (1.0f - 0.85f * clampf(sWink.pos, 0.0f, 1.0f))));
    int thick = imax(3, iround(SH * 0.11f));
    drawHappyEye(iround(cxL), iround(cy), iround(w), rise,  thick);
    drawHappyEye(iround(cxR), iround(cy), iround(w), riseR, thick);
    drawSparkles(t, now < giggleUntil);
  } else {
    int hh = iround(hRaw * (1.0f - 0.92f * blink));        // blink = vertical squash
    int ww = iround(w);
    float lid = clampf(sLidTop.pos, 0.0f, 0.95f);
    drawEye(iround(cxL), iround(cy), ww, hh, true,
            sLookX.pos, sLookY.pos, sPupil.pos, lid, sSlant.pos, sRound.pos);
    drawEye(iround(cxR), iround(cy), ww, hh, false,
            sLookX.pos, sLookY.pos, sPupil.pos, lid, sSlant.pos, sRound.pos);

    if (mood == MOOD_ANGRY && flaring) drawAngerMark(now);
    if (mood == MOOD_SLEEPY && zOn) drawZs(t);
    if (mood == MOOD_SURPRISED && now >= bangShowAt && now < bangUntil) drawBang(now);
  }
  display.display();
}

// ---------------------------------------------------------------- animation clock
void stepAnimation(unsigned long now, float dt) {
  // blink bookkeeping: open again as soon as the lids are shut
  if (!transitioning && sBlink.target > 0.5f && sBlink.pos > 0.93f) sBlink.target = 0;

  // mood swap happens while the eyes are closed
  if (transitioning && sBlink.pos > 0.93f) {
    mood = pendingMood;
    transitioning = false;
    enterMood(now);
    sBlink.target = 0;
  }

  if (transitioning) {
    fxX *= 0.6f; fxY *= 0.6f; fxH *= 0.6f; pjx *= 0.6f; pjy *= 0.6f;
  } else {
    fxX = fxY = fxH = pjx = pjy = 0;
    float t = (now - moodStart) / 1000.0f;
    switch (mood) {
      case MOOD_NORMAL:    updateNormal(now, t);    break;
      case MOOD_HAPPY:     updateHappy(now, t);     break;
      case MOOD_ANGRY:     updateAngry(now, t);     break;
      case MOOD_SLEEPY:    updateSleepy(now, t);    break;
      case MOOD_SURPRISED: updateSurprised(now, t); break;
    }
  }

  // physics, two sub-steps per frame for stability
  const int SUB = 2;
  float h = dt / SUB;
  for (int s = 0; s < SUB; s++) {
    for (int i = 0; i < NUM_SHAPE; i++) shapeSprings[i]->step(h);
    sBlink.step(h);
  }
}

void onButtonPress() {
  if (transitioning) {
    pendingMood = (pendingMood + 1) % MOOD_COUNT;   // fast taps skip ahead
  } else {
    pendingMood = (mood + 1) % MOOD_COUNT;
    transitioning = true;
    sBlink.target = 1.0f;                           // close the eyes
  }
}

// ---------------------------------------------------------------- Arduino entry points
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    while (true) {
      delay(100);
    }
  }

  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  SW = display.width();
  SH = display.height();
  baseW   = SW * 0.28f;
  baseH   = SH * 0.58f;
  baseGap = SW * 0.10f;

  initSprings();

  unsigned long now = millis();
  mood = pendingMood = MOOD_NORMAL;
  enterMood(now);
  sBlink.pos = 1.0f;          // boot with eyes shut...
  sBlink.target = 0;          // ...and wake up
  lastFrame = now;
}

void loop() {
  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastRawButtonState) {
    lastDebounceTime = millis();
    lastRawButtonState = reading;
  }

  if (millis() - lastDebounceTime >= DEBOUNCE_MS) {
    if (reading != stableButtonState) {
      stableButtonState = reading;
      if (stableButtonState == LOW) {
        onButtonPress();
      }
    }
  }

  unsigned long now = millis();
  if (now - lastFrame >= FRAME_MS) {
    float dt = (now - lastFrame) / 1000.0f;
    if (dt > 0.05f) dt = 0.05f;
    lastFrame = now;
    stepAnimation(now, dt);
    drawFrame(now);
  }
}
