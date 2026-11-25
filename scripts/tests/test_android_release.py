import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("android_release", Path(__file__).resolve().parents[1] / "android-release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class AndroidReleaseTests(unittest.TestCase):
    def test_preferred_tools_and_numeric_stable_fallback(self):
        with tempfile.TemporaryDirectory() as sdk, patch.dict(os.environ, {"ANDROID_SDK_ROOT": sdk}, clear=True):
            base = Path(sdk) / "build-tools"
            for version in ("35.0.0", "39.0.11", "39.0.9", "40.0.0", "41.0.0-rc1"):
                directory = base / version
                directory.mkdir(parents=True)
                for tool in ("aapt", "apksigner"):
                    (directory / tool).touch(mode=0o755)
            (base / "40.0.0/apksigner").unlink()
            self.assertEqual(release.build_tools(), base / "35.0.0")
            (base / "35.0.0/aapt").unlink()
            self.assertEqual(release.build_tools(), base / "39.0.11")
            with patch.dict(os.environ, {"ANDROID_HOME": str(base / "missing")}):
                with self.assertRaisesRegex(SystemExit, "build-tools"):
                    release.build_tools()

    def test_single_signer_accepts_only_exact_sha256_forms(self):
        field = "Signer #1 certificate SHA-256 digest: "
        digest = release.SIGNER_SHA256
        grouped = bytes.fromhex(digest).hex(":").upper()
        for valid in (digest, digest.upper(), grouped):
            with self.subTest(valid=valid):
                release.check_signer("Signer #1 certificate DN: CN=test\n" + field + valid)
        for invalid in ("", field + digest[:-1], field + "0" * 64, field + grouped.replace(":", "", 1),
                        field + digest + "\n" + field + digest, field + digest + "\nSigner #x certificate DN: CN=test",
                        field + digest + "\nSigner #2 certificate SHA-256 digest: MALFORMED"):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(SystemExit, "signer"):
                release.check_signer(invalid)
