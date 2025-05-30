#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <optional>
#include <array>
#include <chrono>

#include "offsets.h"
#include "choreographer.h"

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
    std::optional<Value<float>>    master_bpm_val;
    std::optional<Value<int32_t>>  bar1_val, beat1_val, bar2_val, beat2_val;
    std::optional<Value<uint8_t>>  masterdeck_index_val;

    // New: Optionals for artist/track strings (deck 1 & 2)
    std::optional<Value<std::array<char, 100>>> deck1_artist_val;
    std::optional<Value<std::array<char, 100>>> deck1_title_val;
    std::optional<Value<std::array<char, 100>>> deck2_artist_val;
    std::optional<Value<std::array<char, 100>>> deck2_title_val;

    int32_t beats1{ -1 }, beats2{ -1 }, master_beats{ 0 };
    float   master_bpm{ 120.0f };
    uint8_t masterdeck_index{ 0 };

    // New: Actual string fields
    std::string deck1_artist, deck1_title, deck2_artist, deck2_title;

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

        // New: construct Value for artist/track strings
        deck1_artist_val = Value<std::array<char, 100>>::create(h, base, off.deck1artist);
        deck1_title_val  = Value<std::array<char, 100>>::create(h, base, off.deck1title);
        deck2_artist_val = Value<std::array<char, 100>>::create(h, base, off.deck2artist);
        deck2_title_val  = Value<std::array<char, 100>>::create(h, base, off.deck2title);
    }

    void refresh() {
        master_bpm = (*master_bpm_val).read();
        beats1 = (*bar1_val).read() * 4 + (*beat1_val).read();
        beats2 = (*bar2_val).read() * 4 + (*beat2_val).read();
        masterdeck_index = (*masterdeck_index_val).read();
        master_beats = (masterdeck_index == 0 ? beats1 : beats2);

        // Use strnlen to ensure null-termination
        auto arr_to_str = [](const std::array<char, 100>& arr) {
            return std::string(arr.data(), strnlen(arr.data(), arr.size()));
        };
        deck1_artist = arr_to_str((*deck1_artist_val).read());
        deck1_title  = arr_to_str((*deck1_title_val).read());
        deck2_artist = arr_to_str((*deck2_artist_val).read());
        deck2_title  = arr_to_str((*deck2_title_val).read());
        //std::cout << "Rekordbox state: "
        //          << ", Deck 1: " << deck1_artist << " - " << deck1_title
        //         << ", Deck 2: " << deck2_artist << " - " << deck2_title
        //          << std::endl;
    }
};

// ------------------------
// Beat‐tracking logic
// ------------------------
class BeatKeeper {
public:
    BeatKeeper(const RekordboxOffsets& off, Choreographer* choreo)
        : rb_(off)
        , choreo_(choreo)
        , last_beat_(0)
        , beat_fraction_(1.0f)
        , last_masterdeck_index_(0)
        , offset_micros_(0.0f)
        , last_bpm_(0.0f)
        , new_beat_(false)
        , last_master_artist_()
        , last_master_title_()
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
        std::string current_master_artist = (rb_.masterdeck_index == 0) ? rb_.deck1_artist : rb_.deck2_artist;
        std::string current_master_title = (rb_.masterdeck_index == 0) ? rb_.deck1_title : rb_.deck2_title;
        
        if (rb_.masterdeck_index != last_masterdeck_index_ || 
            current_master_artist != last_master_artist_ || 
            current_master_title != last_master_title_) {
            
            last_masterdeck_index_ = rb_.masterdeck_index;
            last_master_artist_ = current_master_artist;
            last_master_title_ = current_master_title;
            last_beat_ = rb_.master_beats;
            
            if (choreo_) {
                choreo_->onMasterTrackChanged(current_master_artist, current_master_title);
                choreo_->onNewBeat(rb_.master_beats);
            }
        }

        // --- Beat tracking ---
        if (std::abs(rb_.master_beats - last_beat_) > 0) {
            last_beat_ = rb_.master_beats;
            beat_fraction_ = 0.0f;
            new_beat_ = true;
            
            if (choreo_) choreo_->onNewBeat(rb_.master_beats);
        } else {
            float beats_per_micro = rb_.master_bpm / 60.0f / 1'000'000.0f;
            beat_fraction_ = std::fmod(beat_fraction_ + actual_delta.count() * beats_per_micro, 1.0f);
        }
        
        // Always send beat fraction update with delta time
        if (choreo_) choreo_->onBeatFraction(getBeatFraction(), actual_delta);
    }

    float getBeatFraction() const {
        float beats_per_micro = rb_.master_bpm / 60.0f / 1'000'000.0f;
        return std::fmod(beat_fraction_ + offset_micros_ * beats_per_micro + 1.0f, 1.0f);
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
    std::string last_master_artist_;
    std::string last_master_title_;
    std::chrono::high_resolution_clock::time_point last_update_time_;
};