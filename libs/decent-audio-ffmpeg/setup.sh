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

# ---------------------------------------------------------------------------
# LAME's autotools helper scripts.
#
# LAME 3.100 ships copies from 2013 that predate the Android triples: its
# config.sub rejects aarch64-linux-android outright ("Invalid configuration
# ... system 'android' not recognized"), so its configure never starts, and
# its config.guess cannot identify an Apple-silicon build host either. These
# files only canonicalise triples; they do not affect a single byte of
# generated code.
#
# Two things here are deliberate:
#
#   * Each candidate is validated by RUNNING it on the job it has to do, not
#     by looking at it. A throttling response, a truncated download and a copy
#     that is simply too old all fail that test, and none of them fails a
#     "does it start with #!" check.
#   * The SHIPPED copies are tested first, so nothing is downloaded on a host
#     where they already work — which is most Linux CI runners, and is why
#     this used to fetch two files to replace one that needed replacing.
#
# git.savannah.gnu.org is upstream, but it throttles consecutive cgit /plain/
# requests and has served a non-script response to a CI runner mid-loop. The
# GCC mirror carries the same two files and is tried second; whichever answers
# first still has to pass the same test.
# ---------------------------------------------------------------------------
CONFIG_SOURCES="
https://git.savannah.gnu.org/cgit/config.git/plain
https://raw.githubusercontent.com/gcc-mirror/gcc/master
"

# config.sub has to canonicalise the triple that broke; config.guess has to
# identify this build host at all.
check_config_script() {
  case "$2" in
    config.sub)   sh "$1" aarch64-linux-android >/dev/null 2>&1 ;;
    config.guess) sh "$1" >/dev/null 2>&1 ;;
    *)            return 1 ;;
  esac
}

refresh_config_script() {
  # Separate statements on purpose: bash expands every word of a `local` line
  # before the builtin assigns any of them, so `local name=$1 dest=$SRC/$name`
  # would build dest from an unset name.
  local name="$1"
  local dest="$SRC/lame/$name"
  local tmp base

  if check_config_script "$dest" "$name"; then
    echo "  $name: the copy LAME ships already handles this build"
    return 0
  fi

  tmp="$(mktemp)"
  for base in $CONFIG_SOURCES; do
    # -f so an HTTP error is an error (and therefore retried) instead of a
    # 200-shaped page written to disk. The timeouts are short because a dead
    # source must not cost more than moving on to the next one.
    if curl -fsSL --retry 2 --retry-delay 2 --connect-timeout 10 --max-time 25 \
         -o "$tmp" "$base/$name" &&
       check_config_script "$tmp" "$name"; then
      mv "$tmp" "$dest"
      chmod +x "$dest"
      echo "  $name: refreshed from $base"
      return 0
    fi
    echo "  $name: $base did not serve a usable copy" >&2
  done
  rm -f "$tmp"
  echo "ERROR: no source served a $name that handles this build." >&2
  return 1
}

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
else
  echo "lame already present at $SRC/lame"
fi

# Runs every time, not only on a fresh unpack: a tree left behind by an
# earlier run of this script may predate the check above.
echo "Checking LAME's config.sub/config.guess..."
refresh_config_script config.sub
refresh_config_script config.guess

echo "Sources ready in $SRC"
