// Storage abstraction. The core never touches a flash API; each HAL supplies
// an IBlobStore. Contract: write() must be atomic (old value or new value
// survives a power cut, never a torn blob). The core adds its own CRC32 on
// top, so a corrupted blob is detected even if the HAL cannot guarantee it.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pd {

struct IBlobStore {
  virtual ~IBlobStore() {}
  // Reads the blob into buf (capacity cap). On success sets len and returns true.
  virtual bool read(const char* key, uint8_t* buf, size_t cap, size_t& len) = 0;
  virtual bool write(const char* key, const uint8_t* data, size_t len) = 0;
  virtual bool erase(const char* key) = 0;
  virtual bool exists(const char* key) = 0;
  virtual const char* backend() const = 0;
};

// Uploaded media (amp captures and impulse responses). The core speaks the upload protocol and keeps the slot names;
// the platform that can actually play/keep them supplies an IMedia. Without one the payload is checked (CRC) and dropped.
// kind: 'C' = capture blob (bytes), 'I' = impulse response (int16 LE mono samples; bytes = 2 x samples).
struct IMedia {
  virtual ~IMedia() {}
  virtual bool begin(char kind, size_t bytes, long gain_mdb, int fs_khz) = 0;      // false: no room for an upload of this size
  virtual void data(size_t off, const uint8_t* d, size_t n) = 0;
  // Validates the staged upload and makes it the LIVE (sounding) item. rc 0 ok, -4 rejected, -100 too big/heavy.
  virtual int end(char kind, char* name, size_t name_cap, int* layers, int* channels) = 0;
  virtual bool save(char kind, int slot) = 0;                                      // persist the live item into the slot
  virtual bool select(char kind, int slot) = 0;                                    // make a stored slot the live one (-1: none)
  virtual bool erase(char kind, int slot) = 0;
  virtual void abort() = 0;                                                        // drop a staged upload
};

// Line output (serial, BLE, test buffer).
struct ILineSink {
  virtual ~ILineSink() {}
  virtual void line(const char* s) = 0;
};
void say(ILineSink& o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace pd
