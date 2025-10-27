# pragma once

#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <fstream>
#include <stdexcept>
#include <cctype>
#include <algorithm>

// ------------------------
// Pointer and Offsets
// ------------------------


struct Pointer {
    std::vector<SIZE_T> offsets;
    SIZE_T final_offset;

    static Pointer fromString(const std::string& s) {
        std::istringstream iss(s);
        std::vector<SIZE_T> all;
        std::string hex;
        while (iss >> hex) {
            all.push_back(std::stoul(hex, nullptr, 16));
        }
        Pointer p;
        if (!all.empty()) {
            p.final_offset = all.back();
            all.pop_back();
        } else {
            p.final_offset = 0;
        }
        p.offsets = std::move(all);
        return p;
    }
};

struct RekordboxOffsets {
    // Version string (e.g. "7.1.4")
    std::string version;

    // Pointer to the masterdeck index
    Pointer masterdeck_index;

    // Per-deck data (2-4 entries expected)
    struct deck_data {
        Pointer bpm;       // Current track BPM
        Pointer beat;      // Current beat (shown in UI)
        Pointer bar;       // Current bar (shown in UI)
        Pointer pitch;     // Pitch %
        Pointer sample;    // Sample number (playback position)
        Pointer info;     // Track info (artist/title)
    };

    std::vector<deck_data> decks;

    static bool isVersionLine(const std::string& s) {
        // Trim
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return false;
        auto end = s.find_last_not_of(" \t\r\n");
        std::string t = s.substr(start, end - start + 1);
        // Must start with a digit and contain at least one dot
        if (!std::isdigit((unsigned char)t[0])) return false;
        if (t.find('.') == std::string::npos) return false;
        // allow digits and dots only (simple check)
        for (char c : t) if (!(std::isdigit((unsigned char)c) || c == '.')) return false;
        return true;
    }

    static std::map<std::string, RekordboxOffsets> loadFromFile(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Could not open offsets file");
        std::map<std::string, RekordboxOffsets> m;

        std::string line;
        RekordboxOffsets current;
        bool haveCurrent = false;

        while (std::getline(in, line)) {
            // trim leading spaces for comment check
            auto pos = line.find_first_not_of(" \t\r\n");
            if (pos == std::string::npos) {
                // blank line -> ignore
                continue;
            }
            std::string trimmed = line.substr(pos);
            if (trimmed.empty()) continue;
            if (trimmed[0] == '#') continue; // comment

            if (isVersionLine(trimmed)) {
                // new version header
                if (haveCurrent) {
                    m[current.version] = current;
                    current = RekordboxOffsets();
                    haveCurrent = false;
                }
                current = RekordboxOffsets();
                current.version = trimmed;
                haveCurrent = true;
                continue;
            }

            if (!haveCurrent) {
                // lines before any version header: ignore
                continue;
            }

            // Expect the first pointer line after version to be masterdeck_index.
            if (current.masterdeck_index.offsets.empty() && current.masterdeck_index.final_offset == 0) {
                try {
                    current.masterdeck_index = Pointer::fromString(trimmed);
                } catch (...) { /* ignore malformed */ }
                continue;
            }

            // Parse deck pointer lines: groups of 6 pointer lines per deck
            const size_t LINES_PER_DECK = 6;
            std::vector<Pointer> deck_buf;

            // The current line is the first in a potential deck block — collect LINES_PER_DECK lines (skipping comments/blanks)
            try {
                deck_buf.push_back(Pointer::fromString(trimmed));
            } catch (...) { /* ignore malformed */ }

            while (deck_buf.size() < LINES_PER_DECK && std::getline(in, line)) {
                auto p2 = line.find_first_not_of(" \t\r\n");
                if (p2 == std::string::npos) continue; // skip blank
                std::string t2 = line.substr(p2);
                if (t2.empty()) continue;
                if (t2[0] == '#') continue;
                if (isVersionLine(t2)) {
                    // encountered next version prematurely — push back current version and restart parsing from this line
                    m[current.version] = current;
                    current = RekordboxOffsets();
                    current.version = t2;
                    haveCurrent = true;
                    // stop collecting deck lines; break outer loop by returning to top
                    break;
                }
                try {
                    deck_buf.push_back(Pointer::fromString(t2));
                } catch (...) { /* ignore malformed */ }
            }

            if (deck_buf.size() == LINES_PER_DECK) {
                deck_data d;
                d.bpm = deck_buf[0];
                d.beat = deck_buf[1];
                d.bar = deck_buf[2];
                d.pitch = deck_buf[3];
                d.sample = deck_buf[4];
                d.info = deck_buf[5];
                current.decks.push_back(std::move(d));
            } else {
                // incomplete deck block: ignore
            }
        }

        // flush last
        if (haveCurrent) m[current.version] = current;

        return m;
    }
};
