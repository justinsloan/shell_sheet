#!/usr/bin/env bash
#
# Installs shell_sheet: its build dependencies, then the program itself.
#
#   ./install.sh                     # deps, build, install to /usr/local
#   ./install.sh --prefix ~/.local   # install somewhere that needs no root
#   ./install.sh --skip-deps         # deps already present
#   ./install.sh --dry-run           # print what would happen, change nothing
#
# Anything needing root is run through sudo one command at a time, and every
# such command is printed before it runs. Nothing here is piped into a shell.

set -euo pipefail

PREFIX="/usr/local"
SKIP_DEPS=0
DRY_RUN=0

usage() {
    sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; shift ;;
        --skip-deps) SKIP_DEPS=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown option: $1" >&2; usage 1 ;;
    esac
done

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }

# Runs a command, showing it first. Under --dry-run it only shows it.
run() {
    printf '    $ %s\n' "$*"
    [ "$DRY_RUN" -eq 1 ] && return 0
    "$@"
}

# Root is only needed for package installs and for writing outside $HOME.
as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        run "$@"
    elif command -v sudo >/dev/null 2>&1; then
        run sudo "$@"
    else
        echo "This step needs root and sudo is not installed:" >&2
        printf '    %s\n' "$*" >&2
        exit 1
    fi
}

# ---------------------------------------------------------------- dependencies

install_dependencies() {
    if pkg-config --exists gtkmm-4.0 2>/dev/null; then
        note "gtkmm-4.0 $(pkg-config --modversion gtkmm-4.0) is already installed."
        return 0
    fi

    # Deliberately explicit per distro rather than guessing: a wrong package
    # name installed as root is a bad way to find out the guess was wrong.
    if   command -v apt-get >/dev/null 2>&1; then
        as_root apt-get update
        as_root apt-get install -y build-essential pkg-config libgtkmm-4.0-dev
    elif command -v dnf >/dev/null 2>&1; then
        as_root dnf install -y gcc-c++ make pkgconf-pkg-config gtkmm4.0-devel
    elif command -v pacman >/dev/null 2>&1; then
        as_root pacman -S --needed --noconfirm base-devel pkgconf gtkmm-4.0
    elif command -v zypper >/dev/null 2>&1; then
        as_root zypper install -y gcc-c++ make pkg-config gtkmm4-devel
    else
        cat >&2 <<'MSG'
Could not recognise this system's package manager, so dependencies were not
installed. shell_sheet needs a C++17 compiler and the gtkmm-4.0 development
package. Install those, then re-run with --skip-deps.
MSG
        exit 1
    fi
}

# ---------------------------------------------------------------------- build

say "Checking prerequisites"
if [ "$SKIP_DEPS" -eq 1 ]; then
    note "Skipping dependency installation (--skip-deps)."
else
    install_dependencies
fi

if ! pkg-config --exists gtkmm-4.0 2>/dev/null && [ "$DRY_RUN" -eq 0 ]; then
    echo "gtkmm-4.0 still is not available to pkg-config; cannot build." >&2
    exit 1
fi

say "Building"
run make

say "Installing to $PREFIX"
# Writing inside your own home needs no privileges; anywhere else does.
case "$PREFIX" in
    "$HOME"/*) run make install PREFIX="$PREFIX" ;;
    *)         as_root make install PREFIX="$PREFIX" ;;
esac

say "Done"
note "Installed: $PREFIX/bin/shell_sheet"
case ":$PATH:" in
    *":$PREFIX/bin:"*) note "Run it with: shell_sheet" ;;
    *) note "Note: $PREFIX/bin is not on your PATH."
       note "Run it with: $PREFIX/bin/shell_sheet" ;;
esac
note "Remove it later with: make uninstall PREFIX=$PREFIX"
