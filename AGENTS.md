# PROJECT KNOWLEDGE BASE

**Generated:** 2026-03-06
**Target:** C++23 / CMake

## OVERVIEW
C++ producer/runtime for the cvmmap shared-memory video IPC stack. This repo captures frames, publishes them to shared memory, and also ships the installable `cvmmap-core` C++ package for downstream consumers.

## STRUCTURE
```
.
├── core/         # installable cvmmap-core package
│   ├── include/  # public headers under include/cvmmap/
│   ├── src/      # target/ipc/client implementation
│   └── fixtures/ # installed protocol fixtures
├── app/          # producer-only modules
│   ├── backends/ # OpenCV, GStreamer, ZED, and dummy implementations
│   ├── config/   # TOML parsing and target URI derivation
│   ├── lib/      # third-party dependencies (CLI11, toml++, spdlog)
│   └── models/   # producer-side ABI definitions mirrored into cvmmap-core
└── src/          # producer entry point (main.cpp)
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Adding a backend | `app/backends/` | Implement facade interface |
| Modifying config | `app/config/` | Update `app_config.cpp`, validation, and `.toml` formats |
| Changing public IPC/client contract | `core/include/cvmmap/` and `core/src/` | This is the consumer-facing source of truth |
| Changing producer-side ABI serialization | `app/models/` | Keep it aligned with `core/include/cvmmap/` and `docs/cvmmap.ksy` |
| Python consumer | external repo `cvmmap-python-client` | Importable Python package (`cvmmap`) |
| C++ downstream consumers | external repos `cvmmap-streamer`, `cv-mmap-gui` | They should link installed `cvmmap-core` directly |

## CONVENTIONS
- **Logging:** Use `spdlog` (`std::format` style), NEVER `std::cout`
- **CLI:** Use `CLI11` for argument parsing
- **Config:** `tomlplusplus` for parsing `.toml` files
- **Formatting:** Adhere to `.clang-format` (LLVM style, Tabs=4, No column limit)
- **Commits:** MUST use Conventional Commits format with detailed logs
- **Architecture:** Consumer process MUST NOT write to shared memory
- **Public API:** New reusable C++ API belongs under `core/include/cvmmap/` in namespace `cvmmap`

## ANTI-PATTERNS (THIS PROJECT)
- **Monolithic main:** `main.cpp` is currently 19KB. Avoid adding more logic there; refactor into `app/` instead.
- **Header placement:** Avoid putting headers in `src/inc/` unless strictly utility. Put them with their implementation in `app/`.
- **Leaking producer internals:** Do not make downstream consumers depend on `app/` headers. Expose reusable pieces through `cvmmap-core` instead.

## COMMANDS
```bash
# Build
cmake -B build -S .
cmake --build build

# Install cvmmap-core for downstream consumers
cmake --install build --prefix /tmp/cvmmap-core-prefix

# Run
./build/cv-mmap
```
