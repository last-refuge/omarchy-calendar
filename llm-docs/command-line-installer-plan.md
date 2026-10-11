# Command-line installer plan

Status: implemented in the working tree; publication and fresh Omarchy desktop
acceptance remain pending.

Date: October 11, 2026.

## Goal

Create a self-contained Bash installer with beautifully formatted terminal
output, following the presentation and interaction patterns of
[Facet's installer](https://github.com/last-refuge/omarchy-facet/blob/master/install.sh).
The reference repository is `last-refuge/omarchy-facet`, with a hyphen in the owner name.

Install Calendar through its existing Arch package so pacman continues to own
the installed files and dependencies. A successful run should clearly report
both installation status and background-service readiness, then explain how
to open Calendar and connect Google Calendar.

## Existing foundations

- The [1.0.0 release](https://github.com/last-refuge/omarchy-calendar/releases/tag/v1.0.0)
  provides an x86_64 application package, a separate debug package, source and
  recipe archives, and `SHA256SUMS`.
- `packaging/arch/PKGBUILD.in` defines the package and supports x86_64 and aarch64.
- Published source archives include the production Desktop OAuth configuration.
- `.github/workflows/release.yml` already builds and uploads release artifacts.
- `packaging/systemd/omarchy-calendar.service` defines the background service.
- Service and DBus activation definitions currently reference `/usr/bin`, so a
  general `--prefix` installation would need separate packaging work.

## 1. Terminal presentation

Follow Facet's compact banner, generous spacing, animated progress, quiet
command output, readable errors, and clear next steps. Use Calendar's purple
accent, muted supporting text, green completion marks, and amber warnings.
Avoid large ASCII artwork and an additional terminal UI dependency.

Example completed run:

```text
  omarchy calendar · Your time, beautifully organized.

  Install  1.0.0
  System   Omarchy · x86_64
  Method   Prebuilt package

  ✔  Checked system requirements
  ✔  Downloaded release
  ✔  Verified package checksum
  ✔  Installed Omarchy Calendar
  ✔  Started background service

  Ready to plan your day.

    Open       Omarchy Calendar from your app launcher
    Terminal   omarchy-calendar
    Google     Connect your account in Settings
```

Requirements:

- Animate only the active step.
- Support `NO_COLOR`, `TERM=dumb`, redirected output, narrow terminals, and
  ASCII fallback.
- Keep noninteractive output readable without cursor-control sequences.
- Restore the cursor on completion, interruption, and errors.
- Report actual progress and outcomes; do not show success before verification.

## 2. Installer entry point and options

Add a single root-level `install.sh` supporting:

| Option | Behavior |
| --- | --- |
| Default | Install the latest stable release |
| `--version VERSION` | Select a particular release |
| `--source` | Build the selected release using its published recipe |
| `--yes` | Accept installation prompts for unattended use |
| `--dry-run` | Show planned actions without changing the system |
| `--no-start` | Install without enabling or starting the service |
| `--verbose` | Show detailed command output |
| `--help` | Explain usage and requirements |

Initial platform scope is Omarchy and Arch Linux on x86_64 and aarch64.
General installation prefixes and other distributions are outside the initial
implementation.

## 3. Release selection and package installation

1. Detect the platform, architecture, installed version, and required tools
   before changing anything.
2. Resolve one release tag, then fetch all assets from that release.
3. Select the application package for the detected architecture, explicitly
   excluding debug packages.
4. Verify the selected download against `SHA256SUMS` before installation.
5. Install with `sudo pacman -U`, preserving package-manager ownership and
   signature policy.
6. Offer a source build when the release has no matching binary. At planning
   time, the current release has an x86_64 binary; aarch64 needs this fallback.
7. Build from the published recipe and source archive, which include the
   production OAuth configuration. Run `makepkg` as the regular user.
8. Treat network failures and checksum failures as errors, with actionable
   messages, rather than silently switching installation methods.

Show the proposed version and installation method before proceeding. Repeated
runs should recognize an already installed version. Downgrades require explicit
version selection. Do not rely on official Omarchy repository availability to
make the initial installer work.

## 4. Background service and first-run experience

After installation, reload the user's systemd units, enable and start the
service, and restart it when upgrading. Check that it becomes active. Respect
`--no-start` throughout this flow.

Distinguish "package installed" from "service ready." If no user session is
available, print the exact activation command instead of reporting complete
success:

```bash
systemctl --user enable --now omarchy-calendar.service
```

Check for a Secret Service provider and explain that Google sign-in needs an
unlocked keyring. Finish with launcher instructions and the Settings sign-in
step. Preserve calendar data, preferences, and credentials during upgrades.

## 5. Errors, logging, and prompts

- Follow Facet's `/dev/tty` handling so prompts work when the script arrives
  through a pipe. Commands must not consume the script's standard input.
- Keep password and package-manager prompts visible. Use spinners for
  noninteractive operations.
- Store logs in a persistent user state directory. Show a short failure excerpt
  and the log path when an operation fails.
- Preserve failure logs after cleanup. Facet's inspected implementation removes
  the temporary directory containing its advertised failure log; Calendar should
  improve on that behavior.
- Without a terminal, require `--yes` before mutations.
- Make cancellation clean up temporary downloads and restore terminal state.
- Keep dry runs free of system changes, including package installation and
  service activation.

## 6. Release integration and documentation

Update `.github/workflows/release.yml` to attach `install.sh` and include it in
`SHA256SUMS`. Publish the installer with the next release, then add this command
to `README.md` once the asset is available:

```bash
curl -fsSL https://github.com/last-refuge/omarchy-calendar/releases/latest/download/install.sh | bash
```

Document the options, source-build fallback, service activation, and how to
locate logs. Retain the existing manual installation instructions as an
alternative.

## 7. Validation

Add installer tests covering:

- Release selection and architecture matching, including exclusion of debug
  packages.
- Checksum failure and download failure.
- Source fallback when a matching binary is absent.
- Cancellation and terminal restoration.
- Missing user session and service startup failure.
- Repeat installation and explicit version selection.
- Mutation-free dry runs.
- Piped execution and noninteractive confirmation behavior.
- Plain output and color-disabled output.

Use command stubs and fixture release metadata for automated tests so they do
not install packages or alter the developer's user service. Run ShellCheck and
`bash -n` in CI.

Verify the formatted output in a real terminal. Test fresh installation and an
upgrade in a disposable Omarchy environment, checking launcher availability,
service readiness, and preservation of existing user data.

## Planned files

| File | Change |
| --- | --- |
| `install.sh` | Add installer, formatting helpers, release selection, and service handling |
| `tests/test-installer.sh` | Add installer behavior tests and fixtures as needed |
| `tests/installer/` | Isolated release fixtures, command doubles, and pseudo-terminal tests |
| `.github/workflows/ci.yml` | Add shell validation and installer tests |
| `.github/workflows/release.yml` | Publish the installer and checksum it |
| `README.md` | Document the one-command installation flow and options |
| `docs/arch-release.md` | Document installer publication and acceptance checks |

## Completion criteria

One command installs Calendar, accurately reports service readiness, produces
attractive output in an interactive terminal, remains readable in logs, and
preserves existing user data. Published installer instructions must point to an
available release asset, and supported installation paths must pass automated
checks and disposable-desktop acceptance testing.

## Implementation and validation record

Implemented on October 11, 2026:

- Root-level `install.sh` with all planned options, terminal formatting, pinned
  release selection, verified downloads, source fallback, pacman transactions,
  service readiness checks, and persistent logs.
- CI shell validation and 30 isolated installer tests, including pseudo-terminal
  checks for confirmation, animation, cursor restoration, and cancellation.
- Release workflow uploads the tagged installer and includes it in `SHA256SUMS`.
- README documents the checkout-based installer and explicitly defers the
  release-hosted one-command instructions until that asset is published.
- Release documentation records publication and fresh desktop acceptance steps.

Validation:

- Bash syntax and ShellCheck 0.11.0 pass.
- The installer behavior tests pass using local command doubles; no host package
  transactions or service changes occur in this suite.
- A live, read-only dry run selected the public 1.0.0-1 x86_64 package and
  recognized the existing installed version.
- The public 1.0.0 recipe and source passed the installer's SHA256 verification
  and makepkg's pinned BLAKE2 verification. A build in a temporary directory,
  using already-installed dependencies, produced the expected application package.
- Colored success output was inspected through a pseudo-terminal. Automated
  checks also cover narrow output, ASCII, `NO_COLOR`, and `TERM=dumb`.

Remaining release checks:

- Publish a new release containing `install.sh`, then verify the hosted installer
  download before enabling the one-command README instructions.
- Test fresh installation and upgrade on a disposable Omarchy desktop, including
  launcher availability, a real systemd user session, and data retention. Docker
  validation on the current machine requires a sudo password; no fresh desktop
  acceptance is claimed by the fixture tests or temporary source build.
- Confirm source fallback on a real aarch64 system. The fixture suite covers
  architecture selection; the temporary source build ran on x86_64.
