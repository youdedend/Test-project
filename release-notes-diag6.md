# diag-6 — `1.0.26314.<build>`: name the addresses we refuse, and the exit we decline

Two diagnostics. Nothing about rendering, memory, the pager or the watchdog's *decisions* changes; this
build should behave like diag-5 plus one or two log lines. Results from the last run, for context:

- turning the debug layer off gave **2,577 presents** (the furthest yet) at roughly **25 fps**, and the
  picture was correct;
- the game still **stalls** and then faults, in `vulkan_dzn.dll+0x20810`, at an address
  `0x23FB16ED5BC` — 1,279 GiB past the end of the guest address space, on a page that was reserved but
  never given memory.

## 1. The address we refuse is now named

`BufferCache::ObtainBuffer` has always refused a guest address outside the 40-bit guest space and
returned the zero buffer instead. It did so **silently**. That is why a run can die at a wild address
with nothing in the log saying an address had already been rejected — the refusal happened, the draw
continued against a zero buffer, and the fault arrived later and somewhere else.

Now it says:

```
GPU [Render.Vulkan] <Warning> ObtainBuffer refused an address outside guest space:
    addr=0x... size=0x... written=0 texel=0 -> zero buffer (guest space ends at 0xffffffffff)
```

Once per distinct address, at most 16 per run (a bad address repeats every frame; a log that repeats
with it is unreadable). **What to look for: the first line like this in a run is the one worth
investigating** — the guest address in it is the value that was already wrong before anything faulted.

## 2. A long stall with memory to spare now says why it did not exit

The watchdog's rule is deliberate: it exits only when presents have been stuck for 10 s **and** the app
is within 150 MiB of its memory limit. Both recent runs froze for 30–45 s with ~660 MiB free, so the
exit was correctly declined — but the log could not tell that apart from a watchdog that had stopped
running. Two runs were read that way, including by me.

Now one line appears per stall, once:

```
UWP watchdog: no clean exit at a 45 s stall with 660 MiB headroom -
    stall reached, but no memory pressure: the exit needs headroom at or below the degrade threshold
```

The wording comes from the same function `Decide()` uses, and eight new checks in
`xbox/tests/watchdog_policy_test.cpp` pin it (all 65 checks pass). If you ever want the app to give up
on a long freeze *regardless* of memory, that is a settings question for a later build, not a bug —
this build only makes the current behaviour visible.

## How to read the next run

1. Play as usual, into the part that hangs.
2. Send `loader.log.txt` and `errors.log.txt` (and `dozen.log` only if it is big — if it is 150 bytes,
   the debug layer is off, which is what we want).
3. What matters, in order:
   - **`ObtainBuffer refused`** — if it appears, send that line first; it names the bad address before
     the fault.
   - **`no clean exit`** — tells us the stall was real and long, without a second run to confirm it.
   - the fault line, which now always carries `module=` and `base=`.

## What is *not* in this build

No fix for the stall and no fix for the fault. Both are now instrumented instead of guessed: the next
run should name the address that goes wrong before the crash, and confirm the stall's length and
character. Guessing at a fix before that would be the diag-3 mistake again.
