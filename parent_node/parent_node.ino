#include <Wire.h>
#include "RTClib.h"
#include <esp_now.h>
#include <WiFi.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>

Servo auger; // continuous servo
RTC_DS1307 rtc;

// Global copy of child
#define NUMCHILDREN 3
esp_now_peer_info_t children[NUMCHILDREN] = {};
int ChildCnt = 0;

#define CHANNEL 1
#define PRINTSCANRESULTS 0
#define SETRTC 0

#define SERVO 26
#define STALL 91 // Middle of servo (no movement)
#define SPEED 100 // Rotation direction and speed 
#define DELAY 3000 // Time to keep servo active

#define PWR 33
#define CONNECTED 25 // TBD

int retries = 5;
DateTime lastFeed;

struct feed_times
{
  int hour;
  int minute;
  String printable_time;
};

class Feed { 
  public:
    feed_times midnight = {0, 0, "00:00 AM"};
    feed_times early = {4, 0, "04:00 AM"};
    feed_times morning = {8, 0, "08:00 AM"};
    feed_times lunch = {12, 0, "12:00 PM"};
    feed_times afternoon = {16, 0, "04:00 PM"};
    feed_times night = {20, 0, "08:00 PM"};

    int feedDelay = 2; // arbitrary number to make sure it doesn't run multiple times in a TimeSpan

    String formatTime(DateTime current_time) {
      String hour = String(current_time.hour());
      String minute = String(current_time.minute());
      return hour + ":" + minute;
    }

    bool timeToFeed(DateTime current_time) {
      int currentHour = current_time.hour();

      TimeSpan span = TimeSpan(0, feedDelay, 0, 0); // d, |h|, m, s
      DateTime difference = current_time - span;
      bool feedThem = (
        currentHour == midnight.hour
          || currentHour == early.hour
          || currentHour == morning.hour
          || currentHour == lunch.hour
          || currentHour == afternoon.hour
          || currentHour == night.hour
        ) && difference >= lastFeed;
      return feedThem;
    }

    String getNextFeed(DateTime current_time) {
      int hour = current_time.hour();
      String next;
      if(hour >= night.hour && minute > night.minute) {
        next = midnight.printable_time;
      } else if(hour >= afternoon.hour && minute > afternoon.minute) {
        next = night.printable_time;
      } else if(hour >= lunch.hour && minute > lunch.minute) {
        next = afternoon.printable_time;
      } else if(hour >= morning.hour && minute > morning.minute) {
        next = lunch.printable_time;
      } else if(hour >= early.hour && minute > early.minute) {
        next = morning.printable_time;
      } else {
        next = early.printable_time;
      }
      return next;
    }
};

Feed feed;

void logToServer(String message) {
  // Start WiFi
  WiFi.begin("SSID", "PASSWORD");
  Serial.println("Connecting to WiFi..");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println(".");
  }
  Serial.println("Connected to the WiFi network");

}

// Init ESP Now with fallback
void InitESPNow() {
  WiFi.disconnect();
  if(esp_now_init() == ESP_OK) {
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

// Scan for children in AP mode
void ScanForChild() {
  int8_t scanResults = WiFi.scanNetworks();
  //reset children
  memset(children, 0, sizeof(children));
  ChildCnt = 0;
  Serial.println("");
  if(scanResults == 0) {
    Serial.println("No WiFi devices in AP Mode found");
  } else {
    Serial.print("Found "); Serial.print(scanResults); Serial.println(" devices ");
    for(int i = 0; i < scanResults; ++i) {
      // Print SSID and RSSI for each device found
      String SSID = WiFi.SSID(i);
      int32_t RSSI = WiFi.RSSI(i);
      String BSSIDstr = WiFi.BSSIDstr(i);

      if(PRINTSCANRESULTS) {
        Serial.print(i + 1); Serial.print(": "); Serial.print(SSID); Serial.print(" ["); Serial.print(BSSIDstr); Serial.print("]"); Serial.print(" ("); Serial.print(RSSI); Serial.print(")"); Serial.println("");
      }
      delay(10);
      // Check if the current device starts with `Child`
      if(SSID.indexOf("ESPChild") == 0) {
        // SSID of interest
        Serial.print(i + 1); Serial.print(": "); Serial.print(SSID); Serial.print(" ["); Serial.print(BSSIDstr); Serial.print("]"); Serial.print(" ("); Serial.print(RSSI); Serial.print(")"); Serial.println("");
        // Get BSSID => Mac Address of the Child
        int mac[6];

        if(6 == sscanf(BSSIDstr.c_str(), "%x:%x:%x:%x:%x:%x",  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5])) {
          for(int ii = 0; ii < 6; ++ii ) {
            children[ChildCnt].peer_addr[ii] = (uint8_t) mac[ii];
          }
        }
        children[ChildCnt].channel = CHANNEL; // pick a channel
        children[ChildCnt].encrypt = 0; // no encryption
        ChildCnt++;
      }
    }
  }

  if(ChildCnt > 0) {
    Serial.print(ChildCnt); Serial.println(" Child(s) found, processing..");
  } else {
    Serial.println("No Child Found, trying again.");
    digitalWrite(CONNECTED, LOW);
  }

  // clean up ram
  WiFi.scanDelete();
}

// Check if the child is already paired with the parent.
// If not, pair the child with parent
void manageChild() {
  if(ChildCnt > 0) {
    for(int i = 0; i < ChildCnt; i++) {
      Serial.print("Processing: ");
      for(int ii = 0; ii < 6; ++ii ) {
        Serial.print((uint8_t) children[i].peer_addr[ii], HEX);
        if(ii != 5) Serial.print(":");
      }
      Serial.print(" Status: ");
      // check if the peer exists
      bool exists = esp_now_is_peer_exist(children[i].peer_addr);
      if(exists) {
        // Child already paired.
        Serial.println("Already Paired");
        digitalWrite(CONNECTED, HIGH);
      } else {
        // Child not paired, attempt pair
        esp_err_t addStatus = esp_now_add_peer(&children[i]);
        if(addStatus == ESP_OK) {
          // Pair success
          Serial.println("Pair success");
          digitalWrite(CONNECTED, HIGH);
        } else if(addStatus == ESP_ERR_ESPNOW_NOT_INIT) {
          // How did we get so far!!
          Serial.println("ESPNOW Not Init");
          digitalWrite(CONNECTED, LOW);
        } else if(addStatus == ESP_ERR_ESPNOW_ARG) {
          Serial.println("Add Peer - Invalid Argument");
          digitalWrite(CONNECTED, LOW);
        } else if(addStatus == ESP_ERR_ESPNOW_FULL) {
          Serial.println("Peer list full");
          digitalWrite(CONNECTED, LOW);
        } else if(addStatus == ESP_ERR_ESPNOW_NO_MEM) {
          Serial.println("Out of memory");
          digitalWrite(CONNECTED, LOW);
        } else if(addStatus == ESP_ERR_ESPNOW_EXIST) {
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
    // No child found to process
    Serial.println("No Child found to process");
    digitalWrite(CONNECTED, LOW);
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

uint8_t data = 0;
// send data
bool sendData() {
  data++;
  Serial.print("CHILD COUNT: ");
  Serial.println(ChildCnt);
  for(int i = 0; i < ChildCnt; i++) {
    const uint8_t *peer_addr = children[i].peer_addr;
    if(i == 0) { // print only for first child
      Serial.print("Sending: ");
      Serial.println(data);
    }
    esp_err_t result = esp_now_send(peer_addr, &data, sizeof(data));
    Serial.print("Send Status: ");
    if(result == ESP_OK) {
      Serial.println("Success");
      if(i == ChildCnt - 1) {
        handleFeed();
        return false;
      }
    } else if(result == ESP_ERR_ESPNOW_NOT_INIT) {
      // How did we get so far!!
      Serial.println("ESPNOW not Init.");
      return true;
    } else if(result == ESP_ERR_ESPNOW_ARG) {
      Serial.println("Invalid Argument");
      return true;
    } else if(result == ESP_ERR_ESPNOW_INTERNAL) {
      Serial.println("Internal Error");
      return true;
    } else if(result == ESP_ERR_ESPNOW_NO_MEM) {
      Serial.println("ESP_ERR_ESPNOW_NO_MEM");
      return true;
    } else if(result == ESP_ERR_ESPNOW_NOT_FOUND) {
      Serial.println("Peer not found.");
      return true;
    } else {
      Serial.println("Not sure what happened");
      return true;
    }
    delay(100);
  }
}

// callback when data is sent from Parent to Child
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

  if(!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  if(SETRTC) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // sets RTC time to time this sketch was compiled
  }
  
  //Set device in STA mode to begin with
  WiFi.mode(WIFI_STA);
  Serial.println("ESPNow/Multi-Child/Parent Example");
  // This is the mac address of the Parent in Station Mode
  Serial.print("STA MAC: "); Serial.println(WiFi.macAddress());
  // Init ESPNow with a fallback logic
  InitESPNow();
  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  esp_now_register_send_cb(OnDataSent);
}

void loop() {
  // In the loop we scan for child
  ScanForChild();
  // If Child is found, it would be populate in `child` variable
  // We will check if `child` is defined and then we proceed further
  if(ChildCnt > 0) { // check if child channel is defined
    // `child` is defined
    // Add child as peer if it has not been added already
    manageChild();
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
    // TODO
    // Add remote logging for when no children are found to process
  }
}
