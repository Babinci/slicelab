#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace slicelab {

struct Marker {
    double seconds = 0;
    juce::String name;
};

// Parse UTF-8 CSV or TSV text, accepting an optional seconds/name header.
// The result is sorted by source time. On any error, output is unchanged.
juce::Result parseMarkers(const juce::String& text, double durationSeconds, std::vector<Marker>& output);

}
