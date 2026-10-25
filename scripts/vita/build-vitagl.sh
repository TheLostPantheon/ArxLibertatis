#!/bin/sh
# Build the pinned vitaGL into Vitagl/ (gitignored), where CMakeLists.txt
# links it from. Re-running is cheap: it only rebuilds when the pin or the
# flags change.
#
#   VITAGL_COMMIT  commit to build (default: the pin below)
#   VITAGL_REPO    where to clone from (default: ~/Dev/vita/vitaGL if present,
#                  else upstream on GitHub)
#   VITAGL_FLAGS   make flags (default: the set the port was tuned with)
#
# scripts/vita/vitagl-patches/*.patch are applied on top of the commit.
set -e

# Upstream Rinnegatamante/vitaGL as of the port's last change (2026-02-16).
PIN=fcace3b
# HAVE_SHADER_CACHE: compiled custom shaders are kept in ux0:data/shader_cache/ARXL00001,
# keyed by a hash of the source, so only the first load after a shader change compiles.
FLAGS_DEFAULT="NO_DEBUG=1 BUFFERS_SPEEDHACK=1 CIRCULAR_VERTEX_POOL=2 DEPTH_STENCIL_HACK=1 HAVE_SHADER_CACHE=1"

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR="$ROOT/Vitagl"
COMMIT=${VITAGL_COMMIT:-$PIN}
FLAGS=${VITAGL_FLAGS:-$FLAGS_DEFAULT}
if [ -z "$VITAGL_REPO" ]; then
	if [ -d "$HOME/Dev/vita/vitaGL/.git" ]; then
		VITAGL_REPO="$HOME/Dev/vita/vitaGL"
	else
		VITAGL_REPO=https://github.com/Rinnegatamante/vitaGL.git
	fi
fi

if [ ! -d "$DIR/.git" ]; then
	git clone --quiet "$VITAGL_REPO" "$DIR"
fi
git -C "$DIR" fetch --quiet origin 2>/dev/null || true
if [ -d "$HOME/Dev/vita/vitaGL/.git" ]; then
	# The local clone has the upstream remote's objects too.
	git -C "$DIR" fetch --quiet "$HOME/Dev/vita/vitaGL" '+refs/remotes/upstream/*:refs/remotes/upstream/*' 2>/dev/null || true
fi
git -C "$DIR" reset --quiet --hard
git -C "$DIR" -c advice.detachedHead=false checkout --quiet "$COMMIT"

# Fixes the port needs on top of the pin (scripts/vita/vitagl-patches/)
PATCHES=$(ls "$ROOT"/scripts/vita/vitagl-patches/*.patch 2>/dev/null || true)
for patch in $PATCHES; do
	git -C "$DIR" apply "$patch"
done
PATCH_SUM=$(cat $PATCHES 2>/dev/null | git hash-object --stdin)

STAMP="$DIR/.arx-build-stamp"
WANT="$(git -C "$DIR" rev-parse HEAD) $PATCH_SUM $FLAGS"
if [ -f "$DIR/libvitaGL.a" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$WANT" ]; then
	echo "vitaGL $(git -C "$DIR" rev-parse --short HEAD) already built ($FLAGS)"
	exit 0
fi

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
make -C "$DIR" clean >/dev/null 2>&1 || true
# shellcheck disable=SC2086
make -C "$DIR" $FLAGS -j"$JOBS"
echo "$WANT" > "$STAMP"
echo "built vitaGL $(git -C "$DIR" rev-parse --short HEAD) + $(echo $PATCHES | wc -w | tr -d ' ') patch(es) ($FLAGS)"
