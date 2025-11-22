// run at 192.168.4.1

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// ================= CONFIGURATION =================
const char* HOME_SSID = "wifi name";      // <--- CHANGE THIS
const char* HOME_PASS = "password";  // <--- CHANGE THIS
String GOOGLE_SCRIPT_ID = ""; 
// =================================================

const char* marauder_ssid = "Marauder Radar";
WebServer server(80);
BLEScan* pBLEScan;
String jsonDevices = "[]"; 
int deviceCount = 0;
unsigned long lastScanTime = 0;
unsigned long lastLogTime = 0;

// --- 1. SYNC LOGIC (Runs cleanly after a reboot) ---
void performSync() {
  Serial.println("\n[MODE] SYNC MODE STARTED (BLE Disabled for max memory)");
  
  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  // Universal DNS Fix
  WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(8,8,8,8), IPAddress(8,8,4,4));
  WiFi.begin(HOME_SSID, HOME_PASS);
  
  Serial.print("[SYNC] Connecting to Home WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) { delay(500); Serial.print("."); attempts++; }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SYNC] Connected!");
    
    File f = LittleFS.open("/log.txt", "r");
    if (f && f.size() > 0) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() < 5) continue;

        JsonDocument savedDoc;
        deserializeJson(savedDoc, line);
        
        JsonDocument payloadDoc;
        payloadDoc["count"] = savedDoc["count"];
        payloadDoc["logs"] = savedDoc["data"]; 
        
        String payload;
        serializeJson(payloadDoc, payload);

        HTTPClient http;
        String url = "https://script.google.com/macros/s/" + GOOGLE_SCRIPT_ID + "/exec";
        
        // Now we have plenty of RAM for HTTPS!
        http.begin(url.c_str());
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/json");

        int code = http.POST(payload);
        Serial.printf("[SYNC] Uploaded snapshot: %d\n", code);
        http.end();
      }
      f.close();
      LittleFS.remove("/log.txt"); 
      Serial.println("[SYNC] Logs Wiped.");
    } else {
      Serial.println("[SYNC] No logs found.");
    }
  } else {
    Serial.println("\n[SYNC] WiFi Failed.");
  }

  // Remove the sync flag so next boot is normal
  LittleFS.remove("/do_sync");
  Serial.println("[SYNC] Done. Restarting to Radar Mode...");
  delay(2000);
  ESP.restart();
}

// --- 2. RADAR LOGIC (Normal Operation) ---
class MyCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice d) {} 
};

void handleRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (f) { server.streamFile(f, "text/html"); f.close(); }
  else server.send(404, "text/plain", "Radar UI Missing");
}

void performScan() {
  BLEScanResults found = pBLEScan->start(3, false); 
  deviceCount = found.getCount();
  
  JsonDocument doc;
  for (int i = 0; i < deviceCount; i++) {
    BLEAdvertisedDevice d = found.getDevice(i);
    JsonObject obj = doc.add<JsonObject>();
    obj["addr"] = d.getAddress().toString();
    obj["rssi"] = d.getRSSI();
    obj["name"] = d.haveName() ? d.getName() : "-";
  }
  
  jsonDevices = "";
  serializeJson(doc, jsonDevices);
  pBLEScan->clearResults(); 
}

void logToMemory() {
  Serial.println("[LOG] Saving snapshot...");
  File f = LittleFS.open("/log.txt", "a");
  if (!f) return;

  JsonDocument doc;
  doc["count"] = deviceCount;
  doc["data"] = jsonDevices;
  String line;
  serializeJson(doc, line);
  f.println(line);
  f.close();
}

void triggerSyncRestart() {
  Serial.println("[CMD] Sync requested. Flagging and Rebooting...");
  // Create the flag file
  File f = LittleFS.open("/do_sync", "w");
  f.print("1");
  f.close();
  delay(500);
  ESP.restart(); // Reboot immediately
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  pinMode(0, INPUT_PULLUP);

  // CHECK FOR SYNC FLAG
  if (LittleFS.exists("/do_sync")) {
    performSync(); // This function connects, uploads, deletes flag, and reboots.
    return; // Never reach the code below
  }

  // NORMAL RADAR STARTUP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(marauder_ssid);
  Serial.print("Marauder Active. IP: "); Serial.println(WiFi.softAPIP());
  
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyCallbacks());
  pBLEScan->setActiveScan(true);

  server.on("/", handleRoot);
  server.on("/api/scan", [](){ server.send(200, "application/json", jsonDevices); });
  server.begin();
}

void loop() {
  // If we are here, we are in Radar Mode
  server.handleClient();
  
  if (millis() - lastScanTime > 5000) {
    performScan();
    lastScanTime = millis();
  }

  if (millis() - lastLogTime > 60000) {
    logToMemory();
    lastLogTime = millis();
  }

  // If Button Pressed -> Restart into Sync Mode
  if (digitalRead(0) == LOW) {
    delay(100);
    if (digitalRead(0) == LOW) triggerSyncRestart();
  }
}