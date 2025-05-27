// main.cpp

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <chrono>
#include <conio.h>
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")

// OSC pack (adjust include paths to your install)
#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"
#include "ip/IpEndpointName.h"
using namespace osc;  // for OutboundPacketStream
//using namespace ip;   // for IpEndpointName & UdpTransmitSocket

// Ableton Link C++ SDK
//#include "Link.hpp"
//using namespace ableton;

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
    Pointer deck1bar, deck1beat, deck2bar, deck2beat, master_bpm, masterdeck_index;

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

// ------------------------
// Utilities to open process
// ------------------------
DWORD getProcessIdByName(const std::wstring& procName) {
    PROCESSENTRY32W entry{ sizeof(entry) };
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (procName == entry.szExeFile) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

SIZE_T getModuleBaseAddress(DWORD pid, const std::wstring& moduleName) {
    MODULEENTRY32W me{ sizeof(me) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (Module32FirstW(snap, &me)) {
        do {
            if (moduleName == me.szModule) {
                CloseHandle(snap);
                return reinterpret_cast<SIZE_T>(me.modBaseAddr);
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return 0;
}

// ------------------------
// Generic memory‐reader
// ------------------------
template<typename T>
class Value {
public:
    static Value<T> create(HANDLE hProc, SIZE_T base, const Pointer& p) {
        SIZE_T addr = base;
        // walk pointer chain
        for (auto off : p.offsets) {
            SIZE_T tmp;
            ReadProcessMemory(hProc, (LPCVOID)(addr + off), &tmp, sizeof(tmp), nullptr);
            addr = tmp;
        }
        addr += p.final_offset;
        return Value(hProc, addr);
    }

    T read() const {
        T v;
        ReadProcessMemory(hProc_, (LPCVOID)address_, &v, sizeof(v), nullptr);
        return v;
    }
private:
    Value(HANDLE h, SIZE_T a) : hProc_(h), address_(a) {}
    HANDLE hProc_;
    SIZE_T address_;
};

// ------------------------
// Rekordbox mirror
// ------------------------
struct Rekordbox {
    // hold in optionals so we can delay construction until we have hProc & base
    std::optional<Value<float>>    master_bpm_val;
    std::optional<Value<int32_t>>  bar1_val, beat1_val, bar2_val, beat2_val;
    std::optional<Value<uint8_t>>  masterdeck_index_val;

    int32_t beats1{ -1 }, beats2{ -1 }, master_beats{ 0 };
    float   master_bpm{ 120.0f };
    uint8_t masterdeck_index{ 0 };

    Rekordbox(const RekordboxOffsets& off) {
        // 1) find & open process
        DWORD pid = getProcessIdByName(L"rekordbox.exe");
        if (!pid) throw std::runtime_error("Rekordbox not running");
        HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
            FALSE, pid);
        if (!h)  throw std::runtime_error("Failed to OpenProcess");

        // 2) find module base
        SIZE_T base = getModuleBaseAddress(pid, L"rekordbox.exe");
        if (!base) throw std::runtime_error("Module base not found");

        // 3) now construct each Value<T> in place
        master_bpm_val = Value<float>::create(h, base, off.master_bpm);
        bar1_val = Value<int32_t>::create(h, base, off.deck1bar);
        beat1_val = Value<int32_t>::create(h, base, off.deck1beat);
        bar2_val = Value<int32_t>::create(h, base, off.deck2bar);
        beat2_val = Value<int32_t>::create(h, base, off.deck2beat);
        masterdeck_index_val = Value<uint8_t>::create(h, base, off.masterdeck_index);
    }

    void refresh() {
        // unwrap optionals (they’re guaranteed to be engaged here)
        master_bpm = (*master_bpm_val).read();
        beats1 = (*bar1_val).read() * 4 + (*beat1_val).read();
        beats2 = (*bar2_val).read() * 4 + (*beat2_val).read();
        masterdeck_index = (*masterdeck_index_val).read();
        master_beats = (masterdeck_index == 0 ? beats1 : beats2);
    }
};

// ------------------------
// Beat‐tracking logic
// ------------------------
class BeatKeeper {
public:
    BeatKeeper(const RekordboxOffsets& off)
        : rb_(off)
        , last_beat_(0)
        , beat_fraction_(1.0f)
        , last_masterdeck_index_(0)
        , offset_micros_(0.0f)
        , last_bpm_(0.0f)
        , new_beat_(false)
    {
    }

    void update(std::chrono::microseconds delta) {
        rb_.refresh();
        // detect deck switch
        if (rb_.masterdeck_index != last_masterdeck_index_) {
            last_masterdeck_index_ = rb_.masterdeck_index;
            last_beat_ = rb_.master_beats;
        }
        // detect new beat
        if (std::abs(rb_.master_beats - last_beat_) > 0) {
            last_beat_ = rb_.master_beats;
            beat_fraction_ = 0.0f;
            new_beat_ = true;
        }
        // advance fraction
        float beats_per_micro = rb_.master_bpm / 60.0f / 1'000'000.0f;
        beat_fraction_ = std::fmod(beat_fraction_ + delta.count() * beats_per_micro, 1.0f);
    }

    bool getNewBeat() {
        if (new_beat_) { new_beat_ = false; return true; }
        return false;
    }

    std::optional<float> getBpmChanged() {
        if (rb_.master_bpm != last_bpm_) {
            last_bpm_ = rb_.master_bpm;
            return rb_.master_bpm;
        }
        return std::nullopt;
    }

    float getBeatFraction() const {
        float beats_per_micro = rb_.master_bpm / 60.0f / 1'000'000.0f;
        return std::fmod(beat_fraction_
            + offset_micros_ * beats_per_micro
            + 1.0f, 1.0f);
    }

    void changeOffsetMs(float ms) {
        offset_micros_ += ms * 1000.0f; // ms→μs
    }

    int32_t lastBeat() const { return last_beat_; }
    uint8_t lastDeck() const { return last_masterdeck_index_; }

private:
    Rekordbox rb_;
    int32_t   last_beat_;
    float     beat_fraction_;
    uint8_t   last_masterdeck_index_;
    float     offset_micros_;   // in μs
    float     last_bpm_;
    bool      new_beat_;
};

// ------------------------
// main()
// ------------------------
int main(int argc, char* argv[]) {
    std::cout << std::fixed;
    std::cout << std::setprecision(2);
    // 1) load or download offsets
    if (!std::ifstream("offsets")) {
        std::cout << "Offsets not found, downloading...\n";
        system("curl -o offsets https://raw.githubusercontent.com/grufkork/rkbx_osc/master/offsets");
    }
    auto versions = RekordboxOffsets::loadFromFile("offsets");
    if (versions.empty()) {
        std::cerr << "No offsets parsed!\n";
        return 1;
    }
    std::string target_version = versions.rbegin()->first;

    bool osc_enabled = false;
    std::string src_addr = "0.0.0.0:0";
    std::string dst_addr = "127.0.0.1:6669";

    // 2) simple flag parse
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-u") {
            std::cout << "Updating offsets...\n";
            system("curl -o offsets https://raw.githubusercontent.com/grufkork/rkbx_osc/master/offsets");
            return 0;
        }
        else if (a == "-o") {
            osc_enabled = true;
        }
        else if (a == "-s" && i + 1 < argc) {
            src_addr = argv[++i];
        }
        else if (a == "-t" && i + 1 < argc) {
            dst_addr = argv[++i];
        }
        else if (a == "-v" && i + 1 < argc) {
            target_version = argv[++i];
        }
        else if (a == "-h") {
            std::cout << R"(
Usage:
  -h        this help
  -u        fetch latest offsets and exit
  -v <ver>  target RB version (default: )" << target_version << "\n"
                "-o        enable OSC\n"
                "-s <src>  source UDP (host:port)\n"
                "-t <dst>  target UDP (host:port)\n"
                "Press i/k to adjust offset by ±1ms, c to quit.\n";
            return 0;
        }
    }

    auto it = versions.find(target_version);
    if (it == versions.end()) {
        std::cerr << "Unsupported version: " << target_version << "\n";
        return 1;
    }
    std::cout << "Targeting Rekordbox version " << target_version << "\n";

    // 3) setup OSC
    UdpTransmitSocket* oscSocket = nullptr;
    WSAData wsa;
    if (osc_enabled) {
        WSAStartup(MAKEWORD(2, 2), &wsa);

        // split "127.0.0.1:6669" into IP + port
        auto sep = dst_addr.find(':');
        std::string host = dst_addr.substr(0, sep);
        unsigned short port = static_cast<unsigned short>(
            std::stoi(dst_addr.substr(sep + 1))
            );

        //osc::IpEndpointName ep{ host.c_str(), port };
        IpEndpointName endpoint(host.c_str(), port);
        //oscSocket = new osc::UdpTransmitSocket(ep);
        UdpTransmitSocket oscSocket(endpoint);

        std::cout << "OSC on " << host << ":" << port << "\n";
    }

    // 4) Ableton Link
    //Link link(120.0);
    //link.enable(false);
    //Link::SessionState state;
    //link.captureAppSessionState(state);
    //link.enable(true);

    // 5) BeatKeeper
    BeatKeeper keeper(it->second);

    using clk = std::chrono::high_resolution_clock;
    auto last = clk::now();

    std::cout << "Entering loop\n";
    while (true) {
        auto now = clk::now();
        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - last);
        last = now;

        keeper.update(delta);

        // send OSC beat‐fraction
        if (oscSocket) {
            char buf[256];
            osc::OutboundPacketStream p{ buf, sizeof(buf) };
            p << osc::BeginMessage("/beat") << keeper.getBeatFraction() << osc::EndMessage;
            oscSocket->Send(p.Data(), p.Size());
        }

        // BPM change
        if (auto bpm = keeper.getBpmChanged()) {
            if (oscSocket) {
                char buf[256];
                osc::OutboundPacketStream p{ buf, sizeof(buf) };
                p << osc::BeginMessage("/bpm") << *bpm << osc::EndMessage;
                oscSocket->Send(p.Data(), p.Size());
            }
            std::cout << "BPM changed to: " << *bpm << "\n";
        }

        std::cout << "Beat fraction: " << keeper.getBeatFraction() 
                  << ", Deck: " << (int)keeper.lastDeck() 
                  << ", Beats: " << keeper.lastBeat() 
                  << "\n";

        // new beat → Ableton Link
        //if (keeper.getNewBeat()) {
        //    double current = std::round(link.clock().beatAtTime(4.0));
        //    double target = std::fmod((keeper.lastBeat() % 4) - std::fmod(current, 4.0) + 4.0, 4.0)
        //        + current - 1.0;
        //    link.captureAppSessionState(state);
        //    state.requestBeatAtTime(target, link.clock().micros(), 4.0);
        //    link.commitAppSessionState(state);
        //}

        // console update
        if (_kbhit()) {
            char c = _getch();
            if (c == 'c') break;
            if (c == 'i') keeper.changeOffsetMs(+1.0f);
            if (c == 'k') keeper.changeOffsetMs(-1.0f);
        }

        using namespace std::chrono_literals;
        std::this_thread::sleep_for(1000000us / 120);
    }

    delete oscSocket;
    return 0;
}
