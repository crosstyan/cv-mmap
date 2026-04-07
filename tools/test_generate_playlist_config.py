from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

from click.testing import CliRunner


SCRIPT_PATH = Path(__file__).with_name("generate_playlist_config.py")
SPEC = importlib.util.spec_from_file_location("generate_playlist_config", SCRIPT_PATH)
assert SPEC is not None
assert SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class GeneratePlaylistConfigTests(unittest.TestCase):
    def setUp(self) -> None:
        self.runner = CliRunner()
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_glob_writes_mcap_snippet(self) -> None:
        (self.root / "a.mcap").write_bytes(b"")
        (self.root / "b.mcap").write_bytes(b"")
        output = self.root / "playlist.toml"

        result = self.runner.invoke(
            MODULE.cli,
            ["glob", str(self.root), "*.mcap", "--output", str(output)],
        )

        self.assertEqual(result.exit_code, 0, result.output)
        content = output.read_text(encoding="utf-8")
        self.assertIn('[video]\nbackend = "mcap"', content)
        self.assertIn("[mcap.playlist]", content)
        self.assertIn('sort_by_recording_time = false', content)
        self.assertIn(str((self.root / "a.mcap").resolve()), content)
        self.assertIn(str((self.root / "b.mcap").resolve()), content)

    def test_rglob_finds_nested_svo_files(self) -> None:
        nested = self.root / "nested" / "camera"
        nested.mkdir(parents=True)
        (nested / "part1.svo2").write_bytes(b"")
        output = self.root / "zed.toml"

        result = self.runner.invoke(
            MODULE.cli,
            ["rglob", str(self.root), "*.svo2", "--output", str(output)],
        )

        self.assertEqual(result.exit_code, 0, result.output)
        content = output.read_text(encoding="utf-8")
        self.assertIn('[video]\nbackend = "zed"', content)
        self.assertIn('[zed]\nstream_mode = "svo"', content)
        self.assertIn("[zed.playlist]", content)
        self.assertIn(str((nested / "part1.svo2").resolve()), content)

    def test_mixed_backends_fail(self) -> None:
        (self.root / "a.mcap").write_bytes(b"")
        (self.root / "b.svo2").write_bytes(b"")
        output = self.root / "mixed.toml"

        result = self.runner.invoke(
            MODULE.cli,
            ["glob", str(self.root), "*", "--output", str(output)],
        )

        self.assertNotEqual(result.exit_code, 0)
        self.assertIn("matched both .mcap and .svo2 files", result.output)

    def test_no_matches_fail(self) -> None:
        output = self.root / "empty.toml"

        result = self.runner.invoke(
            MODULE.cli,
            ["rglob", str(self.root), "*.mcap", "--output", str(output)],
        )

        self.assertNotEqual(result.exit_code, 0)
        self.assertIn("no files matched", result.output)

    def test_sort_flag_is_emitted(self) -> None:
        (self.root / "a.svo2").write_bytes(b"")
        output = self.root / "sorted.toml"

        result = self.runner.invoke(
            MODULE.cli,
            [
                "glob",
                str(self.root),
                "*.svo2",
                "--output",
                str(output),
                "--sort-by-recording-time",
            ],
        )

        self.assertEqual(result.exit_code, 0, result.output)
        self.assertIn("sort_by_recording_time = true", output.read_text(encoding="utf-8"))

    def test_overlay_writes_relative_extends_path(self) -> None:
        base = self.root / "configs" / "base.toml"
        base.parent.mkdir(parents=True)
        base.write_text(
            '\n'.join(
                [
                    'name = "base"',
                    "",
                    "[video]",
                    'backend = "zed"',
                    "",
                    "[zed]",
                    'resolution = "AUTO"',
                    "fps = 30",
                    'depth_mode = "NEURAL"',
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        (self.root / "capture.svo2").write_bytes(b"")
        output = self.root / "generated" / "overlay.toml"

        result = self.runner.invoke(
            MODULE.cli,
            [
                "glob",
                str(self.root),
                "*.svo2",
                "--output",
                str(output),
                "--base-config",
                str(base),
                "--name",
                "playlist-zed",
            ],
        )

        self.assertEqual(result.exit_code, 0, result.output)
        content = output.read_text(encoding="utf-8")
        self.assertIn('extends = "../configs/base.toml"', content)
        self.assertIn('name = "playlist-zed"', content)

    def test_overlay_rejects_conflicting_mcap_base(self) -> None:
        base = self.root / "base.toml"
        base.write_text(
            '\n'.join(
                [
                    'name = "base"',
                    "",
                    "[video]",
                    'backend = "mcap"',
                    "",
                    "[mcap]",
                    'path = "/tmp/already-set.mcap"',
                    'video_topic = "/camera/video"',
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        (self.root / "capture.mcap").write_bytes(b"")
        output = self.root / "overlay.toml"

        result = self.runner.invoke(
            MODULE.cli,
            [
                "glob",
                str(self.root),
                "*.mcap",
                "--output",
                str(output),
                "--base-config",
                str(base),
            ],
        )

        self.assertNotEqual(result.exit_code, 0)
        self.assertIn("base config already defines mcap.path or mcap.playlist", result.output)

    def test_overlay_rejects_inherited_zed_conflict(self) -> None:
        parent = self.root / "parent.toml"
        parent.write_text(
            '\n'.join(
                [
                    'name = "parent"',
                    "",
                    "[zed]",
                    "index = 0",
                    'resolution = "AUTO"',
                    "fps = 30",
                    'depth_mode = "NEURAL"',
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        child = self.root / "child.toml"
        child.write_text(
            f'extends = "{parent.name}"\nname = "child"\n',
            encoding="utf-8",
        )
        (self.root / "capture.svo2").write_bytes(b"")
        output = self.root / "overlay.toml"

        result = self.runner.invoke(
            MODULE.cli,
            [
                "glob",
                str(self.root),
                "*.svo2",
                "--output",
                str(output),
                "--base-config",
                str(child),
            ],
        )

        self.assertNotEqual(result.exit_code, 0)
        self.assertIn("conflicting zed keys (index)", result.output)


if __name__ == "__main__":
    unittest.main()
