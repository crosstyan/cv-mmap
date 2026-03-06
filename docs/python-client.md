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

The Python package remains independent, but it must stay aligned with:

- the versioned Kaitai schemas in:
  - `docs/cvmmap_sync_v1.ksy`
  - `docs/cvmmap_control_v1.ksy`
  - `docs/cvmmap_shm_metadata_v1_v2.ksy`
  - `docs/cvmmap_body_tracking_v1.ksy`
- the aggregate legacy reference in `docs/cvmmap.ksy`
- the installed `cvmmap-core` fixture contract in `core/fixtures/`
- the producer-side ABI structures in `app/models/`
- the consumer-facing C++ contract in `core/include/cvmmap/`

The Python package is not expected to link the C++ library. Its correctness comes from keeping its own protocol test suite in sync with the same ABI and URI contract.
