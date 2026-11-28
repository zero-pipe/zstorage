# Test fixtures

Generated media files are intentionally **not committed** (see `.gitignore`).
Regenerate them with `./generate.sh` (requires ffmpeg).

## Files

| File | Container | Video | Audio |
|------|-----------|-------|-------|
| `h264_aac.flv` | FLV | H.264 (Annex-B / AVCC) | AAC-LC |
| `h264_aac.ts` | MPEG-TS | H.264 | AAC-LC |
| `h264_aac.mp4` | MP4 | H.264 | AAC-LC |
| `h264_aac.mkv` | Matroska | H.264 | AAC-LC |
| `h265_aac.mp4` | MP4 | H.265 | AAC-LC |
| `h265_aac.mkv` | Matroska | H.265 | AAC-LC |
| `av1_opus.mp4` | MP4 | AV1 | Opus |
| `av1_opus.mkv` | Matroska | AV1 | Opus |

H.265 / AV1 files are generated only when the required ffmpeg encoders are
present.

## Packet trace format

The baseline harness records one JSON object per demuxed packet (JSONL):

```json
{"track":"video","codec":"H264","pts_ms":0,"dts_ms":0,"flags":["KEY","CONFIG"],"len":41,"sha256":"..."}
```

Field rules:

- `track`: `video`, `audio`, `subtitle`, or `data`.
- `codec`: canonical `zm_codec_id` name (`H264`, `H265`, `AAC`, `AV1`, ...).
- `pts_ms` / `dts_ms`: canonical int64 milliseconds.
- `flags`: subset of `KEY`, `CONFIG`, `EOS`, `DISCARDABLE`, `LOST`.
- `len`: payload byte count.
- `sha256`: lowercase hex digest of the payload.

Baselines live in `traces/*.jsonl`. The trace tool (`tests/trace_packets`) is
introduced with `zm_reader` in P2; until then, treat the format above as the
contract.

To run a fixture-gated test against an arbitrary file:

```bash
ZCS_FIXTURE=/path/to/file.mp4 ./build/tests/test_zm_mov
ZCS_FIXTURE=/path/to/file.mp4 ./build/tests/test_zm_dash
```
