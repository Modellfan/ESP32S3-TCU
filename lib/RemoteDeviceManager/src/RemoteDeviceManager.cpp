#include "RemoteDeviceManager.h"

#include <ctype.h>
#include <string.h>

#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

namespace RemoteDeviceManager {
namespace {

constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;
constexpr uint32_t STATE_INTERVAL_MS = 30000;
constexpr uint32_t HTTP_TIMEOUT_MS = 30000;
constexpr size_t HTTP_BUFFER_SIZE = 512;
constexpr size_t MQTT_FILE_CHUNK_BYTES = 384;
constexpr size_t MQTT_FILE_MAX_BYTES = 16384;

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

int hexNibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decodeHexBytes(const String& hex, uint8_t* out, size_t maxLen, size_t& outLen)
{
  if ((hex.length() % 2) != 0 || hex.length() / 2 > maxLen) {
    return false;
  }
  outLen = hex.length() / 2;
  for (size_t i = 0; i < outLen; ++i) {
    const int high = hexNibble(hex[2 * i]);
    const int low = hexNibble(hex[2 * i + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

void appendHexByte(String& out, uint8_t value)
{
  static const char* digits = "0123456789abcdef";
  out += digits[(value >> 4) & 0x0F];
  out += digits[value & 0x0F];
}

String hexEncode(const uint8_t* data, size_t len)
{
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    appendHexByte(out, data[i]);
  }
  return out;
}

bool constantTimeEquals(const String& left, const String& right)
{
  if (left.length() != right.length()) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < left.length(); ++i) {
    diff |= static_cast<uint8_t>(left[i] ^ right[i]);
  }
  return diff == 0;
}

String authBase(const String& payload)
{
  static const char* keys[] = {
      "schema", "device_id", "msg_id", "job_id", "op", "session_id", "command_id",
      "request_id", "mode", "path", "size", "crc32", "sha256", "chunk_size",
      "seq", "offset", "encoding", "data", "url", "compact",
  };

  String base;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    const String value = jsonValue(payload, keys[i]);
    if (value.length() == 0) {
      continue;
    }
    base += keys[i];
    base += '=';
    base += value;
    base += '\n';
  }
  return base;
}

String hmacSha256Hex(const char* secret, const String& payload)
{
  uint8_t digest[32];
  const String base = authBase(payload);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) {
    return "";
  }
  const int rc = mbedtls_md_hmac(md,
                                 reinterpret_cast<const unsigned char*>(secret),
                                 strlen(secret),
                                 reinterpret_cast<const unsigned char*>(base.c_str()),
                                 base.length(),
                                 digest);
  return rc == 0 ? hexEncode(digest, sizeof(digest)) : "";
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
    if (!verifyIncomingAuth(payload)) return;
    handleConsoleCommand(payload);
  } else if (topicName == topic("fs/jobs")) {
    if (!verifyIncomingAuth(payload)) return;
    handleFileJob(payload);
  } else if (topicName == topic("fs/data")) {
    if (!verifyIncomingAuth(payload)) return;
    handleFileData(payload);
  } else if (topicName == topic("ota/jobs")) {
    if (!verifyIncomingAuth(payload)) return;
    handleOtaJob(payload);
  } else if (topicName == topic("rdm/alive/request")) {
    if (!verifyIncomingAuth(payload)) return;
    handleAliveRequest(payload);
  } else if (topicName == topic("rdm/transport/set")) {
    if (!verifyIncomingAuth(payload)) return;
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
  mqtt_.subscribe(topic("fs/data").c_str(), config_.log);
  mqtt_.subscribe(topic("ota/jobs").c_str(), config_.log);
  mqtt_.subscribe(topic("rdm/alive/request").c_str(), config_.log);
  mqtt_.subscribe(topic("rdm/transport/set").c_str(), config_.log);
  subscribed_ = true;
}

void Manager::handleAliveRequest(const String& payload)
{
  publishAlive(payload);
}

bool Manager::verifyIncomingAuth(const String& payload) const
{
  if (!hasText(config_.sharedSecret)) {
    if (config_.log) {
      (*config_.log).println("RemoteDeviceManager rejected command: TCALL_RDM_SHARED_SECRET is empty.");
    }
    return false;
  }
  const String received = jsonValue(payload, "auth");
  if (received.length() != 64) {
    if (config_.log) {
      (*config_.log).println("RemoteDeviceManager rejected command: missing auth.");
    }
    return false;
  }
  const String expected = hmacSha256Hex(config_.sharedSecret, payload);
  const bool ok = expected.length() == 64 && constantTimeEquals(received, expected);
  if (!ok && config_.log) {
    (*config_.log).println("RemoteDeviceManager rejected command: invalid auth.");
  }
  return ok;
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
  } else if (op == "put_mqtt") {
    beginMqttPut(jobId, jsonValue(payload, "path"),
                 static_cast<size_t>(jsonValue(payload, "size").toInt()),
                 parseHex32(jsonValue(payload, "crc32")));
  } else if (op == "get_mqtt") {
    size_t chunkSize = static_cast<size_t>(jsonValue(payload, "chunk_size").toInt());
    if (chunkSize == 0 || chunkSize > MQTT_FILE_CHUNK_BYTES) {
      chunkSize = MQTT_FILE_CHUNK_BYTES;
    }
    sendMqttFile(jobId, jsonValue(payload, "path"), chunkSize);
  } else if (op == "delete") {
    deletePath(jobId, jsonValue(payload, "path"));
  } else {
    publishResult("fs/result", jobId, op.c_str(), "rejected", "unsupported_op");
  }
}

void Manager::handleFileData(const String& payload)
{
  const String op = jsonValue(payload, "op");
  if (op != "put_mqtt") {
    return;
  }
  const String jobId = jsonValue(payload, "job_id");
  const uint32_t seq = static_cast<uint32_t>(jsonValue(payload, "seq").toInt());
  if (!incomingFile_.active || jobId != incomingFile_.jobId) {
    publishFileAck(jobId, seq, "rejected", "no_active_transfer");
    return;
  }
  if (seq != incomingFile_.nextSeq) {
    finishMqttPut(false, "unexpected_sequence");
    publishFileAck(jobId, seq, "rejected", "unexpected_sequence");
    return;
  }
  const String encoded = jsonValue(payload, "data");
  uint8_t buffer[MQTT_FILE_CHUNK_BYTES];
  size_t decodedLen = 0;
  if (!decodeHexBytes(encoded, buffer, sizeof(buffer), decodedLen)) {
    finishMqttPut(false, "decode_failed");
    publishFileAck(jobId, seq, "rejected", "decode_failed");
    return;
  }
  if (decodedLen == 0 || incomingFile_.receivedSize + decodedLen > incomingFile_.expectedSize) {
    finishMqttPut(false, "size_overflow");
    publishFileAck(jobId, seq, "rejected", "size_overflow");
    return;
  }
  if (incomingFile_.file.write(buffer, decodedLen) != decodedLen) {
    finishMqttPut(false, "write_failed");
    publishFileAck(jobId, seq, "failed", "write_failed");
    return;
  }
  incomingFile_.crc = crc32Update(incomingFile_.crc, buffer, decodedLen);
  incomingFile_.receivedSize += decodedLen;
  incomingFile_.nextSeq++;
  publishFileAck(jobId, seq, "ok");
  if (incomingFile_.receivedSize >= incomingFile_.expectedSize) {
    finishMqttPut(true, nullptr);
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
           jsonValue(payload, "sha256"),
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
  payload += ",\"capabilities\":{\"list\":true,\"put_mqtt\":true,\"get_mqtt\":true,\"delete\":true";
  payload += ",\"encoding\":\"hex\",\"max_file_bytes\":";
  payload += static_cast<uint32_t>(MQTT_FILE_MAX_BYTES);
  payload += ",\"chunk_bytes\":";
  payload += static_cast<uint32_t>(MQTT_FILE_CHUNK_BYTES);
  payload += "}}";
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

bool Manager::beginMqttPut(const String& jobId, const String& path, size_t size, uint32_t expectedCrc)
{
  const String normalized = normalizePath(path);
  if (!config_.filesystem || !pathAllowed(normalized)) {
    publishResult("fs/result", jobId, "put_mqtt", "rejected", "path_or_filesystem_unavailable");
    return false;
  }
  if (size == 0 || size > MQTT_FILE_MAX_BYTES || expectedCrc == 0) {
    publishResult("fs/result", jobId, "put_mqtt", "rejected", "invalid_size_or_crc");
    return false;
  }
  if (incomingFile_.active) {
    finishMqttPut(false, "superseded");
  }
  File file = config_.filesystem->open(normalized, FILE_WRITE);
  if (!file) {
    publishResult("fs/result", jobId, "put_mqtt", "failed", "open_failed");
    return false;
  }
  incomingFile_.active = true;
  incomingFile_.jobId = jobId;
  incomingFile_.path = normalized;
  incomingFile_.file = file;
  incomingFile_.expectedSize = size;
  incomingFile_.receivedSize = 0;
  incomingFile_.expectedCrc = expectedCrc;
  incomingFile_.crc = 0;
  incomingFile_.nextSeq = 0;
  publishFileAck(jobId, 0, "ready");
  return true;
}

bool Manager::sendMqttFile(const String& jobId, const String& path, size_t chunkSize)
{
  const String normalized = normalizePath(path);
  if (!config_.filesystem || !pathAllowed(normalized)) {
    publishResult("fs/result", jobId, "get_mqtt", "rejected", "path_or_filesystem_unavailable");
    return false;
  }
  File file = config_.filesystem->open(normalized, FILE_READ);
  if (!file || file.isDirectory()) {
    publishResult("fs/result", jobId, "get_mqtt", "failed", "open_failed");
    return false;
  }
  if (file.size() > MQTT_FILE_MAX_BYTES) {
    file.close();
    publishResult("fs/result", jobId, "get_mqtt", "rejected", "file_too_large");
    return false;
  }
  chunkSize = min(chunkSize, MQTT_FILE_CHUNK_BYTES);
  if (chunkSize == 0) {
    chunkSize = MQTT_FILE_CHUNK_BYTES;
  }
  uint8_t buffer[MQTT_FILE_CHUNK_BYTES];
  uint32_t seq = 0;
  uint32_t crc = 0;
  size_t sent = 0;
  while (file.available()) {
    const size_t got = file.read(buffer, chunkSize);
    if (got == 0) {
      break;
    }
    crc = crc32Update(crc, buffer, got);
    String data;
    data.reserve(got * 2);
    for (size_t i = 0; i < got; ++i) {
      appendHexByte(data, buffer[i]);
    }
    String payload;
    payload.reserve(data.length() + 220);
    payload += "{\"schema\":\"rdm-1\",\"device_id\":\"";
    payload += config_.deviceId;
    payload += "\",\"job_id\":\"";
    payload += escapeJson(jobId);
    payload += "\",\"op\":\"get_mqtt\",\"seq\":";
    payload += seq++;
    payload += ",\"offset\":";
    payload += static_cast<uint32_t>(sent);
    payload += ",\"path\":\"";
    payload += escapeJson(normalized);
    payload += "\"";
    payload += ",\"encoding\":\"hex\",\"data\":\"";
    payload += data;
    payload += "\"}";
    if (!mqtt_.publish(topic("fs/data").c_str(), payload.c_str(), false, config_.log)) {
      file.close();
      publishResult("fs/result", jobId, "get_mqtt", "failed", "publish_failed");
      return false;
    }
    sent += got;
    delay(10);
  }
  file.close();
  String result = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  result += config_.deviceId;
  result += "\",\"job_id\":\"";
  result += escapeJson(jobId);
  result += "\",\"op\":\"get_mqtt\",\"status\":\"ok\",\"path\":\"";
  result += escapeJson(normalized);
  result += "\",\"size\":";
  result += static_cast<uint32_t>(sent);
  result += ",\"chunks\":";
  result += seq;
  result += ",\"crc32\":\"";
  char crcText[9];
  snprintf(crcText, sizeof(crcText), "%08lx", static_cast<unsigned long>(crc));
  result += crcText;
  result += "\",\"uptime_ms\":";
  result += millis();
  result += "}";
  return mqtt_.publish(topic("fs/result").c_str(), result.c_str(), false, config_.log);
}

bool Manager::finishMqttPut(bool ok, const char* detail)
{
  if (!incomingFile_.active) {
    return false;
  }
  const String jobId = incomingFile_.jobId;
  const String path = incomingFile_.path;
  const size_t received = incomingFile_.receivedSize;
  const uint32_t crc = incomingFile_.crc;
  const uint32_t expectedCrc = incomingFile_.expectedCrc;
  incomingFile_.file.close();
  if (!ok || received != incomingFile_.expectedSize || (expectedCrc != 0 && crc != expectedCrc)) {
    if (config_.filesystem) {
      config_.filesystem->remove(path);
    }
    incomingFile_.active = false;
    publishResult("fs/result", jobId, "put_mqtt", "failed", detail ? detail : "crc_or_size_mismatch");
    publishFileState();
    return false;
  }
  incomingFile_.active = false;
  String result = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  result += config_.deviceId;
  result += "\",\"job_id\":\"";
  result += escapeJson(jobId);
  result += "\",\"op\":\"put_mqtt\",\"status\":\"ok\",\"path\":\"";
  result += escapeJson(path);
  result += "\",\"size\":";
  result += static_cast<uint32_t>(received);
  result += ",\"crc32\":\"";
  char crcText[9];
  snprintf(crcText, sizeof(crcText), "%08lx", static_cast<unsigned long>(crc));
  result += crcText;
  result += "\",\"uptime_ms\":";
  result += millis();
  result += "}";
  const bool published = mqtt_.publish(topic("fs/result").c_str(), result.c_str(), false, config_.log);
  publishFileState();
  return published;
}

bool Manager::publishFileAck(const String& jobId, uint32_t seq, const char* status, const char* detail)
{
  String payload = "{\"schema\":\"rdm-1\",\"device_id\":\"";
  payload += config_.deviceId;
  payload += "\",\"job_id\":\"";
  payload += escapeJson(jobId);
  payload += "\",\"op\":\"put_mqtt\",\"seq\":";
  payload += seq;
  payload += ",\"status\":\"";
  payload += status;
  payload += "\"";
  if (detail && strlen(detail) > 0) {
    payload += ",\"detail\":\"";
    payload += escapeJson(detail);
    payload += "\"";
  }
  payload += "}";
  return mqtt_.publish(topic("fs/ack").c_str(), payload.c_str(), false, config_.log);
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

bool Manager::otaUrl(const String& jobId,
                     const String& url,
                     uint32_t expectedCrc,
                     const String& expectedSha256,
                     size_t size)
{
  const bool ok = httpDownloadToOta(url, expectedCrc, expectedSha256, size);
  publishResult("ota/result", jobId, "ota_url", ok ? "ok" : "failed", ok ? nullptr : "ota_failed");
  if (ok) {
    delay(1000);
    ESP.restart();
  }
  return ok;
}

bool Manager::httpDownloadToOta(const String& url,
                                uint32_t expectedCrc,
                                const String& expectedSha256,
                                size_t size)
{
  if (!config_.httpClient || expectedCrc == 0 || expectedSha256.length() != 64) {
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
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
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
      mbedtls_sha256_update(&sha, buffer, got);
      if (Update.write(buffer, got) != got) {
        Update.abort();
        client.stop();
        mbedtls_sha256_free(&sha);
        return false;
      }
      written += got;
      if (contentLength > 0 && written >= static_cast<size_t>(contentLength)) break;
    }
    delay(1);
  }
  client.stop();
  uint8_t shaDigest[32];
  mbedtls_sha256_finish(&sha, shaDigest);
  mbedtls_sha256_free(&sha);
  const String actualSha256 = hexEncode(shaDigest, sizeof(shaDigest));
  if ((contentLength > 0 && written != static_cast<size_t>(contentLength)) ||
      crc != expectedCrc ||
      !constantTimeEquals(actualSha256, expectedSha256)) {
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
