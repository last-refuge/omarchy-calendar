#!/usr/bin/env python3
"""Isolated command doubles. Never invoke host package or session commands."""
import json
import os
from pathlib import Path
import shutil
import sys
import time

name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ["INSTALLER_FIXTURE"])
config = json.loads((root / "config.json").read_text())
with (root / "calls.jsonl").open("a") as log:
    log.write(json.dumps([name, *args]) + "\n")

if name == "cat":
    if args == ["/etc/os-release"]:
        print("ID=" + config.get("os", "arch"))
    else:
        for arg in args:
            sys.stdout.buffer.write(Path(arg).read_bytes())
elif name == "uname":
    print(config.get("arch", "x86_64") if args == ["-m"] else "Linux")
elif name == "id":
    print(config.get("uid", "1000"))
elif name == "omarchy":
    pass
elif name == "sudo":
    os.execvp(args[0], args)
elif name == "pacman":
    if args[0] == "-Q":
        installed = root / "installed"
        if not installed.exists():
            sys.exit(1)
        print("omarchy-calendar " + installed.read_text())
    elif args[0] == "-U":
        if config.get("pacman_failure"):
            print("error: package transaction failed", file=sys.stderr)
            sys.exit(1)
        asset = Path(args[-1]).name
        version = asset.removeprefix("omarchy-calendar-").rsplit("-", 1)[0]
        (root / "installed").write_text(config.get("installed_after", version))
    elif args[0] != "-S":
        raise RuntimeError("Unexpected pacman command: " + str(args))
elif name == "vercmp":
    def version(value):
        return tuple(int(part) for part in value.replace("-", ".").split("."))
    left, right = map(version, args)
    print((left > right) - (left < right))
elif name == "curl":
    url = args[-1]
    if config.get("download_failure") and url.endswith(config["download_failure"]):
        print("curl: simulated network error", file=sys.stderr)
        sys.exit(22)
    if "/releases/latest" in url and "api.github.com" in url or "/releases/tags/" in url:
        sys.stdout.write((root / "release.json").read_text())
    else:
        if not url.startswith("https://github.com/last-refuge/omarchy-calendar/releases/download/" + config.get("tag", "v1.0.0") + "/"):
            raise RuntimeError("Unpinned download: " + url)
        if config.get("slow_download"):
            (root / "download.pid").write_text(str(os.getpid()))
            time.sleep(10)
        output = Path(args[args.index("--output") + 1])
        shutil.copyfile(root / "assets" / url.rsplit("/", 1)[1], output)
elif name == "makepkg":
    if config.get("build_failure"):
        sys.exit(1)
    # The installer must stage the checksum-verified source next to the recipe.
    assert Path("PKGBUILD").is_file() and Path(".SRCINFO").is_file()
    assert Path("omarchy-calendar-1.0.0.tar.gz").read_bytes() == b"published source with desktop OAuth"
    Path("omarchy-calendar-1.0.0-1-" + config.get("arch", "x86_64") + ".pkg.tar.zst").write_bytes(b"built package")
    Path("omarchy-calendar-debug-1.0.0-1-" + config.get("arch", "x86_64") + ".pkg.tar.zst").write_bytes(b"debug package")
elif name == "systemctl":
    if "show-environment" in args and config.get("no_session"):
        sys.exit(1)
    if config.get("service_failure") in args:
        print("simulated service failure", file=sys.stderr)
        sys.exit(1)
elif name == "busctl":
    if config.get("no_keyring"):
        sys.exit(1)
    print("org.freedesktop.secrets 123 keyring user :1.2 user@1000.service - -")
else:
    raise RuntimeError("Unexpected stub: " + name)
