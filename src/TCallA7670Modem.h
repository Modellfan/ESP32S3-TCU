#pragma once

#include <Arduino.h>

#include <TinyGsmClient.h>

#include "TCallConfig.h"

namespace tcall {

constexpr int MODEM_DTR_PIN = 14;
constexpr int MODEM_TX_PIN = 26;
constexpr int MODEM_RX_PIN = 25;
constexpr int BOARD_PWRKEY_PIN = 4;
constexpr int BOARD_LED_PIN = 12;
constexpr int MODEM_RING_PIN = 13;
constexpr int MODEM_RESET_PIN = 27;
constexpr int MODEM_RESET_LEVEL = LOW;
constexpr int MODEM_GPS_ENABLE_GPIO = -1;
constexpr int MODEM_GPS_ENABLE_LEVEL = -1;

constexpr uint32_t MODEM_BAUD = 115200;
constexpr uint32_t CONSOLE_BAUD = 115200;
constexpr uint32_t MODEM_POWERON_PULSE_MS = 100;
constexpr uint32_t MODEM_START_WAIT_MS = 3000;

constexpr const char* DEFAULT_APN = TCALL_APN;
constexpr const char* DEFAULT_APN_USER = TCALL_APN_USER;
constexpr const char* DEFAULT_APN_PASS = TCALL_APN_PASS;
constexpr const char* DEFAULT_SIM_PIN = TCALL_SIM_PIN;

struct ApnProfile {
  const char* name;
  const char* apn;
  const char* user;
  const char* pass;
};

struct GnssFix {
  uint8_t fixMode = 0;
  float latitude = 0;
  float longitude = 0;
  float speed = 0;
  float altitude = 0;
  int visibleSatellites = 0;
  int usedSatellites = 0;
  float accuracy = 0;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

class TCallA7670Modem {
 public:
  void begin(Stream& log);
  bool restart(Stream& log, uint32_t onlineTimeoutMs = 45000);
  bool waitOnline(uint32_t timeoutMs, Stream& log);
  String rawAt(const String& command, uint32_t timeoutMs = 3000);

  void printDiagnostics(Stream& out, const char* phase);
  void printSimStatus(Stream& out);
  void printRegistration(Stream& out);
  void printRegistrationSnapshot(Stream& out, const char* phase);

  bool waitForSimReady(uint32_t timeoutMs, Stream& log);
  bool disableSimPin(Stream& log);
  bool prepareForCellular(Stream& log);
  bool configureApn(Stream& log);
  bool configureApn(const char* apn, Stream& log);
  bool selectOperatorAuto(Stream& log);
  bool selectOperatorTelekom(Stream& log);
  bool configureRat(bool lteOnly, Stream& log);
  bool configureRat(bool lteOnly, const char* apn, Stream& log);
  bool waitForEpsRegistration(uint32_t timeoutMs, Stream& log);
  bool activateData(const char* apn = DEFAULT_APN,
                    const char* user = DEFAULT_APN_USER,
                    const char* pass = DEFAULT_APN_PASS,
                    Stream* log = nullptr,
                    uint8_t attempts = 3);
  bool deactivateData();
  bool dataActive();
  String localIP();
  bool httpGet(const char* host, const char* path, uint16_t port, Stream& out);
  String listSms(const char* status = "ALL");
  bool sendSms(const char* number, const String& message, Stream& out);

  bool enableGps(Stream& log);
  bool startGpsEnableRequest();
  bool pollGpsEnableRequest(bool& done);
  bool disableGps();
  bool gpsEnabled();
  bool gpsHotStart();
  bool gpsColdStart();
  String gpsRaw();
  bool gpsFix(GnssFix& fix);
  bool gpsExtended(GPSInfo& info);
  bool modemHasBuiltInGps(const String& modemName) const;

 private:
  void resetModem();
  void powerOnModem();
  int ceregStat();
  void printAt(Stream& out, const char* cmd, uint32_t timeoutMs = 3000);
};

const char* simStatusName(SimStatus status);
const char* regStatusName(RegStatus status);

}  // namespace tcall
