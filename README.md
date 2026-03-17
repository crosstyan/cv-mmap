# cv-mmap

`cv-mmap` is the producer/runtime repository for the cvmmap shared-memory video IPC stack.
It captures frames from a real backend or a built-in synthetic backend, writes them into POSIX shared memory, publishes frame sync over ZeroMQ IPC, and, when NATS is enabled, serves control/status/body over NATS. With NATS disabled, the producer still publishes frames via shared memory + ZMQ frame sync, but control feedback, module status, and body-tracking transport are unavailable.

This repo also installs the reusable C++ package `cvmmap-core`, which is the canonical consumer-facing API used by:

- `cvmmap-streamer`
- `cv-mmap-gui`
- any other C++ consumer that needs cvmmap target resolution, ABI structs, metadata parsing, or client access

The Python consumer remains a separate project at `/home/crosstyan/Code/cvmmap-python-client`.

## Architecture

The current split is:

- `cv-mmap` executable: producer runtime, config parsing, backend orchestration
- `core/`: installable `cvmmap-core` package
- `app/`: producer-only implementation modules

`cvmmap-core` exports three CMake targets:

- `cvmmap::target`
- `cvmmap::ipc`
- `cvmmap::client`

Public headers are installed under `include/cvmmap/`:

- `<cvmmap/target.hpp>`
- `<cvmmap/ipc.hpp>`
- `<cvmmap/parser.hpp>`
- `<cvmmap/client.hpp>`

The public C++ namespace is `cvmmap`.

What stays in `cv-mmap` and is not part of `cvmmap-core`:

- capture backends (`opencv`, `gstreamer`, `zed`, `dummy`)
- producer runtime loop
- CLI and TOML config loading
- shared-memory creation and producer-side publish orchestration

## ZED Playback

The ZED backend now supports direct `.svo` and `.svo2` playback in addition to
live local cameras and ZED network streams.

Minimal playback example:

```toml
[video]
backend = "zed"

[zed]
stream_mode = "svo"
svo_path = "/data/example.svo2"
depth_mode = "neural"
```

Playback notes:

- SVO playback is reported as a finite source with `UnixEpochNs` frame timestamps
- seeking is available through the existing source control API unless `video.finite_stream_ending_behavior = "loop_silent"`
- `zed.resolution` and `zed.fps` are accepted in config for compatibility but ignored at runtime for SVO input
- SDK-side SVO recording controls are disabled while the backend is reading from an SVO file

## ABI Policy

The current protocol state is intentionally mixed-version:

- shared-memory frame metadata: v1 and v2 layouts exist, consumers are expected to handle both
- frame sync wire: v1 over ZMQ
- control and module status: protobuf over NATS when enabled
- body tracking: raw `cvmmap_body_tracking_v1` payload bytes over NATS when enabled

`cvmmap-core` owns the shared consumer-side protocol surface for:

- target and URI resolution
- IPC wire structs and constants
- SHM metadata parsing and validation
- C++ client access

Normative spec documents live under `docs/`, especially:

- `docs/cvmmap_sync_v1.ksy`
- `core/proto/cvmmap/control.proto`
- `docs/cvmmap_shm_metadata_v1_v2.ksy`
- `docs/cvmmap_body_tracking_v1.ksy`
- `docs/abi_changelog.md`
- `docs/abi_v2_contract_checklist.md`
- `docs/abi_v2_migration_guide.md`
- `docs/python-client.md`

Legacy control-wire fixtures and parsers remain in the tree for compatibility tests under `docs/cvmmap_control_v1.ksy`, but the live control plane is the protobuf schema in `core/proto/cvmmap/control.proto`.

## ZED Body Tracking Frame Metadata

When the ZED backend publishes body-tracking packets, the fixed 64-byte body
header now carries native 3D frame metadata without changing header size:

- `coordinate_system_code` byte: `coordinate_system`
- `reference_frame_code` byte: `reference_frame`
- `flags & 0x0010`: `floor_as_origin`

The v1 producer config surface is intentionally small:

- `zed.coordinate_system`: `IMAGE` or `RIGHT_HANDED_Y_UP`
- `zed.body_tracking.reference_frame`: `CAMERA` or `WORLD`
- `zed.body_tracking.set_floor_as_origin`: `true` or `false`

See `config_example.toml` and
`docs/cvmmap_body_tracking_v1.ksy` for the wire-level layout, and
`docs/abi_changelog.md` for ABI-level change history.

## Dummy Backend

`cv-mmap` now includes a built-in `dummy` backend for deterministic testing, replacing the need for downstream consumers to simulate their own producer.

Minimal example:

```toml
name = "example"

[video]
backend = "dummy"
finite_stream_ending_behavior = "loop"

[dummy]
width = 1280
height = 720
fps = 30
frames = 0
startup_delay_ms = 0
# optional: override the bundled JetBrains Mono font
# timestamp_overlay_font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
```

Explicit finite example:

```toml
name = "finite-example"

[video]
backend = "dummy"
finite_stream_ending_behavior = "stop"

[dummy]
width = 1280
height = 720
fps = 30
frames = 300
```

Dummy backend notes:

- output format is fixed to BGR8 / U8 / 3 channels
- every frame includes a burned-in top-left debug overlay with `frame`, `timestamp_ns`, and the timestamp domain (`media_ns` or `unix_ns`)
- the default overlay font is the bundled `JetBrainsMono-Regular.ttf`; `dummy.timestamp_overlay_font_path` can override it with a custom TTF
- `frames = 0` means live/infinite dummy source
- `frames > 0` means finite dummy source
- finite loop behavior is controlled entirely by `video.finite_stream_ending_behavior`:
  - `"stop"`: emit EOS and stop
  - `"loop"`: emit stream reset at wraparound and continue from frame 0
  - `"loop_silent"`: wrap internally with no reset event and no seek support
- `startup_delay_ms` delays first publish
- when used with finite-stream handling, it is suitable for acceptance and fault scenarios

## Build

```bash
cmake -B build -S .
cmake --build build
```

## C++ Compatibility

`cv-mmap` prefers the native C++23 standard library when the toolchain provides:

- `std::expected`
- `std::format`
- `std::move_only_function`

The project also ships compatibility headers under `core/include/cvmmap/compat/`
so older standard-library environments can still build:

- `cvmmap/compat/expected.hpp` uses `std::expected` when available, otherwise `tl::expected`
- `cvmmap/compat/format.hpp` uses `std::format` when available, otherwise `fmt`
- `cvmmap/compat/functional.hpp` uses `std::move_only_function` when available, otherwise `std::function`

In practice this means newer toolchains can build with native C++23 library
support, while older environments can keep working with `libfmt-dev` and
`libexpected-dev`.

Backend build defaults:

- `dummy` is always built.
- `BUILD_BACKEND_OPENCV=AUTO` enables the OpenCV backend when OpenCV is found.
- `BUILD_BACKEND_GSTREAMER=AUTO` enables the GStreamer backend when GStreamer development packages are found.
- `BUILD_BACKEND_ZED=OFF` keeps ZED disabled unless explicitly requested.

Examples:

```bash
cmake -B build -S . -DBUILD_BACKEND_OPENCV=OFF -DBUILD_BACKEND_GSTREAMER=OFF
cmake -B build -S . -DBUILD_BACKEND_OPENCV=ON -DBUILD_BACKEND_GSTREAMER=ON
```

This builds:

- `build/cv-mmap`
- the internal `app` producer libraries
- the installable `cvmmap-core` package targets

## Install `cvmmap-core`

```bash
cmake -B build -S .
cmake --build build
cmake --install build --prefix /tmp/cvmmap-core-prefix
```

Downstream CMake consumers can then use:

```cmake
find_package(cvmmap-core CONFIG REQUIRED)

target_link_libraries(my_consumer
	PRIVATE
		cvmmap::target
		cvmmap::ipc
		cvmmap::client)
```

## Run

```bash
./build/cv-mmap
./build/cv-mmap --config config_example.toml
./build/cv-mmap --config config_zed_1.toml
```

### Config inheritance with `extends`

`cv-mmap` supports single-parent TOML inheritance at the loader boundary. Add a top-level `extends` key pointing at another config file, then override only the keys that differ.

Inheritance rules:

- child scalars override parent scalars
- child tables merge recursively into parent tables
- child arrays replace parent arrays wholesale
- relative `extends` paths resolve from the child config file directory
- inheritance cycles and missing parents fail with a readable error

Example base config:

```toml
name = "zed1"

[video]
backend = "zed"

[zed]
stream_mode = "local"
index = 0
resolution = "AUTO"
fps = 30
depth_mode = "NONE"
```

Example overlay:

```toml
extends = "config_zed_base.toml"
name = "zed3"

[zed]
index = 2
```

The repo now ships `config_zed_base.toml` plus `config_zed_{1..4}.toml` overlays for the common multi-camera local ZED setup.

### Multi-instance startup with Process Compose

Use the checked-in `process-compose.yaml` when you want one command that starts several `cv-mmap` instances together, with a TUI for interactive use and optional detached mode for longer-running sessions.

```bash
process-compose --dry-run -f process-compose.yaml
process-compose -f process-compose.yaml
process-compose -D -f process-compose.yaml
```

By default the launcher expects the producer binary at `./build/cv-mmap`. Override it with `CVMMAP_BIN` if needed:

```bash
CVMMAP_BIN=/absolute/path/to/cv-mmap process-compose -f process-compose.yaml
```

The checked-in launcher starts four independent camera producers: `zed1`, `zed2`, `zed3`, and `zed4`, each with its own config overlay and `on_failure` restart policy.

### When to prefer systemd instead

Use a `systemd` template unit such as `cv-mmap@.service` when you need boot-time startup, restart after reboot, journal integration, or tighter OS-level service management. Keep `process-compose.yaml` as the primary in-repo workflow for development, testing, and user-managed multi-instance sessions.

When `nats.enabled = false`, startup continues in degraded producer-only mode: shared memory creation and ZMQ frame sync still run, but control/status transport and body-tracking transport are skipped.

## Dependencies

### Ubuntu

```bash
sudo apt install build-essential cmake pkg-config \
    libzmq3-dev \
    libfmt-dev \
    libexpected-dev \
    libspdlog-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libssl-dev

sudo apt install libopencv-dev \
    libglew-dev \
    libhdf5-dev

sudo apt install libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    gstreamer1.0-x \
    gstreamer1.0-alsa \
    gstreamer1.0-gl \
    gstreamer1.0-gtk3 \
    gstreamer1.0-qt5 \
    gstreamer1.0-pulseaudio
```

Ubuntu 22.04 notes:

- `cppzmq-dev` is not available from the default Ubuntu 22.04 repositories, so `cppzmq` must be installed manually.
- The default GCC toolchain on Ubuntu 22.04 does not provide the full C++23 standard-library surface needed for `std::expected` and `std::format`.
- On that toolchain, keep `libfmt-dev` and `libexpected-dev` installed. `libexpected-dev` provides the `tl::expected` fallback used by `cvmmap/compat/expected.hpp`.
- If you build with a newer compiler and newer libstdc++ that provide those C++23 library features, the compat layer will prefer the standard-library implementations automatically.

### Arch Linux

```bash
sudo pacman -S opencv \
	gst-plugins-base \
	gst-plugins-good \
	gst-plugins-bad \
	gst-plugins-ugly \
	gstreamer \
	cppzmq \
	spdlog \
	vtk \
	glew \
	hdf5
```

### Notes

- Base build requirements for the current default configuration are `cppzmq`, ZeroMQ, `spdlog`, Protobuf (`libprotobuf-dev` and `protobuf-compiler`), and OpenSSL (`libssl-dev`) for the vendored `nats.c` client.
- `fmt` and `tl::expected` are compatibility dependencies for toolchains that do not yet provide usable `std::format` and `std::expected`.
- OpenCV, GStreamer, and ZED support are optional build/runtime concerns depending on backend selection.
- `dummy` is always available and is the lowest-friction backend for local testing.

## Related Repositories

- Python client: `/home/crosstyan/Code/cvmmap-python-client`
- GUI consumer: `/home/crosstyan/Code/cv-mmap-gui`
- Streamer consumer: `/home/crosstyan/Code/cvmmap-streamer`

## TODO

- [ ] migrate transport/runtime to `iceoryx2`
- [ ] expand automated protocol and runtime coverage
- [x] improve direct GStreamer-side integration where backend-specific control is needed
