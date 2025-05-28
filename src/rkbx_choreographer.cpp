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

#include "offsets.h"
#include "beatkeeper.h"
#include "choreographer.h"

// Ableton Link C++ SDK
//#include "Link.hpp"
//using namespace ableton;

// ------------------------
// main()
// ------------------------
int main(int argc, char* argv[]) {
    std::cout << std::fixed;
    std::cout << std::setprecision(2);
    // 1) load or download offsets
    if (!std::ifstream("offsets.txt")) {
        std::cout << "Offsets not found, downloading...\n";
        system("curl -o offsets https://raw.githubusercontent.com/AnhadSawhney/rkbx_choreographer/master/offsets.txt");
    }
    auto versions = RekordboxOffsets::loadFromFile("offsets.txt");
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
            system("curl -o offsets https://raw.githubusercontent.com/AnhadSawhney/rkbx_choreographer/master/offsets.txt");
            return 0;
        }
        else if (a == "-o") {
            std::cout << "Enabling OSC\n";
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

    // 3) setup Choreographer
    Choreographer choreo;
    if (osc_enabled) {
        if (!choreo.setupOsc(dst_addr)) {
            std::cerr << "Failed to setup OSC socket for " << dst_addr << "\n";
            return 1;
        }
    }

    // 4) Ableton Link
    //Link link(120.0);
    //link.enable(false);
    //Link::SessionState state;
    //link.captureAppSessionState(state);
    //link.enable(true);

    // 5) BeatKeeper
    BeatKeeper keeper(it->second, &choreo);

    using clk = std::chrono::high_resolution_clock;
    auto last = clk::now();

    std::cout << "Entering loop\n";
    while (true) {
        auto now = clk::now();
        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - last);
        last = now;

        keeper.update(delta);

        /*
        // send OSC beat‐fraction
        if (osc_enabled) {
            char buf[256];
            osc::OutboundPacketStream p{ buf, sizeof(buf) };
            p << osc::BeginMessage("/beat") << keeper.getBeatFraction() << osc::EndMessage;
            oscSocket->Send(p.Data(), p.Size());
            std::cout << "Beat fraction: " << keeper.getBeatFraction() << std::endl;
        }

        // BPM change
        if (auto bpm = keeper.getBpmChanged()) {
            if (osc_enabled) {
                char buf[256];
                osc::OutboundPacketStream p{ buf, sizeof(buf) };
                p << osc::BeginMessage("/bpm") << *bpm << osc::EndMessage;
                oscSocket->Send(p.Data(), p.Size());
            }
            std::cout << "BPM changed to: " << *bpm << std::endl;
        }*/

        
                 // << ", Deck: " << (int)keeper.lastDeck() 
                 // << ", Beats: " << keeper.lastBeat() 
                 // << "\n";

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

    //if (oscSocket) delete oscSocket;
    return 0;
}
