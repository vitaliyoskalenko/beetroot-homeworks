#include <Arduino.h>

enum class LedState : uint8_t { Off, On };

enum class LedMode : uint8_t { Blink, AlwaysOn, AlwaysOff };

struct Config {
  static constexpr uint8_t kLedPin = 2;
  static constexpr uint8_t kButtonPin = 0;
  static constexpr uint32_t kBlinkIntervalMs = 500;
  static constexpr uint32_t kDebounceMs = 50;
  static constexpr uint32_t kSerialBaud = 115200;
  static constexpr uint32_t kReportIntervalMs = 1000;
  static constexpr LedMode kInitialMode = LedMode::Blink;
};

static volatile bool buttonPressed = false;

static void IRAM_ATTR onButtonInterrupt() {
  buttonPressed = true;
}

class Led {
 public:
  explicit constexpr Led(uint8_t pin) : pin_(pin), state_(LedState::Off) {}

  void init() {
    pinMode(pin_, OUTPUT);
    set(LedState::Off);
  }

  void set(LedState state) {
    state_ = state;
    digitalWrite(pin_, state_ == LedState::On ? HIGH : LOW);
  }

  void toggle() {
    set(state_ == LedState::On ? LedState::Off : LedState::On);
  }

 private:
  const uint8_t pin_;
  LedState state_;
};

class Button {
 public:
  explicit constexpr Button(uint8_t pin) : pin_(pin), pending_(false), edgeTimeMs_(0) {}

  void init() {
    pinMode(pin_, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pin_), onButtonInterrupt, FALLING);
  }

  bool wasPressed(uint32_t nowMs) {
    if (buttonPressed) {
      buttonPressed = false;
      pending_ = true;
      edgeTimeMs_ = nowMs;
    }

    if (!pending_ || nowMs - edgeTimeMs_ < Config::kDebounceMs) {
      return false;
    }

    pending_ = false;
    return digitalRead(pin_) == LOW;
  }

 private:
  const uint8_t pin_;
  bool pending_;
  uint32_t edgeTimeMs_;
};

class LoopProfiler {
 public:
  void begin() {
    startUs_ = micros();
  }

  void end() {
    const uint32_t elapsedUs = micros() - startUs_;
    totalUs_ += elapsedUs;
    ++iterations_;
    if (elapsedUs < minUs_) {
      minUs_ = elapsedUs;
    }
    if (elapsedUs > maxUs_) {
      maxUs_ = elapsedUs;
    }

    const uint32_t nowMs = millis();
    if (nowMs - lastReportMs_ >= Config::kReportIntervalMs) {
      lastReportMs_ = nowMs;
      report();
      reset();
    }
  }

 private:
  void report() const {
    Serial.printf("loop: avg=%.2f us, min=%u us, max=%u us over %u iterations\n",
                  static_cast<double>(totalUs_) / iterations_,
                  static_cast<unsigned>(minUs_),
                  static_cast<unsigned>(maxUs_),
                  static_cast<unsigned>(iterations_));
  }

  void reset() {
    iterations_ = 0;
    totalUs_ = 0;
    minUs_ = UINT32_MAX;
    maxUs_ = 0;
  }

  uint32_t startUs_ = 0;
  uint32_t iterations_ = 0;
  uint64_t totalUs_ = 0;
  uint32_t minUs_ = UINT32_MAX;
  uint32_t maxUs_ = 0;
  uint32_t lastReportMs_ = 0;
};

class App {
 public:
  App() : led_(Config::kLedPin), button_(Config::kButtonPin) {}

  void init() {
    Serial.begin(Config::kSerialBaud);
    led_.init();
    button_.init();
    applyMode(Config::kInitialMode, millis());
  }

  void run() {
    profiler_.begin();

    const uint32_t nowMs = millis();

    if (button_.wasPressed(nowMs)) {
      applyMode(nextMode(mode_), nowMs);
    }

    if (mode_ == LedMode::Blink && nowMs - lastToggleMs_ >= Config::kBlinkIntervalMs) {
      lastToggleMs_ = nowMs;
      led_.toggle();
    }

    profiler_.end();
  }

 private:
  static constexpr LedMode nextMode(LedMode mode) {
    return mode == LedMode::Blink    ? LedMode::AlwaysOn
           : mode == LedMode::AlwaysOn ? LedMode::AlwaysOff
                                       : LedMode::Blink;
  }

  static const char* modeName(LedMode mode) {
    switch (mode) {
      case LedMode::Blink:
        return "Blink";
      case LedMode::AlwaysOn:
        return "AlwaysOn";
      case LedMode::AlwaysOff:
        return "AlwaysOff";
    }
    return "Unknown";
  }

  void applyMode(LedMode mode, uint32_t nowMs) {
    mode_ = mode;
    lastToggleMs_ = nowMs;

    switch (mode_) {
      case LedMode::Blink:
      case LedMode::AlwaysOn:
        led_.set(LedState::On);
        break;
      case LedMode::AlwaysOff:
        led_.set(LedState::Off);
        break;
    }

    Serial.printf("mode: %s\n", modeName(mode_));
  }

  Led led_;
  Button button_;
  LoopProfiler profiler_;
  LedMode mode_ = Config::kInitialMode;
  uint32_t lastToggleMs_ = 0;
};

static App& app() {
  static App instance;
  return instance;
}

void setup() {
  app().init();
}

void loop() {
  app().run();
}
