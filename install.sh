#!/usr/bin/env bash
# Omarchy Calendar installer. Uses published releases and pacman-owned files.
# curl -fsSL https://github.com/last-refuge/omarchy-calendar/releases/latest/download/install.sh | bash
set -Eeuo pipefail

usage() {
  cat <<'USAGE'
Omarchy Calendar installer. Your time, beautifully organized.

  ./install.sh [options]

Installs on Omarchy and Arch Linux (x86_64 or aarch64). Run as your regular
desktop user; sudo is used only for package transactions.

  --version VERSION  Select a stable release, for example 1.0.0 or v1.0.0
  --source           Build the selected release using its published recipe
  --yes, -y          Accept installation and package-manager prompts
  --dry-run          Show the plan without writing files or changing the system
  --no-start         Do not enable, start, or restart the background service
  --verbose          Show detailed command output
  --help, -h         Show this help

Requires curl, jq, pacman, and the standard Arch command-line tools.
Source builds install base-devel, cmake, ninja, pkgconf and recipe dependencies.
Google sign-in requires an unlocked Secret Service keyring.
Logs: ${XDG_STATE_HOME:-~/.local/state}/omarchy-calendar/installer/
USAGE
}

presentation() {
  ACCENT='' GREEN='' AMBER='' BOLD='' DIM='' RESET='' HIDE='' SHOW=''
  ANIMATE=0
  OK='[ok]' BULLET='-' FAIL='[error]' SEP='|'
  if [[ "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" == *[Uu][Tt][Ff]*8* ]]; then
    OK='✔' BULLET='·' FAIL='✘' SEP='·'
  fi
  if [[ -t 1 && ${TERM:-dumb} != dumb && ! ${NO_COLOR+x} ]]; then
    ACCENT=$'\033[35m' GREEN=$'\033[32m' AMBER=$'\033[33m'
    if [[ ${COLORTERM:-} == *truecolor* || ${COLORTERM:-} == *24bit* ]]; then
      ACCENT=$'\033[38;2;110;86;207m'
    fi
    BOLD=$'\033[1m' DIM=$'\033[2m' RESET=$'\033[0m'
    HIDE=$'\033[?25l' SHOW=$'\033[?25h'
    (( VERBOSE )) || ANIMATE=1
  fi
}

ok() { printf '  %s%s%s  %s\n' "$GREEN" "$OK" "$RESET" "$*"; }
note() { printf '  %s%s  %s%s\n' "$DIM" "$BULLET" "$*" "$RESET"; }
warn() { printf '  %s!%s  %s\n' "$AMBER" "$RESET" "$*" >&2; }
die() {
  printf '%s' "$SHOW" >&2
  printf '\n  %s%s%s  %s\n' "$AMBER" "$FAIL" "$RESET" "$*" >&2
  if [[ -n ${LOG:-} ]]; then
    printf 'Error: %s\n' "$*" >>"$LOG"
    printf '     Full log: %s\n' "$LOG" >&2
  fi
  exit 1
}

cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n ${STEP_PID:-} ]]; then
    kill "$STEP_PID" 2>/dev/null || true
    wait "$STEP_PID" 2>/dev/null || true
  fi
  if [[ -n ${OUTPUT_FD:-} ]]; then exec {OUTPUT_FD}>&-; fi
  if [[ -n ${LOGGER_PID:-} ]]; then wait "$LOGGER_PID" 2>/dev/null || true; fi
  printf '%s' "$SHOW"
  if [[ -n ${WORK:-} && -d $WORK ]]; then rm -rf -- "$WORK"; fi
  exit "$status"
}

confirm() {
  (( ASSUME_YES )) && return 0
  [[ -n $TTYIN ]] || die 'No terminal is available. Run with --yes to authorize installation.'
  local reply=''
  printf '  %s?%s  %s [y/N] ' "$ACCENT" "$RESET" "$1" >"$TTYIN"
  read -r reply <"$TTYIN" || true
  [[ $reply == [yY] || $reply == [yY][eE][sS] ]]
}

# Quiet operations never inherit stdin: under curl | bash it contains this script.
step() {
  local message=$1 completed=$2 status=0 frame=0 label=$1
  shift 2
  if (( VERBOSE )); then
    note "$message"
    exec {OUTPUT_FD}> >(tee -a "$LOG")
    LOGGER_PID=$!
    "$@" </dev/null >&"$OUTPUT_FD" 2>&1 &
  else
    if (( ANIMATE )); then
      # Limit animated lines to fit small terminals. Static lines can wrap naturally.
      local columns=${COLUMNS:-80}
      [[ $columns =~ ^[0-9]+$ ]] || columns=80
      if (( columns < 20 )); then columns=20; fi
      label=${label:0:columns-8}
      printf '%s' "$HIDE"
    else
      note "$message"
    fi
    "$@" </dev/null >>"$LOG" 2>&1 &
  fi
  STEP_PID=$!
  while kill -0 "$STEP_PID" 2>/dev/null; do
    if (( ANIMATE )); then
      local frames=$'|/-\\'
      printf '\r  %s%s%s  %s' "$ACCENT" "${frames:frame++%4:1}" "$RESET" "$label"
    fi
    sleep 0.1
  done
  wait "$STEP_PID" || status=$?
  STEP_PID=''
  if (( VERBOSE )); then
    exec {OUTPUT_FD}>&-
    OUTPUT_FD=''
    wait "$LOGGER_PID" || status=$?
    LOGGER_PID=''
  fi
  if (( ANIMATE )); then printf '\r\033[K%s' "$SHOW"; fi
  if (( status )); then
    tail -n 12 "$LOG" >&2
    die "$message failed."
  fi
  ok "$completed"
}

# Password and package-manager prompts remain visible and read the terminal.
interactive() {
  local message=$1 completed=$2
  shift 2
  note "$message"
  if ! "$@" <"${TTYIN:-/dev/null}" 2>&1 | tee -a "$LOG"; then
    die "$message failed."
  fi
  ok "$completed"
}

download() {
  # Called only inside step's subprocess. exec lets cleanup stop curl directly.
  exec curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
    --connect-timeout 15 --max-time 600 --retry 2 --output "$WORK/$1" "$RELEASE_URL/$1"
}

verify_asset() {
  local asset=$1 hash name extra expected='' count=0 actual
  while read -r hash name extra; do
    if [[ $name == "$asset" || $name == "./$asset" ]]; then
      [[ $hash =~ ^[[:xdigit:]]{64}$ && -z $extra ]] || die "Invalid checksum entry for $asset."
      expected=${hash,,}
      count=$((count + 1))
    fi
  done <"$WORK/SHA256SUMS"
  (( count == 1 )) || die "Expected exactly one checksum for $asset; found $count."
  actual=$(sha256sum "$WORK/$asset")
  [[ ${actual%% *} == "$expected" ]] || die "Checksum failed for $asset. Nothing was installed."
  ok "Verified $asset"
}

platform() {
  local os_info os_id os_like
  [[ $(uname -s) == Linux ]] || die 'This installer supports Omarchy and Arch Linux.'
  os_info=$(cat /etc/os-release)
  os_id=$(sed -n 's/^ID=//p' <<<"$os_info")
  os_like=$(sed -n 's/^ID_LIKE=//p' <<<"$os_info")
  os_id=${os_id//\"/}; os_like=${os_like//\"/}
  [[ $os_id == arch || " $os_like " == *' arch '* ]] || die 'This installer supports Omarchy and Arch Linux.'
  DISTRO='Arch Linux'
  if [[ -d /usr/share/omarchy ]] || command -v omarchy >/dev/null 2>&1; then DISTRO=Omarchy; fi
  ARCH=$(uname -m)
  [[ $ARCH == x86_64 || $ARCH == aarch64 ]] || die "Unsupported architecture: $ARCH. Expected x86_64 or aarch64."
  [[ $(id -u) != 0 ]] || die 'Run this installer as your regular desktop user, without sudo.'
  local tool
  for tool in curl jq pacman vercmp sha256sum tar sudo; do
    command -v "$tool" >/dev/null 2>&1 || die "Missing $tool. Install it with pacman and run this again."
  done
}

resolve_release() {
  local endpoint='latest' metadata candidates
  [[ -z $REQUESTED_VERSION ]] || endpoint="tags/v${REQUESTED_VERSION#v}"
  if ! metadata=$(curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
    --connect-timeout 15 --max-time 60 --retry 2 \
    "https://api.github.com/repos/last-refuge/omarchy-calendar/releases/$endpoint" </dev/null); then
    die 'Could not fetch the release. Check your network or selected version and try again.'
  fi
  if ! TAG=$(jq -er 'select(.draft == false and .prerelease == false) | .tag_name' <<<"$metadata"); then
    die 'The response does not describe a published stable release.'
  fi
  [[ $TAG =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "Unsupported release tag: $TAG."
  [[ -z $REQUESTED_VERSION || $TAG == "v${REQUESTED_VERSION#v}" ]] || die 'The release does not match the requested version.'
  VERSION=${TAG#v}
  RELEASE_URL="https://github.com/last-refuge/omarchy-calendar/releases/download/$TAG"
  # Exact names avoid accidentally installing debug packages or a different architecture.
  candidates=$(jq -r --arg prefix "omarchy-calendar-$VERSION-" --arg suffix "-$ARCH.pkg.tar.zst" \
    '.assets[] | .name | select(startswith($prefix) and endswith($suffix))
     | select(ltrimstr($prefix) | rtrimstr($suffix) | test("^[0-9]+(\\.[0-9]+)*$"))' <<<"$metadata")
  [[ $candidates != *$'\n'* ]] || die 'The release contains multiple matching packages; cannot choose safely.'
  PKG_ASSET=$candidates
  METHOD='Prebuilt package'
  if (( FORCE_SOURCE )) || [[ -z $PKG_ASSET ]]; then
    METHOD='Build from release source'
    SOURCE_BUILD=1
    PKG_ASSET=''
    AUR_ASSET="omarchy-calendar-$VERSION-aur.tar.gz"
    SOURCE_ASSET="omarchy-calendar-$VERSION.tar.gz"
  fi
  local asset
  local required=(SHA256SUMS)
  if (( SOURCE_BUILD )); then required+=("$AUR_ASSET" "$SOURCE_ASSET"); else required+=("$PKG_ASSET"); fi
  for asset in "${required[@]}"; do
    jq -e --arg name "$asset" --arg url "$RELEASE_URL/$asset" \
      '[.assets[] | select(.name == $name and .browser_download_url == $url)] | length == 1' \
      <<<"$metadata" >/dev/null || die "Missing or invalid release asset: $asset."
  done
  TARGET_VERSION=$VERSION
  if (( ! SOURCE_BUILD )); then
    TARGET_VERSION=${PKG_ASSET#omarchy-calendar-}
    TARGET_VERSION=${TARGET_VERSION%-"$ARCH".pkg.tar.zst}
  fi
  INSTALLED_VERSION=$(pacman -Q omarchy-calendar 2>/dev/null | awk '{print $2}') || INSTALLED_VERSION=''
  if [[ -n $INSTALLED_VERSION ]]; then
    local comparison installed=$INSTALLED_VERSION
    if (( SOURCE_BUILD )); then installed=${installed%-*}; fi
    comparison=$(vercmp "$TARGET_VERSION" "$installed")
    if (( comparison < 0 )) && [[ -z $REQUESTED_VERSION ]]; then
      die "Installed $INSTALLED_VERSION is newer than release $TARGET_VERSION. Use --version to select a downgrade explicitly."
    fi
    if (( comparison == 0 )); then ALREADY_INSTALLED=1; fi
  fi
}

build_package() {
  step 'Downloading release recipe' 'Downloaded release recipe' download "$AUR_ASSET"
  verify_asset "$AUR_ASSET"
  step 'Downloading release source' 'Downloaded release source' download "$SOURCE_ASSET"
  verify_asset "$SOURCE_ASSET"
  # Only the two regular recipe files are needed. Do not extract arbitrary paths.
  mkdir "$WORK/aur"
  tar -xOf "$WORK/$AUR_ASSET" aur/PKGBUILD >"$WORK/aur/PKGBUILD" 2>>"$LOG"
  tar -xOf "$WORK/$AUR_ASSET" aur/.SRCINFO >"$WORK/aur/.SRCINFO" 2>>"$LOG"
  cp "$WORK/$SOURCE_ASSET" "$WORK/aur/"
  interactive 'Installing build tools' 'Installed build tools' sudo pacman -S --needed "${PACMAN_FLAGS[@]}" base-devel cmake ninja pkgconf
  command -v makepkg >/dev/null 2>&1 || die 'makepkg is missing after installing base-devel.'
  # makepkg verifies the recipe's pinned source checksum and installs declared dependencies.
  (cd "$WORK/aur" && interactive 'Building release package (this may take a few minutes)' 'Built release package' \
    makepkg --syncdeps --cleanbuild "${PACMAN_FLAGS[@]}")
  local candidates=()
  local package
  shopt -s nullglob
  for package in "$WORK/aur/omarchy-calendar-$VERSION-"*"-$ARCH.pkg.tar.zst"; do candidates+=("$package"); done
  shopt -u nullglob
  (( ${#candidates[@]} == 1 )) || die 'The build did not produce exactly one application package.'
  PACKAGE=${candidates[0]}
}

service_setup() {
  SERVICE_READY=0
  if (( NO_START )); then
    note 'Service activation skipped (--no-start).'
    return 0
  fi
  if ! command -v systemctl >/dev/null 2>&1 || ! systemctl --user show-environment </dev/null >>"$LOG" 2>&1; then
    warn 'Package installed; no systemd user session is available.'
    return 0
  fi
  if ! systemctl --user daemon-reload </dev/null >>"$LOG" 2>&1 \
    || ! systemctl --user enable --now omarchy-calendar.service </dev/null >>"$LOG" 2>&1; then
    warn "Package installed, but service activation failed. Full log: $LOG"
    return 1
  fi
  if (( PACKAGE_CHANGED )) && [[ -n $INSTALLED_VERSION ]]; then
    if ! systemctl --user restart omarchy-calendar.service </dev/null >>"$LOG" 2>&1; then
      warn "Package installed, but service restart failed. Full log: $LOG"
      return 1
    fi
  fi
  if ! systemctl --user is-active --quiet omarchy-calendar.service </dev/null >>"$LOG" 2>&1; then
    warn "Package installed, but the service is not active. Full log: $LOG"
    return 1
  fi
  SERVICE_READY=1
  ok 'Background service is ready'
}

next_steps() {
  printf '\n  %s%s%s\n\n' "$BOLD" "${1:-Package installed.}" "$RESET"
  printf '    Open       Omarchy Calendar from your app launcher\n'
  printf '    Terminal   omarchy-calendar\n'
  printf '    Google     Connect your account in Settings\n'
  if (( ! SERVICE_READY )); then
    printf '\n    Activate   systemctl --user enable --now omarchy-calendar.service\n'
    printf '    Diagnose   journalctl --user -u omarchy-calendar.service -n 50\n'
  fi
  printf '\n'
}

main() {
  FORCE_SOURCE=0 ASSUME_YES=0 DRY=0 NO_START=0 VERBOSE=0 REQUESTED_VERSION=''
  WORK='' LOG='' STEP_PID='' LOGGER_PID='' OUTPUT_FD='' TTYIN=''
  SOURCE_BUILD=0 ALREADY_INSTALLED=0 PACKAGE_CHANGED=0
  while (( $# )); do
    case $1 in
      --source) FORCE_SOURCE=1 ;;
      --yes|-y) ASSUME_YES=1 ;;
      --dry-run) DRY=1 ;;
      --no-start) NO_START=1 ;;
      --verbose) VERBOSE=1 ;;
      --version) shift; [[ $# -gt 0 && -n $1 ]] || { printf '%s\n' '--version requires a version' >&2; return 1; }; REQUESTED_VERSION=$1 ;;
      --version=*) REQUESTED_VERSION=${1#*=}; [[ -n $REQUESTED_VERSION ]] || { printf '%s\n' '--version requires a version' >&2; return 1; } ;;
      --help|-h) usage; return 0 ;;
      *) printf 'Unknown option: %s (try --help)\n' "$1" >&2; return 1 ;;
    esac
    shift
  done
  presentation
  trap cleanup EXIT
  trap 'die "Interrupted. Run the installer again when ready."' INT TERM
  trap 'die "Unexpected failure at line $LINENO."' ERR
  if [[ -n $REQUESTED_VERSION ]]; then
    [[ $REQUESTED_VERSION =~ ^v?[0-9]+\.[0-9]+\.[0-9]+$ ]] || die 'Use a stable version such as 1.0.0 or v1.0.0.'
  fi
  if { : </dev/tty; } 2>/dev/null; then TTYIN=/dev/tty; fi
  if (( ! DRY && ! ASSUME_YES )) && [[ -z $TTYIN ]]; then
    die 'No terminal is available. Run with --yes to authorize installation.'
  fi
  printf '\n  %s%somarchy calendar%s\n  %sYour time, beautifully organized.%s\n\n' "$ACCENT" "$BOLD" "$RESET" "$DIM" "$RESET"
  platform
  resolve_release
  printf '    Install  %s\n    System   %s %s %s\n    Method   %s\n\n' "$TARGET_VERSION" "$DISTRO" "$SEP" "$ARCH" "$METHOD"
  ok 'Checked system requirements'
  if [[ -n $INSTALLED_VERSION ]]; then note "Currently installed: $INSTALLED_VERSION"; fi
  if (( ALREADY_INSTALLED )); then ok 'Selected version is already installed'; fi
  if (( DRY )); then
    note 'Dry run: no files or system settings will be changed.'
    if (( ! ALREADY_INSTALLED )); then
      note "Would download release assets from $RELEASE_URL and verify their checksums."
      if (( SOURCE_BUILD )); then
        note 'Would install build tools and use makepkg to build the published recipe as your user.'
      fi
      note 'Would install the application package with sudo pacman -U.'
    fi
    if (( NO_START )); then note 'Would skip service activation.'
    else note 'Would reload, enable/start and check the user service; restart it after an upgrade.'; fi
    printf '\n'
    return 0
  fi
  if (( SOURCE_BUILD && ! ALREADY_INSTALLED )); then note 'A source build may take a few minutes and needs build dependencies.'; fi
  if (( ! ALREADY_INSTALLED )); then note 'Package installation uses sudo and may ask for your password.'; fi
  if ! confirm 'Proceed with the plan above?'; then note 'Cancelled. No changes were made.'; return 0; fi

  umask 077
  local state_dir=${XDG_STATE_HOME:-$HOME/.local/state}/omarchy-calendar/installer
  mkdir -p "$state_dir"
  LOG=$(mktemp "$state_dir/install-$(date +%Y%m%d-%H%M%S).XXXXXX.log")
  printf 'Release: %s\nSystem: %s %s\nMethod: %s\nPreviously installed: %s\n' \
    "$TAG" "$DISTRO" "$ARCH" "$METHOD" "${INSTALLED_VERSION:-none}" >>"$LOG"
  WORK=$(mktemp -d "${TMPDIR:-/tmp}/omarchy-calendar-install.XXXXXX")
  local PACMAN_FLAGS=()
  (( ! ASSUME_YES )) || PACMAN_FLAGS+=(--noconfirm)
  if (( ! ALREADY_INSTALLED )); then
    step 'Downloading checksum manifest' 'Downloaded checksum manifest' download SHA256SUMS
    if (( SOURCE_BUILD )); then build_package
    else
      step 'Downloading release package' 'Downloaded release package' download "$PKG_ASSET"
      verify_asset "$PKG_ASSET"
      PACKAGE="$WORK/$PKG_ASSET"
    fi
    interactive 'Installing Omarchy Calendar' 'Installed Omarchy Calendar' sudo pacman -U "${PACMAN_FLAGS[@]}" "$PACKAGE"
    local installed
    installed=$(pacman -Q omarchy-calendar)
    local installed_version=${installed#omarchy-calendar }
    if (( SOURCE_BUILD )); then installed_version=${installed_version%-*}; fi
    [[ $installed == 'omarchy-calendar '* && $installed_version == "$TARGET_VERSION" ]] \
      || die "Package transaction finished, but the selected version was not installed: $installed."
    ok "Checked installed package: $installed"
    PACKAGE_CHANGED=1
  fi
  local service_status=0
  service_setup || service_status=$?
  if ! command -v busctl >/dev/null 2>&1 \
    || ! busctl --user --timeout=5 --no-pager list </dev/null 2>>"$LOG" | awk '$1 == "org.freedesktop.secrets" {found=1} END {exit !found}'; then
    warn 'Google sign-in needs a Secret Service provider, such as GNOME Keyring, KWallet or KeePassXC.'
  fi
  note 'Unlock your keyring before connecting Google Calendar.'
  if (( SERVICE_READY )); then next_steps 'Ready to plan your day.'; else next_steps 'Package installed; service activation is pending.'; fi
  exit "$service_status"
}

# Functions can be sourced for inspection; piping the complete script also works.
if [[ ${BASH_SOURCE[0]:-} == "$0" || -z ${BASH_SOURCE[0]:-} ]]; then main "$@"; fi
