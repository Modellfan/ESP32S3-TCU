#include "TCallA7670Modem.h"

#include <ctype.h>

#define SerialAT Serial1

namespace {

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
TinyGsmClientSecure secureClient(modem);
String gpsEnableResponse;
bool gpsEnablePending = false;
uint32_t gpsEnableStartMs = 0;

bool cpinReady(const String& response)
{
  return response.indexOf("+CPIN: READY") >= 0;
}

bool cpinLocked(const String& response)
{
  return response.indexOf("+CPIN: SIM PIN") >= 0;
}

String upperCopy(String value)
{
  value.toUpperCase();
  return value;
}

void trimAtPrefix(String& command)
{
  command.trim();
  String upper = upperCopy(command);
  if (upper == "AT") {
    command = "";
  } else if (upper.startsWith("AT+")) {
    command = command.substring(2);
  } else if (upper.startsWith("AT$")) {
    command = command.substring(2);
  } else if (upper.startsWith("AT&")) {
    command = command.substring(2);
  }
  command.trim();
}

}  // namespace

namespace tcall {

const char* simStatusName(SimStatus status)
{
  switch (status) {
    case SIM_READY: return "READY";
    case SIM_LOCKED: return "LOCKED";
    case SIM_ANTITHEFT_LOCKED: return "ANTITHEFT_LOCKED";
    case SIM_ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

const char* regStatusName(RegStatus status)
{
  switch (status) {
    case REG_OK_HOME: return "OK_HOME";
    case REG_OK_ROAMING: return "OK_ROAMING";
    case REG_DENIED: return "DENIED";
    case REG_SEARCHING: return "SEARCHING";
    case REG_UNREGISTERED: return "UNREGISTERED";
    case REG_SMS_ONLY: return "SMS_ONLY";
    case REG_EMERGENCY_ONLY: return "EMERGENCY_ONLY";
    case REG_NO_RESULT: return "NO_RESULT";
    default: return "UNKNOWN";
  }
}

void TCallA7670Modem::begin(Stream& log)
{
  pinMode(BOARD_LED_PIN, OUTPUT);
  digitalWrite(BOARD_LED_PIN, HIGH);

  resetModem();

  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  powerOnModem();
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  log.println("Starting modem...");
  delay(MODEM_START_WAIT_MS);
}

bool TCallA7670Modem::restart(Stream& log, uint32_t onlineTimeoutMs)
{
  log.println("Restarting modem hardware.");
  SerialAT.end();
  gpsEnablePending = false;
  gpsEnableResponse = "";
  resetModem();
  powerOnModem();
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(MODEM_START_WAIT_MS);
  const bool online = waitOnline(onlineTimeoutMs, log);
  if (online) {
    printAt(log, "+CSCLK=0", 5000);
    printAt(log, "+CFUN=1", 15000);
  }
  return online;
}

bool TCallA7670Modem::waitOnline(uint32_t timeoutMs, Stream& log)
{
  const uint32_t start = millis();
  uint8_t retry = 0;

  while (millis() - start < timeoutMs) {
    if (modem.testAT(1000)) {
      log.println("Modem online.");
      return true;
    }

    log.print('.');
    if (++retry > 30) {
      powerOnModem();
      retry = 0;
    }
  }

  log.println();
  log.println("Modem did not answer AT.");
  return false;
}

String TCallA7670Modem::rawAt(const String& command, uint32_t timeoutMs)
{
  String normalized = command;
  trimAtPrefix(normalized);

  while (SerialAT.available()) {
    SerialAT.read();
  }

  SerialAT.print("AT");
  SerialAT.print(normalized);
  SerialAT.print("\r\n");

  const uint32_t start = millis();
  String response;
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      response += static_cast<char>(SerialAT.read());
    }
    if (response.indexOf("\r\nOK\r\n") >= 0 || response.indexOf("\r\nERROR\r\n") >= 0) {
      break;
    }
    delay(1);
  }
  response.trim();
  return response;
}

void TCallA7670Modem::printDiagnostics(Stream& out, const char* phase)
{
  out.println();
  out.print("=== Modem diagnostics: ");
  out.print(phase);
  out.println(" ===");
  out.print("Modem name: ");
  String modemName = modem.getModemName();
  out.println(modemName);
  out.print("Built-in GNSS supported by model table: ");
  out.println(modemHasBuiltInGps(modemName) ? "yes" : "no");
  out.print("Modem info: ");
  out.println(modem.getModemInfo());
  out.print("IMEI: ");
  out.println(modem.getIMEI());
  out.print("IMSI: ");
  out.println(modem.getIMSI());
  out.print("ICCID: ");
  out.println(modem.getSimCCID());
  printSimStatus(out);
  out.print("Operator: ");
  out.println(modem.getOperator());
  out.print("Signal quality: ");
  out.println(modem.getSignalQuality());
  out.print("Network mode: ");
  out.println(modem.getNetworkModeString());
  printRegistration(out);

  String ueInfo;
  if (modem.getSystemInformation(ueInfo)) {
    out.print("CPSI: ");
    out.println(ueInfo);
  }

  printAt(out, "+SIMCOMATI", 10000);
  printAt(out, "+CPIN?");
  printAt(out, "+CEREG?");
  printAt(out, "+CEREG=2");
  printAt(out, "+CEREG?");
  printAt(out, "+CGREG?");
  printAt(out, "+CREG?");
  printAt(out, "+COPS?");
  printAt(out, "+CNMP?");
  printAt(out, "+CGDCONT?");
  printAt(out, "+NETOPEN?");
  printAt(out, "+CGNSSPWR?");
}

void TCallA7670Modem::printSimStatus(Stream& out)
{
  out.print("SIM status: ");
  out.println(simStatusName(modem.getSimStatus()));
}

void TCallA7670Modem::printRegistration(Stream& out)
{
  out.print("Registration: ");
  out.println(regStatusName(modem.getRegistrationStatus()));
  printAt(out, "+CEREG?");
}

void TCallA7670Modem::printRegistrationSnapshot(Stream& out, const char* phase)
{
  out.print("REG SNAPSHOT phase=");
  out.println(phase);
  printAt(out, "+CSQ", 5000);
  printAt(out, "+CEREG=2", 5000);
  printAt(out, "+CEREG?", 5000);
  printAt(out, "+CREG=2", 5000);
  printAt(out, "+CREG?", 5000);
  printAt(out, "+CGREG=2", 5000);
  printAt(out, "+CGREG?", 5000);
  printAt(out, "+COPS?", 10000);
  printAt(out, "+CPSI?", 10000);
}

bool TCallA7670Modem::waitForSimReady(uint32_t timeoutMs, Stream& log)
{
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    const String cpin = rawAt("+CPIN?", 5000);
    if (cpinReady(cpin)) {
      log.println("SIM READY.");
      return true;
    }

    if (cpinLocked(cpin)) {
      log.println("SIM locked by raw CPIN, attempting configured PIN.");
      if (strlen(DEFAULT_SIM_PIN) > 0) {
        String unlock = "+CPIN=\"";
        unlock += DEFAULT_SIM_PIN;
        unlock += "\"";
        log.print("AT");
        log.print(unlock);
        log.print(" -> ");
        const String unlockResponse = rawAt(unlock, 15000);
        log.println(unlockResponse);
        if (cpinReady(unlockResponse)) {
          log.println("SIM READY.");
          return true;
        }
        delay(2000);
        continue;
      }
    }

    SimStatus sim = modem.getSimStatus();
    if (sim == SIM_READY) {
      const String confirm = rawAt("+CPIN?", 5000);
      if (cpinReady(confirm)) {
        log.println("SIM READY.");
        return true;
      }
      log.print("SIM library said READY but CPIN confirm was: ");
      log.println(confirm);
    }

    if (sim == SIM_LOCKED) {
      log.println("SIM locked, attempting configured PIN.");
      if (strlen(DEFAULT_SIM_PIN) > 0 && modem.simUnlock(DEFAULT_SIM_PIN)) {
        log.println("SIM unlock command accepted.");
        delay(1000);
        continue;
      }
      log.println("SIM unlock failed.");
    } else {
      log.print("SIM status: ");
      log.println(simStatusName(sim));
    }
    delay(1000);
  }

  log.println("SIM not ready before timeout.");
  return false;
}

bool TCallA7670Modem::disableSimPin(Stream& log)
{
  log.println("Disabling SIM PIN lock with configured PIN.");
  if (!waitForSimReady(30000, log)) {
    log.println("Cannot disable SIM PIN because SIM is not ready.");
    return false;
  }
  String cmd = "+CLCK=\"SC\",0,\"";
  cmd += DEFAULT_SIM_PIN;
  cmd += "\"";
  printAt(log, cmd.c_str(), 10000);
  printAt(log, "+CLCK=\"SC\",2", 10000);
  return waitForSimReady(30000, log);
}

bool TCallA7670Modem::prepareForCellular(Stream& log)
{
  log.println("Preparing modem for cellular registration.");
  digitalWrite(MODEM_DTR_PIN, LOW);
  printAt(log, "+CSCLK=0", 5000);
  printAt(log, "+CFUN=1", 15000);
  if (!waitForSimReady(45000, log)) {
    return false;
  }
  return true;
}

bool TCallA7670Modem::configureApn(Stream& log)
{
  return configureApn(DEFAULT_APN, log);
}

bool TCallA7670Modem::configureApn(const char* apn, Stream& log)
{
  log.println("Configuring APN context.");
  printAt(log, "+CMEE=2");
  String cgdc = "+CGDCONT=1,\"IP\",\"";
  cgdc += apn;
  cgdc += "\"";
  printAt(log, cgdc.c_str());
  printAt(log, "+CEREG=2");
  printAt(log, "+CREG=2");
  printAt(log, "+CGREG=2");
  return true;
}

bool TCallA7670Modem::selectOperatorAuto(Stream& log)
{
  log.println("Selecting operator automatically.");
  printAt(log, "+COPS=0", 60000);
  return true;
}

bool TCallA7670Modem::selectOperatorTelekom(Stream& log)
{
  log.println("Selecting Telekom Germany operator 26201 on LTE.");
  printAt(log, "+COPS=1,2,\"26201\",7", 60000);
  return true;
}

bool TCallA7670Modem::configureRat(bool lteOnly, Stream& log)
{
  return configureRat(lteOnly, DEFAULT_APN, log);
}

bool TCallA7670Modem::configureRat(bool lteOnly, const char* apn, Stream& log)
{
  log.println("Configuring RAT and PDP context.");
  configureApn(apn, log);
  printAt(log, lteOnly ? "+CNMP=38" : "+CNMP=2", 10000);
  log.println("Waiting for modem after network mode change.");
  waitOnline(60000, log);
  printAt(log, "+CEREG=2");
  return true;
}

bool TCallA7670Modem::waitForEpsRegistration(uint32_t timeoutMs, Stream& log)
{
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    const int stat = ceregStat();
    if (stat == 1 || stat == 5) {
      log.println("EPS/LTE registration OK.");
      return true;
    }
    if (stat == 3) {
      log.println("EPS/LTE registration denied.");
      return false;
    }

    log.print("Waiting for EPS registration, CEREG stat=");
    log.print(stat);
    log.print(" CSQ=");
    log.print(modem.getSignalQuality());
    log.print(" raw=");
    log.println(rawAt("+CEREG?", 2000));
    delay(2000);
  }

  log.println("EPS/LTE registration timeout.");
  return false;
}

bool TCallA7670Modem::activateData(const char* apn,
                                   const char* user,
                                   const char* pass,
                                   Stream* log,
                                   uint8_t attempts)
{
  if (log) {
    (*log).print("Activating data with APN: ");
    (*log).println(apn);
    (*log).print("APN auth user set: ");
    (*log).println(strlen(user) > 0 ? "yes" : "no");
  }

  printAt(log ? *log : Serial, "+CGATT=1", 10000);
  printAt(log ? *log : Serial, "+CGATT?");
  if (strlen(user) > 0 || strlen(pass) > 0) {
    String auth = "+CGAUTH=1,0,\"";
    auth += user;
    auth += "\",\"";
    auth += pass;
    auth += "\"";
    printAt(log ? *log : Serial, auth.c_str());
  } else {
    printAt(log ? *log : Serial, "+CGAUTH=1,0");
  }

  for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
    if (modem.gprsConnect(apn, user, pass)) {
      if (log) {
        (*log).print("PDP connected. Local IP: ");
        (*log).println(modem.getLocalIP());
      }
      return true;
    }
    if (log) {
      (*log).println("PDP activation failed, retrying.");
      printAt(*log, "+CGACT?");
    }
    delay(3000);
  }

  return false;
}

bool TCallA7670Modem::deactivateData()
{
  if (modem.isGprsConnected()) {
    return modem.gprsDisconnect();
  }
  return modem.setNetworkDeactivate();
}

bool TCallA7670Modem::dataActive()
{
  return modem.isGprsConnected() || modem.getNetworkActive();
}

String TCallA7670Modem::localIP()
{
  return modem.getLocalIP();
}

Client& TCallA7670Modem::cellularClient()
{
  return client;
}

Client& TCallA7670Modem::cellularSecureClient()
{
  return secureClient;
}

bool TCallA7670Modem::httpGet(const char* host, const char* path, uint16_t port, Stream& out)
{
  out.print("HTTP GET ");
  out.print(host);
  out.print(':');
  out.print(port);
  out.println(path);

  if (!client.connect(host, port)) {
    out.println("HTTP connect failed.");
    return false;
  }

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(host);
  client.print("\r\nConnection: close\r\n\r\n");

  uint32_t lastRead = millis();
  size_t total = 0;
  while (client.connected() && millis() - lastRead < 10000) {
    while (client.available()) {
      char c = static_cast<char>(client.read());
      if (total < 1024) {
        out.write(c);
      }
      ++total;
      lastRead = millis();
    }
    delay(1);
  }

  while (client.available()) {
    char c = static_cast<char>(client.read());
    if (total < 1024) {
      out.write(c);
    }
    ++total;
  }
  client.stop();

  out.println();
  out.print("HTTP bytes read: ");
  out.println(total);
  return total > 0;
}

String TCallA7670Modem::listSms(const char* status)
{
  rawAt("+CMGF=1", 5000);
  rawAt("+CSCS=\"GSM\"", 5000);

  String cmd = "+CMGL=\"";
  cmd += status;
  cmd += "\"";
  return rawAt(cmd, 30000);
}

bool TCallA7670Modem::sendSms(const char* number, const String& message, Stream& out)
{
  out.println("Preparing SMS text mode.");
  out.print("AT+CMGF=1 -> ");
  out.println(rawAt("+CMGF=1", 5000));
  out.print("AT+CSCS=\"GSM\" -> ");
  out.println(rawAt("+CSCS=\"GSM\"", 5000));

  while (SerialAT.available()) {
    SerialAT.read();
  }

  SerialAT.print("AT+CMGS=\"");
  SerialAT.print(number);
  SerialAT.print("\"\r\n");

  String response;
  const uint32_t promptStart = millis();
  bool gotPrompt = false;
  while (millis() - promptStart < 10000UL) {
    while (SerialAT.available()) {
      char c = static_cast<char>(SerialAT.read());
      response += c;
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    if (gotPrompt || response.indexOf("ERROR") >= 0) {
      break;
    }
    delay(1);
  }

  out.print("AT+CMGS prompt -> ");
  out.println(response);
  if (!gotPrompt) {
    out.println("SMS prompt not received.");
    return false;
  }

  SerialAT.print(message);
  SerialAT.write(0x1A);

  const uint32_t sendStart = millis();
  response = "";
  while (millis() - sendStart < 60000UL) {
    while (SerialAT.available()) {
      response += static_cast<char>(SerialAT.read());
    }
    if (response.indexOf("\r\nOK\r\n") >= 0 || response.indexOf("\r\nERROR\r\n") >= 0 ||
        response.indexOf("+CMS ERROR") >= 0 || response.indexOf("+CME ERROR") >= 0) {
      break;
    }
    delay(1);
  }

  response.trim();
  out.print("SMS send response -> ");
  out.println(response);
  return response.indexOf("\r\nOK\r\n") >= 0 || response.endsWith("OK");
}

bool TCallA7670Modem::enableGps(Stream& log)
{
  String modemName = modem.getModemName();
  if (!modemHasBuiltInGps(modemName)) {
    log.print("This modem variant does not have built-in GNSS: ");
    log.println(modemName);
    return false;
  }

  if (modem.isEnableGPS()) {
    modem.setGPSBaud(MODEM_BAUD);
    modem.setGPSMode(4);
    log.println("GNSS already enabled.");
    return true;
  }

  for (uint8_t attempt = 0; attempt < 30; ++attempt) {
    if (modem.enableGPS(MODEM_GPS_ENABLE_GPIO, MODEM_GPS_ENABLE_LEVEL)) {
      modem.setGPSBaud(MODEM_BAUD);
      modem.setGPSMode(4);
      log.println("GNSS enabled.");
      return true;
    }
    log.print('.');
    delay(1000);
  }

  log.println();
  log.println("GNSS enable failed.");
  return false;
}

bool TCallA7670Modem::startGpsEnableRequest()
{
  if (modem.isEnableGPS()) {
    modem.setGPSBaud(MODEM_BAUD);
    modem.setGPSMode(4);
    return true;
  }

  while (SerialAT.available()) {
    SerialAT.read();
  }

  gpsEnableResponse = "";
  gpsEnablePending = true;
  gpsEnableStartMs = millis();
  SerialAT.print("AT+CGNSSPWR=1\r\n");
  return true;
}

bool TCallA7670Modem::pollGpsEnableRequest(bool& done)
{
  done = false;
  if (!gpsEnablePending) {
    done = true;
    return modem.isEnableGPS();
  }

  while (SerialAT.available()) {
    gpsEnableResponse += static_cast<char>(SerialAT.read());
  }

  if (gpsEnableResponse.indexOf("+CGNSSPWR: READY!") >= 0 ||
      gpsEnableResponse.indexOf("\r\nOK\r\n") >= 0) {
    gpsEnablePending = false;
    done = true;
    modem.setGPSBaud(MODEM_BAUD);
    modem.setGPSMode(4);
    return true;
  }

  if (gpsEnableResponse.indexOf("\r\nERROR\r\n") >= 0 ||
      millis() - gpsEnableStartMs > 30000UL) {
    gpsEnablePending = false;
    done = true;
    return false;
  }

  return false;
}

bool TCallA7670Modem::disableGps()
{
  return modem.disableGPS(MODEM_GPS_ENABLE_GPIO, !MODEM_GPS_ENABLE_LEVEL);
}

bool TCallA7670Modem::gpsEnabled()
{
  return modem.isEnableGPS();
}

bool TCallA7670Modem::gpsHotStart()
{
  return modem.gpsHotStart();
}

bool TCallA7670Modem::gpsColdStart()
{
  return modem.gpsColdStart();
}

String TCallA7670Modem::gpsRaw()
{
  return modem.getGPSraw();
}

bool TCallA7670Modem::gpsFix(GnssFix& fix)
{
  return modem.getGPS(&fix.fixMode, &fix.latitude, &fix.longitude, &fix.speed,
                      &fix.altitude, &fix.visibleSatellites, &fix.usedSatellites,
                      &fix.accuracy, &fix.year, &fix.month, &fix.day, &fix.hour,
                      &fix.minute, &fix.second);
}

bool TCallA7670Modem::gpsExtended(GPSInfo& info)
{
  return modem.getGPS_Ex(info);
}

bool TCallA7670Modem::modemHasBuiltInGps(const String& modemName) const
{
  if (modemName.startsWith("A7670E-FASE") || modemName.startsWith("A7670SA-FASE")) {
    return true;
  }
  if (modemName.startsWith("A7670E-LNXY-UBL") ||
      modemName.startsWith("A7670E-LNMV") ||
      modemName.startsWith("A7670SA-LASE") ||
      modemName.startsWith("A7670SA-LASC") ||
      modemName.startsWith("A7670G-LLSE") ||
      modemName.startsWith("A7670G-LABE") ||
      modemName.startsWith("A7670E-LASE ")) {
    return false;
  }
  return true;
}

void TCallA7670Modem::resetModem()
{
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
  delay(100);
  digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
  delay(2600);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
}

void TCallA7670Modem::powerOnModem()
{
  pinMode(BOARD_PWRKEY_PIN, OUTPUT);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, HIGH);
  delay(MODEM_POWERON_PULSE_MS);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
}

int TCallA7670Modem::ceregStat()
{
  String response = rawAt("+CEREG?", 2000);
  int idx = response.indexOf("+CEREG:");
  if (idx < 0) {
    return -1;
  }

  int comma = response.indexOf(',', idx);
  if (comma < 0) {
    return -1;
  }

  int start = comma + 1;
  while (start < static_cast<int>(response.length()) &&
         isspace(static_cast<unsigned char>(response[start]))) {
    ++start;
  }

  int end = response.indexOf(',', start);
  if (end < 0) {
    end = response.indexOf('\r', start);
  }
  if (end < 0) {
    end = response.indexOf('\n', start);
  }
  if (end < 0) {
    end = response.length();
  }

  String stat = response.substring(start, end);
  stat.trim();
  return stat.toInt();
}

void TCallA7670Modem::printAt(Stream& out, const char* cmd, uint32_t timeoutMs)
{
  out.print("AT");
  out.print(cmd);
  out.print(" -> ");
  out.println(rawAt(cmd, timeoutMs));
}

}  // namespace tcall
