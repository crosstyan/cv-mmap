# cv-mmap-gui

GUI client for cv-mmap shared memory video streaming. Provides a visual interface for viewing video frames shared via POSIX shared memory.

## Testing

### Headless CLI Test (No GUI/OpenSSL Dependencies)

For CI environments or headless shells where full GUI build is blocked by OpenSSL dependencies, run the standalone protocol fixture checker:

```bash
# Build and run headless CLI test
cmake -S app/cvmmap-client/tests -B build-headless-cli \
    && cmake --build build-headless-cli \
    && ./build-headless-cli/protocol_fixture_check
```

This command:
1. Configures a minimal CMake project from `app/cvmmap-client/tests/CMakeLists.txt`
2. Builds the standalone `protocol_fixture_check` executable
3. Runs protocol fixture validation (v1, v2 left-only, v2 left+depth, malformed rejection)

Exit codes:
- `0` - All fixtures passed
- `1` - One or more fixtures failed
- `2` - Internal error

### Full GUI Build

For the complete GUI application with all features:

```bash
cmake -B build -S .
cmake --build build
./build/cv-mmap-gui
```

## Protocol Fixtures

The headless test validates these protocol scenarios:

| Fixture | Purpose |
|---------|---------|
| v1 valid | Verify backward compatibility with v1 metadata format |
| v2 left-only valid | Verify v2 single-plane (left camera) parsing |
| v2 left+depth valid | Verify v2 two-plane (left + depth) parsing |
| malformed rejection | Verify out-of-bounds plane counts, zero strides, and truncated data are rejected |

## Dependencies

- C++23 compatible compiler
- OpenSSL development headers/libraries (`libssl-dev`) for full GUI build
- GLFW development package (`libglfw3-dev`) for full GUI build
- ZeroMQ (for full GUI build)

### Ubuntu/Debian packages

```bash
sudo apt-get update
sudo apt-get install -y libssl-dev libglfw3-dev
```

## See Also

- [ABI v2 Migration Guide](../docs/abi_v2_migration_guide.md)
- [Protocol Specification](../docs/cvmmap.ksy)
