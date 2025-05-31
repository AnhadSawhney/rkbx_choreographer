#pragma once
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <iostream>
#include <chrono>
#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"
#include "ip/IpEndpointName.h"
#include "choreoparser.h"

using namespace osc;

class Choreographer {
public:
    Choreographer(const std::string& choreoFolder = "") {
        if (!choreoFolder.empty()) {
            loadChoreoFiles(choreoFolder);
        }
    }

    ~Choreographer() {
        if (oscSocket) delete oscSocket;
    }

    bool setupOsc(const std::string& dst_addr) {
        auto sep = dst_addr.find(':');
        if (sep == std::string::npos) return false;
        std::string host = dst_addr.substr(0, sep);
        unsigned short port = static_cast<unsigned short>(std::stoi(dst_addr.substr(sep + 1)));
        IpEndpointName endpoint(host.c_str(), port);
        oscSocket = new UdpTransmitSocket(endpoint);
        std::cout << "OSC on " << host << ":" << port << "\n";
        return true;
    }

    // Callback: New beat occurred
    void onNewBeat(int beatNumber) {
        currentBeat_ = beatNumber;
        lastBeatTime_ = std::chrono::high_resolution_clock::now();
        //std::cout << "Beat: " << currentBeat_ << "\n";
    }

    // Callback: Beat fraction changed
    void onBeatFraction(float beatFraction, std::chrono::microseconds deltaTime) {
        if (!oscSocket || !activeChoreo) return;
        
        // Calculate delta in beats
        double deltaBeats = deltaTime.count() * currentBpm_ / 60.0 / 1'000'000.0;
        
        char buf[1024];
        osc::OutboundPacketStream p{ buf, sizeof(buf) };
        
        if (activeChoreo->update(currentBeat_, static_cast<double>(beatFraction), deltaBeats, p)) {
            oscSocket->Send(p.Data(), p.Size());
        }
    }

    // Callback: BPM changed
    void onBpmChanged(float bpm) {
        currentBpm_ = bpm;
        if (!oscSocket) return;
        char buf[256];
        osc::OutboundPacketStream p{ buf, sizeof(buf) };
        p << osc::BeginMessage("/composition/tempocontroller/tempo") << (bpm-20)/480 << osc::EndMessage; // weird resolume formula
        oscSocket->Send(p.Data(), p.Size());
        std::cout << "BPM changed to: " << bpm << "\n";
    }

    // Callback: Track/Artist changed on master deck
    void onMasterTrackChanged(const std::string& artist, const std::string& title) {
        std::cout << "Master track changed: " << artist << " - " << title << "\n";
        
        // Find matching choreo parser
        activeChoreo = nullptr;
        for (auto& parser : choreoParsers) {
            if (parser->matches(artist, title)) {
                activeChoreo = parser.get();
                std::cout << "Found matching choreography for: " << artist << " - " << title << "\n";
                break;
            }
        }
        
        if (!activeChoreo) {
            std::cout << "No choreography found for: " << artist << " - " << title << "\n";
        }
    }

private:
    void loadChoreoFiles(const std::string& folderPath) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(folderPath)) {
                if (entry.is_regular_file() && entry.path().extension() == ".tsv") {
                    std::cout << "Loading choreography: " << entry.path().string() << "\n";
                    choreoParsers.emplace_back(std::make_unique<choreo::ChoreoParser>(entry.path().string()));
                }
            }
            std::cout << "Loaded " << choreoParsers.size() << " choreography files\n";
        } catch (const std::exception& e) {
            std::cerr << "Error loading choreo files: " << e.what() << "\n";
        }
    }

    UdpTransmitSocket* oscSocket = nullptr;
    std::vector<std::unique_ptr<choreo::ChoreoParser>> choreoParsers;
    choreo::ChoreoParser* activeChoreo = nullptr;
    
    // Beat tracking
    int currentBeat_ = 0;
    float currentBpm_ = 120.0f;
    std::chrono::high_resolution_clock::time_point lastBeatTime_;
};