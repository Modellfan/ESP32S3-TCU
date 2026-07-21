#include "RemoteDeviceManager.h"

#include <ctype.h>
#include <string.h>

namespace RemoteDeviceManager {
namespace {

constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;
constexpr uint32_t STATE_INTERVAL_MS = 30000;
constexpr uint32_t HTTP_TIMEOUT_MS = 30000;
constexpr size_t HTTP_BUFFER_SIZE = 512;

bool hasText(const char* value)
{
  return value != nullptr && strlen(value) > 0;
}

String escapeJson(const String& value)
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

String withDefaultSleepState(String payload)
{
  if (payload.indexOf("\"sleep_state\"") >= 0 || payload.length() < 2) {
    return payload;
  }
  payload.trim();
  if (!payload.startsWith("{") || !payload.endsWith("}")) {
    return payload;
  }
  payload.remove(payload.length() - 1);
  payload += ",\"sleep_state\":\"awake\"}";
  return payload;
}

uint32_t parseHex32(const String& value)
{
  uint32_t out = 0;
  uint8_t digits = 0;
  for (int i = 0; i < static_cast<int>(value.length()); ++i) {
    char c = value[i];
    int nibble = -1;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      nibble = c - 'A' + 10;
    }
    if (nibble >= 0) {
      out = (out << 4) | static_cast<uint32_t>(nibble);
      if (++digits == 8) {
        return out;
      }
    } else {
      out = 0;
      digits = 0;
    }
  }
  return 0;
}

bool readLine(Client& client, String& line, uint32_t timeoutMs)
{
  line = "";
  uint32_t last = millis();
  while (millis() - last < timeoutMs) {
    while (client.available()) {
      const char c = static_cast<char>(client.read());
      last = millis();
      if (c == '\n') {
        line.trim();
        return true;
      }
      line += c;
      if (line.length() > 1024) {
        return false;
      }
    }
    delay(1);
  }
  return false;
}

}  // namespace

String jsonValue(const String& json, const char* key)
{
  String marker = "\"";
  marker += key;
  marker += "\"";
  int keyPos = json.indexOf(marker);
  if (keyPos < 0) {
    return "";
  }
  int colon = json.indexOf(':', keyPos + marker.length());
  if (colon < 0) {
    return "";
  }
  int valueStart = colon + 1;
  while (valueStart < static_cast<int>(json.length()) &&
         isspace(static_cast<unsigned char>(json[valueStart]))) {
    ++valueStart;
  }
  if (valueStart >= static_cast<int>(json.length())) {
    return "";
  }
  if (json[valueStart] == '"') {
    String value;
    bool escaped = false;
    for (int i = valueStart + 1; i < static_cast<int>(json.length()); ++i) {
      const char c = json[i];
      if (escaped) {
        if (c == 'n') {
          value += '\n';
        } else if (c == 'r') {
          value += '\r';
        } else {
          value += c;
        }
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        return value;
      } else {
        value += c;
      }
    }
    return "";
  }
  int end = valueStart;
  while (end < static_cast<int>(json.length()) && json[end] != ',' && json[end] != '}') {
    ++end;
  }
  String value = json.substring(valueStart, end);
  value.trim();
  return value;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len)
{
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

WiFiMqttTransport::WiFiMqttTransport(Client& client) : client_(client) {}

bool WiFiMqttTransport::connect(const char* host,
                                uint16_t port,
                                const char* clientId,
                                const char* user,
                                const char* pass,
                                Stream* log)
{
  client_.stop();
  if (!client_.connect(host, port)) {
    if (log) {
      (*log).println("WiFi MQTT TCP connect failed.");
    }
    return false;
  }

  const bool hasUser = hasText(user);
  const bool hasPass = hasText(pass);
  size_t remaining = 10 + 2 + strlen(clientId);
  if (hasUser) {
    remaining += 2 + strlen(user);
  }
  if (hasPass) {
    remaining += 2 + strlen(pass);
  }

  client_.write(0x10);
  writeRemainingLength(remaining);
  writeString("MQTT");
  client_.write(4);
  uint8_t flags = 0x02;
  if (hasUser) flags |= 0x80;
  if (hasPass) flags |= 0x40;
  client_.write(flags);
  client_.write(0);
  client_.write(30);
  writeString(clientId);
  if (hasUser) writeString(user);
  if (hasPass) writeString(pass);

  uint8_t b = 0;
  if (!readByte(b, 5000) || b != 0x20 || !readByte(b, 5000) || b != 0x02 ||
      !readByte(b, 5000) || !readByte(b, 5000) || b != 0x00) {
    client_.stop();
    if (log) {
      (*log).println("WiFi MQTT CONNACK failed.");
    }
    return false;
  }
  return true;
}

bool WiFiMqttTransport::connected() const
{
  return client_.connected();
}

bool WiFiMqttTransport::publish(const char* topic, const char* payload, bool retain, Stream*)
{
  if (!client_.connected()) {
    return false;
  }
  const size_t payloadLen = strlen(payload);
  const size_t topicLen = strlen(topic);
  client_.write(retain ? 0x31 : 0x30);
  writeRemainingLength(2 + topicLen + payloadLen);
  writeString(topic);
  client_.write(reinterpret_cast<const uint8_t*>(payload), payloadLen);
  return true;
}

bool WiFiMqttTransport::subscribe(const char* topic, Stream*)
{
  if (!client_.connected()) {
    return false;
  }
  const uint16_t id = packetId_++;
  const size_t topicLen = strlen(topic);
  client_.write(0x82);
  writeRemainingLength(2 + 2 + topicLen + 1);
  client_.write(static_cast<uint8_t>(id >> 8));
  client_.write(static_cast<uint8_t>(id & 0xFF));
  writeString(topic);
  client_.write(0);
  return true;
}

bool WiFiMqttTransport::poll(MqttMessage& message)
{
  if (!client_.connected() || !client_.available()) {
    return false;
  }
  uint8_t type = 0;
  uint8_t flags = 0;
  return readPacket(type, flags, message.topic, message.payload) && type == 3;
}

void WiFiMqttTransport::disconnect(Stream*)
{
  if (client_.connected()) {
    client_.write(0xE0);
    client_.write(0x00);
  }
  client_.stop();
}

bool WiFiMqttTransport::readPacket(uint8_t& type, uint8_t& flags, String& topic, String& payload)
{
  uint8_t first = 0;
  if (!readByte(first, 10)) {
    return false;
  }
  type = first >> 4;
  flags = first & 0x0F;
  uint32_t multiplier = 1;
  uint32_t remaining = 0;
  uint8_t digit = 0;
  do {
    if (!readByte(digit, 1000)) {
      return false;
    }
    remaining += (digit & 127) * multiplier;
    multiplier *= 128;
  } while (digit & 128);

  if (type == 3) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!readByte(hi, 1000) || !readByte(lo, 1000)) {
      return false;
    }
    const uint16_t topicLen = (static_cast<uint16_t>(hi) << 8) | lo;
    remaining -= 2;
    topic = "";
    for (uint16_t i = 0; i < topicLen; ++i) {
      uint8_t b = 0;
      if (!readByte(b, 1000)) {
        return false;
      }
      topic += static_cast<char>(b);
    }
    remaining -= topicLen;
    payload = "";
    payload.reserve(remaining);
    while (remaining-- > 0) {
      uint8_t b = 0;
      if (!readByte(b, 1000)) {
        return false;
      }
      payload += static_cast<char>(b);
    }
    return true;
  }

  while (remaining-- > 0) {
    uint8_t ignored = 0;
    if (!readByte(ignored, 1000)) {
      return false;
    }
  }
  return false;
}

bool WiFiMqttTransport::writeRemainingLength(size_t length)
{
  do {
    uint8_t digit = length % 128;
    length /= 128;
    if (length > 0) digit |= 0x80;
    client_.write(digit);
  } while (length > 0);
  return true;
}

void WiFiMqttTransport::writeString(const char* value)
{
  const size_t len = strlen(value);
  client_.write(static_cast<uint8_t>(len >> 8));
  client_.write(static_cast<uint8_t>(len & 0xFF));
  client_.write(reinterpret_cast<const uint8_t*>(value), len);
}

bool WiFiMqttTransport::readByte(uint8_t& value, uint32_t timeoutMs)
{
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (client_.available()) {
      value = static_cast<uint8_t>(client_.read());
      return true;
    }
    delay(1);
  }
  return false;
}

Manager::Manager(MqttTransport& mqtt) : mqtt_(mqtt) {}

void Manager::begin(const Config& config)
{
  config_ = config;
  publishState();
}

void Manager::loop()
{
  ensureConnected();
  MqttMessage message;
  while (mqtt_.poll(message)) {
    handleMqttMessage(message.topic, message.payload);
  }
  const uint32_t now = millis();
  if (now - nextTelemetryMs_ >= TELEMETRY_INTERVAL_MS) {
    nextTelemetryMs_ = now;
    publishTelemetry();
  }
  if (now - nextStateMs_ >= STATE_INTERVAL_MS) {
    nextStateMs_ = now;
    publishState();
  }
}

bool Manager::publishState()
{
  if (!ensureConnected()) {
    return false;
  }
  String payload;
  payload.reserve(384);
  payload += "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"fw_version\":\"";
  payload += config_.firmwareVersion;
  payload += "\",\"transport\":\"";
  payload += mqtt_.name();
  payload += "\",\"mqtt_connected\":";
  payload += mqtt_.connected() ? "true" : "false";
  payload += ",\"sleep_state\":\"awake\"";
  payload += ",\"uptime_ms\":";
  payload += millis();
  if (config_.filesystem) {
    payload += ",\"fs\":{\"used\":";
    payload += 0;
    payload += ",\"total\":";
    payload += 0;
    payload += "}";
  }
  payload += "}";
  const bool ok = mqtt_.publish(topic("rdm/state").c_str(), payload.c_str(), true, config_.log);
  publishConsoleState();
  publishFileState();
  publishOtaState();
  return ok;
}

bool Manager::publishSleepState(const char* sleepState, uint32_t sleepMs, const char* reason)
{
  if (!ensureConnected()) {
    return false;
  }
  String payload;
  payload.reserve(320);
  payload += "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"fw_version\":\"";
  payload += config_.firmwareVersion;
  payload += "\",\"transport\":\"";
  payload += mqtt_.name();
  payload += "\",\"mqtt_connected\":";
  payload += mqtt_.connected() ? "true" : "false";
  payload += ",\"sleep_state\":\"";
  payload += escapeJson(sleepState && strlen(sleepState) ? sleepState : "sleeping");
  payload += "\",\"sleep_ms\":";
  payload += sleepMs;
  payload += ",\"sleep_reason\":\"";
  payload += escapeJson(reason ? reason : "");
  payload += "\",\"uptime_ms\":";
  payload += millis();
  payload += "}";
  const bool ok = mqtt_.publish(topic("rdm/state").c_str(), payload.c_str(), true, config_.log);
  MqttMessage ignored;
  mqtt_.poll(ignored);
  delay(100);
  return ok;
}

bool Manager::publishTelemetry()
{
  if (!ensureConnected()) {
    return false;
  }
  bool ok = true;
  if (config_.statusJson) {
    const String status = config_.statusJson();
    ok = mqtt_.publish(topic("status").c_str(), status.c_str(), true, config_.log) && ok;
  }
  if (config_.gpsJson) {
    const String gps = config_.gpsJson();
    ok = mqtt_.publish(topic("gps").c_str(), gps.c_str(), true, config_.log) && ok;
  }
  return ok;
}

void Manager::handleMqttMessage(const String& topicName, const String& payload)
{
  if (topicName == topic("console/cmd")) {
    handleConsoleCommand(payload);
  } else if (topicName == topic("fs/jobs")) {
    handleFileJob(payload);
  } else if (topicName == topic("ota/jobs")) {
    handleOtaJob(payload);
  } else if (topicName == topic("rdm/alive/request")) {
    handleAliveRequest(payload);
  } else if (topicName == topic("rdm/transport/set")) {
    handleTransportSet(payload);
  }
}

bool Manager::connected() const
{
  return mqtt_.connected();
}

const char* Manager::transportName() const
{
  return mqtt_.name();
}

size_t Manager::CaptureStream::write(uint8_t value)
{
  if (text_.length() < 3500) {
    text_ += static_cast<char>(value);
  }
  return 1;
}

bool Manager::ensureConnected()
{
  if (mqtt_.connected()) {
    if (!subscribed_) {
      subscribeTopics();
    }
    return true;
  }
  const uint32_t now = millis();
  if (now < nextReconnectMs_) {
    return false;
  }
  nextReconnectMs_ = now + MQTT_RECONNECT_INTERVAL_MS;
  subscribed_ = false;
  if (!hasText(config_.mqttHost)) {
    return false;
  }
  if (!mqtt_.connect(config_.mqttHost, config_.mqttPort, config_.mqttClientId, config_.mqttUser,
                     config_.mqttPass, config_.log)) {
    return false;
  }
  subscribeTopics();
  return true;
}

void Manager::subscribeTopics()
{
  mqtt_.subscribe(topic("console/cmd").c_str(), config_.log);
  mqtt_.subscribe(topic("fs/jobs").c_str(), config_.log);
  mqtt_.subscribe(topic("ota/jobs").c_str(), config_.log);
  mqtt_.subscribe(topic("rdm/alive/request").c_str(), config_.log);
  mqtt_.subscribe(topic("rdm/transport/set").c_str(), config_.log);
  subscribed_ = true;
}

void Manager::handleAliveRequest(const String& payload)
{
  publishAlive(payload);
}

void Manager::handleTransportSet(const String& payload)
{
  const String mode = jsonValue(payload, "mode");
  String detail;
  bool ok = false;
  if (!config_.transportSet || mode.length() == 0) {
    detail = "transport switch unavailable";
  } else {
    CaptureStream capture;
    ok = config_.transportSet(mode.c_str(), capture);
    detail = capture.text();
    mqtt_.disconnect(config_.log);
    subscribed_ = false;
    nextReconnectMs_ = 0;
  }
  publishTransportState(mode.c_str(), ok, detail);
}

bool Manager::publishAlive(const String& requestPayload)
{
  if (!ensureConnected()) {
    return false;
  }
  if (config_.connectivityJson) {
    const String payload = withDefaultSleepState(config_.connectivityJson(requestPayload));
    return mqtt_.publish(topic("rdm/alive").c_str(), payload.c_str(), false, config_.log);
  }
  const String requestId = jsonValue(requestPayload, "request_id");
  String status = config_.statusJson ? config_.statusJson() : "{}";
  const String wifiConnected = jsonValue(status, "wifi_connected");
  const String wifiRssi = jsonValue(status, "wifi_rssi");
  const String wifiIp = jsonValue(status, "ip");
  const String transport = jsonValue(status, "mqtt_transport");
  const bool lteConnected = transport.indexOf("simcom") >= 0 || transport.indexOf("cellular") >= 0 ||
                            transport.indexOf("lte") >= 0;
  const char* activeLink = lteConnected ? "lte" : "wifi";

  String payload;
  payload.reserve(420);
  payload += "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"request_id\":\"";
  payload += escapeJson(requestId);
  payload += "\",\"uptime_ms\":";
  payload += millis();
  payload += ",\"active_link\":\"";
  payload += activeLink;
  payload += "\",\"wifi\":{\"connected\":";
  payload += wifiConnected.length() ? wifiConnected : "false";
  payload += ",\"ip\":\"";
  payload += escapeJson(wifiIp);
  payload += "\",\"rssi\":";
  payload += wifiRssi.length() ? wifiRssi : "0";
  payload += "},\"lte\":{\"connected\":";
  payload += lteConnected ? "true" : "false";
  payload += ",\"ip\":\"\"},\"transport\":\"";
  payload += escapeJson(transport.length() ? transport : mqtt_.name());
  payload += "\"}";
  return mqtt_.publish(topic("rdm/alive").c_str(), payload.c_str(), false, config_.log);
}

bool Manager::publishTransportState(const char* requestedMode, bool ok, const String& detail)
{
  if (!ensureConnected()) {
    return false;
  }
  String payload;
  payload.reserve(260);
  payload += "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"requested_mode\":\"";
  payload += escapeJson(requestedMode ? requestedMode : "");
  payload += "\",\"active_transport\":\"";
  payload += mqtt_.name();
  payload += "\",\"status\":\"";
  payload += ok ? "ok" : "error";
  payload += "\",\"detail\":\"";
  payload += escapeJson(detail);
  payload += "\",\"uptime_ms\":";
  payload += millis();
  payload += "}";
  return mqtt_.publish(topic("rdm/transport/state").c_str(), payload.c_str(), true, config_.log);
}

void Manager::publishConsoleState()
{
  String payload = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"enabled\":true,\"busy\":false,\"uptime_ms\":";
  payload += millis();
  payload += "}";
  mqtt_.publish(topic("console/state").c_str(), payload.c_str(), true, config_.log);
}

void Manager::handleConsoleCommand(const String& payload)
{
  const String sessionId = jsonValue(payload, "session_id");
  const String commandId = jsonValue(payload, "command_id");
  const String command = jsonValue(payload, "command");
  if (!config_.consoleCommand || command.length() == 0) {
    publishConsoleOutput(sessionId, commandId, "Console command unavailable\n", true, 2, 0);
    return;
  }
  CaptureStream capture;
  const uint32_t start = millis();
  config_.consoleCommand(command, capture);
  publishConsoleOutput(sessionId, commandId, capture.text(), false, 0, millis() - start);
  publishConsoleOutput(sessionId, commandId, "", true, 0, millis() - start);
}

void Manager::handleFileJob(const String& payload)
{
  const String jobId = jsonValue(payload, "job_id");
  const String op = jsonValue(payload, "op");
  if (op == "list") {
    listFiles(jobId);
  } else if (op == "put_url") {
    putUrl(jobId, jsonValue(payload, "path"), jsonValue(payload, "url"),
           parseHex32(jsonValue(payload, "crc32")));
  } else if (op == "get_url") {
    getUrl(jobId, jsonValue(payload, "path"), jsonValue(payload, "url"));
  } else if (op == "delete") {
    deletePath(jobId, jsonValue(payload, "path"));
  } else {
    publishResult("fs/result", jobId, op.c_str(), "rejected", "unsupported_op");
  }
}

void Manager::handleOtaJob(const String& payload)
{
  const String jobId = jsonValue(payload, "job_id");
  const String op = jsonValue(payload, "op");
  if (op == "github_latest") {
    if (!config_.otaCommand) {
      publishResult("ota/result", jobId, op.c_str(), "rejected", "github_ota_unavailable");
      return;
    }
    CaptureStream capture;
    const bool ok = config_.otaCommand("update", capture);
    publishResult("ota/result", jobId, op.c_str(), ok ? "ok" : "failed", capture.text().c_str());
  } else if (op == "ota_url") {
    otaUrl(jobId, jsonValue(payload, "url"), parseHex32(jsonValue(payload, "crc32")),
           static_cast<size_t>(jsonValue(payload, "size").toInt()));
  } else {
    publishResult("ota/result", jobId, op.c_str(), "rejected", "unsupported_op");
  }
}

void Manager::publishResult(const char* topicSuffix,
                            const String& jobId,
                            const char* op,
                            const char* status,
                            const char* detail)
{
  String payload = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"job_id\":\"";
  payload += escapeJson(jobId);
  payload += "\",\"op\":\"";
  payload += escapeJson(op);
  payload += "\",\"status\":\"";
  payload += status;
  payload += "\"";
  if (detail && strlen(detail) > 0) {
    payload += ",\"detail\":\"";
    payload += escapeJson(detail);
    payload += "\"";
  }
  payload += ",\"uptime_ms\":";
  payload += millis();
  payload += "}";
  mqtt_.publish(topic(topicSuffix).c_str(), payload.c_str(), false, config_.log);
}

void Manager::publishConsoleOutput(const String& sessionId,
                                   const String& commandId,
                                   const String& text,
                                   bool final,
                                   int exitCode,
                                   uint32_t durationMs)
{
  String payload = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"session_id\":\"";
  payload += escapeJson(sessionId);
  payload += "\",\"command_id\":\"";
  payload += escapeJson(commandId);
  payload += "\",\"seq\":";
  payload += msgCounter_++;
  payload += ",\"stream\":\"";
  payload += final ? "status" : "stdout";
  payload += "\",\"encoding\":\"text\",\"payload\":\"";
  payload += escapeJson(text);
  payload += "\",\"final\":";
  payload += final ? "true" : "false";
  if (final) {
    payload += ",\"exit_code\":";
    payload += exitCode;
    payload += ",\"duration_ms\":";
    payload += durationMs;
  }
  payload += "}";
  mqtt_.publish(topic("console/out").c_str(), payload.c_str(), false, config_.log);
}

void Manager::publishFileState()
{
  if (!config_.filesystem) {
    return;
  }
  String payload = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"type\":\"spiffs\",\"used\":";
  payload += 0;
  payload += ",\"total\":";
  payload += 0;
  payload += ",\"capabilities\":{\"list\":true,\"put_url\":true,\"get_url\":true,\"delete\":true}}";
  mqtt_.publish(topic("fs/state").c_str(), payload.c_str(), true, config_.log);
}

void Manager::publishOtaState()
{
  String payload = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"fw_version\":\"";
  payload += config_.firmwareVersion;
  payload += "\",\"capabilities\":{\"github_latest\":true,\"ota_url\":true}}";
  mqtt_.publish(topic("ota/state").c_str(), payload.c_str(), true, config_.log);
}

bool Manager::listFiles(const String& jobId)
{
  if (!config_.filesystem) {
    publishResult("fs/result", jobId, "list", "failed", "filesystem_unavailable");
    return false;
  }
  String payload = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"job_id\":\"";
  payload += escapeJson(jobId);
  payload += "\",\"op\":\"list\",\"status\":\"ok\",\"files\":[";
  bool first = true;
  File root = config_.filesystem->open("/");
  if (root) {
    File file = root.openNextFile();
    while (file) {
      if (!first) payload += ',';
      first = false;
      payload += "{\"path\":\"";
      if (file.name()[0] != '/') payload += '/';
      payload += escapeJson(file.name());
      payload += "\",\"size\":";
      payload += static_cast<uint32_t>(file.size());
      payload += ",\"dir\":";
      payload += file.isDirectory() ? "true" : "false";
      payload += "}";
      file = root.openNextFile();
    }
  }
  payload += "]}";
  return mqtt_.publish(topic("fs/result").c_str(), payload.c_str(), false, config_.log);
}

bool Manager::putUrl(const String& jobId, const String& path, const String& url, uint32_t expectedCrc)
{
  const bool ok = httpDownloadToFile(url, normalizePath(path), expectedCrc);
  publishResult("fs/result", jobId, "put_url", ok ? "ok" : "failed", ok ? nullptr : "download_failed");
  publishFileState();
  return ok;
}

bool Manager::getUrl(const String& jobId, const String& path, const String& url)
{
  const bool ok = httpUploadFile(url, normalizePath(path));
  publishResult("fs/result", jobId, "get_url", ok ? "ok" : "failed", ok ? nullptr : "upload_failed");
  return ok;
}

bool Manager::deletePath(const String& jobId, const String& path)
{
  const String normalized = normalizePath(path);
  bool ok = false;
  if (config_.filesystem && pathAllowed(normalized) && config_.filesystem->exists(normalized)) {
    ok = config_.filesystem->remove(normalized);
  }
  publishResult("fs/result", jobId, "delete", ok ? "ok" : "failed", ok ? nullptr : "delete_failed");
  publishFileState();
  return ok;
}

bool Manager::otaUrl(const String& jobId, const String& url, uint32_t expectedCrc, size_t size)
{
  const bool ok = httpDownloadToOta(url, expectedCrc, size);
  publishResult("ota/result", jobId, "ota_url", ok ? "ok" : "failed", ok ? nullptr : "ota_failed");
  if (ok) {
    delay(1000);
    ESP.restart();
  }
  return ok;
}

bool Manager::httpDownloadToFile(const String& url, const String& path, uint32_t expectedCrc)
{
  if (!config_.filesystem || !config_.httpClient || !pathAllowed(path)) {
    return false;
  }
  String host;
  String requestPath;
  uint16_t port = 80;
  if (!parseUrl(url, host, port, requestPath)) {
    return false;
  }
  Client& client = *config_.httpClient;
  client.stop();
  if (!client.connect(host.c_str(), port)) {
    return false;
  }
  client.print("GET ");
  client.print(requestPath);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(host);
  client.print("\r\nConnection: close\r\n\r\n");
  String line;
  if (!readLine(client, line, HTTP_TIMEOUT_MS) || line.indexOf(" 200 ") < 0) {
    client.stop();
    return false;
  }
  int contentLength = -1;
  while (readLine(client, line, HTTP_TIMEOUT_MS) && line.length() > 0) {
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(15).toInt();
    }
  }
  File file = config_.filesystem->open(path, FILE_WRITE);
  if (!file) {
    client.stop();
    return false;
  }
  uint8_t buffer[HTTP_BUFFER_SIZE];
  uint32_t crc = 0;
  int remaining = contentLength;
  uint32_t last = millis();
  while ((client.connected() || client.available()) && millis() - last < HTTP_TIMEOUT_MS) {
    size_t got = 0;
    while (client.available() && got < sizeof(buffer)) {
      buffer[got++] = static_cast<uint8_t>(client.read());
      last = millis();
    }
    if (got > 0) {
      crc = crc32Update(crc, buffer, got);
      if (file.write(buffer, got) != got) {
        file.close();
        client.stop();
        config_.filesystem->remove(path);
        return false;
      }
      if (remaining > 0) {
        remaining -= static_cast<int>(got);
        if (remaining <= 0) break;
      }
    }
    delay(1);
  }
  file.close();
  client.stop();
  if ((contentLength > 0 && remaining > 0) || (expectedCrc != 0 && crc != expectedCrc)) {
    config_.filesystem->remove(path);
    return false;
  }
  return true;
}

bool Manager::httpUploadFile(const String& url, const String& path)
{
  if (!config_.filesystem || !config_.httpClient || !pathAllowed(path)) {
    return false;
  }
  File file = config_.filesystem->open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    return false;
  }
  String host;
  String requestPath;
  uint16_t port = 80;
  if (!parseUrl(url, host, port, requestPath)) {
    return false;
  }
  Client& client = *config_.httpClient;
  client.stop();
  if (!client.connect(host.c_str(), port)) {
    return false;
  }
  client.print("PUT ");
  client.print(requestPath);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(host);
  client.print("\r\nContent-Length: ");
  client.print(file.size());
  client.print("\r\nConnection: close\r\n\r\n");
  uint8_t buffer[HTTP_BUFFER_SIZE];
  while (file.available()) {
    const size_t got = file.read(buffer, sizeof(buffer));
    if (got > 0) {
      client.write(buffer, got);
    }
  }
  String line;
  const bool ok = readLine(client, line, HTTP_TIMEOUT_MS) && line.indexOf(" 200 ") >= 0;
  client.stop();
  return ok;
}

bool Manager::httpDownloadToOta(const String& url, uint32_t expectedCrc, size_t size)
{
  if (!config_.httpClient || expectedCrc == 0) {
    return false;
  }
  String host;
  String requestPath;
  uint16_t port = 80;
  if (!parseUrl(url, host, port, requestPath)) {
    return false;
  }
  Client& client = *config_.httpClient;
  client.stop();
  if (!client.connect(host.c_str(), port)) {
    return false;
  }
  client.print("GET ");
  client.print(requestPath);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(host);
  client.print("\r\nConnection: close\r\n\r\n");
  String line;
  if (!readLine(client, line, HTTP_TIMEOUT_MS) || line.indexOf(" 200 ") < 0) {
    client.stop();
    return false;
  }
  int contentLength = -1;
  while (readLine(client, line, HTTP_TIMEOUT_MS) && line.length() > 0) {
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(15).toInt();
    }
  }
  const size_t updateSize = size > 0 ? size : (contentLength > 0 ? static_cast<size_t>(contentLength) : UPDATE_SIZE_UNKNOWN);
  if (!Update.begin(updateSize)) {
    client.stop();
    return false;
  }
  uint8_t buffer[HTTP_BUFFER_SIZE];
  uint32_t crc = 0;
  size_t written = 0;
  uint32_t last = millis();
  while ((client.connected() || client.available()) && millis() - last < HTTP_TIMEOUT_MS) {
    size_t got = 0;
    while (client.available() && got < sizeof(buffer)) {
      buffer[got++] = static_cast<uint8_t>(client.read());
      last = millis();
    }
    if (got > 0) {
      crc = crc32Update(crc, buffer, got);
      if (Update.write(buffer, got) != got) {
        Update.abort();
        client.stop();
        return false;
      }
      written += got;
      if (contentLength > 0 && written >= static_cast<size_t>(contentLength)) break;
    }
    delay(1);
  }
  client.stop();
  if ((contentLength > 0 && written != static_cast<size_t>(contentLength)) || crc != expectedCrc) {
    Update.abort();
    return false;
  }
  return Update.end(true);
}

bool Manager::parseUrl(const String& url, String& host, uint16_t& port, String& path)
{
  if (!url.startsWith("http://")) {
    return false;
  }
  int hostStart = 7;
  int pathStart = url.indexOf('/', hostStart);
  String hostPort = pathStart >= 0 ? url.substring(hostStart, pathStart) : url.substring(hostStart);
  path = pathStart >= 0 ? url.substring(pathStart) : "/";
  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
  } else {
    host = hostPort;
    port = 80;
  }
  return host.length() > 0 && port > 0;
}

String Manager::normalizePath(const String& path) const
{
  String normalized = path;
  normalized.trim();
  normalized.replace("\\", "/");
  while (normalized.indexOf("//") >= 0) {
    normalized.replace("//", "/");
  }
  if (!normalized.startsWith("/")) {
    normalized = "/" + normalized;
  }
  if (normalized.indexOf("/../") >= 0 || normalized.endsWith("/..") ||
      normalized.indexOf("/./") >= 0 || normalized.endsWith("/.")) {
    return "";
  }
  return normalized;
}

bool Manager::pathAllowed(const String& path) const
{
  return path.length() > 1 && path.startsWith("/") && path.indexOf("..") < 0 &&
         !path.startsWith("/device-secret") && !path.startsWith("/certs/");
}

String Manager::topic(const char* suffix) const
{
  String out = config_.topicPrefix;
  if (!out.endsWith("/")) {
    out += '/';
  }
  out += suffix;
  return out;
}

}  // namespace RemoteDeviceManager
