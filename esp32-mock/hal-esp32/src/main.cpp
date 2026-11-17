// PulseDeck ESP32 test bench: serial protocol + real flash persistence.
#include <Arduino.h>

#include <deque>
#include <string>

#include "buttons.h"
#include "flash_store.h"
#include "pd/engine.h"

// The core keeps big structs off the stack, this is only extra margin for the Arduino task.
#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(12 * 1024);
#endif

namespace {

constexpr size_t kMaxLine = 199;  // PROTOCOL_SPEC: absolute line limit
constexpr uint8_t kLedPin = 2;    // on-board LED: blinks on every footswitch/scene/preset event

struct SerialSink : pd::ILineSink {
  void line(const char* s) override {
    Serial.print(s);
    Serial.print('\n');
  }
};

// Unsolicited EVT lines are queued and only flushed between commands, so they
// never get interleaved into a reply.
struct EventQueue : pd::ILineSink {
  std::deque<std::string> q;
  uint32_t blinkUntil = 0;
  void line(const char* s) override {
    if (q.size() < 16) q.emplace_back(s);
    blinkUntil = millis() + 80;
  }
};

LittleFsStore presetStore;
NvsStore settingsStore;
pd::Engine engine(presetStore, settingsStore);
Buttons buttons;
SerialSink serialSink;
EventQueue events;
std::string rx;
bool rxOverflow = false;

}  // namespace

void setup() {
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  pinMode(kLedPin, OUTPUT);
  bool fsOk = presetStore.begin();
  bool nvsOk = settingsStore.begin();
  buttons.begin();
  engine.setAsync(&events);
  engine.begin();
  events.q.clear();
  Serial.printf("\nBOOT pulsedeck-bench littlefs=%d nvs=%d fs_used=%u/%u\n", fsOk, nvsOk, (unsigned)presetStore.usedBytes(), (unsigned)presetStore.totalBytes());
}

void loop() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (rxOverflow) serialSink.line("ERR line too long");
      else engine.handleLine(rx.c_str(), serialSink);
      rx.clear();
      rxOverflow = false;
    } else if (c != '\r') {
      if (rx.size() < kMaxLine) rx += c;
      else rxOverflow = true;
    }
  }
  uint32_t now = millis();
  buttons.poll(engine, now);
  engine.tick(now);
  while (!events.q.empty() && Serial.available() == 0) {
    serialSink.line(events.q.front().c_str());
    events.q.pop_front();
  }
  digitalWrite(kLedPin, now < events.blinkUntil ? HIGH : LOW);
}
