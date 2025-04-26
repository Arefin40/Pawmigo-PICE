#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <MFRC522.h>
#include <Servo.h>
#include <Ultrasonic.h>

// WiFi credentials
const char *SSID = "your_SSID";
const char *PASSWORD = "your_PASSWORD";

// Hardware pins
#define RST_PIN 5
#define SS_PIN 4
#define TRIG_PIN 12
#define ECHO_PIN 14
#define SERVO_PIN 13

// WiFi and HTTP client
WiFiClient client;
HTTPClient http;

// RFID Setup
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Servo motor for feeding
Servo feederServo;

// Ultrasonic sensor for food level monitoring
Ultrasonic ultrasonic(TRIG_PIN, ECHO_PIN);

// Action queue URL
const String actionQueueURL = "http://your_api/queue";

// Global state trackers
String scheduledRFID = "";
int scheduledPortion = 0;
unsigned long currentTime = 0;
unsigned long timeoutWindow = 120000; // 2 minutes timeout
unsigned long lastActionTime = 0;

enum FSM_STATE
{
   INIT,
   FETCH_SCHEDULE,
   DISPATCH_ACTIONS,
   PROCESS_SCHEDULE,
   WAIT_FOR_RFID,
   FEED_PET,
   CHECK_FOOD_LEVEL,
   SEND_NOTIFICATION,
   ERROR,
   IDLE
};

FSM_STATE currentState = INIT;

// Function to connect to WiFi
void connectToWiFi()
{
   WiFi.begin(SSID, PASSWORD);
   while (WiFi.status() != WL_CONNECTED)
   {
      delay(1000);
   }
}

// Function to fetch schedules from backend
void fetchSchedules()
{
   if (WiFi.status() == WL_CONNECTED)
   {
      http.begin(client, "http://your_api/schedule");
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK)
      {
         String payload = http.getString();
         // Parse JSON and update scheduledRfid and scheduledPortion
         // Assuming the response format:
         // [{ "rfid": "B395BE0E", "portion": 3.0, "timestamp": "12:30:00" }]
         // You'll need to parse the JSON here accordingly
      }
      http.end();
   }
}

// Function to fetch actions from the queue
void fetchActions()
{
   if (WiFi.status() == WL_CONNECTED)
   {
      http.begin(client, actionQueueURL);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK)
      {
         String payload = http.getString();
         // Parse the action queue for manual feed actions
         // Assuming payload has { "action": "manual_feed", "rfid": "B395BE0E", "portion": 1.0 }
         // Update the scheduledRfid and scheduledPortion for manual feeding
      }
      http.end();
   }
}

// Function to check the RFID scan
void checkRFID()
{
   if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial())
   {
      String scannedRfid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++)
      {
         scannedRfid += String(mfrc522.uid.uidByte[i], HEX);
      }

      if (scannedRfid == scheduledRFID)
      {
         currentState = FEED_PET;
      }
      else if (millis() > currentTime + timeoutWindow)
      {
         // Skip this pet and move to next
         currentState = SEND_NOTIFICATION;
      }
   }
}

// Function to feed the pet
void feedPet()
{
   feederServo.write(90); // Dispense food (adjust angle based on your servo)
   delay(1000);           // Wait for food to dispense
   feederServo.write(0);  // Reset the servo position
   scheduledRFID = "";    // Reset scheduled pet
   scheduledPortion = 0;  // Reset portion
   currentState = SEND_NOTIFICATION;
}

// Function to monitor food level
void checkFoodLevel()
{
   long distance = ultrasonic.read();
   if (distance < 10)
   { // Low food level threshold (adjust as necessary)
     // Handle food refill logic
   }
   currentState = FETCH_SCHEDULE;
}

// Function to send notifications (simplified for this example)
void sendNotification(String message)
{
   if (WiFi.status() == WL_CONNECTED)
   {
      http.begin(client, "http://your_api/notification");
      http.addHeader("Content-Type", "application/json");
      String payload = "{\"message\": \"" + message + "\"}";
      http.POST(payload);
      http.end();
   }
}

void setup()
{
   Serial.begin(115200);
   connectToWiFi();
   mfrc522.PCD_Init();
   feederServo.attach(SERVO_PIN);
}

void loop()
{
   currentTime = millis();
   switch (currentState)
   {
   case INIT:
      currentState = FETCH_SCHEDULE;
      break;

   case FETCH_SCHEDULE:
      fetchSchedules();
      currentState = DISPATCH_ACTIONS;
      break;

   case DISPATCH_ACTIONS:
      fetchActions();
      currentState = PROCESS_SCHEDULE;
      break;

   case PROCESS_SCHEDULE:
      // Check if it's time to feed a pet
      if (scheduledRFID != "")
      {
         currentState = WAIT_FOR_RFID;
      }
      else
      {
         currentState = CHECK_FOOD_LEVEL;
      }
      break;

   case WAIT_FOR_RFID:
      checkRFID();
      break;

   case FEED_PET:
      feedPet();
      break;

   case CHECK_FOOD_LEVEL:
      checkFoodLevel();
      break;

   case SEND_NOTIFICATION:
      sendNotification("Feeding completed.");
      currentState = FETCH_SCHEDULE;
      break;

   case ERROR:
      sendNotification("Error occurred.");
      currentState = FETCH_SCHEDULE;
      break;

   case IDLE:
      delay(1000);
      break;
   }
}