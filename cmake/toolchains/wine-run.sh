#!/bin/sh
# Runs a cross-compiled test through Wine with a sane, quiet environment.
: "${XDG_RUNTIME_DIR:=/tmp/datapath-wine-runtime-$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
export XDG_RUNTIME_DIR
export WINEDEBUG="${WINEDEBUG:--all}"
exec wine "$@"
