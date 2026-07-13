#pragma once

// Copy this file to src/LocalConfig.h and fill in local/private values.
// src/LocalConfig.h is ignored by git and must not be committed.

#define TCALL_WIFI_SSID "your-wifi-ssid"
#define TCALL_WIFI_PASSWORD "your-wifi-password"
#define TCALL_OTA_HOSTNAME "tcall-a7670-v10"

#define TCALL_SIM_PIN ""

#define TCALL_APN "your-apn"
#define TCALL_APN_USER "your-apn-user"
#define TCALL_APN_PASS "your-apn-password"
#define TCALL_PDP_TYPE "IP"

#define TCALL_MQTT_HOST "192.168.1.10"
#define TCALL_MQTT_TRANSPORT "wifi"  // "wifi" or "cellular"
#define TCALL_MQTT_PORT 1883
#define TCALL_MQTT_USER ""
#define TCALL_MQTT_PASS ""
#define TCALL_MQTT_CLIENT_ID "tcall-a7670-v10"
#define TCALL_MQTT_TOPIC_PREFIX "tcall/a7670/v10"
#define TCALL_MQTT_PUBLISH_INTERVAL_MS 10000UL

#define TCALL_GPS_AUTOSTART 0

// GitHub latest-release LTE OTA demo. Publish a release with a firmware .bin asset
// and a CRC sidecar containing the expected 8-digit hex CRC32, for example:
//   firmware.bin
//   firmware.bin.crc32
#define TCALL_GITHUB_OTA_OWNER "your-github-owner"
#define TCALL_GITHUB_OTA_REPO "your-github-repo"
#define TCALL_GITHUB_OTA_BIN_ASSET ""  // Empty selects the first .bin release asset.
#define TCALL_GITHUB_OTA_CRC_ASSET ""  // Empty selects <firmware>.crc32 or <firmware>.crc.
#define TCALL_GITHUB_OTA_USER_AGENT "ESP32S3-TCU-LTE-OTA-Demo"
#define TCALL_GITHUB_OTA_REBOOT_AFTER_UPDATE 1
