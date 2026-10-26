// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for the PS4 software-version helpers used by dump layout detection
// (xbox/platform/sfo_version.h). Pure C++: the version a dump is credited with decides whether
// the startup patch set is applied at all, so the rules are pinned down here.

#include "sfo_version.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("ok: %s\n", what.c_str());
    }
}

void CheckEqual(const std::string& actual, const std::string& expected, const std::string& what) {
    Check(actual == expected, what + " (got '" + actual + "', expected '" + expected + "')");
}

void TestContentIdVersion() {
    CheckEqual(BbPrepare::UpdateVersionFromContentId(
                   "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100"),
               "01.09", "patch CONTENT_ID decodes to the update version");
    CheckEqual(BbPrepare::UpdateVersionFromContentId(
                   "UP9000-CUSA00900_00-BLOODBORNE0000000-A0100-V0100"),
               "01.00", "update group A0100 decodes to 01.00");
    CheckEqual(BbPrepare::UpdateVersionFromContentId(
                   "UP9000-CUSA00900_00-BLOODBORNE0000000"),
               "", "base-game CONTENT_ID has no update version");
    CheckEqual(BbPrepare::UpdateVersionFromContentId(
                   "EP9000-CUSA03179_00-BLOODBORNE0000000-A0110-V0100"),
               "01.10", "EP region update group decodes the same way");
    CheckEqual(BbPrepare::UpdateVersionFromContentId(""), "", "empty CONTENT_ID is empty");
    CheckEqual(BbPrepare::UpdateVersionFromContentId(
                   "UP9000-CUSA00900_00-BLOODBORNE0000000-V0100"),
               "", "the trailing V0100 packaging group is never the app version");
    CheckEqual(BbPrepare::UpdateVersionFromContentId(
                   "UP9000-CUSA00900_00-BLOODBORNE0000000-A0109"),
               "01.09", "a patch CONTENT_ID without the packaging group still decodes");
}

void TestAppType() {
    Check(BbPrepare::IsPatchAppType("patch"), "APP_TYPE=patch is an update");
    Check(BbPrepare::IsPatchAppType("PATCH"), "APP_TYPE=PATCH is an update (case insensitive)");
    Check(BbPrepare::IsPatchAppType("gd"), "APP_TYPE=gd is an update");
    Check(BbPrepare::IsPatchAppType("gde"), "APP_TYPE=gde is an update");
    Check(!BbPrepare::IsPatchAppType("gp"), "APP_TYPE=gp is a base game");
    Check(!BbPrepare::IsPatchAppType(""), "an unknown APP_TYPE is treated as a base game");
}

void TestVersionCompare() {
    Check(BbPrepare::CompareVersions("01.09", "01.00") > 0, "01.09 sorts above 01.00");
    Check(BbPrepare::CompareVersions("01.00", "01.09") < 0, "01.00 sorts below 01.09");
    Check(BbPrepare::CompareVersions("01.09", "01.09") == 0, "equal versions compare equal");
    Check(BbPrepare::CompareVersions("1.09", "01.09") == 0, "leading zeros do not matter");
    Check(BbPrepare::CompareVersions("01.10", "01.09") > 0, "01.10 sorts above 01.09 (numeric)");
    Check(BbPrepare::CompareVersions("", "01.00") < 0, "an unreadable version loses every pick");
    Check(BbPrepare::CompareVersions("02.00", "09.00") < 0, "major numbers are compared numerically");
}

void TestPatchTarget() {
    Check(BbPrepare::IsPatchTargetVersion("01.09"), "01.09 is the patch target");
    Check(BbPrepare::IsPatchTargetVersion("1.09"), "1.09 is also the patch target");
    Check(!BbPrepare::IsPatchTargetVersion("01.00"), "01.00 is not the patch target");
    Check(!BbPrepare::IsPatchTargetVersion("01.10"), "01.10 is not the patch target");
    Check(!BbPrepare::IsPatchTargetVersion(""), "an empty version is not the patch target");
}

} // namespace

int main() {
    TestContentIdVersion();
    TestAppType();
    TestVersionCompare();
    TestPatchTarget();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("ps4 version helpers: all checks passed\n");
    return EXIT_SUCCESS;
}
