#!/usr/bin/env python3
"""Exercise source/AUR artifact generation without production credentials or publication."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tarfile
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("release", PROJECT / "packaging/prepare-release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseArtifactsTest(unittest.TestCase):
    def test_round_trip_and_rejections(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo = root / "repo"
            (repo / "app").mkdir(parents=True)
            (repo / "packaging/arch").mkdir(parents=True)
            (repo / "packaging/omarchy").mkdir()
            metadata = (PROJECT / "packaging/omarchy/package.json").read_text()
            (repo / "packaging/omarchy/package.json").write_text(metadata)
            # A different version catches hardcoded release numbers.
            (repo / "app/version.h").write_text('#define OMARCHY_CALENDAR_VERSION "2.3.4"\n')
            template = (PROJECT / "packaging/arch/PKGBUILD.in").read_text()
            template = re.sub(r"^pkgver=.*$", "pkgver=2.3.4", template, flags=re.M)
            (repo / "packaging/arch/PKGBUILD.in").write_text(template)
            for command in (["init", "-q"], ["add", "."],
                            ["-c", "user.name=Test", "-c", "user.email=test@example.test", "commit", "-qm", "fixture"]):
                release.git(repo, *command)
            oauth = root / "oauth.env"
            oauth.write_text("OMARCHY_CALENDAR_GOOGLE_CLIENT_ID=test.apps.googleusercontent.com\n"
                             "OMARCHY_CALENDAR_GOOGLE_CLIENT_SECRET=test-secret\n")
            first, second = root / "first", root / "second"
            release.prepare(repo, "HEAD", oauth, first)
            release.prepare(repo, "HEAD", oauth, second)
            name = "omarchy-calendar-2.3.4"
            source = first / f"{name}.tar.gz"
            self.assertEqual(source.read_bytes(), (second / source.name).read_bytes())
            checksum = hashlib.blake2b(source.read_bytes()).hexdigest()
            self.assertIn(checksum, (first / "aur/PKGBUILD").read_text())
            self.assertIn(f"b2sums = {checksum}", (first / "aur/.SRCINFO").read_text())
            self.assertNotIn("SKIP", (first / "aur/PKGBUILD").read_text())
            with tarfile.open(source) as tar:
                member = tar.getmember(f"{name}/packaging/google-oauth.env")
                self.assertEqual(member.mode, 0o644)
                self.assertEqual(tar.extractfile(member).read(), oauth.read_bytes())
            with tarfile.open(first / f"{name}-aur.tar.gz") as tar:
                self.assertEqual(set(tar.getnames()), {"aur", "aur/PKGBUILD", "aur/.SRCINFO"})
            with tarfile.open(first / f"{name}-opr.tar.gz") as tar:
                self.assertEqual(set(tar.getnames()), {
                    "pkgbuilds", "pkgbuilds/omarchy-calendar",
                    "pkgbuilds/omarchy-calendar/PKGBUILD",
                    "pkgbuilds/omarchy-calendar/.omarchy",
                    "pkgbuilds/omarchy-calendar/.omarchy/package.json"})
                self.assertEqual(tar.extractfile("pkgbuilds/omarchy-calendar/PKGBUILD").read(),
                                 (first / "aur/PKGBUILD").read_bytes())
                watch = json.load(tar.extractfile("pkgbuilds/omarchy-calendar/.omarchy/package.json"))
                self.assertEqual(watch["source"], "local")
                self.assertEqual(watch["upstream"]["watch"]["github"], "last-refuge/omarchy-calendar")
                pattern = watch["upstream"]["watch"]["pattern"]
                self.assertEqual(re.fullmatch(pattern, "v2.3.4")["version"], "2.3.4")
                self.assertIsNone(re.fullmatch(pattern, "v2.3.4-rc1"))
                self.assertNotIn("release_ring", watch)
            self.assertEqual((first / f"{name}-opr.tar.gz").read_bytes(),
                             (second / f"{name}-opr.tar.gz").read_bytes())
            with self.assertRaises(ValueError):
                release.prepare(repo, "HEAD", oauth, first)
            release.git(repo, "tag", "v9.9.9")
            with self.assertRaises(ValueError):
                release.prepare(repo, "v9.9.9", oauth, root / "wrong-version")
            oauth.write_text(oauth.read_text() + "REFRESH_TOKEN=must-never-ship\n")
            with self.assertRaises(ValueError):
                release.prepare(repo, "HEAD", oauth, root / "unexpected-key")


if __name__ == "__main__":
    unittest.main()
