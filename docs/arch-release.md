# Preparing an Arch, AUR, or Omarchy release

`packaging/arch/PKGBUILD.in` is a template, not an AUR submission. Release
preparation generates `PKGBUILD` and `.SRCINFO` with the checksum of the exact
source archive that users will download. There is no dependency on a private
file beside an AUR checkout or on a missing Git tag during local builds.

## Prepare locally

Install `base-devel`, `python`, `devtools`, and `namcap`. Commit the intended
source changes first: the generator archives a committed revision, not the
working tree. It never creates tags, pushes commits, or publishes artifacts.

Provide a Google **Desktop app** client in a file outside Git, containing only:

```ini
OMARCHY_CALENDAR_GOOGLE_CLIENT_ID=your-client-id.apps.googleusercontent.com
OMARCHY_CALENDAR_GOOGLE_CLIENT_SECRET=your-client-secret
```

The two values will be distributed in the release source archive and installed
package. Google treats installed applications as public clients that cannot
keep secrets; this is distinct from user tokens, which stay in Secret Service.
Do not use a web client, service-account key, access token, or refresh token.
The generator rejects additional fields rather than copying them into an artifact.
See [Google's installed-app documentation](https://developers.google.com/identity/protocols/oauth2/native-app).

From the repository root, as a regular user:

```bash
python3 packaging/prepare-release.py --ref HEAD \
  --oauth-env /absolute/path/to/google-oauth.env --output build-aur-release
cd build-aur-release/aur
extra-x86_64-build
cd ../..
./tests/check-arch-package.sh build-aur-release/aur
```

`extra-x86_64-build` uses sudo to create and run a clean Arch chroot. The archive
already present next to `PKGBUILD` lets it build before any release is public;
makepkg still verifies the pinned checksum. For a faster local build, use
`makepkg --cleanbuild` in that directory instead. Do not install the test package
over your daily application unless you intend to replace it.

Preparation outputs:

- `omarchy-calendar-VERSION.tar.gz`: source plus publisher Desktop app settings.
- `omarchy-calendar-VERSION-aur.tar.gz`: only `aur/PKGBUILD` and `aur/.SRCINFO`.
- `omarchy-calendar-VERSION-opr.tar.gz`: the `pkgbuilds/omarchy-calendar/`
  recipe and `.omarchy/package.json` for the official Omarchy Package Repository.
- `SOURCE_REVISION`: the archived Git commit.
- `SHA256SUMS`: checksums of those release files.
- `aur/`: a local build directory with the same source archive and AUR recipe.
- `opr/`: the unpacked Omarchy contribution, ready to overlay onto an OPR clone.

Use a new output directory for each run. Repeating a revision with the same
client settings produces identical archives. CI uses an obviously fake desktop
client to exercise packaging without exposing publisher credentials. Never
publish CI's test artifacts as an official release.

## Omarchy repository validation

The OPR metadata lives in `packaging/omarchy/package.json`. Its GitHub watch
selects stable `vMAJOR.MINOR.PATCH` releases from `last-refuge/omarchy-calendar`.
It leaves channel policy at Omarchy's default; fast-ring publication and default
installation are separate maintainer decisions. AUR publication is optional.

CI builds the generated package in a fresh Arch container, then overlays the
generated OPR contribution onto pinned upstream tooling and builds natively for
both x86_64 and aarch64 against Omarchy's edge repositories. CI uses test OAuth
settings and retains unsigned artifacts for seven days. Local `extra-x86_64-build`
validation remains useful; nested systemd-nspawn inside GitHub's job container
is not used because the outer container has no running system bus.

To reproduce the Omarchy build from a prepared release and an OPR clone:

```bash
tar -xzf build-aur-release/omarchy-calendar-VERSION-opr.tar.gz -C /path/to/omarchy-pkgs
cp build-aur-release/omarchy-calendar-VERSION.tar.gz /path/to/omarchy-pkgs/pkgbuilds/omarchy-calendar/
cd /path/to/omarchy-pkgs
python3 helpers/upstream-watch.py validate pkgbuilds/omarchy-calendar
bin/build --mirror edge --arch x86_64 --package omarchy-calendar
```

The source archive copied into the recipe directory is only a local cache for
prepublication testing. Never include it in the OPR contribution. Repeat the
build on native aarch64 (or configured QEMU), and test the agreed rc/stable
targets before publication. Source checksums remain enforced even for cached
archives. After a stable upstream release exists, also run:

```bash
python3 helpers/upstream-watch.py check pkgbuilds/omarchy-calendar
```

Release discovery cannot pass until the upstream GitHub release is public.
The watch ignores draft and prerelease releases. See the
[OPR upstream-watch documentation](https://github.com/omacom/omarchy-pkgs/blob/master/docs/upstream-sources.md).

## Maintainer proposal draft

**Subject: Package inclusion proposal: Omarchy Calendar**

I'd like to propose Omarchy Calendar as an optional package in the Omarchy
Package Repository. It is a native Qt 6/QML calendar with day, week, month, and
agenda views, local SQLite storage, Google Calendar sync and editing, reminders,
and Omarchy theme integration.

- Upstream: https://github.com/last-refuge/omarchy-calendar
- Product and screenshots: https://lastrefuge.ai/projects/omarchy-calendar
- License: MIT
- Proposed package name: `omarchy-calendar`
- Maintainer/contact: Jason Alexander, `jason@greatspark.com`
- Proposed architectures: x86_64 and aarch64, subject to successful native builds.
- Packaging: source build from an immutable GitHub release archive with a pinned
  checksum; direct GitHub release watch; no build-time publisher credentials.
- Google integration: the archive includes the publisher's Desktop app client
  settings. User refresh tokens remain in Secret Service. An unlocked provider
  such as GNOME Keyring is required for sign-in; local configuration overrides
  remain supported.

The first public release is held until Google verification and the live
production authorization lifecycle are complete. Would you welcome an inclusion
PR once those checks pass, and do you have a preferred channel policy or any
additional requirements? This proposal is for optional repository availability;
default installation or shell calendar integration would be discussed separately.

Before sending, update the verification status, attach the current passing CI
run and architecture results, and confirm the maintenance commitment. No proposal
or upstream pull request has been sent by the preparation tools.

## Submission gates

- [ ] Hosted contract, Arch package, and both Omarchy architecture builds pass
- [ ] Google verification confirmed and live production lifecycle tested
- [ ] First immutable source release published; fresh downloads verify
- [ ] Upstream watch resolves the public release successfully
- [ ] Fresh Omarchy install, login, notifications, upgrade, and data retention tested
- [ ] Maintainers accept the proposal and agree on channel/architecture policy
- [ ] OPR contribution contains only the recipe and `.omarchy/package.json`
- [ ] OPR checks pass; maintainer build approval obtained if required
- [ ] Maintainers sign/publish/promote; stable installation verified

New contributors' OPR builds may await the `build-approved` label or maintainer
vouching. That approval and repository signing stay with Omarchy's maintainers;
the generated bundle does not grant publication rights.

## Publish after Google approval

1. Finish live authorization, synchronization, create/edit/RSVP/delete, restart,
   disconnect, and revoked-access checks using the production desktop client.
2. Update the version in `app/version.h`, both qmake projects, `CMakeLists.txt`,
   `packaging/arch/PKGBUILD.in`, AppStream metadata, README, and changelog. Run
   metadata, install, application contract, and clean-chroot checks.
3. Commit and push the intended source, then tag that commit `vVERSION` and push
   the tag. GitHub's release workflow takes the Desktop app settings from its
   existing OAuth secrets and creates a draft release with validated artifacts.
4. Review and publish the draft. Verify `SHA256SUMS` and test a fresh extraction
   of the AUR bundle with `makepkg --verifysource`; it must fetch the archive
   through the public release URL without your local source cache.
5. Commit only the generated `PKGBUILD` and `.SRCINFO` to the AUR package repo.
   Do not upload the source archive, binaries, or `PKGBUILD.in` to AUR.

Do not replace published source archives in place. Changed source or client
configuration needs a new upstream release and freshly generated checksums.
Keep the final publication/AUR submission separate from local preparation.
