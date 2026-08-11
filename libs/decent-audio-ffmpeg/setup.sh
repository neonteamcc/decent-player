#!/bin/bash
# Fetches the ffmpeg and LAME sources used by build-ffmpeg.sh.
#
# Upstream sources are NEVER vendored into this repo — they are cloned or
# unpacked at build time, exactly like decent-media3-decoder-flac/setup.sh
# does for libFLAC. Both trees are gitignored.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/src/main/jni/upstream"

# Pinned upstream revisions. Bump deliberately — every later build inherits
# the codec behaviour and the ABI of whatever is pinned here.
FFMPEG_TAG="n8.1.2"
LAME_VERSION="3.100"

mkdir -p "$SRC"

if [ ! -d "$SRC/ffmpeg" ]; then
  echo "Cloning FFmpeg $FFMPEG_TAG (depth=1)..."
  git clone https://github.com/FFmpeg/FFmpeg.git \
    --branch "$FFMPEG_TAG" --depth=1 "$SRC/ffmpeg"
else
  echo "ffmpeg already present at $SRC/ffmpeg"
fi

if [ ! -d "$SRC/lame" ]; then
  echo "Fetching LAME $LAME_VERSION..."
  curl -sL "https://downloads.sourceforge.net/project/lame/lame/$LAME_VERSION/lame-$LAME_VERSION.tar.gz" \
    | tar xz -C "$SRC"
  mv "$SRC/lame-$LAME_VERSION" "$SRC/lame"

  # LAME 3.100 ships autotools helper scripts from 2013 that predate the
  # Android triples. Its own config.sub rejects aarch64-linux-android with
  # "Invalid configuration ... system 'android'", so configure never runs.
  # Refresh both from GNU config upstream. These files only canonicalise
  # triples — they do not affect a single byte of generated code.
  echo "Refreshing LAME config.guess/config.sub for Android triples..."
  for f in config.sub config.guess; do
    curl -sL -o "$SRC/lame/$f" \
      "https://git.savannah.gnu.org/cgit/config.git/plain/$f"
    # A CDN/error page would silently break configure later; fail loudly now.
    head -1 "$SRC/lame/$f" | grep -q '^#!' || {
      echo "ERROR: downloaded $f is not a shell script" >&2; exit 1; }
    chmod +x "$SRC/lame/$f"
  done
else
  echo "lame already present at $SRC/lame"
fi

echo "Sources ready in $SRC"
