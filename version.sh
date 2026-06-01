#!/bin/sh
# Emit a MinVer-style version string for the qpext build.
#
#   - On a tag:        v0.3.0                  (clean checkout, exact tag)
#   - After a tag:     v0.3.0-2-gd34db33       (2 commits after v0.3.0)
#   - Uncommitted:     v0.3.0-2-gd34db33-dirty
#   - No tags yet:     0.0.0-12-gd34db33       (synthesised SemVer-shaped
#                                                pre-tag pseudo-version)
#   - Not a git tree:  dev                     (e.g. tarball install)
#
# Used by:
#   qpext/build.sh   -> -DQPEXT_VERSION="…"   (embedded in qpext.so, used in
#                                              the MQTT sw_version field)
#   qpext/deploy.sh  -> qpext/qml/version.txt (loaded by HelloImpl.qml)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

if ! git -C "$HERE" rev-parse --git-dir >/dev/null 2>&1; then
    echo "dev"
    exit 0
fi

# Preferred path: a real `git describe` against the most recent tag.
if v=$(git -C "$HERE" describe --tags --dirty 2>/dev/null); then
    echo "$v"
    exit 0
fi

# Fallback: repo has no tags. Synthesise a SemVer-looking pseudo-version
# so the device displays something useful (not just a bare SHA). MinVer
# uses 0.0.0 as the "no tag" baseline; we follow that convention.
sha=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)
height=$(git -C "$HERE" rev-list --count HEAD 2>/dev/null || echo 0)
dirty=""
git -C "$HERE" diff --quiet HEAD 2>/dev/null || dirty="-dirty"
echo "0.0.0-${height}-g${sha}${dirty}"
