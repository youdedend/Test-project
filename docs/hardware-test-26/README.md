# Hardware test #26 — the first swap page that came back wrong

Logs: `loader.log.txt`, `errors.log.txt`, `dozen.log.txt` (this folder), sent after the session
ended, before relaunching. Run date 2026-10-28.

Build identity: the log prints `UWP swap: device free=…`, which exists only from `.26301.421`
onward, so the console ran **`.26301.421` or `.26301.437`**. `UWP memory pressure:` never appears,
which does not separate the two: the session was already rendering at 1280x720
(`CoreWindow presentation bridge: 1280x720`, `Scene resolution: raster 1280x720 … (live)`), so the
resolution rung added in `.437` had nothing to lower.

## What the run did

| Reading | This run | `.1119` (`docs/hardware-test-25`) |
|---|---|---|
| Pages stored | **65,475** (319 raw, 19,783 blank, 276 MiB payload, **27% of raw**) | 125,940 (1,492 raw, 20,604 blank, 890 MiB, 45%) |
| Disk pages | 396 MiB of a 768 MiB budget (raised 0) | 1,125 MiB of 1,280 MiB (raised 2) |
| Page-outs / checks | out=1,022 MiB, in=7 MiB, **checks=499, corrupt=0** | checks at the end: corrupt=0 |
| Write failures | **0** | 0 |
| Sweeps | 398, none with nothing to give | — |
| presents | **844** (then frozen) | 941 |
| Frame rate at the end | 6.2 FPS, slowest frame 107.9 ms | 0.6 FPS |
| Ended with | **`STOP: UWP promoted page integrity at 000000104D3B8000, error 0`** | `WriteSpan: Assertion Failed! … No space left on device` |

The compression ratio is worse than `.1119`'s (27% vs 45%) for an ordinary reason: this session
died earlier (844 presents), with fewer of the very compressible pages stored and the same ~19,800
blank pages.

## What it says about the fixes that were shipped

- **The device reserve was not in force.** Every report line reads
  `UWP swap: device free=770 MiB (reserve 0 MiB kept for the game's own writes, 0 record(s) refused
  by it)`. The default is 384 MiB; `runtime_memory_start_paging` read `BB_UWP_SWAP_RESERVE_MB` as 0
  when the variable was unset and then assigned that 0 over the default
  (`guest_swap.inc:1206-1217` before this round). So `.26301.421`'s disk protection was compiled in
  but disabled at run time — the `.1119` `ENOSPC` abort could still have happened. Fixed: only an
  explicit value overrides the default.
- **Free space was never the problem in this run.** 1,096 MiB free for the first half, 770 MiB at
  the end, nothing refused (`device free=` line). This session did not run out of disk; it stopped
  for a different reason.
- **The pager still is not the freeze.** `UWP STALL DETECTED (presents stuck at 844 for 10 s)`
  followed by `lock=sweep held=0 ms sweeps=397 request=64 MiB finished 16 ms ago; out=1020 in=7 MiB
  disk=394 MiB budget=768 MiB` — the third run in a row where the sweep held nothing at the freeze.
  The stall is the guest's working set, unchanged by any of this.

## The stop: a compressed page of exactly 16 KiB

`errors.log.txt` ends with

```
STOP: UWP promoted page integrity at 000000104D3B8000, error 0
```

That is our own check (`guest_memory.inc:377-386`, hash of the page read back from the swap record
against the hash taken when it was stored) — i.e. **the swap returned bytes that were not what it
was given**. 500 verifications passed and the 501st did not.

The record header is a 2-byte length; **a length equal to the page size means "stored raw"**. The
old encoder wrote `length = packed` for every compressed block, and `uwp_lz_compress` can return
exactly 16,384 for a nearly-incompressible page. On the way back in the runtime sees
`length == PAGE`, takes the compressed block for a raw page, and memcpys 16 KiB of LZ4 bytes into
the guest page (`guest_swap.inc:407-410`). The integrity check is the only thing that noticed.

Reproduced on the host, in `xbox/platform/swap_codec.h` terms:

```
best=16384 packed_ge_page=235 bad=0        # 400,000 page-shaped pages, 235 land exactly on 16,384
found at attempt 4: packed=16384 total=16386 raw=1 header=16384   # with the fix, it is stored raw
```

About one page in 1,700 near the raw boundary. A session stores tens of thousands of pages and
verifies every one that is read back, so a run of this length hits it — it is not a rare accident,
it is arithmetic.

## The fix (`.26301.…`, this round)

1. `uwp_swap_encode_page` (`swap_codec.h:155-190`) owns the format rule: anything that does not
   come out **strictly smaller** than the page is stored raw, so a compressed length can never
   collide with the raw marker. `uwp_swap_encode` calls it; `swap_codec_test.cpp` sections 9 and 10
   hold the rule with a page the compressor really does turn into 16,384 bytes, plus 2,000 stored
   records read back through the runtime's own reader.
2. Store-time verification (`guest_swap.inc:373-391`, `437-452`): a compressed payload is
   decompressed and compared with the page **before** it is written; a mismatch stores the page raw
   instead and increments `codec fallbacks=N` in the report. The integrity stop can only catch a
   wrong record on the way back in, when the page is already gone; this catches it while the page
   is still in hand. ~10 µs per store. `BB_UWP_SWAP_VERIFY=0` disables it.
3. `uwp_swap_describe_corruption` (`guest_swap.inc:499-527`, called at `guest_memory.inc:383`):
   before any future integrity stop, one line with the record's physical page, key, slot, length,
   what that length means (raw / compressed / blank / missing), both hashes, and the store counters —
   so the next such log identifies itself instead of printing an address.

## What to send next time

`loader.log.txt` and `errors.log.txt`, right after the session ends, before relaunching. The lines
that answer the most:

- `grep "codec fallbacks" loader.log.txt` — if nonzero, the codec is still producing payloads that
  do not round-trip on the console, and the pager is now storing those pages raw instead of stopping.
- `grep "device free=" loader.log.txt` — the reserve should now read `reserve 384 MiB`.
- `grep "UWP swap: disk pages" loader.log.txt` — stored pages, raw count, payload ratio.
- `grep "corrupt record" errors.log.txt` — should be absent; if present, it says exactly which
  record and which shape.
