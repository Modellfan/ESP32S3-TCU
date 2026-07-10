# T-Call A7670 v1.0 GSM/GNSS Firmware

PlatformIO firmware for the LilyGo T-Call A7670 v1.0 on `COM12`. It provides a USB
serial console, WiFi TCP console, OTA firmware upload, GSM/LTE data proof, SMS commands,
and GNSS helpers for the SIMCom A7670 family via `lewisxhe/TinyGSM-fork`.

`Inputs/` is local reference material only and is ignored by git. The active firmware is
under `src/`, and host helper scripts are under `tools/`.

## Private Configuration

Copy the example config and fill in local values:

```powershell
Copy-Item src\LocalConfig.example.h src\LocalConfig.h
```

`src/LocalConfig.h` is ignored by git. Put WiFi, OTA hostname, SIM PIN, and APN values
there. Do not commit real passwords, SIM PINs, phone numbers, IMSI/ICCID/IMEI values, or
proof logs.

OTA upload passwords should be supplied locally:

```powershell
$env:TCALL_OTA_PASSWORD="your-ota-password"
pio run -e tcall_a7670_v1_0_ota -t upload --upload-flags "--auth=$env:TCALL_OTA_PASSWORD"
```

If mDNS does not resolve:

```powershell
pio run -e tcall_a7670_v1_0_ota -t upload --upload-port <board-ip-address> --upload-flags "--auth=$env:TCALL_OTA_PASSWORD"
```

## Build, Upload, Monitor

```powershell
pio run -e tcall_a7670_v1_0
pio run -e tcall_a7670_v1_0 -t upload
pio device monitor -p COM12 -b 115200
```

The first firmware load must be done over USB. After WiFi is configured, OTA can be used.
If OTA fails after a bad firmware image, recover with USB.

## Hardware

Target board: LilyGo T-Call A7670 v1.0. The v1.1 board uses different pins.

| Signal | GPIO |
| --- | ---: |
| Modem DTR | 14 |
| Modem TX | 26 |
| Modem RX | 25 |
| Modem PWRKEY | 4 |
| Board LED | 12 |
| Modem RING | 13 |
| Modem RESET | 27 |
| RESET active level | LOW |
| GNSS enable GPIO | -1 |

GNSS is built into A7670E-FASE and A7670SA-FASE variants. Use an active GNSS antenna
with sky view.

## Console

Open USB serial at `115200`, or connect to the WiFi TCP console on port `23`:

```powershell
python tools\wifi_console.py <board-ip-or-hostname>
python tools\wifi_console.py <board-ip-or-hostname> -c "wifi status"
```

Commands:

```text
help
diag
at <cmd>
sim [status|pin-off]
sms list [all|unread|read]
sms send <number> <message>
reg
operator auto|telekom|status
rat auto|lte
data up|down|status
gsm prove [timeout_seconds] [host] [path]
gsm reset
http <host> [path]
gps on|off|raw|fix|ex|cache|prove [timeout_seconds]|hot|cold|status
wifi status
```

`sms send` handles the `AT+CMGS` prompt and Ctrl-Z terminator internally. Use it instead
of raw `at +CMGS...`.

`gsm prove` waits for LTE/EPS registration, activates PDP data using the locally
configured APN, fetches an HTTP page through the modem, and prints `GSM PROOF PASS` only
after bytes are received from the web server.

`gps on` starts GNSS and enables the buffered GPS runner. `gps off` stops the runner so
GSM tests have exclusive use of the modem UART.

## Proof Helpers

```powershell
python tools\gsm_proof.py <board-ip-or-hostname> --timeout 180
python tools\gps_proof.py <board-ip-or-hostname> --timeout 900
```

Proof logs are written under `logs/`, which is ignored by git because logs can contain
network names, phone numbers, modem identifiers, and location data.

