#include "GitHubOtaDemo.h"

#include <Update.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

namespace {

constexpr const char* GITHUB_API_HOST = "api.github.com";
constexpr uint16_t HTTPS_PORT = 443;
constexpr uint32_t READ_TIMEOUT_MS = 30000;
constexpr size_t HTTP_BUFFER_SIZE = 512;
constexpr size_t HTTP_HEADER_LINE_LIMIT = 16384;

#ifndef TCALL_GITHUB_OTA_OWNER
#define TCALL_GITHUB_OTA_OWNER ""
#endif

#ifndef TCALL_GITHUB_OTA_REPO
#define TCALL_GITHUB_OTA_REPO ""
#endif

#ifndef TCALL_GITHUB_OTA_BIN_ASSET
#define TCALL_GITHUB_OTA_BIN_ASSET ""
#endif

#ifndef TCALL_GITHUB_OTA_CRC_ASSET
#define TCALL_GITHUB_OTA_CRC_ASSET ""
#endif

#ifndef TCALL_GITHUB_OTA_USER_AGENT
#define TCALL_GITHUB_OTA_USER_AGENT "ESP32S3-TCU-LTE-OTA-Demo"
#endif

#ifndef TCALL_GITHUB_OTA_REBOOT_AFTER_UPDATE
#define TCALL_GITHUB_OTA_REBOOT_AFTER_UPDATE 1
#endif

struct ParsedUrl {
  String host;
  String path;
  bool https = true;
};

struct HttpHeaders {
  int status = 0;
  int contentLength = -1;
  bool chunked = false;
  String location;
};

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

bool parseUrl(const String& url, ParsedUrl& parsed)
{
  int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) {
    return false;
  }

  String scheme = url.substring(0, schemeEnd);
  scheme.toLowerCase();
  parsed.https = scheme == "https";
  if (!parsed.https) {
    return false;
  }

  int hostStart = schemeEnd + 3;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) {
    parsed.host = url.substring(hostStart);
    parsed.path = "/";
  } else {
    parsed.host = url.substring(hostStart, pathStart);
    parsed.path = url.substring(pathStart);
  }
  return parsed.host.length() > 0;
}

String jsonStringValue(const String& json, const char* key, int from = 0)
{
  String marker = "\"";
  marker += key;
  marker += "\"";
  int keyPos = json.indexOf(marker, from);
  if (keyPos < 0) {
    return "";
  }
  int colon = json.indexOf(':', keyPos + marker.length());
  int quote = json.indexOf('"', colon + 1);
  if (colon < 0 || quote < 0) {
    return "";
  }

  String value;
  bool escaped = false;
  for (int i = quote + 1; i < static_cast<int>(json.length()); ++i) {
    char c = json[i];
    if (escaped) {
      value += c;
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

bool endsWithIgnoreCase(const String& value, const char* suffix)
{
  const size_t valueLen = value.length();
  const size_t suffixLen = strlen(suffix);
  if (valueLen < suffixLen) {
    return false;
  }
  String tail = value.substring(valueLen - suffixLen);
  tail.toLowerCase();
  String expected = suffix;
  expected.toLowerCase();
  return tail == expected;
}

bool assetMatchesFirmware(const String& name)
{
  if (strlen(TCALL_GITHUB_OTA_BIN_ASSET) > 0) {
    return name == TCALL_GITHUB_OTA_BIN_ASSET;
  }
  return endsWithIgnoreCase(name, ".bin");
}

bool assetMatchesCrc(const String& name, const String& firmwareName)
{
  if (strlen(TCALL_GITHUB_OTA_CRC_ASSET) > 0) {
    return name == TCALL_GITHUB_OTA_CRC_ASSET;
  }
  return name == firmwareName + ".crc32" || name == firmwareName + ".crc";
}

bool parseReleaseAssets(const String& json, tcall::GitHubReleaseInfo& release)
{
  release.tagName = jsonStringValue(json, "tag_name");
  int pos = 0;
  while (pos >= 0 && pos < static_cast<int>(json.length())) {
    int namePos = json.indexOf("\"name\"", pos);
    if (namePos < 0) {
      break;
    }
    String name = jsonStringValue(json, "name", namePos);
    String url = jsonStringValue(json, "browser_download_url", namePos);
    if (name.length() > 0 && url.length() > 0 && assetMatchesFirmware(name)) {
      release.firmware.name = name;
      release.firmware.url = url;
      break;
    }
    pos = namePos + 6;
  }

  if (release.firmware.name.length() == 0) {
    return false;
  }

  pos = 0;
  while (pos >= 0 && pos < static_cast<int>(json.length())) {
    int namePos = json.indexOf("\"name\"", pos);
    if (namePos < 0) {
      break;
    }
    String name = jsonStringValue(json, "name", namePos);
    String url = jsonStringValue(json, "browser_download_url", namePos);
    if (name.length() > 0 && url.length() > 0 && assetMatchesCrc(name, release.firmware.name)) {
      release.crc.name = name;
      release.crc.url = url;
      break;
    }
    pos = namePos + 6;
  }

  return release.crc.name.length() > 0;
}

bool parseExpectedCrc(const String& text, uint32_t& crc)
{
  uint32_t value = 0;
  uint8_t digits = 0;
  for (int i = 0; i < static_cast<int>(text.length()); ++i) {
    char c = text[i];
    int nibble = -1;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      nibble = c - 'A' + 10;
    } else if (digits > 0) {
      break;
    }

    if (nibble >= 0) {
      value = (value << 4) | static_cast<uint32_t>(nibble);
      ++digits;
      if (digits == 8) {
        crc = value;
        return true;
      }
    }
  }
  return false;
}

bool readLine(Client& client, String& line, uint32_t timeoutMs)
{
  line = "";
  uint32_t last = millis();
  while (millis() - last < timeoutMs) {
    while (client.available()) {
      char c = static_cast<char>(client.read());
      last = millis();
      if (c == '\n') {
        line.trim();
        return true;
      }
      line += c;
      if (line.length() > HTTP_HEADER_LINE_LIMIT) {
        return false;
      }
    }
    delay(1);
  }
  return false;
}

bool readHeaders(Client& client, HttpHeaders& headers, Stream& out)
{
  String line;
  if (!readLine(client, line, READ_TIMEOUT_MS)) {
    out.println("OTA HTTP failed: no status line.");
    return false;
  }
  int firstSpace = line.indexOf(' ');
  headers.status = firstSpace >= 0 ? line.substring(firstSpace + 1).toInt() : 0;

  while (readLine(client, line, READ_TIMEOUT_MS)) {
    if (line.length() == 0) {
      return true;
    }
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      headers.contentLength = line.substring(15).toInt();
    } else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
      headers.chunked = true;
    } else if (lower.startsWith("location:")) {
      headers.location = line.substring(9);
      headers.location.trim();
    }
  }
  out.println("OTA HTTP failed: incomplete headers.");
  return false;
}

bool connectAndRequest(Client& client, const ParsedUrl& url, HttpHeaders& headers, Stream& out)
{
  out.print("OTA HTTPS GET ");
  out.print(url.host);
  out.println(url.path);

  if (!client.connect(url.host.c_str(), HTTPS_PORT)) {
    out.println("OTA HTTPS connect failed.");
    return false;
  }

  client.print("GET ");
  client.print(url.path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(url.host);
  client.print("\r\nUser-Agent: ");
  client.print(TCALL_GITHUB_OTA_USER_AGENT);
  client.print("\r\nAccept: application/octet-stream, application/vnd.github+json, */*");
  client.print("\r\nConnection: close\r\n\r\n");

  return readHeaders(client, headers, out);
}

bool readRawBodyString(Client& client, String& body, size_t maxBytes)
{
  uint32_t last = millis();
  while ((client.connected() || client.available()) && millis() - last < READ_TIMEOUT_MS) {
    while (client.available()) {
      if (body.length() >= maxBytes) {
        client.stop();
        return false;
      }
      body += static_cast<char>(client.read());
      last = millis();
    }
    delay(1);
  }
  return body.length() > 0;
}

bool readChunkSize(Client& client, int& chunkSize)
{
  String line;
  if (!readLine(client, line, READ_TIMEOUT_MS)) {
    return false;
  }
  int semi = line.indexOf(';');
  if (semi >= 0) {
    line = line.substring(0, semi);
  }
  line.trim();
  chunkSize = static_cast<int>(strtol(line.c_str(), nullptr, 16));
  return true;
}

bool readChunkedString(Client& client, String& body, size_t maxBytes)
{
  while (true) {
    int chunkSize = 0;
    if (!readChunkSize(client, chunkSize)) {
      return false;
    }
    if (chunkSize == 0) {
      String trailer;
      readLine(client, trailer, READ_TIMEOUT_MS);
      return body.length() > 0;
    }
    for (int i = 0; i < chunkSize; ++i) {
      uint32_t last = millis();
      while (!client.available() && millis() - last < READ_TIMEOUT_MS) {
        delay(1);
      }
      if (!client.available() || body.length() >= maxBytes) {
        return false;
      }
      body += static_cast<char>(client.read());
    }
    String crlf;
    readLine(client, crlf, READ_TIMEOUT_MS);
  }
}

}  // namespace

namespace tcall {

GitHubOtaDemo::GitHubOtaDemo(TCallA7670Modem& modem) : modem_(modem) {}

void GitHubOtaDemo::printConfig(Stream& out) const
{
  out.print("GitHub owner: ");
  out.println(TCALL_GITHUB_OTA_OWNER);
  out.print("GitHub repo: ");
  out.println(TCALL_GITHUB_OTA_REPO);
  out.print("Firmware asset: ");
  out.println(strlen(TCALL_GITHUB_OTA_BIN_ASSET) > 0 ? TCALL_GITHUB_OTA_BIN_ASSET : "*.bin");
  out.print("CRC asset: ");
  out.println(strlen(TCALL_GITHUB_OTA_CRC_ASSET) > 0 ? TCALL_GITHUB_OTA_CRC_ASSET :
                                                        "<firmware>.crc32");
}

bool GitHubOtaDemo::printLatest(Stream& out)
{
  GitHubReleaseInfo release;
  if (!latestRelease(release, out)) {
    return false;
  }
  out.print("Latest tag: ");
  out.println(release.tagName);
  out.print("Firmware asset: ");
  out.println(release.firmware.name);
  out.print("CRC asset: ");
  out.println(release.crc.name);
  return true;
}

bool GitHubOtaDemo::updateLatest(Stream& out)
{
  GitHubReleaseInfo release;
  if (!latestRelease(release, out)) {
    return false;
  }

  String crcText;
  if (!fetchString(release.crc.url, crcText, out, 128)) {
    out.println("OTA failed: could not download CRC sidecar.");
    return false;
  }

  uint32_t expectedCrc = 0;
  if (!parseExpectedCrc(crcText, expectedCrc)) {
    out.println("OTA failed: CRC sidecar must contain an 8-digit hex CRC32.");
    return false;
  }

  out.print("Latest tag: ");
  out.println(release.tagName);
  out.print("Firmware asset: ");
  out.println(release.firmware.name);
  out.print("Expected CRC32: 0x");
  out.println(expectedCrc, HEX);

  return downloadFirmware(release.firmware.url, expectedCrc, out);
}

bool GitHubOtaDemo::latestRelease(GitHubReleaseInfo& release, Stream& out)
{
  if (strlen(TCALL_GITHUB_OTA_OWNER) == 0 || strlen(TCALL_GITHUB_OTA_REPO) == 0) {
    out.println("OTA disabled: set TCALL_GITHUB_OTA_OWNER and TCALL_GITHUB_OTA_REPO.");
    return false;
  }

  String apiUrl = "https://";
  apiUrl += GITHUB_API_HOST;
  apiUrl += "/repos/";
  apiUrl += TCALL_GITHUB_OTA_OWNER;
  apiUrl += "/";
  apiUrl += TCALL_GITHUB_OTA_REPO;
  apiUrl += "/releases/latest";

  String json;
  if (!fetchString(apiUrl, json, out, 16000)) {
    out.println("OTA failed: could not fetch latest release metadata.");
    return false;
  }
  if (!parseReleaseAssets(json, release)) {
    out.println("OTA failed: latest release needs a matching .bin asset and CRC sidecar.");
    return false;
  }
  return true;
}

bool GitHubOtaDemo::fetchString(const String& url, String& body, Stream& out, size_t maxBytes)
{
  String current = url;
  for (uint8_t redirect = 0; redirect < 4; ++redirect) {
    ParsedUrl parsed;
    if (!parseUrl(current, parsed)) {
      out.print("OTA URL unsupported: ");
      out.println(current);
      return false;
    }

    Client& client = modem_.cellularSecureClient();
    HttpHeaders headers;
    if (!connectAndRequest(client, parsed, headers, out)) {
      client.stop();
      return false;
    }

    if ((headers.status == 301 || headers.status == 302 || headers.status == 303 ||
         headers.status == 307 || headers.status == 308) &&
        headers.location.length() > 0) {
      client.stop();
      current = headers.location;
      continue;
    }

    if (headers.status != 200) {
      out.print("OTA HTTP status: ");
      out.println(headers.status);
      client.stop();
      return false;
    }

    body = "";
    const bool ok = headers.chunked ? readChunkedString(client, body, maxBytes) :
                                      readRawBodyString(client, body, maxBytes);
    client.stop();
    return ok;
  }

  out.println("OTA failed: too many redirects.");
  return false;
}

bool GitHubOtaDemo::downloadFirmware(const String& url, uint32_t expectedCrc, Stream& out)
{
  String current = url;
  for (uint8_t redirect = 0; redirect < 4; ++redirect) {
    ParsedUrl parsed;
    if (!parseUrl(current, parsed)) {
      out.print("OTA URL unsupported: ");
      out.println(current);
      return false;
    }

    Client& client = modem_.cellularSecureClient();
    HttpHeaders headers;
    if (!connectAndRequest(client, parsed, headers, out)) {
      client.stop();
      return false;
    }

    if ((headers.status == 301 || headers.status == 302 || headers.status == 303 ||
         headers.status == 307 || headers.status == 308) &&
        headers.location.length() > 0) {
      client.stop();
      current = headers.location;
      continue;
    }

    if (headers.status != 200) {
      out.print("OTA HTTP status: ");
      out.println(headers.status);
      client.stop();
      return false;
    }

    const size_t updateSize = headers.contentLength > 0 ? static_cast<size_t>(headers.contentLength) :
                                                          UPDATE_SIZE_UNKNOWN;
    if (!Update.begin(updateSize)) {
      out.print("OTA Update.begin failed: ");
      Update.printError(out);
      client.stop();
      return false;
    }

    uint8_t buffer[HTTP_BUFFER_SIZE];
    uint32_t crc = 0;
    size_t written = 0;

    auto writeBytes = [&](const uint8_t* data, size_t len) -> bool {
      crc = crc32Update(crc, data, len);
      const size_t chunkWritten = Update.write(const_cast<uint8_t*>(data), len);
      written += chunkWritten;
      if (chunkWritten != len) {
        out.print("OTA flash write failed: ");
        Update.printError(out);
        return false;
      }
      if (written % (64UL * 1024UL) < len) {
        out.print("OTA written bytes: ");
        out.println(written);
      }
      return true;
    };

    bool ok = true;
    if (headers.chunked) {
      while (ok) {
        int chunkSize = 0;
        if (!readChunkSize(client, chunkSize)) {
          ok = false;
          break;
        }
        if (chunkSize == 0) {
          String trailer;
          readLine(client, trailer, READ_TIMEOUT_MS);
          break;
        }
        int remaining = chunkSize;
        while (remaining > 0 && ok) {
          const size_t want = min(static_cast<size_t>(remaining), sizeof(buffer));
          size_t got = 0;
          uint32_t last = millis();
          while (got < want && millis() - last < READ_TIMEOUT_MS) {
            while (client.available() && got < want) {
              buffer[got++] = static_cast<uint8_t>(client.read());
              last = millis();
            }
            delay(1);
          }
          ok = got == want && writeBytes(buffer, got);
          remaining -= static_cast<int>(got);
        }
        String crlf;
        readLine(client, crlf, READ_TIMEOUT_MS);
      }
    } else {
      uint32_t last = millis();
      while ((client.connected() || client.available()) && millis() - last < READ_TIMEOUT_MS) {
        size_t got = 0;
        while (client.available() && got < sizeof(buffer)) {
          buffer[got++] = static_cast<uint8_t>(client.read());
          last = millis();
        }
        if (got > 0 && !writeBytes(buffer, got)) {
          ok = false;
          break;
        }
        if (headers.contentLength > 0 && written >= static_cast<size_t>(headers.contentLength)) {
          break;
        }
        delay(1);
      }
    }

    client.stop();
    if (!ok || (headers.contentLength > 0 && written != static_cast<size_t>(headers.contentLength))) {
      out.println("OTA failed: incomplete firmware download.");
      Update.abort();
      return false;
    }

    out.print("Computed CRC32: 0x");
    out.println(crc, HEX);
    if (crc != expectedCrc) {
      out.println("OTA failed: CRC mismatch. New image was not committed.");
      Update.abort();
      return false;
    }

    if (!Update.end(true)) {
      out.print("OTA Update.end failed: ");
      Update.printError(out);
      return false;
    }

    out.print("OTA update committed, bytes=");
    out.println(written);
#if TCALL_GITHUB_OTA_REBOOT_AFTER_UPDATE
    out.println("Rebooting into updated firmware.");
    delay(1000);
    ESP.restart();
#else
    out.println("Reboot disabled by TCALL_GITHUB_OTA_REBOOT_AFTER_UPDATE=0.");
#endif
    return true;
  }

  out.println("OTA failed: too many redirects.");
  return false;
}

}  // namespace tcall
