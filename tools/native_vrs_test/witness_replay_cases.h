#pragma once
#include "../../src/d3d11/native_vrs_replay.h"

inline void witnessReplayCases(void (*check)(bool, const char*)) {
    using namespace edvr::native_vrs;
    using Answer = WitnessCohortAppend;
    int objects[5]{};
    const WitnessCohortKey key{&objects[0], &objects[1], &objects[2], &objects[3], 1, 2, 3, 0};
    const WitnessDraw commands[] = {
        WitnessDraw::vertices(7, 5),
        WitnessDraw::indexed(9, 11, -13),
        WitnessDraw::instanced(15, 17, 19, 21),
        WitnessDraw::indexedInstanced(23, 25, 27, -29, 31),
    };
    WitnessCohort cohort;
    check(cohort.empty(), "no pending replay before the first draw");
    for (const auto& draw : commands)
        check(cohort.append(key, draw) == Answer::Appended, "continuous proven draw admitted");

    auto changed = key;
    changed.inputs++;
    check(cohort.append(changed, commands[0]) == Answer::Drain, "new buffer contents must drain old inputs");
    changed = key;
    changed.pipeline++;
    check(cohort.append(changed, commands[0]) == Answer::Drain, "new pipeline must drain old state");
    changed = key;
    changed.state = &objects[4];
    check(cohort.append(changed, commands[0]) == Answer::Drain, "another captured state must drain");
    changed = key;
    changed.frame++;
    check(cohort.append(changed, commands[0]) == Answer::Drain, "new frame cannot inherit a pending cohort");
    changed = key;
    changed.context = &objects[4];
    check(cohort.append(changed, commands[0]) == Answer::Drain, "other owner cannot overtake pending draws");
    changed = key;
    changed.eye = 1;
    check(cohort.append(changed, commands[0]) == Answer::Drain, "eye boundary must drain");
    changed = key;
    changed.depthTarget = &objects[4];
    check(cohort.append(changed, commands[0]) == Answer::Drain, "new depth target must drain");
    changed = key;
    changed.inputs = 0;
    check(cohort.append(changed, commands[0]) == Answer::Drain, "unproven input cannot bypass pending draws");
    check(cohort.append(key, {}) == Answer::Drain, "unsupported draw cannot bypass pending draws");
    check(cohort.size() == 4, "rejected appends leave the original commands pending");

    struct Samples {
        unsigned n = 0;
        bool exact = true;
        void Draw(uint32_t c, uint32_t s) { exact &= n++ == 0 && c == 7 && s == 5; }
        void DrawIndexed(uint32_t c, uint32_t s, int32_t b) { exact &= n++ == 1 && c == 9 && s == 11 && b == -13; }
        void DrawInstanced(uint32_t c, uint32_t i, uint32_t s, uint32_t j) {
            exact &= n++ == 2 && c == 15 && i == 17 && s == 19 && j == 21;
        }
        void DrawIndexedInstanced(uint32_t c, uint32_t i, uint32_t s, int32_t b, uint32_t j) {
            exact &= n++ == 3 && c == 23 && i == 25 && s == 27 && b == -29 && j == 31;
        }
    } samples;
    for (unsigned i = 0; i < cohort.size(); ++i) issueWitnessDraw(samples, cohort.data()[i]);
    check(samples.exact && samples.n == 4, "direct draw arguments and order survive replay");
    cohort.retire();
    check(cohort.empty(), "operator retires only after all draws issue");
    check(cohort.append(changed, commands[0]) == Answer::Refused, "unproven inputs decline with no pending replay");
    check(cohort.append(key, {}) == Answer::Refused, "unknown or zero draw declines with no pending replay");
    bool filled = true;
    for (unsigned i = 0; i < WitnessCohort::capacity; ++i)
        filled &= cohort.append(key, commands[1]) == Answer::Appended;
    check(filled && cohort.size() == 64, "direct replay memory bounded at 64 draws");
    check(cohort.append(key, commands[2]) == Answer::Drain &&
          cohort.size() == 64 && cohort.data()[63].baseVertex == -13,
          "the next draw cannot overflow or overwrite pending work");
}
