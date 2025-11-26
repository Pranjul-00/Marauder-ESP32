#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// --- DUAL BLUETOOTH LIBRARIES ---
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BluetoothSerial.h>

// ================= CONFIGURATION =================
const char* HOME_SSID = "";      // <--- CHANGE THIS
const char* HOME_PASS = "";  // <--- CHANGE THIS
String GOOGLE_SCRIPT_ID = ""; 
// =================================================

WebServer server(80);
BLEScan* pBLEScan;
BluetoothSerial SerialBT;

// Data Storage
JsonDocument bleDoc;
JsonDocument classicDoc;
String finalJson = "[]"; 

unsigned long lastBleScan = 0;
unsigned long lastClassicScan = 0;
unsigned long lastLogTime = 0;

// --- 1. SYNC LOGIC (The Upload Function) ---
void performSyncMode() {
  Serial.println("\n[MODE] SYNC MODE STARTED");
  
  WiFi.mode(WIFI_STA);
  // DNS Fix
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

        // Re-package the saved data for Google
        JsonDocument savedDoc;
        deserializeJson(savedDoc, line);
        
        JsonDocument payloadDoc;
        payloadDoc["count"] = savedDoc["count"];
        payloadDoc["logs"] = savedDoc["data"]; 
        
        String payload;
        serializeJson(payloadDoc, payload);

        HTTPClient http;
        String url = "https://script.google.com/macros/s/" + GOOGLE_SCRIPT_ID + "/exec";
        http.begin(url.c_str());
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/json");

        int code = http.POST(payload);
        if (code > 0) Serial.printf("[SYNC] Uploaded: %d\n", code);
        else Serial.printf("[SYNC] Failed: %s\n", http.errorToString(code).c_str());
        
        http.end();
      }
      f.close();
      LittleFS.remove("/log.txt"); 
      Serial.println("[SYNC] Logs Wiped.");
    } else {
      Serial.println("[SYNC] No logs found.");
    }
  } else {
    Serial.println("\n[SYNC] WiFi Connection Failed.");
  }
  
  // Delete flag and restart to Radar Mode
  LittleFS.remove("/do_sync");
  Serial.println("[SYNC] Restarting to Radar...");
  delay(1000);
  ESP.restart();
}

// --- 2. SCANNING LOGIC ---
class MyCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice d) {} 
};

void scanBLE() {
  // Fast scan (2 seconds)
  BLEScanResults found = pBLEScan->start(2, false); 
  bleDoc.clear();
  
  for (int i = 0; i < found.getCount(); i++) {
    BLEAdvertisedDevice d = found.getDevice(i);
    JsonObject obj = bleDoc.add<JsonObject>();
    
    obj["addr"] = d.getAddress().toString().c_str();
    obj["rssi"] = d.getRSSI();
    obj["type"] = "BLE"; 
    
    String name = "-";
    if (d.haveName()) name = d.getName().c_str();
    else if (d.haveManufacturerData()) {
       std::string md = d.getManufacturerData();
       if (md.length()>=2 && md[0]==0x4C) name = "Apple Device";
       else if (md.length()>=2 && md[0]==0x06) name = "Microsoft Device";
       else if (md.length()>=2 && md[0]==0x75) name = "Samsung Device";
    }
    obj["name"] = name;
  }
  pBLEScan->clearResults();
}

void scanClassic() {
  Serial.println("[RADAR] Deep Scan (Classic) started...");
  // Freezes for 8-10 seconds
  BTScanResults *pResults = SerialBT.discover(8000); 
  
  classicDoc.clear();
  if (pResults) {
    for (int i = 0; i < pResults->getCount(); i++) {
      BTAdvertisedDevice* d = pResults->getDevice(i);
      JsonObject obj = classicDoc.add<JsonObject>();
      
      obj["addr"] = d->getAddress().toString().c_str();
      obj["rssi"] = d->getRSSI();
      obj["type"] = "CL"; // Classic Tag
      
      String name = d->getName().c_str();
      if (name.length() == 0) name = "Unknown Classic Device";
      obj["name"] = name;
    }
  }
  Serial.println("[RADAR] Deep Scan Complete.");
}

void updateWebData() {
  JsonDocument merged;
  for (JsonVariant v : bleDoc.as<JsonArray>()) merged.add(v);
  for (JsonVariant v : classicDoc.as<JsonArray>()) merged.add(v);
  
  finalJson = "";
  serializeJson(merged, finalJson);
}

void triggerSync() {
    Serial.println("Sync Requested. Rebooting...");
    
    // Save current state before reboot
    File f = LittleFS.open("/log.txt", "a");
    String line;
    JsonDocument wrap;
    wrap["count"] = bleDoc.size() + classicDoc.size();
    wrap["data"] = finalJson;
    serializeJson(wrap, line);
    f.println(line);
    f.close();
    
    // Set flag
    File flag = LittleFS.open("/do_sync", "w");
    flag.print("1");
    flag.close();
    delay(200);
    ESP.restart();
}

void handleRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (f) { server.streamFile(f, "text/html"); f.close(); }
  else server.send(404, "text/plain", "Radar UI Missing");
}

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  pinMode(0, INPUT_PULLUP);

  // CHECK FOR SYNC FLAG FIRST
  if (LittleFS.exists("/do_sync")) {
    performSyncMode();
    return; // Stop here
  }

  // NORMAL STARTUP
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Marauder Hybrid");
  Serial.print("Radar IP: "); Serial.println(WiFi.softAPIP());

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyCallbacks());
  pBLEScan->setActiveScan(true);

  SerialBT.begin("Marauder_Host"); // Classic Init

  server.on("/", handleRoot);
  server.on("/api/scan", [](){ server.send(200, "application/json", finalJson); });
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // 1. Fast BLE Scan (Every 4s)
  if (now - lastBleScan > 4000) {
    scanBLE();
    updateWebData();
    lastBleScan = now;
  }

  // 2. Slow Classic Scan (Every 45s)
  if (now - lastClassicScan > 45000) {
    scanClassic(); 
    updateWebData();
    lastClassicScan = now;
  }

  // 3. Auto-Log to memory (Every 60s)
  if (now - lastLogTime > 60000) {
    Serial.println("[LOG] Auto-saving snapshot...");
    File f = LittleFS.open("/log.txt", "a");
    String line;
    JsonDocument wrap;
    wrap["count"] = bleDoc.size() + classicDoc.size();
    wrap["data"] = finalJson;
    serializeJson(wrap, line);
    f.println(line);
    f.close();
    lastLogTime = now;
  }

  // 4. Button Press -> Sync
  if (digitalRead(0) == LOW) {
    delay(100);
    if (digitalRead(0) == LOW) triggerSync();
  }
}