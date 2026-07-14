#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "secrets.h"  // <--- Add this line

// ================= REMOVE OLD HARDCODED VALUES AND USE DEFINE CODES =================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* mqtt_server = MQTT_SERVER;
const int mqtt_port = MQTT_PORT;
const char* mqtt_user = MQTT_USER;
const char* mqtt_pass = MQTT_PASS;

// Each physical device MUST have a unique ID -> unique topics + unique NVS namespace.
// Change this per board when you flash it (or read from a DIP switch / chip ID).
const char* DEVICE_ID = "sensor1";

String topic_temp;
String topic_humid;
String topic_rawadc;       // raw ADC, calibrated (rawADC * calibrationFactor)
String topic_avgadc;       // untouched humidity/temp-compensated moving average
String topic_calibration;  // backend -> device: benchmark ADC value to match

// ================ PINS ==================
#define SENSOR_POWER_PIN  4
#define MQ135_PIN         36
#define DHTPIN            27
#define DHTTYPE           DHT22

DHT dht(DHTPIN, DHTTYPE);
WiFiServer server(80);

WiFiClientSecure espSecureClient;
PubSubClient mqttClient(espSecureClient);

// ============= CALIBRATION (persisted in flash) =============
Preferences prefs;
float calibrationFactor = 1.0; // calibratedRawADC = rawADC * calibrationFactor

unsigned long startTime;

const int NUM_SAMPLES = 10;
float history[NUM_SAMPLES];
int historyIndex = 0;
bool bufferFilled = false;

// Holds the most recent raw ADC reading, used when a calibration command
// arrives so we know "this sensor's current reading" to compare against
// the benchmark.
float lastRawADC = 0;

// --- Load / save calibration factor from NVS ---
void loadCalibration() {
  prefs.begin("mq135cal", true); // read-only
  calibrationFactor = prefs.getFloat("factor", 1.0);
  prefs.end();
  Serial.print("Loaded calibration factor: ");
  Serial.println(calibrationFactor, 6);
}

void saveCalibration(float factor) {
  prefs.begin("mq135cal", false); // read-write
  prefs.putFloat("factor", factor);
  prefs.end();
  calibrationFactor = factor;
  Serial.print("Saved new calibration factor: ");
  Serial.println(calibrationFactor, 6);
}

// --- MQTT message handler ---
// Expects the calibration topic payload to be the benchmark ADC value as a
// plain number, e.g. "500"
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == topic_calibration) {
    float benchmarkADC = msg.toFloat();

    if (benchmarkADC <= 0 || lastRawADC <= 0) {
      Serial.println("Calibration ignored: invalid benchmark or no sensor reading yet.");
      return;
    }

    float newFactor = benchmarkADC / lastRawADC;
    saveCalibration(newFactor);

    Serial.print("Calibrated against benchmark=");
    Serial.print(benchmarkADC);
    Serial.print(" using current raw ADC=");
    Serial.println(lastRawADC);
  }
}

// --- Reconnect to MQTT ---
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting Secure MQTT connection...");
    String clientId = "ESP32Client-" + String(DEVICE_ID);

    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("CONNECTED to secure HiveMQ Cloud!");
      mqttClient.subscribe(topic_calibration.c_str());
      Serial.print("Subscribed to: ");
      Serial.println(topic_calibration);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

// ========================================
void setup() {
  Serial.begin(115200);

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, HIGH);

  dht.begin();

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Build per-device topics
  topic_temp        = "washroom/" + String(DEVICE_ID) + "/temperature";
  topic_humid       = "washroom/" + String(DEVICE_ID) + "/humidity";
  topic_rawadc      = "washroom/" + String(DEVICE_ID) + "/rawadc";
  topic_avgadc      = "washroom/" + String(DEVICE_ID) + "/avgadc";
  topic_calibration = "washroom/" + String(DEVICE_ID) + "/calibration";

  loadCalibration();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.println("\nConnecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  server.begin();

  espSecureClient.setInsecure();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  startTime = millis();

  for (int i = 0; i < NUM_SAMPLES; i++)
    history[i] = 0;
}

// ========================================
void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  if (millis() - startTime < 60000) {
    Serial.println("Sensor warming up...");
    delay(1000);
    return;
  }

  // -------- Read DHT --------
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  if (isnan(humidity)) humidity = 50.0;
  if (isnan(temperature)) temperature = 25.0;

  // -------- Average MQ135 Readings --------
  long totalADC = 0;
  for (int i = 0; i < 20; i++) {
    totalADC += analogRead(MQ135_PIN);
    delay(20);
  }
  int rawADC = totalADC / 20;

  // Temperature/Humidity compensation
  float correction = 1.0 +
      (0.003 * (humidity - 50.0)) +
      (0.01 * (temperature - 25.0));
  float correctedADC = rawADC / correction;

  // -------- Moving Average Filter --------
  history[historyIndex] = correctedADC;
  historyIndex++;
  if (historyIndex >= NUM_SAMPLES) {
    historyIndex = 0;
    bufferFilled = true;
  }

  float avgADC = 0;
  int count = bufferFilled ? NUM_SAMPLES : historyIndex;
  for (int i = 0; i < count; i++) avgADC += history[i];
  avgADC /= count;

  lastRawADC = rawADC; // remember for next calibration command

  // -------- Apply per-device calibration to RAW ADC only --------
  // avgADC (humidity/temp compensated moving average) is left untouched.
  float calibratedRawADC = rawADC * calibrationFactor;

  // -------- Serial Output --------
  Serial.println("--------------------------------");
  Serial.print("Temp: "); Serial.print(temperature); Serial.println(" C");
  Serial.print("Humidity: "); Serial.print(humidity); Serial.println(" %");
  Serial.print("Raw ADC: "); Serial.println(rawADC);
  Serial.print("Calibrated Raw ADC: "); Serial.println(calibratedRawADC, 3);
  Serial.print("Avg ADC (compensated, uncalibrated): "); Serial.println(avgADC, 3);
  Serial.print("Factor: "); Serial.println(calibrationFactor, 6);

  // -------- Publish to MQTT Broker --------
  mqttClient.publish(topic_temp.c_str(), String(temperature, 1).c_str());
  mqttClient.publish(topic_humid.c_str(), String(humidity, 1).c_str());
  mqttClient.publish(topic_rawadc.c_str(), String(calibratedRawADC).c_str()); // calibrated raw value
  mqttClient.publish(topic_avgadc.c_str(), String(avgADC).c_str());           // untouched compensated avg

  // -------- Web Server (optional debug) --------
  WiFiClient client = server.available();
  if (client) {
    while (client.connected() && client.available()) {
      client.readStringUntil('\n');
    }
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.print("Temperature: "); client.println(temperature);
    client.print("Humidity: "); client.println(humidity);
    client.print("Raw ADC: "); client.println(rawADC);
    client.print("Calibrated Raw ADC: "); client.println(calibratedRawADC);
    client.print("Avg ADC (compensated): "); client.println(avgADC);
    client.print("Calibration Factor: "); client.println(calibrationFactor, 6);
    client.stop();
  }

  delay(3000);
}