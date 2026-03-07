# cv-mmap

`cv-mmap` is the producer/runtime repository for the cvmmap shared-memory video IPC stack.
It captures frames from a real backend or a built-in synthetic backend, writes them into POSIX shared memory, and publishes sync and control messages over ZeroMQ IPC.

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

## ABI Policy

The current protocol state is intentionally mixed-version:

- shared-memory frame metadata: v1 and v2 layouts exist, consumers are expected to handle both
- sync/control wire messages: v1

`cvmmap-core` owns the shared consumer-side protocol surface for:

- target and URI resolution
- IPC wire structs and constants
- SHM metadata parsing and validation
- C++ client access

Normative spec documents live under `docs/`, especially:

- `docs/cvmmap_sync_v1.ksy`
- `docs/cvmmap_control_v1.ksy`
- `docs/cvmmap_shm_metadata_v1_v2.ksy`
- `docs/cvmmap_body_tracking_v1.ksy`
- `docs/abi_changelog.md`
- `docs/abi_v2_contract_checklist.md`
- `docs/abi_v2_migration_guide.md`
- `docs/python-client.md`

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
use_finite_as_infinite_stream = false
finite_stream_ending_behavior = "loop"

[dummy]
width = 1280
height = 720
fps = 30
frames = 0
startup_delay_ms = 0
```

Dummy backend notes:

- output format is fixed to BGR8 / U8 / 3 channels
- `frames = 0` means infinite stream
- `startup_delay_ms` delays first publish
- when used with finite-stream handling, it is suitable for acceptance and fault scenarios

## Build

```bash
cmake -B build -S .
cmake --build build
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
```

## Dependencies

### Ubuntu

```bash
apt install libopencv-dev \
    libzmq3-dev \
    libspdlog-dev \
    libglew-dev \
    libhdf5-dev

apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-tools gstreamer1.0-x gstreamer1.0-alsa gstreamer1.0-gl gstreamer1.0-gtk3 gstreamer1.0-qt5 gstreamer1.0-pulseaudio
```

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

- OpenCV, GStreamer, and ZED support are optional build/runtime concerns depending on backend selection.
- `dummy` is the lowest-friction backend for local testing.

## Related Repositories

- Python client: `/home/crosstyan/Code/cvmmap-python-client`
- GUI consumer: `/home/crosstyan/Code/cv-mmap-gui`
- Streamer consumer: `/home/crosstyan/Code/cvmmap-streamer`

## TODO

- [ ] migrate transport/runtime to `iceoryx2`
- [ ] expand automated protocol and runtime coverage
- [x] improve direct GStreamer-side integration where backend-specific control is needed
