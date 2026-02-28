# Python Client

The Python client has been extracted from this repository into a standalone project:

- **Project path (local):** `/home/crosstyan/Code/cvmmap-python-client`
- **Planned upstream repo:** `cvmmap-python-client` (to be published)
- **Import name:** `cvmmap`

## Why extracted

This C++ repository focuses on producer-side capture/IPC logic. The Python consumer library now has:

- independent versioning/release cycle,
- cleaner packaging for external projects,
- optional dependency groups for tooling/tests.

## Dependency policy (library-friendly)

Core runtime deps are minimal:

- `numpy`
- `pyzmq`

Optional extras are used for non-core tasks:

- `tools`: `opencv-python`, `click`, `loguru`, `anyio` (examples/recording CLI)
- `test`: test runner dependencies
- `dev`: convenience superset (`tools` + `test`)

## Protocol compatibility note

The Python package must stay aligned with producer-side IPC structures under `app/models/` and `doc/cvmmap.ksy`.
