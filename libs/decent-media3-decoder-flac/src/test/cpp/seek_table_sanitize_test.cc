// Tests for seek_table_sanitize.h — self-contained, no libFLAC required
// (the sanitizer is templated on the point type for exactly this reason).
//
// Build & run (one line):
//   g++ -std=c++11 -Wall -Werror -o seek_table_sanitize_test
//     libs/decent-media3-decoder-flac/src/test/cpp/seek_table_sanitize_test.cc
//   ./seek_table_sanitize_test
//
// The fixture reproduces a real field file: a backend-generated seek table
// whose frame scan recorded one FALSE SYNC (a byte pattern inside compressed
// audio that passed the 8-bit header CRC) as entry 128 of 206 —
// sample_number=7, frame_samples=1024 between valid neighbors at samples
// 5852160 and 5939712, with a byte offset that is plausible for the slot.
// Because the seek resolver picks the LAST entry <= target, that one entry
// captured every backward seek below its valid successor and teleported
// playback to ~2 minutes in, on that file, every time.

#include <cassert>
#include <cstdio>
#include <vector>

#include "../../main/jni/include/seek_table_sanitize.h"

// Field-shape mirror of FLAC__StreamMetadata_SeekPoint.
struct TestSeekPoint {
  uint64_t sample_number;
  uint64_t stream_offset;
  unsigned frame_samples;
};

static const uint64_t kPlaceholder = 0xFFFFFFFFFFFFFFFFULL;
static const uint64_t kTotalSamples = 9408000;  // from the field file

static bool eq(const TestSeekPoint& a, const TestSeekPoint& b) {
  return a.sample_number == b.sample_number &&
         a.stream_offset == b.stream_offset &&
         a.frame_samples == b.frame_samples;
}

// The field corruption: valid stride-46080 neighborhood with the false-sync
// intruder in the middle (values verbatim from the file's metaflac dump).
static void testFieldFalseSyncDropped() {
  const TestSeekPoint points[] = {
      {5806080, 28695627, 4608},   // point 126
      {5852160, 28924511, 4608},   // point 127
      {7, 29128570, 1024},         // point 128 — the false sync
      {5939712, 29349840, 4608},   // point 129
      {5985792, 29576371, 4608},   // point 130
  };
  std::vector<TestSeekPoint> out;
  size_t dropped = sanitizeSeekPoints(points, 5, kTotalSamples, &out);
  assert(dropped == 1);
  assert(out.size() == 4);
  assert(eq(out[0], points[0]));
  assert(eq(out[1], points[1]));
  assert(eq(out[2], points[3]));  // intruder gone, neighbors intact
  assert(eq(out[3], points[4]));
}

// A valid table passes through byte-identical (baseline discipline: the
// sanitizer must be invisible for well-formed files).
static void testValidTableUntouched() {
  const TestSeekPoint points[] = {
      {0, 0, 4608},
      {46080, 196451, 4608},
      {92160, 390267, 4608},
      {9395712, 46639146, 4608},
  };
  std::vector<TestSeekPoint> out;
  size_t dropped = sanitizeSeekPoints(points, 4, kTotalSamples, &out);
  assert(dropped == 0);
  assert(out.size() == 4);
  for (int i = 0; i < 4; i++) assert(eq(out[i], points[i]));
}

// Trailing placeholders (legal per spec) are skipped without counting as
// corruption.
static void testPlaceholdersSkippedSilently() {
  const TestSeekPoint points[] = {
      {0, 0, 4608},
      {46080, 196451, 4608},
      {kPlaceholder, 0, 0},
      {kPlaceholder, 0, 0},
  };
  std::vector<TestSeekPoint> out;
  size_t dropped = sanitizeSeekPoints(points, 4, kTotalSamples, &out);
  assert(dropped == 0);
  assert(out.size() == 2);
}

// A garbage entry with a HUGE sample number must not survive: kept, it would
// make the monotonicity rule reject every valid entry after it.
static void testHugeGarbageDropped() {
  const TestSeekPoint points[] = {
      {0, 0, 4608},
      {46080, 196451, 4608},
      {0xDEADBEEF00ULL, 400000, 4608},  // >= totalSamples
      {92160, 390267, 4608},
      {138240, 582189, 4608},
  };
  std::vector<TestSeekPoint> out;
  size_t dropped = sanitizeSeekPoints(points, 5, kTotalSamples, &out);
  assert(dropped == 1);
  assert(out.size() == 4);
  assert(out[2].sample_number == 92160);
  assert(out[3].sample_number == 138240);
}

// Unknown total samples (STREAMINFO said 0): the size guard stands down but
// monotonicity still catches the field intruder.
static void testUnknownTotalSamples() {
  const TestSeekPoint points[] = {
      {5852160, 28924511, 4608},
      {7, 29128570, 1024},
      {5939712, 29349840, 4608},
  };
  std::vector<TestSeekPoint> out;
  size_t dropped = sanitizeSeekPoints(points, 3, /* totalSamples= */ 0, &out);
  assert(dropped == 1);
  assert(out.size() == 2);
  assert(out[0].sample_number == 5852160);
  assert(out[1].sample_number == 5939712);
}

// An offset that goes backwards is as corrupt as a sample number that does —
// both halves of an entry must advance.
static void testNonMonotonicOffsetDropped() {
  const TestSeekPoint points[] = {
      {0, 0, 4608},
      {46080, 196451, 4608},
      {92160, 100, 4608},  // sample ok, offset went backwards
      {138240, 582189, 4608},
  };
  std::vector<TestSeekPoint> out;
  size_t dropped = sanitizeSeekPoints(points, 4, kTotalSamples, &out);
  assert(dropped == 1);
  assert(out.size() == 3);
  assert(out[2].sample_number == 138240);
}

// A fully-corrupt table degrades to empty → the parser reports "no table"
// and the extractor falls back to binary search seeking.
static void testFullyCorruptTableDegradesToEmpty() {
  const TestSeekPoint points[] = {
      {9500000, 100, 4608},  // >= totalSamples
      {9600000, 200, 4608},  // >= totalSamples
  };
  std::vector<TestSeekPoint> out;
  size_t dropped = sanitizeSeekPoints(points, 2, kTotalSamples, &out);
  assert(dropped == 2);
  assert(out.empty());
}

int main() {
  testFieldFalseSyncDropped();
  testValidTableUntouched();
  testPlaceholdersSkippedSilently();
  testHugeGarbageDropped();
  testUnknownTotalSamples();
  testNonMonotonicOffsetDropped();
  testFullyCorruptTableDegradesToEmpty();
  std::printf("seek_table_sanitize_test: all tests passed\n");
  return 0;
}
