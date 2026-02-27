# PROJECT KNOWLEDGE BASE

**Generated:** 2026-02-27
**Target:** C++23 / CMake

## OVERVIEW
C++ OpenCV/GStreamer IPC capture application using ZeroMQ and POSIX shared memory. Captures frames and notifies consumers.

## STRUCTURE
```
.
├── app/          # Core modules (backends, config, IPC models)
│   ├── backends/ # OpenCV & GStreamer implementations
│   ├── config/   # TOML parsing 
│   ├── lib/      # Third-party dependencies (CLI11, toml++, spdlog)
│   └── models/   # IPC & Control message structures
├── client/       # Python client consumer
└── src/          # Entry point (main.cpp)
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Adding a backend | `app/backends/` | Implement facade interface |
| Modifying config | `app/config/` | Update `app_config.cpp` and `.toml` formats |
| Changing IPC payload | `app/models/` | Ensure C++/Python struct alignment |
| Python consumer | `client/` | Numpy + pyzmq logic |

## CONVENTIONS
- **Logging:** Use `spdlog` (`std::format` style), NEVER `std::cout`
- **CLI:** Use `CLI11` for argument parsing
- **Config:** `tomlplusplus` for parsing `.toml` files
- **Formatting:** Adhere to `.clang-format` (LLVM style, Tabs=4, No column limit)
- **Commits:** MUST use Conventional Commits format with detailed logs
- **Architecture:** Consumer process MUST NOT write to shared memory

## ANTI-PATTERNS (THIS PROJECT)
- **Monolithic main:** `main.cpp` is currently 19KB. Avoid adding more logic there; refactor into `app/` instead.
- **Header placement:** Avoid putting headers in `src/inc/` unless strictly utility. Put them with their implementation in `app/`.

## COMMANDS
```bash
# Build
cmake -B build -S . && cmake --build build

# Run
./build/cv-mmap
```
