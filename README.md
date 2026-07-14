# ESP32 Odour & Climate Sensor Firmware

This repository contains the firmware for an ESP-based washroom odour tracking node. It collects data from an MQ-135 air quality sensor and a DHT22 climate sensor, cleans the readings via digital filters, and securely streams telemetry data to HiveMQ Cloud over TLS/SSL MQTT.

## 🛠️ Hardware Requirements
- **Microcontroller:** ESP (e.g., ESP32 NodeMCU)
- **Air Quality Sensor:** MQ-135 Odour Sensor
- **Climate Sensor:** DHT22 (AM2302) Temperature & Humidity Sensor
- **Transistor/Switch:** Connected to Pin 4 to handle sensor power lines (warm-up control)

## 📌 Pin Configurations

| Component | ESP32 Pin | Function |
| :--- | :--- | :--- |
| **SENSOR_POWER_PIN** | GPIO 4 | Controls VCC to the MQ-135 sensor |
| **MQ135_PIN** | GPIO 36 (ADC1_CH0) | Reads analogue air quality data |
| **DHTPIN** | GPIO 27 | Reads 1-Wire DHT22 temperature/humidity |

## 🔒 Security & Local Setup
Credentials are separated from the main source code file to prevent secret leakage.

1. Navigate to the firmware directory.
2. Duplicate the file `secrets.h.example` and rename it to `secrets.h`.
3. Open `secrets.h` and fill in your Wi-Fi credentials and HiveMQ Cloud cluster access strings.

> **Note:** The `secrets.h` pattern is ignored globally by Git configuration rules to guarantee your networks and brokers stay protected.

## 📡 MQTT Topic Architecture
Every device uses a unique `DEVICE_ID` string (default: `sensor1`) to establish distinct topics on the broker automatically. 

### Published Topics (Device ➡️ Broker)
- `washroom/<DEVICE_ID>/temperature` — Local workspace temperature in Celsius (`°C`).
- `washroom/<DEVICE_ID>/humidity` — Relative humidity percentage (`%`).
- `washroom/<DEVICE_ID>/rawadc` — Raw analog read value scaled against the device's persistent custom calibration multiplier.
- `washroom/<DEVICE_ID>/avgadc` — Time-smoothed moving average calculated with mathematical temperature and humidity offset adjustments.

### Subscribed Topics (Backend ➡️ Device)
- `washroom/<DEVICE_ID>/calibration` — Expects a raw target benchmark numeric string (e.g. `500.0`). Calculates a fresh relative offset scale factor and saves it permanently to the ESP32's Non-Volatile Storage (NVS).

## ⚙️ Data Pipeline Processing
1. **Warm-up Delay:** The node blocks reads for the first 60 seconds on bootup to let the internal MQ-135 sensor element heat to stable operation levels.
2. **Environmental Compensation:** Raw analog readings shift depending on environment shifts. The code uses real-time DHT22 metrics to scale values to a neutral $25^\circ\text{C}$ and $50\%$ RH baseline.
3. **Smoothing Moving Average:** Eliminates analog signal noise spikes by managing a trailing historical rolling array filter over the last 10 samples.
