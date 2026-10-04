/**
 * @file main.cpp
 * @brief Main application entry point
 * * Professional IoT Hydroponic Control System
 * ESP32 Dev Board based with MQTT telemetry and deep sleep support
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "config.h"
#include "SensorManager.h"
#include "WiFiManager.h"
#include "MQTTManager.h"
#include "PumpController.h"
#include "RelayManager.h"
#include "EnvironmentManager.h"

#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include <esp_task_wdt.h>

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <cmath>

// ============================================================================
// GLOBAL OBJECTS & STATE
// ============================================================================

// Conditionally instantiate the correct client to save memory
#if MQTT_USE_TLS
    #include <WiFiClientSecure.h>
    WiFiClientSecure wifiClient;
#else
    WiFiClient wifiClient;
#endif
#include <esp_wifi.h>
Client* netClient = &wifiClient; // Base pointer to pass to MQTTManager

WiFiManager wifiMgr;
MQTTManager mqttMgr;
SensorManager sensorMgr;
PumpController pumpCtrl(RELAY_MAIN_PUMP);
Preferences preferences;

// Global configuration instance
SystemConfig sysConfig;
SensorData sensorData;
SystemStatus systemStatus = {
    .state = STATE_INIT,
    .wifiState = CONN_DISCONNECTED,
    .mqttState = CONN_DISCONNECTED,
    .pumpState = PUMP_COOLDOWN,
    .errorCount = 0,
    .uptime = 0,
    .lastSleepTime = 0
};
String caCert; // Holds the cert in memory during runtime
String pendingOtaUrl = "";
String pendingOtaSha256 = "";

// Timing variables
unsigned long lastSensorRead = 0;
unsigned long lastMQTTPublish = 0;
unsigned long lastHeartbeat = 0;
unsigned long wakeTime = 0;
unsigned long lastMQTTAttempt = 0;
unsigned long MQTT_RETRY_INTERVAL = 5000; // Retry every 5 seconds

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================
void setupSystem();
void setupWiFi();
void setupMQTT();
void handleMQTTMessage(const char* topic, const char* payload);
void readSensors();
void publishTelemetry();
void checkBatteryLevel();
void enterDeepSleep(uint32_t sleepDurationSec);
void enterLightSleep(uint32_t sleepDurationSec);
void handleEmergency(const char* reason);
void updateSystemStatus();
void printSystemInfo();
void performOTAUpdate(const char* firmwareUrl, const char* expectedSha256);
void validateConfiguration();
void loadConfiguration();
void saveConfiguration();
void checkRollback();
void publishSensorStatusPayload();

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    // Initialize serial
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && millis() - start < 10000) delay(10);
    Serial.println("=== SERIAL CONNECTED ===");

    // ---> REMOVED CUSTOM WATCHDOG INIT HERE <---

    Serial.println();
    Serial.println("====================================");
    Serial.println("  Hydroponic Control System");
    Serial.println("  Firmware: " FIRMWARE_VERSION);
    Serial.println("  Hardware: " HARDWARE_REVISION);
    Serial.println("====================================");

    loadConfiguration();
    validateConfiguration();

    // Record wake time
    wakeTime = millis();

    // Check wake reason
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if(wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        LOG_INFO("Wake from timer");
    } else {
        LOG_INFO("Power on reset");
    }

    // Setup system
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_WATER_LEVEL, ADC_11db);
    analogSetPinAttenuation(PIN_BATTERY_VOLTAGE, ADC_11db);

    setupSystem();
    checkRollback();
}

void setupSystem() {
    systemStatus.state = STATE_INIT;
    
    // Initialize LittleFS
    if (!LittleFS.begin(true)) {
        LOG_ERROR("LittleFS Mount Failed. Formatting...");
    }

    // Configure Network Client Security
    #if MQTT_USE_TLS
        LOG_INFO("Configuring TLS for MQTT...");
        
        #if MQTT_ALLOW_INSECURE_TLS
            wifiClient.setInsecure();
            LOG_WARN("TLS set to INSECURE mode (No CA verification)");
        #elif MQTT_USE_CA_CERT
            File certFile = LittleFS.open(MQTT_CA_FILENAME, "r");
            if (certFile) {
                caCert = certFile.readString();
                wifiClient.setCACert(caCert.c_str());
                certFile.close();
                LOG_INFO("CA Certificate loaded from LittleFS: %s", MQTT_CA_FILENAME);
            } else {
                LOG_ERROR("Failed to open CA certificate! TLS connection will likely fail.");
            }
        #endif
    #endif
    relayManager.begin();
    // Initialize sensors
    LOG_INFO("Initializing sensors...");
    if (!sensorMgr.begin()) {
        LOG_ERROR("Sensor initialization failed");
        systemStatus.errorCount++;
    }
    
    // Initialize pump controller
    LOG_INFO("Initializing pump controller...");
    if (!pumpCtrl.begin()) {
        LOG_ERROR("Pump initialization failed");
        systemStatus.errorCount++;
    }

    // Initialize environment manager (fan, dosing, lighting)
    envManager.begin();
    
    // Setup WiFi
    setupWiFi();

    // Setup MQTT
    if (systemStatus.wifiState == CONN_CONNECTED) setupMQTT();
    
    // System ready
    if (systemStatus.errorCount < MAX_CONSECUTIVE_ERRORS) {
        systemStatus.state = STATE_ACTIVE;
        LOG_INFO("System initialization complete");
        printSystemInfo();
    } else {
        handleEmergency("Too many initialization errors");
    }
}

void setupWiFi() {
    LOG_INFO("Setting up WiFi...");
    
    wifiMgr.begin();
    WiFi.setSleep(false); 
    
    unsigned long startAttemptTime = millis();
    const unsigned long connectionTimeout = 30000; // 30 seconds timeout

    // Loop until connected or timeout
    while (!wifiMgr.isConnected() && (millis() - startAttemptTime < connectionTimeout)) {
        // FEED THE WATCHDOG inside the loop so it doesn't reset the ESP
        esp_task_wdt_reset(); 
        
        // Non-blocking connection check
        wifiMgr.loop(); 
        delay(500);
        Serial.print("."); 
    }
    Serial.println();
    
    if (wifiMgr.isConnected()) {
        systemStatus.wifiState = CONN_CONNECTED;
        LOG_INFO("WiFi connected: %s", wifiMgr.getIPAddress().c_str());
        LOG_INFO("Signal strength: %d dBm", wifiMgr.getRSSI());
    } else {
        systemStatus.wifiState = CONN_ERROR;
        LOG_WARN("WiFi connection failed - continuing in offline mode");
    }
}

void setupMQTT() {
    LOG_INFO("Setting up MQTT...");
    
    // Pass the dereferenced base Client
    mqttMgr.begin(*netClient);
    //mqttMgr.begin(wifiClient);
    mqttMgr.setCallback(handleMQTTMessage);

    if (mqttMgr.connect()) {
        systemStatus.mqttState = CONN_CONNECTED;
        LOG_INFO("MQTT connected");
        mqttMgr.publishSystemStatus(systemStatus); // Publish initial status
        validateConfiguration();
    } else {
        systemStatus.mqttState = CONN_ERROR;
        LOG_WARN("MQTT connection failed");
    }
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    if (systemStatus.state == STATE_OTA_UPDATE) {
        if (mqttMgr.isConnected()) {
            delay(500); // Flush logs/MQTT
            mqttMgr.disconnect();
        }
        LOG_INFO("Executing sequential OTA Update...");
        performOTAUpdate(pendingOtaUrl.c_str(), pendingOtaSha256.c_str());
        // If it returns, OTA failed, fallback to active state
        systemStatus.state = STATE_ACTIVE;
        return;
    }

    unsigned long currentMillis = millis();
    systemStatus.uptime = currentMillis;
    // Update managers
    wifiMgr.loop();
    systemStatus.wifiState = wifiMgr.getState();
    
    // If WiFi is connected but MQTT hasn't even been initialized yet...
    if (systemStatus.wifiState == CONN_CONNECTED) {
        bool needsConnect = (systemStatus.mqttState == CONN_DISCONNECTED || systemStatus.mqttState == CONN_ERROR);
        if (needsConnect && (currentMillis - lastMQTTAttempt >= MQTT_RETRY_INTERVAL)) {
            lastMQTTAttempt = currentMillis;
            LOG_INFO("Attempting MQTT reconnection...");
            setupMQTT();
            if (!mqttMgr.isConnected()) MQTT_RETRY_INTERVAL = min(MQTT_RETRY_INTERVAL * 2, 60000UL);
            else MQTT_RETRY_INTERVAL = 5000; // Reset on success
        }
    }
    else {
        // If WiFi is disconnected, ensure MQTT is marked as disconnected too
        if (systemStatus.mqttState != CONN_DISCONNECTED) {
            systemStatus.mqttState = CONN_DISCONNECTED;
            LOG_INFO("WiFi lost - MQTT marked as disconnected");
        }
    }

    // Process MQTT messages
    if (mqttMgr.isConnected()) mqttMgr.loop();

    // Run actuator controllers
    pumpCtrl.loop();
    envManager.loop();
    
    // Update connection states
    systemStatus.wifiState = wifiMgr.getState();
    systemStatus.mqttState = mqttMgr.getState();
    systemStatus.pumpState = pumpCtrl.getState();
    
    // Read sensors periodically
    if (currentMillis - lastSensorRead >= sysConfig.sensorReadInterval) {
        readSensors();
        checkBatteryLevel(); // Only check when we have new data!
        lastSensorRead = currentMillis;
    }

    // Publish telemetry periodically
    if (currentMillis - lastMQTTPublish >= sysConfig.mqttPublishInterval) {
        publishTelemetry();
        lastMQTTPublish = currentMillis;
    }
    
    // Send heartbeat
    if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        if (mqttMgr.isConnected()) mqttMgr.publishHeartbeat();
        lastHeartbeat = currentMillis;
    }
    
    // Check if it's time to sleep (for battery operation)
    if (sysConfig.deepSleepEnabled) {
        bool pumpIsIdle = (pumpCtrl.getState() == PUMP_OFF || pumpCtrl.getState() == PUMP_COOLDOWN);
        bool isUpdating = (systemStatus.state == STATE_OTA_UPDATE);
        if (!isUpdating && pumpIsIdle && (currentMillis - wakeTime >= sysConfig.activeDurationMs)) {
            // Using the threshold to decide: Deep Sleep vs Light Sleep
            // If cooldown is > 5 mins (300s), go Deep. Else, go Light.
            String payload = "{\"status\":\"sleeping\",\"duration\":" + String(sysConfig.sleepDurationSec);
            if (sysConfig.pumpCooldownTime / 1000 > sysConfig.sleepThreshold) { 
                LOG_INFO("Long cooldown: Entering Deep Sleep");
                payload += ",\"mode\":\"deep\"}";
                mqttMgr.publish(TOPIC_STATUS, payload, true);
                Serial.flush(); 
                delay(500); // Give the WiFi radio time to push the packet!
                enterDeepSleep(sysConfig.sleepDurationSec);
            } else {
                LOG_INFO("Short cooldown: Entering Light Sleep");
                payload += ",\"mode\":\"light\"}";
                mqttMgr.publish(TOPIC_STATUS, payload, true);
                enterLightSleep(sysConfig.sleepDurationSec);
                wakeTime = millis(); // Reset wake timer after Light Sleep
                if(mqttMgr.isConnected()) mqttMgr.loop(); // Important: Re-sync MQTT after Light Sleep
            }
        }
    }

    // Small delay to prevent watchdog issues
    delay(10);
    //esp_task_wdt_reset();
    yield();
}

// ============================================================================
// MQTT MESSAGE HANDLER
// ============================================================================
bool verifyPayloadSignature(const String& payloadStr, const char* signatureBase64) {
    // 1. Load Public Key from LittleFS
    File pubKeyFile = LittleFS.open(OTA_PUBKEY_FILENAME, "r");
    if (!pubKeyFile) {
        LOG_ERROR("Crypto: Failed to open public key file %s", OTA_PUBKEY_FILENAME);
        return false;
    }
    String pubKeyStr = pubKeyFile.readString();
    pubKeyFile.close();

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    // 2. Parse Public Key
    int ret = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)pubKeyStr.c_str(), pubKeyStr.length() + 1);
    if (ret != 0) {
        LOG_ERROR("Crypto: Failed to parse public key (-0x%04x)", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    // 3. Hash the payload string (SHA256)
    uint8_t hash[32];
    mbedtls_md_context_t md_ctx;
    mbedtls_md_init(&md_ctx);
    mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&md_ctx);
    mbedtls_md_update(&md_ctx, (const unsigned char*)payloadStr.c_str(), payloadStr.length());
    mbedtls_md_finish(&md_ctx, hash);
    mbedtls_md_free(&md_ctx);

    // 4. Decode Base64 Signature
    unsigned char sig_bytes[256]; // 256 bytes for RSA-2048
    size_t sig_len = 0;
    ret = mbedtls_base64_decode(sig_bytes, sizeof(sig_bytes), &sig_len, (const unsigned char*)signatureBase64, strlen(signatureBase64));
    if (ret != 0) {
        LOG_ERROR("Crypto: Failed to decode base64 signature (-0x%04x)", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    // 5. Verify the RSA Signature
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig_bytes, sig_len);
    mbedtls_pk_free(&pk);

    if (ret == 0) {
        LOG_INFO("Crypto: Signature verification PASSED!");
        return true;
    } else {
        LOG_ERROR("Crypto: Signature verification FAILED (-0x%04x)", -ret);
        return false;
    }
}

void handleMQTTMessage(const char* topic, const char* payload) {

    LOG_INFO("Processing command: %s -> %s", topic, payload);
    if (strlen(payload) > 1024) {
        LOG_ERROR("Payload too large");
        return;
    }
    // Parse payload
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        LOG_ERROR("JSON parsing failed: %s", error.c_str());
        return;
    }
    
    // Handle pump commands
    if (strcmp(topic, TOPIC_CMD_PUMP) == 0) {
        const char* action = doc["action"] | "unknown";
        
        if (strcmp(action, "on") == 0) {
            unsigned long duration = doc["duration"] | 0;
            if (pumpCtrl.turnOn(duration)) {
                LOG_INFO("Pump command executed: ON");
                mqttMgr.publish(TOPIC_STATUS, "{\"pump\":\"on\"}");
            } else {
                LOG_ERROR("Failed to turn pump on");
                mqttMgr.publishError("Pump start failed");
            }
        } else if (strcmp(action, "off") == 0) {
            pumpCtrl.turnOff();
            LOG_INFO("Pump command executed: OFF");
            mqttMgr.publish(TOPIC_STATUS, "{\"pump\":\"off\"}");
        }
    }
    
    // Handle mode commands
    else if (strcmp(topic, TOPIC_CMD_MODE) == 0) {
        const char* mode = doc["mode"] | "unknown";
        
        if (strcmp(mode, "active") == 0) {
            systemStatus.state = STATE_ACTIVE;
            LOG_INFO("Mode changed: ACTIVE");
        } else if (strcmp(mode, "maintenance") == 0) {
            systemStatus.state = STATE_MAINTENANCE;
            pumpCtrl.turnOff();
            LOG_INFO("Mode changed: MAINTENANCE");
        } else if (strcmp(mode, "sleep") == 0) {
            LOG_INFO("Sleep command received");
            enterDeepSleep(sysConfig.sleepDurationSec);
        }
        
        mqttMgr.publishSystemStatus(systemStatus);
    }
    
    // Handle configuration updates
    else if (strcmp(topic, TOPIC_CMD_CONFIG) == 0) {
        LOG_INFO("Configuration update received");
        bool changed = false;
        
        // Remote Tuning of Parameters
        if (doc.containsKey("sleep_en"))  { sysConfig.deepSleepEnabled = doc["sleep_en"]; changed = true; }
        if (doc.containsKey("sleep_sec")) { sysConfig.sleepDurationSec = doc["sleep_sec"]; changed = true; }
        if (doc.containsKey("active_dur")) { sysConfig.activeDurationMs = doc["active_dur"]; changed = true; }
        if (doc.containsKey("read_int"))  { sysConfig.sensorReadInterval = doc["read_int"]; changed = true; }
        if (doc.containsKey("pub_int"))   { sysConfig.mqttPublishInterval = doc["pub_int"]; changed = true; }
        if (doc.containsKey("pump_max"))  { sysConfig.pumpMaxOnTime = doc["pump_max"]; changed = true; }
        if (doc.containsKey("pump_cool")) { sysConfig.pumpCooldownTime = doc["pump_cool"]; changed = true; }
        if (doc.containsKey("batt_crit")) { sysConfig.batteryCriticalThreshold = doc["batt_crit"]; changed = true; }
        if (doc.containsKey("test_cmds")) { sysConfig.testCommandsEnabled = doc["test_cmds"]; changed = true; }

        // Environment control config
        if (doc.containsKey("fan_en"))    { sysConfig.fanEnabled = doc["fan_en"]; changed = true; }
        if (doc.containsKey("air_t_max")) { sysConfig.airTempMax = doc["air_t_max"]; changed = true; }
        if (doc.containsKey("fan_hyst"))  { sysConfig.fanHysteresis = doc["fan_hyst"]; changed = true; }
        if (doc.containsKey("dose_en"))   { sysConfig.dosingEnabled = doc["dose_en"]; changed = true; }
        if (doc.containsKey("dose_pulse")) { sysConfig.dosingPulseMs = doc["dose_pulse"]; changed = true; }
        if (doc.containsKey("dose_lock"))  { sysConfig.dosingLockoutMs = doc["dose_lock"]; changed = true; }
        if (doc.containsKey("light_en"))  { sysConfig.lightingEnabled = doc["light_en"]; changed = true; }
        if (doc.containsKey("light_on_h")) { sysConfig.lightOnHour = doc["light_on_h"]; changed = true; }
        if (doc.containsKey("light_off_h")) { sysConfig.lightOffHour = doc["light_off_h"]; changed = true; }
        if (doc.containsKey("nut_en"))     { sysConfig.nutrientDosingEnabled = doc["nut_en"]; changed = true; }
        if (doc.containsKey("nut_delay"))  { sysConfig.nutrientDoseDelayMs = doc["nut_delay"]; changed = true; }

        if (changed) {
            validateConfiguration(); // Check the NEW values immediately
            //saveConfiguration();  (saveConfiguration is called inside validateConfiguration if adjusted)
            JsonDocument reply;
            reply["config"] = "updated_and_saved";
            if (doc.containsKey("transactionId")) {
                reply["transactionId"] = doc["transactionId"];
            }
            String replyPayload;
            serializeJson(reply, replyPayload);
            mqttMgr.publish(TOPIC_STATUS, replyPayload);
        }
    }

    // Handle OTA update commands
    // Payload: {"url":"https://...", "version":"1.2.0", "sha256":"<hex>", "signature":"<base64>"}
    else if (strcmp(topic, TOPIC_CMD_OTA) == 0) {
        LOG_INFO("OTA update command received");

        // 1. Extract values with safe fallbacks
        const char* fwUrl = doc["url"] | "unknown";
        const char* newVersion = doc["version"] | "0.0.0";
        const char* fwSha256 = doc["sha256"] | "";
        const char* fwSignature = doc["signature"] | "";

        // 2. Validate URL existence
        if (!fwUrl || strcmp(fwUrl, "unknown") == 0) {
            LOG_ERROR("OTA rejected: Missing URL");
            return;
        }

        // 3. CRYPTOGRAPHIC SIGNATURE CHECK (if enabled)
        // The signature covers the SHA256 hash of the firmware binary.
        // This proves the binary was built and signed by the private key holder.
        #if REQUIRE_SIGNED_FIRMWARE
            if (strlen(fwSha256) == 0 || strlen(fwSignature) == 0) {
                LOG_ERROR("OTA rejected: SHA256 and Signature required in strict mode");
                mqttMgr.publishError("Security Violation: Missing SHA256/Signature");
                return;
            }
            if (!verifyPayloadSignature(String(fwSha256), fwSignature)) {
                LOG_ERROR("SECURITY ALERT: OTA Signature Verification Failed!");
                mqttMgr.publishError("Security Violation: Invalid Signature");
                return;
            }
            LOG_INFO("OTA: RSA-SHA256 Signature Verified. Expected SHA256: %s", fwSha256);
        #endif

        // 4. PROTOCOL ENFORCEMENT
        if (!ALLOW_HTTP_OTA && !String(fwUrl).startsWith("https:")) {
            LOG_ERROR("SECURITY ALERT: HTTPS Required for OTA");
            mqttMgr.publishError("Security Violation: HTTP blocked");
            return;
        }

        // 5. SCHEDULE SYNCHRONOUS OTA
        pendingOtaUrl = String(fwUrl);
        pendingOtaSha256 = String(fwSha256);

        systemStatus.state = STATE_OTA_UPDATE;
        mqttMgr.publishSystemStatus(systemStatus);
        
        LOG_INFO("Scheduled sequential OTA Task for v%s...", newVersion);
        // loop() will catch the STATE_OTA_UPDATE and trigger performOTAUpdate safely.
    }

    else if (strcmp(topic, TOPIC_CMD_TANK) == 0) {
        const char* action = doc["action"];

        // === AUTO CALIBRATION ===
        if (strcmp(action, "calibrate") == 0) {
            if (sensorMgr.calibrateTankEmpty()) {
                mqttMgr.publish(TOPIC_STATUS, "{\"tank\":\"calibrated\",\"empty_distance_cm\":" + String(sysConfig.tankEmptyD) + "}");
            } else {
                mqttMgr.publishError("Tank calibration failed");
            }
        }

        // === MANUAL / FULL CONFIG ===
        else if (strcmp(action, "update_tank") == 0) {
            sensorMgr.updateTankConfig(
                doc["tankType"] | 0,
                doc["dimA"]     | 0,
                doc["dimB"]     | 0,
                doc["tankFullH"]| 0,
                doc["tankEmptyD"] | NAN
            );

            mqttMgr.publish(TOPIC_STATUS, "{\"tank\":\"updated\"}");
        }
    }

    else if (strcmp(topic, TOPIC_CMD_SENSORS) == 0) {
        const char* action = doc["action"] | "";
        if (strcmp(action, "status") == 0) {
            readSensors();
            publishSensorStatusPayload();
        }
    }

    else if (strcmp(topic, TOPIC_CMD_PH) == 0) {
        const char* action = doc["action"] | "";
        if (strcmp(action, "calibrate") != 0) {
            return;
        }
        const char* point = doc["point"] | "";

        if (strcmp(point, "mid") == 0) {
            float v = sensorMgr.samplePhVoltage();
            if (isnan(v)) {
                mqttMgr.publishError("pH calibration: ADC read failed");
                return;
            }
            float standard = doc["standard"] | 7.0f;
            preferences.begin("hydro-config", false);
            preferences.putFloat("ph_mid_v", v);
            preferences.putFloat("ph_mid_std", standard);
            preferences.end();
            StaticJsonDocument<192> out;
            out["ph_cal"] = "mid_done";
            out["raw_voltage"] = v;
            out["standard"] = standard;
            String s;
            serializeJson(out, s);
            mqttMgr.publish(TOPIC_STATUS, s);
        }
        else if (strcmp(point, "low") == 0) {
            preferences.begin("hydro-config", false);
            if (!preferences.isKey("ph_mid_v")) {
                preferences.end();
                mqttMgr.publishError("pH calibration: complete mid (pH 7) step first");
                return;
            }
            float vMid = preferences.getFloat("ph_mid_v", NAN);
            float stdMid = preferences.getFloat("ph_mid_std", 7.0f);
            preferences.remove("ph_mid_v");
            preferences.remove("ph_mid_std");
            preferences.end();

            float vLow = sensorMgr.samplePhVoltage();
            if (isnan(vLow)) {
                mqttMgr.publishError("pH calibration: ADC read failed");
                return;
            }
            float stdLow = doc["standard"] | 4.0f;
            float dv = vMid - vLow;
            if (fabsf(dv) < 1e-4f) {
                mqttMgr.publishError("pH calibration: mid and low voltages identical");
                return;
            }
            float slope = (stdMid - stdLow) / dv;
            float offset = stdMid - slope * vMid;
            sysConfig.phSlope = slope;
            sysConfig.phOffset = offset + PH_CALIBRATION_OFFSET;
            saveConfiguration();

            StaticJsonDocument<256> out;
            out["ph_cal"] = "complete";
            out["slope"] = slope;
            out["offset"] = offset;
            out["raw_voltage"] = vLow;
            String s;
            serializeJson(out, s);
            mqttMgr.publish(TOPIC_STATUS, s);
        }
        else if (strcmp(point, "reset") == 0) {
            sysConfig.phSlope = PH_DEFAULT_SLOPE;
            sysConfig.phOffset = PH_DEFAULT_OFFSET + PH_CALIBRATION_OFFSET;
            preferences.begin("hydro-config", false);
            preferences.remove("ph_mid_v");
            preferences.remove("ph_mid_std");
            preferences.end();
            saveConfiguration();
            mqttMgr.publish(TOPIC_STATUS, "{\"ph_cal\":\"reset\"}");
        }
    }

    else if (strcmp(topic, TOPIC_CMD_EC) == 0) {
        const char* action = doc["action"] | "";
        if (strcmp(action, "calibrate") != 0) {
            return;
        }
        const char* point = doc["point"] | "";

        if (strcmp(point, "dry") == 0) {
            float v = sensorMgr.sampleEcVoltage();
            if (isnan(v)) {
                mqttMgr.publishError("EC calibration: ADC read failed");
                return;
            }
            sysConfig.ecDryVoltage = v;
            saveConfiguration();
            StaticJsonDocument<160> out;
            out["ec_cal"] = "dry_done";
            out["raw_voltage"] = v;
            String s;
            serializeJson(out, s);
            mqttMgr.publish(TOPIC_STATUS, s);
        }
        else if (strcmp(point, "solution") == 0) {
            float v = sensorMgr.sampleEcVoltage();
            if (isnan(v)) {
                mqttMgr.publishError("EC calibration: ADC read failed");
                return;
            }
            float stdEc = doc["standard"] | 1.413f;
            float dv = v - sysConfig.ecDryVoltage;
            if (dv < 0.001f) {
                mqttMgr.publishError("EC calibration: solution voltage too close to dry baseline");
                return;
            }
            float scale = stdEc / dv;
            sysConfig.ecScale = scale;
            saveConfiguration();
            StaticJsonDocument<224> out;
            out["ec_cal"] = "complete";
            out["cell_constant"] = scale;
            out["raw_voltage"] = v;
            String s;
            serializeJson(out, s);
            mqttMgr.publish(TOPIC_STATUS, s);
        }
        else if (strcmp(point, "reset") == 0) {
            sysConfig.ecDryVoltage = EC_DEFAULT_DRY_VOLTAGE;
            sysConfig.ecScale = EC_DEFAULT_SCALE * EC_CALIBRATION_FACTOR;
            saveConfiguration();
            mqttMgr.publish(TOPIC_STATUS, "{\"ec_cal\":\"reset\"}");
        }
    }

    // Handle environment commands (light toggle, dosing override, fan control)
    else if (strcmp(topic, TOPIC_CMD_ENV) == 0) {
        const char* action = doc["action"] | "unknown";
        LOG_DEBUG("Environment command received: %s", action);
        if (strcmp(action, "light_on") == 0) {
            LOG_DEBUG("Environment command: LIGHT ON");
            envManager.setLightOverride(true);
            mqttMgr.publish(TOPIC_STATUS, "{\"light\":\"on\"}");
        }
        else if (strcmp(action, "light_off") == 0) {
            envManager.setLightOverride(false);
            mqttMgr.publish(TOPIC_STATUS, "{\"light\":\"off\"}");
        }
        else if (strcmp(action, "light_auto") == 0) {
            envManager.clearLightOverride();
            mqttMgr.publish(TOPIC_STATUS, "{\"light\":\"auto\"}");
        }
        /*else if (strcmp(action, "dose_now") == 0) {
            envManager.triggerNutrientDose();
            mqttMgr.publish(TOPIC_STATUS, "{\"nutrient_dose\":\"triggered\"}");
        }*/
        else if (strcmp(action, "fan_on") == 0) {
            envManager.forceFanOn();
            mqttMgr.publish(TOPIC_STATUS, "{\"fan\":\"on\"}");
        }
        else if (strcmp(action, "fan_off") == 0) {
            envManager.forceFanOff();
            mqttMgr.publish(TOPIC_STATUS, "{\"fan\":\"off\"}");
        }
        else if (strcmp(action, "fan_auto") == 0) {
            sysConfig.fanEnabled = true; // Ensure fan control is enabled in config
            mqttMgr.publish(TOPIC_STATUS, "{\"fan\":\"auto\"}");
        }
    }
    else if (strcmp(topic, TOPIC_CMD_TEST) == 0) {
        if (!sysConfig.testCommandsEnabled) {
            LOG_WARN("Test command received but test commands are disabled in config");
            mqttMgr.publishError("Test commands disabled");
            return;
        }
        const char* testType = doc["type"] | "unknown";
        //const char* testRelay = doc["relay"] | "unknown";
        LOG_DEBUG("Test command received: %s", testType);
        if (strcmp(testType, "sensor") == 0) {
            readSensors();
        }
        else if (strcmp(testType, "relay") == 0) {
            int relayId = doc["id"] | -1;
            bool state = doc["state"] | false;
            LOG_DEBUG("Test command: Set relay %d to %s", relayId, state ? "ON" : "OFF");
            if (relayId >= 0 && relayId <= RELAY_MAX) {
                LOG_DEBUG("Executing relay test command: Relay %d -> %s", relayId, state ? "ON" : "OFF");
                relayManager.setRelay(static_cast<RelayID>(relayId), state);
                mqttMgr.publish(TOPIC_STATUS, "{\"test\":\"relay\",\"id\":" + String(relayId) + ",\"state\":" + String(state ? "on" : "off") + "}");
            } else LOG_ERROR("Invalid relay ID for test command: %d", relayId);
        }
        
        else {
            LOG_ERROR("Unknown test type: %s", testType);
        }
    }
}

static void sensorHealthEntryToJson(JsonObject o, const SensorHealthEntry& e) {
    o["enabled"] = e.enabled;
    o["ok"] = e.ok;
    if (!e.ok && e.error && e.error[0] != '\0') {
        o["error"] = e.error;
    }
}

void publishSensorStatusPayload() {
    if (!mqttMgr.isConnected()) {
        return;
    }
    SensorHealthSnapshot h = sensorMgr.getSensorHealthSnapshot();
    StaticJsonDocument<384> doc;
    sensorHealthEntryToJson(doc.createNestedObject("ultrasonic"), h.ultrasonic);
    sensorHealthEntryToJson(doc.createNestedObject("ph"), h.ph);
    sensorHealthEntryToJson(doc.createNestedObject("ec"), h.ec);
    sensorHealthEntryToJson(doc.createNestedObject("temperature"), h.temperature);
    sensorHealthEntryToJson(doc.createNestedObject("air"), h.air);
    String out;
    serializeJson(doc, out);
    mqttMgr.publish(TOPIC_SENSOR_STATUS, out, false);
}

// ============================================================================
// SENSOR FUNCTIONS
// ============================================================================
void readSensors() {
    //delay(100);
    LOG_DEBUG("Reading sensors...");
    
    if (sensorMgr.readAll(sensorData)) {
        LOG_DEBUG("Sensors read successfully");
        systemStatus.errorCount = 0;
        
        // Log critical values
        LOG_INFO(
                "Water: %.1f°C | Air: %.1f°C/%.0f%% | Battery: %.2fV | "
                "Water Level: %.1f%% (%.1f L) (ADC: %.1f) | Reservoir: %.1f cm | Pressure: %.2f hPa",
                sensorData.waterTemp,
                sensorData.airTemp,
                sensorData.humidity,
                sensorData.batteryVoltage,
                sensorData.waterLevelPercent,
                sensorData.waterLevelLitres,
                sensorData.waterLevelADC,
                sensorData.reservoirDistance,
                sensorData.pressure
            );

        // Check for emergency conditions
        if (sensorData.waterTemp > sysConfig.emergencyShutdownTemp) {
            handleEmergency("Water temperature too high");
        }
        if (sensorData.airTemp > sysConfig.airTempMax) {
            mqttMgr.publishError("Air temperature too high");
        }
        if (sensorData.waterLevel < 10.00f) { // Critical water level threshold (10% or 1.5L)
            mqttMgr.publishError("Water level critically low");
        }
    } else {
        LOG_WARN("Sensor read failed");
        systemStatus.errorCount++;
        
        if (systemStatus.errorCount >= MAX_CONSECUTIVE_ERRORS) {
            handleEmergency("Too many sensor errors");
        }
    }
}

void publishTelemetry() {
    if (!mqttMgr.isConnected()) {
        LOG_DEBUG("MQTT not connected, skipping telemetry");
        return;
    }

    publishSensorStatusPayload();

    if (!sensorData.valid) {
        LOG_WARN("Sensor data invalid, skipping publish");
        return;
    }

    LOG_DEBUG("Publishing telemetry...");

    if (mqttMgr.publishSensorData(sensorData)) {
        LOG_DEBUG("Telemetry published successfully");
    } else {
        LOG_WARN("Telemetry publish failed");
    }

    mqttMgr.publishSystemStatus(systemStatus);
}

// ============================================================================
// POWER MANAGEMENT
// ============================================================================
void checkBatteryLevel() {
    if (sensorData.batteryVoltage < sysConfig.batteryCriticalThreshold) {
        LOG_ERROR("CRITICAL: Battery at %.2fV. Solar recovery needed.", sensorData.batteryVoltage);
        handleEmergency("Battery critically low"); 
    } 
    else if (sensorData.batteryVoltage < sysConfig.batteryLowThreshold) {
        // Enter "Economy Mode"
        if (sysConfig.pumpCooldownTime < sysConfig.batteryLowCooldown) {
            sysConfig.pumpCooldownTime = sysConfig.batteryLowCooldown;
            sysConfig.mqttPublishInterval = sysConfig.batteryLowMqttInterval;
            LOG_WARN("Low Power Mode: Cooldown increased to %u s", sysConfig.batteryLowCooldown);
        }
    }
    else if (sensorData.batteryVoltage > (sysConfig.batteryLowThreshold + 0.5)) {
        // BATTERY RECOVERED (with 0.5V hysteresis to prevent flickering)
        if (sysConfig.pumpCooldownTime != sysConfig.defaultPumpCooldown) {
            LOG_INFO("Battery Recovered! Restoring normal operations.");
            sysConfig.pumpCooldownTime = sysConfig.defaultPumpCooldown;
            sysConfig.mqttPublishInterval = sysConfig.defaultMqttInterval;
        }
    }
}

void enterDeepSleep(uint32_t sleepDurationSec) {

    LOG_INFO("Preparing for deep sleep...");

    // Turn off all actuators
    pumpCtrl.turnOff();
    envManager.allOff();

    // Publish offline status
    if (mqttMgr.isConnected()) {

        String payload = String("{\"status\":\"sleeping\",\"duration\":") + 
                         sleepDurationSec + "}";
        mqttMgr.publish(TOPIC_STATUS, payload, true);
        mqttMgr.disconnect();
    }

    // Disconnect WiFi
    wifiMgr.disconnect();

    // Configure wake timer
    esp_sleep_enable_timer_wakeup((uint64_t)sleepDurationSec * 1000000ULL);

    LOG_INFO("Entering deep sleep for %u seconds", sleepDurationSec);

    Serial.flush();
    delay(100);

    // Enter deep sleep
    esp_deep_sleep_start();
}

void enterLightSleep(uint32_t sleepDurationSec) {
    LOG_INFO("Entering light sleep for %u seconds...", sleepDurationSec);
    
    // 1. Tell the WiFi radio it's okay to sleep
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); 
    
    // 2. Set the timer
    esp_sleep_enable_timer_wakeup((uint64_t)sleepDurationSec * 1000000ULL);
    
    Serial.flush();
    
    // 3. Start Light Sleep
    esp_light_sleep_start();
    
    // --- The ESP "pauses" here ---
    
    // 4. Waking up! 
    LOG_INFO("Woke up from light sleep!");
    
    // Restore full power to WiFi
    esp_wifi_set_ps(WIFI_PS_NONE); 
}

// ============================================================================
// ERROR HANDLING
// ============================================================================
void handleEmergency(const char* reason) {
    LOG_ERROR("EMERGENCY: %s", reason);
    systemStatus.state = STATE_EMERGENCY;
    pumpCtrl.emergencyStop();
    envManager.allOff();
    
    // Publish error if possible
    if (mqttMgr.isConnected()) {
        mqttMgr.publishError(reason);
        mqttMgr.publishSystemStatus(systemStatus);
    }
    
    delay(5000);  // Wait for MQTT to transmit
    int32_t sleepTime = sysConfig.sleepDurationSec;
    if (strcmp(reason, "Battery critically low") == 0) {
        LOG_ERROR("HIBERNATING: Battery critical. Will not wake up until manual reset/charge.");
        sleepTime = sysConfig.solarRecoverySleep;   // Set wake timer to 0 or 24 hours
    } 
    else if (strstr(reason, "sensor") != NULL || strstr(reason, "Safety") != NULL) {
        // Any env/sensor reason (Sensor error, Temp too high, etc.)
        LOG_WARN("Environment unsafe. Entering stabilization sleep.");
        sleepTime = sysConfig.envStabilizationSleep; 
    }
    enterDeepSleep(sleepTime);
}

// ============================================================================
// DIAGNOSTICS
// ============================================================================
void updateSystemStatus() {
    systemStatus.uptime = millis();
    systemStatus.wifiState = wifiMgr.getState();
    systemStatus.mqttState = mqttMgr.getState();
    systemStatus.pumpState = pumpCtrl.getState();
}

void printSystemInfo() {
    Serial.println();
    Serial.println("=== SYSTEM INFORMATION ===");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf("Hardware: %s\n", HARDWARE_REVISION);
    Serial.printf("Device: %s\n", SYSTEM_NAME);
    Serial.printf("Chip: %s\n", ESP.getChipModel());
    Serial.printf("CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Flash: %d KB\n", ESP.getFlashChipSize() / 1024);
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("WiFi: %s\n", wifiMgr.isConnected() ? "Connected" : "Disconnected");
    Serial.printf("MQTT: %s\n", mqttMgr.isConnected() ? "Connected" : "Disconnected");
    Serial.printf("Deep Sleep: %s\n", sysConfig.deepSleepEnabled ? "Enabled" : "Disabled");
    Serial.println("=========================");
    Serial.println();
}

// ============================================================================
// OTA UPDATE HANDLER
// ============================================================================

void performOTAUpdate(const char* firmwareUrl, const char* expectedSha256) {
    LOG_INFO("Initializing OTA update process...");
    
    const int statusLed = 2; 
    pinMode(statusLed, OUTPUT);
    // Visual indicator: Fast toggle
    for(int i = 0; i < 10; i++) {
        digitalWrite(statusLed, HIGH); delay(50);
        digitalWrite(statusLed, LOW); delay(50);
    }

    String url = String(firmwareUrl);
    bool isHTTPS = url.startsWith("https:");
    t_httpUpdate_return ret;

    httpUpdate.rebootOnUpdate(false); // Disable auto-reboot so we can validate the partition ourselves

    // 1. Execute Update with Dedicated OTA CA
    if (isHTTPS) {
        WiFiClientSecure secureClient;
        
        #if OTA_VERIFY_CA
            // Full CA chain verification — use with real CAs (Let's Encrypt, AWS, GitHub)
            File otaCertFile = LittleFS.open(OTA_CA_FILENAME, "r");
            if (otaCertFile) {
                caCert = otaCertFile.readString();
                secureClient.setCACert(caCert.c_str());
                otaCertFile.close();
                LOG_INFO("OTA: CA Certificate loaded from %s", OTA_CA_FILENAME);
            } else {
                LOG_ERROR("OTA: CA Cert missing for HTTPS (%s). Aborting.", OTA_CA_FILENAME);
                goto ota_fail_exit;
            }
        #else
            // setInsecure: TLS is used for encryption but CA chain is NOT verified.
            // Firmware integrity is still guaranteed by RSA-SHA256 signature (checked
            // before download) and SHA256 partition check (checked after download).
            // Use this for local/self-signed servers on a trusted LAN.
            // Set OTA_VERIFY_CA true in config.h when hosting on a real CA.
            secureClient.setInsecure();
            LOG_WARN("OTA: TLS transport active, CA chain verification SKIPPED (OTA_VERIFY_CA=false)");
            LOG_WARN("OTA: Firmware integrity enforced via RSA+SHA256 — safe on trusted LAN.");
        #endif
        
        secureClient.setTimeout(15000);
        httpUpdate.onProgress([](int cur, int total) {
            int percent = (cur * 100) / total;
            LOG_INFO("OTA Progress: %d%%", percent);
        });
        ret = httpUpdate.update(secureClient, firmwareUrl);
    } else {
        WiFiClient basicClient;
        LOG_WARN("OTA: Using unencrypted HTTP. Vulnerable to MitM!");
        basicClient.setTimeout(15000);
        httpUpdate.onProgress([](int cur, int total) {
            int percent = (cur * 100) / total;
            LOG_INFO("OTA Progress: %d%%", percent);
        });
        ret = httpUpdate.update(basicClient, firmwareUrl);
    }

    // 2. Process result
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            LOG_ERROR("OTA ERROR (%d): %s", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            goto ota_fail_exit;

        case HTTP_UPDATE_NO_UPDATES:
            LOG_WARN("OTA: Server returned no update.");
            goto ota_fail_exit;

        case HTTP_UPDATE_OK: {
            LOG_INFO("OTA Download complete. Validating partition...");

            const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
            esp_app_desc_t new_app_info;

            if (esp_ota_get_partition_description(update_partition, &new_app_info) != ESP_OK) {
                LOG_ERROR("Could not read partition description!");
                goto ota_fail_exit;
            }

            LOG_INFO("New Firmware: %s (Project: %s)", new_app_info.version, new_app_info.project_name);

            // 3. SHA256 INTEGRITY CHECK (if strict security is enabled)
            // The RSA signature was already verified BEFORE download in handleMQTTMessage.
            // Now we verify the downloaded binary matches the signed hash.
            #if REQUIRE_SIGNED_FIRMWARE
            {
                if (strlen(expectedSha256) == 0) {
                    LOG_ERROR("OTA: No SHA256 provided for verification!");
                    goto ota_fail_exit;
                }

                // Compute SHA256 of the downloaded partition
                uint8_t sha256_result[32];
                if (esp_partition_get_sha256(update_partition, sha256_result) != ESP_OK) {
                    LOG_ERROR("OTA: Failed to compute SHA256 of downloaded partition");
                    goto ota_fail_exit;
                }

                // Convert to hex string for comparison
                char computed_hex[65];
                for (int i = 0; i < 32; i++) {
                    sprintf(computed_hex + (i * 2), "%02x", sha256_result[i]);
                }
                computed_hex[64] = '\0';

                if (strcasecmp(computed_hex, expectedSha256) != 0) {
                    LOG_ERROR("OTA SECURITY FAILURE: SHA256 mismatch!");
                    LOG_ERROR("  Expected: %s", expectedSha256);
                    LOG_ERROR("  Computed: %s", computed_hex);
                    goto ota_fail_exit;
                }
                LOG_INFO("OTA: SHA256 verification PASSED");
            }
            #endif

            // 4. All checks passed — commit the update
            LOG_INFO("Validation PASSED. Setting boot partition and rebooting...");
            esp_err_t err = esp_ota_set_boot_partition(update_partition);
            if (err != ESP_OK) {
                LOG_ERROR("Failed to set boot partition: %d", err);
                goto ota_fail_exit;
            }
            delay(1000);
            ESP.restart();
            break;
        }
    }
    return;

ota_fail_exit:
    digitalWrite(statusLed, LOW);
    systemStatus.state = STATE_ACTIVE;
    LOG_INFO("OTA process aborted. Resuming normal operation...");
    if (WiFi.status() == WL_CONNECTED) {
        setupMQTT();
        mqttMgr.publishError("OTA Update Failed - Check Logs");
    }
}

void checkRollback() {
    const esp_partition_t* running_partition = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running_partition, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            LOG_WARN("Pending OTA detected. Marking as valid to prevent rollback.");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    } else {
        LOG_ERROR("Failed to get OTA state for rollback check");
    }
}

void performFactoryReset() {
    LOG_WARN("Performing factory reset...");
    preferences.begin("hydro-config", false);
    preferences.clear();
    preferences.end();
    LOG_INFO("Factory reset complete. Restarting...");
    delay(1000);
    ESP.restart();
}

void loadConfiguration() {
    preferences.end();
    saveConfiguration();
    preferences.begin("hydro-config", false); 

    // Fix: Check for the newest float keys, not just the old 'sleep_en' key
    /*if (!preferences.isKey("sleep_en") || !preferences.isKey("air_t_max") || !preferences.isKey("fan_hyst")) {
        LOG_WARN("Missing config keys detected in Flash. Saving complete defaults...");
        preferences.end();
        saveConfiguration(); 
        preferences.begin("hydro-config", false);
    }*/

    sysConfig.deepSleepEnabled = preferences.getBool("sleep_en", sysConfig.deepSleepEnabled);
    sysConfig.sleepDurationSec = preferences.getUInt("sleep_sec", sysConfig.sleepDurationSec);
    sysConfig.activeDurationMs = preferences.getUInt("active_dur", sysConfig.activeDurationMs);
    sysConfig.sensorReadInterval = preferences.getUInt("read_int", sysConfig.sensorReadInterval);
    sysConfig.mqttPublishInterval = preferences.getUInt("pub_int", sysConfig.mqttPublishInterval);
    sysConfig.pumpMaxOnTime = preferences.getUInt("pump_max", sysConfig.pumpMaxOnTime);
    sysConfig.pumpCooldownTime = preferences.getUInt("pump_cool", sysConfig.pumpCooldownTime);
    sysConfig.batteryCriticalThreshold = preferences.getFloat("batt_crit", sysConfig.batteryCriticalThreshold);
    sysConfig.emergencyShutdownTemp = preferences.getFloat("temp_shut", sysConfig.emergencyShutdownTemp);
    sysConfig.envStabilizationSleep = preferences.getUInt("env_stab_s", sysConfig.envStabilizationSleep);
    sysConfig.testCommandsEnabled = preferences.getBool("test_cmds", sysConfig.testCommandsEnabled);
    // Environment control
    sysConfig.fanEnabled = preferences.getBool("fan_en", sysConfig.fanEnabled);
    sysConfig.airTempMax = preferences.getFloat("air_t_max", sysConfig.airTempMax);
    sysConfig.fanHysteresis = preferences.getFloat("fan_hyst", sysConfig.fanHysteresis);
    sysConfig.dosingEnabled = preferences.getBool("dose_en", sysConfig.dosingEnabled);
    sysConfig.dosingPulseMs = preferences.getUInt("dose_pulse", sysConfig.dosingPulseMs);
    sysConfig.dosingLockoutMs = preferences.getUInt("dose_lock", sysConfig.dosingLockoutMs);
    sysConfig.lightingEnabled = preferences.getBool("light_en", sysConfig.lightingEnabled);
    sysConfig.lightOnHour = preferences.getUChar("light_on_h", sysConfig.lightOnHour);
    sysConfig.lightOffHour = preferences.getUChar("light_off_h", sysConfig.lightOffHour);
    sysConfig.nutrientDosingEnabled = preferences.getBool("nut_en", sysConfig.nutrientDosingEnabled);
    sysConfig.nutrientDoseDelayMs = preferences.getUInt("nut_delay", sysConfig.nutrientDoseDelayMs);

    sysConfig.phSlope = preferences.getFloat("ph_slope", sysConfig.phSlope);
    sysConfig.phOffset = preferences.getFloat("ph_off", sysConfig.phOffset);
    sysConfig.ecDryVoltage = preferences.getFloat("ec_dry_v", sysConfig.ecDryVoltage);
    sysConfig.ecScale = preferences.getFloat("ec_scale", sysConfig.ecScale);

    preferences.end();
    
    // SYNC DEFAULTS
    sysConfig.defaultPumpCooldown = sysConfig.pumpCooldownTime;
    sysConfig.defaultMqttInterval = sysConfig.mqttPublishInterval;
    LOG_INFO("Configuration loaded from Flash");
}

void saveConfiguration() {
    preferences.begin("hydro-config", false);
    preferences.putBool("sleep_en", sysConfig.deepSleepEnabled);
    preferences.putUInt("sleep_sec", sysConfig.sleepDurationSec);
    preferences.putUInt("active_dur", sysConfig.activeDurationMs);
    preferences.putUInt("read_int", sysConfig.sensorReadInterval);
    preferences.putUInt("pub_int", sysConfig.mqttPublishInterval);
    preferences.putUInt("pump_max", sysConfig.pumpMaxOnTime);
    preferences.putUInt("pump_cool", sysConfig.pumpCooldownTime);
    preferences.putFloat("batt_crit", sysConfig.batteryCriticalThreshold);
    preferences.putFloat("temp_shut", sysConfig.emergencyShutdownTemp);
    preferences.putUInt("env_stab_s", sysConfig.envStabilizationSleep);
    preferences.putBool("test_cmds", sysConfig.testCommandsEnabled);

    // Environment control
    preferences.putBool("fan_en", sysConfig.fanEnabled);
    preferences.putFloat("air_t_max", sysConfig.airTempMax);
    preferences.putFloat("fan_hyst", sysConfig.fanHysteresis);
    preferences.putBool("dose_en", sysConfig.dosingEnabled);
    preferences.putUInt("dose_pulse", sysConfig.dosingPulseMs);
    preferences.putUInt("dose_lock", sysConfig.dosingLockoutMs);
    preferences.putBool("light_en", sysConfig.lightingEnabled);
    preferences.putUChar("light_on_h", sysConfig.lightOnHour);
    preferences.putUChar("light_off_h", sysConfig.lightOffHour);
    preferences.putBool("nut_en", sysConfig.nutrientDosingEnabled);
    preferences.putUInt("nut_delay", sysConfig.nutrientDoseDelayMs);

    preferences.putFloat("ph_slope", sysConfig.phSlope);
    preferences.putFloat("ph_off", sysConfig.phOffset);
    preferences.putFloat("ec_dry_v", sysConfig.ecDryVoltage);
    preferences.putFloat("ec_scale", sysConfig.ecScale);

    preferences.end();
    LOG_INFO("Configuration saved to Flash");
}

void validateConfiguration() {
    bool adjusted = false;
    String warningMsg = "";

    // Check 1: Wake time vs Pump time
    if (sysConfig.activeDurationMs < sysConfig.pumpMaxOnTime) {
        sysConfig.activeDurationMs = sysConfig.pumpMaxOnTime + 5000; // 5s buffer
        warningMsg = "Active duration increased to " + String(sysConfig.activeDurationMs) + "ms to cover pump runtime.";
        adjusted = true;
    }

    // Check 2: (Optional but recommended) Ensure pump cooldown isn't 0
    if (sysConfig.pumpCooldownTime < 1000) {
        sysConfig.pumpCooldownTime = 60000; // Default to 1 min if set too low
        warningMsg += " | Cooldown too low, reset to 60s.";
        adjusted = true;
    }

    if (adjusted) {
        LOG_WARN("%s", warningMsg.c_str());
        
        // Send to MQTT so the user sees it in their dashboard/app
        if (mqttMgr.isConnected()) {
            StaticJsonDocument<256> doc;
            doc["type"] = "config_warning";
            doc["message"] = warningMsg;
            doc["active_dur"] = sysConfig.activeDurationMs;
            
            char buffer[256];
            serializeJson(doc, buffer);
            mqttMgr.publish(TOPIC_STATUS, buffer); 
        }
        
        // Save the corrected values back to Flash so they persist
        saveConfiguration();
    }
}
