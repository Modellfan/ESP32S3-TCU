#pragma once

#include <Arduino.h>

#include "TCallA7670Modem.h"

namespace tcall {

struct BufferedGpsTime {
  bool valid = false;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint32_t updatedMs = 0;
};

struct BufferedGpsInfo {
  bool valid = false;
  bool hasFix = false;
  uint8_t fixMode = 0;
  double latitude = 0;
  double longitude = 0;
  char ns = '\0';
  char ew = '\0';
  float altitudeMeters = 0;
  float speedMps = 0;
  float courseDegrees = 0;
  uint16_t gpsSatellites = 0;
  uint16_t beidouSatellites = 0;
  uint16_t glonassSatellites = 0;
  uint16_t galileoSatellites = 0;
  uint16_t totalSatellites = 0;
  float pdop = 0;
  float hdop = 0;
  float vdop = 0;
  BufferedGpsTime utc;
  uint32_t updatedMs = 0;
};

class GpsBufferedService {
 public:
  explicit GpsBufferedService(TCallA7670Modem& modem);

  void begin(uint32_t pollIntervalMs = 5000, uint32_t enableRetryMs = 30000);
  void stop();
  void runner(Stream* log = nullptr);

  BufferedGpsTime getTime() const;
  BufferedGpsInfo getGpsInformation() const;
  bool started() const;
  bool gpsPowerSeen() const;
  bool lastPollOk() const;
  uint32_t lastPollAttemptMs() const;
  uint32_t lastFixMs() const;
  bool fixEverSeen() const;

 private:
  void cacheGpsInfo(const GPSInfo& info);

  TCallA7670Modem& modem_;
  bool started_ = false;
  bool gpsPowerSeen_ = false;
  bool lastPollOk_ = false;
  bool enableAttempted_ = false;
  bool enableRequestActive_ = false;
  bool hotStartRequested_ = false;
  bool fixEverSeen_ = false;
  uint32_t pollIntervalMs_ = 5000;
  uint32_t enableRetryMs_ = 30000;
  uint32_t startedMs_ = 0;
  uint32_t nextPollMs_ = 0;
  uint32_t nextEnableCheckMs_ = 0;
  uint32_t lastPollAttemptMs_ = 0;
  uint32_t lastFixMs_ = 0;
  uint32_t lastColdStartMs_ = 0;
  BufferedGpsTime time_;
  BufferedGpsInfo gps_;
};

}  // namespace tcall
