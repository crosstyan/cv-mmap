# Issues — zed-backend-depth-plane

- Could not verify diagnostics via clangd (binary unavailable in environment).
- Full link step fails in existing baseline due to fmt ABI/linker configuration (`undefined reference to fmt::v9`), not due to this model-only change.
- [2026-03-04] Notes:
  - clangd unavailable for lsp_diagnostics check.
  - Build passes model compilation units but fails at link stage due existing fmt symbol/linker mismatch.
- [2026-03-04] Task-5 verification notes:
  - `cmake -B build-zed -S . -DWITH_BACKEND_ZED=ON` now fails early at configure with actionable message when ZED SDK is missing (expected behavior for optional integration).
  - `lsp_diagnostics` for `app/CMakeLists.txt` is unsupported in this environment because no LSP server is configured for CMake/CMakeLists extension mapping.
- [2026-03-04] Task-4 config verification: `lsp_diagnostics` unavailable due missing clangd; build still fails in link stage from pre-existing `fmt::v9` ABI mismatch.
- [2026-03-04] Task-4 config parse check script (`python3` + tomllib) added/ran to confirm: valid `config_example.toml` passes and config selecting `zed` without `[zed]` fails as intended.
- [2026-03-04] Scope-fix pass: removed Task-5/cross-task contamination from working tree; only Task-4 files remain changed (`app/config/*`, `config_example.toml`, Task-4 notepad entries).
- [2026-03-04] Task-5 isolated execution: re-added `WITH_BACKEND_ZED` in `app/CMakeLists.txt` only; `cmake -B build-zed -S . -DWITH_BACKEND_ZED=ON` is consumed (no "Manually-specified variables were not used") and fails deterministically with actionable missing-SDK guidance.
- [2026-03-04] Task-5 correction pass: switched to ZED sample-style package discovery in `app/CMakeLists.txt` (`find_package(ZED REQUIRED)` + `find_package(CUDA ${ZED_CUDA_VERSION} REQUIRED)`), plus sample-style `${ZED_LIBRARIES}`/`${ZED_STATIC_LIBRARIES}` selection via `LINK_SHARED_ZED`; `WITH_BACKEND_ZED` remains optional/OFF by default.
- [2026-03-04] Verification: `cmake -B build-zed -S . -DWITH_BACKEND_ZED=ON` now consumes the option (no "Manually-specified variables were not used") and resolves via package flow in this environment; baseline default build still fails at existing `fmt::v9` linker issue.

- [2026-03-04] Verification commands executed; both default and ZED-enabled builds still fail at final link stage due pre-existing fmt::v9 unresolved symbols in environment (independent of ZED ethernet restoration).
