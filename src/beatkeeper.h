#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <optional>
#include <array>
#include <chrono>

#include "offsets.h"
#include "choreographer.h"
#include "beat_utils.h"

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

// Add this specialization after your generic Value<T>
template<>
class Value<std::array<char, 100>> {
public:
    static Value<std::array<char, 100>> create(HANDLE hProc, SIZE_T base, const Pointer& p) {
        return Value(hProc, base, p);
    }

    std::array<char, 100> read() const {
        SIZE_T addr = base_;
        // Walk pointer chain: at each step, read pointer at (addr + offset)
        for (auto off : pointer_.offsets) {
            SIZE_T tmp = 0;
            if (!ReadProcessMemory(hProc_, (LPCVOID)(addr + off), &tmp, sizeof(tmp), nullptr))
                return {};
            addr = tmp;
        }
        addr += pointer_.final_offset;
        std::array<char, 100> v{};
        ReadProcessMemory(hProc_, (LPCVOID)addr, v.data(), v.size(), nullptr);
        return v;
    }
private:
    Value(HANDLE h, SIZE_T base, const Pointer& p)
        : hProc_(h), base_(base), pointer_(p) {}
    HANDLE hProc_;
    SIZE_T base_;
    Pointer pointer_;
};

// ------------------------
// Rekordbox mirror
// ------------------------
struct Rekordbox {
    // hold in optionals so we can delay construction until we have hProc & base
    // new: masterdeck index value
    std::optional<Value<uint8_t>>  masterdeck_index_val;

    // per-deck optionals (size = number of decks in offsets)
    std::vector<std::optional<Value<float>>> deck_bpm_val;
    std::vector<std::optional<Value<int32_t>>> deck_bar_val;
    std::vector<std::optional<Value<int32_t>>> deck_beat_val;
    std::vector<std::optional<Value<int32_t>>> deck_pitch_val;
    std::vector<std::optional<Value<int32_t>>> deck_sample_val;
    std::vector<std::optional<Value<std::array<char,100>>>> deck_info_val;

    // per-deck dynamic state
    std::vector<int32_t> deck_beats;                // beat number per deck (legacy/unused for read)
    std::vector<int64_t> deck_samples;              // current sample position per deck
    int32_t master_beats{ 0 };
    float   master_bpm{ 120.0f };
    uint8_t masterdeck_index{ 0 };

    // per-deck artist/title strings (size == number of decks)
    std::vector<std::string> deck_infos;

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
        // masterdeck index
        masterdeck_index_val = Value<uint8_t>::create(h, base, off.masterdeck_index);

        // per-deck values
        size_t n = off.decks.size();
        deck_bpm_val.resize(n);
        deck_bar_val.resize(n);
        deck_beat_val.resize(n);
        deck_pitch_val.resize(n);
        deck_sample_val.resize(n);
        deck_info_val.resize(n);

        // initialize per-deck state containers
        deck_beats.assign(n, -1);
        deck_infos.assign(n, std::string());
        deck_samples.assign(n, 0);

        for (size_t i = 0; i < n; ++i) {
            const auto& d = off.decks[i];
            deck_bpm_val[i] = Value<float>::create(h, base, d.bpm);
            deck_beat_val[i] = Value<int32_t>::create(h, base, d.beat);
            deck_bar_val[i]  = Value<int32_t>::create(h, base, d.bar);
            deck_pitch_val[i]= Value<int32_t>::create(h, base, d.pitch);
            deck_sample_val[i]= Value<int32_t>::create(h, base, d.sample);
            deck_info_val[i]= Value<std::array<char,100>>::create(h, base, d.info);
        }
    }

    void refresh() {
        // read masterdeck index
        masterdeck_index = (*masterdeck_index_val).read();

        // for each deck, read values
        size_t n = deck_bpm_val.size();
        for (size_t i = 0; i < n; ++i) {
            float bpm = (*deck_bpm_val[i]).read();
            // read sample position (use sample to derive beat info later)
            int32_t sample_val = (*deck_sample_val[i]).read();
            deck_samples[i] = static_cast<int64_t>(sample_val);

            // legacy beat number field kept but NOT based on unreliable bar/beat
            deck_beats[i] = -1;

            // always read info (artist/title packed) into per-deck vectors
            auto arr = (*deck_info_val[i]).read();
            deck_infos[i] = std::string(arr.data(), strnlen(arr.data(), arr.size()));
        }

        // master BPM comes from the master deck's bpm
        if (masterdeck_index < deck_bpm_val.size()) {
            master_bpm = (*deck_bpm_val[masterdeck_index]).read();
            // master_beats will be computed from samples by BeatKeeper (sample-based), keep previous value if needed
        }

        // print out the state
        /*
        for (size_t i = 0; i < n; ++i) {
            if (i == masterdeck_index) {
                std::cout << "[Deck " << (i + 1) << "]: ";
            } else {
                std::cout << " Deck " << (i + 1) << ": ";
            }

            std::cout //<< deck_infos[i] 
                        << "bpm" << (*deck_bpm_val[i]).read() 
                        << " bar" << (*deck_bar_val[i]).read() 
                        << " beat" << (*deck_beat_val[i]).read() 
                        << " p" << (*deck_pitch_val[i]).read() 
                        << " s" << (*deck_sample_val[i]).read() << " |";
        }
        std::cout << std::endl;
        */
    }
};

// ------------------------
// Beat‐tracking logic
// ------------------------

class BeatKeeper {
public:
    // added delay_seconds: seconds of delay compensation to apply to sample->beat conversion
    BeatKeeper(const RekordboxOffsets& off, Choreographer* choreo, float delay_seconds = 0.0f)
         : rb_(off)
         , choreo_(choreo)
         , last_beat_(0)
         , beat_fraction_(1.0f)
         , last_masterdeck_index_(0)
         , offset_micros_(0.0f)
         , last_bpm_(0.0f)
         , new_beat_(false)
         , last_master_info_()
         , delay_seconds_(delay_seconds)
         , last_update_time_(std::chrono::high_resolution_clock::now())
     {
     }

    void update(std::chrono::microseconds delta) {
        rb_.refresh();
        
        auto current_time = std::chrono::high_resolution_clock::now();
        auto actual_delta = std::chrono::duration_cast<std::chrono::microseconds>(
            current_time - last_update_time_);
        last_update_time_ = current_time;

        // --- BPM change ---
        if (rb_.master_bpm != last_bpm_) {
            last_bpm_ = rb_.master_bpm;
            if (choreo_) choreo_->onBpmChanged(rb_.master_bpm);
        }

        // --- Deck switch or track change on master deck ---
        std::string current_master_info;
        if (rb_.masterdeck_index < rb_.deck_infos.size()) {
            current_master_info = rb_.deck_infos[rb_.masterdeck_index];
        } else {
            current_master_info.clear();
        }
        
        if (rb_.masterdeck_index != last_masterdeck_index_ || 
            current_master_info != last_master_info_) {
            
            last_masterdeck_index_ = rb_.masterdeck_index;
            last_master_info_ = current_master_info;
            // reset last_beat_ to the current master beat when track/deck changes
            last_beat_ = rb_.master_beats;
            
            if (choreo_) {
                choreo_->onMasterTrackChanged(current_master_info);
                choreo_->onNewBeat(rb_.master_beats);
            }
        }

        // --- Sample-based Beat tracking ---
        // Get origin/sample-rate from Choreographer (defaults provided by choreographer if none active)
        int64_t origin_sample = 0;
        int sample_rate = 44100;
        if (choreo_) {
            origin_sample = choreo_->getActiveOriginSample();
            sample_rate = choreo_->getActiveSampleRate();
        }

        // read current sample from master deck
        int64_t current_sample = 0;
        if (rb_.masterdeck_index < rb_.deck_samples.size()) {
            current_sample = rb_.deck_samples[rb_.masterdeck_index];
        }

        // apply delay compensation (shift sample by delay_seconds_)
        if (delay_seconds_ != 0.0f) {
            current_sample = static_cast<int64_t>(static_cast<double>(current_sample) + delay_seconds_ * static_cast<double>(sample_rate));
        }

        // compute samples per beat (avoid division by zero bpm)
        float bpm = rb_.master_bpm > 0.0f ? rb_.master_bpm : 120.0f;
        double samples_per_beat = static_cast<double>(sample_rate) * 60.0 / static_cast<double>(bpm);

        // beat_float can be negative (audio before origin). Beats are zero-indexed at origin.
        double beat_float = (static_cast<double>(current_sample) - static_cast<double>(origin_sample)) / samples_per_beat;

        // integer beat index and fractional part in [0,1)
        int32_t master_beat_index = static_cast<int32_t>(std::floor(beat_float));
        float fraction = static_cast<float>(beat_float - std::floor(beat_float));
        if (fraction < 0.0f) fraction += 1.0f; // ensure positive fractional component (shouldn't be needed after floor)

        // update Rekordbox mirror master_beats so other code can access it
        rb_.master_beats = master_beat_index;

        // Detect beat boundary (integer change)
        if (master_beat_index != last_beat_) {
            last_beat_ = master_beat_index;
            beat_fraction_ = fraction;
            new_beat_ = true;
            if (choreo_) choreo_->onNewBeat(master_beat_index);
        } else {
            // update fraction based on sample-derived value (no reliance on elapsed time)
            beat_fraction_ = fraction;
        }
        
        // Always send beat fraction update with actual_delta time
        if (choreo_) choreo_->onBeatFraction(getBeatFraction(), actual_delta);

        //std::cout << "sample:" << current_sample << " bpm:" << rb_.master_bpm << " beat:" << master_beat_index << " frac:" << getBeatFraction() << "\n";
    }

    float getBeatFraction() const {
        // beat_fraction_ already represents fractional position in beat [0,1)
        return std::fmod(beat_fraction_ + 1.0f, 1.0f);
    }

    void changeOffsetMs(float ms) {
        offset_micros_ += ms * 1000.0f;
    }

private:
    Rekordbox rb_;
    Choreographer* choreo_;
    int32_t   last_beat_;
    float     beat_fraction_;
    uint8_t   last_masterdeck_index_;
    float     offset_micros_;
    float     last_bpm_;
    bool      new_beat_;
    std::string last_master_info_;
    float     delay_seconds_;
    std::chrono::high_resolution_clock::time_point last_update_time_;
};