# pragma once

#include <vector>
#include <string>
#include <sstream>
#include <map>

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
        // last element is final_offset
        Pointer p;
        p.final_offset = all.back();
        all.pop_back();
        p.offsets = std::move(all);
        return p;
    }
};

struct RekordboxOffsets {
    std::string version;
    Pointer deck1bar, deck1beat, deck2bar, deck2beat, master_bpm, masterdeck_index, deck1artist, deck1title, deck2artist, deck2title;

    static std::map<std::string, RekordboxOffsets> loadFromFile(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Could not open offsets file");
        std::map<std::string, RekordboxOffsets> m;
        std::vector<std::string> block;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                if (!block.empty()) {
                    RekordboxOffsets o;
                    o.version = block[0];
                    o.deck1bar = Pointer::fromString(block[1]);
                    o.deck1beat = Pointer::fromString(block[2]);
                    o.deck2bar = Pointer::fromString(block[3]);
                    o.deck2beat = Pointer::fromString(block[4]);
                    o.master_bpm = Pointer::fromString(block[5]);
                    o.masterdeck_index = Pointer::fromString(block[6]);
                    o.deck1artist = Pointer::fromString(block[7]);
                    o.deck1title = Pointer::fromString(block[8]);
                    o.deck2artist = Pointer::fromString(block[9]);
                    o.deck2title = Pointer::fromString(block[10]);
                    m[o.version] = o;
                    block.clear();
                }
            }
            else if (line[0] != '#') {
                block.push_back(line);
            }
        }
        return m;
    }
};
