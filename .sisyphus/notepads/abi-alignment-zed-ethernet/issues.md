## 2026-03-04: F3 QA blocker - full GUI app build/launch unavailable

- ~~Full GUI configure/build in this environment is blocked by missing OpenSSL discovery inputs.~~
- ~~Evidence command: `cmake -S . -B build-f3-gui && cmake --build build-f3-gui` in `/workspaces/zed-playground/cv-mmap-gui`.~~
- ~~CMake failure: `Could NOT find OpenSSL ... (missing: OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)` from `app/lib/nats.c/CMakeLists.txt`.~~
- ~~Impact: parser fixture checker remains runnable/passing, but full GUI executable launch cannot be verified here.~~

### UPDATE 2026-03-05: RESOLVED

- OpenSSL and GLFW dev packages now installed in environment.
- Remaining blocker was code-level: `PoseDetection` struct field mismatch (`timestamp_unix_ns` referenced but doesn't exist).
- Fixed: removed timestamp references from `main.cpp` logging code (lines 370-384).
- Full GUI build now passes: `cmake -B build-final-gui -S /workspaces/zed-playground/cv-mmap-gui && cmake --build build-final-gui` ✅
