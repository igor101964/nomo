#!/bin/bash
# nomo install script
# Supports: Ubuntu 22.04/24.04/26.04 (x86_64), Debian 12/13 (ARM64/x86_64)

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${GREEN}[nomo]${NC} $1"; }
warn()    { echo -e "${YELLOW}[warn]${NC} $1"; }
error()   { echo -e "${RED}[error]${NC} $1"; exit 1; }

NOMO_DIR="/home/$(whoami)/nomo"
INSTALL_BIN="$HOME/.local/bin/nomo"

# detect platform
detect_platform() {
    if [ ! -f /etc/os-release ]; then
        error "Cannot detect OS. /etc/os-release not found."
    fi
    . /etc/os-release
    DISTRO=$ID
    VERSION=$VERSION_ID
    ARCH=$(uname -m)

    info "Detected: $PRETTY_NAME ($ARCH)"

    case "$DISTRO" in
        ubuntu)
            case "$VERSION_ID" in
                22.04|24.04|26.04) ;;
                *) warn "Ubuntu $VERSION_ID is not tested. Proceeding anyway." ;;
            esac
            ;;
        debian)
            case "$VERSION_ID" in
                12|13) ;;
                *) warn "Debian $VERSION_ID is not tested. Proceeding anyway." ;;
            esac
            ;;
        *)
            warn "Distro '$DISTRO' is not officially supported. Trying anyway."
            ;;
    esac

    case "$ARCH" in
        x86_64|aarch64) ;;
        *) warn "Architecture $ARCH is not tested." ;;
    esac
}

# install dependencies
install_deps() {
    info "Installing dependencies..."
    if ! command -v sudo &>/dev/null; then
        error "sudo not found. Run as root or install sudo."
    fi

    sudo apt-get update -qq
    sudo apt-get install -y \
        build-essential \
        pkg-config \
        libgtk-4-dev \
        figlet

    info "Dependencies installed."
}

# build
build() {
    info "Building nomo..."
    make clean 2>/dev/null || true
    make
    info "Build successful."
}

# install binary and prompts
install_files() {
    info "Installing to $NOMO_DIR ..."
    mkdir -p "$NOMO_DIR/sysprompts"
    mkdir -p "$HOME/.local/bin"

    # copy binary
    cp build/nomo "$INSTALL_BIN"
    chmod 755 "$INSTALL_BIN"

    # copy prompts (don't overwrite existing ones)
    for f in sysprompts/*.prm; do
        dest="$NOMO_DIR/sysprompts/$(basename $f)"
        if [ ! -f "$dest" ]; then
            cp "$f" "$dest"
            info "  prompt: $(basename $f)"
        else
            info "  skipped (exists): $(basename $f)"
        fi
    done

    info "Binary installed: $INSTALL_BIN"
}

# check PATH
check_path() {
    if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
        warn "$HOME/.local/bin is not in PATH."
        warn "Add this to your ~/.bashrc or ~/.zshrc:"
        echo "  export PATH=\"\$HOME/.local/bin:\$PATH\""
    fi
}

# main
echo ""
echo "  nomo — ASCII Art Generator"
echo "  =========================="
echo ""

detect_platform
install_deps
build
install_files
check_path

echo ""
info "Done! Run nomo with:"
echo ""
echo "  nomo &"
echo ""
echo "  Or from mshell session for full LLM support."
echo ""
