#!/usr/bin/env bash
# Installs swapp's build dependencies. Run once per machine, with sudo; the
# build itself (z_run.sh) needs no elevation.
set -euo pipefail

usage() {
    echo "Usage: sudo $0"
    exit 1
}

if [[ $# -gt 0 ]]; then
    usage
fi

if [[ "$(id -u)" -ne 0 ]]; then
    echo "This script installs system packages and must be run with sudo." >&2
    usage
fi

cd "$(dirname "${BASH_SOURCE[0]}")"

check_deps() {
    missing=()
    command -v cmake >/dev/null 2>&1 || missing+=("cmake")
    command -v pkg-config >/dev/null 2>&1 || missing+=("pkg-config")
    { command -v gcc >/dev/null 2>&1 || command -v cc >/dev/null 2>&1; } || missing+=("c-compiler")
    pkg-config --exists gtk+-3.0 2>/dev/null || missing+=("gtk+-3.0-dev")
    { pkg-config --exists ayatana-appindicator3-0.1 2>/dev/null \
        || pkg-config --exists appindicator3-0.1 2>/dev/null; } \
        || missing+=("appindicator3-dev")
    pkg-config --exists libnotify 2>/dev/null || missing+=("libnotify-dev")
    pkg-config --exists x11 2>/dev/null || missing+=("libx11-dev")
    pkg-config --exists glib-2.0 2>/dev/null || missing+=("libglib2.0-dev")
    # DDC/CI monitor probing talks i2c-dev directly (no ddcutil dependency),
    # which needs the userspace i2c-dev.h header shipped by libi2c-dev/i2c-tools.
    [[ -f /usr/include/linux/i2c-dev.h ]] || missing+=("libi2c-dev")
}

install_deps() {
    if [[ ! -r /etc/os-release ]]; then
        echo "Cannot detect distro (/etc/os-release missing)." >&2
        echo "Install manually: cmake, ninja, a C compiler, pkg-config, GTK3 dev headers, AppIndicator dev headers." >&2
        exit 1
    fi

    # shellcheck disable=SC1091
    . /etc/os-release
    local ids="${ID:-} ${ID_LIKE:-}"

    echo "Installing missing build dependencies (detected: ${ID:-unknown})..."
    case "$ids" in
        *debian*|*ubuntu*)
            apt-get update
            apt-get install -y build-essential cmake ninja-build pkg-config libgtk-3-dev
            apt-get install -y libayatana-appindicator3-dev \
                || apt-get install -y libappindicator3-dev
            apt-get install -y libnotify-dev libx11-dev libi2c-dev libglib2.0-dev
            ;;
        *fedora*|*rhel*|*centos*)
            if command -v dnf >/dev/null 2>&1; then
                dnf install -y gcc make cmake ninja-build pkgconf-pkg-config gtk3-devel
                dnf install -y libappindicator-gtk3-devel
                dnf install -y libnotify-devel libX11-devel i2c-tools-devel glib2-devel
            else
                yum install -y gcc make cmake ninja-build pkgconfig gtk3-devel
                yum install -y libappindicator-gtk3-devel
                yum install -y libnotify-devel libX11-devel i2c-tools-devel glib2-devel
            fi
            ;;
        *arch*)
            pacman -Sy --needed --noconfirm base-devel cmake ninja pkgconf gtk3
            pacman -Sy --needed --noconfirm libayatana-appindicator \
                || pacman -Sy --needed --noconfirm libappindicator-gtk3
            pacman -Sy --needed --noconfirm libnotify libx11 i2c-tools glib2
            ;;
        *suse*)
            zypper --non-interactive install gcc make cmake ninja pkg-config gtk3-devel
            zypper --non-interactive install libappindicator3-devel
            zypper --non-interactive install libnotify-devel libX11-devel i2c-tools-devel glib2-devel
            ;;
        *alpine*)
            apk add --no-cache build-base cmake ninja pkgconfig gtk+3.0-dev
            apk add --no-cache libappindicator-dev
            apk add --no-cache libnotify-dev libx11-dev i2c-tools-dev glib-dev
            ;;
        *)
            echo "Unrecognized distro (ID=${ID:-} ID_LIKE=${ID_LIKE:-})." >&2
            echo "Install manually: cmake, ninja, a C compiler, pkg-config, GTK3 dev headers, AppIndicator dev headers." >&2
            exit 1
            ;;
    esac
}

check_deps
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Missing build dependencies: ${missing[*]}"
    install_deps
    check_deps
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "Still missing after install attempt: ${missing[*]}. Install them manually and re-run." >&2
        exit 1
    fi
fi

check_deps
if [[ ${#missing[@]} -eq 0 ]]; then
    echo "All build dependencies are already installed."
    exit 0
fi

echo "Missing build dependencies: ${missing[*]}"
install_deps
check_deps
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Still missing after install attempt: ${missing[*]}. Install them manually and re-run." >&2
    exit 1
fi

echo "Build dependencies installed. Build and start the app with ./z_run.sh"
