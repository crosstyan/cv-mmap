# Learnings — zed-backend-depth-plane

- Added model-level depth semantics for F32 depth plane payloads (meters) and explicit INVALID/unknown helper behavior for depth/plane enums.
- Verified explicit frame-plane typing in `FramePlaneType` and string conversion support in app_models helpers.
- Noted build failure pre-existing in environment: linker undefined references to fmt symbols; compile-time model changes themselves are clean.
- [2026-03-04] Task-3 helper updates added for `FramePlaneType`, depth-to-string helper, and explicit F32 depth semantic comment.
- [2026-03-04] Task-5 CMake wiring: added `WITH_BACKEND_ZED` option (default `OFF`) in `app/CMakeLists.txt` so default configure/build path does not require ZED SDK.
- [2026-03-04] ZED SDK discovery is now explicit and deterministic when enabled: checks `include/sl/Camera.hpp` plus `lib/libsl_zed` (or `lib64`) with `ZED_SDK_ROOT`/`$ENV{ZED_SDK_ROOT}` hints.
- [2026-03-04] Enabled ZED path defines `WITH_BACKEND_ZED=1` for `app_video_backends` and links through imported target `ZED::sl_zed`.
- [2026-03-04] Task-4: added `ZedConfig` model with optional `serial/index`, required `resolution/fps/depth_mode`, optional numeric defaults, and bool `reconnect`.
- [2026-03-04] Task-4 parsing now validates backend-specific section selection and errors explicitly when selecting `zed` without `[zed]`.
- [2026-03-04] Task-4 added `[zed]` section to `config_example.toml` demonstrating all configured fields and defaults.

- [2026-03-04] Restored functional ZED ethernet runtime path: stream_mode aliases now include local|usb|device|auto and network aliases ethernet|network|stream with canonical serialization to local/network.
- [2026-03-04] ZED backend now chooses init input at runtime: network mode calls setFromStream(ip[,port]) while local mode preserves serial/index/default camera selection and logs chosen path.
