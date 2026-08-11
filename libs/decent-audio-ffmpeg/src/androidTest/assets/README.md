# Instrumentation fixtures

Two containers this module's own encoders cannot synthesise, and therefore the
two operations that could otherwise only be asserted by reading FFmpeg's
sources:

| file | why it exists |
|---|---|
| `flac_in_mp4.m4a` | FLAC inside MP4 — the input the whole "stream copy instead of re-encode" route rests on. Remuxing it to a `.flac` works only if `mov_read_dfla` hands the demuxer's extradata to `flacenc` in the shape it expects (a 34-byte STREAMINFO). Nothing in the shipped codec set can produce this file, so it has to be committed. |
| `eac3_51.mp4` | E-AC-3 5.1 — the Dolby branch's real input. This build has an E-AC-3 **decoder** and no E-AC-3 encoder, so again it cannot be made on device. It is what proves a 6-channel source reaches the FLAC encoder as 6 channels at 24 bits rather than being downmixed or refused. |

Both are synthetic: a generated sine tone, no recorded material of any kind.
Regenerate them with a host ffmpeg, byte-for-byte the commands below.

```sh
ffmpeg -f lavfi -i "sine=f=440:r=44100:d=2" -ac 2 -c:a flac -f mp4 flac_in_mp4.m4a

ffmpeg -f lavfi -i "sine=f=440:r=48000:d=2" \
  -af "pan=5.1|c0=c0|c1=0.8*c0|c2=0.6*c0|c3=0.2*c0|c4=0.5*c0|c5=0.4*c0" \
  -c:a eac3 -b:a 768k -f mp4 eac3_51.mp4
```

A tone rather than `anullsrc` silence on purpose: silence encodes to constant
subframes, which is the one case in which a broken decode path still produces a
plausible-looking output. The `pan` filter gives each of the six channels a
different amplitude, so a channel that gets lost or duplicated is visible in the
result rather than indistinguishable from its neighbours.
