#!/usr/bin/env bash

set -u -o pipefail

ROOT_DIR="/workspaces/zed-playground/cv-mmap"
PY_REPO="/workspaces/zed-playground/cvmmap-python-client"
GUI_REPO="/workspaces/zed-playground/cv-mmap-gui"

RUN_ID="${1:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
OUT_DIR="${ROOT_DIR}/.sisyphus/evidence/task-11-interop-matrix/${RUN_ID}"
CELL_DIR="${OUT_DIR}/cells"
SUMMARY_TSV="${OUT_DIR}/matrix-summary.tsv"
SUMMARY_JSON="${OUT_DIR}/matrix-summary.json"
SUMMARY_MD="${OUT_DIR}/matrix-summary.md"
TASK11_HAPPY="${ROOT_DIR}/.sisyphus/evidence/task-11-interop-matrix-happy.txt"
TASK11_ERROR="${ROOT_DIR}/.sisyphus/evidence/task-11-interop-matrix-error.txt"

mkdir -p "${CELL_DIR}"

printf 'cell_id\tcomponent\tcommand\tresult\treason\tevidence_path\n' > "${SUMMARY_TSV}"

sanitize_field() {
	local value="$1"
	value="${value//$'\t'/ }"
	value="${value//$'\n'/ }"
	printf '%s' "${value}"
}

classify_cell() {
	local log_path="$1"
	local exit_code="$2"
	python3 - "$log_path" "$exit_code" <<'PY'
import re
import sys
from pathlib import Path

log_path = Path(sys.argv[1])
exit_code = int(sys.argv[2])
lines = []
if log_path.exists():
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()

if exit_code == 0:
    print("PASS\texit code 0")
    raise SystemExit(0)

blocked_patterns = [
    r"command not found",
    r"No such file or directory",
    r"Could NOT find",
    r"Could not find a package configuration file",
    r"Package .* was not found",
    r"pkg-config",
    r"ModuleNotFoundError",
    r"ImportError",
    r"not installed",
    r"cannot find -l",
    r"fatal error: .*: No such file",
    r"CMake Error at .*find_package",
    r"uv: .*not found",
]
blocked_re = re.compile("|".join(f"(?:{p})" for p in blocked_patterns), re.IGNORECASE)

for line in lines:
    if blocked_re.search(line):
        print(f"BLOCKED\t{line.strip()[:260]}")
        raise SystemExit(0)

failure_hint = re.compile(r"(error|failed|assert|exception|out of bounds|invalid)", re.IGNORECASE)
for line in lines:
    if failure_hint.search(line):
        print(f"FAIL\t{line.strip()[:260]}")
        raise SystemExit(0)

print(f"FAIL\tcommand exited with code {exit_code}")
PY
}

append_row() {
	local cell_id="$1"
	local component="$2"
	local command="$3"
	local result="$4"
	local reason="$5"
	local evidence_path="$6"

	printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$(sanitize_field "$cell_id")" \
		"$(sanitize_field "$component")" \
		"$(sanitize_field "$command")" \
		"$(sanitize_field "$result")" \
		"$(sanitize_field "$reason")" \
		"$(sanitize_field "$evidence_path")" >> "${SUMMARY_TSV}"
}

run_simple_cell() {
	local cell_id="$1"
	local component="$2"
	local workdir="$3"
	local command="$4"
	local evidence_path="${CELL_DIR}/${cell_id}.log"
	local exit_code=0

	{
		echo "cell_id=${cell_id}"
		echo "component=${component}"
		echo "workdir=${workdir}"
		echo "command=${command}"
		echo "utc_start=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "${evidence_path}"

	(
		cd "${workdir}"
		bash -lc "${command}"
	) >> "${evidence_path}" 2>&1
	exit_code=$?

	{
		echo "exit_code=${exit_code}"
		echo "utc_end=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} >> "${evidence_path}"

	local status_line
	status_line="$(classify_cell "${evidence_path}" "${exit_code}")"
	local result
	local reason
	IFS=$'\t' read -r result reason <<< "${status_line}"
	append_row "${cell_id}" "${component}" "${command}" "${result}" "${reason}" "${evidence_path}"
}

run_control_cell() {
	local cell_id="$1"
	local component="$2"
	local mode="$3"
	local evidence_path="${CELL_DIR}/${cell_id}.log"
	local producer_log="${CELL_DIR}/${cell_id}.producer.log"
	local exit_code=0
	local command_display

	if [[ "${mode}" == "reset" ]]; then
		command_display="build/cv-mmap -c config.toml (background) + PYTHONPATH=<python-client>/src uv run python reset_frame_count"
	else
		command_display="build/cv-mmap -c config.toml (background) + PYTHONPATH=<python-client>/src uv run python raw unsupported-major control request"
	fi

	{
		echo "cell_id=${cell_id}"
		echo "component=${component}"
		echo "workdir=${ROOT_DIR}"
		echo "command=${command_display}"
		echo "producer_log=${producer_log}"
		echo "utc_start=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "${evidence_path}"

	(
		set -u -o pipefail
		"${ROOT_DIR}/build/cv-mmap" -c "${ROOT_DIR}/config.toml" > "${producer_log}" 2>&1 &
		producer_pid=$!

		cleanup() {
			kill "${producer_pid}" 2>/dev/null || true
			wait "${producer_pid}" 2>/dev/null || true
		}
		trap cleanup EXIT

		ready=0
		for _ in $(seq 1 100); do
			if [[ -S "/tmp/cvmmap_example_control" ]]; then
				ready=1
				break
			fi
			sleep 0.1
		done

		if [[ "${ready}" -ne 1 ]]; then
			echo "control socket did not appear: /tmp/cvmmap_example_control"
			exit 2
		fi

		if [[ "${mode}" == "reset" ]]; then
			cd "${PY_REPO}"
			PYTHONPATH="${PY_REPO}/src" uv run python - <<'PY'
import asyncio

from cvmmap import CvMmapRequestClient
from cvmmap.msg import CONTROL_RESPONSE_OK


async def main() -> None:
    client = CvMmapRequestClient("example")
    try:
        response = await client.reset_frame_count(timeout_ms=5000)
        print(f"response_code={response.response_code}")
        if response.response_code != CONTROL_RESPONSE_OK:
            raise RuntimeError(
                f"Expected CONTROL_RESPONSE_OK ({CONTROL_RESPONSE_OK}), got {response.response_code}"
            )
    finally:
        client.close()


asyncio.run(main())
PY
		else
			cd "${PY_REPO}"
			PYTHONPATH="${PY_REPO}/src" uv run python - <<'PY'
import asyncio
import struct

import zmq
import zmq.asyncio

from cvmmap.msg import (
    CONTROL_MSG_CMD_RESET_FRAME_COUNT,
    CONTROL_MESSAGE_REQUEST_MAGIC,
    CONTROL_RESPONSE_INVALID_VERSION,
    ControlMessageResponse,
)

LABEL_LEN_MAX = 24
VERSION_MINOR = 0
UNSUPPORTED_MAJOR = 9


async def main() -> None:
    ctx = zmq.asyncio.Context.instance()
    sock = ctx.socket(zmq.REQ)
    try:
        sock.connect("ipc:///tmp/cvmmap_example_control")
        label = b"example".ljust(LABEL_LEN_MAX, b"\0")
        request = struct.pack(
            f"=BxBBi{LABEL_LEN_MAX}sHxx",
            CONTROL_MESSAGE_REQUEST_MAGIC,
            UNSUPPORTED_MAJOR,
            VERSION_MINOR,
            CONTROL_MSG_CMD_RESET_FRAME_COUNT,
            label,
            0,
        )
        print(f"request_size={len(request)}")
        await sock.send(request)
        response_bytes = await sock.recv()
        response = ControlMessageResponse.unmarshal(response_bytes)
        print(f"response_code={response.response_code}")
        if response.response_code != CONTROL_RESPONSE_INVALID_VERSION:
            raise RuntimeError(
                f"Expected CONTROL_RESPONSE_INVALID_VERSION ({CONTROL_RESPONSE_INVALID_VERSION}), got {response.response_code}"
            )
    finally:
        sock.close(0)


asyncio.run(main())
PY
		fi
	) >> "${evidence_path}" 2>&1
	exit_code=$?

	{
		echo "exit_code=${exit_code}"
		echo "utc_end=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} >> "${evidence_path}"

	local status_line
	status_line="$(classify_cell "${evidence_path}" "${exit_code}")"
	local result
	local reason
	IFS=$'\t' read -r result reason <<< "${status_line}"
	append_row "${cell_id}" "${component}" "${command_display}" "${result}" "${reason}" "${evidence_path}"
}

run_simple_cell \
	"producer-build-zed-off" \
	"producer-build" \
	"${ROOT_DIR}" \
	"cmake -B build -S . && cmake --build build"

run_simple_cell \
	"producer-build-zed-on" \
	"producer-build" \
	"${ROOT_DIR}" \
	"cmake -B build-zed -S . -DWITH_BACKEND_ZED=ON && cmake --build build-zed"

run_simple_cell \
	"python-parser-full-suite" \
	"python-parser" \
	"${PY_REPO}" \
	"uv run pytest -q"

run_simple_cell \
	"python-v1-valid" \
	"python-parser" \
	"${PY_REPO}" \
	"uv run pytest -q tests/test_import_and_protocol.py::test_v1_parse_pass"

run_simple_cell \
	"python-v2-left-only-valid" \
	"python-parser" \
	"${PY_REPO}" \
	"uv run pytest -q tests/test_import_and_protocol.py::test_v2_left_only_parse_pass"

run_simple_cell \
	"python-v2-left-depth-valid" \
	"python-parser" \
	"${PY_REPO}" \
	"uv run pytest -q tests/test_import_and_protocol.py::test_v2_left_depth_parse_pass"

run_simple_cell \
	"python-v2-malformed" \
	"python-parser" \
	"${PY_REPO}" \
	"uv run pytest -q tests/test_import_and_protocol.py::test_v2_malformed_descriptor_rejected"

run_simple_cell \
	"gui-parser-fixture-checker" \
	"gui-parser" \
	"${GUI_REPO}" \
	"cmake -B build-fixture-check -S app/cvmmap-client/tests && cmake --build build-fixture-check && ./build-fixture-check/protocol_fixture_check"

run_simple_cell \
	"gui-full-build" \
	"gui-build" \
	"${GUI_REPO}" \
	"cmake -B build-matrix-task11 -S . && cmake --build build-matrix-task11"

run_control_cell "control-reset-compatibility" "control-wire" "reset"
run_control_cell "control-unsupported-major-rejection" "control-wire" "unsupported_major"

python3 - "${SUMMARY_TSV}" "${SUMMARY_JSON}" "${SUMMARY_MD}" <<'PY'
import csv
import json
import sys
from pathlib import Path

summary_tsv = Path(sys.argv[1])
summary_json = Path(sys.argv[2])
summary_md = Path(sys.argv[3])

with summary_tsv.open("r", encoding="utf-8") as f:
    reader = csv.DictReader(f, delimiter="\t")
    rows = list(reader)

summary_json.write_text(json.dumps(rows, indent=2), encoding="utf-8")

headers = ["cell_id", "component", "command", "result", "reason", "evidence_path"]
lines = [
    "| " + " | ".join(headers) + " |",
    "| " + " | ".join(["---"] * len(headers)) + " |",
]
for row in rows:
    lines.append("| " + " | ".join(row[h].replace("|", "\\|") for h in headers) + " |")

summary_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

python3 - "${SUMMARY_TSV}" "${TASK11_HAPPY}" "${TASK11_ERROR}" "${RUN_ID}" "${OUT_DIR}" <<'PY'
import csv
import sys
from pathlib import Path

summary_tsv = Path(sys.argv[1])
happy_path = Path(sys.argv[2])
error_path = Path(sys.argv[3])
run_id = sys.argv[4]
out_dir = sys.argv[5]

with summary_tsv.open("r", encoding="utf-8") as f:
    rows = list(csv.DictReader(f, delimiter="\t"))

pass_count = sum(1 for r in rows if r["result"] == "PASS")
fail_rows = [r for r in rows if r["result"] != "PASS"]

happy_lines = [
    f"task-11 interop matrix run_id: {run_id}",
    f"artifact_dir: {out_dir}",
    f"cells_total: {len(rows)}",
    f"cells_pass: {pass_count}",
    f"cells_non_pass: {len(fail_rows)}",
    "",
    "matrix_summary_tsv:",
    str(summary_tsv),
]
happy_path.write_text("\n".join(happy_lines) + "\n", encoding="utf-8")

if not fail_rows:
    error_lines = [
        f"task-11 interop matrix run_id: {run_id}",
        "non-pass cells: none",
    ]
else:
    error_lines = [f"task-11 interop matrix run_id: {run_id}", "non-pass cells:"]
    for row in fail_rows:
        error_lines.append(
            f"- {row['cell_id']} [{row['result']}] reason={row['reason']} evidence={row['evidence_path']}"
        )

error_path.write_text("\n".join(error_lines) + "\n", encoding="utf-8")
PY

printf 'run_id=%s\n' "${RUN_ID}"
printf 'summary_tsv=%s\n' "${SUMMARY_TSV}"
printf 'summary_json=%s\n' "${SUMMARY_JSON}"
printf 'summary_md=%s\n' "${SUMMARY_MD}"
printf 'task11_happy=%s\n' "${TASK11_HAPPY}"
printf 'task11_error=%s\n' "${TASK11_ERROR}"
