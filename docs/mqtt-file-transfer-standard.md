# RemoteDeviceManager MQTT Protocol

Version: `rdm-1`

RemoteDeviceManager uses MQTT only as a control plane. Files and firmware images are
transferred through HTTP URLs. The ESP32 is always the client, so the workflow is the
same for WiFi and LTE and does not require inbound connectivity to the device.

## Topics

Default device id and topic prefix: `eboxster`.

```text
eboxster/status
eboxster/gps
eboxster/rdm/state
eboxster/console/cmd
eboxster/console/out
eboxster/console/state
eboxster/console/cancel
eboxster/fs/jobs
eboxster/fs/result
eboxster/fs/state
eboxster/ota/jobs
eboxster/ota/result
eboxster/ota/state
```

All payloads are UTF-8 JSON. Jobs include `schema`, `msg_id`, `device_id`, `job_id`,
`op`, and optional operation fields.

## Device State

The device publishes retained state:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "fw_version": "dev",
  "transport": "simcom-native-mqtt",
  "mqtt_connected": true,
  "uptime_ms": 123456,
  "fs": {
    "used": 8192,
    "total": 196608
  }
}
```

`eboxster/console/*` is the console topic family.

## MQTT Console

Controller publishes:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "session_id": "webui",
  "command_id": "cmd-001",
  "command": "diag"
}
```

Device publishes output chunks to `console/out`:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "session_id": "webui",
  "command_id": "cmd-001",
  "seq": 1,
  "stream": "stdout",
  "encoding": "text",
  "payload": "Modem online\n",
  "final": false
}
```

The final message has `final: true`, `exit_code`, and `duration_ms`.

## File Jobs

### List

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-001",
  "op": "list"
}
```

Result:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-001",
  "op": "list",
  "status": "ok",
  "files": [
    {
      "path": "/config.json",
      "size": 123,
      "dir": false
    }
  ]
}
```

### Upload To Device

The controller hosts a file and publishes:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-002",
  "op": "put_url",
  "path": "/config.json",
  "url": "http://192.168.3.2:8080/files/job-002/config.json",
  "crc32": "1a2b3c4d"
}
```

The device downloads the URL with HTTP GET, writes the file to SPIFFS, verifies CRC32
when provided, and publishes `fs/result`.

### Download From Device

The controller publishes an upload URL:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-003",
  "op": "get_url",
  "path": "/log.txt",
  "url": "http://192.168.3.2:8080/uploads/job-003"
}
```

The device uploads the file with HTTP PUT.

### Delete

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-004",
  "op": "delete",
  "path": "/old.json"
}
```

The device rejects empty paths, `/`, paths containing `..`, and protected paths.

## OTA Jobs

### GitHub Latest Release

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-005",
  "op": "github_latest"
}
```

The device runs the configured GitHub latest-release OTA flow.

### Local Firmware URL

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-006",
  "op": "ota_url",
  "url": "http://192.168.3.2:8080/files/job-006/firmware.bin",
  "size": 1048576,
  "crc32": "89abcdef"
}
```

The device downloads the firmware over HTTP, verifies CRC32, commits the OTA image, and
reboots only after `Update.end(true)` succeeds.

## Security

Minimum for field use:

- MQTT authentication and per-device ACLs.
- Non-public or authenticated HTTP file URLs.
- CRC32 for demo OTA and file jobs; use signed metadata or SHA-256 before production.
- Disable dangerous console commands in production deployments.

## Non-Goals

- No MQTT blockstreams.
- No inbound server on the ESP32 for LTE.
- No broker-specific features.
