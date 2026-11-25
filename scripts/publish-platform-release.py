"""Stage, download and verify platform installers before publishing their GitHub Releases."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REPO = "hamanpaul/liu-kai"


def run(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True, encoding="utf-8").strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(platform, directory):
    version = (ROOT / ("app" if platform == "android" else "windows") / "VERSION").read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit(f"Unexpected {platform} version: {version}")
    tag = f"{platform}-v{version}"
    name = (f"liu-kai-android-{version}.apk" if platform == "android"
            else f"liu-kai-windows-{version}-x64-setup.exe")
    files = {n: directory / n for n in (name, "SOURCE.txt", "SHA256SUMS.txt")}
    record = dict(line.split("=", 1) for line in files["SOURCE.txt"].read_text(encoding="utf-8-sig").splitlines())
    commit = run("git", "rev-parse", "HEAD")
    remote_ref = json.loads(run("gh","api",f"repos/{REPO}/git/ref/tags/{tag}"))["object"]
    if remote_ref["type"] != "commit" or remote_ref["sha"] != commit:
        raise SystemExit(f"Remote lightweight tag {tag} must point to this checkout")
    if (record["source_commit"], record["tag"], record["package_version"]) != (commit, tag, version):
        raise SystemExit(f"Wrong source or version in {platform} build")
    checksum = files["SHA256SUMS.txt"].read_text(encoding="utf-8-sig").strip().split()
    if checksum != [digest(files[name]), name]:
        raise SystemExit(f"Wrong installer or checksum in {directory}")
    return tag, files, commit


def release(tag):
    result = subprocess.run(["gh","release","view",tag,"-R",REPO,"--json","isDraft,assets,url"],
                            capture_output=True, text=True, encoding="utf-8")
    if result.returncode == 0:
        return json.loads(result.stdout)
    if "release not found" in result.stderr.lower():
        return None
    raise SystemExit(result.stderr)


def stage(tag, files, commit):
    info = release(tag)
    if info is None:
        platform, version = tag.split("-v")
        instructions = ("Android 11 以上。下載 APK 安裝或直接覆蓋舊版。"
                        if platform == "android" else "Windows x64。下載 EXE 安裝或升級，再次開啟同一支 EXE 可解除安裝。")
        title = f"Liu-Kai {platform.capitalize()} {version}"
        notes = f"{instructions}\n\n來源提交 `{commit}`。SHA256SUMS.txt 可用於核對安裝檔。"
        run("gh","release","create",tag,"-R",REPO,"--draft","--verify-tag","--title",title,"--notes",notes)
        info = release(tag)
    present = {asset["name"] for asset in info["assets"]}
    missing = files.keys() - present
    if present - files.keys() or (missing and not info["isDraft"]):
        raise SystemExit(f"Unexpected assets in {tag}; inspect the release")
    for name in sorted(missing):
        run("gh","release","upload",tag,str(files[name]),"-R",REPO)
    verify(tag, files)


def verify(tag, files):
    with tempfile.TemporaryDirectory() as tmp:
        run("gh","release","download",tag,"-R",REPO,"--dir",tmp)
        downloaded = {p.name: digest(p) for p in Path(tmp).iterdir()}
        if downloaded != {name: digest(path) for name, path in files.items()}:
            raise SystemExit(f"Downloaded {tag} assets differ from the verified build; inspect before retrying")


def check_acceptance(packages, accepted):
    tags = {tag for tag, _, _ in packages}
    first = {"android-v0.1.0", "windows-v0.1.0"}
    if tags & first and not first.issubset(tags):
        raise SystemExit("Publish both initial platform releases together")
    for tag, files, _ in packages:
        platform = tag.split("-v")[0]
        installer, = (path for path in files.values() if path.suffix in (".apk", ".exe"))
        if not accepted[platform] or digest(installer) != accepted[platform].lower():
            raise SystemExit(f"{tag} does not match the SHA-256 recorded during installation tests")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android", type=Path, help="directory containing the tested APK and receipts")
    parser.add_argument("--windows", type=Path, help="directory containing the tested setup and receipts")
    parser.add_argument("--publish", action="store_true", help="publish after all supplied releases verify")
    parser.add_argument("--accepted-android-sha256", help="APK hash recorded during installation acceptance")
    parser.add_argument("--accepted-windows-sha256", help="setup hash recorded during installation acceptance")
    args = parser.parse_args()
    if not (args.android or args.windows):
        parser.error("supply at least one platform")
    packages = [prepare(name, directory.resolve()) for name, directory in
                (("android", args.android), ("windows", args.windows)) if directory]
    if args.publish:
        check_acceptance(packages, {"android": args.accepted_android_sha256, "windows": args.accepted_windows_sha256})
    for tag, files, commit in packages:
        stage(tag, files, commit)
    if args.publish:
        for tag, files, _ in packages:
            if release(tag)["isDraft"]:
                run("gh","release","edit",tag,"-R",REPO,"--draft=false")
        for tag, files, _ in packages:
            info = release(tag)
            if info["isDraft"]:
                raise SystemExit(f"{tag} is still a draft")
            verify(tag, files)
            print(info["url"])
    else:
        print("Verified GitHub assets:", ", ".join(tag for tag, _, _ in packages))


if __name__ == "__main__":
    main()
