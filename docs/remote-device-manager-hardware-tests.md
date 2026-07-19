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

Status: not run

Ziel: Nach Flash oder Power-Cycle publiziert das Device `status`,
`rdm/state`, `fs/state`, `ota/state` und optional `gps`.

Automatisierbar: Teilautomatisch. Flash und Reset koennen automatisiert werden,
die Spannungsversorgung und physische Verkabelung bleiben hardwareabhaengig.

### HW-02 WiFi MQTT Control Plane

Status: not run

Ziel: Device verbindet ueber WiFi zum NanoMQ Broker, subscribed auf Control
Topics und publiziert Status/Alive-Antworten.

Automatisierbar: Automatisch ueber MQTT-Client und `/api/state`.

### HW-03 LTE MQTT Control Plane

Status: not run

Ziel: Device nutzt native SIMCom MQTT API und erreicht den Broker ueber den
oeffentlichen Endpoint.

Automatisierbar: Teilautomatisch. Skript kann Topic-Antworten messen, LTE
Abdeckung, SIM und Provider-Routing muessen real vorhanden sein.

### HW-04 WiFi HTTP Data Plane

Status: not run

Ziel: Device kann HTTP GET/PUT gegen die LAN-Adresse des Python-Tools ausfuehren.

Automatisierbar: Automatisch ueber File Upload/Download Jobs.

### HW-05 LTE HTTP Data Plane

Status: not run

Ziel: Device kann HTTP GET/PUT gegen den Public Endpoint des Python-Tools
ausfuehren.

Automatisierbar: Teilautomatisch. Der Test ist skriptbar, aber Portfreigabe,
IPv4/IPv6 Exposure und Mobilfunkrouting sind externe Voraussetzungen.

### HW-06 Alive Polling beider Links

Status: not run

Ziel: Alive Request/Response liefert getrennte WiFi- und LTE-Informationen fuer
MQTT und HTTP, auch wenn nur einer der Links aktiv fuer die Control Plane ist.

Automatisierbar: Automatisch ueber `rdm/alive/request`, `rdm/alive` und WebUI
DOM/API Checks.

### HW-07 Transport Umschalten

Status: not run

Ziel: Umschalten zwischen WiFi und LTE wird quittiert, der aktive Link wechselt
und die WebUI bleibt bedienbar.

Automatisierbar: Teilautomatisch. MQTT Job und Alive-Verifikation sind
automatisch, ein fehlschlagender LTE-Link kann manuelle Diagnose erfordern.

### HW-08 MQTT Console

Status: not run

Ziel: Console-Kommandos wie `help`, `mqtt status`, `wifi status`, `gps status`
werden ueber MQTT ausgefuehrt und Ausgabe erscheint in der WebUI.

Automatisierbar: Automatisch ueber `/api/console` und `console/out`.

### HW-09 SPIFFS List

Status: not run

Ziel: `list` Job liefert die aktuelle SPIFFS Struktur und die WebUI zeigt sie
als Datei-Icons.

Automatisierbar: Automatisch ueber `/api/files/list`.

### HW-10 SPIFFS Upload

Status: not run

Ziel: Datei wird im Tool gehostet, Device laedt sie per HTTP GET und schreibt sie
nach SPIFFS.

Automatisierbar: Automatisch. Erfolgsnachweis per `fs/result`, anschliessendem
`list` und optionalem Download-Vergleich.

### HW-11 SPIFFS Download

Status: not run

Ziel: Device laedt eine SPIFFS-Datei per HTTP PUT zum Tool hoch.

Automatisierbar: Automatisch. Erfolgsnachweis per `fs/result` und Byte-/CRC-
Vergleich der Upload-Datei.

### HW-12 SPIFFS Delete

Status: not run

Ziel: Normale Dateien koennen geloescht werden, `/`, Pfade mit `..` und
geschuetzte Pfade werden abgelehnt.

Automatisierbar: Automatisch ueber Delete-Jobs und Listen-Vergleich.

### HW-13 Filesystem Fehlerfaelle

Status: not run

Ziel: Volles SPIFFS, fehlende Dateien, ungueltige Pfade und HTTP-Fehler liefern
saubere Fehler und blockieren die Loop nicht.

Automatisierbar: Automatisch, sofern Testdateien und erwartete Fehlerfaelle
skriptgesteuert erzeugt werden.

### HW-14 Lokale OTA mit CRC OK

Status: not run

Ziel: Lokale `.bin` wird hochgeladen, CRC32 wird berechnet, Device verifiziert
die CRC und bootet anschliessend in die neue Firmware.

Automatisierbar: Teilautomatisch. Upload und Job sind skriptbar; Boot, Version
und Erreichbarkeit nach Reset muessen gemessen werden.

### HW-15 Lokale OTA mit CRC Fehler

Status: not run

Ziel: Manipulierte oder falsche CRC verhindert Flash und Device bleibt auf der
alten Firmware.

Automatisierbar: Automatisch ueber absichtlich falschen CRC-Job und
Versionsvergleich.

### HW-16 GitHub Latest OTA

Status: not run

Ziel: Device findet das neueste Release `.bin`, laedt es, prueft CRC/Metadaten
und flasht erfolgreich.

Automatisierbar: Teilautomatisch. GitHub Release Erstellung kann per `gh`
automatisiert werden, der Hardware-Reboot und die finale Erreichbarkeit bleiben
Integrationstest.

### HW-17 OTA Recovery

Status: not run

Ziel: Reset, Stromverlust oder Netzwerkabbruch waehrend OTA fuehrt nicht zu
einem dauerhaft nicht bootenden Device.

Automatisierbar: Manuell. Power-Cut und Timing muessen bewusst und mit
geeigneter Hardware durchgefuehrt werden.

### HW-18 Broker Ausfall

Status: not run

Ziel: Stop von NanoMQ oder Public Broker fuehrt zu Offline-Status, Reconnect und
erneuter Subscription nach Broker-Rueckkehr.

Automatisierbar: Teilautomatisch. Broker Stop/Start ist skriptbar, LTE Public
Routing kann externe Effekte haben.

### HW-19 HTTP Server Ausfall

Status: not run

Ziel: Wenn das Python HTTP Tool nicht erreichbar ist, melden File-/OTA-Jobs
einen Fehler und das Device bleibt responsiv.

Automatisierbar: Automatisch durch Stoppen des HTTP Servers und Job-Auswertung.

### HW-20 LTE Netzverlust

Status: not run

Ziel: LTE Signalverlust, APN-Fehler oder SIM-Probleme werden erkannt; nach
Rueckkehr verbindet sich das Device erneut.

Automatisierbar: Manuell. Netzverlust ist hardware-/providerabhaengig; Messung
der Recovery kann danach automatisiert erfolgen.

### HW-21 Power Cycle und Retained State

Status: not run

Ziel: Nach Neustart publiziert das Device wieder konsistente retained States und
die WebUI zeigt keine alten falschen Werte als aktuell an.

Automatisierbar: Teilautomatisch mit schaltbarer Steckdose oder Labornetzteil.

### HW-22 GNSS/Map Telemetrie

Status: not run

Ziel: GNSS Fix, Position, Satelliten und Geschwindigkeit werden publiziert und in
der Map angezeigt.

Automatisierbar: Teilautomatisch. Datenpruefung ist automatisch, echter Fix oder
Bewegung braucht reale Umgebung oder GNSS-Simulator.

### HW-23 Langzeittest

Status: not run

Ziel: 12-24 Stunden Betrieb mit regelmaessigen Alive, File-Listen und Console-
Kommandos ohne Heap-Abfall, Watchdog Reset oder Verbindungsverlust.

Automatisierbar: Teilautomatisch. Skript kann messen und Logs sammeln; Hardware
muss dauerhaft stabil versorgt und online bleiben.

### HW-24 Versorgung/Brownout

Status: not run

Ziel: Unterspannung und Versorgungseinbrueche fuehren nicht zu korruptem
Filesystem, haengendem Modem oder defektem OTA-Zustand.

Automatisierbar: Manuell. Benoetigt Labornetzteil, definierte Lastprofile und
kontrollierte Brownout-Szenarien.
