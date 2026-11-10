# diag-4 — the driver switches are reachable from bbport.ini (publish notes)

Prepared by the research session, 2026-11-10. Tip `d82bfd9` over the published diag-3 tip
`489444f` (`xbox-alpha-1.0.26314.2016`). Publish with `NEW-CHAT-RUNBOOK.md` v6.

## Why this exists

The diag-3 run (`26314.2016`) named the crash host: **`umd12ddi_arden.dll`**, Microsoft's Xbox
D3D12 user-mode driver, on the thread recording Vulkan commands, reading `[0x10]` (fault context:
`state=0x10000` MEM_FREE, `protect=0x1`). Details and the cross-repo evidence:
`03-Memory-CPU-And-JIT/hardware-run-2026-11-10-run4-2016-driver-crash.md`.

The next step is the translator's own reporting and its capability switches
(`DZN_DEBUG=d3d12`, `nobindless`, `no16bit`, `nostencilref` — `dzn_device.c:200-212` at the pinned
Mesa `a76cd3caa23f`). The port forwards `DZN_*`/`MESA_*`/`BB_*` keys from
`LocalState\graphics-debug.txt` (`main.cpp:2412`), **but that file does not exist on the console and
nothing creates it** — the user went looking for it and there was none. The instruction was valid
and unusable.

## What this build changes

1. **`bbport.ini` carries the same variables.** Every `BB_`/`DZN_`/`MESA_` key in `bbport.ini` is
   exported to the environment. Precedence: `graphics-debug.txt` first, then `bbport.ini`, never
   overriding a value that already exists (the launcher's included). Section aliases
   (`[dump] key` → `dump.key`) are skipped so one line cannot be exported twice.
2. **`dzn_debug=<options>`** in `bbport.ini` becomes `DZN_DEBUG=<options>` (empty = off). The
   generated default `bbport.ini` documents it.
3. **`xbox/graphics-debug.sample.txt`** — the ready-to-copy file, whose only active line is
   `DZN_DEBUG=d3d12`. Verified against `BbIni::Parse` with CRLF endings: one variable out, the
   commented examples ignored.

No rendering, memory or reclaim behaviour changes: only which environment variables the process
starts with.

## The run to ask for (no file surgery needed)

Two launches, each one line different in `bbport.ini`:

1. add `dzn_debug=d3d12` under `[memory]` → the D3D12 debug layer reports the invalid call
   **before** the crash instead of after it. Ask for `loader.log.txt` + **`dozen.log`** (the last
   run's `dozen.log` was 150 bytes = the layer was off, so it said nothing).
2. change it to `dzn_debug=d3d12,nobindless,no16bit,nostencilref` → hides three D3D12 capabilities
   the console driver may advertise without supporting. If the crash moves or disappears, the
   report names the capability to avoid.

If the user prefers the old path, `graphics-debug.sample.txt` is that file and the same keys work
there (it still wins over `bbport.ini`).

## What is not claimed

That `d3d12` or any capability switch fixes anything. They are the cheapest experiments that
produce evidence about a crash that is now demonstrably inside a driver we cannot patch. Nothing in
this build changes the game's behaviour when the new keys are left empty — which is the default.
