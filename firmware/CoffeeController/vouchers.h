#pragma once
#include <ArduinoJson.h>

// Pay-at-the-counter codes. The cashier takes payment and creates a 4-digit code
// for one drink and size on the counter screen (/counter). The customer types the
// code on the machine. A code is used up only when the drink is delivered; if the
// drink fails, the code works again.
namespace vouchers {
  void begin();
  bool create(const char* drink, const char* size, String& code, String& error);
  bool cancel(const char* code);
  // Checks a code. On success fills drink/size and returns true.
  bool check(const char* code, String& drink, String& size, String& error);
  void markUsed(const char* code);
  void list(JsonArray out);
  bool lockedOut();
}
