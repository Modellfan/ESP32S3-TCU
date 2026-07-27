# RemoteDeviceManager MQTT Protocol

Version: `rdm-1`

RemoteDeviceManager uses MQTT for command, status, console, and bounded SPIFFS file
transfer. File bytes are carried as small hex-encoded MQTT JSON chunks so the same
wire protocol works over WiFi and LTE without inbound connectivity to the ESP32.
OTA images remain URL-based because full firmware binaries are too large for this
simple JSON chunk path.

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
eboxster/fs/data
eboxster/fs/ack
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

### Upload To Device Over MQTT

The controller publishes a `put_mqtt` job first:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-002",
  "op": "put_mqtt",
  "path": "/config.json",
  "size": 123,
  "crc32": "1a2b3c4d",
  "encoding": "hex",
  "chunk_size": 384
}
```

The device responds on `fs/ack` with `status:"ready"`, then the controller publishes
chunks on `fs/data`:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-002",
  "op": "put_mqtt",
  "seq": 0,
  "offset": 0,
  "encoding": "hex",
  "data": "7b226d6f6465223a2274657374227d"
}
```

The device ACKs each chunk on `fs/ack`:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-002",
  "op": "put_mqtt",
  "seq": 0,
  "status": "ok"
}
```

When the expected byte count is received, the device verifies CRC32 and publishes
`fs/result`. The demo limit is 16 KiB with 384 byte raw chunks.

### Download From Device Over MQTT

The controller publishes:

```json
{
  "schema": "rdm-1",
  "device_id": "eboxster",
  "job_id": "job-003",
  "op": "get_mqtt",
  "path": "/log.txt",
  "encoding": "hex",
  "chunk_size": 384
}
```

The device publishes file chunks on `fs/data` with `op:"get_mqtt"` and a final
`fs/result` containing `size`, `chunks`, and `crc32`.

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
  "url": "http://192.168.3.2:8080/files/job-006/firmware.bin?exp=1785160000&sig=...",
  "size": 1048576,
  "crc32": "89abcdef",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

The device downloads the firmware over a time-limited signed URL, verifies CRC32
and SHA-256, commits the OTA image, and reboots only after `Update.end(true)`
succeeds.

## Security

Minimum for field use:

- Run the RemoteDeviceManager WebUI/API on localhost only. Remote UI access must
  stay disabled unless it is behind a trusted tunnel or VPN.
- Protect the WebUI/API with HTTP Basic Auth.
- Set `TCALL_RDM_SHARED_SECRET` on the device and `RDM_SHARED_SECRET` in the
  Python tool. MQTT commands and file chunks are HMAC-SHA256 signed; the device
  rejects missing or invalid signatures.
- Use MQTT broker authentication and per-device ACLs when the broker is exposed
  outside the local network.
- Use MQTT size limits for MQTT file transfer.
- For local OTA binaries, the Python tool publishes a signed, time-limited
  artifact URL and includes both CRC32 and SHA-256. The device verifies both
  before accepting the image.
- Disable dangerous console commands in production deployments.

## Non-Goals

- No broker-specific file-stream feature.
- No inbound server on the ESP32 for LTE.
- No broker-specific features.
