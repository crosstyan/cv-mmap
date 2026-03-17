# Third-Party Dependencies

All vendored dependencies in this repo live under `third_party/`.

Dependencies:
- `CLI11` (git submodule): command-line parsing, target `CLI11::CLI11`
- `nats.c` (git submodule): NATS C client, target `cnats::nats_static`
- `proxy` (git submodule): proxy/rtti support, target `msft_proxy4::proxy`
- `spdlog` (git submodule): logging, target `spdlog::spdlog`
- `stb` (vendored header): text rasterization support for the dummy backend overlay, target `cvmmap_stb_truetype_headers`
- `tomlplusplus` (git submodule): TOML parsing, target `tomlplusplus::tomlplusplus`

Assets:
- `fonts/JetBrainsMono-Regular.ttf`: bundled default dummy overlay font
- `fonts/JetBrainsMono-OFL.txt`: bundled font license text

Bootstrap:
- `git submodule sync --recursive`
- `git submodule update --init --recursive`

Rule:
- New vendored dependencies must live under `third_party/` and be exposed through `third_party/CMakeLists.txt`.
