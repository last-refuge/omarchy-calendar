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
settings and retains unsigned artifacts for seven days. The CI bootstrap imports Omarchy’s public keyring from the pinned upstream
commit rather than relying on a live keyserver lookup; fingerprint-specific
trust and package signature verification remain enabled. Local `extra-x86_64-build`
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

Validation recorded on October 2, 2026: source revision
`e179729da75f53ba710d201ace268ac051252db3` passed
[all four CI jobs](https://github.com/last-refuge/omarchy-calendar/actions/runs/37050584629):
application build/install/visual/contract checks, Arch package validation, and
native Omarchy edge package builds for x86_64 and aarch64. The generated OPR
metadata also passed upstream validation, and the cached source archive passed
`makepkg --verifysource`. These used test OAuth settings. They do not establish
live Google authorization, public release discovery, or desktop acceptance.

## Production validation on October 4, 2026

Google's Verification Center reports both branding and data access verified.
An isolated service profile on the publisher's existing Omarchy x86_64 desktop,
using the approved Desktop app client and an unlocked private Secret Service,
passed these live Google API checks:

- Sign-in without the unverified-app warning, followed by synchronization.
- Creation and editing of a temporary event without guests or reminders.
- Credential restoration and preservation of the edit after service restart.
- Deletion of that event and confirmation it stayed absent after another sync.
- Account disconnect, with zero remaining accounts, calendars, events, or
  exported feed events, and no stored refresh token. A further restart retained
  the empty state.

The temporary Google event was deleted. Testing exposed and fixed a stale
compatibility-feed import that could restore disconnected calendar data; the
new regression test also covers importing that stale export into a fresh database.

The release build, install-layout check, 14-view visual matrix, and all 23
contract checks passed locally. The 20,000-event performance fixture passed
with 250 range queries in 3,992 ms and 25 searches in 1,074 ms. A final targeted
database regression check passed after the disconnect fix. [Hosted CI](https://github.com/last-refuge/omarchy-calendar/actions/runs/37235332029)
passed all four jobs for tagged source
`8e983357ee985bd42b886a2e0a3a32b6c00a719a`: application build/install/visual/contracts,
Arch package validation, and native Omarchy edge builds for x86_64 and aarch64.
The Omarchy bootstrap used the pinned upstream public keyring after the public
keyserver failed; fingerprint-specific trust and signature checks were retained.

This was service validation on an existing desktop, not acceptance on a fresh
Omarchy installation. Live RSVP, permission revocation, offline reconnect,
notifications, OS logout/login, and package upgrade were not repeated here;
mock contract checks cover RSVP, recovery, mutation queues, and token failures.
Fresh desktop and channel acceptance remain explicit maintainer/release tasks.

## Publication status on October 4, 2026

- [Version 1.0.0 is public](https://github.com/last-refuge/omarchy-calendar/releases/tag/v1.0.0),
  including the source, recipe bundles, x86_64 binary and debug packages,
  checksums, and source revision.
- The [production release workflow](https://github.com/last-refuge/omarchy-calendar/actions/runs/37236054987)
  passed. Downloaded assets passed every `SHA256SUMS` check, and the three
  generated archives matched an independent local preparation byte-for-byte.
- `makepkg --verifysource` fetched the public source with no local cache and
  verified its pinned BLAKE2 checksum. Current upstream-watch validation passed;
  discovery recognized the stable release as already current at 1.0.0.
- [Omarchy inclusion PR #804](https://github.com/omacom/omarchy-pkgs/pull/804)
  is open with only the recipe and release-watch metadata. Maintainer review,
  required workflow/build approval, signing, and repository publication remain
  pending. Do not claim official repository availability until those complete.
- Direct AUR submission is deferred at the publisher’s request. The downloadable
  recipe bundle can still be used to build and install locally.

The first release attempt passed testing and packaging but failed while creating
its draft because the GitHub CLI inferred the repository from a checkout owned
by the build user. The corrected workflow supplies the repository explicitly and
supports retries of an existing tag. Version 1.0.0 was retried without moving its
tag or changing its source archive.

## Submitted maintainer proposal

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
- Proposed architectures: x86_64 and aarch64; both passed native edge builds in
  [CI](https://github.com/last-refuge/omarchy-calendar/actions/runs/37235332029).
- Packaging: source build from an immutable GitHub release archive with a pinned
  checksum; direct GitHub release watch; no build-time publisher credentials.
- Google integration: the archive includes the publisher's Desktop app client
  settings. User refresh tokens remain in Secret Service. An unlocked provider
  such as GNOME Keyring is required for sign-in; local configuration overrides
  remain supported.

Google production OAuth approval was confirmed in the Verification Center on
October 4, 2026. Production sign-in, synchronization, test-event creation and
editing, credential restoration after restart, and deletion/resync have been
retested. The inclusion PR proposes the default edge-to-rc-to-stable channel
policy; maintainers decide whether additional acceptance checks are required.
This proposal is for optional repository availability;
default installation or shell calendar integration would be discussed separately.

The submitted PR includes the public release URL, passing CI and architecture
results, public source checks, and the scope and limits of desktop acceptance.
The generated bundle alone does not submit future updates automatically.

## Submission gates

- [x] Hosted contract, Arch package, and both Omarchy architecture builds pass
- [x] Google verification confirmed in the production Verification Center
- [x] Post-approval production sign-in, sync, mutation, restart, and disconnect checks recorded
- [x] First immutable source release published; fresh downloads verify
- [x] Upstream watch resolves the public release successfully
- [ ] Fresh Omarchy install, login, notifications, upgrade, and data retention tested
- [ ] Maintainers accept the proposal and agree on channel/architecture policy
- [x] OPR contribution contains only the recipe and `.omarchy/package.json`
- [x] Omarchy inclusion PR submitted with release and validation evidence
- [ ] OPR checks pass; maintainer build approval obtained if required
- [ ] Maintainers sign/publish/promote; stable installation verified

New contributors' OPR builds may await the `build-approved` label or maintainer
vouching. That approval and repository signing stay with Omarchy's maintainers;
the generated bundle does not grant publication rights.

## Desktop acceptance checks

Use a disposable, current Omarchy installation with an unlocked Secret Service
provider. Record the Omarchy version, channel, architecture, source revision,
and package checksum with the results. Build success alone does not cover these
desktop checks:

1. Install the candidate package with pacman and launch it from the application
   menu. Check the icon, Omarchy theme, and service activation in a fresh login.
2. With the approved production Desktop app client, connect a dedicated test
   account, sync calendars, and create, edit, RSVP to, and delete test events.
   Confirm changes in Google Calendar and verify a reminder notification.
3. Restart the application and log out/in. Confirm account reconnection, retained
   calendar data and preferences, and the absence of duplicate background services.
4. Disconnect the network, queue a test change, restart, and reconnect. Confirm
   the change reaches Google once. Test revoked access and account disconnect.
5. Upgrade from the previous candidate with test data and preferences present.
   Confirm both survive. Remove/reinstall the package and confirm user data is
   retained; verify that account disconnect still removes the account's tokens.

Run these against the agreed release channel before claiming that channel is
supported. CI currently covers native builds against edge, not a full desktop
session or rc/stable compatibility.

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

## Submit the Omarchy contribution

After maintainer agreement and the public source release, extract that release's
`omarchy-calendar-VERSION-opr.tar.gz` into a current fork of
[`omacom/omarchy-pkgs`](https://github.com/omacom/omarchy-pkgs). Use the production
release bundle, not the CI bundle containing a fake OAuth client.

1. Confirm the contribution changes only `pkgbuilds/omarchy-calendar/PKGBUILD`
   and `pkgbuilds/omarchy-calendar/.omarchy/package.json`.
2. Without a cached source archive, run `makepkg --verifysource` from the package
   directory to verify the public download and pinned checksum. From the OPR
   root, run the upstream-watch `validate` and `check` commands shown above.
3. Build with the current upstream tooling for the agreed architectures and
   channels. Include the public release URL, exact source revision, CI evidence,
   and desktop acceptance results in the inclusion PR.
4. Address maintainer feedback and any required `build-approved`/vouching step.
   Maintainers control merging, signing, repository publication, and promotion.
5. Once published, verify installation through pacman on a fresh installation
   using the agreed Omarchy repository channel, then repeat the desktop checks.
   Record the published version and channel before announcing availability.

Future releases use the same immutable source-archive process. Omarchy's release
watch proposes version/checksum updates; packaging changes still need a separate
OPR contribution. A new client configuration also requires a new upstream release.
