#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_NAME="${0##*/}"
readonly RUNTIME_PACKAGES=(
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
Usage:
  install_runtime_deps.sh [--verify]
  install_runtime_deps.sh --install
  install_runtime_deps.sh --help

Verify or install the distro-managed runtime packages required by the
Jetson portable bundle.

Modes:
  --verify   Verify packages are installed (default)
  --install  Install missing packages with: sudo apt-get install
  --help     Show this help text

Notes:
  - Only Debian/Ubuntu-style apt environments are supported.
  - Bundled tools such as nats-server are intentionally excluded.
EOF
}

die() {
  printf '%s: %s\n' "$SCRIPT_NAME" "$*" >&2
  exit 1
}

mode="verify"
case "${1-}" in
  "")
    ;;
  --verify)
    mode="verify"
    shift
    ;;
  --install)
    mode="install"
    shift
    ;;
  --help|-h)
    usage
    exit 0
    ;;
  *)
    die "unknown argument: $1"
    ;;
esac

if (($# > 0)); then
  die "unexpected extra arguments: $*"
fi

if [[ ! -r /etc/os-release ]]; then
  die "cannot determine operating system; /etc/os-release is not readable"
fi

# shellcheck disable=SC1091
source /etc/os-release
os_id="${ID:-}"
os_like="${ID_LIKE:-}"
if [[ "$os_id" != "debian" && "$os_id" != "ubuntu" && "$os_like" != *debian* && "$os_like" != *ubuntu* ]]; then
  die "unsupported distribution '${PRETTY_NAME:-unknown}'; this helper only supports Debian/Ubuntu apt environments"
fi

if ! command -v apt-get >/dev/null 2>&1; then
  die "apt-get is required but was not found in PATH"
fi
if ! command -v dpkg-query >/dev/null 2>&1; then
  die "dpkg-query is required but was not found in PATH"
fi

missing_packages=()
for package in "${RUNTIME_PACKAGES[@]}"; do
  if ! dpkg-query -W -f='${Status}\n' "$package" 2>/dev/null | grep -q '^install ok installed$'; then
    missing_packages+=("$package")
  fi
done

if ((${#missing_packages[@]} == 0)); then
  printf 'All runtime packages are installed.\n'
  exit 0
fi

if [[ "$mode" == "verify" ]]; then
  printf 'Missing runtime packages:\n' >&2
  printf '  %s\n' "${missing_packages[@]}" >&2
  exit 1
fi

printf 'Installing missing runtime packages:\n'
printf '  %s\n' "${missing_packages[@]}"
sudo apt-get update
sudo apt-get install -y --no-install-recommends "${missing_packages[@]}"
