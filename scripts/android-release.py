#!/usr/bin/env python3
"""Build or verify the Android distribution APK on the controlled Linux/WSL host."""
import argparse
from hashlib import sha256
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / "app/VERSION").read_text().strip()
VERSION_CODE = int((ROOT / "app/VERSION_CODE").read_text().strip())
# From the signed public v0.1.1 APK. Update the table digest only after reviewing
# a data or compiler change against the independently approved dictionary.
TABLE_SHA256 = "eb6cd2c28724a992a7864e33e6bf6b123a87f2aa3281a80053287d856786db28"
SIGNER_SHA256 = "a61ee662f83fd693890e1272e4d150d9797915d00f2655a2b4a32ff5c5eee222"


def verify(apk):
    with zipfile.ZipFile(apk) as archive:
        table = "assets/bundled.liutable"
        if archive.namelist().count(table) != 1:
            raise SystemExit("Expected exactly one bundled dictionary in the distribution APK")
        if sha256(archive.read(table)).hexdigest() != TABLE_SHA256:
            raise SystemExit("Packaged dictionary differs from the approved release dictionary")
    sdk = Path(os.environ["ANDROID_HOME"])
    tools = sdk / "build-tools/35.0.0"
    certs = subprocess.check_output([tools / "apksigner", "verify", "--print-certs", apk], text=True)
    signers = re.findall(r"^Signer #\d+ certificate SHA-256 digest: (\w+)$", certs, re.MULTILINE)
    if signers != [SIGNER_SHA256]:
        raise SystemExit("APK signer differs from the installed application")
    manifest = subprocess.check_output(
        [tools / "aapt", "dump", "xmltree", apk, "AndroidManifest.xml"], text=True)
    if re.search(r'A: android:debuggable\([^)]*\)=\(type 0x12\)0x(?:f+|1)\b', manifest):
        raise SystemExit("Refusing a debuggable APK for distribution")
    if ('A: package="com.hamanpaul.liukai"' not in manifest
            or not re.search(r'A: android:versionName\([^)]*\)="' + re.escape(VERSION) + '"', manifest)
            or not re.search(r'A: android:versionCode\([^)]*\)=\(type 0x10\)0x' + f'{VERSION_CODE:x}' + r'\s', manifest)):
        raise SystemExit("APK package or install version differs from the release configuration")
    return sha256(apk.read_bytes()).hexdigest()


def build(output_dir):
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
        raise SystemExit("Build the distribution APK from a clean source checkout")
    if not (Path.home() / ".android/debug.keystore").is_file():
        raise SystemExit("Existing Android distribution signing identity is missing")
    subprocess.run([ROOT / "gradlew", "--no-daemon", ":app:assembleRelease", "-Pliukai.signForDistribution=true"],
                   cwd=ROOT, check=True)
    apk = ROOT / "app/build/outputs/apk/release/app-release.apk"
    digest = verify(apk)
    output_dir.mkdir(parents=True, exist_ok=True)
    deliverable = output_dir / f"liu-kai-android-{VERSION}.apk"
    shutil.copyfile(apk, deliverable)
    (output_dir / "SHA256SUMS.txt").write_text(f"{digest}  {deliverable.name}\n", encoding="utf-8")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    (output_dir / "SOURCE.txt").write_text(
        f"source_commit={commit}\ntag=android-v{VERSION}\npackage_version={VERSION}\n"
        f"version_code={VERSION_CODE}\nsigner_sha256={SIGNER_SHA256}\ndictionary_sha256={TABLE_SHA256}\n",
        encoding="utf-8",
    )
    print(deliverable)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("build", help="build, sign and validate the distributable APK").add_argument("output_dir", type=Path)
    commands.add_parser("verify", help="validate a newly built or downloaded APK").add_argument("apk", type=Path)
    args = parser.parse_args()
    if args.command == "build":
        build(args.output_dir)
    else:
        print(f"{verify(args.apk)}  {args.apk.name}")
