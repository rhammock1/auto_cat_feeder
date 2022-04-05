#include <Wire.h>
#include "RTClib.h"
#include <esp_now.h>
#include <WiFi.h>
#include <ESP32Servo.h>

Servo auger; // continuous servo
RTC_DS1307 rtc;

// Global copy of slave
#define NUMSLAVES 3
esp_now_peer_info_t slaves[NUMSLAVES] = {};
int SlaveCnt = 0;

#define CHANNEL 1
#define PRINTSCANRESULTS 0

#define SERVO 26
#define STALL 91 // Middle of servo (no movement)
#define SPEED 100 // Rotation direction and speed 
#define DELAY 9000 // Time to keep servo active

#define PWR 33
#define CONNECTED 25 // TBD

int retries = 5;
DateTime lastFeed;

class Feed { 
  public:
    int morningHour = 8;
    int afternoonHour = 17;

    String morning = "8:00";
    String afternoon = "17:00";

    int feedDelay = 6; // arbitrary number to make sure it doesn't run multiple times in a TimeSpan

    String formatTime(DateTime current_time) {
      String hour = String(current_time.hour());
      String minute = String(current_time.minute());
      return hour + ":" + minute;
    }

    bool timeToFeed(DateTime current_time) {
      int currentHour = current_time.hour();

      TimeSpan span = TimeSpan(0, feedDelay, 0, 0); // d, |h|, m, s
      DateTime difference = current_time - span;
      bool feedThem = (currentHour == morningHour
        || currentHour == afternoonHour) 
        && difference >= lastFeed;
      return feedThem;
    }

    String getNextFeed(DateTime current_time) {
      int hour = current_time.hour();
      String next = hour > morningHour && hour <= afternoonHour ? afternoon : morning;

      return next;
    }
};

Feed feed;

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

// Scan for slaves in AP mode
void ScanForSlave() {
  int8_t scanResults = WiFi.scanNetworks();
  //reset slaves
  memset(slaves, 0, sizeof(slaves));
  SlaveCnt = 0;
  Serial.println("");
  if (scanResults == 0) {
    Serial.println("No WiFi devices in AP Mode found");
  } else {
    Serial.print("Found "); Serial.print(scanResults); Serial.println(" devices ");
    for (int i = 0; i < scanResults; ++i) {
      // Print SSID and RSSI for each device found
      String SSID = WiFi.SSID(i);
      int32_t RSSI = WiFi.RSSI(i);
      String BSSIDstr = WiFi.BSSIDstr(i);

      if (PRINTSCANRESULTS) {
        Serial.print(i + 1); Serial.print(": "); Serial.print(SSID); Serial.print(" ["); Serial.print(BSSIDstr); Serial.print("]"); Serial.print(" ("); Serial.print(RSSI); Serial.print(")"); Serial.println("");
      }
      delay(10);
      // Check if the current device starts with `Slave`
      if (SSID.indexOf("ESPChild") == 0) {
        // SSID of interest
        Serial.print(i + 1); Serial.print(": "); Serial.print(SSID); Serial.print(" ["); Serial.print(BSSIDstr); Serial.print("]"); Serial.print(" ("); Serial.print(RSSI); Serial.print(")"); Serial.println("");
        // Get BSSID => Mac Address of the Slave
        int mac[6];

        if ( 6 == sscanf(BSSIDstr.c_str(), "%x:%x:%x:%x:%x:%x",  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5] ) ) {
          for (int ii = 0; ii < 6; ++ii ) {
            slaves[SlaveCnt].peer_addr[ii] = (uint8_t) mac[ii];
          }
        }
        slaves[SlaveCnt].channel = CHANNEL; // pick a channel
        slaves[SlaveCnt].encrypt = 0; // no encryption
        SlaveCnt++;
      }
    }
  }

  if (SlaveCnt > 0) {
    Serial.print(SlaveCnt); Serial.println(" Slave(s) found, processing..");
  } else {
    Serial.println("No Slave Found, trying again.");
    digitalWrite(CONNECTED, LOW);
  }

  // clean up ram
  WiFi.scanDelete();
}

// Check if the slave is already paired with the master.
// If not, pair the slave with master
void manageSlave() {
  if (SlaveCnt > 0) {
    for (int i = 0; i < SlaveCnt; i++) {
      Serial.print("Processing: ");
      for (int ii = 0; ii < 6; ++ii ) {
        Serial.print((uint8_t) slaves[i].peer_addr[ii], HEX);
        if (ii != 5) Serial.print(":");
      }
      Serial.print(" Status: ");
      // check if the peer exists
      bool exists = esp_now_is_peer_exist(slaves[i].peer_addr);
      if (exists) {
        // Slave already paired.
        Serial.println("Already Paired");
        digitalWrite(CONNECTED, HIGH);
      } else {
        // Slave not paired, attempt pair
        esp_err_t addStatus = esp_now_add_peer(&slaves[i]);
        if (addStatus == ESP_OK) {
          // Pair success
          Serial.println("Pair success");
          digitalWrite(CONNECTED, HIGH);
        } else if (addStatus == ESP_ERR_ESPNOW_NOT_INIT) {
          // How did we get so far!!
          Serial.println("ESPNOW Not Init");
          digitalWrite(CONNECTED, LOW);
        } else if (addStatus == ESP_ERR_ESPNOW_ARG) {
          Serial.println("Add Peer - Invalid Argument");
          digitalWrite(CONNECTED, LOW);
        } else if (addStatus == ESP_ERR_ESPNOW_FULL) {
          Serial.println("Peer list full");
          digitalWrite(CONNECTED, LOW);
        } else if (addStatus == ESP_ERR_ESPNOW_NO_MEM) {
          Serial.println("Out of memory");
          digitalWrite(CONNECTED, LOW);
        } else if (addStatus == ESP_ERR_ESPNOW_EXIST) {
          Serial.println("Peer Exists");
          digitalWrite(CONNECTED, LOW);
        } else {
          Serial.println("Not sure what happened");
          digitalWrite(CONNECTED, LOW);
        }
        delay(100);
      }
    }
  } else {
    // No slave found to process
    Serial.println("No Slave found to process");
    digitalWrite(CONNECTED, LOW);
  }
}

void handleFeed() {
  // Run for half time
  // Rotate the opposite direction quickly to loosen any potential blockage
  // Continue 
  auger.attach(SERVO);
  delay(500);

  int half = DELAY / 2;
  auger.write(SPEED);
  delay(half);
  auger.write(80);
  delay(250);
  auger.write(SPEED);
  delay(half);

  auger.write(STALL);
  delay(500);
  auger.detach();
}

uint8_t data = 0;
// send data
bool sendData() {
  data++;
  Serial.print("CHILD COUNT: ");
  Serial.println(SlaveCnt);
  for (int i = 0; i < SlaveCnt; i++) {
    const uint8_t *peer_addr = slaves[i].peer_addr;
    if (i == 0) { // print only for first slave
      Serial.print("Sending: ");
      Serial.println(data);
    }
    esp_err_t result = esp_now_send(peer_addr, &data, sizeof(data));
    Serial.print("Send Status: ");
    if (result == ESP_OK) {
      Serial.println("Success");
      if(i == SlaveCnt - 1) {
        handleFeed();
        return false;
      }
    } else if (result == ESP_ERR_ESPNOW_NOT_INIT) {
      // How did we get so far!!
      Serial.println("ESPNOW not Init.");
      return true;
    } else if (result == ESP_ERR_ESPNOW_ARG) {
      Serial.println("Invalid Argument");
      return true;
    } else if (result == ESP_ERR_ESPNOW_INTERNAL) {
      Serial.println("Internal Error");
      return true;
    } else if (result == ESP_ERR_ESPNOW_NO_MEM) {
      Serial.println("ESP_ERR_ESPNOW_NO_MEM");
      return true;
    } else if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
      Serial.println("Peer not found.");
      return true;
    } else {
      Serial.println("Not sure what happened");
      return true;
    }
    delay(100);
  }
}

// callback when data is sent from Master to Slave
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  Serial.print("Last Packet Sent to: "); Serial.println(macStr);
  Serial.print("Last Packet Send Status: "); Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

void setup() {
  Serial.begin(115200);
  
  pinMode(PWR, OUTPUT);
  pinMode(CONNECTED, OUTPUT);

  digitalWrite(PWR, HIGH);
  digitalWrite(CONNECTED, LOW);

  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // sets RTC time to time this sketch was compiled
  
  //Set device in STA mode to begin with
  WiFi.mode(WIFI_STA);
  Serial.println("ESPNow/Multi-Slave/Master Example");
  // This is the mac address of the Master in Station Mode
  Serial.print("STA MAC: "); Serial.println(WiFi.macAddress());
  // Init ESPNow with a fallback logic
  InitESPNow();
  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  esp_now_register_send_cb(OnDataSent);
}

void loop() {
  // In the loop we scan for slave
  ScanForSlave();
  // If Slave is found, it would be populate in `slave` variable
  // We will check if `slave` is defined and then we proceed further
  if (SlaveCnt > 0) { // check if slave channel is defined
    // `slave` is defined
    // Add slave as peer if it has not been added already
    manageSlave();
    // pair success or already paired
    // Send data to device
    int current_try = 1;
    DateTime now = rtc.now();

    if(feed.timeToFeed(now)) {
      Serial.print(now.hour(), DEC);
      Serial.print(":");
      Serial.print(now.minute(), DEC);
      Serial.println(" Time to feed the kitties!");
      // Check if feedDelay is >= time elapsed since lastFeed
      while(current_try <= retries) {
        bool error = sendData();
        if(error) {
          Serial.println("ERROR... retrying!");
          current_try++;
        } else {
          Serial.println("Successfully sent data");
          break;
        }
      }
      lastFeed = now;
    } else {
      Serial.print("It is not the right time to feed the kitties");
      Serial.println(feed.formatTime(now));
      Serial.print("Next feeding at: ");
      Serial.println(feed.getNextFeed(now));
    }
    
  } else {
    // No slave found to process
  }

  delay(20); 
}
