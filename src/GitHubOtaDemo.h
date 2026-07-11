#pragma once

#include <Arduino.h>

#include "TCallA7670Modem.h"

namespace tcall {

struct GitHubReleaseAsset {
  String name;
  String url;
};

struct GitHubReleaseInfo {
  String tagName;
  GitHubReleaseAsset firmware;
  GitHubReleaseAsset crc;
};

class GitHubOtaDemo {
 public:
  explicit GitHubOtaDemo(TCallA7670Modem& modem);

  void printConfig(Stream& out) const;
  bool printLatest(Stream& out);
  bool updateLatest(Stream& out);

 private:
  TCallA7670Modem& modem_;

  bool latestRelease(GitHubReleaseInfo& release, Stream& out);
  bool fetchString(const String& url, String& body, Stream& out, size_t maxBytes);
  bool downloadFirmware(const String& url, uint32_t expectedCrc, Stream& out);
};

}  // namespace tcall
