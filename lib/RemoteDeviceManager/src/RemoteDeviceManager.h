#pragma once

#include <Arduino.h>
#include <Client.h>
#include <FS.h>
#include <Update.h>
#include <WiFi.h>

namespace RemoteDeviceManager {

struct MqttMessage {
  String topic;
  String payload;
  bool retain = false;
};

class MqttTransport {
 public:
  virtual ~MqttTransport() = default;
  virtual bool connect(const char* host,
                       uint16_t port,
                       const char* clientId,
                       const char* user,
                       const char* pass,
                       Stream* log) = 0;
  virtual bool connected() const = 0;
  virtual bool publish(const char* topic, const char* payload, bool retain, Stream* log) = 0;
  virtual bool subscribe(const char* topic, Stream* log) = 0;
  virtual bool poll(MqttMessage& message) = 0;
  virtual void disconnect(Stream* log) = 0;
  virtual const char* name() const = 0;
};

class WiFiMqttTransport : public MqttTransport {
 public:
  explicit WiFiMqttTransport(Client& client);

  bool connect(const char* host,
               uint16_t port,
               const char* clientId,
               const char* user,
               const char* pass,
               Stream* log) override;
  bool connected() const override;
  bool publish(const char* topic, const char* payload, bool retain, Stream* log) override;
  bool subscribe(const char* topic, Stream* log) override;
  bool poll(MqttMessage& message) override;
  void disconnect(Stream* log) override;
  const char* name() const override { return "wifi-mqtt"; }

 private:
  bool readPacket(uint8_t& type, uint8_t& flags, String& topic, String& payload);
  bool writeRemainingLength(size_t length);
  void writeString(const char* value);
  bool readByte(uint8_t& value, uint32_t timeoutMs);

  Client& client_;
  uint16_t packetId_ = 1;
};

using ConsoleCommandCallback = void (*)(String line, Stream& out);
using JsonCallback = String (*)();
using OtaCommandCallback = bool (*)(const char* command, Stream& out);
using TransportSetCallback = bool (*)(const char* mode, Stream& out);
using ConnectivityJsonCallback = String (*)(const String& requestPayload);

struct Config {
  const char* deviceId = "eboxster";
  const char* topicPrefix = "eboxster";
  const char* mqttHost = "";
  uint16_t mqttPort = 1883;
  const char* mqttUser = "";
  const char* mqttPass = "";
  const char* mqttClientId = "eboxster";
  const char* sharedSecret = "";
  const char* firmwareVersion = "dev";
  fs::FS* filesystem = nullptr;
  Client* httpClient = nullptr;
  Stream* log = nullptr;
  ConsoleCommandCallback consoleCommand = nullptr;
  JsonCallback statusJson = nullptr;
  JsonCallback gpsJson = nullptr;
  OtaCommandCallback otaCommand = nullptr;
  TransportSetCallback transportSet = nullptr;
  ConnectivityJsonCallback connectivityJson = nullptr;
};

class Manager {
 public:
  explicit Manager(MqttTransport& mqtt);

  void begin(const Config& config);
  void loop();
  bool publishState();
  bool publishSleepState(const char* sleepState, uint32_t sleepMs = 0, const char* reason = "");
  bool publishTelemetry();
  void handleMqttMessage(const String& topic, const String& payload);
  bool connected() const;
  const char* transportName() const;

 private:
  class CaptureStream : public Stream {
   public:
    size_t write(uint8_t value) override;
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    const String& text() const { return text_; }

   private:
    String text_;
  };

  bool ensureConnected();
  void subscribeTopics();
  void publishConsoleState();
  void handleConsoleCommand(const String& payload);
  void handleFileJob(const String& payload);
  void handleFileData(const String& payload);
  void handleOtaJob(const String& payload);
  void handleAliveRequest(const String& payload);
  void handleTransportSet(const String& payload);
  bool verifyIncomingAuth(const String& payload) const;
  bool publishAlive(const String& requestPayload = "");
  bool publishTransportState(const char* requestedMode, bool ok, const String& detail = "");
  void publishResult(const char* topicSuffix, const String& jobId, const char* op, const char* status,
                     const char* detail = nullptr);
  void publishConsoleOutput(const String& sessionId,
                            const String& commandId,
                            const String& text,
                            bool final,
                            int exitCode,
                            uint32_t durationMs);
  void publishFileState();
  void publishOtaState();
  bool listFiles(const String& jobId);
  bool beginMqttPut(const String& jobId, const String& path, size_t size, uint32_t expectedCrc);
  bool sendMqttFile(const String& jobId, const String& path, size_t chunkSize);
  bool finishMqttPut(bool ok, const char* detail);
  bool publishFileAck(const String& jobId, uint32_t seq, const char* status, const char* detail = nullptr);
  bool deletePath(const String& jobId, const String& path);
  bool otaUrl(const String& jobId, const String& url, uint32_t expectedCrc, const String& expectedSha256, size_t size);
  bool httpDownloadToOta(const String& url, uint32_t expectedCrc, const String& expectedSha256, size_t size);
  bool parseUrl(const String& url, String& host, uint16_t& port, String& path);
  String normalizePath(const String& path) const;
  bool pathAllowed(const String& path) const;
  String topic(const char* suffix) const;

  MqttTransport& mqtt_;
  Config config_;
  bool subscribed_ = false;
  uint32_t nextReconnectMs_ = 0;
  uint32_t nextTelemetryMs_ = 0;
  uint32_t nextStateMs_ = 0;
  uint32_t msgCounter_ = 0;

  struct IncomingFileTransfer {
    bool active = false;
    String jobId;
    String path;
    File file;
    size_t expectedSize = 0;
    size_t receivedSize = 0;
    uint32_t expectedCrc = 0;
    uint32_t crc = 0;
    uint32_t nextSeq = 0;
  } incomingFile_;
};

String jsonValue(const String& json, const char* key);
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

}  // namespace RemoteDeviceManager
