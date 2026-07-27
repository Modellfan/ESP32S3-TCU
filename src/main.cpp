#include <Arduino.h>

#include <ArduinoOTA.h>
#include <RemoteDeviceManager.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_sleep.h>
#include <math.h>
#include <string.h>

#include "GitHubOtaDemo.h"
#include "GpsBufferedService.h"
#include "TCallA7670Modem.h"

namespace {

constexpr const char* WIFI_SSID = TCALL_WIFI_SSID;
constexpr const char* WIFI_PASSWORD = TCALL_WIFI_PASSWORD;
constexpr const char* OTA_HOSTNAME = TCALL_OTA_HOSTNAME;
constexpr const char* DEVICE_ID = TCALL_DEVICE_ID;
constexpr const char* MQTT_HOST = TCALL_MQTT_HOST;
constexpr const char* MQTT_CELLULAR_HOST = TCALL_MQTT_CELLULAR_HOST;
constexpr uint16_t MQTT_CELLULAR_PORT = TCALL_MQTT_CELLULAR_PORT;
constexpr bool MQTT_CELLULAR_NATIVE = TCALL_MQTT_CELLULAR_NATIVE != 0;
constexpr bool RDM_LTE_ALIVE_AUTOSTART = TCALL_RDM_LTE_ALIVE_AUTOSTART != 0;
constexpr bool RDM_STANDBY_ENABLED = TCALL_RDM_STANDBY_ENABLED != 0;
constexpr uint32_t RDM_STANDBY_AFTER_MS = TCALL_RDM_STANDBY_AFTER_MS;
constexpr uint32_t RDM_STANDBY_WAKE_INTERVAL_MS = TCALL_RDM_STANDBY_WAKE_INTERVAL_MS;
constexpr uint32_t RDM_STANDBY_PROBE_WINDOW_MS = TCALL_RDM_STANDBY_PROBE_WINDOW_MS;
constexpr const char* MQTT_TRANSPORT = TCALL_MQTT_TRANSPORT;
constexpr uint16_t MQTT_PORT = TCALL_MQTT_PORT;
constexpr const char* MQTT_USER = TCALL_MQTT_USER;
constexpr const char* MQTT_PASS = TCALL_MQTT_PASS;
constexpr const char* MQTT_CLIENT_ID = TCALL_MQTT_CLIENT_ID;
constexpr const char* MQTT_TOPIC_PREFIX = TCALL_MQTT_TOPIC_PREFIX;
constexpr uint32_t MQTT_PUBLISH_INTERVAL_MS = TCALL_MQTT_PUBLISH_INTERVAL_MS;
constexpr const char* RDM_SHARED_SECRET = TCALL_RDM_SHARED_SECRET;
constexpr const char* FW_VERSION = TCALL_FW_VERSION;
constexpr bool GPS_AUTOSTART = TCALL_GPS_AUTOSTART != 0;
constexpr uint16_t WIFI_CONSOLE_PORT = 23;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t RDM_LTE_ALIVE_RETRY_MS = 60000;

tcall::TCallA7670Modem modemService;
tcall::GitHubOtaDemo githubOta(modemService);
tcall::GpsBufferedService gpsBuffer(modemService);
WiFiServer wifiConsoleServer(WIFI_CONSOLE_PORT);
WiFiClient wifiConsoleClient;
WiFiClient mqttNetClient;
WiFiClient remoteHttpClient;
String serialCommandLine;
String wifiCommandLine;
RemoteDeviceManager::WiFiMqttTransport wifiMqttTransport(mqttNetClient);

class NativeCellularMqttTransport : public RemoteDeviceManager::MqttTransport {
 public:
  explicit NativeCellularMqttTransport(tcall::TCallA7670Modem& modem) : modem_(modem) {}

  bool connect(const char* host,
               uint16_t port,
               const char* clientId,
               const char* user,
               const char* pass,
               Stream* log) override
  {
    active_ = this;
    modem_.mqttSetCallback(&NativeCellularMqttTransport::onMessage);
    connected_ = modem_.mqttBegin(false, log) && modem_.mqttConnect(host, port, clientId, user, pass, log);
    return connected_;
  }

  bool connected() const override { return connected_; }

  bool publish(const char* topic, const char* payload, bool retain, Stream* log) override
  {
    connected_ = modem_.mqttPublish(topic, payload, retain, log);
    return connected_;
  }

  bool subscribe(const char* topic, Stream* log) override
  {
    connected_ = modem_.mqttSubscribe(topic, log);
    return connected_;
  }

  bool poll(RemoteDeviceManager::MqttMessage& message) override
  {
    modem_.mqttHandle(1000);
    if (!pending_) {
      return false;
    }
    message = pendingMessage_;
    pending_ = false;
    return true;
  }

  void disconnect(Stream* log) override
  {
    modem_.mqttDisconnect(log);
    connected_ = false;
  }

  const char* name() const override { return "simcom-native-mqtt"; }

 private:
  static void onMessage(const char* topic, const uint8_t* payload, uint32_t len)
  {
    if (!active_) {
      return;
    }
    if (active_->pending_) {
      return;
    }
    active_->pendingMessage_.topic = topic;
    active_->pendingMessage_.payload = "";
    active_->pendingMessage_.payload.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
      active_->pendingMessage_.payload += static_cast<char>(payload[i]);
    }
    active_->pending_ = true;
  }

  tcall::TCallA7670Modem& modem_;
  bool connected_ = false;
  bool pending_ = false;
  RemoteDeviceManager::MqttMessage pendingMessage_;
  static NativeCellularMqttTransport* active_;
};

NativeCellularMqttTransport* NativeCellularMqttTransport::active_ = nullptr;
NativeCellularMqttTransport cellularMqttTransport(modemService);

class SwitchableMqttTransport : public RemoteDeviceManager::MqttTransport {
 public:
  SwitchableMqttTransport(RemoteDeviceManager::MqttTransport& wifi,
                          RemoteDeviceManager::MqttTransport& cellular)
      : wifi_(wifi), cellular_(cellular), active_(&wifi) {}

  bool setMode(const char* mode, Stream& out)
  {
    if (!mode) {
      out.println("missing transport mode");
      return false;
    }
    String requested = mode;
    requested.trim();
    requested.toLowerCase();
    RemoteDeviceManager::MqttTransport* next = nullptr;
    if (requested == "wifi") {
      next = &wifi_;
    } else if (requested == "lte" || requested == "cellular") {
      next = &cellular_;
    } else {
      out.print("unsupported transport mode: ");
      out.println(mode);
      return false;
    }
    if (next == active_) {
      out.print("transport already active: ");
      out.println(name());
      return true;
    }
    active_->disconnect(&out);
    active_ = next;
    out.print("transport switched to ");
    out.println(name());
    return true;
  }

  bool usingCellular() const { return active_ == &cellular_; }

  bool connect(const char* host,
               uint16_t port,
               const char* clientId,
               const char* user,
               const char* pass,
               Stream* log) override
  {
    const char* selectedHost = usingCellular() && strlen(MQTT_CELLULAR_HOST) > 0 ? MQTT_CELLULAR_HOST : host;
    const uint16_t selectedPort = usingCellular() && MQTT_CELLULAR_PORT > 0 ? MQTT_CELLULAR_PORT : port;
    return active_->connect(selectedHost, selectedPort, clientId, user, pass, log);
  }

  bool connected() const override { return active_->connected(); }
  bool publish(const char* topic, const char* payload, bool retain, Stream* log) override
  {
    return active_->publish(topic, payload, retain, log);
  }
  bool subscribe(const char* topic, Stream* log) override { return active_->subscribe(topic, log); }
  bool poll(RemoteDeviceManager::MqttMessage& message) override { return active_->poll(message); }
  void disconnect(Stream* log) override { active_->disconnect(log); }
  const char* name() const override { return active_->name(); }

 private:
  RemoteDeviceManager::MqttTransport& wifi_;
  RemoteDeviceManager::MqttTransport& cellular_;
  RemoteDeviceManager::MqttTransport* active_;
};

SwitchableMqttTransport switchableMqttTransport(wifiMqttTransport, cellularMqttTransport);
RemoteDeviceManager::Manager* remoteManager = nullptr;

enum class RemotePowerState {
  Awake,
  EnteringStandby,
  StandbyProbe,
};

RemotePowerState remotePowerState = RemotePowerState::Awake;
uint32_t lastRemoteActivityMs = 0;
uint32_t standbyProbeStartMs = 0;
bool standbyNetworksPrepared = false;

const char* remotePowerStateName();
void setupWifi();
void restoreAwakePeripherals();

struct ProbeEndpoint {
  String host;
  uint16_t port = 0;
};

constexpr tcall::ApnProfile GSM_APN_PROFILES[] = {
    {"configured_default", tcall::DEFAULT_APN, tcall::DEFAULT_APN_USER,
     tcall::DEFAULT_APN_PASS},
};

void printHelp(Stream& out)
{
  out.println();
  out.println("Commands:");
  out.println("  help");
  out.println("  diag");
  out.println("  at <cmd>");
  out.println("  sim [status|pin-off]");
  out.println("  sms list [all|unread|read]");
  out.println("  sms send <number> <message>");
  out.println("  reg");
  out.println("  operator auto|telekom|status");
  out.println("  rat auto|lte");
  out.println("  data up|down|status");
  out.println("  mqtt status|publish");
  out.println("  ota config|latest|update");
  out.println("  gsm prove [timeout_seconds] [host] [path]");
  out.println("  gsm tcp <host> <port> [timeout_seconds]");
  out.println("  gsm reset");
  out.println("  http <host> [path]");
  out.println("  gps on|off|raw|fix|ex|cache|prove [timeout_seconds]|hot|cold|status");
  out.println("  wifi status");
  out.println();
}

String nextToken(String& line)
{
  line.trim();
  int sep = line.indexOf(' ');
  if (sep < 0) {
    String token = line;
    line = "";
    token.trim();
    return token;
  }

  String token = line.substring(0, sep);
  line = line.substring(sep + 1);
  token.trim();
  line.trim();
  return token;
}

void printBufferedGps(Stream& out)
{
  const tcall::BufferedGpsInfo info = gpsBuffer.getGpsInformation();
  const tcall::BufferedGpsTime time = gpsBuffer.getTime();

  out.print("GNSS power seen: ");
  out.println(gpsBuffer.gpsPowerSeen() ? "yes" : "no");
  out.print("Last poll OK: ");
  out.println(gpsBuffer.lastPollOk() ? "yes" : "no");
  out.print("Cache valid: ");
  out.println(info.valid ? "yes" : "no");
  out.print("Has fix: ");
  out.println(info.hasFix ? "yes" : "no");
  out.print("FixMode: ");
  out.println(info.fixMode);
  out.print("Latitude: ");
  out.println(info.latitude, 6);
  out.print("Longitude: ");
  out.println(info.longitude, 6);
  out.print("Speed m/s: ");
  out.println(info.speedMps, 3);
  out.print("Altitude m: ");
  out.println(info.altitudeMeters);
  out.print("Course deg: ");
  out.println(info.courseDegrees);
  out.print("Satellites total/gps/bds/glo/gal: ");
  out.print(info.totalSatellites);
  out.print('/');
  out.print(info.gpsSatellites);
  out.print('/');
  out.print(info.beidouSatellites);
  out.print('/');
  out.print(info.glonassSatellites);
  out.print('/');
  out.println(info.galileoSatellites);
  out.print("PDOP/HDOP/VDOP: ");
  out.print(info.pdop);
  out.print('/');
  out.print(info.hdop);
  out.print('/');
  out.println(info.vdop);
  out.print("UTC valid: ");
  out.println(time.valid ? "yes" : "no");
  out.print("UTC: ");
  out.print(time.year);
  out.print('-');
  out.print(time.month);
  out.print('-');
  out.print(time.day);
  out.print('T');
  out.print(time.hour);
  out.print(':');
  out.print(time.minute);
  out.print(':');
  out.println(time.second);
  if (info.valid) {
    out.print("Cache age ms: ");
    out.println(millis() - info.updatedMs);
  }
}

bool mqttConfigured()
{
  return strlen(MQTT_HOST) > 0;
}

bool mqttUseCellular()
{
  return strcmp(MQTT_TRANSPORT, "cellular") == 0 || strcmp(MQTT_TRANSPORT, "lte") == 0 ||
         strcmp(MQTT_TRANSPORT, "gsm") == 0;
}

String escapeJsonValue(const String& value)
{
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (static_cast<uint8_t>(c) >= 0x20) {
      out += c;
    }
  }
  return out;
}

ProbeEndpoint endpointFromHostPort(const String& host, const String& port, uint16_t fallbackPort)
{
  ProbeEndpoint endpoint;
  endpoint.host = host;
  endpoint.host.trim();
  endpoint.port = port.length() ? static_cast<uint16_t>(port.toInt()) : fallbackPort;
  return endpoint;
}

ProbeEndpoint endpointFromUrl(const String& url, uint16_t fallbackPort)
{
  ProbeEndpoint endpoint;
  String value = url;
  value.trim();
  int start = value.indexOf("://");
  start = start >= 0 ? start + 3 : 0;
  int slash = value.indexOf('/', start);
  String hostPort = slash >= 0 ? value.substring(start, slash) : value.substring(start);
  int colon = hostPort.lastIndexOf(':');
  if (colon > 0) {
    endpoint.host = hostPort.substring(0, colon);
    endpoint.port = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
  } else {
    endpoint.host = hostPort;
    endpoint.port = fallbackPort;
  }
  endpoint.host.trim();
  return endpoint;
}

bool wifiTcpProbe(const ProbeEndpoint& endpoint, uint16_t timeoutMs = 900)
{
  if (endpoint.host.length() == 0 || endpoint.port == 0 || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  WiFiClient probe;
  probe.setTimeout(timeoutMs);
  const bool ok = probe.connect(endpoint.host.c_str(), endpoint.port);
  probe.stop();
  return ok;
}

bool cellularTcpProbe(const ProbeEndpoint& endpoint, uint8_t timeoutSeconds = 2)
{
  if (endpoint.host.length() == 0 || endpoint.port == 0 || !modemService.dataActive()) {
    return false;
  }
  return modemService.tcpProbe(endpoint.host.c_str(), endpoint.port, timeoutSeconds, Serial);
}

struct ConnectivityProbeCache {
  String wifiMqttKey;
  String lteMqttKey;
  bool wifiMqttOk = false;
  bool lteMqttOk = false;
  uint32_t wifiMqttCheckedMs = 0;
  uint32_t lteMqttCheckedMs = 0;
};

ConnectivityProbeCache connectivityProbeCache;
uint32_t nextLteAliveDataAttemptMs = 0;

String endpointKey(const ProbeEndpoint& endpoint)
{
  return endpoint.host + ":" + String(endpoint.port);
}

bool cachedWifiMqttProbe(const ProbeEndpoint& endpoint, bool wifiConnected)
{
  if (!wifiConnected || endpoint.host.length() == 0 || endpoint.port == 0) {
    connectivityProbeCache.wifiMqttOk = false;
    return false;
  }
  const uint32_t now = millis();
  const String key = endpointKey(endpoint);
  if (key != connectivityProbeCache.wifiMqttKey ||
      now - connectivityProbeCache.wifiMqttCheckedMs > 30000UL) {
    connectivityProbeCache.wifiMqttKey = key;
    connectivityProbeCache.wifiMqttCheckedMs = now;
    connectivityProbeCache.wifiMqttOk = wifiTcpProbe(endpoint, 350);
  }
  return connectivityProbeCache.wifiMqttOk;
}

bool cachedLteMqttProbe(const ProbeEndpoint& endpoint, bool lteConnected)
{
  if (!lteConnected || endpoint.host.length() == 0 || endpoint.port == 0) {
    connectivityProbeCache.lteMqttOk = false;
    return false;
  }
  const uint32_t now = millis();
  const String key = endpointKey(endpoint);
  if (key != connectivityProbeCache.lteMqttKey ||
      now - connectivityProbeCache.lteMqttCheckedMs > 30000UL) {
    connectivityProbeCache.lteMqttKey = key;
    connectivityProbeCache.lteMqttCheckedMs = now;
    connectivityProbeCache.lteMqttOk = cellularTcpProbe(endpoint, 2);
  }
  return connectivityProbeCache.lteMqttOk;
}

bool ensureCellularData(Stream& out)
{
  if (modemService.dataActive()) {
    out.print("Cellular data already active. Local IP: ");
    out.println(modemService.localIP());
    return true;
  }

  out.println("Cellular data is down; bringing LTE data up.");
  if (!modemService.waitForSimReady(30000, out)) {
    return false;
  }
  modemService.configureRat(true, out);
  if (!modemService.waitForEpsRegistration(120000, out)) {
    return false;
  }
  if (!modemService.activateData(tcall::DEFAULT_APN, tcall::DEFAULT_APN_USER,
                                 tcall::DEFAULT_APN_PASS, &out)) {
    out.println("Cellular data activation failed.");
    return false;
  }

  out.print("Cellular data active. Local IP: ");
  out.println(modemService.localIP());
  return true;
}

bool ensureLteDataForAlive()
{
  if (modemService.dataActive()) {
    return true;
  }
  if (!RDM_LTE_ALIVE_AUTOSTART) {
    return false;
  }
  const uint32_t now = millis();
  if (now < nextLteAliveDataAttemptMs) {
    return false;
  }
  nextLteAliveDataAttemptMs = now + RDM_LTE_ALIVE_RETRY_MS;
  Serial.println("RemoteDeviceManager alive: LTE data is down; retrying LTE data activation.");
  const bool ok = ensureCellularData(Serial);
  if (!ok) {
    Serial.println("RemoteDeviceManager alive: LTE data activation retry failed.");
  }
  return ok;
}

String mqttTopic(const char* suffix)
{
  String topic = MQTT_TOPIC_PREFIX;
  if (!topic.endsWith("/")) {
    topic += '/';
  }
  topic += suffix;
  return topic;
}

bool mqttEnsureConnected(Stream* out = nullptr)
{
  if (remoteManager && remoteManager->connected()) {
    return true;
  }
  if (out) {
    (*out).println("MQTT connection is managed by RemoteDeviceManager.");
  }
  return false;
}

String gpsJson()
{
  const tcall::BufferedGpsInfo info = gpsBuffer.getGpsInformation();
  const tcall::BufferedGpsTime time = gpsBuffer.getTime();
  String json;
  json.reserve(384);
  json += "{\"valid\":";
  json += info.valid ? "true" : "false";
  json += ",\"has_fix\":";
  json += info.hasFix ? "true" : "false";
  json += ",\"fix_mode\":";
  json += info.fixMode;
  json += ",\"lat\":";
  json += String(info.latitude, 6);
  json += ",\"lon\":";
  json += String(info.longitude, 6);
  json += ",\"speed_mps\":";
  json += String(info.speedMps, 3);
  json += ",\"alt_m\":";
  json += String(info.altitudeMeters, 2);
  json += ",\"course_deg\":";
  json += String(info.courseDegrees, 2);
  json += ",\"sat_total\":";
  json += info.totalSatellites;
  json += ",\"sat_gps\":";
  json += info.gpsSatellites;
  json += ",\"sat_bds\":";
  json += info.beidouSatellites;
  json += ",\"sat_glo\":";
  json += info.glonassSatellites;
  json += ",\"sat_gal\":";
  json += info.galileoSatellites;
  json += ",\"pdop\":";
  json += String(info.pdop, 2);
  json += ",\"hdop\":";
  json += String(info.hdop, 2);
  json += ",\"vdop\":";
  json += String(info.vdop, 2);
  json += ",\"utc_valid\":";
  json += time.valid ? "true" : "false";
  json += ",\"utc\":\"";
  json += time.year;
  json += '-';
  json += time.month;
  json += '-';
  json += time.day;
  json += 'T';
  json += time.hour;
  json += ':';
  json += time.minute;
  json += ':';
  json += time.second;
  json += "\",\"age_ms\":";
  json += info.valid ? String(millis() - info.updatedMs) : String(-1);
  json += ",\"runner_started\":";
  json += gpsBuffer.started() ? "true" : "false";
  json += ",\"powered\":";
  json += gpsBuffer.gpsPowerSeen() ? "true" : "false";
  json += ",\"poll_ok\":";
  json += gpsBuffer.lastPollOk() ? "true" : "false";
  json += ",\"poll_age_ms\":";
  json += gpsBuffer.lastPollAttemptMs() > 0 ? String(millis() - gpsBuffer.lastPollAttemptMs())
                                             : String(-1);
  json += ",\"searching\":";
  json += (gpsBuffer.started() && gpsBuffer.gpsPowerSeen() && !info.hasFix) ? "true" : "false";
  json += ",\"fix_seen\":";
  json += gpsBuffer.fixEverSeen() ? "true" : "false";
  json += ",\"last_fix_age_ms\":";
  json += gpsBuffer.lastFixMs() > 0 ? String(millis() - gpsBuffer.lastFixMs()) : String(-1);
  json += '}';
  return json;
}

String statusJson()
{
  String json;
  json.reserve(256);
  json += "{\"uptime_ms\":";
  json += millis();
  json += ",\"wifi_connected\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"wifi_rssi\":";
  json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String(0);
  json += ",\"ip\":\"";
  json += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  json += "\",\"mqtt_connected\":";
  json += remoteManager && remoteManager->connected() ? "true" : "false";
  json += ",\"mqtt_transport\":\"";
  json += remoteManager ? remoteManager->transportName() : "none";
  json += "\"";
  json += ",\"gps_power_seen\":";
  json += gpsBuffer.gpsPowerSeen() ? "true" : "false";
  json += ",\"gps_last_poll_ok\":";
  json += gpsBuffer.lastPollOk() ? "true" : "false";
  json += ",\"remote_power_state\":\"";
  json += remotePowerStateName();
  json += "\",\"standby_enabled\":";
  json += RDM_STANDBY_ENABLED ? "true" : "false";
  json += ",\"standby_in_ms\":";
  const uint32_t idleMs = millis() - lastRemoteActivityMs;
  json += remotePowerState == RemotePowerState::Awake && idleMs < RDM_STANDBY_AFTER_MS
              ? String(RDM_STANDBY_AFTER_MS - idleMs)
              : String(0);
  json += '}';
  return json;
}

String connectivityJson(const String& requestPayload)
{
  const String requestId = RemoteDeviceManager::jsonValue(requestPayload, "request_id");
  const ProbeEndpoint wifiMqtt =
      endpointFromHostPort(RemoteDeviceManager::jsonValue(requestPayload, "local_mqtt_host"),
                           RemoteDeviceManager::jsonValue(requestPayload, "mqtt_port"), MQTT_PORT);
  const ProbeEndpoint lteMqtt =
      endpointFromHostPort(RemoteDeviceManager::jsonValue(requestPayload, "public_mqtt_host"),
                           RemoteDeviceManager::jsonValue(requestPayload, "public_mqtt_port"),
                           MQTT_CELLULAR_PORT > 0 ? MQTT_CELLULAR_PORT : MQTT_PORT);

  const bool activeLte = switchableMqttTransport.usingCellular();
  const bool activeWifi = !activeLte;
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool wifiMqttOk = activeWifi && remoteManager && remoteManager->connected()
                              ? true
                              : cachedWifiMqttProbe(wifiMqtt, wifiConnected);
  const bool lteConnected = modemService.dataActive() || ensureLteDataForAlive();
  const bool lteMqttOk = activeLte && remoteManager && remoteManager->connected()
                              ? true
                              : cachedLteMqttProbe(lteMqtt, lteConnected);

  String json;
  json.reserve(560);
  json += "{\"schema\":\"rdm-1\",\"device_id\":\"";
  json += DEVICE_ID;
  json += "\",\"request_id\":\"";
  json += escapeJsonValue(requestId);
  json += "\",\"uptime_ms\":";
  json += millis();
  json += ",\"active_link\":\"";
  json += activeLte ? "lte" : "wifi";
  json += "\",\"transport\":\"";
  json += remoteManager ? remoteManager->transportName() : "none";
  json += "\",\"wifi\":{\"connected\":";
  json += wifiConnected ? "true" : "false";
  json += ",\"ip\":\"";
  json += wifiConnected ? WiFi.localIP().toString() : "";
  json += "\",\"rssi\":";
  json += wifiConnected ? String(WiFi.RSSI()) : String(0);
  json += ",\"mqtt\":";
  json += wifiMqttOk ? "true" : "false";
  json += "},\"lte\":{\"connected\":";
  json += lteConnected ? "true" : "false";
  json += ",\"ip\":\"";
  json += lteConnected ? modemService.localIP() : "";
  json += "\",\"mqtt\":";
  json += lteMqttOk ? "true" : "false";
  json += "}}";
  return json;
}

bool mqttPublishTelemetry(Stream* out = nullptr)
{
  const bool ok = remoteManager && remoteManager->publishTelemetry();
  if (out) {
    (*out).print("RemoteDeviceManager telemetry publish -> ");
    (*out).println(ok ? "OK" : "FAILED");
  }
  return ok;
}

void printBasicFix(const tcall::GnssFix& fix, Stream& out)
{
  out.print("FixMode: ");
  out.println(fix.fixMode);
  out.print("Latitude: ");
  out.println(fix.latitude, 6);
  out.print("Longitude: ");
  out.println(fix.longitude, 6);
  out.print("Speed: ");
  out.println(fix.speed);
  out.print("Altitude: ");
  out.println(fix.altitude);
  out.print("Visible satellites: ");
  out.println(fix.visibleSatellites);
  out.print("Used satellites: ");
  out.println(fix.usedSatellites);
  out.print("Accuracy: ");
  out.println(fix.accuracy);
  out.print("UTC date: ");
  out.print(fix.year);
  out.print('-');
  out.print(fix.month);
  out.print('-');
  out.println(fix.day);
  out.print("UTC time: ");
  out.print(fix.hour);
  out.print(':');
  out.print(fix.minute);
  out.print(':');
  out.println(fix.second);
}

void printExtendedFix(const GPSInfo& info, Stream& out)
{
  out.print("FixMode: ");
  out.println(info.isFix);
  out.print("Latitude: ");
  out.println(info.latitude, 6);
  out.print("Longitude: ");
  out.println(info.longitude, 6);
  out.print("Speed: ");
  out.println(info.speed);
  out.print("Altitude: ");
  out.println(info.altitude);
  out.print("GPS satellites: ");
  out.println(info.gps_satellite_num);
  out.print("BDS satellites: ");
  out.println(info.beidou_satellite_num);
  out.print("GLONASS satellites: ");
  out.println(info.glonass_satellite_num);
  out.print("GALILEO satellites: ");
  out.println(info.galileo_satellite_num);
  out.print("UTC date: ");
  out.print(info.year);
  out.print('-');
  out.print(info.month);
  out.print('-');
  out.println(info.day);
  out.print("UTC time: ");
  out.print(info.hour);
  out.print(':');
  out.print(info.minute);
  out.print(':');
  out.println(info.second);
  out.print("Course: ");
  out.println(info.course);
  out.print("PDOP: ");
  out.println(info.PDOP);
  out.print("HDOP: ");
  out.println(info.HDOP);
  out.print("VDOP: ");
  out.println(info.VDOP);
}

void printProofSample(uint32_t elapsedSeconds, const GPSInfo& info, const String& raw, Stream& out)
{
  const int totalSatellites = info.gps_satellite_num + info.beidou_satellite_num +
                              info.glonass_satellite_num + info.galileo_satellite_num;

  out.print("GPS PROOF SAMPLE elapsed_s=");
  out.print(elapsedSeconds);
  out.print(" fix=");
  out.print(info.isFix);
  out.print(" lat=");
  out.print(info.latitude, 6);
  out.print(" lon=");
  out.print(info.longitude, 6);
  out.print(" sats=");
  out.print(totalSatellites);
  out.print(" gps=");
  out.print(info.gps_satellite_num);
  out.print(" bds=");
  out.print(info.beidou_satellite_num);
  out.print(" glo=");
  out.print(info.glonass_satellite_num);
  out.print(" gal=");
  out.print(info.galileo_satellite_num);
  out.print(" pdop=");
  out.print(info.PDOP);
  out.print(" hdop=");
  out.print(info.HDOP);
  out.print(" vdop=");
  out.print(info.VDOP);
  out.print(" utc=");
  out.print(info.year);
  out.print('-');
  out.print(info.month);
  out.print('-');
  out.print(info.day);
  out.print('T');
  out.print(info.hour);
  out.print(':');
  out.print(info.minute);
  out.print(':');
  out.println(info.second);
  out.print("GPS PROOF RAW ");
  out.println(raw);
}

bool proofPasses(const GPSInfo& info)
{
  const int totalSatellites = info.gps_satellite_num + info.beidou_satellite_num +
                              info.glonass_satellite_num + info.galileo_satellite_num;
  return (info.isFix == 2 || info.isFix == 3) &&
         fabs(info.latitude) > 0.000001F &&
         fabs(info.longitude) > 0.000001F &&
         totalSatellites > 0;
}

void printGsmProofDiagnostics(Stream& out)
{
  out.println("GSM PROOF DIAG BEGIN");
  out.print("GSM PROOF DIAG data_active=");
  out.println(modemService.dataActive() ? "yes" : "no");
  out.print("GSM PROOF DIAG local_ip=");
  out.println(modemService.localIP());
  out.print("GSM PROOF DIAG AT+CPIN? ");
  out.println(modemService.rawAt("+CPIN?", 5000));
  out.print("GSM PROOF DIAG AT+CSQ ");
  out.println(modemService.rawAt("+CSQ", 5000));
  out.print("GSM PROOF DIAG AT+CEREG? ");
  out.println(modemService.rawAt("+CEREG?", 5000));
  out.print("GSM PROOF DIAG AT+CREG? ");
  out.println(modemService.rawAt("+CREG?", 5000));
  out.print("GSM PROOF DIAG AT+CGREG? ");
  out.println(modemService.rawAt("+CGREG?", 5000));
  out.print("GSM PROOF DIAG AT+COPS? ");
  out.println(modemService.rawAt("+COPS?", 10000));
  out.print("GSM PROOF DIAG AT+CPSI? ");
  out.println(modemService.rawAt("+CPSI?", 10000));
  out.print("GSM PROOF DIAG AT+CNMP? ");
  out.println(modemService.rawAt("+CNMP?", 5000));
  out.print("GSM PROOF DIAG AT+CGDCONT? ");
  out.println(modemService.rawAt("+CGDCONT?", 5000));
  out.print("GSM PROOF DIAG AT+CGATT? ");
  out.println(modemService.rawAt("+CGATT?", 5000));
  out.print("GSM PROOF DIAG AT+NETOPEN? ");
  out.println(modemService.rawAt("+NETOPEN?", 5000));
  out.println("GSM PROOF DIAG END");
}

bool commandClientStillAlive(Stream& out)
{
  ArduinoOTA.handle();
  if (&out == &wifiConsoleClient && (!wifiConsoleClient || !wifiConsoleClient.connected())) {
    Serial.println("WiFi command client disconnected; aborting command.");
    return false;
  }
  return true;
}

bool runGsmProof(uint32_t timeoutSeconds, const String& host, const String& path, Stream& out)
{
  out.print("GSM PROOF START timeout_s=");
  out.print(timeoutSeconds);
  out.print(" host=");
  out.print(host);
  out.print(" path=");
  out.println(path);

  const uint32_t registrationTimeoutMs = timeoutSeconds * 1000UL;
  const size_t profileCount = sizeof(GSM_APN_PROFILES) / sizeof(GSM_APN_PROFILES[0]);

  out.println("GSM PROOF STEP cellular_prepare");
  modemService.disableGps();
  if (!modemService.prepareForCellular(out)) {
    out.println("GSM PROOF STEP modem_recovery_restart");
    if (!modemService.restart(out, 45000) ||
        !modemService.prepareForCellular(out)) {
      printGsmProofDiagnostics(out);
      out.println("GSM PROOF FAIL reason=cellular_prepare_failed");
      return false;
    }
  }

  for (size_t i = 0; i < profileCount; ++i) {
    if (!commandClientStillAlive(out)) {
      return false;
    }

    const tcall::ApnProfile& profile = GSM_APN_PROFILES[i];
    out.print("GSM PROOF STEP apn_profile index=");
    out.print(i);
    out.print(" name=");
    out.print(profile.name);
    out.print(" apn=");
    out.print(profile.apn);
    out.print(" user_set=");
    out.println(strlen(profile.user) > 0 ? "yes" : "no");

    out.println("GSM PROOF STEP sim_ready");
    if (!modemService.waitForSimReady(30000, out)) {
      out.print("GSM PROOF PROFILE FAIL name=");
      out.print(profile.name);
      out.println(" reason=sim_not_ready");
      continue;
    }

    if (!commandClientStillAlive(out)) {
      return false;
    }

    out.println("GSM PROOF STEP apn_only");
    modemService.configureApn(profile.apn, out);

    out.println("GSM PROOF STEP eps_registration");
    const bool registered = modemService.waitForEpsRegistration(registrationTimeoutMs, out);
    out.print("GSM PROOF REGISTRATION ");
    out.println(registered ? "registered" : "not_registered");
    if (!registered) {
      modemService.printRegistrationSnapshot(out, profile.name);
      out.print("GSM PROOF PROFILE FAIL name=");
      out.print(profile.name);
      out.println(" reason=eps_not_registered_skip_pdp");
      continue;
    }

    out.println("GSM PROOF STEP data_activation");
    if (!modemService.activateData(profile.apn, profile.user, profile.pass, &out, 1)) {
      out.print("GSM PROOF PROFILE FAIL name=");
      out.print(profile.name);
      out.println(" reason=data_activation_failed");
      continue;
    }

    out.print("GSM PROOF LOCAL_IP ");
    out.println(modemService.localIP());

    out.println("GSM PROOF STEP webpage_fetch");
    const bool fetched = modemService.httpGet(host.c_str(), path.c_str(), 80, out);
    if (!fetched) {
      out.print("GSM PROOF PROFILE FAIL name=");
      out.print(profile.name);
      out.println(" reason=webpage_fetch_failed");
      modemService.deactivateData();
      continue;
    }

    out.print("GSM PROOF PASS profile=");
    out.print(profile.name);
    out.print(" apn=");
    out.print(profile.apn);
    out.print(" local_ip=");
    out.print(modemService.localIP());
    out.print(" host=");
    out.print(host);
    out.print(" path=");
    out.println(path);
    return true;
  }

  printGsmProofDiagnostics(out);
  out.println("GSM PROOF FAIL reason=no_apn_profile_fetched_webpage");
  return false;
}

void waitDuringGpsProof(uint32_t waitMs)
{
  const uint32_t start = millis();
  while (millis() - start < waitMs) {
    ArduinoOTA.handle();
    delay(50);
  }
}

void printWifiStatus(Stream& out)
{
  out.print("WiFi SSID: ");
  out.println(WIFI_SSID);
  out.print("WiFi status: ");
  out.println(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  if (WiFi.status() == WL_CONNECTED) {
    out.print("IP: ");
    out.println(WiFi.localIP());
    out.print("RSSI: ");
    out.println(WiFi.RSSI());
    out.print("TCP console: ");
    out.print(WiFi.localIP());
    out.print(':');
    out.println(WIFI_CONSOLE_PORT);
    out.print("OTA hostname: ");
    out.print(OTA_HOSTNAME);
    out.println(".local");
  }
}

void handleGsmCommand(String args, Stream& out)
{
  String action = nextToken(args);
  action.toLowerCase();

  if (action == "reset") {
    out.println(modemService.restart(out, 45000) ? "Modem restarted." : "Modem restart failed.");
    return;
  }

  if (action == "prove") {
    uint32_t timeoutSeconds = static_cast<uint32_t>(nextToken(args).toInt());
    if (timeoutSeconds == 0) {
      timeoutSeconds = 180;
    }

    String host = nextToken(args);
    String path = nextToken(args);
    if (host.length() == 0) {
      host = "example.com";
    }
    if (path.length() == 0) {
      path = "/";
    }
    if (!path.startsWith("/")) {
      path = "/" + path;
    }

    runGsmProof(timeoutSeconds, host, path, out);
    return;
  }

  if (action == "tcp") {
    String host = nextToken(args);
    const uint16_t port = static_cast<uint16_t>(nextToken(args).toInt());
    uint32_t timeoutSeconds = static_cast<uint32_t>(nextToken(args).toInt());
    if (timeoutSeconds == 0) {
      timeoutSeconds = 30;
    }

    if (host.length() == 0 || port == 0) {
      out.println("Usage: gsm tcp <host> <port> [timeout_seconds]");
      return;
    }

    if (!ensureCellularData(out)) {
      out.println("LTE data unavailable.");
      return;
    }

    modemService.tcpProbe(host.c_str(), port, static_cast<uint8_t>(min<uint32_t>(timeoutSeconds, 255)), out);
    return;
  }

  out.println("Usage: gsm prove [timeout_seconds] [host] [path] | gsm tcp <host> <port> [timeout_seconds] | gsm reset");
}

void handleSimCommand(String args, Stream& out)
{
  String action = nextToken(args);
  action.toLowerCase();

  if (action == "pin-off") {
    out.println("This permanently disables the SIM PIN lock using the configured PIN.");
    out.println(modemService.disableSimPin(out) ? "SIM PIN lock disabled or already ready." :
                                                  "SIM PIN disable failed.");
    return;
  }

  if (action.length() == 0 || action == "status") {
    modemService.waitForSimReady(30000, out);
    modemService.printSimStatus(out);
    out.print("Raw CPIN: ");
    out.println(modemService.rawAt("+CPIN?", 5000));
    return;
  }

  out.println("Usage: sim [status|pin-off]");
}

void handleSmsCommand(String args, Stream& out)
{
  String action = nextToken(args);
  action.toLowerCase();

  if (action == "list" || action.length() == 0) {
    String status = nextToken(args);
    status.toLowerCase();
    const char* cmglStatus = "ALL";
    if (status == "unread") {
      cmglStatus = "REC UNREAD";
    } else if (status == "read") {
      cmglStatus = "REC READ";
    }

    if (!modemService.waitForSimReady(30000, out)) {
      out.println("SIM not ready for SMS listing.");
      return;
    }
    out.print("SMS LIST status=");
    out.println(cmglStatus);
    out.println(modemService.listSms(cmglStatus));
    return;
  }

  if (action == "send") {
    String number = nextToken(args);
    if (number.length() == 0 || args.length() == 0) {
      out.println("Usage: sms send <number> <message>");
      return;
    }

    if (!modemService.waitForSimReady(30000, out)) {
      out.println("SIM not ready for SMS sending.");
      return;
    }
    out.print("SMS SEND number=");
    out.println(number);
    out.println(modemService.sendSms(number.c_str(), args, out) ? "SMS SEND PASS" :
                                                            "SMS SEND FAIL");
    return;
  }

  out.println("Usage: sms list [all|unread|read] | sms send <number> <message>");
}

void handleDataCommand(String args, Stream& out)
{
  String action = nextToken(args);
  action.toLowerCase();

  if (action == "up") {
    if (!modemService.waitForSimReady(30000, out)) {
      return;
    }
    modemService.configureRat(true, out);
    if (!modemService.waitForEpsRegistration(120000, out)) {
      return;
    }
    if (!modemService.activateData(tcall::DEFAULT_APN, tcall::DEFAULT_APN_USER,
                                   tcall::DEFAULT_APN_PASS, &out)) {
      out.println("Data activation failed.");
      return;
    }
    out.print("Local IP: ");
    out.println(modemService.localIP());
    return;
  }

  if (action == "down") {
    out.println(modemService.deactivateData() ? "Data deactivated." : "Data deactivate failed.");
    return;
  }

  if (action == "status") {
    out.print("Data active: ");
    out.println(modemService.dataActive() ? "yes" : "no");
    out.print("Local IP: ");
    out.println(modemService.localIP());
    return;
  }

  out.println("Usage: data up|down|status");
}

void handleMqttCommand(String args, Stream& out)
{
  String action = nextToken(args);
  action.toLowerCase();

  if (action.length() == 0 || action == "status") {
    out.print("MQTT configured: ");
    out.println(mqttConfigured() ? "yes" : "no");
    out.print("MQTT host: ");
    out.println(MQTT_HOST);
    out.print("MQTT cellular host: ");
    out.println(MQTT_CELLULAR_HOST);
    out.print("MQTT cellular native: ");
    out.println(MQTT_CELLULAR_NATIVE ? "yes" : "no");
    out.print("MQTT port: ");
    out.println(MQTT_PORT);
    out.print("MQTT transport: ");
    out.println(mqttUseCellular() ? "cellular" : "wifi");
    out.print("MQTT topic prefix: ");
    out.println(MQTT_TOPIC_PREFIX);
    out.print("MQTT connected: ");
    out.println(remoteManager && remoteManager->connected() ? "yes" : "no");
    out.print("MQTT implementation: ");
    out.println(remoteManager ? remoteManager->transportName() : "not started");
    return;
  }

  if (action == "publish") {
    out.println(mqttPublishTelemetry(&out) ? "MQTT PUBLISH PASS" : "MQTT PUBLISH FAIL");
    return;
  }

  if (action == "transport") {
    String mode = nextToken(args);
    mode.toLowerCase();
    if (mode.length() == 0) {
      out.println("Usage: mqtt transport <wifi|lte>");
      return;
    }
    bool ok = true;
    if (mode == "lte" || mode == "cellular" || mode == "gsm") {
      ok = ensureCellularData(out);
    }
    ok = ok && switchableMqttTransport.setMode(mode.c_str(), out);
    if (remoteManager) {
      remoteManager->loop();
    }
    out.println(ok ? "MQTT TRANSPORT SWITCH PASS" : "MQTT TRANSPORT SWITCH FAIL");
    return;
  }

  out.println("Usage: mqtt status|publish|transport <wifi|lte>");
}

const char* remotePowerStateName()
{
  switch (remotePowerState) {
    case RemotePowerState::Awake: return "awake";
    case RemotePowerState::EnteringStandby: return "entering_standby";
    case RemotePowerState::StandbyProbe: return "standby_probe";
  }
  return "unknown";
}

void noteRemoteActivity(const char* topicSuffix)
{
  lastRemoteActivityMs = millis();
  if (remotePowerState != RemotePowerState::Awake) {
    remotePowerState = RemotePowerState::Awake;
    standbyNetworksPrepared = false;
    Serial.print("RemoteDeviceManager activity woke device: ");
    Serial.println(topicSuffix ? topicSuffix : "unknown");
    restoreAwakePeripherals();
  }
}

void restoreAwakePeripherals()
{
  if (switchableMqttTransport.usingCellular()) {
    modemService.wakeFromStandby(Serial);
    ensureCellularData(Serial);
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      setupWifi();
    }
    modemService.wakeFromStandby(Serial);
  }
  if (GPS_AUTOSTART) {
    gpsBuffer.begin();
  }
}

void handleOtaCommand(String args, Stream& out)
{
  String action = nextToken(args);
  action.toLowerCase();

  if (action.length() == 0 || action == "config") {
    githubOta.printConfig(out);
    return;
  }

  if (action == "latest") {
    if (!ensureCellularData(out)) {
      out.println("OTA unavailable: could not bring LTE data up.");
      return;
    }
    out.println(githubOta.printLatest(out) ? "OTA LATEST PASS" : "OTA LATEST FAIL");
    return;
  }

  if (action == "update") {
    if (!ensureCellularData(out)) {
      out.println("OTA unavailable: could not bring LTE data up.");
      return;
    }
    out.println("OTA UPDATE START");
    out.println(githubOta.updateLatest(out) ? "OTA UPDATE PASS" : "OTA UPDATE FAIL");
    return;
  }

  out.println("Usage: ota config|latest|update");
}

void handleGpsCommand(String args, Stream& out)
{
  String action = nextToken(args);
  action.toLowerCase();

  if (action == "on") {
    gpsBuffer.begin();
    modemService.enableGps(out);
    return;
  }
  if (action == "off") {
    gpsBuffer.stop();
    out.println(modemService.disableGps() ? "GNSS disabled." : "GNSS disable failed.");
    return;
  }
  if (action == "raw") {
    out.print("GNSS raw: ");
    out.println(modemService.gpsRaw());
    return;
  }
  if (action == "fix") {
    tcall::GnssFix fix;
    if (modemService.gpsFix(fix)) {
      printBasicFix(fix, out);
    } else {
      out.println("No GNSS fix yet.");
    }
    return;
  }
  if (action == "ex") {
    GPSInfo info;
    if (modemService.gpsExtended(info)) {
      printExtendedFix(info, out);
    } else {
      out.println("No extended GNSS fix yet.");
    }
    return;
  }
  if (action == "cache") {
    printBufferedGps(out);
    return;
  }
  if (action == "prove") {
    gpsBuffer.stop();
    uint32_t timeoutSeconds = static_cast<uint32_t>(nextToken(args).toInt());
    if (timeoutSeconds == 0) {
      timeoutSeconds = 900;
    }

    out.print("GPS PROOF START timeout_s=");
    out.println(timeoutSeconds);

    if (!modemService.enableGps(out)) {
      out.println("GPS PROOF FAIL reason=gnss_enable_failed");
      gpsBuffer.begin();
      return;
    }

    modemService.gpsHotStart();
    const uint32_t start = millis();
    uint32_t sample = 0;
    while (millis() - start < timeoutSeconds * 1000UL) {
      GPSInfo info;
      const bool gotExtended = modemService.gpsExtended(info);
      const String raw = modemService.gpsRaw();
      const uint32_t elapsedSeconds = (millis() - start) / 1000UL;

      if (gotExtended) {
        printProofSample(elapsedSeconds, info, raw, out);
        if (proofPasses(info)) {
          out.println("GPS PROOF PASS");
          gpsBuffer.begin();
          return;
        }
      } else {
        out.print("GPS PROOF SAMPLE elapsed_s=");
        out.print(elapsedSeconds);
        out.println(" fix=none");
        out.print("GPS PROOF RAW ");
        out.println(raw);
      }

      ++sample;
      if (sample * 15UL >= timeoutSeconds) {
        break;
      }
      waitDuringGpsProof(15000);
    }

    out.println("GPS PROOF FAIL reason=timeout_no_real_fix");
    gpsBuffer.begin();
    return;
  }
  if (action == "hot") {
    out.println(modemService.gpsHotStart() ? "GNSS hot start OK." : "GNSS hot start failed.");
    return;
  }
  if (action == "cold") {
    out.println(modemService.gpsColdStart() ? "GNSS cold start OK." : "GNSS cold start failed.");
    return;
  }
  if (action == "status") {
    out.print("GNSS enabled: ");
    out.println(modemService.gpsEnabled() ? "yes" : "no");
    out.print("GNSS raw: ");
    out.println(modemService.gpsRaw());
    return;
  }

  out.println("Usage: gps on|off|raw|fix|ex|cache|prove [timeout_seconds]|hot|cold|status");
}

void executeCommand(String line, Stream& out)
{
  line.trim();
  if (line.length() == 0) {
    return;
  }

  String command = nextToken(line);
  command.toLowerCase();

  if (command == "help" || command == "?") {
    printHelp(out);
  } else if (command == "diag") {
    modemService.printDiagnostics(out, "console");
  } else if (command == "at") {
    out.println(modemService.rawAt(line, 10000));
  } else if (command == "sim") {
    handleSimCommand(line, out);
  } else if (command == "sms") {
    handleSmsCommand(line, out);
  } else if (command == "reg") {
    modemService.printRegistration(out);
    modemService.waitForEpsRegistration(120000, out);
  } else if (command == "operator") {
    String mode = nextToken(line);
    mode.toLowerCase();
    if (mode == "telekom") {
      modemService.selectOperatorTelekom(out);
    } else if (mode == "auto") {
      modemService.selectOperatorAuto(out);
    } else if (mode == "status") {
      modemService.printRegistrationSnapshot(out, "operator_status");
    } else {
      out.println("Usage: operator auto|telekom|status");
    }
  } else if (command == "rat") {
    String mode = nextToken(line);
    mode.toLowerCase();
    if (mode == "lte") {
      modemService.configureRat(true, out);
    } else if (mode == "auto") {
      modemService.configureRat(false, out);
    } else {
      out.println("Usage: rat auto|lte");
    }
  } else if (command == "data") {
    handleDataCommand(line, out);
  } else if (command == "mqtt") {
    handleMqttCommand(line, out);
  } else if (command == "ota") {
    handleOtaCommand(line, out);
  } else if (command == "gsm") {
    handleGsmCommand(line, out);
  } else if (command == "http") {
    String host = nextToken(line);
    String path = nextToken(line);
    if (host.length() == 0) {
      out.println("Usage: http <host> [path]");
      return;
    }
    if (path.length() == 0) {
      path = "/";
    }
    if (!path.startsWith("/")) {
      path = "/" + path;
    }
    modemService.httpGet(host.c_str(), path.c_str(), 80, out);
  } else if (command == "gps") {
    handleGpsCommand(line, out);
  } else if (command == "wifi") {
    String action = nextToken(line);
    action.toLowerCase();
    if (action == "status") {
      printWifiStatus(out);
    } else {
      out.println("Usage: wifi status");
    }
  } else {
    out.print("Unknown command: ");
    out.println(command);
    printHelp(out);
  }
}

void pollStreamConsole(Stream& io, String& buffer)
{
  while (io.available()) {
    char c = static_cast<char>(io.read());
    if (c == '\r' || c == '\n') {
      if (buffer.length() > 0) {
        String line = buffer;
        buffer = "";
        executeCommand(line, io);
        io.print("> ");
      }
    } else if (isprint(static_cast<unsigned char>(c))) {
      buffer += c;
      if (buffer.length() > 180) {
        buffer.remove(0, buffer.length() - 180);
      }
    }
  }
}

void setupWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting WiFi to ");
  Serial.print(WIFI_SSID);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print('.');
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connect timeout. USB serial console remains available.");
    return;
  }

  wifiConsoleServer.begin();
  wifiConsoleServer.setNoDelay(true);

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(WIFI_PASSWORD);
  ArduinoOTA
      .onStart([]() { Serial.println("OTA start."); })
      .onEnd([]() { Serial.println("OTA end."); })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA progress: %u%%\r", (progress * 100U) / total);
      })
      .onError([](ota_error_t error) {
        Serial.printf("OTA error[%u]\n", error);
      });
  ArduinoOTA.begin();

  printWifiStatus(Serial);
}

void pollWifiConsole()
{
  if (wifiConsoleServer.hasClient()) {
    WiFiClient nextClient = wifiConsoleServer.available();
    if (!wifiConsoleClient || !wifiConsoleClient.connected()) {
      wifiConsoleClient = nextClient;
      wifiConsoleClient.setNoDelay(true);
      wifiCommandLine = "";
      wifiConsoleClient.println("T-Call A7670 v1.0 WiFi console");
      printWifiStatus(wifiConsoleClient);
      printHelp(wifiConsoleClient);
      wifiConsoleClient.print("> ");
    } else {
      nextClient.println("Console busy. Only one WiFi client is supported.");
      nextClient.stop();
    }
  }

  if (wifiConsoleClient && wifiConsoleClient.connected()) {
    pollStreamConsole(wifiConsoleClient, wifiCommandLine);
  } else if (wifiConsoleClient) {
    wifiConsoleClient.stop();
  }
}

void pollMqtt()
{
  if (remoteManager) {
    remoteManager->loop();
  }
}

bool remoteOtaCommand(const char* command, Stream& out)
{
  String args = command;
  args.trim();
  if (args.length() == 0 || args == "github_latest") {
    args = "update";
  }
  handleOtaCommand(args, out);
  return true;
}

bool remoteSetTransport(const char* mode, Stream& out)
{
  String requested = mode ? mode : "";
  requested.trim();
  requested.toLowerCase();
  if (requested == "lte" || requested == "cellular") {
    if (!ensureCellularData(out)) {
      out.println("cellular data connection failed");
      return false;
    }
  }
  return switchableMqttTransport.setMode(requested.c_str(), out);
}

void prepareNetworksForStandbyProbe()
{
  if (standbyNetworksPrepared) {
    return;
  }

  if (switchableMqttTransport.usingCellular()) {
    modemService.wakeFromStandby(Serial);
    ensureCellularData(Serial);
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      setupWifi();
    }
  }

  if (remoteManager) {
    remoteManager->reconnectNow();
  }
  standbyNetworksPrepared = true;
}

void enterTimedStandby()
{
  if (!remoteManager) {
    return;
  }

  Serial.println("RemoteDeviceManager standby timeout; entering timed standby.");
  remoteManager->publishSleepState("sleeping", RDM_STANDBY_WAKE_INTERVAL_MS, "rdm_idle_timeout");
  delay(150);
  remoteManager->reconnectNow();
  gpsBuffer.stop();
  modemService.enterStandby(Serial);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(RDM_STANDBY_WAKE_INTERVAL_MS) * 1000ULL);
  esp_light_sleep_start();

  Serial.println("RemoteDeviceManager standby wake; probing MQTT.");
  standbyProbeStartMs = millis();
  standbyNetworksPrepared = false;
  remotePowerState = RemotePowerState::StandbyProbe;
}

void handleRemotePowerState()
{
  if (!RDM_STANDBY_ENABLED || !remoteManager) {
    return;
  }

  const uint32_t now = millis();
  if (remotePowerState == RemotePowerState::Awake) {
    if (now - lastRemoteActivityMs >= RDM_STANDBY_AFTER_MS) {
      remotePowerState = RemotePowerState::EnteringStandby;
    }
    return;
  }

  if (remotePowerState == RemotePowerState::EnteringStandby) {
    enterTimedStandby();
    return;
  }

  if (remotePowerState == RemotePowerState::StandbyProbe) {
    prepareNetworksForStandbyProbe();
    ArduinoOTA.handle();
    pollMqtt();
    if (remotePowerState == RemotePowerState::Awake) {
      if (remoteManager) {
        remoteManager->publishSleepState("awake", 0, "remote_activity");
      }
      return;
    }
    if (millis() - standbyProbeStartMs >= RDM_STANDBY_PROBE_WINDOW_MS) {
      remotePowerState = RemotePowerState::EnteringStandby;
    }
  }
}

void setupRemoteDeviceManager()
{
  if (!mqttConfigured()) {
    Serial.println("RemoteDeviceManager disabled: TCALL_MQTT_HOST is empty.");
    return;
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed; RemoteDeviceManager file functions disabled.");
  }

  static RemoteDeviceManager::Config config;
  config.deviceId = DEVICE_ID;
  config.topicPrefix = MQTT_TOPIC_PREFIX;
  config.mqttHost = MQTT_HOST;
  config.mqttPort = MQTT_PORT;
  config.mqttUser = MQTT_USER;
  config.mqttPass = MQTT_PASS;
  config.mqttClientId = MQTT_CLIENT_ID;
  config.sharedSecret = RDM_SHARED_SECRET;
  config.firmwareVersion = FW_VERSION;
  config.filesystem = &SPIFFS;
  config.httpClient = &remoteHttpClient;
  config.log = &Serial;
  config.consoleCommand = executeCommand;
  config.statusJson = statusJson;
  config.gpsJson = gpsJson;
  config.otaCommand = remoteOtaCommand;
  config.transportSet = remoteSetTransport;
  config.connectivityJson = connectivityJson;
  config.activity = noteRemoteActivity;

  switchableMqttTransport.setMode(MQTT_TRANSPORT, Serial);
  remoteManager = new RemoteDeviceManager::Manager(switchableMqttTransport);
  remoteManager->begin(config);
  lastRemoteActivityMs = millis();
  Serial.print("RemoteDeviceManager started with ");
  Serial.println(remoteManager->transportName());
}

}  // namespace

void setup()
{
  Serial.begin(tcall::CONSOLE_BAUD);
  delay(200);
  Serial.println();
  Serial.println("T-Call A7670 v1.0 GSM/GNSS console");

  setupWifi();
  modemService.begin(Serial);
  modemService.waitOnline(45000, Serial);
  if (mqttUseCellular() || RDM_LTE_ALIVE_AUTOSTART) {
    ensureCellularData(Serial);
  }
  setupRemoteDeviceManager();
  if (GPS_AUTOSTART) {
    Serial.println("GPS autostart enabled.");
    gpsBuffer.begin();
  }
  printHelp(Serial);
  Serial.print("> ");
}

void loop()
{
  pollStreamConsole(Serial, serialCommandLine);
  handleRemotePowerState();
  if (remotePowerState != RemotePowerState::Awake) {
    delay(1);
    return;
  }

  ArduinoOTA.handle();
  gpsBuffer.runner();
  pollWifiConsole();
  pollMqtt();
  delay(1);
}
