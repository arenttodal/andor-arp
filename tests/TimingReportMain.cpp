// Prints the scheduler model's measurements as a Markdown table (docs/results/timing-simulation.md).

#include "SchedulerSimulation.h"

#include <cstdio>
#include <string>
#include <vector>

int main()
{
    std::puts ("| Scenario | Ref notes | Matched | Missing | Extra | Mean dev ms | Mean abs ms | p99 abs ms | Max abs ms | Re-anchors | Hanging |");
    std::puts ("|---|---|---|---|---|---|---|---|---|---|---|");
    std::vector<std::string> notes;
    for (const auto& c : andor_arp_test::standardScenarios())
    {
        const auto r = andor_arp_test::simulate (c);
        std::puts (andor_arp_test::formatResult (c, r).c_str());
        for (const auto& u : r.unmatched)
            notes.push_back (c.name + ": unmatched " + u);
    }
    if (! notes.empty())
    {
        std::puts ("\nUnmatched notes (reference or adapter, seconds, MIDI note):\n");
        for (const auto& n : notes)
            std::printf ("- %s\n", n.c_str());
    }
    return 0;
}
