# diag-3 — the crash moved, and the reports now name what they see (publish notes)

Prepared by the research session, 2026-11-10. Tip `e2de023` over the published diag-1 tip
`f531b31` (`xbox-alpha-1.0.26314.1903`). This supersedes the diag-2 notes: the bundle contains
**both** diag-2 (the render-thread reclaim fix) and diag-3 (the diagnostics). Publish with
`NEW-CHAT-RUNBOOK.md` v5.

## What the diag-2 run proved (build `1.0.26314.1946`, `fef8bc4f`)

The user: *"now i can see character but crash when gameplay suppose to start"*.

- **The dzn crash is gone.** No `vulkan_dzn.dll+0x1fd79` fault anywhere in `loader-2.log` — the
  fault both 26314 builds died on, in four consecutive runs before it.
- **The fix ran and is visible:** `loader-2.log:954`
  `Texture cache: render-thread reclaim of the watchdog's request freed 76 MiB of 112 MiB asked
  (76 MiB in total; running it here is diag-2)`.
- **diag-1's staging row now reads properly:** `ring=272 MiB in 17 block(s), resident=272 MiB`
  (it was `0 MiB` for the whole of the previous build).
- **Progress:** 2,326 presents (2,176 before), character creation completed, world load began.

## What it then hit

A **stall**, then a **different** crash:

```
UWP run curve: ... t=68s presents=2325     <- last movement
UWP STALL DETECTED (presents stuck at 2326 for 10 s):
UWP thread op: tid 5924 in 'recording Vulkan commands' for 785 ms
UWP thread op: tid 1028 in 'processing a PM4 submission' for 1262 ms
fences: submitted=4649 completed=4648 outstanding=1   (age 39,961 ms, i.e. 40 s)
...
  [73969 ms] recovery: degraded
UNHANDLED NATIVE FAULT: code=0xc0000005 module=unknown+0x23ec55 rip=0x7ffac2edec55
                       rsp=0x899bff350 addr=0x1429f5fed70
```

Two facts about that line: the module is **unknown** to the filter, and the fault address points
into a **private allocation** (0x1429_f5fed70 is not in any image). Note also that the *previous*
build's fault came one second after the same `recovery: degraded` breadcrumb.

## What diag-3 changes (no guesswork, only reporting)

1. **`module=unknown` cannot happen again.** `ModuleNameForAddress()` enumerates the process's
   modules and names whichever one contains an address — for the faulting rip *and* for the fault
   address. A new line follows the fault:
   `UNHANDLED FAULT CONTEXT: addr is in <module|a private allocation>, protect=... state=... type=...`
   and then a full thread report, so the state of every other thread at the moment of death is in
   the log (1946 died during a watchdog step and nothing recorded that).
2. **The stall dump names the command, not just the thread.** `CommandChunk::Execute` tags the
   command index and its vtable pointer (throttled to one tag per second, so it costs nothing per
   command), and `DrawRecord`/`DispatchRecord` register what they are drawing
   (`UWP thread draw: tid N graphics #123 marker=indexed`).
3. **A stuck fence dumps on its own.** Previously the stall report required the *presenter* to
   stop; 1946's fence stood still for 40 s with the presenter already stopped at 2,326, and one
   dump was taken by luck. Fence age > 3 s now triggers the dump after a single tick and the
   report says which condition fired.

## What is NOT claimed

Nothing is claimed about the new crash's cause. The two candidate explanations both need the
diag-3 report to be decided:

- **a kernel read from paged-out guest memory** — the run paged 1,020 MiB of guest memory out and
  the fault address is private memory, so the address may have been valid when the faulting thread
  read it and paged out underneath the read. `UWP swap: ... checks=17685 corrupt=0 window
  evictions=12220` in the same run is suspicious;
- **a stale object in host code**, which the first explanation would only look like because the
  crash happened while the watchdog was degrading features.

Either way, the diag-3 report says *which module* and *which thread*, which is what the next step
needs.

## The run to ask for

Same route: intro → character creation → confirm → into the world. Ask for `loader.log.txt` +
`errors.log.txt` only. Two optional extras that would each settle a question:

- `swap_file=off` in `bbport.ini` (`[memory]`): the same route with the pager disabled tells us in
  one run whether paging participates in the crash. Only if the user is willing to run a build
  twice.
- `DZN_DEBUG=d3d12` in `graphics-debug.txt`: adds the dzn debug output to `dozen.log`.
