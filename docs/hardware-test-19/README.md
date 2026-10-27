# Console log — still build `.559`: the same disk-full stop, with the pager working harder

Sent 2026-10-27. **This is not `.623`.** The pager's start line has no `disk budget=` field, which
`.623` added, so it is the previous build — the stop below is the one `.623` was written for.

| File | What it is |
|---|---|
| `loader.log.txt` | 1,011 lines: telemetry, pager reports, a 10 s stall dump |
| `errors.log.txt` | GPU/`stderr` side, ending in `STOP: UWP swap private write …, error 112` |

## What it adds to `.559`'s own run

| Measurement | previous `.559` run | this run |
|---|---|---|
| swapped out before the stop | 1,016 MiB | **1,252 MiB** |
| `pageable private` at the stop | 881 MiB | 619 MiB (still paging when the disk filled) |
| page-out passes / nothing to give | 260 / 0 | **347 / 0** |
| `guest pinned` | 0 MiB | 0 MiB |
| stall | 10 s at `presents=829` | 10 s at `presents=815` |

`corrupt=0` and `checks=420`: every page that went to disk came back verified, so the pager's data
path is sound. What ends the run is `ERROR_DISK_FULL` on a private write, which `.623` turns into
"leave the page resident and carry on" plus a 768 MiB budget.

The repetition also shows the failure is not a one-off volume-state fluke: two builds, two runs,
~1.0-1.25 GiB of swap file, the same stop, at the same scene.
