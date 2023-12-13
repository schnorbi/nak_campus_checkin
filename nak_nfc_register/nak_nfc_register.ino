// == Internet definition START
#include <WiFi.h>
#include <HTTPClient.h>

// == Debug helpers START
// Comment out to disable debug
#define CUSTOM_DEBUG

#ifdef CUSTOM_DEBUG
#define DPRINT(...) Serial.print(__VA_ARGS__)
#define DPRINTLN(...) Serial.println(__VA_ARGS__)
#define DPRINTF(...) Serial.printf(__VA_ARGS__)
#define DBEGIN(...) Serial.begin(__VA_ARGS__)
#else
#define DPRINT(...)    //blank line
#define DPRINTLN(...)  //blank line
#define DPRINTF(...)   //blank line
#define DBEGIN(...)    //blank line
#endif
// == Debug helpers END

#define PB_HOST "https://h2949706.stratoserver.net"
#define PB_MAKE_URL(path) (PB_HOST "" path)

#ifndef STASSID
#define STASSID "iPhone von Daniel"
#define STAPSK "danielistcool"
#endif

const char *ssid = STASSID;
const char *password = STAPSK;

uint8_t current_wifi_status = WL_IDLE_STATUS;
auto_init_mutex(wifi_status_m);
// == Internet definition END

// == Queue definition START
#define QUEUE_LENGTH 1

typedef struct scan_item_t {
  uint8_t uid[7];
  uint8_t uid_length;  // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
} scan_item_t;

queue_t scan_queue;

typedef struct scan_result_item_t {
  bool success;
} scan_result_item_t;

queue_t scan_results_queue;
// == Queue definition END

// =============================================
//    Internet Logic
// =============================================

int postHTTP(const char *url, char *body) {
  // Initialize http client
  HTTPClient http;
  // Accept ssl certificates without validation
  http.setInsecure();
  // Debug message
  DPRINTF("[HTTP] POST... calling `%s` with body `%s`\n", url, body);
  // configure target server and url
  if (http.begin(url)) {
    http.addHeader("Content-Type", "application/json");

    // start connection and send HTTP header and body
    int httpCode = http.POST(body);

    // httpCode will be negative on error
    if (httpCode > 0) {
      DPRINTF("[HTTP] POST... status code: %d\n", httpCode);
      http.end();
      return httpCode;
    } else {
      DPRINTF("[HTTP] POST... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
  } else {
    DPRINTLN("[HTTP] POST... failed to initialize");
  }

  return -1;
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
  DBEGIN(115200);

  while(!Serial);

  // Initialize queues
  DPRINTLN("[SYS] Initializing queues...");
  queue_init(&scan_queue, sizeof(scan_item_t), QUEUE_LENGTH);
  queue_init(&scan_results_queue, sizeof(scan_result_item_t), QUEUE_LENGTH);

  // Initialize mutex
  DPRINTLN("[SYS] Initializing muextes...");
  mutex_init(&wifi_status_m);

  // Start display boot sequence
  DPRINTLN("[APP] Starting boot sequence...");

  // Read board unique id
  flash_get_unique_id((uint8_t *)sensor_id);
  sprintf(sensor_id_str, "0x%X", sensor_id);

  DPRINTF("[SYS] Board ID: %s\n", sensor_id_str);

  // Connect to wifi
  DPRINTF("[WIFI] Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  // Check for successful connection
  while (WiFi.status() != WL_CONNECTED) {
    DPRINTF("[WIFI] WiFi failed. Current status: %d. Reconnecting...\n", WiFi.status());
    // Some number of wait time
    delay(2000);
    // Reconnect WiFi
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
  DPRINTF("[WIFI] WiFi connected! IP: %s; Status: %d\n", WiFi.localIP().toString().c_str(), WiFi.status());
}

void loop() {
  DPRINTF("[HEAP] Free: %d; Used: %d; Total: %d\n", rp2040.getFreeHeap(), rp2040.getUsedHeap(), rp2040.getTotalHeap());

  if (mutex_try_enter(&wifi_status_m, NULL)) {
    DPRINTLN("[WIFI] Updating WiFi status in mutex...");
    current_wifi_status = WiFi.status();
    mutex_exit(&wifi_status_m);
  }

  // Check if WiFi connection failed
  if (WiFi.status() != WL_CONNECTED) {
    DPRINTF("[WIFI] Connection lost! Current status: %d. Reconnecting...\n", WiFi.status());
    // Try reconnecting to WiFi
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    delay(5000);
    return;
  }

  // Check for next scan in queue
  if (!queue_is_empty(&scan_queue)) {
    DPRINTLN("[APP] New scan in queue! Processing...");

    scan_item_t scan;
    queue_remove_blocking(&scan_queue, &scan);

    char uid_str[32] = "0x";
    char *ptr = &uid_str[strlen(uid_str)];
    for (uint8_t i = 0; i < scan.uid_length; i++) {
      ptr += sprintf(ptr, "%02x", scan.uid[i]);
    }
    DPRINTF("[APP] Scan card id: %s\n", uid_str);

    // Prepare the json body
    char scan_json[512];
    snprintf(scan_json, 512, "{\"sensor_id\":\"%s\",\"card_id\":\"%s\"}", sensor_id_str, uid_str);

    // Make the http POST request
    int scanSubmitHttpCode = postHTTP(PB_MAKE_URL("/api/collections/unregistered_cards/records"), scan_json);

    // Build a scan result based on the response code
    scan_result_item_t scan_result;
    scan_result.success = (scanSubmitHttpCode == HTTP_CODE_OK);
    queue_add_blocking(&scan_results_queue, &scan_result);

    delay(1000);
  }
  delay(500);
}

// =============================================
//    NFC Logic
// =============================================

#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_PN532.h>

// == Pixels definition START
#define PIXEL_PIN 2
#define NUM_PIXELS 1

Adafruit_NeoPixel pixels(NUM_PIXELS, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
// == Pixels definition END

// == PN532 definition START
#define PN532_RESET 7
#define PN532_IRQ 6
#define PN532_SS 17

Adafruit_PN532 nfc(PN532_SS);
// == PN532 definition END

void ensureWiFiConnectionWithVisual() {
  for (uint8_t toggle = 0, status = WL_IDLE_STATUS;; toggle ^= 0x1) {
    // Read current status
    mutex_enter_blocking(&wifi_status_m);
    status = current_wifi_status;
    mutex_exit(&wifi_status_m);
    // If connected, exit this function as connection is established
    if (status == WL_CONNECTED) {
      // Clear indicator before exit
      pixels.clear();
      pixels.show();
      return;
    }
    // Toggle the visual indicator
    if (toggle) {
      pixels.fill(pixels.Color(10, 0, 0));
      pixels.show();
    } else {
      pixels.clear();
      pixels.show();
    }
    delay(500);
  }
}

void setup1() {
  // Initialize serial
  DBEGIN(115200);

  // Initialize pixels
  pixels.begin();
  pixels.clear();
  // Initialize nfc
  nfc.begin();
  // Delay for bus intialization
  delay(1000);

  // Extract nfc chip data
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    DPRINTLN("[APP] Didn't find PN53x board");
    while (1)
      ;  // halt
  }

  // Got ok data, print it out!
  DPRINTF("[APP] Found chip PN5%02X, Firmware: %02d.%02d\n", (versiondata >> 24) & 0xFF, (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);

  // Set the max number of retry attempts to read from a card
  // This prevents us from waiting forever for a card, which is
  // the default behaviour of the PN532.
  nfc.setPassiveActivationRetries(0xFF);

  DPRINTLN("Waiting for an ISO14443A card");

  // Waiting for WiFi connection
  ensureWiFiConnectionWithVisual();
}

uint32_t crc32b(const char *str) {
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

void visualizeScan(scan_item_t *scan) {
  // Generate hash from uid
  char uid_str[32] = "0x";
  char *ptr = &uid_str[strlen(uid_str)];
  for (uint8_t i = 0; i < scan->uid_length; i++) {
    ptr += sprintf(ptr, "%02x", scan->uid[i]);
  }
  uint32_t hash = crc32b(uid_str);

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

uint8_t last_uid[7] = { 0, 0, 0, 0, 0, 0, 0 };
unsigned long last_scan = 0;

void loop1() {
  // Ensure there is a WiFi connection before we accept more scans
  ensureWiFiConnectionWithVisual();
  // Display a light blue color on the Ring for "ready"
  pixels.fill(pixels.Color(0, 0, 10));
  pixels.show();

  // Status variables for our next scan
  boolean success;
  scan_item_t scan;
  memset(scan.uid, 7, 0);

  // Wait for an ISO14443A type cards (Mifare, etc.).  When one is found
  // 'uid' will be populated with the UID, and uidLength will indicate
  // if the uid is 4 bytes (Mifare Classic) or 7 bytes (Mifare Ultralight)
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &(scan.uid[0]), &(scan.uid_length));

  if (success) {
    // Check if last uid equals newly scanned and last scan is not older than 1 minute
    if (memcmp(last_uid, scan.uid, 7) == 0 && (millis() - last_scan) < 60000) {
      last_scan = millis();
      DPRINTLN("[APP] Card already submitted");
      return;
    }
    // Push to queue
    queue_add_blocking(&scan_queue, &scan);
    // Remember last uid
    memcpy(last_uid, scan.uid, 7);
    last_scan = millis();
    // Visual scan response
    visualizeScan(&scan);
    // Wait until card is processed
    while (!queue_is_empty(&scan_queue))
      ;
    // Wait for response
    while (queue_is_empty(&scan_results_queue))
      ;
    // Process responses
    if (!queue_is_empty(&scan_results_queue)) {
      scan_result_item_t scan_result;
      queue_remove_blocking(&scan_results_queue, &scan_result);

      if (!scan_result.success) {
        pixels.fill(pixels.Color(50, 0, 0));
        pixels.show();
        // Allow direct rescan
        memset(last_uid, 0, 7);
        last_scan = 0;
      } else {
        pixels.fill(pixels.Color(0, 50, 0));
        pixels.show();
      }
    }

    delay(2000);
  } else {
    DPRINTLN("[APP] Card timeout! New reading...");
  }
}