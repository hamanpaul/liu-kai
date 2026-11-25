import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("publish", Path(__file__).resolve().parents[1] / "publish-platform-release.py")
publish = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish)


class PublishTests(unittest.TestCase):
    def test_first_release_requires_both_platforms(self):
        with self.assertRaisesRegex(SystemExit, "both initial"):
            publish.check_acceptance([("android-v0.1.0", {}, "commit")], {})

    def test_hash_from_different_installation_test_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "current.apk"
            binary.write_bytes(b"candidate B")
            with self.assertRaisesRegex(SystemExit, "installation tests"):
                publish.check_acceptance([("android-v0.1.1", {binary.name: binary}, "commit")],
                                         {"android": "a" * 64})

    def test_download_mismatch_blocks_release(self):
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "installer"
            local.write_bytes(b"tested")

            def download(*args):
                (Path(args[-1]) / "installer").write_bytes(b"other build")

            with patch.object(publish, "run", download), self.assertRaisesRegex(SystemExit, "differ"):
                publish.verify("windows-v0.1.0", {"installer": local})

    def test_retry_fills_only_missing_draft_assets(self):
        assets = {"setup.exe": Path("setup.exe"), "SOURCE.txt": Path("SOURCE.txt")}
        draft = {"isDraft": True, "assets": [{"name": "setup.exe"}]}
        with patch.object(publish, "release", return_value=draft), patch.object(publish, "verify") as verify:
            with patch.object(publish, "run") as run:
                publish.stage("windows-v0.1.0", assets, "test-commit")
        run.assert_called_once_with("gh", "release", "upload", "windows-v0.1.0", "SOURCE.txt", "-R", publish.REPO)
        verify.assert_called_once_with("windows-v0.1.0", assets)

    def test_second_platform_failure_keeps_first_unpublished(self):
        def prepare(platform, directory):
            return platform, {}, "same-commit"

        argv = ["publish", "--android", ".", "--windows", ".", "--publish"]
        with patch.object(sys, "argv", argv), patch.object(publish, "prepare", prepare), patch.object(publish, "check_acceptance"):
            with patch.object(publish, "stage", side_effect=[None, SystemExit("bad Windows installer")]):
                with patch.object(publish, "run") as run, self.assertRaisesRegex(SystemExit, "bad Windows"):
                    publish.main()
                run.assert_not_called()

    def test_recover_after_publish_timeout_without_republishing(self):
        def prepare(platform, directory):
            return platform, {}, "same-commit"

        already_published = {"isDraft": False, "url": "existing release"}
        with patch.object(sys, "argv", ["publish", "--android", ".", "--publish"]):
            with patch.object(publish, "prepare", prepare), patch.object(publish, "stage"), patch.object(publish, "check_acceptance"):
                with patch.object(publish, "release", return_value=already_published), patch.object(publish, "run") as run:
                    with patch.object(publish, "verify") as verify, contextlib.redirect_stdout(io.StringIO()):
                        publish.main()
                    run.assert_not_called()
                    verify.assert_called_once_with("android", {})
