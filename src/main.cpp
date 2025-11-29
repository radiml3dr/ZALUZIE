// ESP8266 + CC1101 OOK transmitter with RadioLib
// Uses direct mode and precise GPIO toggling with interrupts off
// to avoid WiFi/scheduler jitter.

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

// ---------- WiFi Stuff ----------
const char* config_ssid = "HONZUV_OVLADAC";
const char* config_password = "lednicka7";

WiFiClient espClient;
std::unique_ptr<ESP8266WebServer> server;

// ---------- Pin mapping (NodeMCU style) ----------
static const uint8_t PIN_GDO0 = 4;   // D2 -> GDO0 (OOK data)
static const uint8_t PIN_SCK  = 14;  // D5 -> SCK
static const uint8_t PIN_MISO = 12;  // D6 -> MISO
static const uint8_t PIN_MOSI = 13;  // D7 -> MOSI
static const uint8_t PIN_CSN  = 15;  // D8 -> CSN

CC1101 radio = new Module(PIN_CSN, PIN_GDO0, RADIOLIB_NC, RADIOLIB_NC);

// ---------- Timing constants (microseconds) ----------
const uint32_t START_HIGH_US = 4000;
const uint32_t START_LOW_US  = 4000;
const uint32_t BIT0_HIGH_US  = 642;
const uint32_t BIT1_HIGH_US  = 1300;
const uint32_t GAP_LOW_US    = 750;

// ---------- Frame parts ----------
const char* CONST_17 = "00001001110011111";
const char* ACTION_DOWN = "10";
const char* ACTION_STOP = "10";
const char* ACTION_UP   = "00";
const char* ID4         = "0101";  // your ID

const uint32_t BASE_UP = 346;
const uint32_t BASE_DOWN = 217;
const uint32_t BASE_STOP = 474;

// ---------- Low-level GPIO toggle helpers ----------
inline void gpioHigh(uint8_t pin) {
  // Direct register write for speed
  gpio_output_set((1 << pin), 0, (1 << pin), 0);
}
inline void gpioLow(uint8_t pin) {
  gpio_output_set(0, (1 << pin), (1 << pin), 0);
}

// ---------- Waveform building ----------
void txHigh(uint32_t us) {
  gpioHigh(PIN_GDO0);
  delayMicroseconds(us);
}

void txLow(uint32_t us) {
  gpioLow(PIN_GDO0);
  delayMicroseconds(us);
}

void sendPreambleAndTrailer() {
  txLow(START_LOW_US);
  txHigh(START_HIGH_US);
  txLow(START_LOW_US);
  txHigh(START_HIGH_US);
  txLow(START_LOW_US);
}

void sendBitString(const String& bits) {
  for (size_t i = 0; i < bits.length(); ++i) {
    bool one = (bits[i] == '1');
    txHigh(one ? BIT1_HIGH_US : BIT0_HIGH_US);
    txLow(GAP_LOW_US);
  }
}

String byteArrayFromInt(int input) {
  if(input < 0 || input > 15) {
    Serial.printf("Input: %d is outside <0,15> range\n", input);
    return "0000";
  }

  String byteArr = "0000";

  byteArr[3] = (input & 1) + '0';
  byteArr[2] = ((input >> 1) & 1) + '0';
  byteArr[1] = ((input >> 2) & 1) + '0';
  byteArr[0] = ((input >> 3) & 1) + '0';

  return byteArr;
}

String byteArrayFromInt9bit(int input) {
  if (input < 0 || input > 511) {
    Serial.printf("Input: %d is outside <0,511> range\n", input);
    return "000000000";
  }

  String byteArr = "000000000";  // 9 bits

  for (int i = 0; i < 9; i++) {
    byteArr[8 - i] = ((input >> i) & 1) + '0';
  }

  return byteArr;
}

bool sendMessage(const int base, const int idx, const String& const17, const String& action2, int repeats = 3) {
  String idxCounter = byteArrayFromInt9bit(base + idx);

  String byteArray = byteArrayFromInt(idx);

  String payload = "000" + byteArray + const17 + "000000" + idxCounter + action2;
  if (payload.length() != 41) {
    Serial.printf("Bad length: %d bits\n", payload.length());
    return false;
  }

  int16_t st = radio.transmitDirectAsync();
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("transmitDirectAsync failed: %d\n", st);
    return false;
  }

  noInterrupts(); // hard timing block
  for (int r = 0; r < repeats; ++r) {
    sendPreambleAndTrailer();
    sendBitString(payload);
  }
  sendPreambleAndTrailer();
  interrupts();

  radio.packetMode();
  delay(1);
  return true;
}

void handleRoot() {
  String lower_url = "http://" + WiFi.localIP().toString() + "/lower";
  String raise_url = "http://" + WiFi.localIP().toString() + "/raise";
  String pause_url = "http://" + WiFi.localIP().toString() + "/pause";
  String message = "<html><body>\n\n";
  message += "To lower all <a href='" + lower_url + "' location='blank'>LOWER</a>\n";
  message += "To raise all <a href='" + raise_url + "' location='blank'>RAISE</a>\n";
  message += "To pause all <a href='" + pause_url + "' location='blank'>PAUSE</a>\n";
  message += "</body></html>";
  server->send(200, "text/html", message);
}

void handleDynamic() {
  String uri = server->uri();
  Serial.println(uri);

  if(uri.startsWith("/raise/")) {
    String numStr = uri.substring(7);
    int value = numStr.toInt();

    Serial.printf("Value = %d\n", value);

    if(value == 1) {
      sendMessage(BASE_DOWN, value, CONST_17, ACTION_DOWN);
    } else {
      sendMessage(BASE_UP, value, CONST_17, ACTION_UP);
    }

    server->send(200, "text/plain", "OK");
    return;
  }

  if(uri.startsWith("/lower/")) {
    String numStr = uri.substring(7);
    int value = numStr.toInt();

    Serial.printf("Value = %d\n", value);

    if(value == 1) {
      sendMessage(BASE_UP, value, CONST_17, ACTION_UP);
    } else {
      sendMessage(BASE_DOWN, value, CONST_17, ACTION_DOWN);
    }

    server->send(200, "text/plain", "OK");
    return;
  }

  if(uri.startsWith("/pause/")) {
    String numStr = uri.substring(7);
    int value = numStr.toInt();

    Serial.printf("Value = %d\n", value);

    sendMessage(BASE_STOP, value, CONST_17, ACTION_STOP);

    server->send(200, "text/plain", "OK");
    return;
  }

  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server->uri();
  message += "\nMethod: ";
  message += (server->method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server->args();
  message += "\n";
  for (uint8_t i = 0; i < server->args(); i++) {
    message += " " + server->argName(i) + ": " + server->arg(i) + "\n";
  }
  server->send(404, "text/plain", message);
}

void handleLower() {
  Serial.println(F("Sending DOWN to ALL..."));

  for(int i = 0; i <= 15; i++) {
    if(i == 1) {
      sendMessage(BASE_UP, i, CONST_17, ACTION_UP);
    } else {
      sendMessage(BASE_DOWN, i, CONST_17, ACTION_DOWN);
    }

    delay(50);
  }

  server->send(200, "text/plain", "OK");
}

void handleRaise() {
  Serial.println(F("Sending UP to ALL..."));

  for(int i = 0; i <= 15; i++) {
    if(i == 1) {
      sendMessage(BASE_DOWN, i, CONST_17, ACTION_DOWN);
    } else {
      sendMessage(BASE_UP, i, CONST_17, ACTION_UP);
    }
    delay(50);
  }

  server->send(200, "text/plain", "OK");
}

void handlePause() {
  Serial.println(F("Sending STOP to ALL..."));

  for(int i = 0; i <= 15; i++) {
    sendMessage(BASE_STOP, i, CONST_17, ACTION_STOP);
    delay(50);
  }

  server->send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(50);

  Serial.println(F("Starting WiFi"));

  WiFiManager wifiManager;

  if (!wifiManager.autoConnect(config_ssid,config_password)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }  

  Serial.println(F("WiFI Connected"));
  server.reset(new ESP8266WebServer(WiFi.localIP(), 80));

  server->on("/", handleRoot);
  server->on("/lower", handleLower);
  server->on("/raise", handleRaise);
  server->on("/pause", handlePause);
  
  server->onNotFound(handleDynamic);

  server->begin();
  Serial.println("HTTP server started");

  WiFi.softAPdisconnect(true);

  Serial.println("Local IP:");
  Serial.println(WiFi.localIP());  

  Serial.println(F("ESP8266 CC1101 OOK direct TX"));

  SPI.begin();
  pinMode(PIN_GDO0, OUTPUT);
  gpioLow(PIN_GDO0);

  int16_t state = radio.begin(433.92);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin failed: %d\n", state);
    while (true) delay(1000);
  }

  radio.setOOK(true);
  radio.setOutputPower(10);
  Serial.println(F("Radio ready."));
}

void loop() {
  server->handleClient();
}