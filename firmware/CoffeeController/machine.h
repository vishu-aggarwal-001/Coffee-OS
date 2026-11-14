#pragma once
#include <ArduinoJson.h>

// The order engine: takes an order from the touchscreen (paid at the counter), drops a
// cup, runs the drink's recipe steps, drops a lid and waits for the customer.
namespace machine {
  void begin();
  void update();
  bool placeOrder(const char* drinkId, const char* size, int sugar, const char* code, bool bypassCode, String& error);
  void cancelOrder();
  bool startRinse(String& error);
  bool testVend(const char* drinkId, const char* size, String& error);
  bool dispense(const char* target, int index, float seconds, String& error);  // calibration
  void menu(JsonObject out);
  void status(JsonObject out);
  bool busy();
}
