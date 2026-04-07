#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "click>=8.1,<9",
# ]
# ///

from __future__ import annotations

import os
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Literal

import click


BackendKind = Literal["mcap", "zed"]

MCAP_SUFFIX = ".mcap"
SVO_SUFFIX = ".svo2"


@dataclass(frozen=True)
class DiscoveryResult:
    backend: BackendKind
    paths: list[Path]


class PlaylistConfigError(RuntimeError):
    pass


def toml_quote(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def merge_toml(parent: dict[str, Any], child: dict[str, Any]) -> dict[str, Any]:
    merged = dict(parent)
    for key, child_value in child.items():
        parent_value = merged.get(key)
        if isinstance(parent_value, dict) and isinstance(child_value, dict):
            merged[key] = merge_toml(parent_value, child_value)
        else:
            merged[key] = child_value
    return merged


def load_effective_config(path: Path, active_stack: tuple[Path, ...] = ()) -> dict[str, Any]:
    resolved_path = path.resolve()
    if resolved_path in active_stack:
        cycle = " -> ".join(str(item) for item in (*active_stack, resolved_path))
        raise PlaylistConfigError(f"base config extends cycle detected: {cycle}")
    if not resolved_path.exists():
        raise PlaylistConfigError(f"base config does not exist: {resolved_path}")

    try:
        data = tomllib.loads(resolved_path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as exc:
        raise PlaylistConfigError(f"failed to parse TOML from {resolved_path}: {exc}") from exc

    if not isinstance(data, dict):
        raise PlaylistConfigError(f"base config root must be a TOML table: {resolved_path}")

    extends_value = data.get("extends")
    if extends_value is None:
        return data
    if not isinstance(extends_value, str):
        raise PlaylistConfigError(f"extends must be a string in {resolved_path}")

    extends_path = Path(extends_value)
    if not extends_path.is_absolute():
        extends_path = (resolved_path.parent / extends_path).resolve()

    child = dict(data)
    child.pop("extends", None)
    parent = load_effective_config(extends_path, (*active_stack, resolved_path))
    return merge_toml(parent, child)


def validate_base_config(base_config: Path, backend: BackendKind) -> None:
    data = load_effective_config(base_config)

    if backend == "mcap":
        mcap_section = data.get("mcap")
        if isinstance(mcap_section, dict):
            if "path" in mcap_section or "playlist" in mcap_section:
                raise PlaylistConfigError(
                    "base config already defines mcap.path or mcap.playlist; "
                    "use a neutral template or generate a snippet instead"
                )
        return

    zed_section = data.get("zed")
    if isinstance(zed_section, dict):
        conflicting_keys = {
            "svo_path",
            "playlist",
            "index",
            "serial",
            "ip_address",
            "port",
        }
        conflicts = [key for key in conflicting_keys if key in zed_section]
        if conflicts:
            conflict_list = ", ".join(sorted(conflicts))
            raise PlaylistConfigError(
                f"base config already defines conflicting zed keys ({conflict_list}); "
                "use a neutral template or generate a snippet instead"
            )


def discover_files(root: Path, patterns: Iterable[str], recursive: bool) -> DiscoveryResult:
    if not root.exists():
        raise PlaylistConfigError(f"root path does not exist: {root}")
    if not root.is_dir():
        raise PlaylistConfigError(f"root path must be a directory: {root}")

    discovered: set[Path] = set()
    for pattern in patterns:
        iterator = root.rglob(pattern) if recursive else root.glob(pattern)
        for path in iterator:
            if path.is_file():
                discovered.add(path.resolve())

    if not discovered:
        raise PlaylistConfigError(
            f"no files matched under {root.resolve()} for the provided pattern set"
        )

    mcap_paths = sorted(path for path in discovered if path.suffix.lower() == MCAP_SUFFIX)
    svo_paths = sorted(path for path in discovered if path.suffix.lower() == SVO_SUFFIX)
    other_paths = sorted(path for path in discovered if path.suffix.lower() not in {MCAP_SUFFIX, SVO_SUFFIX})

    if other_paths:
        unexpected = ", ".join(str(path) for path in other_paths[:5])
        suffix = "" if len(other_paths) <= 5 else ", ..."
        raise PlaylistConfigError(
            f"matched unsupported file types: {unexpected}{suffix}; "
            "narrow the pattern to only .mcap or .svo2 files"
        )
    if mcap_paths and svo_paths:
        raise PlaylistConfigError(
            "matched both .mcap and .svo2 files; narrow the pattern or run the helper separately per backend"
        )
    if mcap_paths:
        return DiscoveryResult(backend="mcap", paths=mcap_paths)
    if svo_paths:
        return DiscoveryResult(backend="zed", paths=svo_paths)

    raise PlaylistConfigError(
        "no .mcap or .svo2 files matched; narrow the pattern to only supported playlist file types"
    )


def format_path_array(paths: list[Path], indent: str = "") -> list[str]:
    lines = [f"{indent}paths = ["]
    for path in paths:
        lines.append(f"{indent}  {toml_quote(str(path))},")
    lines.append(f"{indent}]")
    return lines


def append_playlist_section(
    lines: list[str],
    section_name: str,
    paths: list[Path],
    sort_by_recording_time: bool,
) -> None:
    lines.extend(["", section_name])
    lines.extend(format_path_array(paths))
    lines.append(
        f"sort_by_recording_time = {'true' if sort_by_recording_time else 'false'}"
    )


def build_snippet(
    discovery: DiscoveryResult,
    *,
    sort_by_recording_time: bool,
    generated_from: Path,
    name_override: str | None,
    base_config: Path | None,
) -> str:
    lines = [
        f"# Generated by tools/generate_playlist_config.py from {generated_from.resolve()}",
    ]
    if base_config is None:
        lines.extend(
            [
                "# This snippet is intended to be pasted into an existing config or used as an overlay.",
                "# Remove conflicting source-selection keys from the destination config before using it.",
            ]
        )
    if name_override:
        lines.extend(["", f"name = {toml_quote(name_override)}"])

    if discovery.backend == "mcap":
        lines.extend(["", "[video]", 'backend = "mcap"'])
        append_playlist_section(
            lines, "[mcap.playlist]", discovery.paths, sort_by_recording_time
        )
    else:
        lines.extend(["", "[video]", 'backend = "zed"', "", "[zed]", 'stream_mode = "svo"'])
        append_playlist_section(
            lines, "[zed.playlist]", discovery.paths, sort_by_recording_time
        )

    return "\n".join(lines) + "\n"


def build_overlay(
    discovery: DiscoveryResult,
    *,
    output: Path,
    base_config: Path,
    sort_by_recording_time: bool,
    name_override: str | None,
) -> str:
    relative_base = os.path.relpath(base_config.resolve(), start=output.parent.resolve())
    lines = [
        f"# Generated by tools/generate_playlist_config.py from base config {base_config.resolve()}",
        f"extends = {toml_quote(relative_base)}",
    ]
    if name_override:
        lines.append(f"name = {toml_quote(name_override)}")

    if discovery.backend == "mcap":
        lines.extend(["", "[video]", 'backend = "mcap"'])
        append_playlist_section(
            lines, "[mcap.playlist]", discovery.paths, sort_by_recording_time
        )
    else:
        lines.extend(["", "[video]", 'backend = "zed"', "", "[zed]", 'stream_mode = "svo"'])
        append_playlist_section(
            lines, "[zed.playlist]", discovery.paths, sort_by_recording_time
        )

    return "\n".join(lines) + "\n"


def ensure_writable_output(output: Path, force: bool) -> None:
    if output.exists() and not force:
        raise PlaylistConfigError(
            f"output file already exists: {output}; pass --force to overwrite"
        )

    output.parent.mkdir(parents=True, exist_ok=True)


def write_output(
    root: Path,
    patterns: tuple[str, ...],
    recursive: bool,
    output: Path,
    base_config: Path | None,
    sort_by_recording_time: bool,
    force: bool,
    name_override: str | None,
) -> DiscoveryResult:
    ensure_writable_output(output, force)
    discovery = discover_files(root.resolve(), patterns, recursive)

    if base_config is not None:
        resolved_base = base_config.resolve()
        if output.resolve() == resolved_base:
            raise PlaylistConfigError(
                "--output must be different from --base-config; in-place edits are not supported"
            )
        validate_base_config(resolved_base, discovery.backend)
        content = build_overlay(
            discovery,
            output=output,
            base_config=resolved_base,
            sort_by_recording_time=sort_by_recording_time,
            name_override=name_override,
        )
    else:
        content = build_snippet(
            discovery,
            sort_by_recording_time=sort_by_recording_time,
            generated_from=root,
            name_override=name_override,
            base_config=None,
        )

    output.write_text(content, encoding="utf-8")
    return discovery


@click.group()
def cli() -> None:
    """Generate MCAP or ZED SVO playlist config files from glob matches."""


def add_common_options(command: click.Command) -> click.Command:
    command = click.argument("patterns", nargs=-1, required=True)(command)
    command = click.argument("root", type=click.Path(exists=True, file_okay=False, path_type=Path))(command)
    command = click.option(
        "--output",
        required=True,
        type=click.Path(path_type=Path),
        help="Path to the generated TOML file.",
    )(command)
    command = click.option(
        "--base-config",
        type=click.Path(exists=True, dir_okay=False, path_type=Path),
        help="Optional base TOML config to extend instead of writing a raw snippet.",
    )(command)
    command = click.option(
        "--sort-by-recording-time",
        is_flag=True,
        default=False,
        help="Emit sort_by_recording_time = true in the generated playlist.",
    )(command)
    command = click.option(
        "--force",
        is_flag=True,
        default=False,
        help="Overwrite the output file if it already exists.",
    )(command)
    command = click.option(
        "--name",
        "name_override",
        type=str,
        help="Optional name override to emit into the generated file.",
    )(command)
    return command


@cli.command()
@add_common_options
def glob(
    root: Path,
    patterns: tuple[str, ...],
    output: Path,
    base_config: Path | None,
    sort_by_recording_time: bool,
    force: bool,
    name_override: str | None,
) -> None:
    """Match files with pathlib.Path.glob from ROOT."""

    try:
        discovery = write_output(
            root=root,
            patterns=patterns,
            recursive=False,
            output=output,
            base_config=base_config,
            sort_by_recording_time=sort_by_recording_time,
            force=force,
            name_override=name_override,
        )
    except PlaylistConfigError as exc:
        raise click.ClickException(str(exc)) from exc

    click.echo(
        f"wrote {discovery.backend} playlist config with {len(discovery.paths)} files to {output.resolve()}"
    )


@cli.command()
@add_common_options
def rglob(
    root: Path,
    patterns: tuple[str, ...],
    output: Path,
    base_config: Path | None,
    sort_by_recording_time: bool,
    force: bool,
    name_override: str | None,
) -> None:
    """Match files with pathlib.Path.rglob from ROOT."""

    try:
        discovery = write_output(
            root=root,
            patterns=patterns,
            recursive=True,
            output=output,
            base_config=base_config,
            sort_by_recording_time=sort_by_recording_time,
            force=force,
            name_override=name_override,
        )
    except PlaylistConfigError as exc:
        raise click.ClickException(str(exc)) from exc

    click.echo(
        f"wrote {discovery.backend} playlist config with {len(discovery.paths)} files to {output.resolve()}"
    )


if __name__ == "__main__":
    try:
        cli()
    except BrokenPipeError:
        sys.stderr.close()
