#include "MarkerImport.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>
#include <vector>

namespace slicelab {
namespace {
constexpr std::size_t maxTextBytes = 256 * 1024;
constexpr std::size_t maxNameBytes = 80;
constexpr std::size_t maxMarkers = 32;

// A record is one physical line. Quoted fields may include the separator or
// doubled quotes; control characters and multiline names are not supported.
bool fieldsOf(const std::string& line, char separator, std::vector<std::string>& fields) {
    std::size_t pos = 0;
    for (;;) {
        std::string field;
        std::size_t first = pos;
        while (first < line.size() && line[first] == ' ') ++first;
        if (first < line.size() && line[first] == '"') {
            pos = first + 1;
            bool closed = false;
            while (pos < line.size()) {
                const char c = line[pos++];
                if (c == '"') {
                    if (pos < line.size() && line[pos] == '"') { field += '"'; ++pos; }
                    else { closed = true; break; }
                } else {
                    if ((unsigned char)c < 0x20 || c == 0x7f) return false;
                    field += c;
                }
            }
            if (!closed) return false;
            while (pos < line.size() && line[pos] == ' ') ++pos;
            if (pos < line.size() && line[pos] != separator) return false;
        } else {
            while (pos < line.size() && line[pos] != separator) {
                const char c = line[pos++];
                if (c == '"' || (unsigned char)c < 0x20 || c == 0x7f) return false;
                field += c;
            }
        }
        fields.push_back(std::move(field));
        if (fields.size() > 2) return false;
        if (pos == line.size()) return fields.size() == 2;
        ++pos;
    }
}

char findSeparator(const std::string& line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') ++i;
            else quoted = !quoted;
        } else if (!quoted && (line[i] == ',' || line[i] == '\t')) return line[i];
    }
    return 0;
}

bool number(const juce::String& input, double& value) {
    auto s = input.trim().toStdString();
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = first + s.size();
    if (*first == '+') ++first;
    if (first == last) return false;
    const auto result = std::from_chars(first, last, value, std::chars_format::general);
    return result.ec == std::errc() && result.ptr == last && std::isfinite(value);
}
}

juce::Result parseMarkers(const juce::String& text, double durationSeconds, std::vector<Marker>& output) {
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0)
        return juce::Result::fail("Load a source before importing markers.");
    auto data = text.toStdString();
    if (data.size() > maxTextBytes) return juce::Result::fail("Marker file exceeds 256 KB.");
    if (data.size() >= 3 && (unsigned char)data[0] == 0xef && (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf)
        data.erase(0, 3);
    std::vector<Marker> parsed;
    char separator = 0;
    bool firstRecord = true;
    std::size_t start = 0;
    int lineNumber = 0;
    while (start < data.size()) {
        ++lineNumber;
        const auto end = data.find_first_of("\r\n", start);
        auto line = data.substr(start, end == std::string::npos ? end : end - start);
        if (end == std::string::npos) start = data.size();
        else {
            start = end + 1;
            if (data[end] == '\r' && start < data.size() && data[start] == '\n') ++start;
        }
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        if (!separator) separator = findSeparator(line);
        std::vector<std::string> fields;
        if (!separator || !fieldsOf(line, separator, fields))
            return juce::Result::fail("Invalid marker record on line " + juce::String(lineNumber) + ". Expected seconds and name.");
        auto time = juce::String::fromUTF8(fields[0].c_str(), (int)fields[0].size()).trim();
        auto name = juce::String::fromUTF8(fields[1].c_str(), (int)fields[1].size()).trim();
        if (firstRecord && time.equalsIgnoreCase("seconds") && name.equalsIgnoreCase("name")) {
            firstRecord = false; continue;
        }
        firstRecord = false;
        double seconds = 0;
        if (!number(time, seconds) || seconds < 0 || seconds >= durationSeconds)
            return juce::Result::fail("Invalid marker time on line " + juce::String(lineNumber) + ". Times must be within the source.");
        if ((std::size_t)name.getNumBytesAsUTF8() > maxNameBytes)
            return juce::Result::fail("Marker name on line " + juce::String(lineNumber) + " exceeds 80 UTF-8 bytes.");
        parsed.push_back({seconds, std::move(name)});
        if (parsed.size() > maxMarkers) return juce::Result::fail("A marker file can contain at most 32 markers.");
    }
    if (parsed.empty()) return juce::Result::fail("The marker file contains no markers.");
    std::sort(parsed.begin(), parsed.end(), [](const Marker& a, const Marker& b) { return a.seconds < b.seconds; });
    for (std::size_t i = 1; i < parsed.size(); ++i)
        if (!(parsed[i - 1].seconds < parsed[i].seconds)) return juce::Result::fail("Marker times must be unique.");
    output = std::move(parsed);
    return juce::Result::ok();
}

}
