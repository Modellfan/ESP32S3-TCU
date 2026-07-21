# RemoteDeviceManager Hardware Testplan

Dieser Testplan beschreibt die Hardware- und Integrationstests, die noetig sind,
um RemoteDeviceManager fuer den praktischen Einsatz zu haerten. Die
Uebersichtstabelle verlinkt auf die Ergebnisabschnitte weiter unten in dieser
Datei. Jeder Ergebnisabschnitt soll nach einem Lauf mit Datum, Firmware-Version,
Setup, Ergebnis und Log-Auszug aktualisiert werden.

## Testumgebung

Mindestaufbau:

- ESP32-S3 TCU / T-Call A7670 Zielhardware mit Antennen fuer LTE und GNSS.
- SIM-Karte mit Datenvolumen und erreichbarem APN.
- Stabile 5 V Versorgung mit Strommessung oder Labornetzteil.
- PC im gleichen LAN mit PlatformIO, Python und RemoteDeviceManager WebUI.
- Lokaler NanoMQ Broker.
- Erreichbarer Public Endpoint, z. B. DuckDNS, fuer HTTP und MQTT Tests ueber LTE.
- GitHub Repository mit Release und `.bin` Asset fuer OTA Tests.
- Testdateien fuer SPIFFS: kleine Textdatei, JSON, Binary, groessere Datei nahe
  am realistischen OTA-/Filesystem-Limit.

## Automatisierungsbewertung

- **Automatisch**: Kann durch Skript, WebUI API, MQTT und serielle Auswertung ohne
  manuelle Eingriffe laufen.
- **Teilautomatisch**: Skript kann den Test ausloesen und messen, aber Hardware,
  SIM, Netzabdeckung, Power-Cycle oder visuelle Kontrolle bleiben manuell.
- **Manuell**: Benoetigt bewusste Hardwaremanipulation, Beobachtung oder
  Sicherheitsentscheidung.

## Uebersicht

| ID | Test | Ziel | Transport | Automatisierung | Ergebnis |
|---|---|---|---|---|---|
| HW-01 | Boot und Baseline-Telemetrie | Device bootet sauber und publiziert Status | USB, WiFi | Teilautomatisch | [Ergebnis](#hw-01-boot-und-baseline-telemetrie) |
| HW-02 | WiFi MQTT Control Plane | MQTT Connect, Subscribe, Publish ueber WiFi | WiFi | Automatisch | [Ergebnis](#hw-02-wifi-mqtt-control-plane) |
| HW-03 | LTE MQTT Control Plane | Native SIMCom MQTT erreicht den Broker | LTE | Teilautomatisch | [Ergebnis](#hw-03-lte-mqtt-control-plane) |
| HW-04 | WiFi HTTP Data Plane | Device erreicht HTTP-Dateiserver ueber LAN | WiFi | Automatisch | [Ergebnis](#hw-04-wifi-http-data-plane) |
| HW-05 | LTE HTTP Data Plane | Device erreicht HTTP-Dateiserver ueber Public Endpoint | LTE | Teilautomatisch | [Ergebnis](#hw-05-lte-http-data-plane) |
| HW-06 | Alive Polling beider Links | WiFi und LTE Checks laufen unabhaengig vom aktiven Link | WiFi, LTE | Automatisch | [Ergebnis](#hw-06-alive-polling-beider-links) |
| HW-07 | Transport Umschalten | Wechsel WiFi <-> LTE ohne Kontrollverlust | WiFi, LTE | Teilautomatisch | [Ergebnis](#hw-07-transport-umschalten) |
| HW-08 | MQTT Console | Remote Console fuehrt Firmware-Kommandos aus | WiFi, LTE | Automatisch | [Ergebnis](#hw-08-mqtt-console) |
| HW-09 | SPIFFS List | WebUI zeigt Datei-/Ordnerliste | WiFi, LTE | Automatisch | [Ergebnis](#hw-09-spiffs-list) |
| HW-10 | SPIFFS Upload | Datei wird per HTTP GET vom Tool auf SPIFFS geschrieben | WiFi, LTE | Automatisch | [Ergebnis](#hw-10-spiffs-upload) |
| HW-11 | SPIFFS Download | Datei wird per HTTP PUT zum Tool uebertragen | WiFi, LTE | Automatisch | [Ergebnis](#hw-11-spiffs-download) |
| HW-12 | SPIFFS Delete | Datei wird geloescht, geschuetzte Pfade bleiben blockiert | WiFi, LTE | Automatisch | [Ergebnis](#hw-12-spiffs-delete) |
| HW-13 | Filesystem Fehlerfaelle | Voller Speicher, fehlende Datei, ungueltiger Pfad | WiFi, LTE | Automatisch | [Ergebnis](#hw-13-filesystem-fehlerfaelle) |
| HW-14 | Lokale OTA mit CRC OK | Hochgeladene `.bin` wird validiert und geflasht | WiFi, LTE | Teilautomatisch | [Ergebnis](#hw-14-lokale-ota-mit-crc-ok) |
| HW-15 | Lokale OTA mit CRC Fehler | Falsche CRC verhindert Flash | WiFi, LTE | Automatisch | [Ergebnis](#hw-15-lokale-ota-mit-crc-fehler) |
| HW-16 | GitHub Latest OTA | Device findet Release Asset und aktualisiert Firmware | LTE, WiFi | Teilautomatisch | [Ergebnis](#hw-16-github-latest-ota) |
| HW-17 | OTA Recovery | Abbruch/Reset waehrend OTA fuehrt zu bootfaehigem Zustand | WiFi, LTE | Manuell | [Ergebnis](#hw-17-ota-recovery) |
| HW-18 | Broker Ausfall | Device erkennt MQTT-Ausfall und verbindet neu | WiFi, LTE | Teilautomatisch | [Ergebnis](#hw-18-broker-ausfall) |
| HW-19 | HTTP Server Ausfall | File-/OTA-Jobs melden Fehler ohne Haenger | WiFi, LTE | Automatisch | [Ergebnis](#hw-19-http-server-ausfall) |
| HW-20 | LTE Netzverlust | APN/Signalverlust wird erkannt und erholt sich | LTE | Manuell | [Ergebnis](#hw-20-lte-netzverlust) |
| HW-21 | Power Cycle und Retained State | Nach Neustart erscheinen Status und Version wieder | WiFi, LTE | Teilautomatisch | [Ergebnis](#hw-21-power-cycle-und-retained-state) |
| HW-22 | GNSS/Map Telemetrie | GPS Position und Geschwindigkeit werden angezeigt | LTE, GNSS | Teilautomatisch | [Ergebnis](#hw-22-gnssmap-telemetrie) |
| HW-23 | Langzeittest | 12-24 h Betrieb ohne Speicherleck oder Verbindungsverlust | WiFi, LTE | Teilautomatisch | [Ergebnis](#hw-23-langzeittest) |
| HW-24 | Versorgung/Brownout | Unterspannung fuehrt nicht zu korruptem FS/OTA-Zustand | Power | Manuell | [Ergebnis](#hw-24-versorgungbrownout) |

## Automatisierbare Testausfuehrung

Diese Tests koennen gut automatisiert werden:

- Build-Check: `pio run -e tcall_a7670_v1_0`.
- Start NanoMQ und RemoteDeviceManager Tool.
- Polling von `/api/state`.
- MQTT Publish/Subscribe auf `eboxster/...`.
- Console-Kommandos ueber `POST /api/console`.
- File-Jobs ueber `POST /api/files/list`, `/upload`, `/download`, `/delete`.
- OTA-Negativtest mit falscher CRC.
- Browser-/DOM-Pruefung der WebUI fuer Device, Console, Files, OTA und Map.

Diese Tests bleiben teilautomatisch, weil externe Hardware oder Netze beteiligt
sind:

- LTE MQTT/HTTP ueber DuckDNS oder andere Public Endpoints.
- Transportwechsel, wenn LTE nicht dauerhaft erreichbar ist.
- GitHub OTA, weil ein echter Release-Stand und Reboot verifiziert werden muss.
- GNSS, weil Empfang und Bewegung real oder simuliert bereitgestellt werden muss.
- Power-Cycle, Brownout und OTA-Abbruch.

## Ergebnisvorlage

Jeder Ergebnisabschnitt sollte dieses Format nutzen:

```text
Status: not run | pass | fail | blocked
Datum:
Branch:
Commit:
Firmware-Version:
Device-ID:
Transport:
Setup:
Schritte:
Erwartung:
Beobachtung:
Logs/Links:
Naechste Aktion:
```

## Testergebnisse

### HW-01 Boot und Baseline-Telemetrie

Status: pass
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: wifi-mqtt
Setup: Laufendes Device ueber RemoteDeviceManager WebUI/API, kein frischer
Power-Cycle in diesem Lauf.
Schritte: `/api/state` gelesen und retained Topics geprueft.
Erwartung: `status`, `rdm/state`, `fs/state`, `ota/state`, `console/state` und
`gps` sind vorhanden.
Beobachtung: Status vorhanden; IP `192.168.0.86`, RSSI `-71 dBm`,
`mqtt_connected=true`, Firmware `dev`, GPS Power seen `true`.
Logs/Links: `/api/state`, `eboxster/status`, `eboxster/rdm/state`.
Naechste Aktion: Test bei Bedarf nach definiertem Power-Cycle wiederholen.

### HW-02 WiFi MQTT Control Plane

Status: pass
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi
Setup: NanoMQ lokal auf `1883`, RemoteDeviceManager Tool auf `8093`.
Schritte: `/api/device/alive` mit Request `hw-alive-1784424361` publiziert und
`eboxster/rdm/alive` ausgewertet.
Erwartung: WiFi ist verbunden, MQTT ist erreichbar.
Beobachtung: WiFi `connected=true`, IP `192.168.0.86`, RSSI `-71 dBm`,
`mqtt=true`.
Logs/Links: `eboxster/rdm/alive`.
Naechste Aktion: In Langzeittest auf Reconnect-Stabilitaet pruefen.

### HW-03 LTE MQTT Control Plane

Status: fail
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: LTE
Setup: Device meldet LTE IP `10.165.104.4`; Public MQTT Endpoint
`eboxster.duckdns.org:8093` ist vom Backend aus erreichbar.
Schritte: Alive Response ausgewertet.
Erwartung: LTE `mqtt=true`.
Beobachtung: LTE `connected=true`, IP `10.165.104.4`, aber `mqtt=false`.
Logs/Links: Alive Payload `lte: { connected: true, ip: "10.165.104.4",
mqtt: false, http: false }`.
Naechste Aktion: SIMCom native MQTT Pfad und Provider/Public-Port Routing auf
dem Device debuggen.

### HW-04 WiFi HTTP Data Plane

Status: pass
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi
Setup: RemoteDeviceManager HTTP `192.168.0.37:8093`.
Schritte: Alive HTTP Check und Backend HTTP Check ausgewertet.
Erwartung: WiFi HTTP Check ist true.
Beobachtung: Alive meldet WiFi `http=true`; Backend HTTP LAN Check meldet
`200`.
Logs/Links: `/api/state.server_checks.http_lan`, `/api/state.retained.rdm/alive`.
Naechste Aktion: File-Jobs separat reparieren, da HW-10/HW-11 trotz HTTP Check
fehlgeschlagen sind.

### HW-05 LTE HTTP Data Plane

Status: fail
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: LTE
Setup: Public HTTP `eboxster.duckdns.org:8093` ist vom Backend aus mit `200`
erreichbar.
Schritte: Alive Response ausgewertet.
Erwartung: LTE HTTP Check ist true.
Beobachtung: LTE `connected=true`, aber `http=false`.
Logs/Links: Alive Payload und `/api/state.server_checks.http_public`.
Naechste Aktion: Device-seitige LTE HTTP Probe gegen Public Endpoint debuggen.

### HW-06 Alive Polling beider Links

Status: pass
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: active WiFi, LTE separately checked
Setup: RemoteDeviceManager WebUI pollt Alive alle 2 Sekunden.
Schritte: Manuelle Alive Request `hw-alive-1784424361` gesendet.
Erwartung: Antwort enthaelt getrennte `wifi` und `lte` Objekte.
Beobachtung: Antwort enthaelt `wifi.connected=true`, `wifi.mqtt=true`,
`wifi.http=true`, `lte.connected=true`, `lte.mqtt=false`, `lte.http=false`.
Logs/Links: `eboxster/rdm/alive`.
Naechste Aktion: LTE Checks reparieren, aber Alive-Struktur ist korrekt.

### HW-07 Transport Umschalten

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi -> LTE geplant
Setup: WebUI/API blockiert LTE Switch, wenn LTE MQTT nicht erreichbar ist.
Schritte: Vortest HW-03 ausgewertet.
Erwartung: LTE MQTT muss true sein, bevor sicher umgeschaltet wird.
Beobachtung: LTE MQTT ist false; Umschalten wurde nicht erzwungen, um die
Control Plane nicht zu verlieren.
Logs/Links: Alive Payload `lte.mqtt=false`.
Naechste Aktion: Nach HW-03 Fix erneut testen; nur mit `force` testen, wenn
Fallback ueber USB bereitsteht.

### HW-08 MQTT Console

Status: pass
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi MQTT
Setup: Commands ueber `POST /api/console`.
Schritte: `help`, `mqtt status`, `wifi status`, `gps status` ausgefuehrt und
`console/out final=true` abgewartet.
Erwartung: Alle Kommandos liefern Exit-Code `0`.
Beobachtung: Alle vier Kommandos Exit-Code `0`; `mqtt status` meldet native
cellular MQTT enabled, `wifi status` meldet IP `192.168.0.86`.
Logs/Links: `eboxster/console/out` fuer die vier getesteten Command-IDs.
Naechste Aktion: Weitere Kommandos in automatisierten Regressionstest aufnehmen.

### HW-09 SPIFFS List

Status: pass
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi MQTT Control
Setup: File list ueber `POST /api/files/list`.
Schritte: `fs/jobs` op `list` gesendet und `fs/result` abgewartet.
Erwartung: `status=ok` und Dateiliste.
Beobachtung: `status=ok`; Dateien: `/devices.json` 3917 B, `/server.pem`
3562 B, `/setting.json` 2238 B.
Logs/Links: `eboxster/fs/result` Job `job-8431d2dacd11`.
Naechste Aktion: SPIFFS `used/total` Werte pruefen, da `fs/state` aktuell
`0 / 0` meldet.

### HW-10 SPIFFS Upload

Status: fail
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi MQTT Control, HTTP Data Plane
Setup: Testdatei `/rdm-hw-test-1784424361.txt`, 61 B, CRC32 `5fb68733`.
Schritte: `POST /api/files/upload?path=/rdm-hw-test-1784424361.txt` gesendet
und `fs/result` fuer Job `job-cf9e849b657c` abgewartet.
Erwartung: `put_url` endet mit `status=ok`.
Beobachtung: `put_url` endet mit `status=failed`, Detail `download_failed`.
Datei erscheint danach nicht in `list`.
Logs/Links: `eboxster/fs/result` Job `job-cf9e849b657c`.
Naechste Aktion: Device HTTP GET Pfad fuer gehostete Dateien debuggen; besonders
Host/Port/Public-Base-URL und HTTP-Client-Portbehandlung pruefen.

### HW-11 SPIFFS Download

Status: fail
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi MQTT Control, HTTP Data Plane
Setup: Downloadversuch fuer Testdatei `/rdm-hw-test-1784424361.txt`.
Schritte: `POST /api/files/download` gesendet und `fs/result` fuer Job
`job-ff21b2707578` abgewartet.
Erwartung: `get_url` endet mit `status=ok`, lokale Upload-Datei entspricht
Original.
Beobachtung: `get_url` endet mit `status=failed`, Detail `upload_failed`;
lokale Datei hat 0 B und passt nicht zur Testdatei.
Logs/Links: `eboxster/fs/result` Job `job-ff21b2707578`.
Naechste Aktion: Device HTTP PUT Pfad und Ziel-URL pruefen; HW-10 muss zuerst
stabil werden.

### HW-12 SPIFFS Delete

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi MQTT Control
Setup: Safe Delete sollte die vorher hochgeladene Testdatei loeschen.
Schritte: Delete fuer `/rdm-hw-test-1784424361.txt` gesendet.
Erwartung: Normale Testdatei wird geloescht.
Beobachtung: Upload in HW-10 ist fehlgeschlagen, daher existierte keine sichere
Testdatei. Delete Job `job-f41a17509094` meldete `status=failed`,
`detail=delete_failed`.
Logs/Links: `eboxster/fs/result` Job `job-f41a17509094`.
Naechste Aktion: Nach erfolgreichem Upload erneut testen; keine bestehenden
Produktivdateien fuer Delete-Test verwenden.

### HW-13 Filesystem Fehlerfaelle

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: Fehlerfalltests benoetigen funktionierende Basisoperationen.
Schritte: Vortests HW-10 bis HW-12 ausgewertet.
Erwartung: Fehlerfaelle werden nach erfolgreichem Basis-Upload/Download
gezielt erzeugt.
Beobachtung: Basisoperationen Upload/Download sind fehlgeschlagen; voller
Speicher und Schutzpfade wurden nicht getestet.
Logs/Links: HW-10, HW-11, HW-12.
Naechste Aktion: Nach Fix der HTTP File Data Plane automatisierte
Negativtests ergaenzen.

### HW-14 Lokale OTA mit CRC OK

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: OTA waere veraendernd und fuehrt Reboot/Flash aus.
Schritte: Build erzeugt Firmware erfolgreich; OTA Job nicht gestartet.
Erwartung: Nur mit expliziter Freigabe und Recovery-Pfad ausfuehren.
Beobachtung: Build `pio run -e tcall_a7670_v1_0` erfolgreich; keine lokale OTA
ausgefuehrt.
Logs/Links: PlatformIO Build SUCCESS, Flash 74.2 %, RAM 15.8 %.
Naechste Aktion: Nach Freigabe `.bin` hochladen, CRC dokumentieren und
Version nach Reboot pruefen.

### HW-15 Lokale OTA mit CRC Fehler

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: OTA-Negativtest benoetigt kontrollierten OTA-Job ohne Risiko fuer
Produktivfirmware.
Schritte: Nicht ausgefuehrt.
Erwartung: Falsche CRC fuehrt zu `status=failed` ohne Flash.
Beobachtung: Test wurde nicht gestartet, weil OTA-Jobs veraendernd sind und
HW-10/HW-11 HTTP Data Plane aktuell fehlschlagen.
Logs/Links: HW-10, HW-11.
Naechste Aktion: Nach HTTP Data Plane Fix und expliziter OTA-Freigabe testen.

### HW-16 GitHub Latest OTA

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: LTE/WiFi geplant
Setup: GitHub OTA veraendert Firmware und rebootet.
Schritte: Nicht ausgefuehrt.
Erwartung: Freigegebener Release-Stand wird geflasht und nach Reboot gemeldet.
Beobachtung: Nicht gestartet, weil keine explizite Freigabe fuer erneuten OTA in
diesem Testlauf vorlag und LTE HTTP/MQTT aktuell failt.
Logs/Links: HW-03, HW-05.
Naechste Aktion: Nach LTE/HTTP Fix und Release-Freigabe ausfuehren.

### HW-17 OTA Recovery

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: Erfordert absichtlichen Reset/Stromverlust waehrend OTA.
Schritte: Nicht ausgefuehrt.
Erwartung: Device bleibt bootfaehig oder rollt sauber zurueck.
Beobachtung: Ohne schaltbare Versorgung/Recovery-Plan nicht sicher
durchfuehrbar.
Logs/Links: Keine.
Naechste Aktion: Mit Labornetzteil oder steuerbarer Steckdose und serieller
Recovery-Konsole planen.

### HW-18 Broker Ausfall

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: NanoMQ lief auf Port `1883`; RemoteDeviceManager Tool nutzte denselben
Broker.
Schritte: Nicht ausgefuehrt, weil Stop von NanoMQ die laufende Testsession und
Device Control Plane unterbrechen wuerde.
Erwartung: Nach Broker Stop/Start reconnectet das Device.
Beobachtung: Backend Server Checks fuer MQTT LAN und Public waren `connack`.
Logs/Links: `/api/state.server_checks.mqtt_lan`, `.mqtt_public`.
Naechste Aktion: In separatem Reconnect-Testfenster mit automatischem
Broker-Restart ausfuehren.

### HW-19 HTTP Server Ausfall

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: HTTP Server lief auf Port `8093`.
Schritte: Nicht ausgefuehrt, um die laufende WebUI/API-Testsession nicht
abzubrechen.
Erwartung: Jobs melden Fehler und Device bleibt responsiv.
Beobachtung: HTTP LAN/Public Checks waren beide `200`.
Logs/Links: `/api/state.server_checks.http_lan`, `.http_public`.
Naechste Aktion: In separatem Testlauf mit Watchdog-Skript ausfuehren.

### HW-20 LTE Netzverlust

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: LTE
Setup: LTE war verbunden, aber MQTT/HTTP ueber LTE nicht erfolgreich.
Schritte: Kein Antennen-/SIM-/Providerverlust provoziert.
Erwartung: Netzverlust und Recovery werden sauber erkannt.
Beobachtung: Nicht getestet; LTE Basis ist bereits teilweise fehlerhaft
(`mqtt=false`, `http=false`).
Logs/Links: HW-03, HW-05.
Naechste Aktion: Erst LTE Data Plane stabilisieren, dann Netzverlust testen.

### HW-21 Power Cycle und Retained State

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: Kein steuerbarer Power-Cycle in diesem Lauf genutzt.
Schritte: Nicht ausgefuehrt.
Erwartung: Nach Power-Cycle werden retained States neu und plausibel
publiziert.
Beobachtung: Laufender Zustand ist plausibel, aber kein Power-Cycle-Nachweis.
Logs/Links: HW-01.
Naechste Aktion: Mit steuerbarer Versorgung wiederholen.

### HW-22 GNSS/Map Telemetrie

Status: fail
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi MQTT, GNSS
Setup: GPS Status ueber Console und `eboxster/gps` ausgewertet.
Schritte: `gps status` ausgefuehrt und GPS retained Payload gelesen.
Erwartung: `has_fix=true`, Position und Geschwindigkeit plausibel.
Beobachtung: `gps status` Exit-Code `0`, aber `GNSS raw: ,,,,,,,,`;
`valid=false`, `has_fix=false`, `sat_total=0`, Geschwindigkeit `0.0`.
Logs/Links: `eboxster/gps`, Console Output `gps status`.
Naechste Aktion: GNSS Antenne/Position/Freifeld oder Simulator pruefen.

### HW-23 Langzeittest

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: WiFi/LTE geplant
Setup: Kurztest lief wenige Minuten, kein 12-24 h Fenster.
Schritte: Nicht ausgefuehrt.
Erwartung: Langzeitbetrieb ohne Reconnect-/Heap-/Watchdog-Probleme.
Beobachtung: Fuer diesen Lauf nicht lang genug gemessen.
Logs/Links: Kurztest-Messung ueber `/api/state`, `rdm/alive`,
`console/out` und `fs/result`.
Naechste Aktion: Separates Dauertestskript ueber Nacht starten.

### HW-24 Versorgung/Brownout

Status: blocked
Datum: 2026-07-19 03:24 +02:00
Branch: feature/remote-device-manager
Commit: 0f70bda
Firmware-Version: dev
Device-ID: eboxster
Transport: Power
Setup: Kein Labornetzteil-/Brownout-Profil in diesem Lauf.
Schritte: Nicht ausgefuehrt.
Erwartung: Unterspannung erzeugt keinen korrupten FS-/OTA-Zustand.
Beobachtung: Nicht getestet.
Logs/Links: Keine.
Naechste Aktion: Mit Labornetzteil, Strommessung und serieller Konsole planen.
