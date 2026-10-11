#!/usr/bin/env python3
"""Exercise the actual installer with local release fixtures and command doubles."""
import hashlib
import io
import json
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import tarfile
import tempfile
import time
import unittest

PROJECT = Path(__file__).resolve().parents[2]
INSTALLER = PROJECT / "install.sh"
STUB = Path(__file__).with_name("stub.py")


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="calendar-installer-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("cat", "uname", "id", "omarchy", "sudo", "pacman", "vercmp", "curl", "makepkg", "systemctl", "busctl"):
            (self.bin / name).symlink_to(STUB)
        self.env = dict(os.environ, PATH=str(self.bin) + ":" + os.environ["PATH"],
                        INSTALLER_FIXTURE=str(self.root),
                        XDG_STATE_HOME=str(self.root / "state"), TMPDIR=str(self.root / "tmp"),
                        TERM="xterm-256color", LANG="C.UTF-8", LC_ALL="C.UTF-8", COLORTERM="truecolor")
        self.env.pop("NO_COLOR", None)
        (self.root / "tmp").mkdir()
        self.config = {}
        self.fixtures()

    def fixtures(self, *, binary=True, corrupt=False, missing_checksum=False, duplicate_checksum=False,
                 extra_packages=False, invalid_url=False):
        self.save_config()
        assets = self.root / "assets"
        assets.mkdir(exist_ok=True)
        files = {
            "omarchy-calendar-debug-1.0.0-1-x86_64.pkg.tar.zst": b"debug",
            "omarchy-calendar-1.0.0-aur.tar.gz": self.recipe(),
            "omarchy-calendar-1.0.0.tar.gz": b"published source with desktop OAuth",
        }
        if binary:
            files["omarchy-calendar-1.0.0-1-x86_64.pkg.tar.zst"] = b"application package"
        if extra_packages:
            files["omarchy-calendar-1.0.0-2-x86_64.pkg.tar.zst"] = b"ambiguous"
        sums = []
        for name, content in files.items():
            (assets / name).write_bytes(content)
            if not (missing_checksum and name == "omarchy-calendar-1.0.0-1-x86_64.pkg.tar.zst"):
                sums.append(hashlib.sha256(content).hexdigest() + "  ./" + name)
        if duplicate_checksum:
            sums.append(sums[-1])
        if corrupt:
            (assets / "omarchy-calendar-1.0.0-1-x86_64.pkg.tar.zst").write_bytes(b"corrupt")
        (assets / "SHA256SUMS").write_text("\n".join(sums) + "\n")
        url = "https://github.com/last-refuge/omarchy-calendar/releases/download/" + self.config.get("tag", "v1.0.0")
        release = dict(tag_name=self.config.get("tag", "v1.0.0"), draft=False, prerelease=False,
                       assets=[dict(name=name, browser_download_url=url + "/" + name) for name in [*files, "SHA256SUMS"]])
        if invalid_url:
            release["assets"][-1]["browser_download_url"] = "https://example.invalid/SHA256SUMS"
        (self.root / "release.json").write_text(json.dumps(release))

    @staticmethod
    def recipe():
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name in ("aur/PKGBUILD", "aur/.SRCINFO"):
                data = b"fixture release recipe\n"
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        return stream.getvalue()

    def save_config(self):
        (self.root / "config.json").write_text(json.dumps(self.config))

    def run_installer(self, *args, piped=False):
        self.save_config()
        command = ["bash", "-s", "--", *args] if piped else ["bash", str(INSTALLER), *args]
        script_input = INSTALLER.read_text() if piped else ""
        # setsid removes the controlling terminal, regardless of how tests were launched.
        return subprocess.run(command, input=script_input, text=True, env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              start_new_session=True, timeout=30)

    def calls(self, name=None):
        log = self.root / "calls.jsonl"
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return [call for call in calls if name is None or call[0] == name]

    def assert_no_mutations(self):
        self.assertFalse(self.calls("sudo"))
        self.assertFalse(self.calls("makepkg"))
        self.assertFalse(self.calls("systemctl"))
        self.assertFalse((self.root / "state").exists())
        self.assertFalse(list((self.root / "tmp").iterdir()))

    def test_prebuilt_piped_install(self):
        result = self.run_installer("--yes", piped=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Ready to plan your day", result.stdout)
        self.assertIn("Method   Prebuilt package", result.stdout)
        self.assertFalse(self.calls("makepkg"))
        installed = [c for c in self.calls("pacman") if c[1] == "-U"]
        self.assertEqual(len(installed), 1)
        self.assertIn("omarchy-calendar-1.0.0-1-x86_64", installed[0][-1])
        self.assertNotIn("debug", installed[0][-1])
        self.assertNotIn("\x1b", result.stdout)
        self.assertFalse(list((self.root / "tmp").iterdir()))
        self.assertEqual(len(list((self.root / "state").rglob("*.log"))), 1)

    def test_dry_run_writes_nothing(self):
        result = self.run_installer("--dry-run")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Would install", result.stdout)
        self.assert_no_mutations()
        self.assertTrue(all("--output" not in c for c in self.calls("curl")))

    def test_source_dry_run(self):
        result = self.run_installer("--dry-run", "--source", "--no-start")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("makepkg", result.stdout)
        self.assert_no_mutations()

    def test_requires_authorization_without_terminal(self):
        result = self.run_installer(piped=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--yes", result.stdout)
        self.assert_no_mutations()

    def test_source_fallback_on_arm(self):
        self.config["arch"] = "aarch64"
        result = self.run_installer("--yes")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Build from release source", result.stdout)
        self.assertEqual(len(self.calls("makepkg")), 1)
        installed = [c for c in self.calls("pacman") if c[1] == "-U"]
        self.assertTrue(installed[0][-1].endswith("aarch64.pkg.tar.zst"))

    def test_explicit_source(self):
        result = self.run_installer("--yes", "--source")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(self.calls("makepkg"))

    def test_absent_binary_uses_source(self):
        self.fixtures(binary=False)
        result = self.run_installer("--yes")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(self.calls("makepkg"))

    def test_failed_download_does_not_build(self):
        self.config["download_failure"] = "omarchy-calendar-1.0.0-1-x86_64.pkg.tar.zst"
        result = self.run_installer("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls("makepkg"))
        self.assertFalse(self.calls("sudo"))
        self.assertIn("Full log:", result.stdout)
        logs = list((self.root / "state").rglob("*.log"))
        self.assertEqual(len(logs), 1)
        self.assertIn("simulated network error", logs[0].read_text())
        self.assertFalse(list((self.root / "tmp").iterdir()))

    def test_checksum_failures(self):
        for settings in ({"corrupt": True}, {"missing_checksum": True}, {"duplicate_checksum": True}):
            with self.subTest(settings=settings):
                self.fixtures(**settings)
                result = self.run_installer("--yes")
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse(self.calls("sudo"))
                self.assertFalse(self.calls("makepkg"))

    def test_ambiguous_assets(self):
        self.fixtures(extra_packages=True)
        result = self.run_installer("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("multiple matching packages", result.stdout)
        self.assert_no_mutations()

    def test_invalid_asset_url(self):
        self.fixtures(invalid_url=True)
        result = self.run_installer("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assert_no_mutations()

    def test_pinned_version(self):
        result = self.run_installer("--yes", "--version", "v1.0.0")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("/tags/v1.0.0", self.calls("curl")[0][-1])

    def test_repeat_skips_package_transaction(self):
        (self.root / "installed").write_text("1.0.0-1")
        result = self.run_installer("--yes")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("already installed", result.stdout)
        self.assertFalse(self.calls("sudo"))
        self.assertTrue(self.calls("systemctl"))
        self.assertFalse(any("restart" in c for c in self.calls("systemctl")))

    def test_upgrade_restarts_service(self):
        (self.root / "installed").write_text("0.9.0-1")
        self.env["HOME"] = str(self.root / "home")
        data = self.root / "home/.local/share/omarchy-calendar/calendar.db"
        settings = self.root / "home/.config/omarchy-calendar/settings.ini"
        for path in (data, settings):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("existing user data")
        result = self.run_installer("--yes")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(any("restart" in c for c in self.calls("systemctl")))
        for path in (data, settings):
            self.assertEqual(path.read_text(), "existing user data")

    def test_wrong_installed_version(self):
        self.config["installed_after"] = "0.9.0-1"
        result = self.run_installer("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("selected version was not installed", result.stdout)
        self.assertFalse(self.calls("systemctl"))

    def test_source_checksums_before_build_dependencies(self):
        for name in ("omarchy-calendar-1.0.0-aur.tar.gz", "omarchy-calendar-1.0.0.tar.gz"):
            self.fixtures()
            (self.root / "assets" / name).write_bytes(b"corrupt")
            result = self.run_installer("--yes", "--source")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Checksum failed", result.stdout)
            self.assertFalse(self.calls("sudo"))

    def test_package_transaction_failure(self):
        self.config["pacman_failure"] = True
        result = self.run_installer("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("package transaction failed", result.stdout)
        self.assertFalse(self.calls("systemctl"))

    def test_downgrade_requires_version(self):
        (self.root / "installed").write_text("2.0.0-1")
        result = self.run_installer("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assert_no_mutations()
        result = self.run_installer("--yes", "--version=1.0.0")
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_no_start(self):
        result = self.run_installer("--yes", "--no-start")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.calls("systemctl"))
        self.assertNotIn("Ready to plan your day", result.stdout)
        self.assertIn("systemctl --user enable --now", result.stdout)

    def test_no_session(self):
        self.config["no_session"] = True
        result = self.run_installer("--yes")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no systemd user session", result.stdout)
        self.assertNotIn("Ready to plan your day", result.stdout)

    def test_service_failure(self):
        self.config["service_failure"] = "is-active"
        result = self.run_installer("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("service is not active", result.stdout)
        self.assertIn("journalctl", result.stdout)
        self.assertNotIn("Ready to plan your day", result.stdout)

    def test_missing_keyring(self):
        self.config["no_keyring"] = True
        result = self.run_installer("--yes")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Secret Service provider", result.stdout)

    def test_ascii_output(self):
        self.env["LC_ALL"] = "C"
        result = self.run_installer("--yes")
        self.assertEqual(result.returncode, 0, result.stdout)
        result.stdout.encode("ascii")
        self.assertIn("[ok]", result.stdout)

    def test_invalid_arguments(self):
        for args in (("--version",), ("--version=",), ("--version", "../main"), ("--unknown",)):
            result = self.run_installer(*args)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(self.calls("curl"))

    def test_platform_rejection(self):
        for key, value in (("os", "ubuntu"), ("arch", "riscv64"), ("uid", "0")):
            self.config = {key: value}
            result = self.run_installer("--yes")
            self.assertNotEqual(result.returncode, 0)
            self.assert_no_mutations()

    def terminal_run(self, args, *, reply=None, interrupt=False):
        self.save_config()
        pid, fd = pty.fork()
        if pid == 0:
            os.execve("/usr/bin/bash", ["bash", str(INSTALLER), *args], self.env)
        output = b""
        deadline = time.monotonic() + 30
        try:
            while time.monotonic() < deadline:
                readable, _, _ = select.select([fd], [], [], 0.1)
                if readable:
                    try:
                        chunk = os.read(fd, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output += chunk
                if reply is not None and b"[y/N]" in output:
                    os.write(fd, reply)
                    reply = None
                if interrupt and (b"\x1b[?25l" in output or (self.root / "download.pid").exists()):
                    os.kill(pid, signal.SIGTERM)
                    interrupt = False
            else:
                os.kill(pid, signal.SIGKILL)
                self.fail("PTY installer timed out: " + output.decode())
            _, status = os.waitpid(pid, 0)
        finally:
            os.close(fd)
        return os.waitstatus_to_exitcode(status), output.decode()

    def test_cancel_in_terminal(self):
        code, output = self.terminal_run([], reply=b"n\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Cancelled", output)
        self.assert_no_mutations()

    def test_terminal_no_color_and_dumb(self):
        for variable in ("NO_COLOR", "TERM"):
            self.env.pop("NO_COLOR", None)
            self.env[variable] = "" if variable == "NO_COLOR" else "dumb"
            code, output = self.terminal_run(["--yes"])
            self.assertEqual(code, 0, output)
            self.assertNotIn("\x1b", output)

    def test_terminal_spinner_and_cursor_restoration(self):
        self.config["slow_download"] = True
        self.env["COLUMNS"] = "32"
        code, output = self.terminal_run(["--yes"], interrupt=True)
        self.assertNotEqual(code, 0)
        self.assertIn("\x1b[?25l", output)
        self.assertIn("\x1b[?25h", output)
        self.assertIn("Interrupted", output)
        self.assertFalse(list((self.root / "tmp").iterdir()))
        self.assertFalse(self.calls("sudo"))
        pid_file = self.root / "download.pid"
        if pid_file.exists():
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pid_file.read_text()), 0)

    def test_verbose(self):
        result = self.run_installer("--yes", "--verbose")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("\x1b", result.stdout)

    def test_verbose_cancellation(self):
        self.config["slow_download"] = True
        code, output = self.terminal_run(["--yes", "--verbose"], interrupt=True)
        self.assertNotEqual(code, 0)
        self.assertIn("Interrupted", output)
        self.assertFalse(list((self.root / "tmp").iterdir()))
        self.assertFalse(self.calls("sudo"))
        pid_file = self.root / "download.pid"
        if pid_file.exists():
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pid_file.read_text()), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
