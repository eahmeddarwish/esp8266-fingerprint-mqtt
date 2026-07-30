/*
 * ESP8266 Fingerprint over MQTT
 * =============================
 * A Wi-Fi fingerprint access node built on a Wemos D1 / ESP8266 and an optical
 * fingerprint sensor. It runs three modes that are switched remotely over MQTT:
 *
 *   READING  — scan fingers; on a match, publish {id, state, confidence} as JSON
 *   LEARNING — enrol a new fingerprint at an incoming ID
 *   DELETE   — delete a stored fingerprint by ID
 *
 * Status and results are published to MQTT topics so a home-automation hub
 * (Home Assistant, Node-RED, etc.) can react to who was recognised.
 *
 * SECURITY: all credentials below are PLACEHOLDERS. Fill in your own Wi-Fi and
 * MQTT details and never commit the real ones.
 *
 * Author : Ahmed Darwish  <eahmeddarwish@gmail.com>
 * License: MIT
 */

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>

// ------------------------------- CONFIG (placeholders) ------------------- //
#define SSID            "Your Wifi SSID"
#define PASSWORD        "Your Wifi Password"

#define HOSTNAME        "fingerprint-sensor"
#define MQTT_SERVER     "your.mqtt.broker"
#define MQTT_PORT       1883
#define MQTT_USERNAME   "MQTT Username"
#define MQTT_PASSWORD   "MQTT Password"

// MQTT topics
#define STATE_TOPIC        "/fingerprint/state"
#define RESULT_TOPIC       "/fingerprint/result"
#define AVAILABILITY_TOPIC "/fingerprint/available"
#define MODE_READING       "/fingerprint/mode/reading"
#define MODE_LEARNING      "/fingerprint/mode/learning"
#define MODE_DELETE        "/fingerprint/mode/delete"

#define MQTT_INTERVAL   5000     // ms between idle status messages
#define SENSOR_RX       12       // GPIO to sensor TX
#define SENSOR_TX       14       // GPIO to sensor RX
// ------------------------------------------------------------------------- //

SoftwareSerial sensorSerial(SENSOR_RX, SENSOR_TX);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&sensorSerial);
WiFiClient wifiClient;
PubSubClient client(wifiClient);

enum Mode { READING, LEARNING, DELETE_ID };
Mode mode = READING;
uint16_t targetId = 0;             // ID used by LEARNING / DELETE
unsigned long lastMsg = 0;

DynamicJsonDocument doc(128);
char buffer[128];

// ----------------------------------- setup ------------------------------- //
void setup() {
  Serial.begin(57600);
  finger.begin(57600);
  delay(50);
  Serial.println(finger.verifyPassword() ? "Fingerprint sensor found."
                                          : "Fingerprint sensor NOT found.");

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.print("\nIP: "); Serial.println(WiFi.localIP());

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(onMessage);
}

// ----------------------------------- loop -------------------------------- //
void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  if (mode == READING) {
    int id = getFingerprintID();
    if (id >= 0) publishResult("reading", id, "Matched", finger.confidence);
    else if (millis() - lastMsg > MQTT_INTERVAL) {
      publishResult("reading", -1, "No match", 0);
      lastMsg = millis();
    }
  } else if (mode == LEARNING) {
    bool ok = enrollFinger(targetId);
    publishResult("learning", targetId, ok ? "Enrolled" : "Failed", 0);
    mode = READING;                 // one enrolment per command
  } else if (mode == DELETE_ID) {
    bool ok = (finger.deleteModel(targetId) == FINGERPRINT_OK);
    publishResult("delete", targetId, ok ? "Deleted" : "Failed", 0);
    mode = READING;
  }
}

// ------------------------------ MQTT plumbing ---------------------------- //
void reconnect() {
  while (!client.connected()) {
    if (client.connect(HOSTNAME, MQTT_USERNAME, MQTT_PASSWORD,
                        AVAILABILITY_TOPIC, 0, true, "offline")) {
      client.publish(AVAILABILITY_TOPIC, "online", true);
      client.subscribe(MODE_READING);
      client.subscribe(MODE_LEARNING);
      client.subscribe(MODE_DELETE);
    } else {
      delay(2000);
    }
  }
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  int id = 0;
  for (unsigned int i = 0; i < len; i++)
    if (payload[i] >= '0' && payload[i] <= '9') id = id * 10 + (payload[i] - '0');

  if (strcmp(topic, MODE_READING) == 0)       mode = READING;
  else if (strcmp(topic, MODE_LEARNING) == 0) { mode = LEARNING;  targetId = id; }
  else if (strcmp(topic, MODE_DELETE) == 0)   { mode = DELETE_ID; targetId = id; }
}

void publishResult(const char* m, int id, const char* state, int confidence) {
  doc.clear();
  doc["mode"] = m;
  doc["id"] = id;
  doc["state"] = state;
  doc["confidence"] = confidence;
  size_t n = serializeJson(doc, buffer);
  client.publish(RESULT_TOPIC, buffer, n);
}

// ----------------------------- fingerprint ------------------------------- //
// Returns the matched ID, or -1 on no-match / no-finger.
int getFingerprintID() {
  if (finger.getImage() != FINGERPRINT_OK) return -1;
  if (finger.image2Tz() != FINGERPRINT_OK) return -1;
  if (finger.fingerFastSearch() != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

// Two-capture enrolment into slot `id`.
bool enrollFinger(uint16_t id) {
  Serial.printf("Enrolling ID %u — place finger...\n", id);
  while (finger.getImage() != FINGERPRINT_OK) {}
  if (finger.image2Tz(1) != FINGERPRINT_OK) return false;
  Serial.println("Remove finger...");
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) {}
  Serial.println("Place the same finger again...");
  while (finger.getImage() != FINGERPRINT_OK) {}
  if (finger.image2Tz(2) != FINGERPRINT_OK) return false;
  if (finger.createModel() != FINGERPRINT_OK) return false;
  return finger.storeModel(id) == FINGERPRINT_OK;
}
