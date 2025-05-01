#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <MFRC522.h>
#include <Servo.h>
#include <SPI.h>

// Hardware pins
#define BUZZER_PIN D1
#define SERVO_PIN D2
#define RST_PIN D3
#define SS_PIN D8

// Network credentials
const char SSID[] PROGMEM = "UAP";
const char PASSWORD[] PROGMEM = "abcelp@uap";
const char API_URL[] = "https://avid-cuttlefish-149.convex.cloud/api";

// Initialize modules
HTTPClient http;
WiFiUDP ntpUDP;
Servo feederServo;
MFRC522 rfid(SS_PIN, RST_PIN);
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);
std::unique_ptr<BearSSL::WiFiClientSecure> secureClient(new BearSSL::WiFiClientSecure);

// Beep patterns
const int PATTERN_SUCCESS[] = {100, 100, 300};
const int PATTERN_SKIPPED[] = {100, 500, 100};
const int PATTERN_ERROR[] = {400, 100, 100, 100, 400};

// Feeding queue structure
struct FeedingQueue
{
   String id;
   String rfid;
   int portion = 0;
   int beep = 0;
   bool isManual = false;
   unsigned long feedTime = 0;

   // Parameterized constructor
   FeedingQueue(const String &id, const String &rfid, int portion, int beep, bool isManual, unsigned long feedTime)
       : id(id), rfid(rfid), portion(portion), beep(beep), isManual(isManual), feedTime(feedTime) {}

   FeedingQueue()
   {
      clear();
   }

   void clear()
   {
      id = "";
      rfid = "";
      portion = 0;
      beep = 0;
      feedTime = 0;
      isManual = false;
   }

   bool shouldFeedNow() const
   {
      Serial.println("feedTime: " + String(feedTime) + "; CurrentTime: " + String(timeClient.getEpochTime()));
      if (isEmpty())
         return false;
      return feedTime > 0 && timeClient.getEpochTime() >= feedTime;
   }

   bool isEmpty() const
   {
      return id.isEmpty() && rfid.isEmpty() && beep == 0 && portion == 0 && feedTime == 0;
   }
};

FeedingQueue queue;
String scannedRFID = "";
bool servoAttached = false;
bool waitingForPet = false;

// Time tracking
unsigned long lastTimeSync = 0;
unsigned long lastFeedTime = 0;
unsigned long lastRFIDCheck = 0;
unsigned long lastFeedTimeCheck = 0;
unsigned long lastQueueFetchTime = 0;
unsigned long feedingStartWaitTime = 0;

// Intervals
const int servoTimeUnit = 100;                              // 100ms
const unsigned long rfidCheckInterval = 200;                // 200ms
const unsigned long feedTimeCheckInterval = 10 * 1000;      // 10s
const unsigned long timeSyncCheckInterval = 60 * 60 * 1000; // 1hour
const unsigned long queueFetchInterval = 60 * 1000;         // 30s
const unsigned long feedingWaitTimeout = 5 * 60 * 1000;     // 5 minutes

void setup()
{
   Serial.begin(115200);

   // Initialize modules
   pinMode(BUZZER_PIN, OUTPUT);
   digitalWrite(BUZZER_PIN, LOW);
   SPI.begin();
   rfid.PCD_Init();

   // Configure WiFi
   WiFi.mode(WIFI_STA);
   WiFi.setAutoReconnect(true);
   WiFi.persistent(true);
   WiFi.begin(FPSTR(SSID), FPSTR(PASSWORD));

   Serial.print("Connecting to WiFi");
   unsigned long wifiStart = millis();
   while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000)
   {
      delay(200);
      Serial.print(".");
   }

   if (WiFi.status() != WL_CONNECTED)
   {
      Serial.println("\nFailed to connect to WiFi");
   }
   else
   {
      Serial.println("\nConnected!");
   }

   // Initialize time client
   timeClient.begin();
   if (!syncNTPTime())
   {
      Serial.println("Warning: Continuing without time sync");
   }

   Serial.println("System initialized");
   fetchQueue();
}

void loop()
{
   // Check if time is synced
   if (millis() - lastTimeSync > timeSyncCheckInterval)
   {
      Serial.println("Checking time sync");
      syncNTPTime();
      lastTimeSync = millis();
   }

   // Fetch queue periodically when its empty
   if (millis() - lastQueueFetchTime > queueFetchInterval && queue.id.isEmpty())
   {
      fetchQueue();
   }

   // Check RFID periodically
   if (millis() - lastRFIDCheck > rfidCheckInterval)
   {
      // Serial.println("Checking RFID");
      checkRFID();
      lastRFIDCheck = millis();
   }

   // Check feeding time periodically
   if (millis() - lastFeedTimeCheck > feedTimeCheckInterval)
   {
      Serial.println("Checking feeding time");
      Serial.println("Should feed: " + String(queue.shouldFeedNow()) + "; Wait For State: " + waitingForPet);
      if (queue.shouldFeedNow() && !waitingForPet)
      {
         waitingForPet = true;
         Serial.println("Waiting for pet: " + queue.rfid);
         beep(queue.beep);
         feedingStartWaitTime = millis();
      }
      lastFeedTimeCheck = millis();
   }

   // Check if we're waiting for the right pet and timeout has occurred
   if (waitingForPet && (millis() - feedingStartWaitTime > feedingWaitTimeout))
   {
      Serial.println("Timeout waiting for pet " + queue.rfid + " - skipping feeding");
      skipFeeding();
      waitingForPet = false;
   }

   // Detach servo if its not used
   if (millis() - lastFeedTime > 60 * 100 && servoAttached)
   {
      Serial.println("Detaching Servo");
      feederServo.detach();
      servoAttached = false;
   }

   // Check system health
   // checkSystemHealth();
   delay(500);
}

// ===============================================

void beep(int times)
{
   for (int i = 0; i < times; i++)
   {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
      delay(100);
   }
}

void beepInPattern(const int *pattern, int length)
{
   for (int i = 0; i < length; i++)
   {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(pattern[i]);
      digitalWrite(BUZZER_PIN, LOW);
      delay(100);
   }
}

bool syncNTPTime()
{
   for (int i = 0; i < 3; i++)
   {
      timeClient.update();
      if (timeClient.getEpochTime() > 100000)
      {
         return true;
      }
      delay(1000);
   }
   Serial.println("NTP sync failed after 3 attempts");
   return false;
}

String readRFID()
{
   if (!rfid.PICC_IsNewCardPresent())
      return "";

   if (!rfid.PICC_ReadCardSerial())
   {
      rfid.PICC_HaltA();
      return "";
   }

   char hexString[15] = {0};
   for (byte i = 0; i < rfid.uid.size; i++)
   {
      sprintf(hexString + i * 2, "%02X", rfid.uid.uidByte[i]);
   }

   rfid.PICC_HaltA();
   rfid.PCD_StopCrypto1();

   return String(hexString);
}

void checkRFID()
{
   scannedRFID = readRFID();
   if (scannedRFID == "")
      return;
   Serial.println("RFID: " + scannedRFID);

   if (waitingForPet)
   {
      if (scannedRFID == queue.rfid)
      {
         waitingForPet = false;
         handleFeeding();
      }
      else
      {
         Serial.println("Wrong pet detected!");
         sendPostRequest("activities:logPetActivity", [&](JsonObject &args)
                         {
        args["rfid"] = scannedRFID;
        args["activityType"] = "rfid_scan";
        args["timestamp"] = timeClient.getEpochTime(); });
      }
   }
}

void dispenseFood()
{
   Serial.println("Dispansing food");
   if (!servoAttached)
   {
      feederServo.attach(SERVO_PIN);
      servoAttached = true;
   }

   feederServo.write(180);
   delay(queue.portion * servoTimeUnit);
   feederServo.write(0);
}

void handleFeeding()
{
   Serial.println("Handling feed");
   dispenseFood();
   markQueuedFeedingAsCompleted();
   logPetActivity(queue.isManual ? "manual_feeding" : "scheduled_feeding");
   queue.clear();
}

// ===============================================

template <typename ArgBuilder>
void sendPostRequest(const String &path, ArgBuilder &&argBuilder)
{
   if (WiFi.status() == WL_CONNECTED)
   {
      secureClient->setInsecure();

      http.begin(*secureClient, String(API_URL) + "/mutation");
      http.addHeader("Content-Type", "application/json");

      DynamicJsonDocument doc(256);
      doc["path"] = path;
      doc["format"] = "json";
      JsonObject args = doc.createNestedObject("args");
      argBuilder(args);
      String requestBody;
      serializeJson(doc, requestBody);

      int httpCode = http.POST(requestBody);

      if (httpCode != HTTP_CODE_OK)
      {
         Serial.println("Failed to send mutation. HTTP Code: " + String(httpCode));
         Serial.println("Request body: " + requestBody);
         Serial.println("Response: " + http.getString());
      }

      http.end();
   }
   else
   {
      Serial.println("WiFi not connected. Mutation not sent.");
   }
}

void logDeviceActivity(const String &activityType)
{
   Serial.println("Logging device activity: " + activityType);
   return sendPostRequest("activities:logDeviceActivity", [&](JsonObject &args)
                          {
    args["activityType"] = activityType;
    args["timestamp"] = timeClient.getEpochTime(); });
}

void logPetActivity(const String &activityType)
{
   return sendPostRequest("activities:logPetActivity", [&](JsonObject &args)
                          {
    args["rfid"] = queue.rfid;
    args["activityType"] = activityType;
    args["timestamp"] = timeClient.getEpochTime(); });
}

void skipFeeding()
{
   if (queue.isEmpty())
      return;
   Serial.println("Skipped feeding for " + queue.rfid);
   logPetActivity("skip_feeding");
   queue.clear();
}

void markQueuedFeedingAsCompleted()
{
   Serial.println("Marking complete: " + queue.id);
   return sendPostRequest("queue:complete", [&](JsonObject &args)
                          { args["id"] = queue.id; });
}

bool parseQueuePayload(String &payload)
{
   Serial.println("Parsing queue");
   Serial.println(payload);
   DynamicJsonDocument responseDoc(512);
   DeserializationError error = deserializeJson(responseDoc, payload);
   bool success = false;

   if (!error)
   {
      // Serial.println("No error while parsing queue");
      JsonObject item = responseDoc["value"];

      if (!item.isNull())
      {
         Serial.println("Item exits");
         // Safely extract each field with checks
         queue.id = item["_id"].as<String>();
         queue.rfid = item["rfid"].as<String>();
         queue.beep = item["beep"].as<int>();
         queue.portion = item["portion"].as<int>();
         queue.feedTime = item["timestamp"].as<unsigned long>();
         queue.isManual = item["isManual"].as<bool>();
      }
      else
      {
         Serial.println("No items in queue");
      }

      success = true;
   }
   else
   {
      Serial.println("JSON parsing failed");
   }
   responseDoc.clear();
   return success;
}

void fetchQueue()
{
   Serial.println("Fetching queue");

   if (WiFi.status() != WL_CONNECTED)
   {
      Serial.println("WiFi is not connected. Failed to fetch queue.");
      return;
   }

   secureClient->setInsecure();

   http.begin(*secureClient, String(API_URL) + "/query");
   http.addHeader("Content-Type", "application/json");

   JsonDocument doc;
   doc["path"] = "queue:getFirst";
   doc["format"] = "json";
   JsonObject args = doc["args"].to<JsonObject>();
   doc.shrinkToFit();
   String requestBody;
   serializeJson(doc, requestBody);

   int httpCode = http.POST(requestBody);

   if (httpCode == HTTP_CODE_OK)
   {
      Serial.println("Fetch queue success");
      lastQueueFetchTime = millis();
      String payload = http.getString();
      parseQueuePayload(payload);
   }
   else
   {
      Serial.println("Failed to fetch queue. HTTP Code: " + String(httpCode));
   }
   http.end();
}

// ===============================================

// void checkSystemHealth() {
//   Serial.println("Checking system health");
//   static unsigned long lastCheck = 0;
//   if (millis() - lastCheck > 60000) {  // Every minute
//     if (WiFi.status() != WL_CONNECTED) {
//       Serial.println("WiFi connection lost");
//     }
//     if (timeClient.getEpochTime() < 100000) {
//       Serial.println("Time not synchronized");
//     }
//     lastCheck = millis();
//   }
// }