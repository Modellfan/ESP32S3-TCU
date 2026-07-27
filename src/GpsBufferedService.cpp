#include "GpsBufferedService.h"

#include <math.h>

namespace {

constexpr float KNOTS_TO_METERS_PER_SECOND = 0.514444F;
constexpr uint32_t FIRST_COLD_START_AFTER_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t COLD_START_RETRY_MS = 15UL * 60UL * 1000UL;

bool validUtc(const tcall::BufferedGpsTime& time)
{
  return time.year >= 2020 && time.month >= 1 && time.month <= 12 &&
         time.day >= 1 && time.day <= 31 && time.hour <= 23 &&
         time.minute <= 59 && time.second <= 59;
}

}  // namespace

namespace tcall {

GpsBufferedService::GpsBufferedService(TCallA7670Modem& modem) : modem_(modem) {}

void GpsBufferedService::begin(uint32_t pollIntervalMs, uint32_t enableRetryMs)
{
  pollIntervalMs_ = pollIntervalMs;
  enableRetryMs_ = enableRetryMs;
  startedMs_ = millis();
  nextPollMs_ = 0;
  nextEnableCheckMs_ = 0;
  lastPollAttemptMs_ = 0;
  lastColdStartMs_ = 0;
  hotStartRequested_ = false;
  enableAttempted_ = false;
  enableRequestActive_ = false;
  started_ = true;
}

void GpsBufferedService::stop()
{
  started_ = false;
  gpsPowerSeen_ = false;
  lastPollOk_ = false;
  enableAttempted_ = false;
  enableRequestActive_ = false;
  hotStartRequested_ = false;
  lastPollAttemptMs_ = 0;
  lastColdStartMs_ = 0;
}

void GpsBufferedService::runner(Stream* log)
{
  if (!started_) {
    return;
  }

  const uint32_t now = millis();
  if (!gpsPowerSeen_ && static_cast<int32_t>(now - nextEnableCheckMs_) >= 0) {
    if (enableRequestActive_) {
      bool done = false;
      gpsPowerSeen_ = modem_.pollGpsEnableRequest(done);
      if (!done) {
        return;
      }
      enableRequestActive_ = false;
      if (!gpsPowerSeen_) {
        enableAttempted_ = false;
        nextEnableCheckMs_ = now + enableRetryMs_;
        return;
      }
    } else {
      gpsPowerSeen_ = modem_.gpsEnabled();
    }

    if (!gpsPowerSeen_ && !enableAttempted_) {
      enableAttempted_ = true;
      enableRequestActive_ = modem_.startGpsEnableRequest();
      if (log) {
        (*log).println("GNSS enable requested.");
      }
      nextEnableCheckMs_ = now + 250;
      return;
    }

    nextEnableCheckMs_ = gpsPowerSeen_ ? now + enableRetryMs_ : now + 250;
  }

  if (!gpsPowerSeen_ || static_cast<int32_t>(now - nextPollMs_) < 0) {
    return;
  }

  if (!hotStartRequested_) {
    modem_.gpsHotStart();
    hotStartRequested_ = true;
    if (log) {
      (*log).println("GNSS hot start requested.");
    }
  }

  if (!gps_.hasFix && (now - startedMs_) >= FIRST_COLD_START_AFTER_MS &&
      (lastColdStartMs_ == 0 ||
       (now - lastColdStartMs_) >= COLD_START_RETRY_MS)) {
    if (modem_.gpsColdStart()) {
      lastColdStartMs_ = now;
      if (log) {
        (*log).println("GNSS cold start requested after extended no-fix period.");
      }
    }
  }

  nextPollMs_ = now + pollIntervalMs_;
  lastPollAttemptMs_ = now;

  GPSInfo info;
  lastPollOk_ = modem_.gpsExtended(info);
  if (lastPollOk_) {
    cacheGpsInfo(info);
  }
}

BufferedGpsTime GpsBufferedService::getTime() const
{
  return time_;
}

BufferedGpsInfo GpsBufferedService::getGpsInformation() const
{
  return gps_;
}

bool GpsBufferedService::gpsPowerSeen() const
{
  return gpsPowerSeen_;
}

bool GpsBufferedService::started() const
{
  return started_;
}

bool GpsBufferedService::lastPollOk() const
{
  return lastPollOk_;
}

uint32_t GpsBufferedService::lastPollAttemptMs() const
{
  return lastPollAttemptMs_;
}

uint32_t GpsBufferedService::lastFixMs() const
{
  return lastFixMs_;
}

bool GpsBufferedService::fixEverSeen() const
{
  return fixEverSeen_;
}

void GpsBufferedService::cacheGpsInfo(const GPSInfo& info)
{
  const uint32_t now = millis();

  time_.year = info.year;
  time_.month = info.month;
  time_.day = info.day;
  time_.hour = info.hour;
  time_.minute = info.minute;
  time_.second = info.second;
  time_.updatedMs = now;
  time_.valid = validUtc(time_);

  gps_.fixMode = info.isFix;
  gps_.latitude = info.latitude;
  gps_.longitude = info.longitude;
  gps_.ns = info.NS_indicator;
  gps_.ew = info.EW_indicator;
  gps_.altitudeMeters = info.altitude;
  gps_.speedMps = info.speed * KNOTS_TO_METERS_PER_SECOND;
  gps_.courseDegrees = info.course;
  gps_.gpsSatellites = info.gps_satellite_num;
  gps_.beidouSatellites = info.beidou_satellite_num;
  gps_.glonassSatellites = info.glonass_satellite_num;
  gps_.galileoSatellites = info.galileo_satellite_num;
  gps_.totalSatellites = gps_.gpsSatellites + gps_.beidouSatellites +
                         gps_.glonassSatellites + gps_.galileoSatellites;
  gps_.pdop = info.PDOP;
  gps_.hdop = info.HDOP;
  gps_.vdop = info.VDOP;
  gps_.utc = time_;
  gps_.updatedMs = now;
  gps_.valid = true;
  gps_.hasFix = (gps_.fixMode == 2 || gps_.fixMode == 3) &&
                fabs(gps_.latitude) > 0.000001 &&
                fabs(gps_.longitude) > 0.000001 &&
                gps_.totalSatellites > 0;
  if (gps_.hasFix) {
    fixEverSeen_ = true;
    lastFixMs_ = now;
  }
}

}  // namespace tcall
