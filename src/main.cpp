#include <Arduino.h>

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <ctype.h>
#include <math.h>

#include "GpsBufferedService.h"
#include "TCallA7670Modem.h"

namespace {

constexpr const char* WIFI_SSID = TCALL_WIFI_SSID;
constexpr const char* WIFI_PASSWORD = TCALL_WIFI_PASSWORD;
constexpr const char* OTA_HOSTNAME = TCALL_OTA_HOSTNAME;
constexpr uint16_t WIFI_CONSOLE_PORT = 23;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;

tcall::TCallA7670Modem modemService;
tcall::GpsBufferedService gpsBuffer(modemService);
WiFiServer wifiConsoleServer(WIFI_CONSOLE_PORT);
WiFiClient wifiConsoleClient;
String serialCommandLine;
String wifiCommandLine;

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
  out.println("  gsm prove [timeout_seconds] [host] [path]");
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

  out.println("Usage: gsm prove [timeout_seconds] [host] [path] | gsm reset");
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
  printHelp(Serial);
  Serial.print("> ");
}

void loop()
{
  ArduinoOTA.handle();
  gpsBuffer.runner();
  pollStreamConsole(Serial, serialCommandLine);
  pollWifiConsole();
  delay(1);
}
