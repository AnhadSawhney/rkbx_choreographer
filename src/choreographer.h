#pragma once
#include <string>
#include <memory>
#include <iostream>

#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"
#include "ip/IpEndpointName.h"
using namespace osc;

class Choreographer {
public:
    Choreographer() : oscSocket(nullptr) {}

    ~Choreographer() {
        if (oscSocket) delete oscSocket;
    }

    // Setup OSC socket
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

    // Callback: Beat fraction changed
    void onBeatFraction(float beatFraction) {
        if (!oscSocket) return;
        char buf[256];
        osc::OutboundPacketStream p{ buf, sizeof(buf) };
        p << osc::BeginMessage("/beat") << beatFraction << osc::EndMessage;
        oscSocket->Send(p.Data(), p.Size());
    }

    // Callback: BPM changed
    void onBpmChanged(float bpm) {
        if (!oscSocket) return;
        char buf[256];
        osc::OutboundPacketStream p{ buf, sizeof(buf) };
        p << osc::BeginMessage("/bpm") << bpm << osc::EndMessage;
        oscSocket->Send(p.Data(), p.Size());

        std::cout << "BPM changed to: " << bpm << "\n";
    }

    // Callback: Artist changed (deck 1 or 2)
    void onArtistChanged(int deck, const std::string& artist) {
        if (!oscSocket) return;
        char buf[256];
        std::string path = deck == 1 ? "/deck1/artist" : "/deck2/artist";
        osc::OutboundPacketStream p{ buf, sizeof(buf) };
        p << osc::BeginMessage(path.c_str()) << artist.c_str() << osc::EndMessage;
        oscSocket->Send(p.Data(), p.Size());

        std::cout << "New artist on deck " << deck << ": " << artist << "\n";
    }

    // Callback: Track changed (deck 1 or 2)
    void onTrackChanged(int deck, const std::string& track) {
        if (!oscSocket) return;
        char buf[256];
        std::string path = deck == 1 ? "/deck1/track" : "/deck2/track";
        osc::OutboundPacketStream p{ buf, sizeof(buf) };
        p << osc::BeginMessage(path.c_str()) << track.c_str() << osc::EndMessage;
        oscSocket->Send(p.Data(), p.Size());

        std::cout << "New track on deck " << deck << ": " << track << "\n";
    }

private:
    UdpTransmitSocket* oscSocket;
};