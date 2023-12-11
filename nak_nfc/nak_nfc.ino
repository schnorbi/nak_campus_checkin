#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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

  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("Display not found...");
    for (;;)
      ;  // Don't proceed, loop forever
  }

  // Apply display settings
  display.setRotation(0);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Display splash screen
  display.display();
  delay(1000);
  display.clearDisplay();

  // Start display boot sequence
  display.print("Booting.");
  display.display();

  // Read board unique id
  flash_get_unique_id((uint8_t*)sensor_id);
  sprintf(sensor_id_str, "0x%X", sensor_id);

  Serial.print("Board ID: ");
  Serial.println(sensor_id_str);
  display.print(".");
  display.display();

  // Connect to wifi
  WiFi.begin(ssid, password);
  // Check for successful connection
  while (WiFi.status() != WL_CONNECTED) {
    // Show WiFi failure on display
    display.clearDisplay();
    display.print(".\nWiFi failed!\nReconnecting.");
    display.display();
    // Some number of wait time
    delay(2000);
    display.print(".");
    display.display();
    // Reconnect WiFi
    WiFi.begin(ssid, password);
    display.print(".");
    display.display();
  }

  // Successful boot
  display.clearDisplay();
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
  delay(2000);
}

void loop() {
  // Clear display
  display.clearDisplay();
  display.setCursor(0, 0);

  // Check if WiFi connection failed
  if (WiFi.status() != WL_CONNECTED) {
    display.print("Connecting WiFi...");
    display.display();
    // Try reconnecting to WiFi
    WiFi.begin(ssid, password);
    return;
  } else {
    display.print("WiFi connected!");
    display.display();
  }

  // Waiting for next scan to submit
  display.println("\nWaiting for scan...");
  display.display();

  delay(200);

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

    display.clearDisplay();
    display.print("Card ID: ");
    display.println(uid_str);
    display.print("\nSubmitting");
    display.display();

    String scan_json = String("{\"sensor_id\":\"");
    scan_json += sensor_id_str;
    scan_json += "\",\"card_id\":\"";
    scan_json += uid_str;
    scan_json += "\"}";

    display.print(".");
    display.display();

    Serial.print("Submitting scan: ");
    Serial.println(scan_json);

    const String& payload = postHTTP(PB_MAKE_URL("/api/collections/scans/records"), scan_json);

    scan_result_item_t scan_result;
    scan_result.success = (payload != HTTP_EMPTY_RESPONSE);
    scan_results_queue.push(&scan_result);

    display.print(".");
    display.display();

    if (!scan_result.success) {
      Serial.println(payload);
      display.println("\nFailed to submit scan!");
      display.display();
    } else {
      display.println("\nScan submitted!");
      display.display();
    }

    delay(1000);
  }
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

//Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET, &Wire1);
Adafruit_PN532 nfc(PN532_SS);
// == PN532 definition END

// == Buzzer definition START
#include "notes.h"

#define BUZZER_PIN 3

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
  pixels.clear();
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

  //playNokiaMelody();

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
  // Sound for the user that the scan has been registered
  playScanRegistered();

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
      } else {
        pixels.fill(pixels.Color(0, 50, 0));
        pixels.show();
        playLevelUp();
      }
    }

    delay(1000);
  } else {
    // PN532 probably timed out waiting for a card
    Serial.println("Timed out waiting for a card");
  }
}