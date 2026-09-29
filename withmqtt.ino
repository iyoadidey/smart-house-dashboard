#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Servo.h>

// Wi-Fi
const char* ssid = "Starlink";
const char* wifiPassword = "Skybulok321";

// HiveMQ Cloud broker
const char* mqttServer = "6ccecdc3f8c0410bb616be6f868c657d.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUsername = "group3";
const char* mqttPassword = "group3**";

// ThingSpeak
const char* thingspeakApiKey = "ENNMZFWX99ZS97HS";
const unsigned long thingSpeakInterval = 15000;
unsigned long lastThingSpeakPublish = 0;

// Pin definitions
#define DHT_PIN D2
#define DHT_TYPE DHT11
#define LED_PIN_1 D0
#define LDR_PIN_1 A0
#define LED_PIN_2 D1
#define LDR_PIN_2 D8
#define PIR_PIN D5
#define BUZZER_PIN D7
#define SERVO_PIN_1 D4

// MQTT topics
const char* topicTemperature = "SmartHouse/sensors/temperature";
const char* topicHumidity = "SmartHouse/sensors/humidity";
const char* topicLight1 = "SmartHouse/sensors/light1";
const char* topicLight2 = "SmartHouse/sensors/light2";
const char* topicMotion = "SmartHouse/sensors/motion";

const char* topicLed1Status = "SmartHouse/devices/led1";
const char* topicLed2Status = "SmartHouse/devices/led2";
const char* topicDoorStatus = "SmartHouse/devices/door";
const char* topicModeStatus = "SmartHouse/devices/mode";

const char* topicModeCommand = "SmartHouse/commands/mode";
const char* topicLed1Command = "SmartHouse/commands/led1";
const char* topicLed2Command = "SmartHouse/commands/led2";
const char* topicDoorCommand = "SmartHouse/commands/door";
const char* topicBuzzerCommand = "SmartHouse/commands/buzzer";

DHT dht(DHT_PIN, DHT_TYPE);
Servo doorServo;
BearSSL::WiFiClientSecure mqttClientTransport;
BearSSL::WiFiClientSecure thingSpeakClient;
PubSubClient mqttClient(mqttClientTransport);

const int lightThreshold = 500;
const int photo2DarkState = LOW;
const unsigned long publishInterval = 5000;
const unsigned long manualOverrideDuration = 10000;
const unsigned long doorOpenDuration = 3000;
const unsigned long photo2SampleInterval = 100;
const unsigned long mqttReconnectInterval = 5000;
const unsigned long dhtSampleInterval = 2000;
const unsigned long thingSpeakMotionQuietTime = 5000;

String controlMode = "AUTO";
int previousMotionState = LOW;
int lastPublishedMotionState = LOW;
bool doorOpen = false;
float lastHumidity = NAN;
float lastTemperature = NAN;
unsigned long led1OverrideUntil = 0;
unsigned long led2OverrideUntil = 0;
unsigned long doorOverrideUntil = 0;
unsigned long doorCloseAt = 0;
unsigned long lastPhoto2Sample = 0;
int photo2StableState = HIGH;
unsigned long lastPublish = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastDhtSample = 0;
unsigned long lastMotionChange = 0;

void playTestBuzzer() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(220);
  digitalWrite(BUZZER_PIN, LOW);
  delay(120);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(420);
  digitalWrite(BUZZER_PIN, LOW);
}

void connectWiFi() {
  Serial.println();
  Serial.print("[WIFI] Connecting to: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, wifiPassword);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("[WIFI] Connected");
  Serial.print("[WIFI] IP Address: ");
  Serial.println(WiFi.localIP());
}

void sendToThingSpeak(float temperature, float humidity, int lightValue, int motion, int door) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ThingSpeak] WiFi not connected");
    return;
  }

  HTTPClient http;
  String url = "https://api.thingspeak.com/update?api_key=" + String(thingspeakApiKey);
  url += "&field1=" + String(temperature, 2);
  url += "&field2=" + String(humidity, 2);
  url += "&field3=" + String(lightValue);
  url += "&field4=" + String(motion);
  url += "&field5=" + String(door);

  thingSpeakClient.setInsecure();
  http.begin(thingSpeakClient, url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.print("[ThingSpeak] HTTP code: ");
    Serial.println(httpCode);
  } else {
    Serial.println("[ThingSpeak] Failed to send data");
  }

  http.end();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;

  for (unsigned int index = 0; index < length; index++) {
    message += static_cast<char>(payload[index]);
  }

  message.trim();
  message.toUpperCase();

  Serial.print("[MQTT] ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(message);

  if (strcmp(topic, topicModeCommand) == 0) {
    if (message == "AUTO" || message == "MANUAL") {
      controlMode = message;
      mqttClient.publish(topicModeStatus, controlMode.c_str(), true);
      Serial.print("[MQTT] Control mode: ");
      Serial.println(controlMode);
    }
    return;
  }

  if (strcmp(topic, topicLed1Command) == 0) {
    if (message == "ON" || message == "OFF") {
      digitalWrite(LED_PIN_1, message == "ON" ? HIGH : LOW);
      led1OverrideUntil = millis() + manualOverrideDuration;
      mqttClient.publish(topicLed1Status, message.c_str(), true);
    }
  }
  else if (strcmp(topic, topicLed2Command) == 0) {
    if (message == "ON" || message == "OFF") {
      digitalWrite(LED_PIN_2, message == "ON" ? HIGH : LOW);
      led2OverrideUntil = millis() + manualOverrideDuration;
      mqttClient.publish(topicLed2Status, message.c_str(), true);
    }
  }
  else if (strcmp(topic, topicDoorCommand) == 0) {
    if (message == "OPEN") {
      doorOpen = true;
      doorOverrideUntil = millis() + manualOverrideDuration;
      doorServo.write(180);
      mqttClient.publish(topicDoorStatus, "OPEN", true);
    }
    else if (message == "CLOSE") {
      doorOpen = false;
      doorOverrideUntil = millis() + manualOverrideDuration;
      doorServo.write(0);
      mqttClient.publish(topicDoorStatus, "CLOSED", true);
    }
  }
  else if (strcmp(topic, topicBuzzerCommand) == 0) {
    if (message == "ON" || message == "TEST") {
      Serial.println("[BUZZER] Test beep");
      playTestBuzzer();
    }
  }
}

void connectMQTT() {
  if (mqttClient.connected() ||
      millis() - lastMqttAttempt < mqttReconnectInterval) {
    return;
  }

  lastMqttAttempt = millis();
  Serial.println("[MQTT] Connecting to local broker...");

  String clientId = "SmartHouse_ESP8266_" + String(ESP.getChipId(), HEX);

  if (mqttClient.connect(clientId.c_str(), mqttUsername, mqttPassword)) {
    Serial.println("[MQTT] Connected!");
    mqttClient.subscribe(topicModeCommand);
    mqttClient.subscribe(topicLed1Command);
    mqttClient.subscribe(topicLed2Command);
    mqttClient.subscribe(topicDoorCommand);
    mqttClient.subscribe(topicBuzzerCommand);
    mqttClient.publish(topicModeStatus, controlMode.c_str(), true);
  }
  else {
    Serial.print("[MQTT] Connection failed. State = ");
    Serial.println(mqttClient.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("SMART HOUSE LOCAL MQTT SYSTEM");

  pinMode(PIR_PIN, INPUT);
  pinMode(LDR_PIN_2, INPUT_PULLUP);
  pinMode(LED_PIN_1, OUTPUT);
  pinMode(LED_PIN_2, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_PIN_1, LOW);
  digitalWrite(LED_PIN_2, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();

  doorServo.attach(SERVO_PIN_1);
  doorServo.write(0);

  connectWiFi();

  mqttClientTransport.setInsecure();
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);

  Serial.println("[SYSTEM] Ready");
}

void loop() {
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  int motion = digitalRead(PIR_PIN);

  if (motion != lastPublishedMotionState) {
    mqttClient.publish(
      topicMotion,
      motion == HIGH ? "DETECTED" : "CLEAR"
    );
    lastPublishedMotionState = motion;
    lastMotionChange = millis();
  }

  if (millis() - lastDhtSample >= dhtSampleInterval) {
    lastDhtSample = millis();
    lastHumidity = dht.readHumidity();
    lastTemperature = dht.readTemperature();
  }

  int ldrValue1 = analogRead(LDR_PIN_1);
  int ldrValue2 = digitalRead(LDR_PIN_2);

  if (millis() >= led2OverrideUntil &&
      millis() - lastPhoto2Sample >= photo2SampleInterval) {
    lastPhoto2Sample = millis();
    photo2StableState = ldrValue2;
    digitalWrite(LED_PIN_2, photo2StableState == photo2DarkState ? HIGH : LOW);
  }

  if (controlMode == "AUTO") {
    if (millis() >= led1OverrideUntil) {
      digitalWrite(LED_PIN_1, ldrValue1 < lightThreshold ? HIGH : LOW);
    }

    if (millis() >= doorOverrideUntil) {
      if (motion == HIGH) {
        doorOpen = true;
        doorCloseAt = millis() + doorOpenDuration;
        doorServo.write(180);
        if (previousMotionState == LOW) {
          digitalWrite(BUZZER_PIN, HIGH);
          delay(300);
          digitalWrite(BUZZER_PIN, LOW);
        }
      }
      else if (doorOpen && millis() >= doorCloseAt) {
        doorOpen = false;
        doorServo.write(0);
        digitalWrite(BUZZER_PIN, LOW);
      }
    }

  }

  previousMotionState = motion;

  if (motion == LOW &&
      millis() - lastMotionChange >= thingSpeakMotionQuietTime &&
      millis() - lastThingSpeakPublish >= thingSpeakInterval) {
    lastThingSpeakPublish = millis();
    sendToThingSpeak(
      lastTemperature,
      lastHumidity,
      ldrValue1,
      motion == HIGH ? 1 : 0,
      doorOpen ? 1 : 0
    );
  }

  if (millis() - lastPublish >= publishInterval) {
    lastPublish = millis();

    if (!isnan(lastTemperature)) {
      char value[10];
      dtostrf(lastTemperature, 1, 2, value);
      mqttClient.publish(topicTemperature, value);
    }

    if (!isnan(lastHumidity)) {
      char value[10];
      dtostrf(lastHumidity, 1, 2, value);
      mqttClient.publish(topicHumidity, value);
    }

    char value[10];
    sprintf(value, "%d", ldrValue1);
    mqttClient.publish(topicLight1, value);
    sprintf(value, "%d", ldrValue2);
    mqttClient.publish(topicLight2, value);

    mqttClient.publish(topicMotion, motion == HIGH ? "DETECTED" : "CLEAR");
    mqttClient.publish(topicLed1Status, digitalRead(LED_PIN_1) == HIGH ? "ON" : "OFF", true);
    mqttClient.publish(topicLed2Status, digitalRead(LED_PIN_2) == HIGH ? "ON" : "OFF", true);

    if (controlMode == "AUTO") {
      mqttClient.publish(topicDoorStatus, doorOpen ? "OPEN" : "CLOSED", true);
    }
    Serial.println("[MQTT] Sensor data published");
    Serial.print("[SENSOR] LDR #2 digital value: ");
    Serial.println(ldrValue2);
    Serial.print("[SENSOR] PIR value: ");
    Serial.println(motion);
  }

  delay(100);
}