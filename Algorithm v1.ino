#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <MFRC522.h>
#include <Servo.h>
#include <time.h>
#include <ArduinoJson.h>

// WiFi credentials
const char *SSID = "Nahida akter";
const char *PASSWORD = "@1019290852";

// Hardware pins
#define SS_PIN D8
#define RST_PIN D3
#define BUZZER_PIN D1
#define SERVO_PIN D2

// WiFi and HTTP client
WiFiClient client;
HTTPClient http;

std::unique_ptr<BearSSL::WiFiClientSecure> secureClient(new BearSSL::WiFiClientSecure);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo feederServo;

const String API_URL = "https://avid-cuttlefish-149.convex.cloud/api";

// Global state trackers
const unsigned long servoOpenDuration = 1000;            // 1 second
const unsigned long timeoutWindow = 2 * 60;              // 2 minutes
const unsigned long queueFetchInterval = 10 * 60 * 1000; // 10 minutes
const unsigned long portionInterval = 1500;              // 1.5 seconds
unsigned long lastQueueFetchTime = 0;
unsigned long currentTime = 0;
unsigned long rfidWaitStart = 0;
unsigned long servoStartTime = 0;
unsigned long portionStartTime = 0;
int portionsDispensed = 0;
bool isFeeding = false;

// Beep patterns
const int PATTERN_SUCCESS[] = {100, 100, 300};
const int PATTERN_SKIPPED[] = {100, 500, 100};
const int PATTERN_ERROR[] = {400, 100, 100, 100, 400};

// Feeding queue state
String queueId = "";
String rfid = "";
int portion = 0;
int beepCount = 0;
bool isManual = false;
unsigned long feedTime = 0;

enum FSM_STATE
{
   INIT,
   FETCH_QUEUE,
   PROCESS_QUEUE,
   WAIT_FOR_RFID,
   FEED_PET,
   ERROR,
   IDLE
};

FSM_STATE currentState = INIT;

void connectToWiFi()
{
   WiFi.begin(SSID, PASSWORD);
   while (WiFi.status() != WL_CONNECTED)
   {
      Serial.println(".");
      delay(1000);
   }
   Serial.println("Connected to WiFi");
}

void setupTime()
{
   configTime(6 * 3600, 0, "pool.ntp.org"); // UTC+6 for Bangladesh
   Serial.print("Waiting for time sync");
   while (time(nullptr) < 100000)
   {
      delay(500);
      Serial.print(".");
   }
   Serial.println("\nTime synced!");
}

void useMutation(const String &requestBody)
{
   if (WiFi.status() == WL_CONNECTED)
   {
      secureClient->setInsecure(); // Bypass SSL certificate check

      http.begin(*secureClient, API_URL + "/mutation");
      http.addHeader("Content-Type", "application/json");
      int httpCode = http.POST(requestBody);

      if (httpCode != HTTP_CODE_OK)
      {
         Serial.println("Failed to send mutation. HTTP Code: " + String(httpCode));
      }

      http.end();
   }
}

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

void resetState()
{
   rfid = "";
   portion = 0;
   beepCount = 0;
   feedTime = 0;
}

String createActivityRequestBody(const String &activityType, const String &rfid = "")
{
   DynamicJsonDocument doc(512);

   doc["path"] = String("activities:") + (rfid != "" ? "logPetActivity" : "logDeviceActivity");
   doc["format"] = "json";

   JsonObject args = doc.createNestedObject("args");
   args["activityType"] = activityType;
   args["timestamp"] = time(nullptr);

   if (rfid != "")
   {
      args["rfid"] = rfid;
   }

   String requestBody;
   serializeJson(doc, requestBody);
   return requestBody;
}

void markQueuedFeedingAsCompleted(const String &queueId)
{
   DynamicJsonDocument doc(512);
   doc["path"] = "queue:complete";
   doc["format"] = "json";
   JsonObject args = doc.createNestedObject("args");
   args["id"] = queueId;

   String requestBody;
   serializeJson(doc, requestBody);
   useMutation(requestBody);
}

void skipFeeding(const String &rfid = "")
{
   Serial.println("Skipped feeding for " + rfid);
   beepInPattern(PATTERN_SKIPPED, 3);
   String requestBody = createActivityRequestBody("skip_feeding", rfid);
   useMutation(requestBody);
   resetState();
   currentState = IDLE;
}

void sendActivityLog(const String &type)
{
   String requestBody = createActivityRequestBody(type);
   useMutation(requestBody);
}

void parseQueuePayload(String &payload)
{
   JsonDocument responseDoc;
   DeserializationError error = deserializeJson(responseDoc, payload);

   if (!error)
   {
      JsonObject item = responseDoc["value"];

      if (!item.isNull())
      {
         queueId = item["_id"].as<String>();
         rfid = item["rfid"].as<String>();
         portion = item["portion"].as<int>();
         beepCount = item["beepCount"].as<int>();
         feedTime = item["feedTime"].as<unsigned long>();
         isManual = item["isManual"].as<bool>();

         currentState = PROCESS_QUEUE;
      }
      else
      {
         Serial.println("No items in queue");
         currentState = IDLE;
      }
   }
   else
   {
      Serial.println("JSON parsing failed");
      currentState = ERROR;
   }
   responseDoc.clear();
}

void fetchQueue()
{
   if (WiFi.status() == WL_CONNECTED)
   {
      secureClient->setInsecure(); // Bypass SSL certificate check
      Serial.println("Free heap: " + String(ESP.getFreeHeap()));

      http.begin(*secureClient, API_URL + "/query");
      http.addHeader("Content-Type", "application/json");

      JsonDocument doc;
      doc["path"] = "queue:getFirst";
      doc["format"] = "json";
      JsonObject args = doc["args"].to<JsonObject>();
      doc.shrinkToFit();

      String requestBody;
      serializeJson(doc, requestBody);

      Serial.println("Sending request to: " + API_URL + "/query");
      Serial.println("Request body: " + requestBody);
      int httpCode = http.POST(requestBody);

      if (httpCode == HTTP_CODE_OK)
      {
         lastQueueFetchTime = currentTime;
         String payload = http.getString();
         parseQueuePayload(payload);
      }
      else
      {
         Serial.println("Failed to fetch queue. HTTP Code: " + String(httpCode));
         currentState = IDLE;
      }
      http.end();
   }
}

void processQueue()
{
   time_t now = time(nullptr);

   if (now >= feedTime && now <= feedTime + timeoutWindow)
   {
      beep(beepCount);
      rfidWaitStart = millis();
      currentState = WAIT_FOR_RFID;
   }
   else if (now > feedTime + timeoutWindow)
   {
      skipFeeding(rfid);
   }
   else
   {
      currentState = IDLE;
   }
}

void checkRFID()
{
   if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial())
   {
      // Get scanned RFID
      String scannedRFID = "";
      for (byte i = 0; i < mfrc522.uid.size; i++)
      {
         if (mfrc522.uid.uidByte[i] < 0x10)
            scannedRFID += "0";
         scannedRFID += String(mfrc522.uid.uidByte[i], HEX);
      }

      // Check if scanned RFID matches the queue RFID
      scannedRFID.toUpperCase();
      if (scannedRFID == rfid)
      {
         beepInPattern(PATTERN_SUCCESS, 3);
         currentState = FEED_PET;
      }
      else
      {
         time_t now = time(nullptr);
         if (now < feedTime || now > feedTime + timeoutWindow)
         {
            String body = createActivityRequestBody("rfid_scan", scannedRFID);
            useMutation(body);
         }
      }
   }
}

void feedPet()
{
   feederServo.attach(SERVO_PIN);
   feederServo.write(180);
   servoStartTime = millis();
   isFeeding = true;
   portionsDispensed = 0;
   portionStartTime = millis();
}

void setup()
{
   Serial.begin(115200);
   connectToWiFi();
   setupTime();

   pinMode(SS_PIN, INPUT);      // SS Pin for MFRC522
   pinMode(RST_PIN, OUTPUT);    // RST Pin for MFRC522
   pinMode(BUZZER_PIN, OUTPUT); // Buzzer pin
   pinMode(SERVO_PIN, OUTPUT);  // Servo pin

   mfrc522.PCD_Init();
}

void loop()
{
   currentTime = millis();

   if (WiFi.status() != WL_CONNECTED)
   {
      connectToWiFi();
   }

   // Manage non-blocking servo operation
   if (isFeeding)
   {
      if (currentTime - servoStartTime >= 500 && feederServo.read() == 180)
      {
         feederServo.write(0); // Close after 0.5 sec
      }

      // Wait between portion cycles
      if (currentTime - portionStartTime >= portionInterval)
      {
         portionsDispensed++;
         if (portionsDispensed >= portion)
         {
            // Done feeding
            feederServo.detach();
            isFeeding = false;
            markQueuedFeedingAsCompleted(queueId);
            resetState();
            currentState = FETCH_QUEUE;
         }
         else
         {
            feederServo.write(180); // Open again
            servoStartTime = millis();
            portionStartTime = millis();
         }
      }
   }

   if (!isFeeding)
   {
      // Fetch queue every 10 minutes
      if (currentTime - lastQueueFetchTime >= queueFetchInterval)
      {
         fetchQueue();
         lastQueueFetchTime = currentTime;
      }

      switch (currentState)
      {
      case INIT:
         currentState = FETCH_QUEUE;
         break;

      case FETCH_QUEUE:
         Serial.println("Transitioning to FETCH_QUEUE");
         fetchQueue();
         break;

      case PROCESS_QUEUE:
         Serial.println("Transitioning to PROCESS_QUEUE");
         processQueue();
         break;

      case WAIT_FOR_RFID:
         Serial.println("Transitioning to WAIT_FOR_RFID");
         checkRFID();

         // Fallback: no RFID scanned within timeout window
         if (millis() - rfidWaitStart >= timeoutWindow * 1000)
         {
            Serial.println("RFID timeout reached, skipping feeding.");
            skipFeeding(rfid);
         }
         break;

      case FEED_PET:
         Serial.println("Transitioning to FEED_PET");
         feedPet();
         break;

      case ERROR:
         Serial.println("Transitioning to ERROR");
         beepInPattern(PATTERN_ERROR, 5);
         sendActivityLog("error");
         currentState = FETCH_QUEUE;
         break;

      case IDLE:
      default:
         Serial.println("Transitioning to IDLE");
         delay(1000);
         break;
      }
   }
}