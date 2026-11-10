# diag-5 — `1.0.26314.<build>`: make the crash name itself

Two changes, both diagnostics. Nothing about rendering, memory or the watchdog is touched, so this
build should behave exactly like diag-4b plus two better lines in the log. If the run looks different,
that is information too — but it should not.

## 1. `module=unknown` cannot happen again

Run 3 died with

```
UNHANDLED NATIVE FAULT: code=0xc0000005 module=unknown+0x23ec55 rip=0x7ffac2edec55 addr=0x1429f5fed70
```

and there was no way to tell what `+0x23ec55` belonged to. The cause is boring: the fault filter
names a module by enumerating the process's modules through two kernel32 exports, and when those are
not available (as in the UWP API partition) it returned "nothing" — for a `rip` that was inside our
own executable.

- The executable the process was started from is now always nameable, without those exports.
- **Both** fault lines now carry `base=0x…` next to `module=+0x…`, so even an address that cannot be
  named by any means is still identifiable against a build.

The line you will see is a superset of the old one; anything reading `module=`/`addr=` keeps working.

## 2. A truncated command stream says what it was

Run 5 (the `dzn_debug=d3d12` run) produced exactly one emulator-side error that run 4 did not:

```
GPU [Lib.GnmDriver] <Error> amdgpu/liverpool.cpp:70 NextPacket: packet length exceeds remaining
submission size. Packet dword count=2, remaining submission dwords=1
```

A submission ended one dword short of a complete packet, so the rest of that command stream was
dropped — silently, because the loop simply stops. This build prints the stream's own dwords when it
happens:

```
GPU [Lib.GnmDriver] <Error> … : truncated stream: N dwords, overrun by M; first [xxxxxxxx …] last […]
```

The first eight dwords are the stream's header (it says what the stream starts with and which queue
it belongs to) and the last eight are what it did contain — the leftover half-packet is normally the
last word. It reads only inside the submission, so it cannot fault.

## How to read the run

1. Launch and play to the point it usually stops.
2. Send `loader.log.txt` **from that run** — the archived `loader-<timestamp>.log` files are previous
   runs, and the suffix is a collision counter, not a run number. The line `UWP build: version=…` in
   the file says which build it belongs to, and that is the number that matters.
3. Send `errors.log` from the same run.

What diag-5 is trying to answer, in order:

- **Where does it stop now?** The `presents=` curve plus the fault line. Run 5's own crash point was
  never measured (its loader log never arrived), so this run is the first measurement of the
  migration build since diag-4b.
- **If it faults:** which module, and at what offset. `module=` should never read `unknown` again.
- **If the truncated-stream line appears,** its dwords identify the stream — that is the only
  emulator-side defect in run 5's log and the next candidate to chase.

The D3D12 debug layer now stays off (`dzn_debug=` empty). It did its job: 14,742 messages, all
resource-state complaints, no fatal line, and it never named crash #2 — so leaving it on would only
cost frame time. If you want it again, put `dzn_debug=d3d12` back in `bbport.ini` and expect
`dozen.log` to grow to megabytes.

## Not in this build, on purpose

A fix for the buffer state tracking the debug layer exposed (we emit one global memory barrier and
never a per-buffer one, so dzn has to guess a buffer's state on first use). The reason it is not here
is that a buffer barrier in dzn becomes a whole-resource transition, and every dense arena is a single
`VkBuffer` of hundreds of MiB — so a naive version can make things worse, and it cannot be validated
without the debug layer on, which would confound this run's measurement. The plan for it is written
up in the research notes; it gets its own build.
