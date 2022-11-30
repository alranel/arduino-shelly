/*

Arduino firmware for Shelly Plug S

Copyright (C) 2022 Alessandro Ranellucci

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <ArduinoIoTCloud.h>
#include <Arduino_ConnectionHandler.h>
#include <Button2.h>
#include <EEPROM.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WiFi.h>
#include "HLW8012.h"
#include <WiFiManager.h>

// Pins of the Shelly device
constexpr int SHELLY_LED = 0;
constexpr int SHELLY_LINK_LED = 2;
constexpr int SHELLY_BUTTON = 13;
constexpr int SHELLY_RELAY = 15;
constexpr int SHELLY_BL0937_CF = 5;
constexpr int SHELLY_BL0937_CF1 = 14;
constexpr int SHELLY_BL0937_SEL = 12;

// Data structure for cloud credentials to store in EEPROM
struct stored_config_t {
  char prefix[7+1];
  char DEVICE_ID[36+1];
  char SECRET_KEY[20+1];
  double current_multiplier, voltage_multiplier, power_multiplier;
} stored_config;

// Various global variables
char SSID[32+1];
char PASS[63+1];
Button2 button;
HLW8012 hlw8012;
WiFiManager wm;
WiFiManagerParameter* wm_device_id = nullptr;
WiFiManagerParameter* wm_secret_key = nullptr;
WiFiConnectionHandler* conn = nullptr;

// Cloud variables
String cmd;
bool relay;
float active_power, voltage, current;

void ICACHE_RAM_ATTR hlw8012_cf_interrupt() {
  hlw8012.cf_interrupt();
}
void ICACHE_RAM_ATTR hlw8012_cf1_interrupt() {
  hlw8012.cf1_interrupt();
}

void setup() {
  Serial.begin(9600);
  Serial.println("*** Arduino firmware for Shelly Plug S ***");
  cmd = "";

  // Init pins
  pinMode(SHELLY_LED, OUTPUT);
  pinMode(SHELLY_LINK_LED, OUTPUT);
  pinMode(SHELLY_BUTTON, INPUT_PULLUP);
  pinMode(SHELLY_RELAY, OUTPUT);
  relay = false;
  updateRelay();
  
  // Initialize power monitoring
  hlw8012.begin(SHELLY_BL0937_CF, SHELLY_BL0937_CF1, SHELLY_BL0937_SEL, HIGH, true);
  hlw8012.setResistors(0.001, 5 * 470000, 1000);
  attachInterrupt(digitalPinToInterrupt(SHELLY_BL0937_CF), hlw8012_cf_interrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(SHELLY_BL0937_CF1), hlw8012_cf1_interrupt, CHANGE);

  // Read config from EEPROM and check whether it was initialized
  EEPROM.begin(sizeof stored_config);
  EEPROM.get(0, stored_config);
  if (strcmp(stored_config.prefix, "arduino") != 0) {
    Serial.println("Memory not initialized");
    strcpy(stored_config.prefix, "arduino");
    memset(stored_config.DEVICE_ID, 0, sizeof stored_config.DEVICE_ID);
    memset(stored_config.SECRET_KEY, 0, sizeof stored_config.SECRET_KEY);
    resetPowerMonitoring();
  }

  // Apply power monitoring multipliers
  updatePowerMonitoring();

  // Check WiFi connection and enter access point if no WiFi is available
  {    
    wm_device_id = new WiFiManagerParameter("device_id", "Arduino Cloud - Device ID", stored_config.DEVICE_ID, sizeof stored_config.DEVICE_ID);
    wm_secret_key = new WiFiManagerParameter("secret_key", "Arduino Cloud - Secret Key", stored_config.SECRET_KEY, sizeof stored_config.SECRET_KEY);
    wm.addParameter(wm_device_id);
    wm.addParameter(wm_secret_key);
    
    wm.setDisableConfigPortal(true);
    wm.setBreakAfterConfig(true);

    // If button is pressed at startup for more than 5 seconds, launch the config portal
    bool forcePortal = false;
    if (!digitalRead(SHELLY_BUTTON)) {
      delay(5000);
      if (!digitalRead(SHELLY_BUTTON)) {
        Serial.println("Button was pressed for 5 secs; enabling access point mode");
        forcePortal = true;
      }      
    }

    String APname = String("Arduino-") + WiFi.macAddress();
    bool res;
    if (forcePortal || strlen(stored_config.DEVICE_ID) == 0) {
      // startConfigPortal() does not try to connect using the stored WiFi credentials
      // before launching the portal. If the stored credentials are not changed by the user,
      // it will not try to connect after config is saved. For this reason, we're still
      // calling autoConnect() afterwards to force connection.
      wm.startConfigPortal(APname.c_str(), "");
    }
    res = wm.autoConnect(APname.c_str(), "");

    if (strlen(wm_device_id->getValue()) > 0) {
      strcpy(stored_config.DEVICE_ID, wm_device_id->getValue());
      strcpy(stored_config.SECRET_KEY, wm_secret_key->getValue());
    }

    saveConfig();

    if (!res) {
      Serial.println("Failed to connect to WiFi; rebooting");
      delay(3000);
      ESP.restart();      
    }
  }

  Serial.println("Connection to WiFi succeeded! Connecting to cloud now...");

  // Get WiFi configuration
  strcpy(SSID, wm.getWiFiSSID().c_str());
  strcpy(PASS, wm.getWiFiPass().c_str());

  // Connect to Arduino IoT Cloud
  conn = new WiFiConnectionHandler(SSID, PASS);
  ArduinoCloud.setBoardId(stored_config.DEVICE_ID);
  ArduinoCloud.setSecretDeviceKey(stored_config.SECRET_KEY);
  ArduinoCloud.addProperty(cmd, READWRITE, ON_CHANGE, handleCmd);
  ArduinoCloud.addProperty(relay, READWRITE, ON_CHANGE, updateRelay);
  ArduinoCloud.addProperty(active_power, READ, 1 * SECONDS);
  ArduinoCloud.addProperty(voltage, READ, 1 * SECONDS);
  ArduinoCloud.addProperty(current, READ, 1 * SECONDS);
  ArduinoCloud.begin(*conn);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();

  // Set up button handler
  button.begin(SHELLY_BUTTON);
  button.setTapHandler([](Button2& btn) {
    relay = !relay;
    updateRelay();
  });
}

void loop() {
  ArduinoCloud.update();
  button.loop();

  // If we're not connected to the cloud, blink the LED
  if (!ArduinoCloud.connected()) {
    digitalWrite(SHELLY_LINK_LED, millis() % 500 > 250);
  } else {
    digitalWrite(SHELLY_LINK_LED, LOW);    
  }

  // Update power monitoring values.  
  static unsigned long last = millis();
  if ((millis() - last) > 5000) {
      active_power  = hlw8012.getActivePower();
      voltage       = hlw8012.getVoltage();
      current       = hlw8012.getCurrent();
      last          = millis();
  }
}

void updateRelay() {
  digitalWrite(SHELLY_RELAY, relay);
  digitalWrite(SHELLY_LED, !relay);
}

void nonBlockingDelay(unsigned long mseconds) {
    unsigned long timeout = millis();
    while ((millis() - timeout) < mseconds) delay(1);
}

void resetPowerMonitoring() {
  Serial.println("Resetting power monitoring multipliers");
  stored_config.current_multiplier = 11000.0f;
  stored_config.voltage_multiplier = 150000.0f;
  stored_config.power_multiplier   = 3100000.0f;
}

void updatePowerMonitoring() {
  hlw8012.setCurrentMultiplier(stored_config.current_multiplier);
  hlw8012.setVoltageMultiplier(stored_config.voltage_multiplier);
  hlw8012.setPowerMultiplier(stored_config.power_multiplier);
}

void handleCmd() {
  char cmd_buf[255];
  char* token;
  strcpy(cmd_buf, cmd.c_str());
  const char* cmd0 = strtok(cmd_buf, " ");
  if (cmd0 == nullptr) return;
  Serial.print("Remote command received: ");
  Serial.println(cmd0);

  if (strcmp(cmd0, "version") == 0) {
    cmd = __DATE__ " " __TIME__;
  } else if (strcmp(cmd0, "ota") == 0) {
    const char* url = strtok(nullptr, " ");
    if (url == nullptr) return;

    // Trick: this is actually a workaround for a IoT Cloud bug that will
    // trigger OTA forever if the last command issued by user is "ota"
    cmd = "Starting OTA. Now type 'ok' to prevent infinite loop";
    ArduinoCloud.update();

    Serial.print("OTA request received; URL = ");
    Serial.println(url);
    Serial.println("Attempting OTA...");
    ESPhttpUpdate.setLedPin(SHELLY_LINK_LED, HIGH);
    WiFiClient client;
    auto ret = ESPhttpUpdate.update(client, url);
    if (ret == HTTP_UPDATE_FAILED) {
      char buffer[255];
      sprintf(buffer, "HTTP_UPDATE_FAILED Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      cmd = buffer;
    }
  } else if (strcmp(cmd0, "reset_power_monitoring") == 0) {
    resetPowerMonitoring();
    saveConfig();
    updatePowerMonitoring();
    cmd = "Resetting multipliers to default values";
  } else if (strcmp(cmd0, "calibrate") == 0) {
    token = strtok(nullptr, " ");
    if (token == nullptr) return;
    const float expected_active_power = atof(token);

    token = strtok(nullptr, " ");
    if (token == nullptr) return;
    const float expected_voltage = atof(token); 

    float expected_current = 0;
    token = strtok(nullptr, " ");
    if (token != nullptr) {
      expected_current = atof(token);      
    }
    
    if (expected_active_power == 0 || expected_voltage == 0) {
      cmd = "Invalid input";
      return;
    }

    // Apply load
    bool previousRelayState = relay;
    relay = true;
    updateRelay();
    delay(1000);  // Let the power stabilize        

    hlw8012.getActivePower();
    hlw8012.setMode(MODE_CURRENT);
    nonBlockingDelay(2000);
    hlw8012.getCurrent();
    hlw8012.setMode(MODE_VOLTAGE);
    nonBlockingDelay(2000);
    hlw8012.getVoltage();

    // This assumes calibration is performed with a pure resistive load like a lightbulb     
    hlw8012.expectedActivePower(expected_active_power);
    hlw8012.expectedVoltage(expected_voltage);
    if (expected_current > 0) {
      hlw8012.expectedCurrent(expected_current);
    } else {
      hlw8012.expectedCurrent(expected_active_power / expected_voltage);
    }

    stored_config.current_multiplier = hlw8012.getCurrentMultiplier();
    stored_config.voltage_multiplier = hlw8012.getVoltageMultiplier();
    stored_config.power_multiplier = hlw8012.getPowerMultiplier();
    saveConfig();

    relay = previousRelayState;
    updateRelay();

    cmd = "Calibration done";
  } else if (strcmp(cmd0, "set_current_multiplier") == 0) {
    char* token = strtok(nullptr, " ");
    if (token == nullptr) return;
    const float val = atof(token);
    if (val > 0) {
      hlw8012.setCurrentMultiplier(stored_config.current_multiplier = val);
      saveConfig();
      cmd = "Current multiplier saved";
    } else {
      cmd = "Invalid input";
    }
  } else if (strcmp(cmd0, "set_voltage_multiplier") == 0) {
    char* token = strtok(nullptr, " ");
    if (token == nullptr) return;
    const float val = atof(token);
    if (val > 0) {
      hlw8012.setVoltageMultiplier(stored_config.voltage_multiplier = val);
      saveConfig();
      cmd = "Voltage multiplier saved";
    } else {
      cmd = "Invalid input";
    }
  } else if (strcmp(cmd0, "set_power_multiplier") == 0) {
    char* token = strtok(nullptr, " ");
    if (token == nullptr) return;
    const float val = atof(token);
    if (val > 0) {
      hlw8012.setPowerMultiplier(stored_config.power_multiplier = val);
      saveConfig();
      cmd = "Power multiplier saved";
    } else {
      cmd = "Invalid input";
    }
  } else if (strcmp(cmd0, "get_current_multiplier") == 0) {
    cmd = String(hlw8012.getCurrentMultiplier());
  } else if (strcmp(cmd0, "get_voltage_multiplier") == 0) {
    cmd = String(hlw8012.getVoltageMultiplier());
  } else if (strcmp(cmd0, "get_power_multiplier") == 0) {
    cmd = String(hlw8012.getPowerMultiplier());
  }
}

void saveConfig() {
  EEPROM.put(0, stored_config);
  EEPROM.commit();
}



