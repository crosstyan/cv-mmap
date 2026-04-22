#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
workspace_root="$(cd "${repo_root}/.." && pwd)"

cv_mmap_repo="${CVMMAP_REPO:-${repo_root}}"
cv_mmap_gui_repo="${CVMMAP_GUI_REPO:-${workspace_root}/cv-mmap-gui}"
cvmmap_streamer_repo="${CVMMAP_STREAMER_REPO:-${workspace_root}/cvmmap-streamer}"
srs_repo="${SRS_REPO:-${workspace_root}/srs}"
output_root="${OUTPUT_ROOT:-${repo_root}/out/portable_bundle}"
bundle_name="${BUNDLE_NAME:-cvmmap-jetson-bundle}"
work_root="${WORK_ROOT:-${output_root}/work}"
artifact_root="${ARTIFACT_ROOT:-${output_root}/artifacts}"
bundle_root="${work_root}/${bundle_name}"
install_root="${work_root}/install"
download_root="${work_root}/downloads"
extract_root="${work_root}/extract"
build_root="${work_root}/build"
cmake_generator="${CMAKE_GENERATOR:-}"
cmake_build_type="${CMAKE_BUILD_TYPE:-Release}"
cmake_parallel_level="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
run_default_command="${RUN_DEFAULT_COMMAND:-}"
run_extract_subdir="${RUN_EXTRACT_SUBDIR:-${bundle_name}}"

readonly nats_version="v2.12.6"
readonly nats_asset="nats-server-v2.12.6-linux-arm64.tar.gz"
readonly nats_sha256="fddaf3f223c7af3f4d0a0d2c2fc084406e6b3ec7adfd1b9e6a37fbd03bfe222f"
readonly process_compose_version="v1.100.0"
readonly process_compose_asset="process-compose_linux_arm64.tar.gz"
readonly process_compose_sha256="5ce45fc12c2231b1deea5fe347878444bcf77706137db3760f7fa89e102008d4"
readonly runtime_packages=(
  libzmq5
  libprotobuf23
  libspdlog1
  libfmt8
  libssl3
  libglfw3
  libopengl0
  libglx0
  libx11-6
  libavcodec58
  libavformat58
  libavutil56
  libswscale5
)

usage() {
  cat <<'EOF'
Usage: package_jetson_bundle.sh [--output-root DIR] [--bundle-name NAME] [--run-default-command CMD]

Builds and stages the Jetson portable bundle, then emits both .tar.gz and self-extracting .run artifacts.

Environment overrides:
  CVMMAP_REPO, CVMMAP_GUI_REPO, CVMMAP_STREAMER_REPO, SRS_REPO
  OUTPUT_ROOT, WORK_ROOT, ARTIFACT_ROOT, BUNDLE_NAME
  CMAKE_GENERATOR, CMAKE_BUILD_TYPE, CMAKE_BUILD_PARALLEL_LEVEL
  RUN_DEFAULT_COMMAND
EOF
}

log() {
  printf '[portable-bundle] %s\n' "$*"
}

fail() {
  printf '[portable-bundle] ERROR: %s\n' "$*" >&2
  exit 1
}

require_file() {
  local path="$1"
  [[ -f "${path}" ]] || fail "required file missing: ${path}"
}

require_dir() {
  local path="$1"
  [[ -d "${path}" ]] || fail "required directory missing: ${path}"
}

require_command() {
  local name="$1"
  command -v "${name}" >/dev/null 2>&1 || fail "required command not found: ${name}"
}

real_file() {
  python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"
}


run_cmake_configure() {
  local source_dir="$1"
  local build_dir="$2"
  local install_prefix="$3"
  shift 3

  mkdir -p "${build_dir}"
  local -a cmd=(cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${cmake_build_type}" -DCMAKE_INSTALL_PREFIX="${install_prefix}")
  if [[ -n "${cmake_generator}" ]]; then
    cmd+=(-G "${cmake_generator}")
  fi
  cmd+=("$@")
  "${cmd[@]}"
}

run_cmake_build() {
  local build_dir="$1"
  cmake --build "${build_dir}" --parallel "${cmake_parallel_level}"
}

run_cmake_install() {
  local build_dir="$1"
  cmake --install "${build_dir}"
}

find_runtime_lib_dir() {
  local probe="$1"
  local resolved
  if [[ -x "${probe}" ]]; then
    resolved="$(real_file "${probe}")"
  else
    resolved="$(real_file "$(command -v "${probe}")")"
  fi
  local dir
  dir="$(dirname "${resolved}")/../lib64"
  [[ -d "${dir}" ]] || fail "expected GCC runtime lib directory next to ${probe}: ${dir}"
  printf '%s\n' "$(real_file "${dir}")"
}

copy_runtime_lib() {
  local source="$1"
  local destination_dir="$2"
  local resolved
  resolved="$(real_file "${source}")"
  cp -a "${resolved}" "${destination_dir}/"
}

stage_runtime_libs() {
  local dest_dir="$1"
  mkdir -p "${dest_dir}"

  local gcc_bin=""
  local -a gcc_candidates=(
	"${GCC_RUNTIME_BIN:-}"
	"${CXX:-}"
	"${CC:-}"
	"/opt/gcc-15.2.0/bin/g++"
	"/opt/gcc-15.2.0/bin/gcc"
	"$(command -v g++-15 || true)"
	"$(command -v gcc-15 || true)"
	"$(command -v g++ || true)"
	"$(command -v gcc || true)"
)
  local candidate
  for candidate in "${gcc_candidates[@]}"; do
    [[ -n "${candidate}" ]] || continue
    if [[ -x "${candidate}" ]]; then
      gcc_bin="${candidate}"
      break
    fi
  done
  [[ -n "${gcc_bin}" ]] || fail "unable to locate a GCC runtime probe binary; set GCC_RUNTIME_BIN to a GCC 15 compiler path"

  local lib_dir
  lib_dir="$(find_runtime_lib_dir "${gcc_bin}")"
  if [[ ! -f "${lib_dir}/libstdc++.so.6" || ! -f "${lib_dir}/libgcc_s.so.1" ]]; then
    local fallback_lib_dir="/opt/gcc-15.2.0/lib64"
    if [[ -f "${fallback_lib_dir}/libstdc++.so.6" && -f "${fallback_lib_dir}/libgcc_s.so.1" ]]; then
      lib_dir="$(real_file "${fallback_lib_dir}")"
    else
      fail "could not find GCC runtime libraries next to ${gcc_bin} or under ${fallback_lib_dir}"
    fi
  fi
  require_file "${lib_dir}/libstdc++.so.6"
  require_file "${lib_dir}/libgcc_s.so.1"

  local libstdcpp_target
  libstdcpp_target="$(readlink -f "${lib_dir}/libstdc++.so.6")"
  [[ -f "${libstdcpp_target}" ]] || fail "libstdc++.so.6 symlink target missing in ${lib_dir}"

  cp -a "${libstdcpp_target}" "${dest_dir}/"
  ln -sfn "$(basename "${libstdcpp_target}")" "${dest_dir}/libstdc++.so.6"
  copy_runtime_lib "${lib_dir}/libgcc_s.so.1" "${dest_dir}"

  local extra_count
  extra_count="$(find "${dest_dir}" -maxdepth 1 -type f | wc -l)"
  [[ "${extra_count}" -le 3 ]] || fail "bundle lib directory contains unexpected extra files"
}

sha256_verify() {
  local file="$1"
  local expected="$2"
  local actual
  actual="$(sha256sum "${file}" | awk '{print $1}')"
  [[ "${actual}" == "${expected}" ]] || fail "checksum mismatch for ${file}: expected ${expected}, got ${actual}"
}

fetch_release_asset() {
  local url="$1"
  local asset_path="$2"
  local expected_sha="$3"

  if [[ ! -f "${asset_path}" ]]; then
    log "downloading $(basename "${asset_path}")"
    curl --fail --location --retry 3 --output "${asset_path}" "${url}"
  fi
  sha256_verify "${asset_path}" "${expected_sha}"
}

extract_single_binary_from_tarball() {
  local tarball="$1"
  local pattern="$2"
  local output_path="$3"

  mkdir -p "$(dirname "${output_path}")"
  local member
  member="$(python3 - "${tarball}" "${pattern}" <<'PY'
import re
import subprocess
import sys

tarball, pattern = sys.argv[1:3]
regex = re.compile(pattern)
listing = subprocess.run(
    ["tar", "-tzf", tarball],
    check=True,
    stdout=subprocess.PIPE,
    text=True,
).stdout.splitlines()
for entry in listing:
    if regex.search(entry):
        print(entry)
        break
PY
  )"
  [[ -n "${member}" ]] || fail "could not locate ${pattern} in ${tarball}"
  tar -xzf "${tarball}" -C "$(dirname "${output_path}")" "${member}"
  local extracted
  extracted="$(dirname "${output_path}")/${member}"
  [[ -f "${extracted}" ]] || fail "expected extracted binary missing: ${extracted}"
  if [[ "$(real_file "${extracted}")" != "$(real_file "${output_path}")" ]]; then
    mv "${extracted}" "${output_path}"
  fi
  chmod 0755 "${output_path}"
}

write_runtime_helper() {
  local path="$1"
  cat >"${path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

mode="verify"
if [[ "${1:-}" == "--install" ]]; then
  mode="install"
  shift
elif [[ "${1:-}" == "--verify" ]]; then
  shift
fi
[[ $# -eq 0 ]] || {
  printf 'usage: %s [--verify|--install]\n' "$0" >&2
  exit 2
}

packages=(
  libzmq5
  libprotobuf23
  libspdlog1
  libfmt8
  libssl3
  libglfw3
  libopengl0
  libglx0
  libx11-6
  libavcodec58
  libavformat58
  libavutil56
  libswscale5
)

missing=()
for pkg in "${packages[@]}"; do
  if ! dpkg-query -W -f='${Status}' "${pkg}" 2>/dev/null | grep -q 'install ok installed'; then
    missing+=("${pkg}")
  fi
done

if [[ ${#missing[@]} -eq 0 ]]; then
  printf 'All required distro runtime packages are installed.\n'
  exit 0
fi

printf 'Missing distro runtime packages:\n' >&2
printf '  %s\n' "${missing[@]}" >&2

if [[ "${mode}" == "install" ]]; then
  sudo apt-get update
  sudo apt-get install -y "${missing[@]}"
  exit 0
fi

printf 'Run with --install to install the missing packages.\n' >&2
exit 1
EOF
  chmod 0755 "${path}"
}

write_run_cv_mmap() {
  local path="$1"
  cat >"${path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle_root="$(cd "${script_dir}/.." && pwd)"
config_default="${bundle_root}/share/cvmmap-bundle/config/config_zed_1.toml"
config_path="${CVMMAP_CONFIG:-}"
if [[ -z "${config_path}" ]]; then
  has_config_flag=0
  for arg in "$@"; do
    if [[ "${arg}" == "--config" || "${arg}" == --config=* ]]; then
      has_config_flag=1
      break
    fi
  done
  if [[ ${has_config_flag} -eq 0 ]]; then
    config_path="${config_default}"
  fi
fi

export LD_LIBRARY_PATH="${bundle_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
if [[ -n "${config_path}" ]]; then
  exec "${bundle_root}/bin/cv-mmap" --config "${config_path}" "$@"
fi
exec "${bundle_root}/bin/cv-mmap" "$@"
EOF
  chmod 0755 "${path}"
}

write_run_cv_mmap_gui() {
  local path="$1"
  cat >"${path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle_root="$(cd "${script_dir}/.." && pwd)"
state_root="${XDG_STATE_HOME:-${HOME}/.local/state}/cvmmap-bundle"
mkdir -p "${state_root}"
export LD_LIBRARY_PATH="${bundle_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export IMGUI_INI_FILE="${state_root}/imgui.ini"
cd "${state_root}"
exec "${bundle_root}/bin/cv_mmap_gui" "$@"
EOF
  chmod 0755 "${path}"
}

write_run_streamer() {
  local path="$1"
  cat >"${path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle_root="$(cd "${script_dir}/.." && pwd)"
export LD_LIBRARY_PATH="${bundle_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${bundle_root}/bin/cvmmap_streamer" "$@"
EOF
  chmod 0755 "${path}"
}

write_run_nats() {
  local path="$1"
  cat >"${path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle_root="$(cd "${script_dir}/.." && pwd)"
exec "${bundle_root}/tools/nats-server" "$@"
EOF
  chmod 0755 "${path}"
}

write_srs_systemd_unit() {
  local path="$1"
  cat >"${path}" <<'EOF'
[Unit]
Description=SRS media server
Documentation=https://github.com/ossrs/srs
After=network-online.target remote-fs.target nss-lookup.target
Wants=network-online.target

[Service]
Type=forking
WorkingDirectory=/usr/local/srs
PIDFile=/usr/local/srs/objs/srs.pid
LimitNOFILE=65535
LimitCORE=infinity
ExecStartPre=/usr/local/srs/objs/srs -t -c /usr/local/srs/conf/srs.conf
ExecStart=/usr/local/srs/objs/srs -c /usr/local/srs/conf/srs.conf
ExecReload=/bin/kill -s HUP $MAINPID
ExecStop=/bin/kill -s TERM $MAINPID
TimeoutStopSec=30
Restart=on-failure
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF
  chmod 0644 "${path}"
}

write_srs_readme() {
  local path="$1"
  cat >"${path}" <<'EOF'
# Bundled SRS assets

This bundle stages SRS in an install-shaped layout so it can be copied onto a target host without the SRS source tree.

## Included assets

- `/usr/local/srs/objs/srs`
- `/usr/local/srs/conf/*`
- `/usr/lib/systemd/system/srs.service`

## Tuned defaults

- `/usr/local/srs/conf/srs.conf` sets `max_connections 10000;`
- `srs.service` sets `LimitNOFILE=65535`
- `srs.service` validates config before start with `srs -t -c /usr/local/srs/conf/srs.conf`

## Install example

```bash
sudo mkdir -p /usr/local/srs
sudo cp -a ./usr/local/srs/. /usr/local/srs/
sudo install -Dm644 ./usr/lib/systemd/system/srs.service /usr/lib/systemd/system/srs.service
sudo systemctl daemon-reload
sudo systemctl enable --now srs
```

## Operations

- Reload config: `sudo systemctl reload srs`
- Stop service: `sudo systemctl stop srs`
- Graceful quit equivalent: `sudo kill -SIGQUIT $(cat /usr/local/srs/objs/srs.pid)`
- Reopen logs equivalent: `sudo kill -SIGUSR1 $(cat /usr/local/srs/objs/srs.pid)`
EOF
  chmod 0644 "${path}"
}

write_manifest() {
  local path="$1"
  python3 - "${path}" "${bundle_name}" "${cmake_build_type}" "$(real_file "${cv_mmap_repo}")" "$(real_file "${cv_mmap_gui_repo}")" "$(real_file "${cvmmap_streamer_repo}")" "$(real_file "${srs_repo}")" "${nats_version}" "${nats_asset}" "${nats_sha256}" "${process_compose_version}" "${process_compose_asset}" "${process_compose_sha256}" "${runtime_packages[@]}" <<'PY'
import json
import sys
from datetime import datetime, timezone

(
    path,
    bundle_name,
    cmake_build_type,
    cv_mmap_repo,
    cv_mmap_gui_repo,
    cvmmap_streamer_repo,
    srs_repo,
    nats_version,
    nats_asset,
    nats_sha256,
    process_compose_version,
    process_compose_asset,
    process_compose_sha256,
    *runtime_packages,
) = sys.argv[1:]

manifest = {
    "bundle_name": bundle_name,
    "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "cmake_build_type": cmake_build_type,
    "repos": {
        "cv-mmap": cv_mmap_repo,
        "cv-mmap-gui": cv_mmap_gui_repo,
        "cvmmap-streamer": cvmmap_streamer_repo,
        "srs": srs_repo,
    },
    "profiles": {
        "cv-mmap": {
            "BUILD_BACKEND_ZED": "ON",
            "BUILD_BACKEND_MCAP": "OFF",
            "BUILD_BACKEND_OPENCV": "OFF",
            "BUILD_BACKEND_GSTREAMER": "OFF",
        },
        "cv-mmap-gui": {
            "CVMMAP_CNATS_PROVIDER": "workspace",
        },
        "cvmmap-streamer": {
            "CVMMAP_CNATS_PROVIDER": "workspace",
            "CVMMAP_STREAMER_ENABLE_MCAP": "OFF",
            "CVMMAP_STREAMER_ENABLE_MCAP_DEPTH": "OFF",
        },
    },
    "upstream_assets": {
        "nats-server": {
            "version": nats_version,
            "asset": nats_asset,
            "sha256": nats_sha256,
        },
        "process-compose": {
            "version": process_compose_version,
            "asset": process_compose_asset,
            "sha256": process_compose_sha256,
        },
    },
    "bundled_install_assets": {
        "srs": {
            "binary": "share/cvmmap-bundle/srs/usr/local/srs/objs/srs",
            "config_dir": "share/cvmmap-bundle/srs/usr/local/srs/conf",
            "systemd_unit": "share/cvmmap-bundle/srs/usr/lib/systemd/system/srs.service",
            "readme": "share/cvmmap-bundle/srs/README.md",
            "tuned_defaults": {
                "max_connections": 10000,
                "limit_nofile": 65535,
            },
        },
    },
    "runtime_packages": runtime_packages,
}

with open(path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY
}

write_self_extracting_runner() {
  local run_path="$1"
  local tarball_name="$2"
  local default_command="$3"

  cat >"${run_path}" <<EOF
#!/usr/bin/env bash
set -euo pipefail

script_path="\${BASH_SOURCE[0]}"
payload_name="${tarball_name}"
default_extract_root="\${PWD}"
default_bundle_dir="${run_extract_subdir}"
default_command="${default_command}"

target_dir=""
launch_after_extract=0
print_next_steps=0
remaining=()

usage() {
  cat <<'USAGE'
Usage: ./$(basename "${run_path}") [--target-dir DIR] [--launch] [--print-next-steps] [-- ARGS...]
USAGE
}

while [[ \$# -gt 0 ]]; do
  case "\$1" in
    --target-dir)
      [[ \$# -ge 2 ]] || { usage >&2; exit 2; }
      target_dir="\$2"
      shift 2
      ;;
    --launch)
      launch_after_extract=1
      shift
      ;;
    --print-next-steps)
      print_next_steps=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      remaining=("\$@")
      break
      ;;
    *)
      remaining+=("\$1")
      shift
      ;;
  esac
done

if [[ -z "\${target_dir}" ]]; then
  target_dir="\${default_extract_root}/\${default_bundle_dir}"
fi
mkdir -p "\${target_dir}"
archive_tmp="\$(mktemp "\${TMPDIR:-/tmp}/cvmmap-bundle.XXXXXX.tar.gz")"
cleanup() {
  rm -f "\${archive_tmp}"
}
trap cleanup EXIT

python3 - "\${script_path}" "\${archive_tmp}" <<'PY'
from pathlib import Path
import sys

script_path = Path(sys.argv[1])
archive_path = Path(sys.argv[2])
marker = b"__ARCHIVE_BELOW__\n"
data = script_path.read_bytes()
index = data.find(marker)
if index < 0:
    raise SystemExit("payload marker not found")
archive_path.write_bytes(data[index + len(marker):])
PY
tar -xzf "\${archive_tmp}" -C "\${target_dir}" --strip-components=1
bundle_dir="\${target_dir}"

if [[ \${launch_after_extract} -eq 1 ]]; then
  if [[ -n "\${default_command}" ]]; then
    exec "\${bundle_dir}/bin/\${default_command}" "\${remaining[@]}"
  fi
  exec "\${bundle_dir}/bin/run-cv-mmap" "\${remaining[@]}"
fi

printf 'Bundle extracted to %s\n' "\${bundle_dir}"
if [[ \${print_next_steps} -eq 1 || \${launch_after_extract} -eq 0 ]]; then
  printf 'Verify apt dependencies: %s\n' "\${bundle_dir}/bin/install-runtime-deps --verify"
  printf 'Start NATS: %s\n' "\${bundle_dir}/bin/run-nats -js"
  printf 'Start default producer: %s\n' "\${bundle_dir}/bin/run-cv-mmap"
  printf 'Start GUI: %s\n' "\${bundle_dir}/bin/run-cv-mmap-gui"
fi
exit 0
EOF
  printf '__ARCHIVE_BELOW__\n' >>"${run_path}"
  cat "${artifact_root}/${tarball_name}" >>"${run_path}"
  chmod 0755 "${run_path}"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --output-root)
        [[ $# -ge 2 ]] || fail "--output-root requires a value"
        output_root="$2"
        work_root="${output_root}/work"
        artifact_root="${output_root}/artifacts"
        bundle_root="${work_root}/${bundle_name}"
        install_root="${work_root}/install"
        download_root="${work_root}/downloads"
        extract_root="${work_root}/extract"
        build_root="${work_root}/build"
        run_extract_subdir="${RUN_EXTRACT_SUBDIR:-${bundle_name}}"
        shift 2
        ;;
      --bundle-name)
        [[ $# -ge 2 ]] || fail "--bundle-name requires a value"
        bundle_name="$2"
        bundle_root="${work_root}/${bundle_name}"
        run_extract_subdir="${RUN_EXTRACT_SUBDIR:-${bundle_name}}"
        shift 2
        ;;
      --run-default-command)
        [[ $# -ge 2 ]] || fail "--run-default-command requires a value"
        run_default_command="$2"
        shift 2
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        fail "unknown argument: $1"
        ;;
    esac
  done
}

stage_bundle_layout() {
  mkdir -p \
    "${bundle_root}/bin" \
    "${bundle_root}/lib" \
    "${bundle_root}/share/cvmmap-bundle/config" \
    "${bundle_root}/share/cvmmap-bundle/fonts" \
    "${bundle_root}/share/cvmmap-bundle/srs/usr/local/srs/conf" \
    "${bundle_root}/share/cvmmap-bundle/srs/usr/local/srs/objs" \
    "${bundle_root}/share/cvmmap-bundle/srs/usr/lib/systemd/system" \
    "${bundle_root}/tools" \
    "${install_root}" \
    "${download_root}" \
    "${extract_root}" \
    "${build_root}" \
    "${artifact_root}"
}

build_and_install_repos() {
  local cv_mmap_build_dir="${build_root}/cv-mmap"
  local cv_mmap_install_dir="${install_root}/cv-mmap"
  local gui_build_dir="${build_root}/cv-mmap-gui"
  local gui_install_dir="${install_root}/cv-mmap-gui"
  local streamer_build_dir="${build_root}/cvmmap-streamer"
  local streamer_install_dir="${install_root}/cvmmap-streamer"

  local gcc_cxx="${GCC_RUNTIME_BIN:-}"
  if [[ -z "${gcc_cxx}" ]]; then
    if [[ -x "/opt/gcc-15.2.0/bin/g++" ]]; then
      gcc_cxx="/opt/gcc-15.2.0/bin/g++"
    elif [[ -n "${CXX:-}" ]]; then
      gcc_cxx="${CXX}"
    fi
  fi
  local gcc_cc="${CC:-}"
  if [[ -z "${gcc_cc}" && -n "${gcc_cxx}" ]]; then
    if [[ "${gcc_cxx}" == *"/g++" ]]; then
      gcc_cc="${gcc_cxx%/g++}/gcc"
    elif [[ "${gcc_cxx}" == *"/c++" ]]; then
      gcc_cc="${gcc_cxx%/c++}/gcc"
    fi
  fi
  local -a common_cmake_args=()
  if [[ -n "${gcc_cxx}" ]]; then
    common_cmake_args+=("-DCMAKE_CXX_COMPILER=${gcc_cxx}")
  fi
  if [[ -n "${gcc_cc}" ]]; then
    common_cmake_args+=("-DCMAKE_C_COMPILER=${gcc_cc}")
  fi

  log "configuring cv-mmap"
  run_cmake_configure "${cv_mmap_repo}" "${cv_mmap_build_dir}" "${cv_mmap_install_dir}" \
    "${common_cmake_args[@]}" \
    -DBUILD_BACKEND_ZED=ON \
    -DBUILD_BACKEND_MCAP=OFF \
    -DBUILD_BACKEND_OPENCV=OFF \
    -DBUILD_BACKEND_GSTREAMER=OFF
  log "building cv-mmap"
  run_cmake_build "${cv_mmap_build_dir}"
  log "installing cv-mmap"
  run_cmake_install "${cv_mmap_build_dir}"

  log "configuring cv-mmap-gui"
  run_cmake_configure "${cv_mmap_gui_repo}" "${gui_build_dir}" "${gui_install_dir}" \
    "${common_cmake_args[@]}" \
    -DCVMMAP_CNATS_PROVIDER=workspace \
    -DCVMMAP_LOCAL_ROOT="${cv_mmap_repo}" \
    -DCVMMAP_LOCAL_BUILD="${cv_mmap_build_dir}/core" \
    -DCVMMAP_LOCAL_NATS_STATIC="${cv_mmap_build_dir}/lib/libnats_static.a" \
    -DCVMMAP_LOCAL_CORE_DIR="${cv_mmap_build_dir}/core"
  log "building cv-mmap-gui"
  run_cmake_build "${gui_build_dir}"
  log "installing cv-mmap-gui"
  run_cmake_install "${gui_build_dir}"

  log "configuring cvmmap-streamer"
  run_cmake_configure "${cvmmap_streamer_repo}" "${streamer_build_dir}" "${streamer_install_dir}" \
    "${common_cmake_args[@]}" \
    -DCVMMAP_CNATS_PROVIDER=workspace \
    -DCVMMAP_LOCAL_ROOT="${cv_mmap_repo}" \
    -DCVMMAP_LOCAL_BUILD="${cv_mmap_build_dir}/core" \
    -DCVMMAP_LOCAL_CORE_DIR="${cv_mmap_build_dir}/core" \
    -DCVMMAP_LOCAL_NATS_STATIC="${cv_mmap_build_dir}/lib/libnats_static.a" \
    -DCVMMAP_STREAMER_ENABLE_MCAP=OFF \
    -DCVMMAP_STREAMER_ENABLE_MCAP_DEPTH=OFF
  log "building cvmmap-streamer"
  run_cmake_build "${streamer_build_dir}"
  log "installing cvmmap-streamer"
  run_cmake_install "${streamer_build_dir}"
}

stage_installed_binaries() {
  require_file "${install_root}/cv-mmap/bin/cv-mmap"
  require_file "${install_root}/cv-mmap-gui/bin/cv_mmap_gui"
  require_file "${install_root}/cvmmap-streamer/bin/cvmmap_streamer"

  cp -a "${install_root}/cv-mmap/bin/cv-mmap" "${bundle_root}/bin/"
  cp -a "${install_root}/cv-mmap-gui/bin/cv_mmap_gui" "${bundle_root}/bin/"
  cp -a "${install_root}/cvmmap-streamer/bin/cvmmap_streamer" "${bundle_root}/bin/"
}

stage_assets() {
  require_file "${repo_root}/third_party/fonts/JetBrainsMono-Regular.ttf"
  require_file "${script_dir}/process-compose.yaml"
  require_file "${script_dir}/config_zed_base.toml"
  require_file "${script_dir}/config_zed_1.toml"
  require_file "${script_dir}/config_zed_2.toml"
  require_file "${script_dir}/config_zed_3.toml"
  require_file "${script_dir}/config_zed_4.toml"

  cp -a "${repo_root}/third_party/fonts/JetBrainsMono-Regular.ttf" "${bundle_root}/share/cvmmap-bundle/fonts/"
  cp -a "${script_dir}/process-compose.yaml" "${bundle_root}/share/cvmmap-bundle/config/"
  cp -a "${script_dir}/config_zed_base.toml" "${bundle_root}/share/cvmmap-bundle/config/"
  cp -a "${script_dir}/config_zed_1.toml" "${bundle_root}/share/cvmmap-bundle/config/"
  cp -a "${script_dir}/config_zed_2.toml" "${bundle_root}/share/cvmmap-bundle/config/"
  cp -a "${script_dir}/config_zed_3.toml" "${bundle_root}/share/cvmmap-bundle/config/"
  cp -a "${script_dir}/config_zed_4.toml" "${bundle_root}/share/cvmmap-bundle/config/"
}

stage_srs_assets() {
  local srs_trunk_dir="${srs_repo}/trunk"
  local staged_conf_dir="${bundle_root}/share/cvmmap-bundle/srs/usr/local/srs/conf"
  local staged_service_unit="${bundle_root}/share/cvmmap-bundle/srs/usr/lib/systemd/system/srs.service"
  local staged_readme="${bundle_root}/share/cvmmap-bundle/srs/README.md"
  local staged_default_conf="${staged_conf_dir}/srs.conf"
  require_file "${srs_trunk_dir}/objs/srs"
  require_dir "${srs_trunk_dir}/conf"

  cp -a "${srs_trunk_dir}/objs/srs" "${bundle_root}/share/cvmmap-bundle/srs/usr/local/srs/objs/srs"
  cp -a "${srs_trunk_dir}/conf/." "${staged_conf_dir}/"
  python3 - "${staged_default_conf}" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
updated, count = re.subn(r'(^\s*max_connections\s+)\d+(;)', r'\g<1>10000\2', text, count=1, flags=re.MULTILINE)
if count != 1:
    raise SystemExit(f"expected max_connections setting in {path}")
path.write_text(updated, encoding="utf-8")
PY
  write_srs_systemd_unit "${staged_service_unit}"
  write_srs_readme "${staged_readme}"
}
stage_generated_scripts() {
  write_runtime_helper "${bundle_root}/bin/install-runtime-deps"
  write_run_cv_mmap "${bundle_root}/bin/run-cv-mmap"
  write_run_cv_mmap_gui "${bundle_root}/bin/run-cv-mmap-gui"
  write_run_streamer "${bundle_root}/bin/run-cvmmap-streamer"
  write_run_nats "${bundle_root}/bin/run-nats"
}

stage_third_party_tools() {
  local nats_tarball="${download_root}/${nats_asset}"
  local pc_tarball="${download_root}/${process_compose_asset}"

  fetch_release_asset \
    "https://github.com/nats-io/nats-server/releases/download/${nats_version}/${nats_asset}" \
    "${nats_tarball}" \
    "${nats_sha256}"
  fetch_release_asset \
    "https://github.com/F1bonacc1/process-compose/releases/download/${process_compose_version}/${process_compose_asset}" \
    "${pc_tarball}" \
    "${process_compose_sha256}"

  extract_single_binary_from_tarball "${nats_tarball}" '(^|/)nats-server$' "${bundle_root}/tools/nats-server"
  extract_single_binary_from_tarball "${pc_tarball}" '(^|/)process-compose$' "${bundle_root}/tools/process-compose"
}

emit_artifacts() {
  local tarball_name="${bundle_name}.tar.gz"
  local tarball_path="${artifact_root}/${tarball_name}"
  local run_path="${artifact_root}/${bundle_name}.run"

  tar -czf "${tarball_path}" -C "${work_root}" "${bundle_name}"
  write_self_extracting_runner "${run_path}" "${tarball_name}" "${run_default_command}"
  log "artifacts written: ${tarball_path} ${run_path}"
}

main() {
  parse_args "$@"

  require_command cmake
  require_command curl
  require_command tar
  require_command sha256sum
  require_command python3
  require_dir "${cv_mmap_repo}"
  require_dir "${cv_mmap_gui_repo}"
  require_dir "${cvmmap_streamer_repo}"
  require_dir "${srs_repo}/trunk"

  rm -rf "${bundle_root}" "${install_root}" "${extract_root}"
  stage_bundle_layout
  build_and_install_repos
  stage_installed_binaries
  stage_runtime_libs "${bundle_root}/lib"
  stage_assets
  stage_srs_assets
  stage_generated_scripts
  stage_third_party_tools
  write_manifest "${bundle_root}/share/cvmmap-bundle/manifest.json"
  emit_artifacts
}

main "$@"
