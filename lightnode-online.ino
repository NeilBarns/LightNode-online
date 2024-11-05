#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <ArduinoOTA.h>
#include <FirebaseESP8266.h>

#define RELAY_PIN D1
#define AP_BUTTON_PIN D2
#define PUSH_BUTTON_PIN D3
#define AP_LED_PIN D4
#define ERROR_LED_PIN D7
#define PROCESSING_LED_PIN D8

WiFiServer telnetServer(23);
WiFiClient telnetClient;

// Firebase credentials
#define FIREBASE_HOST "lightmaster-beta-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "PNLUl700R2TkJjVpOaX5XLhhJbSI7ycN7DwpWuTk"  // Replace this with your database secret

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

// Configuration parameters
char ssid[32] = "";
char password[32] = ""; 
char serverAppDomain[32] = ""; 
char clientName[32] = ""; 
char serverAppPort[6] = "";
const char* registerDeviceURL = "/api/device/insert";
const char* updateDeviceURL = "/api/device/update";
const char* stopDeviceTimeURL = "/api/device-time/end";
const char* pauseDeviceTimeURL = "/api/device-time/pause";
char deviceName[32] = "";
int deviceId = 0;

// Network configuration
IPAddress local_IP; 
IPAddress gateway;  
IPAddress subnet;   

// Store network configuration as strings
char ipString[16] = "";
char gatewayString[16] = "";
char subnetString[16] = "";

String manufacturerDeviceName = "Lightnode";
String versionNumber = "1.0.9-0";
String APSSID = manufacturerDeviceName + "-" + versionNumber + "-AP";
String APPass = "L1ghtN0d3@2024";
const char* mDNSHostname = "110lightnode"; 
const char* otaPassword = "L1ghtN0d3@2024";
char hostURL[128];
String firebasePath = "";

// SPIFFS
const char* timeFilePath = "/time.txt";
const char* errorFilePath = "/error.txt";

ESP8266WebServer server(80);

bool isRegistered = false; 
bool isLEDOn = false; // State of the LED
bool isTesting = false; // State of test light
bool isDisabled = false; // State of disabled device
bool isPaused = false; //State of paused time
bool isFree = false; //State of free light
bool isOpenTime = false; //State of open time
int storedTimeInSeconds = 0;
bool writeToFirebase = false;
bool requireRestart = false;
bool endingTime = false;

bool isButtonCurrentlyPressed = false; // State of the button press
bool previousButtonState = HIGH; // Last known state of the button
unsigned long buttonDebounceStartTime = 0;
const unsigned long APDebounceDelay = 50; // Debounce time in milliseconds
unsigned long APLastDebounceTime = 0;
bool APButtonPressed = false;
unsigned int watchdogIntervalMinutes = 10;

// Time tracking
unsigned long lastMillis = 0; // Last recorded time
unsigned long offDuration = 0; // Time duration the device was off

unsigned long lastRetryTime = 0;
const unsigned long retryInterval = 60000; // Retry every 60 seconds

// Watchdog timer setup
Ticker restartTicker;

// Last state of the button
bool APLastButtonState = HIGH;

struct Request {
    String method;
    String url;
    String payload;
};

std::vector<Request> requestQueue;

void addToQueue(String method, String url, String payload) {
    Request req = {method, url, payload};
    requestQueue.push_back(req);
}

// Function to retry sending queued requests
void retryQueuedRequests() {
  Serial.println("retryQueuedRequests");
  checkFileContent("/logs.txt");
    for (auto it = requestQueue.begin(); it != requestQueue.end(); ) {
        if (sendRequest(it->method, it->url, it->payload)) {
            it = requestQueue.erase(it);  // Remove successful requests
            delay(5000);
            ESP.restart();
        } else {
            ++it;  // Try the next request if current one fails
        }
    }
}

void checkFileContent(const char* filePath) {
    File file = SPIFFS.open(filePath, "r");
    
    if (!file) {
        Serial.println("Failed to open file for reading.");
        return;
    }

    if (file.size() == 0) {
        Serial.println("File is empty.");
        digitalWrite(ERROR_LED_PIN, LOW);
    } else {
        Serial.println("File has content:");
        digitalWrite(ERROR_LED_PIN, HIGH);
    }
    
    file.close();
}

bool sendRequest(String method, String url, String payload) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        WiFiClient client;

        http.begin(client, url);
        http.addHeader("Content-Type", "application/json");

        int httpResponseCode = -1;

        if (method == "POST") {
            httpResponseCode = http.POST(payload);
        } else if (method == "GET") {
            httpResponseCode = http.GET();
        }

        if (httpResponseCode >= 200 && httpResponseCode < 300) {
            Serial.println("Server response: " + http.getString());
            http.end();
            return true;  // Request succeeded
        } else {
            Serial.println("Failed to send request, response code: " + String(httpResponseCode));
            http.end();
            return false;  // Request failed
        }
    } else {
        Serial.println("No WiFi connection.");
        return false;  // No WiFi connection
    }
}

void streamCallback(StreamData data) {
  if (data.dataType() == "json") {

    FirebaseJson json = data.jsonObject();
    FirebaseJsonData jsonData;
    
    String command = "";
    int span = 0;

    if (json.get(jsonData, "command")) {
      Serial.print("Command: ");
      Serial.println(jsonData.stringValue);
      command = jsonData.stringValue;
    }

    // Extract the 'span' value from the JSON
    if (json.get(jsonData, "span")) {
      Serial.print("Span: ");
      Serial.println(jsonData.intValue);
      span = jsonData.intValue;
    }

    if (command == "delete") {
        deleteCommand();
        writeToFirebase = true;
    } 
    else if (command == "test") {
        testCommand();
        writeToFirebase = true;
    }
    else if (command == "disable") {
        disableCommand();
        writeToFirebase = true;
    }
    else if (command == "enable") {
        enableCommand();
        writeToFirebase = true;
    }
    else if (command == "setWatchdogInterval") {
        setWatchdogIntervalCommand(span);
        writeToFirebase = true;
    }
    else if (command == "startRatedTime") {
        startRatedTimeCommand(span);
        writeToFirebase = true;
    }
    else if (command == "startOpenTime") {
        startOpenTimeCommand();
        writeToFirebase = true;
    }
    else if (command == "endTime") {
        endTimeCommand();
        writeToFirebase = true;
        endingTime = true;
    }
    else if (command == "extendTime") {
        startRatedTimeCommand(span);
        writeToFirebase = true;
    }
    else if (command == "pauseTime") {
        pauseCommand();
        writeToFirebase = true;
    }
    else if (command == "resumeTime") {
        resumeCommand();
        writeToFirebase = true;
    }
    else if (command == "startFree") {
        startFreeCommand();
        writeToFirebase = true;
    }
    else if (command == "stopFree") {
        stopFreeCommand();
        writeToFirebase = true;
    }
    else {
        writeToFirebase = false;
    }
  }
}


void stopFreeCommand()
{
    bool errorOccurred = false;
    String errorMessage;
    
    Serial.println("Stop Free light");
    
    // Attempt to stop free mode
    isFree = false;
    EEPROM.put(211, isFree);
    
    // Check if EEPROM commit is successful
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit free state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
    
    // Turn off the light
    digitalWrite(RELAY_PIN, LOW);
    
    // Respond to the client
    if (errorOccurred) {
      server.send(500, "text/plain", "Failed to stop free light.");
    } else {
      server.send(200, "text/plain", "Free light stopped");
    }
}

void startFreeCommand()
{
    bool errorOccurred = false;
    String errorMessage;
    
    Serial.println("Free light");
    
    // Set the free state
    isFree = true;
    EEPROM.put(211, isFree);
    
    // Turn on the light
    digitalWrite(RELAY_PIN, HIGH);
    
    // Check if EEPROM commit is successful
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit free state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
}

void resumeCommand()
{
    bool errorOccurred = false;
    String errorMessage;
    
    // Attempt to resume the device
    isPaused = false;
    EEPROM.put(210, isPaused);
    
    // Check if EEPROM commit for isPaused is successful
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit resume state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
    
    // Attempt to turn on the LED
    digitalWrite(RELAY_PIN, HIGH);
    isLEDOn = true;
    EEPROM.put(300, isLEDOn);
    
    // Check if EEPROM commit for isLEDOn is successful
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit LED state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
}

void pauseCommand()
{
    bool errorOccurred = false;
    String errorMessage;
    
    // Attempt to pause the device
    isPaused = true;
    EEPROM.put(210, isPaused);
    
    // Check if EEPROM commit for isPaused is successful
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit pause state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
    
    // Attempt to turn off the LED
    digitalWrite(RELAY_PIN, LOW);
    isLEDOn = false;
    EEPROM.put(300, isLEDOn);
    
    // Check if EEPROM commit for isLEDOn is successful
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit LED state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
}

void startOpenTimeCommand()
{
    bool errorOccurred = false;
    String errorMessage;
    
    Serial.println("Open time");
    
    // Set the open time state
    isOpenTime = true;
    EEPROM.put(212, isOpenTime);
    
    // Turn on the light
    digitalWrite(RELAY_PIN, HIGH);
    
    // Check if EEPROM commit is successful
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit free state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
    
    // Respond to the client
    if (errorOccurred) {
      if (errorOccurred) {
        errorMessage = "Error starting open time";
        logMessage("Error: " + errorMessage);
      }
    }
}

void endTimeCommand()
{
    bool errorOccurred = false;
    String errorMessage;
    
    Serial.println("Forced stop");

    // Attempt to turn off LED and update EEPROM
    digitalWrite(RELAY_PIN, LOW);
    isLEDOn = false;
    EEPROM.put(300, isLEDOn);
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit LED state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
    
    // Attempt to update EEPROM
    isPaused = false;
    EEPROM.put(210, isPaused);
    isOpenTime = false;
    EEPROM.put(212, isOpenTime);
    
    if (!EEPROM.commit()) {
      errorMessage = "Failed to commit paused state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    }
    
    // Respond to the client with storedTimeInSeconds
    if (errorOccurred) {
      errorMessage = "Error ending time";
      logMessage("Error: " + errorMessage);
    } else {
      if (!writeTimeToSPIFFS(0)) {
        errorMessage = "Failed to write time to SPIFFS.";
        logMessage("Error: " + errorMessage);
        errorOccurred = true;
      } else {
        storedTimeInSeconds = 0;
      }
      delay(1000);
      requireRestart = true;
    }
}

void startRatedTimeCommand(int span)
{
    bool errorOccurred = false;
    String errorMessage;
    int temp_storedTimeInSeconds = 0;
    
    int timeInSeconds = span;
    
    // Check if conversion was successful
    if (timeInSeconds == 0) {
      errorMessage = "Invalid time value: " + String(timeInSeconds);
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    } else if (timeInSeconds < 0) {
      errorMessage = "Negative time value not allowed: " + String(timeInSeconds);
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
    } else {
      temp_storedTimeInSeconds = readTimeFromSPIFFS();
      if (temp_storedTimeInSeconds < 0) {
        errorMessage = "Failed to read time from SPIFFS.";
        logMessage("Error: " + errorMessage);
        errorOccurred = true;
      } else {
        Serial.println("timeInSeconds:" + String(timeInSeconds));
        Serial.println("storedTimeInSeconds:" + String(temp_storedTimeInSeconds));
        temp_storedTimeInSeconds += timeInSeconds;
        Serial.println("extended-storedTimeInSeconds:" + String(temp_storedTimeInSeconds));
    
        if (!writeTimeToSPIFFS(temp_storedTimeInSeconds)) {
          errorMessage = "Failed to write time to SPIFFS.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
        } else {
          storedTimeInSeconds = temp_storedTimeInSeconds;
        }
      }
    }
    
    if (errorOccurred) {
      errorMessage = "Error starting rated time";
      logMessage("Error: " + errorMessage);
    }
}

void setWatchdogIntervalCommand(int interval)
{
  Serial.println("intervalStr: " + String(interval));
  if (interval > 0) {
      watchdogIntervalMinutes = interval * 60;
      Serial.println("watchdogIntervalMinutes: " + String(watchdogIntervalMinutes));
      saveConfig();
      restartTicker.detach(); // Stop the previous ticker
      restartTicker.attach(watchdogIntervalMinutes, []() {
        if (storedTimeInSeconds > 0 || isLEDOn || isFree || isOpenTime) {
            Serial.println("Watchdog maintenance skipped due to active timer or light on.");
        } else {
            saveState();
            Serial.println(F("Restarting"));
            ESP.restart();
        }
      });
  server.send(200, "text/plain", "Watchdog interval set to " + String(watchdogIntervalMinutes) + " minutes.");
  } else {
      server.send(400, "text/plain", "Invalid interval. Must be greater than 0.");
  }
}

void enableCommand()
{
  bool errorOccurred = false;
  String errorMessage;

  // Attempt to enable the device
  isDisabled = false;
  EEPROM.put(209, isDisabled);

  // Check if EEPROM commit is successful
  if (!EEPROM.commit()) {
      errorMessage = "Failed to commit enabled state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
  }

  // Respond to the client
  if (errorOccurred) {
      server.send(500, "text/plain", "Failed to enable the device.");
  } else {
      server.send(200, "text/plain", "Device enabled");
  }
}

void disableCommand()
{
  bool errorOccurred = false;
  String errorMessage;
  
  // Attempt to disable the device
  isDisabled = true;
  EEPROM.put(209, isDisabled);
  
  // Check if EEPROM commit is successful
  if (!EEPROM.commit()) {
      errorMessage = "Failed to commit disabled state to EEPROM.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
  }
  
  // Respond to the client
  if (errorOccurred) {
     server.send(500, "text/plain", "Failed to disable the device.");
  } else {
     server.send(200, "text/plain", "Device disabled");
  }
}

void testCommand()
{
  bool errorOccurred = false;
  String errorMessage;
  
  Serial.println("Testing");
  
  // Set the testing state
  isTesting = true;
  
  // Set a longer time for testing
  storedTimeInSeconds = 10; // Ensure correct value, as the message indicates
  
  // Check if the state has been set correctly
  if (storedTimeInSeconds != 10) {
      errorMessage = "Failed to set the test time correctly.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
  }
  
  // Respond to the client
  if (errorOccurred) {
      server.send(500, "text/plain", "Failed to initiate device test.");
  } else {
      server.send(200, "text/plain", "Device test initiated. Time set to 10000 seconds");
  }
}

void deleteCommand()
{
  bool errorOccurred = false;
  String errorMessage;
  
  // Attempt to reset the device
  digitalWrite(RELAY_PIN, LOW); // Turn off LED or other indicator
  
  // Attempt to reset EEPROM and SPIFFS
  if (!resetEEPROMSPIFFS()) {
      errorMessage = "Failed to reset EEPROM and SPIFFS.";
      logMessage("Error: " + errorMessage);
      errorOccurred = true;
  }
  
  if (errorOccurred) {
      server.send(500, "text/html", "<html><body><h1>" + errorMessage + "</h1></body></html>");
  } else {
      server.send(200, "text/html", "<html><body><h1>Device Reset</h1><p>Device has been reset successfully.</p></body></html>");
      delay(2000);
      ESP.restart();
  }
}

// Callback for stream timeout
void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream Timeout, check network or database.");
  }
}

void setup() {
  pinMode(RELAY_PIN, INPUT_PULLDOWN_16);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  delay(1000);
  
  Serial.begin(115200);
  EEPROM.begin(512);
  if (!SPIFFS.begin()) {
    Serial.println(F("Failed to mount file system"));
    return;
  }

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  
  pinMode(AP_LED_PIN, OUTPUT);
  digitalWrite(AP_LED_PIN, LOW);
  pinMode(PROCESSING_LED_PIN, OUTPUT);  
  digitalWrite(PROCESSING_LED_PIN, LOW);
  pinMode(PUSH_BUTTON_PIN, INPUT_PULLUP);
  pinMode(AP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ERROR_LED_PIN, OUTPUT);
  digitalWrite(ERROR_LED_PIN, LOW);
  
  APLastButtonState = digitalRead(AP_BUTTON_PIN);
  digitalWrite(RELAY_PIN, LOW);
  
  loadConfig();
  connectToWiFi();

// Firebase config
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  // Initialize Firebase
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Firebase initialized.");

  firebasePath = String("/") + clientName + "/devices/" + deviceId;
  
  Serial.println(firebasePath);
  // Set up the stream to listen for changes at "/device/status"
  if (!Firebase.beginStream(firebaseData, firebasePath)) {
    Serial.println("Could not start stream.");
    Serial.println(firebaseData.errorReason());
  } else {
    Firebase.setStreamCallback(firebaseData, streamCallback, streamTimeoutCallback);
    Serial.println("Firebase stream started.");
  }

  
  startMDNS();
  setupServer();

  EEPROM.get(300, isLEDOn);
  digitalWrite(RELAY_PIN, isLEDOn ? LOW : HIGH);

  Serial.print("Light state on startup: ");
  Serial.println(isLEDOn ? "ON" : "OFF");

  // Configure OTA
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = F("sketch");
    } else { // U_SPIFFS
      type = F("filesystem");
    }
    Serial.println(F("Start updating ") + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println(F("\nEnd"));
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println(F("Auth Failed"));
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println(F("Begin Failed"));
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println(F("Connect Failed"));
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println(F("Receive Failed"));
    } else if (error == OTA_END_ERROR) {
      Serial.println(F("End Failed"));
    }
  });

  ArduinoOTA.begin();

  telnetServer.begin();
  telnetServer.setNoDelay(true);

  if (!isRegistered) {
    registerDevice();
  }

  loadState();

  storedTimeInSeconds = readTimeFromSPIFFS();
  loadStateAfterPowerInterrupt(readTimeFromSPIFFS());
//  unsigned long offDuration = calculateOffDuration();
//  storedTimeInSeconds -= offDuration;
  if (storedTimeInSeconds < 0) {
    storedTimeInSeconds = 0;
  }
  Serial.println("After: " + String(storedTimeInSeconds));
  EEPROM.put(308, readTimeFromSPIFFS()); // Save remaining time
  EEPROM.commit();

  EEPROM.get(308, storedTimeInSeconds);
    
  writeTimeToSPIFFS(storedTimeInSeconds);
  Serial.println(F("Adjusted storedTimeInSeconds: ") + String(storedTimeInSeconds));

 if (watchdogIntervalMinutes == 0) {
   watchdogIntervalMinutes = 180; // Default to 180 minutes if set to 0
   watchdogIntervalMinutes = 3 * 60;
 }

 checkFileContent("/logs.txt");

Serial.println("watchdogIntervalMinutes: " + String(watchdogIntervalMinutes));

 // Detach any existing ticker and attach the new one
// restartTicker.detach();
 restartTicker.attach(watchdogIntervalMinutes, []() {
   if (storedTimeInSeconds > 0 || isLEDOn || isFree || isOpenTime) {
       Serial.println("Watchdog maintenance skipped due to active timer or light on.");
   } else {
       saveState();
       Serial.println(F("Restarting"));
       ESP.restart();
   }
 });
  lastMillis = millis();
}

void loop() {
  server.handleClient();

  manageLEDTiming();  
  
  ArduinoOTA.handle(); // Handle OTA updates

  if (writeToFirebase) {
      writeToFirebase = false;  
      
      Serial.println(firebasePath + "/command");
      
      if (Firebase.setString(firebaseData, firebasePath + "/command", "acknowledged")) {
        Serial.println("Data written successfully.");
      } else {
        Serial.println("Error writing to Firebase: " + firebaseData.errorReason());
      }

      if (endingTime)
      {
          if (isOpenTime)
          {
              if (Firebase.setInt(firebaseData, firebasePath + "/span", storedTimeInSeconds)) {
                  Serial.println("Span value written successfully.");
              } else {
                  Serial.println("Error writing span to Firebase: " + firebaseData.errorReason());
              } 
          }
          else {
            if (Firebase.setInt(firebaseData, firebasePath + "/span", 0)) {
                Serial.println("Span value written successfully.");
            } else {
                Serial.println("Error writing span to Firebase: " + firebaseData.errorReason());
            } 
          }
      }
      else {
          if (Firebase.setInt(firebaseData, firebasePath + "/span", 0)) {
              Serial.println("Span value written successfully.");
          } else {
              Serial.println("Error writing span to Firebase: " + firebaseData.errorReason());
          } 
      }
    }

  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) telnetClient.stop();
      telnetClient = telnetServer.available();
    }
  }
  if (telnetClient && telnetClient.connected()) {
    while (telnetClient.available()) {
      Serial.write(telnetClient.read());
    }
    if (Serial.available()) {
      telnetClient.write(Serial.read());
    }
  }

  unsigned long currentTime = millis();
  if (currentTime - lastRetryTime > retryInterval) {
    //retryQueuedRequests();
    lastRetryTime = currentTime;
  }
    
  checkAPButtonPress(); // Check if the button is pressed
  if (storedTimeInSeconds < 1) {
    handleButtonPressCheck();
  }

  if (requireRestart){
    ESP.restart();
  }
//  checkWiFiConnection(); // Ensure WiFi is connected
}

void loadStateAfterPowerInterrupt(int remTime) {
    EEPROM.get(300, isLEDOn);
    EEPROM.get(308, storedTimeInSeconds);
    Serial.println("loadStateAfterPowerInterrupt: " + String(remTime));
    if (remTime > 0 || isLEDOn) {
        // Resume in a paused state if there was an interruption
        isPaused = true;
        EEPROM.put(210, isPaused);
        digitalWrite(RELAY_PIN, LOW);  // Turn off the light
        isLEDOn = false;
        EEPROM.put(300, isLEDOn);
        Serial.println("Resuming in paused state due to power interruption.");

        notifyServerOfPause(remTime);
    }
}

void notifyServerOfPause(int remTime) {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClient client;
        HTTPClient http;

        int deviceId;
        EEPROM.get(224, deviceId);  // Load the device ID from EEPROM

        String fullURL = String(hostURL) + pauseDeviceTimeURL;  // Assuming the API is "/api/pause"
        Serial.println("Sending pause request to server: " + fullURL);

        http.begin(client, fullURL);
        http.addHeader("Content-Type", "application/json");

        // Prepare the JSON payload
        String payload = "{\"device_id\": \"" + String(deviceId) + "\", \"remaining_time\": \"" + String(remTime) + "\"}";

        // Send the request
        int httpResponseCode = http.POST(payload);

        if (httpResponseCode >= 200 && httpResponseCode < 300) {
            String response = http.getString();
            Serial.println("Server response: " + response);
        } else {
            Serial.println("Error sending pause request: " + String(httpResponseCode));
            digitalWrite(ERROR_LED_PIN, HIGH);
            addToQueue("POST", fullURL, payload);
        }

        http.end();
    } else {
        Serial.println("WiFi not connected. Cannot send pause status.");
    }
}


void connectToWiFi() {
    local_IP.fromString(ipString);
    gateway.fromString(gatewayString);
    subnet.fromString(subnetString);

    if (strlen(ssid) == 0 || strlen(password) == 0) {
        Serial.println("No SSID and Password found. Starting AP mode.");
        startAPMode();
        return;  // Exit function to prevent further connection attempts
    }

    if (!local_IP.isSet() || !gateway.isSet() || !subnet.isSet()) {
        Serial.println("Invalid IP configuration. Using DHCP.");
        WiFi.begin(ssid, password);
    } else {
//        WiFi.config(local_IP, gateway, subnet);
        WiFi.begin(ssid, password);
    }

    Serial.print(F("Connecting to WiFi "));
    int attempt = 0;
    while (WiFi.status() != WL_CONNECTED && attempt < 20) {
        delay(1000);
        digitalWrite(LED_BUILTIN, LOW);
        digitalWrite(PROCESSING_LED_PIN, HIGH);
        digitalWrite(AP_LED_PIN, LOW);
        Serial.print(F("."));
        delay(100);
        digitalWrite(LED_BUILTIN, HIGH);
        digitalWrite(PROCESSING_LED_PIN, LOW);
        digitalWrite(AP_LED_PIN, LOW);
        delay(100);
        attempt++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("Connected to WiFi"));
        Serial.print(F("IP Address: "));
        Serial.println(WiFi.localIP());
        digitalWrite(LED_BUILTIN, LOW);
        digitalWrite(PROCESSING_LED_PIN, HIGH);
        digitalWrite(AP_LED_PIN, LOW);

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("WiFi Connected: " + WiFi.localIP().toString());
        } else {
            Serial.println("WiFi not connected.");
        }

        if (SPIFFS.exists("/logs.txt")) {
            if (SPIFFS.remove("/logs.txt")) {
                Serial.println("Log file cleared successfully.");
            }
        }
    } else {
        Serial.println(F("Failed to connect to WiFi. Starting AP mode."));
        startAPMode();
    }
}


void startAPMode() {
    digitalWrite(AP_LED_PIN, HIGH);
    WiFi.softAP(APSSID, APPass);
    Serial.print(F("AP IP address: "));
    Serial.println(WiFi.softAPIP());
    digitalWrite(LED_BUILTIN, HIGH);
    digitalWrite(PROCESSING_LED_PIN, LOW);
    logMessage("Failed to connect to WiFi with SSID: " + String(ssid) + ". Starting AP mode.");

    // Start the web server
    setupServer();

    // Ensure the device does not attempt to restart the WiFi process
    while (true) {
        server.handleClient(); // Handle HTTP server requests
        delay(100);
    }
}

void startMDNS() {
  if (MDNS.begin(mDNSHostname)) {
    Serial.println("MDNS responder started");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error setting up MDNS responder!");
  }
}

void loadConfig() {
    // Ensure buffers are clear before reading
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
    memset(serverAppDomain, 0, sizeof(serverAppDomain));
    memset(serverAppPort, 0, sizeof(serverAppPort));
    memset(deviceName, 0, sizeof(deviceName));
    memset(ipString, 0, sizeof(ipString));
    memset(gatewayString, 0, sizeof(gatewayString));
    memset(subnetString, 0, sizeof(subnetString));
    memset(clientName, 0, sizeof(clientName));

    // Read data from EEPROM
    EEPROM.get(0, ssid);
    EEPROM.get(32, password);
    EEPROM.get(64, serverAppDomain);
    EEPROM.get(96, serverAppPort);
    EEPROM.get(128, deviceName);
    EEPROM.get(160, isRegistered);
    EEPROM.get(161, ipString);
    EEPROM.get(177, gatewayString);
    EEPROM.get(193, subnetString);
    EEPROM.get(209, isDisabled);
    EEPROM.get(210, isPaused);
    EEPROM.get(211, isFree);
    EEPROM.get(212, isOpenTime);
    EEPROM.get(224, deviceId);
    EEPROM.get(312, watchdogIntervalMinutes);
    EEPROM.get(320, clientName);
    EEPROM.get(224, deviceId);

    // Null-terminate strings to ensure safe string operations
    ssid[sizeof(ssid) - 1] = '\0';
    password[sizeof(password) - 1] = '\0';
    serverAppDomain[sizeof(serverAppDomain) - 1] = '\0';
    serverAppPort[sizeof(serverAppPort) - 1] = '\0';
    deviceName[sizeof(deviceName) - 1] = '\0';
    ipString[sizeof(ipString) - 1] = '\0';
    gatewayString[sizeof(gatewayString) - 1] = '\0';
    subnetString[sizeof(subnetString) - 1] = '\0';
    clientName[sizeof(clientName) - 1] = '\0';

    Serial.print("isRegistered");
    Serial.println(String(isRegistered));

    snprintf(hostURL, sizeof(hostURL), "http://%s:%s", serverAppDomain, serverAppPort);
}


void saveConfig() {
    EEPROM.put(0, ssid);
    EEPROM.put(32, password);
    EEPROM.put(64, serverAppDomain);
    EEPROM.put(96, serverAppPort);
    EEPROM.put(128, deviceName);
    EEPROM.put(160, isRegistered);
    EEPROM.put(161, ipString);
    EEPROM.put(177, gatewayString);
    EEPROM.put(193, subnetString);
    EEPROM.put(209, isDisabled);
    EEPROM.put(210, isPaused);
    EEPROM.put(211, isFree);
    EEPROM.put(212, isOpenTime);
    EEPROM.put(224, deviceId);
    EEPROM.put(312, watchdogIntervalMinutes);
    EEPROM.put(320, clientName);
    
    
    if (EEPROM.commit()) {
        Serial.println("Configuration saved.");
    } else {
        Serial.println("Error saving configuration to EEPROM.");
    }
}


void setupServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", generateHTML());
  });

  server.on("/save", HTTP_POST, []() {
    bool errorOccurred = false;
    String errorMessage;

    if (server.hasArg("ssid") && server.hasArg("password") && server.hasArg("hostname") && server.hasArg("device_name")) {
        
        // Validate and copy each argument
        if (server.arg("ssid").length() >= sizeof(ssid) || server.arg("ssid").length() == 0) {
            errorMessage = "Invalid SSID length";
            errorOccurred = true;
        } else {
            strncpy(ssid, server.arg("ssid").c_str(), sizeof(ssid));
        }

        if (server.arg("password").length() >= sizeof(password) || server.arg("password").length() == 0) {
            errorMessage = "Invalid Password length";
            errorOccurred = true;
        } else {
            strncpy(password, server.arg("password").c_str(), sizeof(password));
        }

        if (server.arg("hostname").length() >= sizeof(serverAppDomain) || server.arg("hostname").length() == 0) {
            errorMessage = "Invalid Server Hostname length";
            errorOccurred = true;
        } else {
            strncpy(serverAppDomain, server.arg("hostname").c_str(), sizeof(serverAppDomain));
        }

        if (server.arg("clientname").length() >= sizeof(clientName) || server.arg("clientname").length() == 0) {
            errorMessage = "Invalid Server Client Name length";
            errorOccurred = true;
        } else {
            strncpy(clientName, server.arg("clientname").c_str(), sizeof(clientName));
        }

        if (server.arg("port").length() >= sizeof(serverAppPort) || server.arg("port").length() == 0) {
            errorMessage = "Invalid Server Port length";
            errorOccurred = true;
        } else {
            strncpy(serverAppPort, server.arg("port").c_str(), sizeof(serverAppPort));
        }

        if (server.arg("device_name").length() >= sizeof(deviceName) || server.arg("device_name").length() == 0) {
            errorMessage = "Invalid Device Name length";
            errorOccurred = true;
        } else {
            strncpy(deviceName, server.arg("device_name").c_str(), sizeof(deviceName));
        }

        if (errorOccurred) {
            logMessage("Error: " + errorMessage);
            server.send(400, "text/html", "<html><body><h1>" + errorMessage + "</h1></body></html>");
        } else {
            // No errors, proceed to save configuration
            isRegistered = false; // Reset registration status on config save
            saveConfig();
            server.send(200, "text/html", "<html><body><h1>Configuration Saved!</h1><p>Device will reboot now.</p></body></html>");
            delay(2000);
            ESP.restart();
        }

    } else {
        errorMessage = "Missing required parameters";
        logMessage("Error: " + errorMessage);
        server.send(400, "text/html", "<html><body><h1>" + errorMessage + "</h1></body></html>");
    }
});


  server.on("/api/reset", HTTP_DELETE, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      // Attempt to reset the device
      digitalWrite(RELAY_PIN, LOW); // Turn off LED or other indicator
  
      // Attempt to reset EEPROM and SPIFFS
      if (!resetEEPROMSPIFFS()) {
          errorMessage = "Failed to reset EEPROM and SPIFFS.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }
  
      if (errorOccurred) {
          server.send(500, "text/html", "<html><body><h1>" + errorMessage + "</h1></body></html>");
      } else {
          server.send(200, "text/html", "<html><body><h1>Device Reset</h1><p>Device has been reset successfully.</p></body></html>");
          delay(2000);
          ESP.restart();
      }
  });


server.on("/api/span", HTTP_GET, []() {
    bool errorOccurred = false;
    String errorMessage;
    int temp_storedTimeInSeconds = 0;

    if (server.hasArg("time")) {
        String timeArg = server.arg("time");
        int timeInSeconds = timeArg.toInt();

        // Check if conversion was successful
        if (timeInSeconds == 0 && timeArg != "0") {
            errorMessage = "Invalid time value: " + timeArg;
            logMessage("Error: " + errorMessage);
            errorOccurred = true;
        } else if (timeInSeconds < 0) {
            errorMessage = "Negative time value not allowed: " + timeArg;
            logMessage("Error: " + errorMessage);
            errorOccurred = true;
        } else {
            temp_storedTimeInSeconds = readTimeFromSPIFFS();
            if (temp_storedTimeInSeconds < 0) {
                errorMessage = "Failed to read time from SPIFFS.";
                logMessage("Error: " + errorMessage);
                errorOccurred = true;
            } else {
                Serial.println("timeInSeconds:" + String(timeInSeconds));
                Serial.println("storedTimeInSeconds:" + String(temp_storedTimeInSeconds));
                temp_storedTimeInSeconds += timeInSeconds;
                Serial.println("extended-storedTimeInSeconds:" + String(temp_storedTimeInSeconds));
                
                if (!writeTimeToSPIFFS(temp_storedTimeInSeconds)) {
                    errorMessage = "Failed to write time to SPIFFS.";
                    logMessage("Error: " + errorMessage);
                    errorOccurred = true;
                } else {
                    storedTimeInSeconds = temp_storedTimeInSeconds;
                }
            }
        }

        if (errorOccurred) {
            server.send(400, "text/plain", errorMessage);
        } else {
            server.send(200, "text/plain", "Time set to " + String(temp_storedTimeInSeconds) + " seconds");
        }
    } else {
        errorMessage = "Missing time parameter";
        logMessage("Error: " + errorMessage);
        server.send(400, "text/plain", errorMessage);
    }
});


server.on("/api/stop", HTTP_GET, []() {
    bool errorOccurred = false;
    String errorMessage;

    Serial.println("Forced stop");

    // Attempt to turn off LED and update EEPROM
    digitalWrite(RELAY_PIN, LOW);
    isLEDOn = false;
    EEPROM.put(300, isLEDOn);
    if (!EEPROM.commit()) {
        errorMessage = "Failed to commit LED state to EEPROM.";
        logMessage("Error: " + errorMessage);
        errorOccurred = true;
    }

    // Attempt to update EEPROM
    isPaused = false;
    EEPROM.put(210, isPaused);
    isOpenTime = false;
    EEPROM.put(212, isOpenTime);
    
    if (!EEPROM.commit()) {
        errorMessage = "Failed to commit paused state to EEPROM.";
        logMessage("Error: " + errorMessage);
        errorOccurred = true;
    }

    // Respond to the client with storedTimeInSeconds
    if (errorOccurred) {
        server.send(500, "application/json", "{\"success\": false, \"error\": \"" + errorMessage + "\"}");
    } else {
        String jsonResponse = "{\"success\": true, \"storedTimeInSeconds\": " + String(storedTimeInSeconds) + "}";
        // Attempt to write 0 to SPIFFS
        if (!writeTimeToSPIFFS(0)) {
            errorMessage = "Failed to write time to SPIFFS.";
            logMessage("Error: " + errorMessage);
            errorOccurred = true;
        } else {
            storedTimeInSeconds = 0;
        }
        server.send(200, "application/json", jsonResponse);
        server.client().flush(); 
        delay(1000); 
        ESP.restart();
    }
});



  server.on("/api/test", HTTP_GET, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      Serial.println("Testing");
  
      // Set the testing state
      isTesting = true;
  
      // Set a longer time for testing
      storedTimeInSeconds = 10; // Ensure correct value, as the message indicates
  
      // Check if the state has been set correctly
      if (storedTimeInSeconds != 10) {
          errorMessage = "Failed to set the test time correctly.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          server.send(500, "text/plain", "Failed to initiate device test.");
      } else {
          server.send(200, "text/plain", "Device test initiated. Time set to 10000 seconds");
      }
  });


  server.on("/api/disable", HTTP_GET, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      // Attempt to disable the device
      isDisabled = true;
      EEPROM.put(209, isDisabled);
  
      // Check if EEPROM commit is successful
      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit disabled state to EEPROM.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          server.send(500, "text/plain", "Failed to disable the device.");
      } else {
          server.send(200, "text/plain", "Device disabled");
      }
  });


  server.on("/api/enable", HTTP_GET, []() {
    bool errorOccurred = false;
    String errorMessage;

    // Attempt to enable the device
    isDisabled = false;
    EEPROM.put(209, isDisabled);

    // Check if EEPROM commit is successful
    if (!EEPROM.commit()) {
        errorMessage = "Failed to commit enabled state to EEPROM.";
        logMessage("Error: " + errorMessage);
        errorOccurred = true;
    }

    // Respond to the client
    if (errorOccurred) {
        server.send(500, "text/plain", "Failed to enable the device.");
    } else {
        server.send(200, "text/plain", "Device enabled");
    }
});

server.on("/api/pause", HTTP_GET, []() {
    bool errorOccurred = false;
    String errorMessage;

    // Attempt to pause the device
    isPaused = true;
    EEPROM.put(210, isPaused);

    // Check if EEPROM commit for isPaused is successful
    if (!EEPROM.commit()) {
        errorMessage = "Failed to commit pause state to EEPROM.";
        logMessage("Error: " + errorMessage);
        errorOccurred = true;
    }

    // Attempt to turn off the LED
    digitalWrite(RELAY_PIN, LOW);
    isLEDOn = false;
    EEPROM.put(300, isLEDOn);

    // Check if EEPROM commit for isLEDOn is successful
    if (!EEPROM.commit()) {
        errorMessage = "Failed to commit LED state to EEPROM.";
        logMessage("Error: " + errorMessage);
        errorOccurred = true;
    }

    // Respond to the client
    if (errorOccurred) {
        server.send(500, "text/plain", "Failed to pause device time.");
    } else {
        server.send(200, "text/plain", "Device paused time");
    }
});


  server.on("/api/resume", HTTP_GET, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      // Attempt to resume the device
      isPaused = false;
      EEPROM.put(210, isPaused);
  
      // Check if EEPROM commit for isPaused is successful
      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit resume state to EEPROM.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }
  
      // Attempt to turn on the LED
      digitalWrite(RELAY_PIN, HIGH);
      isLEDOn = true;
      EEPROM.put(300, isLEDOn);
  
      // Check if EEPROM commit for isLEDOn is successful
      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit LED state to EEPROM.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          server.send(500, "text/plain", "Failed to resume device time.");
      } else {
          server.send(200, "text/plain", "Device resumed time");
      }
  });


  server.on("/api/startfree", HTTP_GET, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      Serial.println("Free light");
  
      // Set the free state
      isFree = true;
      EEPROM.put(211, isFree);

      // Turn on the light
      digitalWrite(RELAY_PIN, HIGH);
  
      // Check if EEPROM commit is successful
      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit free state to EEPROM.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }

      // Respond to the client
      if (errorOccurred) {
          server.send(500, "text/plain", "Failed to start free light.");
      } else {
          server.send(200, "text/plain", "Device free light");
      }
  });


   server.on("/api/stopfree", HTTP_GET, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      Serial.println("Stop Free light");
  
      // Attempt to stop free mode
      isFree = false;
      EEPROM.put(211, isFree);
  
      // Check if EEPROM commit is successful
      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit free state to EEPROM.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }
  
      // Turn off the light
      digitalWrite(RELAY_PIN, LOW);
  
      // Respond to the client
      if (errorOccurred) {
          server.send(500, "text/plain", "Failed to stop free light.");
      } else {
          server.send(200, "text/plain", "Free light stopped");
      }
  });


  server.on("/api/updateDeviceName", HTTP_POST, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      if (server.hasArg("plain")) {
          String newDeviceName = server.arg("plain"); // Get the new device name from the request body
          if (newDeviceName.length() > 0 && newDeviceName.length() < sizeof(deviceName)) {
              newDeviceName.toCharArray(deviceName, sizeof(deviceName)); // Copy new device name into deviceName variable
              EEPROM.put(128, deviceName); // Save the updated device name to EEPROM
  
              // Check if EEPROM commit is successful
              if (!EEPROM.commit()) {
                  errorMessage = "Failed to commit device name to EEPROM.";
                  logMessage("Error: " + errorMessage);
                  errorOccurred = true;
              } else {
                  Serial.println("Device name updated to: " + String(deviceName));
              }
          } else {
              errorMessage = "Invalid device name length.";
              logMessage("Error: " + errorMessage);
              errorOccurred = true;
          }
      } else {
          errorMessage = "No device name provided.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          server.send(400, "text/plain", errorMessage);
      } else {
          server.send(200, "text/plain", "Device name updated successfully");
      }
  });

  server.on("/api/logs", HTTP_GET, []() {
    if (!SPIFFS.exists("/logs.txt")) {
        server.send(404, "text/plain", "Log file not found.");
        return;
    }

    File logFile = SPIFFS.open("/logs.txt", "r");
    if (!logFile) {
        server.send(500, "text/plain", "Failed to open log file.");
        return;
    }

    String logContent;
    while (logFile.available()) {
        logContent += char(logFile.read());
    }
    logFile.close();

    server.send(200, "text/plain", logContent);
  });

server.on("/api/clearlogs", HTTP_DELETE, []() {
    if (SPIFFS.exists("/logs.txt")) {
        if (SPIFFS.remove("/logs.txt")) {
            server.send(200, "text/plain", "Log file cleared successfully.");
            Serial.println("Log file cleared successfully.");
        } else {
            server.send(500, "text/plain", "Failed to clear log file.");
            Serial.println("Failed to clear log file.");
        }
    } else {
        server.send(404, "text/plain", "Log file not found.");
        Serial.println("Log file not found.");
    }
});


server.on("/api/setWatchdogInterval", HTTP_POST, []() {
    if (server.hasArg("plain")) {
        String intervalStr = server.arg("plain");
        int interval = intervalStr.toInt();
        Serial.println("intervalStr: " + String(interval));
        if (interval > 0) {
            watchdogIntervalMinutes = interval * 60;
            Serial.println("watchdogIntervalMinutes: " + String(watchdogIntervalMinutes));
            saveConfig();
            restartTicker.detach(); // Stop the previous ticker
            restartTicker.attach(watchdogIntervalMinutes, []() {
                if (storedTimeInSeconds > 0 || isLEDOn || isFree || isOpenTime) {
                    Serial.println("Watchdog maintenance skipped due to active timer or light on.");
                } else {
                    saveState();
                    Serial.println(F("Restarting"));
                    ESP.restart();
                }
            });
            server.send(200, "text/plain", "Watchdog interval set to " + String(watchdogIntervalMinutes) + " minutes.");
        } else {
            server.send(400, "text/plain", "Invalid interval. Must be greater than 0.");
        }
    } else {
        server.send(400, "text/plain", "No interval provided.");
    }
});

server.on("/api/startopentime", HTTP_GET, []() {
      bool errorOccurred = false;
      String errorMessage;
  
      Serial.println("Open time");
  
      // Set the open time state
      isOpenTime = true;
      EEPROM.put(212, isOpenTime);

      // Turn on the light
      digitalWrite(RELAY_PIN, HIGH);
  
      // Check if EEPROM commit is successful
      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit free state to EEPROM.";
          logMessage("Error: " + errorMessage);
          errorOccurred = true;
      }

      // Respond to the client
      if (errorOccurred) {
          server.send(500, "text/plain", "Failed to start open time.");
      } else {
          server.send(200, "text/plain", "Device open time");
      }
  });

  server.begin();
  Serial.println("HTTP server started");
}

void manageLEDTiming() {
  if (isFree || isOpenTime)
  {
    if (isOpenTime)
    {
      delay(1000);
      storedTimeInSeconds++;
      Serial.println("Remaining time: " + String(storedTimeInSeconds));
      writeTimeToSPIFFS(storedTimeInSeconds);
    }
    else {
      digitalWrite(RELAY_PIN, HIGH);
    }
  }
  else
  {
   if (!isDisabled && !isPaused)
    {
       if (storedTimeInSeconds > 0) {
        if (isTesting)
        {
          digitalWrite(RELAY_PIN, HIGH);
          delay(1000);
          storedTimeInSeconds--;
          digitalWrite(RELAY_PIN, LOW);
          delay(1000);
          storedTimeInSeconds--;
        }
        else
        {
          if (!isLEDOn) {
            digitalWrite(RELAY_PIN, HIGH);
            isLEDOn = true;
            saveState(); // Save state whenever the LED state changes
          }
          
          delay(1000);
          storedTimeInSeconds--;
          Serial.println("Remaining time: " + String(storedTimeInSeconds));
          writeTimeToSPIFFS(storedTimeInSeconds);
        }
      } else {
        if (isLEDOn || isTesting) 
        {
          digitalWrite(RELAY_PIN, LOW);
          isLEDOn = false;
          saveState(); // Save state whenever the LED state changes
          //notifyServerOfTimeEnd();

          if (isTesting) 
          {
            isTesting = false;
          }
        }
      } 
    } 
  }
}


int readTimeFromSPIFFS() {
  File file = SPIFFS.open(timeFilePath, "r");
  if (!file) {
    Serial.println("Failed to open time file for reading");
    return 0;
  }

  String timeString = file.readString();
  file.close();
  Serial.println("Read time from SPIFFS: " + timeString);
  return timeString.toInt();
}

bool writeTimeToSPIFFS(int time) {
    File file = SPIFFS.open(timeFilePath, "w");
    if (!file) {
        Serial.println("Failed to open time file for writing");
        return false;
    }

    file.println(time);
    file.close();
    Serial.println("Written time to SPIFFS: " + String(time));
    return true;
}


bool resetEEPROMSPIFFS() {
  bool success = true;
  
  // Reset EEPROM
  for (int i = 0; i < 512; ++i) {
    EEPROM.write(i, 0);
  }
  if (!EEPROM.commit()) {
    logMessage("Error: Failed to commit EEPROM reset.");
    success = false;
  }

  // Format SPIFFS
  if (!SPIFFS.format()) {
    logMessage("Error: Failed to format SPIFFS.");
    success = false;
  }

  // Close SPIFFS
  SPIFFS.end();

  if (success) {
    Serial.println("EEPROM and SPIFFS reset complete");
  }

  return success;
}


void createSPIFFSFile() {
  //TIME LOGGING
  File file = SPIFFS.open(timeFilePath, "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }

  int initialTime = 0; // Set your initial time value here
  file.println(initialTime);
  file.close();

  Serial.println(F("Time file created with initial value: ") + String(initialTime));
}

void logMessage(const String& message) {
  //ERROR LOGGING
  File logFile = SPIFFS.open("/logs.txt", "a"); // Open log file in append mode
  if (!logFile) {
    Serial.println("Failed to open log file for writing");
    return;
  }
  logFile.println(message); // Write the message to the log file
  logFile.close();
  Serial.println("Logged message: " + message);
}

String generateHTML() {
    String page = "<!DOCTYPE html><html lang=\"en\"><head>";
    page += "<meta charset=\"UTF-8\">";
    page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";
    page += "<title>Device Configuration</title>";
    page += "<style>";
    page += "body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f4f4f9; text-align: center; }";
    page += "h1 { color: #333; }";
    page += "form { background-color: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1); text-align: left; }";
    page += "input[type='text'], button { box-sizing: border-box; width: 100%; padding: 8px; margin: 10px 0; border: 1px solid #ccc; border-radius: 4px; }";
    page += "input[type='submit'], button { background-color: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; }";
    page += "input[type='submit']:hover, button:hover { background-color: #45a049; }";
    page += ".version { font-size: 0.9em; color: #777; margin-bottom: 20px; }";
    page += ".divider { border: 1px solid lightgray; margin: 15px 0px; }";
    page += "#logs { max-height: 300px; overflow-y: auto; margin-top: 20px; background-color: #fff; padding: 10px; border-radius: 8px; border: 1px solid #ccc; }";
    page += "#clearLogsBtn { display: none; margin-top: 10px; }"; // Initially hidden
    page += "</style>";
    page += "</head><body>";
    page += "<h1>Device Configuration</h1>";
    page += "<div class='version'>" + manufacturerDeviceName + "  <span id='versionNumber'>" + versionNumber + "</span></div>";
    page += "<form action='/save' method='POST'>";
    page += "<b>SSID:</b> <input type='text' name='ssid' placeholder='WiFi SSID' value='" + String(ssid) + "'><br>";
    page += "<b>Password:</b> <input type='text' name='password' placeholder='WiFi password' value='" + String(password) + "'><br>";
    page += "<b>Client Name:</b> <input type='text' name='clientname' placeholder='Client Name' value='" + String(clientName) + "'><br>";
    page += "<b>Server Domain:</b> <input type='text' name='hostname' placeholder='Server IP' value='" + String(serverAppDomain) + "'><br>";
    page += "<b>Server Port:</b> <input type='text' name='port' placeholder='Server port' value='" + String(serverAppPort) + "'><br>";
    page += "<b>Device name:</b> <input type='text' name='device_name' placeholder='Device name' value='" + String(deviceName) + "'><br>";
    page += "<input type='submit' value='Save'>";
    page += "</form>";
    page += "<button onclick='fetchLogs()'>Show Logs</button>";
    page += "<div id='logs'></div>"; // Container to display logs
    page += "<button id='clearLogsBtn' onclick='clearLogs()'>Clear Error Log</button>"; // Button to clear logs
    page += "<script>";
    page += "function fetchLogs() {";
    page += "  console.log('Fetching logs...');"; // Debugging log
    page += "  fetch('/api/logs')"; // Fetch logs from the endpoint
    page += "    .then(response => {";
    page += "      if (!response.ok) { throw new Error('Network response was not ok'); }"; // Check for network error
    page += "      return response.text();"; // Convert the response to text
    page += "    })";
    page += "    .then(data => {";
    page += "      console.log('Logs fetched:', data);"; // Debugging log for fetched data
    page += "      document.getElementById('logs').innerHTML = '<pre>' + data + '</pre>';";
    page += "      document.getElementById('clearLogsBtn').style.display = 'block';"; // Show clear logs button
    page += "    })";
    page += "    .catch(error => console.error('Error fetching logs:', error));";
    page += "}";
    page += "function clearLogs() {";
    page += "  console.log('Clearing logs...');"; // Debugging log
    page += "  fetch('/api/clearlogs', { method: 'DELETE' })"; // Send DELETE request to clear logs
    page += "    .then(response => {";
    page += "      if (response.ok) {";
    page += "        document.getElementById('logs').innerHTML = '<pre>Logs cleared.</pre>';";
    page += "        document.getElementById('clearLogsBtn').style.display = 'none';"; // Hide the button again
    page += "        console.log('Logs cleared successfully');"; // Debugging log
    page += "      } else {";
    page += "        console.error('Failed to clear logs');"; // Debugging log
    page += "      }";
    page += "    })";
    page += "    .catch(error => console.error('Error clearing logs:', error));";
    page += "}";
    page += "</script>";
    page += "</body></html>";
    return page;
}



void registerDevice() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;

    int checkId = 0;
    EEPROM.get(224, checkId);

    String payload = F("{");

    if (checkId > 0)
    {
      Serial.println(String(hostURL) + updateDeviceURL);
      http.begin(client, String(hostURL) + updateDeviceURL); 

      payload += F("\"DeviceID\":\"") + String(checkId) + F("\",");
    }
    else 
    {
      Serial.println(String(hostURL) + registerDeviceURL);
      http.begin(client, String(hostURL) + registerDeviceURL); 
    }

    
    http.addHeader(F("Content-Type"), F("application/json"));

    payload += F("\"DeviceName\":\"") + String(deviceName) + F("\",");
    payload += F("\"ClientName\":\"") + String(clientName) + F("\",");
    payload += F("\"DeviceStatusID\":1"); // Set initial status as 1/Pending Configuration
    payload += F("}");

    Serial.println(payload);
    
    int httpResponseCode = http.POST(payload);
    Serial.println(httpResponseCode);
    if (httpResponseCode >= 200 && httpResponseCode < 300) {
      String response = http.getString();
      Serial.println(F("Device registered successfully: ") + response);

      // Parse the JSON response to extract device_id
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, response);

      if (!error) {
        int tmp_deviceId = doc["device_id"];
        Serial.println("Response Device ID: " + String(tmp_deviceId));

        // Save the device ID to EEPROM
        EEPROM.put(224, tmp_deviceId); // Save deviceId at address 224
        EEPROM.commit();

        EEPROM.get(224, deviceId);
        Serial.println("Stored Device ID: " + String(deviceId));

        firebasePath = String("/") + clientName + "/devices/" + deviceId;
        Serial.println(firebasePath);

        isRegistered = true; 
        EEPROM.put(160, isRegistered);
        saveConfig(); 

        ESP.restart();
        
      } else {
        Serial.println(F("Failed to parse JSON response"));
        digitalWrite(ERROR_LED_PIN, HIGH);
        addToQueue("POST", String(hostURL) + registerDeviceURL, payload);
      }
    } else {
      logMessage("ERROR: Unable to register device");
      digitalWrite(ERROR_LED_PIN, HIGH);
      addToQueue("POST", String(hostURL) + updateDeviceURL, payload); // Queue the request if failed
      Serial.print(F("Error on sending POST: "));
      Serial.println(httpResponseCode);
    }

    http.end();
  } else {
    Serial.println(F("Error in WiFi connection"));
  }
}

void notifyServerOfTimeEnd() {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClient client;
        HTTPClient http;

        int id;
        EEPROM.get(224, id); // Load deviceId from address 224
        Serial.println(F("deviceId:") + String(id));
        
        String fullURL = String(hostURL) + stopDeviceTimeURL;
        Serial.println("Full URL: " + fullURL);
        
        http.begin(client, fullURL); 
        http.addHeader(F("Content-Type"), F("application/json"));

        String payload = F("{");
        payload += F("\"device_id\":\"") + String(id) + F("\",");
        if (isTesting)
        {
          isTesting = false;
          payload += F("\"from_testing\":1");
        }
        else
        {
          payload += F("\"from_testing\":0");
        }
        payload += F("}");
        
        int httpResponseCode = http.POST(payload);
        if (httpResponseCode >= 200 && httpResponseCode < 300) {
            String response = http.getString();
            Serial.println("Server notified successfully: " + response);
            delay(1000);
            ESP.restart();
        } else {
            logMessage("Failed to notify server, response code: " + String(httpResponseCode));
            digitalWrite(ERROR_LED_PIN, HIGH);
            addToQueue("POST", fullURL, payload); // Queue the request if failed
        }

        http.end();
    } else {
        Serial.println("Error in WiFi connection");
    }
}

void saveState() {
  EEPROM.put(300, isLEDOn); // Save LED state
  EEPROM.put(304, millis()); // Save current time in millis
  EEPROM.put(308, storedTimeInSeconds); // Save remaining time
  EEPROM.put(312, watchdogIntervalMinutes);
  EEPROM.commit();
  Serial.println("Saved state: millis = " + String(millis()) + ", storedTimeInSeconds = " + String(storedTimeInSeconds));
}

void loadState() {
  EEPROM.get(300, isLEDOn);
  EEPROM.get(304, lastMillis); // Get the last recorded time in millis
  EEPROM.get(308, storedTimeInSeconds); // Load remaining time
  EEPROM.get(312, watchdogIntervalMinutes);
  Serial.println("Loaded state: millis = " + String(lastMillis) + ", storedTimeInSeconds = " + String(storedTimeInSeconds));

  if (isLEDOn) {
    digitalWrite(RELAY_PIN, HIGH); // Restore LED state
  } else {
    digitalWrite(RELAY_PIN, LOW);
  }
}

unsigned long calculateOffDuration() {
  unsigned long currentMillis = millis();
  if (lastMillis > currentMillis) {
    // Handle the case where millis() overflowed
    lastMillis = currentMillis; 
    return 0; 
  }
  unsigned long offDurationMillis = currentMillis - lastMillis; // Calculate duration in milliseconds
  Serial.println(F("Calculated offDurationMillis: ") + String(offDurationMillis));
  return offDurationMillis / 1000; // Return the duration in seconds
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi connection lost. Reconnecting..."));
    connectToWiFi();
  }
}

// Function to check if the button is pressed
void checkAPButtonPress() {
  bool APCurrentButtonState = digitalRead(AP_BUTTON_PIN);

  if (APCurrentButtonState != APLastButtonState) {
    APLastDebounceTime = millis();  // Reset debounce timer
  }

  if ((millis() - APLastDebounceTime) > APDebounceDelay) {
    // Change detected, check for button press
    if (APCurrentButtonState == LOW && !APButtonPressed) {
      Serial.println(F("Button pressed. Switching to AP mode."));
      switchToAPModeOnDemand();
      APButtonPressed = true;  // Prevent re-triggering
    } else if (APCurrentButtonState == HIGH && APButtonPressed) {
      // Button released
      APButtonPressed = false;  // Reset button pressed state
    }
  }
  
  APLastButtonState = APCurrentButtonState;
}

// Function to switch to AP mode
void switchToAPModeOnDemand() {
  digitalWrite(AP_LED_PIN, HIGH);
  // Clear stored WiFi credentials
  memset(ssid, 0, sizeof(ssid));
  memset(password, 0, sizeof(password));
  saveConfig();

  // Restart device to start in AP mode
  delay(1000);
  ESP.restart();
}

void handleButtonPressCheck() {
    bool currentButtonState = digitalRead(PUSH_BUTTON_PIN);

    if (storedTimeInSeconds > 0) {
        // Timer is running, ignore button presses
        Serial.println("Button pressed but light state unchanged due to active timer.");
    } else { 
        // Control the light based on the button state
        if (currentButtonState == LOW) {
            // Button is pressed, turn on the light
            digitalWrite(RELAY_PIN, HIGH);
        } else {
            // Button is released, turn off the light
           if (!isFree && !isOpenTime) 
           {
              digitalWrite(RELAY_PIN, LOW);
           }
        }
    }
}
