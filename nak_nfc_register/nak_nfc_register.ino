// == Internet definition START
#include <WiFi.h>
#include <HTTPClient.h>

#define PB_HOST "https://h2949706.stratoserver.net"
#define PB_MAKE_URL(path) (PB_HOST "" path)

#ifndef STASSID
#define STASSID "iPhone von Daniel"
#define STAPSK "danielistcool"
#endif

const char* ssid = STASSID;
const char* password = STAPSK;
// == Internet definition END

// == Queue definition START
#include <cppQueue.h>

#define IMPLEMENTATION FIFO

typedef struct scan_item_t {
  uint8_t uid[7];
  uint8_t uid_length;  // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
} scan_item_t;

cppQueue scan_queue(sizeof(scan_item_t), 1, IMPLEMENTATION);

typedef struct scan_result_item_t {
  bool success;
} scan_result_item_t;

cppQueue scan_results_queue(sizeof(scan_result_item_t), 1, IMPLEMENTATION);
// == Queue definition END

// =============================================
//    Display & Internet Logic
// =============================================

const String& HTTP_EMPTY_RESPONSE = "";
const String& postHTTP(String url, String body) {
  // Initialize http client
  HTTPClient http;
  // Accept ssl certificates without validation
  http.setInsecure();
  // configure target server and url
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  // start connection and send HTTP header and body
  int httpCode = http.POST(body);

  // httpCode will be negative on error
  if (httpCode > 0) {
    // HTTP header has been send and Server response header has been handled
    //Serial.printf("[HTTP] POST (%s with `%s`) code: %d\n", url, body, httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK) {
      const String& payload = http.getString();
      return payload;
    }
  } else {
    Serial.printf("[HTTP] POST... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return HTTP_EMPTY_RESPONSE;
}

// Unique board id from the rp2040
uint8_t sensor_id[8];
char sensor_id_str[2 + (8 * 2) + 1];  // 0x(sensor_id as hex)\0

extern "C" {
#include "hardware/flash.h"
#include "pico/bootrom.h"
}

void setup() {
  // Open Serial Connection
  Serial.begin(115200);

  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB
  }

  // Read board unique id
  flash_get_unique_id((uint8_t*)sensor_id);
  sprintf(sensor_id_str, "0x%X", sensor_id);

  Serial.print("Board ID: ");
  Serial.println(sensor_id_str);

  // Connect to wifi
  WiFi.begin(ssid, password);
  // Check for successful connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(5000);
    // Reconnect WiFi
    WiFi.begin(ssid, password);
  }
}

void loop() {
  // Check if WiFi connection failed
  if (WiFi.status() != WL_CONNECTED) {
    // Try reconnecting to WiFi
    WiFi.begin(ssid, password);
    return;
  }

  // Check for next scan in queue
  if (!scan_queue.isEmpty()) {
    scan_item_t scan;
    scan_queue.pop(&scan);

    String uid_str = "0x";

    for (uint8_t i = 0; i < scan.uid_length; i++) {
      uid_str += String(scan.uid[i], HEX);
    }
    Serial.print("Scanned card: ");
    Serial.println(uid_str);

    String scan_json = String("{\"sensor_id\":\"");
    scan_json += sensor_id_str;
    scan_json += "\",\"card_id\":\"";
    scan_json += uid_str;
    scan_json += "\"}";

    Serial.print("Submitting scan: ");
    Serial.println(scan_json);

    const String& payload = postHTTP(PB_MAKE_URL("/api/collections/unregistered_cards/records"), scan_json);

    scan_result_item_t scan_result;
    scan_result.success = (payload != HTTP_EMPTY_RESPONSE);
    scan_results_queue.push(&scan_result);

    delay(1000);
  }
}

// =============================================
//    NFC Logic
// =============================================

#include <Adafruit_NeoPixel.h>
#include <Adafruit_PN532.h>

// == Pixels definition START
#define PIXEL_PIN 2
#define NUM_PIXELS 1

Adafruit_NeoPixel pixels(NUM_PIXELS, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
// == Pixels definition END

// == PN532 definition START
//#define PN532_RESET 7
//#define PN532_IRQ 6
#define PN532_SS 17

Adafruit_PN532 nfc(PN532_SS);
// == PN532 definition END

void ensureWiFiConnectionWithVisual() {
  for(uint8_t toggle = 0; WiFi.status() != WL_CONNECTED; toggle ^= 0x1) {
    if(toggle) {
      pixels.fill(pixels.Color(10,0,0));
      pixels.show();
    } else {
      pixels.clear();
      pixels.show();
    }
    delay(500);
  }
}

void setup1() {
  // NOTE: Serial will be initialized by core0
  Serial.begin(115200);

  // Initialize pixels
  pixels.begin();
  pixels.show();
  // Initialize nfc
  nfc.begin();

  delay(1000);

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.print("Didn't find PN53x board");
    while (1)
      ;  // halt
  }

  // Got ok data, print it out!
  Serial.print("Found chip PN5");
  Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print("Firmware ver. ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  // Set the max number of retry attempts to read from a card
  // This prevents us from waiting forever for a card, which is
  // the default behaviour of the PN532.
  nfc.setPassiveActivationRetries(0xFF);

  Serial.println("Waiting for an ISO14443A card");

  // Waiting for WiFi connection
  ensureWiFiConnectionWithVisual();
}

uint32_t crc32b(const char* str) {
  // Source: https://stackoverflow.com/a/21001712
  unsigned int byte, crc, mask;
  int i = 0, j;
  crc = 0xFFFFFFFF;
  while (str[i] != 0) {
    byte = str[i];
    crc = crc ^ byte;
    for (j = 7; j >= 0; j--) {
      mask = -(crc & 1);
      crc = (crc >> 1) ^ (0xEDB88320 & mask);
    }
    i = i + 1;
  }
  return ~crc;
}

void visualizeScan(scan_item_t* scan) {
  // Generate hash from uid
  String uid_str = "0x";
  for (uint8_t i = 0; i < scan->uid_length; i++) {
    uid_str += String(scan->uid[i], HEX);
  }
  uint32_t hash = crc32b(uid_str.c_str());

  // Visualize each byte as a color in the ring
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t d = (hash >> i) & 0xFF;
    uint8_t r = (d & 0xE0) >> 5;
    uint8_t g = (d & 0x1C) >> 2;
    uint8_t b = (d & 0x03);

    // Visual scan success
    pixels.fill(pixels.Color(r, g, b));
    pixels.show();
    delay(250);
  }
}

uint8_t last_uid[7] = {0,0,0,0,0,0,0};
unsigned long last_scan = 0;

void loop1() {
  ensureWiFiConnectionWithVisual();

  pixels.fill(pixels.Color(0, 0, 10));
  pixels.show();

  boolean success;
  scan_item_t scan;
  memset(scan.uid, 7, 0);

  // Wait for an ISO14443A type cards (Mifare, etc.).  When one is found
  // 'uid' will be populated with the UID, and uidLength will indicate
  // if the uid is 4 bytes (Mifare Classic) or 7 bytes (Mifare Ultralight)
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &(scan.uid[0]), &(scan.uid_length));

  if (success) {
    // Check if last uid equals newly scanned and last scan is not older than 1 minute
    if(memcmp(last_uid, scan.uid, 7) == 0 && (millis() - last_scan) < 60000) {
      last_scan = millis();
      Serial.println("Card already submitted");
      return;
    }
    // Push to queue
    scan_queue.push(&scan);
    // Remember last uid
    memcpy(last_uid, scan.uid, 7);
    last_scan = millis();
    // Visual scan response
    visualizeScan(&scan);
    // Wait until card is processed
    while (!scan_queue.isEmpty())
      ;
    delay(100);
    // Wait for response
    while (scan_results_queue.isEmpty())
      ;
    // Process responses
    if (!scan_results_queue.isEmpty()) {
      scan_result_item_t scan_result;
      scan_results_queue.pop(&scan_result);

      if (!scan_result.success) {
        pixels.fill(pixels.Color(50, 0, 0));
        pixels.show();
        delay(1000);
      } else {
        pixels.fill(pixels.Color(0, 50, 0));
        pixels.show();
      }
    }

    delay(1000);
  } else {
    // PN532 probably timed out waiting for a card
    Serial.println("Timed out waiting for a card");
  }
}
