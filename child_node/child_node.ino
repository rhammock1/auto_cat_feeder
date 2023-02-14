/*
  Based on example ESPNOW sketch - Basic communication - Author: Arvind Ravulavaru <https://github.com/arvindr21>
*/

#include <esp_now.h>
#include <WiFi.h>
#include <ESP32Servo.h>

Servo auger; // continuous servo

#define CHANNEL 1

#define SERVO 26
#define STALL 90 // Middle of servo (no movement)
#define SPEED 100 // Rotation direction and speed 
#define DELAY 3000 // Time to keep servo active

#define PWR 33
#define CONNECTED 25// TBD

// Init ESP Now with fallback
void InitESPNow() {
  WiFi.disconnect();
  if (esp_now_init() == ESP_OK) {
    Serial.println("ESPNow Init Success");
  }
  else {
    Serial.println("ESPNow Init Failed");
    // Retry InitESPNow, add a counte and then restart?
    // InitESPNow();
    // or Simply Restart
    ESP.restart();
  }
}

// config AP SSID
void configDeviceAP() {
  String Prefix = "ESPChild:";
  String Mac = WiFi.macAddress();
  String SSID = Prefix + Mac;
  String Password = "?R3@L__s3curr!!";
  bool result = WiFi.softAP(SSID.c_str(), Password.c_str(), CHANNEL, 0);
  if (!result) {
    Serial.println("AP Config failed.");
  } else {
    Serial.println("AP Config Success. Broadcasting with AP: " + String(SSID));
  }
}

void handleFeed() {
  auger.attach(SERVO);
  delay(500);

  auger.write(SPEED);
  delay(DELAY);

  auger.write(STALL);
  delay(500);
  auger.detach();
}

void setup() {
  Serial.begin(115200);

  pinMode(PWR, OUTPUT);
  pinMode(CONNECTED, OUTPUT);

  digitalWrite(PWR, HIGH);
  digitalWrite(CONNECTED, LOW);
  
  Serial.println("Automatic Feeder --- Child Node")
  //Set device in AP mode to begin with
  WiFi.mode(WIFI_AP);
  // configure device AP mode
  configDeviceAP();
  // This is the mac address of the child in AP Mode
  Serial.print("AP MAC: "); Serial.println(WiFi.softAPmacAddress());
  // Init ESPNow with a fallback logic
  InitESPNow();
  // Once ESPNow is successfully Init, we will register for recv CB to
  // get recv packer info.
  esp_now_register_recv_cb(OnDataRecv);
}

// callback when data is recv from Parent
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  Serial.print("Last Packet Recv from: "); Serial.println(macStr);
  Serial.print("Last Packet Recv Data: "); Serial.println(*data);
  Serial.println("");
  digitalWrite(CONNECTED, HIGH);
  handleFeed();
}

void loop() {
  // Chill
}
