#!/usr/bin/env python3
"""Prepare immutable source and AUR artifacts from a committed revision. No publishing."""

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def desktop_client(path):
    # Never source this file or copy unrelated environment variables/tokens into a release.
    expected = {"OMARCHY_CALENDAR_GOOGLE_CLIENT_ID", "OMARCHY_CALENDAR_GOOGLE_CLIENT_SECRET"}
    values = {}
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or key not in expected or key in values:
            raise ValueError("OAuth file must contain only the two desktop client settings")
        if not re.fullmatch(r"[A-Za-z0-9._-]+", value):
            raise ValueError("OAuth client settings must be nonempty, unquoted values")
        values[key] = value
    if values.keys() != expected or not values["OMARCHY_CALENDAR_GOOGLE_CLIENT_ID"].endswith(".apps.googleusercontent.com"):
        raise ValueError("A Google Desktop app client ID and client secret are required")
    return "".join(f"{key}={values[key]}\n" for key in sorted(values)).encode()


def archive(path, root, names, epoch):
    # Stable metadata and gzip header make repeated preparation of a revision reproducible.
    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = epoch
        if info.isdir():
            info.mode = 0o755
        elif info.isfile():
            info.mode = 0o755 if info.mode & 0o111 else 0o644
        return info

    with path.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
            for name in names:
                tar.add(root / name, arcname=name, filter=normalize)


def prepare(repo, ref, oauth, output):
    client = desktop_client(oauth)
    revision = git(repo, "rev-parse", "--verify", f"{ref}^{{commit}}").decode().strip()
    epoch = int(git(repo, "show", "-s", "--format=%ct", revision))
    if output.exists():
        raise ValueError("Output directory already exists; choose a new directory")
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        tree = work / "tree"
        tree.mkdir()
        with tarfile.open(fileobj=io.BytesIO(git(repo, "archive", revision))) as tar:
            tar.extractall(tree, filter="data")
        match = re.search(r'^#define OMARCHY_CALENDAR_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"$',
                          (tree / "app/version.h").read_text(), re.M)
        if not match:
            raise ValueError("Source revision has no supported release version")
        version = match[1]
        if ref.startswith("v") and ref != f"v{version}":
            raise ValueError("Tag does not match the source version")
        template = (tree / "packaging/arch/PKGBUILD.in").read_text()
        if f"pkgver={version}\n" not in template or template.count("@SOURCE_B2SUM@") != 1:
            raise ValueError("PKGBUILD template does not match the source version")
        source_name = f"omarchy-calendar-{version}"
        source = work / source_name
        tree.rename(source)
        config = source / "packaging/google-oauth.env"
        config.write_bytes(client)
        config.chmod(0o644)
        artifacts = work / "artifacts"
        artifacts.mkdir()
        source_archive = artifacts / f"{source_name}.tar.gz"
        archive(source_archive, work, [source_name], epoch)
        checksum = hashlib.blake2b(source_archive.read_bytes()).hexdigest()
        aur = artifacts / "aur"
        aur.mkdir()
        (aur / "PKGBUILD").write_text(template.replace("@SOURCE_B2SUM@", checksum))
        srcinfo = subprocess.check_output(["makepkg", "--printsrcinfo"], cwd=aur)
        (aur / ".SRCINFO").write_bytes(srcinfo)
        archive(artifacts / f"{source_name}-aur.tar.gz", artifacts, ["aur"], epoch)
        # OPR owns its recipe and discovers updates from the upstream release
        # watch. It consumes the same immutable source as AUR, not our binary.
        opr = work / "opr"
        package = opr / "pkgbuilds/omarchy-calendar"
        (package / ".omarchy").mkdir(parents=True)
        shutil.copyfile(aur / "PKGBUILD", package / "PKGBUILD")
        metadata = json.loads((source / "packaging/omarchy/package.json").read_text())
        (package / ".omarchy/package.json").write_text(json.dumps(metadata, indent=2) + "\n")
        archive(artifacts / f"{source_name}-opr.tar.gz", opr, ["pkgbuilds"], epoch)
        shutil.copytree(opr, artifacts / "opr")
        # makepkg uses this exact archive locally before it is published at the source URL.
        shutil.copyfile(source_archive, aur / source_archive.name)
        (artifacts / "SOURCE_REVISION").write_text(revision + "\n")
        with (artifacts / "SHA256SUMS").open("w") as sums:
            for path in sorted(artifacts.iterdir()):
                if path.is_file() and path.name != "SHA256SUMS":
                    sums.write(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(artifacts, output)
    print(f"Prepared {source_name} from {revision} in {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--ref", required=True, help="Committed revision or release tag")
    parser.add_argument("--oauth-env", required=True, type=Path, help="Desktop client configuration (included in public artifacts)")
    parser.add_argument("--output", required=True, type=Path, help="New output directory")
    args = parser.parse_args()
    try:
        prepare(args.repo.resolve(), args.ref, args.oauth_env, args.output.resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release preparation failed: {error}\n")
