// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for the recovery watchdog's rules (xbox/platform/watchdog_policy.h).
//
// The watchdog is what a run at the app's memory limit relies on, and its failure modes are quiet:
// a threshold parsed a factor of 1024 wrong, a step that repeats every second and becomes the stall
// it was meant to survive, a cooldown that delays the exit step behind a resolution change, or an
// order that runs the pager before the cheap release. None of those crash, so none of them is caught
// anywhere else.
#include "watchdog_policy.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;
constexpr unsigned long long MiB = 1ull << 20;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("ok: %s\n", what.c_str());
    }
}

void TestThresholdsAreBytes() {
    // The plan's numbers: 400 / 250 / 150 MiB and a 1 s check with a ~2 s cooldown per step.
    const BbWatchdog::Thresholds defaults;
    Check(defaults.ladder_bytes == 400 * MiB, "the ladder threshold is 400 MiB");
    Check(defaults.pager_bytes == 250 * MiB, "the pager threshold is 250 MiB");
    Check(defaults.degrade_bytes == 150 * MiB, "the degrade threshold is 150 MiB");
    Check(defaults.stuck_degrade_ms == 3000, "presents stuck 3 s degrades");
    Check(defaults.stuck_exit_ms == 10000, "presents stuck 10 s exits");
    Check(defaults.cooldown_ms == 2000, "each step is at least 2 s apart");
}

void TestStepOrder() {
    const BbWatchdog::Thresholds t;
    // Plenty of room: nothing runs. A watchdog that reclaims while the app is calm is a stutter.
    Check(BbWatchdog::Decide(2000 * MiB, 0, t) == BbWatchdog::StepNone,
          "2,000 MiB of headroom: no step");
    Check(BbWatchdog::Decide(400 * MiB + 1, 0, t) == BbWatchdog::StepNone,
          "just above the ladder threshold: no step");
    Check(BbWatchdog::Decide(400 * MiB, 0, t) == BbWatchdog::StepLadder,
          "at the ladder threshold: the cheap release runs");
    Check(BbWatchdog::Decide(150 * MiB + 1, 0, t) == BbWatchdog::StepPager,
          "between pager and degrade: the pager sweeps, not the release");
    Check(BbWatchdog::Decide(150 * MiB, 0, t) == BbWatchdog::StepDegrade,
          "at the degrade threshold: features go off");
    Check(BbWatchdog::Decide(1 * MiB, 0, t) == BbWatchdog::StepDegrade,
          "at the limit with presents moving: still degrade, never exit on arithmetic alone");
    Check(BbWatchdog::Decide(0, 0, t) == BbWatchdog::StepDegrade,
          "a limit of zero headroom is not an exit");

    // A stall escalates on its own, whatever the headroom says.
    Check(BbWatchdog::Decide(2000 * MiB, 3000, t) == BbWatchdog::StepDegrade,
          "presents stuck 3 s degrades even with memory to spare");
    Check(BbWatchdog::Decide(2000 * MiB, 10000, t) == BbWatchdog::StepDegrade,
          "a 10 s stall with memory to spare degrades but does not exit");
    Check(BbWatchdog::Decide(100 * MiB, 10000, t) == BbWatchdog::StepExit,
          "the exit outranks the degrade at the limit");
    Check(BbWatchdog::Decide(100 * MiB, 2999, t) == BbWatchdog::StepDegrade,
          "one millisecond short of the stall threshold does not exit");
}

void TestDisabledAndUnknownLimit() {
    BbWatchdog::Thresholds t;
    t.enabled = false;
    Check(BbWatchdog::Decide(0, 60000, t) == BbWatchdog::StepNone,
          "watchdog=0 disables every step, including the exit");
    // The caller passes headroom 0 when the console reports no limit; that must not read as "no
    // memory left". The pager step is the cheapest thing that cannot hurt, so it is what remains.
    BbWatchdog::Thresholds live;
    Check(BbWatchdog::Decide(0, 0, live) != BbWatchdog::StepExit,
          "an unknown limit never exits the app");
}

void TestCooldown() {
    const BbWatchdog::Thresholds t;
    Check(BbWatchdog::CooldownElapsed(5000, 0, t), "the first step is never delayed");
    Check(!BbWatchdog::CooldownElapsed(6000, 5000, t), "1 s after a step is inside the 2 s cooldown");
    Check(BbWatchdog::CooldownElapsed(7000, 5000, t), "2 s after a step the next one may run");
    Check(BbWatchdog::CooldownElapsed(10, 5000, t), "a clock that went backwards does not stall it");
}

void TestShouldRun() {
    const BbWatchdog::Thresholds t;
    using namespace BbWatchdog;
    // Nothing to do.
    Check(!ShouldRun(StepNone, StepNone, 10000, 0, t), "no step runs for 'none'");
    // The release repeats, once per cooldown: each run may find arenas that were in flight before.
    Check(ShouldRun(StepLadder, StepLadder, 10000, 0, t), "the release runs the first time");
    Check(!ShouldRun(StepLadder, StepLadder, 11000, 10000, t), "it does not repeat inside the cooldown");
    Check(ShouldRun(StepLadder, StepLadder, 12000, 10000, t), "it repeats after the cooldown");
    // The pager likewise.
    Check(ShouldRun(StepPager, StepPager, 30000, 20000, t), "the pager sweep repeats after the cooldown");
    // Escalation is not delayed: a run that is about to be killed must not wait for a cooldown.
    Check(ShouldRun(StepDegrade, StepLadder, 10001, 10000, t),
          "degrade follows the release immediately");
    Check(ShouldRun(StepExit, StepDegrade, 10001, 10000, t),
          "the exit follows the degrade immediately");
    // One-shot: the features are already off, and the exit happens once.
    Check(!ShouldRun(StepDegrade, StepDegrade, 100000, 0, t), "degrade does not repeat");
    Check(!ShouldRun(StepExit, StepExit, 100000, 0, t), "the exit does not repeat");
    // And a lower one-shot step never re-runs over a higher one.
    Check(!ShouldRun(StepDegrade, StepExit, 100000, 0, t), "a lower one-shot step never re-runs");
}

void TestParsing() {
    using namespace BbWatchdog;
    // `bbport.ini` spellings, and the typos a player makes.
    Check(ParseMiB("400", 1, 64 * MiB, 4096 * MiB) == 400 * MiB, "400 -> 400 MiB");
    Check(ParseMiB("  400", 1, 64 * MiB, 4096 * MiB) == 1, "a leading space is not a number: fallback");
    Check(ParseMiB("", 7 * MiB, 64 * MiB, 4096 * MiB) == 7 * MiB, "an empty value takes the fallback");
    Check(ParseMiB(nullptr, 7 * MiB, 64 * MiB, 4096 * MiB) == 7 * MiB, "a missing key takes the fallback");
    Check(ParseMiB("abc", 7 * MiB, 64 * MiB, 4096 * MiB) == 7 * MiB, "a word takes the fallback");
    Check(ParseMiB("16", 7 * MiB, 64 * MiB, 4096 * MiB) == 64 * MiB, "below the floor it clamps up");
    Check(ParseMiB("999999", 7 * MiB, 64 * MiB, 4096 * MiB) == 4096 * MiB, "above the ceiling it clamps down");
    Check(ParseMiB("400mb", 1, 64 * MiB, 4096 * MiB) == 400 * MiB, "a trailing unit is ignored");

    Check(ParseMs("3000", 1, 200, 60000) == 3000, "3000 -> 3000 ms");
    Check(ParseMs("10", 1, 200, 60000) == 200, "below the floor it clamps up");
    Check(ParseMs("999999", 1, 200, 60000) == 60000, "above the ceiling it clamps down");
    Check(ParseMs("", 2000, 200, 60000) == 2000, "an empty value takes the fallback");
}

void TestThresholdsFromIni() {
    // The four keys item 15 asks for, read the way the watchdog reads them.
    BbWatchdog::Thresholds t;
    t.ladder_bytes = BbWatchdog::ParseMiB("512", t.ladder_bytes, 64 * MiB, 2048 * MiB);
    t.pager_bytes = BbWatchdog::ParseMiB("300", t.pager_bytes, 64 * MiB, 2048 * MiB);
    t.degrade_bytes = BbWatchdog::ParseMiB("200", t.degrade_bytes, 32 * MiB, 1024 * MiB);
    t.cooldown_ms = BbWatchdog::ParseMs("500", t.cooldown_ms, 200, 30000);
    Check(t.ladder_bytes == 512 * MiB && t.pager_bytes == 300 * MiB && t.degrade_bytes == 200 * MiB,
          "the three byte thresholds come from the ini");
    Check(t.cooldown_ms == 500, "so does the cooldown");
    Check(BbWatchdog::Decide(180 * MiB, 0, t) == BbWatchdog::StepDegrade,
          "with 200 MiB for degrade, 180 MiB of headroom degrades");
}

} // namespace

int main() {
    TestThresholdsAreBytes();
    TestStepOrder();
    TestDisabledAndUnknownLimit();
    TestCooldown();
    TestShouldRun();
    TestParsing();
    TestThresholdsFromIni();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("watchdog policy: all checks passed\n");
    return EXIT_SUCCESS;
}
