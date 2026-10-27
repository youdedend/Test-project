# The precompile crash at item 1025 — what the community says

Research round, 2026-10-27. Question: the "Precompile shaders" step in the Xbox UWP port dies on the
item after 1024 of 10256 (last line `control_flow_graph.cpp:407 RemoveUnreachableBlocks: Removing
unreachable block begin=0x27c`), and has to be relaunched. Has anyone else hit this, and how did they
get past it?

Sources are public: shadPS4 issues/PRs, the sibling Bloodborne "bbport" repositories, and
r/BloodbornePC. Everything below is quoted or paraphrased from those; nothing here is a guess about
what our own code does (that is in `handoff.md` §2c).

## 1. Nobody reports this exact crash

Searched (GitHub issue/PR search API + web):

| Query | Hits about this crash |
|---|---|
| `"DXIL cached"`, `"Precompiling Bloodborne shaders"`, `"driver.vkcache"` | none |
| `"Shader precompile"`, `"pipelines preloaded"` (fingerprints of our own log lines) | only `visy/bloodborne_pc_windows_port` #20, which is a Linux launch issue, no precompile failure |
| `precompile` in `visy/…`, `Mrsuss60/…`, `Jgamrot/…`, `deadinside28/…`, `ayozetr/…`, `bmy/bbport-mac`, `StanTheMackiar/bloodborne-android-port` | no crash reports for the precompile step |
| `"RemoveUnreachableBlocks"`, `"kMaxGotoTransforms"`, `GotoPass` in shadPS4 issues/PRs | none |
| `repo:shadps4-emu/shadPS4 Bloodborne precompile` | none — upstream shadPS4 has no precompile feature at all |

So this is ours: the bulk sweep over every shader extracted from the game's `.shf` archives is a
feature of this port family, and the other ports that have it (`Jgamrot`, `visy`, `Mrsuss60`) run it
to completion on the same game. That is the most useful signal in the whole search: **the sibling
ports precompile Bloodborne without dying**, so the failure is specific to how we do it, not to
Bloodborne's shaders being uncompilable.

## 2. How the sibling ports do the same job differently

### `Jgamrot/bloodborne_pc` — a *catalog* of shaders the game is known to use

> "Shader pre-build: shaders the game is known to use are built while it starts (*Preparing shaders*
> in the window title) instead of stuttering the first time an area or effect appears. The included
> catalog covers the game up to the Blood-starved Beast so far; it lists shaders, not game code, and
> each install builds them from its own files. Off with `BB_SHADER_PREBUILD=0`."

It does **not** compile everything in the archives. It compiles a list of shaders that were observed
being used, and the list is incomplete on purpose ("Areas and effects beyond the shader catalog still
stutter briefly the first time").

Reddit, r/BloodbornePC, `1x2qx0g` (the launcher announcement):

> "Less shader stutter: shaders the game uses are now built while the game starts ("Preparing
> shaders" in the title bar) instead of mid-fight. On a fresh install, Central Yharnam went from 479
> shaders compiled while playing to 2."

### `visy/bloodborne_pc_windows_port` — compile the *recorded* cache, not the archives

`docs/SHADERS.md`:

> "The port records each pipeline the game uses in `user\cache\CUSA03173\` and: **Precompiles the
> cache before the intro** (`BB_SHADER_PRECOMPILE`, default on): all cores build every cached pipeline
> before the logos… Later launches reuse the driver's own disk cache and are quick."

and, the sentence that matters most here:

> "**Shaders the game has never used on any machine cannot be prepared up front: a pipeline depends on
> draw state the game only sets while running.** A shared cache from a player who visited more areas
> covers them."

The same document describes the shader sources being kept next to the compiled shaders:

> "Next to the compiled shaders it keeps each shader's source (`.src` files: the game's shader code
> and the inputs of its translation). When the cache was made for another GPU (or by a build with a
> changed shader translator), the first launch translates every shader again from these sources…
> `BB_SHADER_CACHE_SELFTEST=1` translates each newly compiled shader a second time from its stored
> source and logs any difference."

### `Awakenchan/LanternEngine` (another port, same game) — a catalog plus a save bug

Release notes:

> "the shader catalog that ships with the engine was being ignored on every PC since 0.9.34 beta
> 9.9, so every shader was built during play. It is fixed and rebuilt: on an NVIDIA GPU about 510
> pipelines are now built before the game starts."

> "A bug in how shader records were saved made most of them unreadable."

~510 pipelines, not 8,207 archive programs. The numbers are the point: what the game actually uses is
one to two orders of magnitude smaller than what the archives contain.

### What this implies for us

Our sweep compiles **8,207 extracted CS/PS/VS programs + 2,048 linked VS+PS pairs = 10,256 items**
with synthetic register state, on the theory that any of them might be needed. The sibling ports
converged on the opposite theory: compile only what the game has been observed to use, and let
everything else compile lazily at draw time with the real draw state.

If the sibling model is right, item 1025 is a shader **variant the game never asks for** — a
combination our synthetic arena can produce that the game does not. A crash there costs the whole
precompile run but would never have cost a stutter in play, because the variant never runs.

## 3. Related crashes in shadPS4 that *were* root-caused (the general shape)

| Where | What it was | How it was settled |
|---|---|---|
| shadPS4 #4292 Uncharted: AV in `GpuComm` right after `CompileModule: Compiling cs shader 0x43b8ee5e` | a per-game setting (`directMemoryAccess`), not the compiler | maintainer: "given the shader that's failing to compile, that's likely your issue"; enabling it stopped the crash |
| shadPS4 #321 "Crash in `Shader::Gcn::CFG::EmitBlocks()`, out of bounds list access" | frontend bug in block splitting | fixed by an unrelated earlier fix (#497); the loop was fine |
| `axylpup/shadPS4` #5 (Radeon RX 5500 XT) | one generated shader contained a zero-length list; AMD's driver divided by it and crashed | find and fix the emission — "NVIDIA's driver ignores that, and yours divides by it" |
| `axylpup/shadPS4` #25 (RX 570) | the driver refused one generated pipeline and the emulator closed | **fail open**: "The next version no longer closes when the driver refuses one of the faster shader versions: that object is drawn the normal way instead, and the log names the shader so I can look at it." |
| `axylpup/shadPS4` #17 "Shader compilation stuck at 56%, 17441 completed" (same stuck % on every boot) | not the compiler — Windows thread data read under Wine | bisected with one-setting-at-a-time flags (`SPINLOCK_PARK`, `WRITE_FAULT_OFFLOAD`, `A2`) |
| `boykopovar/AnyPS5` #9702 | structurization of one 4,750-block graph took 177 s and tripped a watchdog | algorithmic fix in the structurizer (bitsets/binary search, dominator trees), verified by dumping the graph and **replaying it offline** (`agc_shader_replay`) |

Two habits run through all of them:

1. **Name the shader, then fail open.** No project in this space lets one pipeline take the process
   down if it can be avoided. (This is what §2c added to our build.)
2. **Replay one shader offline.** Every real root-cause above needed the offending shader in
   isolation — dumped source/registers, or a CFG dump replayed through the structurizer alone.

## 4. Options that follow from the research

Ordered by how well the evidence supports them.

### Route A — precompile only what the game uses (the sibling ports' model)

Stop sweeping all 10,256 archive items by default. Keep the startup "warm the recorded pipeline
cache" step (the 65/65 pipelines we already warm), and make the archive sweep a separate, explicitly
chosen "deep precompile", with a **catalog**: the set of shader hashes the game has actually drawn,
recorded during play (we already log `Compiling cs/fs/vs shader 0x…` at draw time, so the data
exists), stored next to the cache and shipped/pickable like `visy`'s exportable cache.

*Evidence:* `Jgamrot` catalog (works, ships), `visy` "cannot be prepared up front… a pipeline depends
on draw state the game only sets while running" (works, documented), `LanternEngine` ~510 pipelines.
*Effect on the crash:* a fabricated variant is never compiled, so item 1025 cannot stall the run.
*Cost:* a shader the game uses but the catalog never saw compiles lazily once — the stutter the
feature exists to avoid, for that one shader.

### Route B — keep the sweep, but make one bad shader cost nothing

What §2c started, pushed to its conclusion: per-shader time budget, dump each item's source before
compiling it (journal), resume from the last completed item instead of recompiling from 0, and a
visible "N skipped, quarantined" list with a button to re-try them. The sweep stays complete-by-design
and a poisoned shader costs seconds, not a relaunch.

*Evidence:* `axylpup` #25 fail-open + "the log names the shader"; `visy`'s `.src` dump + self-test;
`AnyPS5`'s replay tool.
*Effect on the crash:* the crash still happens, but only once, and the victim is identified.
*Cost:* more code in the precompile path; a shader that dies the process still costs one relaunch if
it is not an access violation.

### Route C — replay the shader offline first (diagnosis, no behaviour change)

Journal every item's code + synthetic registers during the sweep, then build a tiny host tool that
feeds one record through `TranslateProgram`/`EmitSPIRV` and, on a debug build, dumps the CFG dot
(`CFG::Dot()` already exists). This is how `AnyPS5` found the 177-second structurization.

*Effect on the crash:* none directly; it turns item 1025 into a reproducible input, so the real fix
can be written and tested without the console.
*Cost:* small, and it is the prerequisite for fixing the underlying frontend bug rather than routing
around it.

A and C are complementary; A is the one that makes the crash stop happening on the console, C is the
one that finds out whether the port has a genuine recompiler bug worth reporting upstream.

## 5. What the search rules out

- **Not a known shadPS4 bug we can cherry-pick.** Upstream `control_flow_graph.cpp` still has the
  same `RemoveUnreachableBlocks` shape (our version is *more* thorough: reachability from the entry
  rather than a single-pass predecessor count), and no upstream issue or PR reports this failure. The
  frontend in this tree also carries our own hardening (goto-transform cap, tree guards, `VisitExpr`
  depth limit) that upstream does not have — so "update from upstream to fix it" is not available.
- **Not a driver-version problem in the usual sense.** Every "crashes after compiling shaders" report
  in the wild is either a missing VC++ redistributable, a wrong dump/update layout, or a per-game
  setting; none of them match a deterministic stop at a fixed item number.
- **Not evidence of a broken dump.** 65/65 recorded pipelines warm successfully and the executable's
  own shaders are not involved; the run dies inside the recompiler on archive content, before any
  game code runs.
