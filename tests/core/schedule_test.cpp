// Time bands decide the limit by the clock; the evaluation is pure so the tests
// do not depend on when they run.
#include "schedule.h"
#include "test_support.h"

using namespace litefetch;
using namespace litefetch::test;

namespace {
void withoutBandsTheBaseApplies()
{
    checkEqual(limitAtMinute({}, 600, 1000), 1000LL, "no bands keeps the base limit");
}

void aBandReplacesTheLimit()
{
    const std::vector<RateBand> bands{{480, 1080, 64 * 1024}}; // 08:00-18:00
    checkEqual(limitAtMinute(bands, 480, 1 << 20), 64LL * 1024, "the band starts at its minute");
    checkEqual(limitAtMinute(bands, 900, 1 << 20), 64LL * 1024, "inside the band");
    checkEqual(limitAtMinute(bands, 1079, 1 << 20), 64LL * 1024, "the last minute is inside");
    checkEqual(limitAtMinute(bands, 1080, 1 << 20), 1LL << 20, "the end minute is outside");
    checkEqual(limitAtMinute(bands, 479, 1 << 20), 1LL << 20, "before the band");
}

void bandsCanWrapMidnight()
{
    const std::vector<RateBand> bands{{1380, 360, 0}}; // 23:00-06:00, unlimited
    checkEqual(limitAtMinute(bands, 1400, 500), 0LL, "late evening is inside");
    checkEqual(limitAtMinute(bands, 120, 500), 0LL, "early morning is inside");
    checkEqual(limitAtMinute(bands, 720, 500), 500LL, "noon is outside");
}

void theFirstMatchWins()
{
    const std::vector<RateBand> bands{{480, 1080, 100}, {600, 660, 10}};
    checkEqual(limitAtMinute(bands, 620, 999), 100LL, "the earlier band wins, so narrow ones go first");
}

void parsingRejectsGarbage()
{
    RateBand band;
    check(parseRateBand("480-1080:65536", band), "a well formed band parses");
    checkEqual(static_cast<long long>(band.startMinute), 480LL, "start");
    checkEqual(static_cast<long long>(band.endMinute), 1080LL, "end");
    checkEqual(band.limitBytesPerSecond, 65536LL, "limit");
    checkEqual(formatRateBand(band), std::string("480-1080:65536"), "the text round trips");

    check(!parseRateBand("480:1080", band), "a missing dash is refused");
    check(!parseRateBand("480-480:1", band), "an empty band is refused");
    check(!parseRateBand("480-2000:1", band), "a minute past midnight is refused");
    check(!parseRateBand("a-b:c", band), "letters are refused");

    const auto many = parseRateBands("480-1080:100\nnoise\n1380-360:0");
    checkEqual(static_cast<long long>(many.size()), 2LL, "only the valid lines survive");
    checkEqual(formatRateBands(many), std::string("480-1080:100\n1380-360:0"),
        "the list round trips");
}
} // namespace

void scheduledActionsFireOnceOnTheirMinute()
{
    const std::vector<ScheduledAction> actions{{480, true}, {1080, false}};
    const auto morning = actionAtMinute(actions, 480);
    check(morning.has_value(), "the morning action is due at its minute");
    check(morning->start, "the morning action starts everything");
    const auto evening = actionAtMinute(actions, 1080);
    check(evening.has_value() && !evening->start, "the evening action pauses everything");
    check(!actionAtMinute(actions, 600).has_value(), "a minute without an entry does nothing");

    // Firing once: the same minute must not run twice.
    check(shouldFire(-1, 480, *morning), "the first time this minute is seen it fires");
    check(!shouldFire(480, 480, *morning), "the same minute never fires again");
    check(!shouldFire(480, 481, *morning), "the action is not due at another minute");

    ScheduledAction parsed;
    check(parseScheduledAction("480:s", parsed), "a start entry parses");
    check(parsed.start && parsed.minute == 480, "the entry keeps its meaning");
    check(formatScheduledAction(parsed) == std::string("480:s"), "the entry round trips");
    check(parseScheduledAction("1080:p", parsed) && !parsed.start, "a pause entry parses");
    check(!parseScheduledAction("1080:x", parsed), "an unknown action is refused");
    check(!parseScheduledAction("1440:s", parsed), "minute 1440 is not a schedule slot");

    const auto many = parseScheduledActions("480:s\n1080:p\nnoise\n1439:p");
    checkEqual(static_cast<long long>(many.size()), 3LL, "only the valid lines survive");
    checkEqual(formatScheduledActions(many), std::string("480:s\n1080:p\n1439:p"),
        "the schedule round trips");
}
int main()
{
    return run("schedule", [] {
        withoutBandsTheBaseApplies();
        aBandReplacesTheLimit();
        bandsCanWrapMidnight();
        theFirstMatchWins();
        parsingRejectsGarbage();
        scheduledActionsFireOnceOnTheirMinute();
        return 0;
    });
}
