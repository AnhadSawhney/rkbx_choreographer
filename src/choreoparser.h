// ChoreoParser.h
#pragma once

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <cmath>

#include "osc/OscOutboundPacketStream.h"

/// If defined, wrappers BeginBundleImmediate/EndBundle are emitted
#define CHOREO_BUNDLE_MESSAGES

namespace choreo {

/// Simple OSC message container
struct OSCMessage {
    std::string address;
    char        type;   // 'i','f','s', etc.
    std::string data;   // textual representation
};

/// Instruction at a specific time (in beats)
struct Instruction {
    double time;
    std::vector<OSCMessage> msgs;
};

class ChoreoParser {
public:
    /// Load, optimize (merge & sort), and rewrite the file in-place
    explicit ChoreoParser(const std::string& filename) {
        loadAndOptimize(filename);
        buildRuntimeInstructions();
        writeOptimizedFile(filename);
        nextIndex_ = 0;
    }

    /// Case-insensitive alnum-only match against patterns
    bool matches(const std::string& artist, const std::string& title) const {
        return anyMatch(matchArtists_, artist)
            && anyMatch(matchTitles_,  title);
    }

    /// Update by beat position. deltaBeat in beats
    /// Adds messages to `p`. Returns true if any were added.
    bool update(int beat, double frac,
             double deltaBeat,
             osc::OutboundPacketStream& p)
    {
        double cur = beat + frac;
        double w0  = cur - deltaBeat;
        double w1  = cur + deltaBeat;

#ifdef CHOREO_BUNDLE_MESSAGES
        p << osc::BeginBundleImmediate;
#endif
        bool sent = false;

        // Binary search for first instruction >= w0 (search entire list)
        Instruction probe_w0{w0, {}};
        auto it_start = std::lower_bound(
            instructions_.begin(),
            instructions_.end(), 
            probe_w0,
            [](const auto& a, const auto& b) { return a.time < b.time; }
        );

        // Binary search for first instruction > w1 (search entire list)
        Instruction probe_w1{w1, {}};
        auto it_end = std::upper_bound(
            instructions_.begin(),
            instructions_.end(),
            probe_w1,
            [](const auto& a, const auto& b) { return a.time < b.time; }
        );

        // Send all instructions in the range [w0, w1]
        for (auto it = it_start; it != it_end; ++it) {
            for (const auto& m : it->msgs) {
                p << osc::BeginMessage(m.address.c_str());
                pushArg(p, m);
                p << osc::EndMessage;
                sent = true;
            }
        }

#ifdef CHOREO_BUNDLE_MESSAGES
        p << osc::EndBundle;
#endif
        return sent;
    }

    /// Wrapper by time in seconds (delta in sec, bpm)
    bool updateWithTime(double currentTimeSec,
                    double deltaTimeSec,
                    double bpm,
                    osc::OutboundPacketStream& p)
    {
        double beatsNow   = currentTimeSec * bpm / 60.0;
        int    beatInt    = static_cast<int>(std::floor(beatsNow));
        double beatFrac   = beatsNow - beatInt;
        double deltaBeats = deltaTimeSec * bpm / 60.0;
        return update(beatInt, beatFrac, deltaBeats, p);
    }

    /// Wrapper with int beat, double frac, and delta in seconds
    bool updateWithMixed(int beat, double frac,
                double deltaTimeSec, double bpm,
                osc::OutboundPacketStream& p)
    {
        double deltaBeats = deltaTimeSec * bpm / 60.0;
        return update(beat, frac, deltaBeats, p);
    }

private:
    // raw file structure, preserving comments and header lines
    struct ParsedLine {
        double time;
        std::vector<OSCMessage> msgs;
    };
    struct RawElement {
        bool isCommentOrHeader;
        std::string text;
        std::vector<ParsedLine> rows;
    };

    std::vector<std::string> matchTitles_, matchArtists_;
    std::vector<RawElement> elements_;
    std::vector<Instruction> instructions_;
    size_t nextIndex_ = 0;

    /// Read TSV, group by comments, merge per-block, rebuild runtime list
    void loadAndOptimize(const std::string& fn) {
        std::ifstream in(fn);
        if (!in) throw std::runtime_error("Cannot open " + fn);

        elements_.clear();
        RawElement currentBlock{false, "", {}};
        int lineStage = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (line.front() == '#') {
                // flush any pending rows
                if (!currentBlock.rows.empty()) {
                    elements_.push_back(currentBlock);
                    currentBlock = RawElement{false, "", {}};
                }
                // comment line
                elements_.push_back({true, line, {}});
                continue;
            }

            // remove quotation marks from the line
            line.erase(std::remove(line.begin(), line.end(), '"'), line.end());
            // Continue if the line is empty or contains only tabs/whitespace
            if (line.empty() || std::all_of(line.begin(), line.end(), [](char c){ return std::isspace((unsigned char)c); }))
                continue;

            if (lineStage < 2) {
                // Match Song / Match Artist
                const char* expect = (lineStage==0 ? "Match Song" : "Match Artist");
                auto &dest = (lineStage==0 ? matchTitles_ : matchArtists_);
                parseMatchLine(line, expect, dest);
                elements_.push_back({true, line, {}});
                ++lineStage;
                continue;
            }
            if (lineStage == 2) {
                // header row
                elements_.push_back({true, line, {}});
                ++lineStage;
                continue;
            }
            else {
                // data row
                //std::cout << "Parsing data row: " << line << '\n';

                auto cols = split(line, '\t');
                if (cols.size() < 5 || (cols.size()-2)%3 != 0) {
                    throw std::runtime_error("Row in " + fn + "has wrong number of populated cells: " + line);
                }
                double t = parseTime(cols[0], cols[1]);
                ParsedLine pl{t,{}};
                for (size_t i=2; i+2<cols.size(); i+=3) {
                    pl.msgs.push_back({cols[i], cols[i+2][0], cols[i+1]});
                }
                currentBlock.rows.push_back(std::move(pl));
            }
        }
        // flush last block
        if (!currentBlock.rows.empty())
            elements_.push_back(currentBlock);
    }

    /// After loadAndOptimize, build global instruction list (merged & sorted)
    void buildRuntimeInstructions() {
        std::map<double, Instruction> globalMap;
        for (auto const& elem : elements_) {
            if (elem.isCommentOrHeader) continue;
            for (auto const& pl : elem.rows) {
                auto &inst = globalMap[pl.time];
                inst.time = pl.time;
                inst.msgs.insert(inst.msgs.end(), pl.msgs.begin(), pl.msgs.end());
            }
        }
        instructions_.clear();
        instructions_.reserve(globalMap.size());
        for (auto const& kv : globalMap)
            instructions_.push_back(kv.second);
    }

    /// Overwrite original file, sorting blocks and preserving comments
    void writeOptimizedFile(const std::string& fn) const {
        std::ofstream out(fn);
        if (!out) throw std::runtime_error("Cannot write " + fn);
        for (auto const& elem : elements_) {
            if (elem.isCommentOrHeader) {
                out << elem.text << '\n';
            } else {
                // data block: sort & merge within block
                std::map<double, std::vector<OSCMessage>> blockMap;
                for (auto const& pl : elem.rows) {
                    auto &v = blockMap[pl.time];
                    v.insert(v.end(), pl.msgs.begin(), pl.msgs.end());
                }
                for (auto const& kv : blockMap) {
                    out << kv.first;
                    for (auto const& m : kv.second) {
                        out << '\t' << m.address
                            << '\t' << m.data
                            << '\t' << m.type;
                    }
                    out << '\n';
                }
            }
        }
    }

    /// Parse Match lines
    static void parseMatchLine(const std::string& line,
                               const std::string& expect,
                               std::vector<std::string>& dest)
    {
        auto cols = split(line, '\t');
        if (cols.empty() || cols[0] != expect)
            throw std::runtime_error("Expected '" + expect + "' line");
        dest.assign(cols.begin()+1, cols.end());
    }

    static std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, delim)) out.push_back(tok);
        return out;
    }

    /// c0: beat or bar.beat, c1: fractional beats [0,1)
    static double parseTime(const std::string& c0,
                            const std::string& c1)
    {
        double base;
        auto dot = c0.find('.');
        if (dot != std::string::npos) {
            int bar  = std::stoi(c0.substr(0, dot));
            int beat = std::stoi(c0.substr(dot+1));
            base = (bar - 1) * 4.0 + static_cast<double>(beat);
        } else {
            base = std::stod(c0);
        }
        return base + std::stod(c1);
    }

    static void pushArg(osc::OutboundPacketStream& p,
                        OSCMessage const& m)
    {
        switch(m.type) {
            case 'i': p << std::stoi(m.data); break;
            case 'f': p << std::stof(m.data); break;
            case 's': p << m.data.c_str();    break;
            default: /* extend for 'd','b',... */ break;
        }

        //std::cout << "Pushing OSC arg: " << m.address << " type: " << m.type << " data: " << m.data << '\n';
    }

    static std::string normalize(std::string s) {
        std::string out;
        for (char c: s) if (std::isalnum((unsigned char)c))
            out.push_back(std::tolower((unsigned char)c));
        return out;
    }

    static bool anyMatch(const std::vector<std::string>& pats,
                         const std::string& text)
    {
        auto norm = normalize(text);
        for (auto const& p: pats)
            if (normalize(p) == norm)
                return true;
        return false;
    }
};

} // namespace choreo
