# cv-mmap Jetson bundle deployment

This Ansible area deploys an already-built portable bundle tarball to a Jetson over SSH.

It does not build the tarball. Build the bundle separately first, then point the playbook at the local `.tar.gz` artifact with `cvmmap_bundle_artifact`.

## Files

- `deploy.yml` — main deployment playbook
- `inventory/jetson.example.ini` — example inventory
- `group_vars/all.yml` — default variables
- `templates/nats-server.conf.j2` — NATS config template
- `templates/nats.service.j2` — NATS systemd unit template
- `templates/cvmmap-app-stack.service.j2` — app stack systemd unit template
- `templates/process-compose.yaml.j2` — generated process-compose file template

## Prerequisites

Control machine:

- Ansible installed, or install it first with `sudo apt-get update && sudo apt-get install -y ansible`
- SSH access to the Jetson
- Sudo access on the Jetson for package install and systemd-managed services
- A bundle tarball already produced by `tools/portable_bundle/package_jetson_bundle.sh`

Target host:

- Debian/Ubuntu-style apt environment
- Python available for Ansible modules

## Inventory setup

Copy the example inventory and adjust host, user, and SSH key as needed.

```bash
cp inventory/jetson.example.ini inventory/jetson.ini
```

Example inventory shape:

```ini
[jetson]
jetson-1 ansible_host=192.168.1.50 ansible_user=nvidia
```

You can also keep host-specific variables in inventory, but the recommended pattern is:

- inventory for connection details
- `group_vars/all.yml` for common deployment defaults
- `-e ...` overrides for per-deploy choices such as artifact path or release id

## Variables

### Required

- `cvmmap_bundle_artifact`
  - Local path on the control machine to the already-built bundle tarball.
  - Example: `/home/you/cv-mmap/out/portable_bundle/artifacts/cvmmap-jetson-bundle.tar.gz`

### Optional

- `cvmmap_release_id`
  - Override release directory name.
  - If omitted, the playbook derives it from the tarball basename.
- `cvmmap_root`
  - Remote install root.
  - Default: `/opt/cvmmap-bundle`
- `cvmmap_deploy_user`
  - Remote user that owns release and shared bundle content and runs the app stack.
  - By default this should match the SSH deploy user unless explicitly overridden.
- `cvmmap_start_nats`
  - Start NATS after install.
  - Default: `true`
- `cvmmap_start_srs`
  - Start SRS after install.
  - Default: `true`
- `cvmmap_install_app_stack_unit`
  - Install the `cvmmap-app-stack` systemd unit.
  - Default: `true`
- `cvmmap_start_app_stack`
  - Enable/start the app stack unit.
  - Default: `false`
- `cvmmap_streamer_args`
  - Extra arguments for `run-cvmmap-streamer`.
  - Default: empty
  - When empty, the streamer process is omitted from the generated process-compose stack.
- `cvmmap_force_srs_conf_sync`
  - Overwrite `/usr/local/srs/conf` from the bundle.
  - Default: `false`
- `cvmmap_force_shared_config_refresh`
  - Overwrite shared app config files from the bundle.
  - Default: `false`
- `cvmmap_nats_http_port`
  - Optional NATS monitoring HTTP port.

## Default behavior

A deploy installs or updates:

- extracted bundle release under `/opt/cvmmap-bundle/releases/<release>`
- `/opt/cvmmap-bundle/current` symlink to the active release
- shared app configs under `/opt/cvmmap-bundle/shared/config`
- generated runtime process-compose file under `/opt/cvmmap-bundle/shared/process-compose/process-compose.yaml`
- NATS config and systemd unit
- bundled SRS binary, systemd unit, and host config tree
- runtime apt dependencies via `current/bin/install-runtime-deps`

By default:

- NATS is installed and started
- SRS is installed and started
- the app stack systemd unit is installed
- the app stack unit is not enabled or started unless you opt in
- the streamer is not included unless `cvmmap_streamer_args` is set

## Preserve-by-default config behavior

Mutable state lives outside versioned releases.

### Shared app config

The playbook seeds shared config files from the bundle into:

- `/opt/cvmmap-bundle/shared/config/config_zed_base.toml`
- `/opt/cvmmap-bundle/shared/config/config_zed_1.toml`
- `/opt/cvmmap-bundle/shared/config/config_zed_2.toml`
- `/opt/cvmmap-bundle/shared/config/config_zed_3.toml`
- `/opt/cvmmap-bundle/shared/config/config_zed_4.toml`

Existing remote files are preserved by default. Set `cvmmap_force_shared_config_refresh=true` only when you intentionally want to replace them from the release payload.

### SRS config

The playbook preserves `/usr/local/srs/conf` by default. Set `cvmmap_force_srs_conf_sync=true` only when you intentionally want bundle-provided SRS config to overwrite the host copy.

## Remote layout

Versioned release content:

```text
/opt/cvmmap-bundle/
├── current -> /opt/cvmmap-bundle/releases/<release>
├── releases/
│   └── <release>/
└── shared/
    ├── config/
    ├── log/
    ├── nats/jetstream/
    ├── process-compose/
    └── state/
```

Use this split as the mental model:

- `releases/<release>`: immutable extracted bundle payload
- `current`: active release pointer
- `shared/...`: operator-edited config, generated runtime files, logs, and persistent state

## Deploy commands

Run from this directory:

```bash
cd /home/crosstyan/Code/cv-mmap/tools/portable_bundle/ansible
```

Basic deploy using a local tarball path:

```bash
ansible-playbook -i inventory/jetson.ini deploy.yml \
  -e cvmmap_bundle_artifact=/home/you/cv-mmap/out/portable_bundle/artifacts/cvmmap-jetson-bundle.tar.gz
```

Deploy with an explicit release id:

```bash
ansible-playbook -i inventory/jetson.ini deploy.yml \
  -e cvmmap_bundle_artifact=/home/you/cv-mmap/out/portable_bundle/artifacts/cvmmap-jetson-bundle.tar.gz \
  -e cvmmap_release_id=2026-04-15-jetson-test
```

Deploy and also start the app stack:

```bash
ansible-playbook -i inventory/jetson.ini deploy.yml \
  -e cvmmap_bundle_artifact=/home/you/cv-mmap/out/portable_bundle/artifacts/cvmmap-jetson-bundle.tar.gz \
  -e cvmmap_start_app_stack=true
```

Deploy, start the app stack, and include the streamer:

```bash
ansible-playbook -i inventory/jetson.ini deploy.yml \
  -e cvmmap_bundle_artifact=/home/you/cv-mmap/out/portable_bundle/artifacts/cvmmap-jetson-bundle.tar.gz \
  -e cvmmap_start_app_stack=true \
  -e 'cvmmap_streamer_args=--input nats://127.0.0.1:4222 --output rtsp://0.0.0.0:8554/live'
```

Force refresh of shared app configs:

```bash
ansible-playbook -i inventory/jetson.ini deploy.yml \
  -e cvmmap_bundle_artifact=/home/you/cv-mmap/out/portable_bundle/artifacts/cvmmap-jetson-bundle.tar.gz \
  -e cvmmap_force_shared_config_refresh=true
```

Force sync of SRS config:

```bash
ansible-playbook -i inventory/jetson.ini deploy.yml \
  -e cvmmap_bundle_artifact=/home/you/cv-mmap/out/portable_bundle/artifacts/cvmmap-jetson-bundle.tar.gz \
  -e cvmmap_force_srs_conf_sync=true
```

## What gets started

Infrastructure services are managed directly by systemd:

- `nats`
- `srs`

Application processes are managed by `process-compose` under a single systemd unit:

- `cvmmap-app-stack`

The generated app stack contains the ZED producer processes by default. It includes `cvmmap-streamer` only when `cvmmap_streamer_args` is non-empty.

## Post-deploy verification

Useful checks on the Jetson after a successful deploy:

```bash
readlink -f /opt/cvmmap-bundle/current
ls -la /opt/cvmmap-bundle/releases
ls -la /opt/cvmmap-bundle/shared/config
/opt/cvmmap-bundle/current/bin/install-runtime-deps --verify
sudo systemctl status nats --no-pager
sudo systemctl status srs --no-pager
sudo systemctl cat srs
sudo systemctl status cvmmap-app-stack --no-pager
sed -n '1,220p' /opt/cvmmap-bundle/shared/process-compose/process-compose.yaml
```

Quick active-state checks:

```bash
sudo systemctl is-active nats
sudo systemctl is-active srs
sudo systemctl is-active cvmmap-app-stack
```

Logs:

```bash
journalctl -u nats -n 50 --no-pager
journalctl -u srs -n 50 --no-pager
journalctl -u cvmmap-app-stack -n 50 --no-pager
```

## Notes

- Building the bundle and deploying the bundle are separate steps.
- This workflow assumes an SSH-reachable Jetson and an existing sudo-capable deploy user.
- The README documents only the workflow implemented in this Ansible area. If you add playbook behavior later, update this file in the same change.
