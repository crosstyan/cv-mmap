# Plan: ZED Backend with Depth Plane Support

**Objective**: Add a ZED SDK backend that captures left RGB image + F32 depth plane, supporting single USB camera now with ethernet multi-cam extensibility later.

**Scope**: Single USB ZED camera (left + depth), optional SDK dependency, ABI v2 multi-plane header, clean config surface.

---

## Goals

1. Support ZED SDK capture (left image + depth) alongside existing OpenCV/GStreamer backends
2. Maintain ABI v2 compatibility with multi-plane shared memory layout
3. Keep ZED SDK dependency optional (graceful degrade when unavailable)
4. Provide clean config surface for resolution, FPS, depth mode, and connection params
5. Design for future ethernet multi-cam expansion (placeholder stubs)

---

## Constraints

- **ABI v2**: Multi-plane frame metadata with plane count + per-plane stride/offset/descriptor
- **Depth semantics**: F32 depth in meters, explicit INVALID/unknown handling
- **Config**: Bounded defaults (reconnect timeout, max failures, interval)
- **Build**: Optional backend via `WITH_BACKEND_ZED` (default OFF)
- **Consumer safety**: Consumer MUST NOT write to shared memory

---

## Dependencies

- ZED SDK (optional, via find_package)
- CUDA (required when ZED enabled)
- Existing backends (OpenCV, GStreamer) remain functional

---

## Task Checklist

### Phase 1: Foundation (ABI & Models)

- [x] **Task 1**: IPC v2 metadata contract
  - Status: COMPLETED
  - Note: Base ABI structures (sync_message_t, frame_info_t, frame_metadata_t) already in place
  - Location: `app/models/app_metadata_models.hpp`

- [ ] **Task 2**: Multi-plane ABI v2 header extension
  - Add multi-plane support to frame_info_t or create frame_info_v2_t
  - Define plane descriptor struct (offset, stride, width, height, format, depth)
  - Support up to 4 planes (RGB/BGR left + F32 depth + 2 reserved)
  - Maintain backward compatibility or bump version
  - Verify alignment and padding for ABI stability
  - Dependencies: Task 1

- [x] **Task 3**: Enums/helpers depth semantics
  - Status: COMPLETED
  - F32 depth type for meters: `Depth::F32` exists
  - INVALID/unknown helper behavior in helpers
  - `FramePlaneType` enum and string conversion helpers
  - Location: `app/models/app_enum_models.hpp`, `app/models/app_models.cpp`

### Phase 2: Configuration & Build

- [x] **Task 4**: ZED backend config surface
  - Status: COMPLETED
  - `ZedConfig` struct with fields:
    - `serial` (optional int)
    - `index` (optional int) 
    - `resolution` (required string: "720p", "1080p", "2K", "4K")
    - `fps` (required int)
    - `depth_mode` (required string: "performance", "quality", "ultra", "neural")
    - `open_timeout_ms` (default 10000)
    - `warmup_frames` (default 15)
    - `max_consecutive_failures` (default 30)
    - `reconnect_interval_ms` (default 1000)
    - `reconnect` (default true)
    - `left_pixel_format` (default "bgr8")
  - TOML parsing with validation (backend=zed requires [zed] section)
  - Error handling for missing config sections
  - Location: `app/config/app_config.hpp`, `app/config/app_config.cpp`, `config_example.toml`

- [x] **Task 5**: Optional ZED CMake integration (sample-style find_package)
  - Status: COMPLETED
  - `WITH_BACKEND_ZED` option (default OFF)
  - Sample-style discovery: `find_package(ZED REQUIRED)`, `find_package(CUDA ${ZED_CUDA_VERSION} REQUIRED)`
  - Support `LINK_SHARED_ZED` toggle (ON=shared, OFF=static)
  - Proper include/link directories
  - Compile definitions: `WITH_BACKEND_ZED=1`
  - Graceful error when SDK missing but requested
  - Location: `app/CMakeLists.txt`

### Phase 3: Backend Implementation

- [ ] **Task 6**: ZED backend implementation
  - Create `backends/app_backends_zed.cpp` + `app_backends_zed.hpp`
  - Implement facade pattern matching existing backends
  - Methods: `open()`, `close()`, `read_frame()`, `is_opened()`, `reset()`
  - Integrate with proxy pattern (msft_proxy4)
  - Handle ZED SDK lifecycle (Camera object, InitParameters, RuntimeParameters)
  - Extract left image (sl::VIEW::LEFT) and depth (sl::MEASURE::DEPTH)
  - Convert sl::Mat to appropriate formats for shared memory
  - Dependencies: Task 2, Task 4, Task 5

- [ ] **Task 7**: Depth plane metadata population
  - Populate multi-plane frame info (plane 0: left RGB/BGR, plane 1: F32 depth)
  - Set correct strides, offsets, dimensions for each plane
  - Depth in meters as F32
  - Mark depth plane type appropriately
  - Handle depth retrieval failure gracefully (plane count = 1 fallback)
  - Dependencies: Task 2, Task 6

- [ ] **Task 8**: Configuration wiring end-to-end
  - Wire `ZedConfig` into backend factory/creation
  - Map config fields to ZED SDK parameters
  - Runtime resolution/fps/depth_mode string to SDK enums
  - Validate config at open time
  - Test with `config_example.toml` [zed] section
  - Dependencies: Task 4, Task 6

### Phase 4: Polish & Future-Proofing

- [ ] **Task 9**: Graceful degrade / safe defaults
  - When `WITH_BACKEND_ZED=OFF`, backend unavailable at runtime
  - Clear error message when attempting to use zed backend without SDK
  - Ensure OpenCV/GStreamer backends unaffected
  - Verify default build (all optional backends OFF) still works
  - Dependencies: Task 5, Task 6

- [ ] **Task 10**: Ethernet-ready placeholders
  - Commented stubs for multi-cam ethernet support
  - Network config fields (optional): `ip_address`, `port`, `stream_mode`
  - Serial-based camera selection (already in config)
  - Document future extension points
  - No functional code (preparation only)
  - Dependencies: Task 4

### Phase 5: Verification

- [ ] **Final Verification**: Integration testing
  - Build with `-DWITH_BACKEND_ZED=ON` (when SDK available)
  - Build with `-DWITH_BACKEND_ZED=OFF` (default)
  - Run with config selecting zed backend
  - Verify shared memory layout matches ABI v2
  - Verify Python consumer can read frames
  - Verify depth values are in meters (F32)
  - Verify reconnect behavior
  - Dependencies: All previous tasks

---

## Verification Criteria

1. **Build**: `cmake -B build -S . -DWITH_BACKEND_ZED=ON` succeeds (with SDK)
2. **Default Build**: `cmake -B build -S .` succeeds without ZED SDK
3. **Config**: `[zed]` section in TOML properly parsed and validated
4. **Runtime**: Backend captures left image + depth plane to shared memory
5. **ABI**: Multi-plane metadata correctly describes both planes
6. **Consumer**: Python client reads both planes without errors
7. **Robustness**: Reconnect works after camera disconnect
8. **Isolation**: OpenCV/GStreamer backends unaffected

---

## Notes

- Current implementation targets single USB camera (ZED 2/ZED 2i)
- Ethernet multi-cam support deferred to future iteration
- Depth mode "neural" requires newer SDK (2.8+)
- Consider adding unit tests for config parsing (future)

## Related Files

- `app/models/app_metadata_models.hpp` - ABI v2 structures
- `app/models/app_enum_models.hpp` - Enums (Depth, PixelFormat, FramePlaneType)
- `app/models/app_models.cpp` - String conversion helpers
- `app/config/app_config.hpp` - Config structs
- `app/config/app_config.cpp` - TOML parsing
- `app/CMakeLists.txt` - Build configuration
- `app/backends/app_backends_zed.cpp` - Implementation (to create)
- `config_example.toml` - Example configuration

## Completed Work Summary

- Task 1: Base IPC metadata structures in place
- Task 3: Depth enum F32 + helpers completed
- Task 4: ZedConfig struct + parsing completed
- Task 5: CMake integration with sample-style find_package completed

## Remaining Work

- Task 2: Multi-plane ABI v2 header extension
- Task 6: ZED backend implementation
- Task 7: Depth plane metadata population
- Task 8: Configuration wiring end-to-end
- Task 9: Graceful degrade verification
- Task 10: Ethernet placeholders
- Final Verification: Integration testing
