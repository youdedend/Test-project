// SPDX-License-Identifier: GPL-2.0-or-later
// BUILD 3, item 15: the recovery watchdog's rules, as plain arithmetic.
//
// The watchdog is the answer to a shape both `0717` and `819` had: the app sat at its limit for
// seconds, the collector ran, the pager ran, and nothing was logged between "we are at the limit"
// and the allocation failure - so the run could not say which step was reached or what each one
// bought. The rules live here (no threads, no Win32, no Vulkan) so they can be tested on the build
// machine: xbox/tests/watchdog_policy_test.cpp.
//
// Everything is in bytes and milliseconds. The step *order* is the part that has to be right: a
// smaller headroom must never run a cheaper step, and a stall that is already at the exit threshold
// must not degrade the resolution first and then find out there is nothing left to try.
#pragma once

#include <cstdint>

namespace BbWatchdog {

/// What the watchdog does when a threshold is crossed. Numbered by escalation so a caller can log
/// the step and a test can assert the order; `StepNone` is "nothing to do".
enum Step : int {
    StepNone = 0,
    /// (a) within `ladder_bytes` of the limit: release retired arenas, trim staging, emergency
    /// reclaim of images. The cheap steps: nothing the player can see changes.
    StepLadder = 1,
    /// (b) within `pager_bytes`: force a pager sweep (including the compressed pool). Costs
    /// page-out time on the guest's threads, so it is only reached once (a) was not enough.
    StepPager = 2,
    /// (c) within `degrade_bytes`, or presents stuck for `stuck_degrade_ms`: switch optional
    /// features off at run time (upscaler, frame generation, optional buffers) and lower the
    /// internal resolution. Visible, so it is the last step before giving up.
    StepDegrade = 3,
    /// (d) presents stuck for `stuck_exit_ms` while at the limit: dump state, flush saves and exit
    /// cleanly with a reason, instead of being killed by the console mid-save.
    StepExit = 4,
};

/// Thresholds, in the units the console reports. Defaults are the plan's numbers; every one can be
/// overridden from `bbport.ini` (item 15 asks for that explicitly).
struct Thresholds {
    bool enabled = true;
    std::uint64_t ladder_bytes = std::uint64_t{400} << 20;
    std::uint64_t pager_bytes = std::uint64_t{250} << 20;
    std::uint64_t degrade_bytes = std::uint64_t{150} << 20;
    std::uint64_t stuck_degrade_ms = 3000;
    std::uint64_t stuck_exit_ms = 10000;
    /// Minimum time between two steps. Each step costs milliseconds-to-seconds on the guest's
    /// threads; running one per second while the app hovers at a threshold is how a watchdog
    /// becomes the stall it was meant to survive.
    std::uint64_t cooldown_ms = 2000;
};

/// `headroom` is `limit - usage` (0 when the limit is unknown, which disables the byte thresholds:
/// a console that does not report a limit must not be degraded on arithmetic).
/// `stuck_ms` is how long the present counter has been unchanged while the app is visible, 0 when it
/// is moving or the app is in the background.
[[nodiscard]] inline Step Decide(std::uint64_t headroom, std::uint64_t stuck_ms,
                                 const Thresholds& thresholds) {
    if (!thresholds.enabled) {
        return StepNone;
    }
    // The exit is decided first, and it needs both halves: a 10 s stall *and* no headroom. A 10 s
    // stall with memory to spare is a bug the stall report should still be able to catch, and
    // exiting would take the log's evidence with it; a run that is at its ceiling and has stopped
    // presenting is the one that will be killed mid-save if nothing does it cleanly.
    if (stuck_ms >= thresholds.stuck_exit_ms && headroom <= thresholds.degrade_bytes) {
        return StepExit;
    }
    if (stuck_ms >= thresholds.stuck_degrade_ms) {
        return StepDegrade;
    }
    if (headroom <= thresholds.degrade_bytes) {
        return StepDegrade;
    }
    if (headroom <= thresholds.pager_bytes) {
        return StepPager;
    }
    if (headroom <= thresholds.ladder_bytes) {
        return StepLadder;
    }
    return StepNone;
}

/// True when `now_ms - last_step_ms` has reached the cooldown. `last_step_ms == 0` (never stepped)
/// always passes: the first crossing is not delayed.
[[nodiscard]] inline bool CooldownElapsed(std::uint64_t now_ms, std::uint64_t last_step_ms,
                                         const Thresholds& thresholds) {
    if (last_step_ms == 0) {
        return true;
    }
    if (now_ms < last_step_ms) {
        return true; // the clock went backwards (a wrap or a re-init): do not stall the watchdog
    }
    return now_ms - last_step_ms >= thresholds.cooldown_ms;
}

/// Steps that are worth repeating every cooldown while the condition holds: (a) and (b) find more
/// each time memory becomes available. (c) and (d) do not - the features are already off, and the
/// exit happens once - so they run only when they are a new, higher step than the last one.
[[nodiscard]] inline bool Repeatable(Step step) {
    return step == StepLadder || step == StepPager;
}

/// Whether this step should run now, given the last one that ran.
///
/// (a) and (b) are gated by the cooldown and repeat: each run may find memory the previous one
/// could not. (c) and (d) are one-shot escalations: the features are already off, and the exit
/// happens once. An escalation above the last step is *not* held back by the cooldown - waiting
/// before the exit step is how a watchdog would spend the exit threshold on a resolution change.
[[nodiscard]] inline bool ShouldRun(Step step, Step last_step, std::uint64_t now_ms,
                                    std::uint64_t last_step_ms, const Thresholds& thresholds) {
    if (step == StepNone) {
        return false;
    }
    if (!Repeatable(step)) {
        return step > last_step;
    }
    return CooldownElapsed(now_ms, last_step_ms, thresholds);
}

/// `key=value` -> bytes, for `*_mb` ini settings. Returns `fallback` for anything unparseable, and
/// clamps to [min_bytes, max_bytes] so a typo cannot disable a step or make it fire immediately.
[[nodiscard]] inline std::uint64_t ParseMiB(const char* text, std::uint64_t fallback,
                                            std::uint64_t min_bytes, std::uint64_t max_bytes) {
    if (text == nullptr || text[0] == '\0') {
        return fallback;
    }
    std::uint64_t value = 0;
    bool any = false;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            break;
        }
        value = value * 10 + std::uint64_t(*p - '0');
        any = true;
    }
    if (!any) {
        return fallback;
    }
    const std::uint64_t bytes = value << 20;
    if (bytes < min_bytes) {
        return min_bytes;
    }
    return bytes > max_bytes ? max_bytes : bytes;
}

/// `key=value` -> milliseconds, same rules.
[[nodiscard]] inline std::uint64_t ParseMs(const char* text, std::uint64_t fallback,
                                           std::uint64_t min_ms, std::uint64_t max_ms) {
    if (text == nullptr || text[0] == '\0') {
        return fallback;
    }
    std::uint64_t value = 0;
    bool any = false;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            break;
        }
        value = value * 10 + std::uint64_t(*p - '0');
        any = true;
    }
    if (!any) {
        return fallback;
    }
    if (value < min_ms) {
        return min_ms;
    }
    return value > max_ms ? max_ms : value;
}

/// The step's name, for the log line each step prints.
[[nodiscard]] inline const char* StepName(Step step) {
    switch (step) {
    case StepLadder: return "release (arenas, staging, images)";
    case StepPager: return "pager sweep";
    case StepDegrade: return "degrade (features off, resolution down)";
    case StepExit: return "exit";
    case StepNone: break;
    }
    return "none";
}

} // namespace BbWatchdog
