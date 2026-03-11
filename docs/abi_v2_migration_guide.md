# ABI v2 Migration Guide

**Scope:** Shared-memory metadata migration from v1 to v2.  
**Policy:** Control/sync wire remains v1 throughout the migration window.

---

## Version Policy

| Component | Version | Notes |
|-----------|---------|-------|
| SHM Metadata | v2 (major=2) | 256-byte header with plane descriptors |
| Control Wire | v1 (major=1) | Request/response structs unchanged |
| Sync Wire | v1 (major=1) | ZMQ pub/sub framing unchanged |

---

## Rollout Sequencing (Critical)

Deploy consumers **before** producer-only-v2 deployments.

1. Deploy updated Python client and GUI consumer (they parse v1/v2).
2. Deploy producer-only-v2 builds.
3. Never deploy producer-v2 before consumers are ready.

Failure to follow this sequence will result in consumer parse failures.

---

## Migration Window State

During the migration window, the system may be in a mixed state:

- **SHM metadata:** v2 (256-byte header with plane descriptors)
- **Control wire:** v1 (36-byte requests, 40-byte responses)
- **Sync wire:** v1 (24-byte sync headers)

This is the intended and supported configuration. Consumers must accept both v1 and v2 SHM metadata.

### Depth Unit Extension Within SHM v2

The v2 SHM header now assigns byte offset `0x2C` to `depth_unit` without changing
the major or minor version:

- `0` = `unknown`
- `1` = `millimeter`
- `2` = `meter`

Compatibility expectations:

- Older v2 producers that left the byte zeroed remain valid and decode as `unknown`.
- Updated consumers must treat `unknown` as "depth present but unit unspecified".
- Updated producers should set `depth_unit` whenever they publish a depth plane with
  a known metric unit.

---

## Consumer Compatibility Matrix

| Consumer Version | SHM v1 | SHM v2 | Control v1 | Notes |
|------------------|--------|--------|------------|-------|
| Python < 2.0 | Yes | No | Yes | Legacy, no depth plane support |
| Python >= 2.0 | Yes | Yes | Yes | Full v1/v2 support |
| GUI < 2.0 | Yes | No | Yes | Legacy |
| GUI >= 2.0 | Yes | Yes | Yes | Full v1/v2 support |

---

## ZED Configuration Validation

Config values are validated at parse time (not backend open time).

### Resolution

Canonical values: `HD2K`, `HD1200`, `HD1080`, `HD720`, `SVGA`, `VGA`, `AUTO`

Aliases accepted:
- `2k` → HD2K
- `1080p`, `fhd` → HD1080
- `720p`, `hd` → HD720

Invalid values (like deprecated `ultra`, `quality`, `performance`, or unknown `4k`) are rejected with a clear error message.

### Depth Mode

Canonical values: `NONE`, `NEURAL`, `NEURAL_LIGHT`, `NEURAL_PLUS`

Aliases accepted for neural variants:
- `neural light`, `neural-light` → `NEURAL_LIGHT`
- `neural plus`, `neural-plus` → `NEURAL_PLUS`

Deprecated legacy modes (`PERFORMANCE`, `QUALITY`, `ULTRA`) are rejected at config parse time.

### Stream Mode

Local modes: `local`, `usb`, `device`, `auto`
Network modes: `ethernet`, `network`, `stream` (all aliases for the same receiver behavior)

Network mode requirements:
- `ip_address` is required
- `port` defaults to 30000
- `serial` and `index` identifiers are NOT allowed

Local mode requirements:
- `serial` (number) or `index` (camera index) may be used
- `ip_address` and `port` are NOT allowed

Mixed identifier errors are caught at config parse time.

---

## Ethernet Streaming Behavior

When `stream_mode` is set to `ethernet`, `network`, or `stream`:

- The producer acts as a **receiver** (consumer) of a ZED sender's broadcast
- Uses the ZED SDK `StreamingReceiver` API
- The sender must be already broadcasting on the network
- Connection parameters: `ip_address` and `port`

This is passive reception, not active sender coordination. Sender management is a future enhancement.

---

## Fixture-Driven Parser Validation

Both Python and C++ client consumers use deterministic protocol fixtures for testing:

| Fixture | Purpose |
|---------|---------|
| v1 valid | Verify v1 backward compatibility |
| v2 left-only valid | Verify v2 single-plane parsing |
| v2 left+depth valid | Verify v2 two-plane parsing and `depth_unit=unknown` compatibility |
| v2 left+depth meter-unit valid | Verify v2 two-plane parsing with `depth_unit=meter` |
| v2 left+depth+confidence valid | Verify optional v2 confidence-plane parsing |
| v2 malformed | Verify rejection of out-of-bounds descriptors |

Test harnesses:
- Python: `pytest tests/test_import_and_protocol.py`
- `cvmmap-core`: build/install `cv-mmap`, then run downstream builds against the installed package
- downstream C++ consumers: `cvmmap-streamer` and `cv-mmap-gui` now validate ABI v2 support through their normal builds against `find_package(cvmmap-core CONFIG REQUIRED)`

---

## Reference Documents

- `docs/abi_v2_contract_checklist.md` - Full v2 field specifications
- `docs/cvmmap_shm_metadata_v1_v2.ksy` - versioned SHM metadata Kaitai schema
- `docs/cvmmap_sync_v1.ksy` - versioned sync message Kaitai schema
- `docs/cvmmap_control_v1.ksy` - versioned control message Kaitai schema
- `docs/cvmmap_body_tracking_v1.ksy` - versioned body tracking Kaitai schema
- split Kaitai references:
  - `docs/cvmmap_sync_v1.ksy`
  - `docs/cvmmap_control_v1.ksy`
  - `docs/cvmmap_shm_metadata_v1_v2.ksy`
  - `docs/cvmmap_body_tracking_v1.ksy`
- `docs/python-client.md` - Python client documentation
- `core/fixtures/uri_targets.json` - installed URI resolution fixture contract
