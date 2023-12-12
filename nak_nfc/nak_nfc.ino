#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <ArduinoJson.h>

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

// == Screen definition START
#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels

#define OLED_RESET -1        // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C  ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// == Screen definition END

// == Internet definition START
#include <WiFi.h>
#include <HTTPClient.h>

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
//    Display & Internet Logic
// =============================================

const String &HTTP_EMPTY_RESPONSE = "";

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

int getHTTP(char *url, DynamicJsonDocument *doc) {
  // Initialize http client
  HTTPClient http;
  // Debug message
  DPRINTF("[HTTP] GET... calling `%s`\n", url);
  // Accept ssl certificates without validation
  http.setInsecure();
  // configure target server and url
  if (http.begin(url)) {
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.GET();

    // httpCode will be negative on error
    if (httpCode > 0) {
      DPRINTF("[HTTP] GET... status code: %d\n", httpCode);

      // file found at server
      if (httpCode == HTTP_CODE_OK) {
        DeserializationError error = deserializeJson(*doc, http.getStream());
        if (error) {
          http.end();
          return -1;
        }
      }

      http.end();
      return httpCode;
    } else {
      DPRINTF("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
  } else {
    DPRINTLN("[HTTP] GET... failed to initialize");
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

  // Initialize queues
  DPRINTLN("[SYS] Initializing queues...");
  queue_init(&scan_queue, sizeof(scan_item_t), QUEUE_LENGTH);
  queue_init(&scan_results_queue, sizeof(scan_result_item_t), QUEUE_LENGTH);

  // Initialize mutex
  DPRINTLN("[SYS] Initializing muextes...");
  mutex_init(&wifi_status_m);

  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    DPRINTLN("[APP] Display not found...");
    for (;;)
      ;  // Don't proceed, loop forever
  }

  // Apply display settings
  DPRINTLN("[APP] Initializing display...");
  display.setRotation(2);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.cp437(true);

  // Display splash screen
  display.display();
  delay(1000);
  display.clearDisplay();
  display.setCursor(0, 0);

  // Start display boot sequence
  DPRINTLN("[APP] Starting boot sequence...");
  display.print("Booting.");
  display.display();

  // Read board unique id
  flash_get_unique_id((uint8_t *)sensor_id);
  sprintf(sensor_id_str, "0x%X", sensor_id);

  DPRINTF("[SYS] Board ID: %s\n", sensor_id_str);
  display.print(".");
  display.display();

  // Connect to wifi
  DPRINTF("[WIFI] Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  // Check for successful connection
  while (WiFi.status() != WL_CONNECTED) {
    DPRINTF("[WIFI] WiFi failed. Current status: %d. Reconnecting...\n", WiFi.status());
    // Show WiFi failure on display
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("WiFi failed!\nReconnecting.");
    display.display();
    // Some number of wait time
    delay(2000);
    display.print(".");
    display.display();
    // Reconnect WiFi
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    display.print(".");
    display.display();
  }
  DPRINTF("[WIFI] WiFi connected! IP: %s; Status: %d\n", WiFi.localIP().toString(), WiFi.status());

  // Successful boot
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Booted!");
  display.display();
  delay(500);

  // Display WiFi Connection details
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("SSID: ");
  display.println(ssid);
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.print("Sensor ID: ");
  display.println(sensor_id_str);
  display.display();

  // Allow users to see everything worked
  delay(1000);
}

#define NUM_SCAN_STATES 4
char scan_states[NUM_SCAN_STATES] = { '/', '-', '\\', '|' };
uint8_t scan_state = 0;
void loop() {
  DPRINTF("[HEAP] Free: %d; Used: %d; Total: %d\n", rp2040.getFreeHeap(), rp2040.getUsedHeap(), rp2040.getTotalHeap());

  if (mutex_try_enter(&wifi_status_m, NULL)) {
    DPRINTLN("[WIFI] Updating WiFi status in mutex...");
    current_wifi_status = WiFi.status();
    mutex_exit(&wifi_status_m);
  }

  // Clear display
  display.clearDisplay();
  display.setCursor(0, 0);

  // Check if WiFi connection failed
  if (WiFi.status() != WL_CONNECTED) {
    DPRINTF("[WIFI] Connection lost! Current status: %d. Reconnecting...\n", WiFi.status());
    display.println("Connecting WiFi...");
    display.display();
    // Try reconnecting to WiFi
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    delay(5000);
    return;
  } else {
    display.println("WiFi connected!");
  }

  // Waiting for next scan to submit
  display.print("\nWaiting for scan ");
  char scanStateChar = scan_states[scan_state];
  scan_state = (scan_state + 1) % NUM_SCAN_STATES;
  display.print(scanStateChar);
  display.display();

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

    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Card ID: ");
    display.println(uid_str);
    display.print("Searching card.");
    display.display();

    // Allocate json document for parsing
    DynamicJsonDocument doc(1024);
    // Display progress dot
    display.print(".");
    display.display();
    // Generating url
    char url[255];
    snprintf(url, 255, "%s?filter=(card_id='%s')", PB_MAKE_URL("/api/collections/cards/records"), uid_str);
    // Display progress dot
    display.print(".");
    display.display();
    // Making http request and reading json response
    int lookupHttpCode = getHTTP(url, &doc);

    // Test if parsing succeeds.
    if (lookupHttpCode == -1 || lookupHttpCode != HTTP_CODE_OK) {
      display.print("\nFailed to lookup card!");
      display.display();

      scan_result_item_t scan_result;
      scan_result.success = false;
      queue_add_blocking(&scan_results_queue, &scan_result);

      delay(2000);
      return;
    }
    // Reading returned number of results
    long itemCount = doc["totalItems"];
    // If no results, card is not registered
    if (itemCount == 0) {
      // Display error message when no items found
      display.print("\nUnknown card!\nPlease register!");
      display.display();
      // Submit scan result for visual response
      scan_result_item_t scan_result;
      scan_result.success = false;
      queue_add_blocking(&scan_results_queue, &scan_result);
      // Delay to make a read the display
      delay(2000);
      return;
    }

    // Get the first item from the returned search
    JsonArray items = doc["items"].as<JsonArray>();
    JsonObject item = items[0];
    // Extract card details
    String cardOwner = item["name"], realId = item["id"];
    // Display card owner name
    display.print("\nHello ");
    display.print(cardOwner);
    // Start submitting the scan
    display.print("\nSubmitting.");
    display.display();
    // Prepare the json body
    char scan_json[512];
    snprintf(scan_json, 512, "{\"sensor_id\":\"%s\",\"card_id\":\"%s\"}", sensor_id_str, realId.c_str());
    // Display progress dot
    display.print(".");
    display.display();
    // Make the http POST request
    int scanSubmitHttpCode = postHTTP(PB_MAKE_URL("/api/collections/scans/records"), scan_json);
    // Build a scan result based on the response code
    scan_result_item_t scan_result;
    scan_result.success = (scanSubmitHttpCode == HTTP_CODE_OK);
    queue_add_blocking(&scan_results_queue, &scan_result);
    // Display progress dot
    display.print(".");
    display.display();
    // Display scan result on the display
    if (!scan_result.success) {
      display.println("\nFailed!");
      display.display();
    } else {
      display.println("\nScan submitted!");
      display.display();
    }
    // Delay to read the message on the display
    delay(2000);
  }
  // Base delay to avoid overloading the loop
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
#define NUM_PIXELS 16

Adafruit_NeoPixel pixels(NUM_PIXELS, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
// == Pixels definition END

// == PN532 definition START
#define PN532_RESET 7
#define PN532_IRQ 6
#define PN532_SS 17

Adafruit_PN532 nfc(PN532_SS);
// == PN532 definition END

// == Buzzer definition START
#include "notes.h"

#define BUZZER_PIN 3 // Use e.g. 9 to deactivate

void playNokiaMelody() {
  const int melody[] = { NOTE_E5, NOTE_D5, NOTE_FS4, NOTE_GS4, NOTE_CS5, NOTE_B4, NOTE_D4, NOTE_E4, NOTE_B4, NOTE_A4, NOTE_CS4, NOTE_E4, NOTE_A4 };
  const int noteDurations[] = { 8, 8, 4, 4, 8, 8, 4, 4, 8, 8, 4, 4, 2 };
  // iterate over the notes of the melody:
  for (int thisNote = 0; thisNote < 13; thisNote++) {
    // to calculate the note duration, take one second divided by the note type.
    //e.g. quarter note = 1000 / 4, eighth note = 1000/8, etc.
    int noteDuration = 1000 / noteDurations[thisNote];
    tone(BUZZER_PIN, melody[thisNote], noteDuration);
    // to distinguish the notes, set a minimum time between them.
    // the note's duration + 30% seems to work well:
    int pauseBetweenNotes = noteDuration * 1.30;
    delay(pauseBetweenNotes);
    // stop the tone playing:
    noTone(BUZZER_PIN);
  }
}

void playLevelUp() {
  tone(BUZZER_PIN, NOTE_E4);
  delay(150);
  tone(BUZZER_PIN, NOTE_G4);
  delay(150);
  tone(BUZZER_PIN, NOTE_E5);
  delay(150);
  tone(BUZZER_PIN, NOTE_C5);
  delay(150);
  tone(BUZZER_PIN, NOTE_D5);
  delay(150);
  tone(BUZZER_PIN, NOTE_G5);
  delay(150);
  noTone(BUZZER_PIN);
}

void playScanRegistered() {
  tone(BUZZER_PIN, NOTE_G5);
  delay(300);
  noTone(BUZZER_PIN);
}
// == Buzzer definition END

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
  DPRINTF("[APP] FOund chip PN5%02X, Firmware: %02d.%02d\n", (versiondata >> 24) & 0xFF, (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);

  // Set the max number of retry attempts to read from a card
  // This prevents us from waiting forever for a card, which is
  // the default behaviour of the PN532.
  nfc.setPassiveActivationRetries(0xFF);

  DPRINTLN("Waiting for an ISO14443A card");

  //playNokiaMelody();

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
  // Sound for the user that the scan has been registered
  playScanRegistered();

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
        delay(1000);
      } else {
        pixels.fill(pixels.Color(0, 50, 0));
        pixels.show();
        playLevelUp();
      }
    }

    delay(1000);
  } else {
    DPRINTLN("[APP] Card timeout! New reading...");
  }
}