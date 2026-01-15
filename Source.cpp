#ifndef UNICODE
#define UNICODE
#endif 

#define _CRT_SECURE_NO_WARNINGS 

#include <windows.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dsound.h> // DirectSound
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <ctime>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

using Byte = uint8_t;
using Word = uint16_t;
using SignedByte = int8_t;

const int GB_WIDTH = 160;
const int GB_HEIGHT = 144;
const int SAMPLE_RATE = 44100;

#define IDM_FILE_OPEN 1001
#define IDM_FILE_EXIT 1002

template <class T> void SafeRelease(T** ppT) {
    if (*ppT) { (*ppT)->Release(); *ppT = NULL; }
}

// 前方宣言
class APU;
class MMU;
class PPU;
class CPU;
class GameBoyCore;

// -----------------------------------------------------------------------------
// AudioDriver
// -----------------------------------------------------------------------------
class AudioDriver {
    IDirectSound8* m_pDS;
    IDirectSoundBuffer* m_pPrimary;
    IDirectSoundBuffer* m_pSecondary;
    int m_bufferSize;
    int m_nextWriteOffset;

public:
    AudioDriver() : m_pDS(NULL), m_pPrimary(NULL), m_pSecondary(NULL), m_bufferSize(0), m_nextWriteOffset(0) {}
    ~AudioDriver() {
        SafeRelease(&m_pSecondary);
        SafeRelease(&m_pPrimary);
        SafeRelease(&m_pDS);
    }

    bool Initialize(HWND hwnd) {
        if (FAILED(DirectSoundCreate8(NULL, &m_pDS, NULL))) return false;
        if (FAILED(m_pDS->SetCooperativeLevel(hwnd, DSSCL_PRIORITY))) return false;

        DSBUFFERDESC dsbd = { 0 };
        dsbd.dwSize = sizeof(DSBUFFERDESC);
        dsbd.dwFlags = DSBCAPS_PRIMARYBUFFER;
        if (FAILED(m_pDS->CreateSoundBuffer(&dsbd, &m_pPrimary, NULL))) return false;

        WAVEFORMATEX wfx = { 0 };
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = SAMPLE_RATE;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = SAMPLE_RATE * 4;
        if (FAILED(m_pPrimary->SetFormat(&wfx))) return false;

        m_bufferSize = wfx.nAvgBytesPerSec / 10;
        dsbd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;
        dsbd.dwBufferBytes = m_bufferSize;
        dsbd.lpwfxFormat = &wfx;

        if (FAILED(m_pDS->CreateSoundBuffer(&dsbd, &m_pSecondary, NULL))) return false;

        ClearBuffer();
        m_pSecondary->Play(0, 0, DSBPLAY_LOOPING);
        return true;
    }

    void ClearBuffer() {
        void* p1, * p2; DWORD l1, l2;
        if (SUCCEEDED(m_pSecondary->Lock(0, m_bufferSize, &p1, &l1, &p2, &l2, 0))) {
            ZeroMemory(p1, l1);
            if (p2) ZeroMemory(p2, l2);
            m_pSecondary->Unlock(p1, l1, p2, l2);
        }
        m_nextWriteOffset = 0;
    }

    void Pause() { if (m_pSecondary) m_pSecondary->Stop(); }
    void Resume() { if (m_pSecondary) m_pSecondary->Play(0, 0, DSBPLAY_LOOPING); }

    int GetBufferFreeSpace() {
        if (!m_pSecondary) return 0;
        DWORD play, write;
        m_pSecondary->GetCurrentPosition(&play, &write);
        int freeSpace = 0;
        if ((int)play > m_nextWriteOffset) freeSpace = (int)play - m_nextWriteOffset;
        else freeSpace = m_bufferSize - (m_nextWriteOffset - (int)play);
        int safety = 1024;
        if (freeSpace > safety) freeSpace -= safety; else freeSpace = 0;
        return freeSpace;
    }

    void PushSamples(const std::vector<int16_t>& samples) {
        if (!m_pSecondary || samples.empty()) return;
        int size = (int)samples.size() * 2;
        void* p1, * p2; DWORD l1, l2;
        HRESULT hr = m_pSecondary->Lock(m_nextWriteOffset, size, &p1, &l1, &p2, &l2, 0);
        if (hr == DSERR_BUFFERLOST) {
            m_pSecondary->Restore();
            hr = m_pSecondary->Lock(m_nextWriteOffset, size, &p1, &l1, &p2, &l2, 0);
        }
        if (SUCCEEDED(hr)) {
            memcpy(p1, samples.data(), l1);
            if (p2) memcpy(p2, (uint8_t*)samples.data() + l1, l2);
            m_pSecondary->Unlock(p1, l1, p2, l2);
            m_nextWriteOffset = (m_nextWriteOffset + size) % m_bufferSize;
        }
    }
};

// -----------------------------------------------------------------------------
// APU
// -----------------------------------------------------------------------------
class APU {
public:
    Byte regs[0x40];
    Byte waveRam[0x10];
    struct Sweep { int period; int timer; bool enabled; int shadowFreq; };
    struct Channel { bool enabled; int lengthCounter; int envelopeVolume; int envelopeTimer; int freqTimer; int dutyPos; int period; Sweep sweep; } ch1, ch2, ch3, ch4;
    int frameSequencer;
    float accL; float accR; int accCount;
    const int CLOCK_RATE = 4194304;
    std::vector<int16_t> buffer;
    const int dutyPatterns[4][8] = { {0,0,0,0,0,0,0,1}, {1,0,0,0,0,0,0,1}, {1,0,0,0,0,1,1,1}, {0,1,1,1,1,1,1,0} };
    uint16_t lfsr;

    APU() { Reset(); }
    void Reset() {
        memset(regs, 0, sizeof(regs)); memset(waveRam, 0, sizeof(waveRam)); buffer.clear();
        frameSequencer = 0; accL = 0; accR = 0; accCount = 0;
        memset(&ch1, 0, sizeof(Channel)); memset(&ch2, 0, sizeof(Channel));
        memset(&ch3, 0, sizeof(Channel)); memset(&ch4, 0, sizeof(Channel));
        lfsr = 0x7FFF; regs[0x26] = 0xF1;
    }
    Byte Read(Word addr) {
        if (addr >= 0xFF30 && addr <= 0xFF3F) return waveRam[addr - 0xFF30];
        if (addr >= 0xFF10 && addr <= 0xFF3F) {
            int r = addr - 0xFF00; Byte val = regs[r];
            if (addr == 0xFF26) {
                val = (val & 0xF0);
                if (ch1.enabled) val |= 0x01; if (ch2.enabled) val |= 0x02;
                if (ch3.enabled) val |= 0x04; if (ch4.enabled) val |= 0x08;
            }
            return val;
        }
        return 0xFF;
    }
    void Write(Word addr, Byte value) {
        if (addr >= 0xFF30 && addr <= 0xFF3F) { waveRam[addr - 0xFF30] = value; return; }
        if (addr >= 0xFF10 && addr <= 0xFF3F) {
            int r = addr - 0xFF00; regs[r] = value;
            if (r == 0x14 && (value & 0x80)) TriggerCh1();
            if (r == 0x19 && (value & 0x80)) TriggerCh2();
            if (r == 0x1E && (value & 0x80)) TriggerCh3();
            if (r == 0x23 && (value & 0x80)) TriggerCh4();
            if (r == 0x26) { if (!(value & 0x80)) { Reset(); regs[0x26] = 0x00; } }
        }
    }
    int CalcNewFreq() {
        int shift = regs[0x10] & 0x07; int diff = ch1.sweep.shadowFreq >> shift;
        int newFreq = ch1.sweep.shadowFreq; if (regs[0x10] & 0x08) newFreq -= diff; else newFreq += diff;
        return newFreq;
    }
    void TriggerCh1() {
        ch1.enabled = true; int lenVal = regs[0x11] & 0x3F; ch1.lengthCounter = lenVal ? (64 - lenVal) : 64;
        ch1.envelopeVolume = (regs[0x12] >> 4); ch1.envelopeTimer = (regs[0x12] & 0x07);
        ch1.period = (2048 - ((regs[0x14] & 7) << 8 | regs[0x13])) * 4; ch1.freqTimer = ch1.period;
        int sweepPeriod = (regs[0x10] >> 4) & 0x07; int sweepShift = regs[0x10] & 0x07;
        ch1.sweep.period = sweepPeriod ? sweepPeriod : 8; ch1.sweep.timer = ch1.sweep.period;
        ch1.sweep.shadowFreq = ((regs[0x14] & 7) << 8 | regs[0x13]);
        ch1.sweep.enabled = (sweepPeriod > 0 || sweepShift > 0);
        if (sweepShift > 0) { if (CalcNewFreq() > 2047) ch1.enabled = false; }
    }
    void TriggerCh2() {
        ch2.enabled = true; int lenVal = regs[0x16] & 0x3F; ch2.lengthCounter = lenVal ? (64 - lenVal) : 64;
        ch2.envelopeVolume = (regs[0x17] >> 4); ch2.envelopeTimer = (regs[0x17] & 0x07);
        ch2.period = (2048 - ((regs[0x19] & 7) << 8 | regs[0x18])) * 4; ch2.freqTimer = ch2.period;
    }
    void TriggerCh3() {
        ch3.enabled = true; int lenVal = regs[0x1B]; ch3.lengthCounter = (256 - lenVal);
        ch3.period = (2048 - ((regs[0x1E] & 7) << 8 | regs[0x1D])) * 2; ch3.freqTimer = ch3.period; ch3.dutyPos = 0;
    }
    void TriggerCh4() {
        ch4.enabled = true; int lenVal = regs[0x20] & 0x3F; ch4.lengthCounter = lenVal ? (64 - lenVal) : 64;
        ch4.envelopeVolume = (regs[0x21] >> 4); ch4.envelopeTimer = (regs[0x21] & 0x07); lfsr = 0x7FFF;
    }
    void Step(int cycles) {
        frameSequencer += cycles;
        if (frameSequencer >= 8192) {
            frameSequencer -= 8192; static int step = 0; step = (step + 1) & 7;
            if (!(step & 1)) {
                if ((regs[0x14] & 0x40) && ch1.lengthCounter > 0 && --ch1.lengthCounter == 0) ch1.enabled = false;
                if ((regs[0x19] & 0x40) && ch2.lengthCounter > 0 && --ch2.lengthCounter == 0) ch2.enabled = false;
                if ((regs[0x1E] & 0x40) && ch3.lengthCounter > 0 && --ch3.lengthCounter == 0) ch3.enabled = false;
                if ((regs[0x23] & 0x40) && ch4.lengthCounter > 0 && --ch4.lengthCounter == 0) ch4.enabled = false;
            }
            if (step == 2 || step == 6) {
                if (ch1.sweep.enabled && ch1.enabled) {
                    if (--ch1.sweep.timer <= 0) {
                        int period = (regs[0x10] >> 4) & 0x07; ch1.sweep.timer = period ? period : 8;
                        if (period > 0) {
                            int newFreq = CalcNewFreq();
                            if (newFreq <= 2047 && (regs[0x10] & 0x07) > 0) {
                                ch1.sweep.shadowFreq = newFreq; regs[0x13] = newFreq & 0xFF; regs[0x14] = (regs[0x14] & 0xF8) | ((newFreq >> 8) & 0x07);
                                ch1.period = (2048 - newFreq) * 4; if (CalcNewFreq() > 2047) ch1.enabled = false;
                            }
                            else if (newFreq > 2047) ch1.enabled = false;
                        }
                    }
                }
            }
            if (step == 7) {
                auto DoEnv = [&](Channel& ch, Byte reg) {
                    if (ch.enabled && (reg & 7) && --ch.envelopeTimer <= 0) {
                        ch.envelopeTimer = reg & 7; if (reg & 8) { if (ch.envelopeVolume < 15) ch.envelopeVolume++; }
                        else { if (ch.envelopeVolume > 0) ch.envelopeVolume--; }
                    }
                    };
                DoEnv(ch1, regs[0x12]); DoEnv(ch2, regs[0x17]); DoEnv(ch4, regs[0x21]);
            }
        }
        int s1 = 0; if (ch1.enabled) { ch1.freqTimer -= cycles; if (ch1.freqTimer <= 0) { ch1.freqTimer += (2048 - ((regs[0x14] & 7) << 8 | regs[0x13])) * 4; ch1.dutyPos = (ch1.dutyPos + 1) & 7; } if (dutyPatterns[regs[0x11] >> 6][ch1.dutyPos]) s1 = ch1.envelopeVolume; }
        int s2 = 0; if (ch2.enabled) { ch2.freqTimer -= cycles; if (ch2.freqTimer <= 0) { ch2.freqTimer += (2048 - ((regs[0x19] & 7) << 8 | regs[0x18])) * 4; ch2.dutyPos = (ch2.dutyPos + 1) & 7; } if (dutyPatterns[regs[0x16] >> 6][ch2.dutyPos]) s2 = ch2.envelopeVolume; }
        int s3 = 0; if (ch3.enabled && (regs[0x1A] & 0x80)) { ch3.freqTimer -= cycles; if (ch3.freqTimer <= 0) { ch3.freqTimer += (2048 - ((regs[0x1E] & 7) << 8 | regs[0x1D])) * 2; ch3.dutyPos = (ch3.dutyPos + 1) & 31; } Byte b = waveRam[ch3.dutyPos / 2]; int s = (ch3.dutyPos % 2 == 0) ? (b >> 4) : (b & 0xF); int vCode = (regs[0x1C] >> 5) & 3; if (vCode == 0) s3 = 0; else if (vCode == 1) s3 = s; else if (vCode == 2) s3 = s >> 1; else if (vCode == 3) s3 = s >> 2; }
        int s4 = 0; if (ch4.enabled) { int divCode = regs[0x22] & 7; int shift = (regs[0x22] >> 4) & 0xF; int timerPeriod = (divCode ? (divCode << 4) : 8) << shift; static int c4 = 0; c4 += cycles; while (c4 >= timerPeriod) { c4 -= timerPeriod; int xorBit = (lfsr & 1) ^ ((lfsr >> 1) & 1); lfsr >>= 1; lfsr |= (xorBit << 14); if (regs[0x22] & 8) { lfsr &= ~(1 << 6); lfsr |= (xorBit << 6); } } if (!(lfsr & 1)) s4 = ch4.envelopeVolume; }
        int l = 0, r = 0; Byte nr51 = regs[0x25];
        if (nr51 & 0x01) r += s1; if (nr51 & 0x10) l += s1; if (nr51 & 0x02) r += s2; if (nr51 & 0x20) l += s2;
        if (nr51 & 0x04) r += s3; if (nr51 & 0x40) l += s3; if (nr51 & 0x08) r += s4; if (nr51 & 0x80) l += s4;
        Byte nr50 = regs[0x24]; int volL = (nr50 >> 4) & 7; int volR = nr50 & 7; l = l * (volL + 1); r = r * (volR + 1);
        accL += l * cycles; accR += r * cycles; accCount += cycles;
        const int CYCLES_PER_SAMPLE = CLOCK_RATE / SAMPLE_RATE;
        while (accCount >= CYCLES_PER_SAMPLE) { buffer.push_back((int16_t)((accL / CYCLES_PER_SAMPLE) * 64)); buffer.push_back((int16_t)((accR / CYCLES_PER_SAMPLE) * 64)); accL -= (int)(accL / CYCLES_PER_SAMPLE) * CYCLES_PER_SAMPLE; accR -= (int)(accR / CYCLES_PER_SAMPLE) * CYCLES_PER_SAMPLE; accCount -= CYCLES_PER_SAMPLE; }
    }
};

// -----------------------------------------------------------------------------
// MMU
// -----------------------------------------------------------------------------
class MMU
{
public:
    std::vector<Byte> rom;
    std::vector<Byte> vram;
    std::vector<Byte> wram;
    std::vector<Byte> hram;
    std::vector<Byte> io;
    std::vector<Byte> oam;
    std::vector<Byte> sram;

    Byte interruptFlag;
    Byte interruptEnable;

    int mbcType;
    bool ramEnable;
    bool hasBattery;
    int romBank;
    int ramBank;
    int bankingMode;

    int divCounter;
    int tacCounter;

    bool rtcMapped;
    Byte rtcS, rtcM, rtcH, rtcDL, rtcDH, rtcLatch;
    time_t lastTime;

    Byte joypadButtons;
    Byte joypadDir;

    APU* apu;

    MMU() : apu(nullptr) { Reset(); }
    void SetAPU(APU* p) { apu = p; }

    void Reset() {
        vram.assign(0x2000, 0); wram.assign(0x2000, 0); hram.assign(0x80, 0); io.assign(0x80, 0); oam.assign(0xA0, 0); sram.assign(0x20000, 0);
        if (rom.size() < 0x8000) rom.resize(0x8000, 0);
        interruptFlag = 0; interruptEnable = 0; mbcType = 0; ramEnable = false; romBank = 1; ramBank = 0; bankingMode = 0;
        divCounter = 0; tacCounter = 0; rtcMapped = false; rtcS = rtcM = rtcH = rtcDL = rtcDH = rtcLatch = 0; lastTime = time(NULL);
        joypadButtons = 0x0F; joypadDir = 0x0F; hasBattery = false;
    }

    void LoadRomData(const std::vector<Byte>& data) {
        rom = data;
        if (rom.size() < 0x8000) rom.resize(0x8000, 0);
        if (sram.size() < 0x20000) sram.resize(0x20000, 0);
        std::fill(sram.begin(), sram.end(), 0);

        Byte type = rom[0x0147];
        if (type == 0x05 || type == 0x06) mbcType = 2;
        else if (type >= 0x0F && type <= 0x13) mbcType = 3;
        else if (type >= 0x01 && type <= 0x03) mbcType = 1;
        else if (type >= 0x19 && type <= 0x1E) mbcType = 5;
        else mbcType = 0;

        hasBattery = (type == 0x03 || type == 0x06 || type == 0x09 || type == 0x0F || type == 0x10 || type == 0x13 || type == 0x1B || type == 0x1E || type == 0xFF);
        ramEnable = false; romBank = 1; ramBank = 0; bankingMode = 0; rtcMapped = false;
    }

    void LoadRAM(const std::wstring& path) {
        if (!hasBattery) return;
        FILE* fp = NULL; _wfopen_s(&fp, path.c_str(), L"rb");
        if (fp) { fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET); if (size > 0) { if (size > (long)sram.size()) sram.resize(size); fread(sram.data(), 1, size, fp); } fclose(fp); }
    }
    void SaveRAM(const std::wstring& path) {
        if (!hasBattery) return;
        FILE* fp = NULL; _wfopen_s(&fp, path.c_str(), L"wb"); if (fp) { fwrite(sram.data(), 1, sram.size(), fp); fclose(fp); }
    }

    void RequestInterrupt(int bit) { interruptFlag |= (1 << bit); }

    void DoDMA(Byte value) {
        Word srcBase = value << 8; for (int i = 0; i < 0xA0; i++) oam[i] = Read(srcBase + i);
    }

    void UpdateRTC() {
        if (mbcType != 3) return;
        time_t now = time(NULL);
        if (now > lastTime) {
            lastTime = now;
            if (!(rtcDH & 0x40)) {
                rtcS++; if (rtcS >= 60) { rtcS = 0; rtcM++; } if (rtcM >= 60) { rtcM = 0; rtcH++; } if (rtcH >= 24) { rtcH = 0; rtcDL++; if (rtcDL == 0) rtcDH |= 1; }
            }
        }
    }
    void UpdateTimers(int cycles) {
        divCounter += cycles; while (divCounter >= 256) { io[0x04]++; divCounter -= 256; }
        Byte tac = io[0x07];
        if (tac & 0x04) {
            tacCounter += cycles; int threshold = 1024;
            switch (tac & 0x03) { case 0: threshold = 1024; break; case 1: threshold = 16; break; case 2: threshold = 64; break; case 3: threshold = 256; break; }
                                        while (tacCounter >= threshold) { tacCounter -= threshold; Byte tima = io[0x05]; if (tima == 0xFF) { io[0x05] = io[0x06]; RequestInterrupt(2); } else { io[0x05]++; } }
        }
    }

    void CheckJoypadInterrupt() {
        Byte select = io[0x00];
        bool req = false;
        if (!(select & 0x10)) { // Directions selected
            if ((joypadDir & 0x0F) != 0x0F) req = true;
        }
        if (!(select & 0x20)) { // Buttons selected
            if ((joypadButtons & 0x0F) != 0x0F) req = true;
        }
        if (req) RequestInterrupt(4);
    }

    Byte GetJoypadState() {
        Byte select = io[0x00]; Byte result = 0xCF | select;
        if (!(select & 0x10)) result &= (0xF0 | joypadDir);
        if (!(select & 0x20)) result &= (0xF0 | joypadButtons);
        return result;
    }

    Byte Read(Word addr) {
        if (addr < 0x4000) return rom[addr];
        if (addr < 0x8000) {
            int bank = romBank;
            if (mbcType == 1 && bankingMode == 0) bank |= (ramBank << 5);
            int maxBanks = (int)(rom.size() / 0x4000); if (maxBanks == 0) maxBanks = 1; bank %= maxBanks;
            return rom[(bank * 0x4000) + (addr - 0x4000)];
        }
        if (addr < 0xA000) return vram[addr - 0x8000];
        if (addr < 0xC000) {
            if (!ramEnable) return 0xFF;
            if (mbcType == 3 && rtcMapped) {
            switch (ramBank) { case 0x08: return rtcS; case 0x09: return rtcM; case 0x0A: return rtcH; case 0x0B: return rtcDL; case 0x0C: return rtcDH; default: return 0xFF; }
            }
            else if (mbcType == 2) { if (addr < 0xA200) return (sram[addr - 0xA000] & 0x0F); return 0xFF; }
            else {
                int bank = (mbcType == 3 || mbcType == 5) ? ramBank : ((bankingMode == 1) ? ramBank : 0);
                int idx = (bank * 0x2000) + (addr - 0xA000); if (idx < sram.size()) return sram[idx]; return 0xFF;
            }
        }
        if (addr < 0xE000) return wram[addr - 0xC000];
        if (addr < 0xFE00) return wram[addr - 0xE000];
        if (addr < 0xFEA0) return oam[addr - 0xFE00];
        if (addr < 0xFF00) return 0xFF;
        if (addr == 0xFF00) return GetJoypadState();
        if (addr == 0xFF0F) return interruptFlag;
        if (addr >= 0xFF10 && addr <= 0xFF3F) { if (apu) return apu->Read(addr); return 0xFF; }
        if (addr < 0xFF80) return io[addr - 0xFF00];
        if (addr < 0xFFFF) return hram[addr - 0xFF80];
        if (addr == 0xFFFF) return interruptEnable;
        return 0xFF;
    }

    void Write(Word addr, Byte value) {
        if (addr < 0x8000) {
            if (mbcType == 1) {
                if (addr < 0x2000) ramEnable = ((value & 0x0F) == 0x0A);
                else if (addr < 0x4000) { romBank = value & 0x1F; if (romBank == 0) romBank = 1; }
                else if (addr < 0x6000) ramBank = value & 0x03;
                else if (addr < 0x8000) bankingMode = value & 0x01;
            }
            else if (mbcType == 2) {
                if (addr < 0x4000) { if (addr & 0x0100) { romBank = value & 0x0F; if (romBank == 0) romBank = 1; } else ramEnable = ((value & 0x0F) == 0x0A); }
            }
            else if (mbcType == 3) {
                if (addr < 0x2000) ramEnable = ((value & 0x0F) == 0x0A); else if (addr < 0x4000) { romBank = value & 0x7F; if (romBank == 0) romBank = 1; }
                else if (addr < 0x6000) { ramBank = value; rtcMapped = (value >= 0x08 && value <= 0x0C); }
                else if (addr < 0x8000) rtcLatch = value;
            }
            else if (mbcType == 5) {
                if (addr < 0x2000) ramEnable = ((value & 0x0F) == 0x0A); else if (addr < 0x3000) romBank = (romBank & 0x100) | value; else if (addr < 0x4000) romBank = (romBank & 0x0FF) | ((value & 0x01) << 8); else if (addr < 0x6000) ramBank = value & 0x0F;
            }
            return;
        }
        if (addr < 0xA000) { vram[addr - 0x8000] = value; return; }
        if (addr < 0xC000) {
            if (ramEnable) {
                if (mbcType == 3 && rtcMapped) { /* RTC Write */ }
                else if (mbcType == 2) { if (addr < 0xA200) sram[addr - 0xA000] = value & 0x0F; }
                else {
                    int bank = (mbcType == 3 || mbcType == 5) ? ramBank : ((bankingMode == 1) ? ramBank : 0); int idx = (bank * 0x2000) + (addr - 0xA000); if (idx < sram.size()) sram[idx] = value;
                }
            }
            return;
        }
        if (addr < 0xE000) { wram[addr - 0xC000] = value; return; }
        if (addr < 0xFE00) { wram[addr - 0xE000] = value; return; }
        if (addr < 0xFEA0) { oam[addr - 0xFE00] = value; return; }
        if (addr < 0xFF00) return;

        if (addr == 0xFF00) { io[0x00] = value; CheckJoypadInterrupt(); return; }
        if (addr == 0xFF04) { io[0x04] = 0; divCounter = 0; return; }
        if (addr == 0xFF0F) { interruptFlag = value; return; }
        if (addr == 0xFF46) { DoDMA(value); return; }
        if (addr == 0xFF41) { io[0x41] = (value & 0xF8) | (io[0x41] & 0x07); return; }
        if (addr == 0xFF44) { io[0x44] = 0; return; }

        if (addr >= 0xFF10 && addr <= 0xFF3F) { if (apu) apu->Write(addr, value); return; }
        if (addr < 0xFF80) { io[addr - 0xFF00] = value; return; }
        if (addr < 0xFFFF) { hram[addr - 0xFF80] = value; return; }
        if (addr == 0xFFFF) { interruptEnable = value; return; }
    }

    void SetKey(int keyId, bool pressed) {
        Byte* target = (keyId < 4) ? &joypadDir : &joypadButtons;
        int bit = keyId % 4;
        Byte oldVal = *target;
        if (pressed) *target &= ~(1 << bit); else *target |= (1 << bit);

        if (pressed && (oldVal & (1 << bit))) {
            CheckJoypadInterrupt();
        }
    }

    std::string GetTitle() {
        if (rom.size() < 0x143) return "";
        char buf[17] = { 0 }; for (int i = 0; i < 16; i++) { char c = rom[0x0134 + i]; if (c == 0) break; buf[i] = c; }
        return std::string(buf);
    }
    std::string GetMBCName() {
        if (mbcType == 1) return "MBC1"; if (mbcType == 2) return "MBC2"; if (mbcType == 3) return "MBC3"; if (mbcType == 5) return "MBC5"; return "ROM ONLY";
    }
};

// -----------------------------------------------------------------------------
// PPU
// -----------------------------------------------------------------------------
class PPU {
private:
    MMU* mmu; uint32_t* screenBuffer; int cycleCounter; int mode; int windowLine; bool statIntSignal;
    Byte latchSCX, latchSCY, latchBGP, latchOBP0, latchOBP1, latchLCDC, latchWY; int latchWX;
    const uint32_t PALETTE[4] = { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 };
public:
    PPU(MMU* m) : mmu(m), screenBuffer(nullptr), cycleCounter(0), mode(2), windowLine(0), statIntSignal(false) {}
    void Reset() {
        cycleCounter = 0; mode = 2; windowLine = 0; statIntSignal = false;
        if (mmu) mmu->io[0x41] = (mmu->io[0x41] & 0xFC) | 2;
    }
    void SetScreenBuffer(uint32_t* buffer) { screenBuffer = buffer; }
    Byte GetLY() { return mmu->io[0x44]; }
    void SetLY(Byte v) { mmu->io[0x44] = v; }
    Byte GetLCDC() { return mmu->io[0x40]; }
    Byte GetSTAT() { return mmu->io[0x41]; }
    void SetSTAT(Byte v) { mmu->io[0x41] = v; }
    Byte GetLYC() { return mmu->io[0x45]; }

    void Step(int cycles) {
        Byte lcdc = GetLCDC();
        if (!(lcdc & 0x80)) {
            SetLY(0); cycleCounter = 0; mode = 2; windowLine = 0; statIntSignal = false;
            SetSTAT(GetSTAT() & 0xFC); return;
        }

        cycleCounter += cycles; Byte ly = GetLY(); Byte stat = GetSTAT();
        bool lycMatch = (ly == GetLYC());
        if (lycMatch) stat |= 0x04; else stat &= ~0x04;

        if (mode == 2) {
            if (cycleCounter >= 80) {
                cycleCounter -= 80; mode = 3;
                latchSCX = mmu->io[0x43]; latchSCY = mmu->io[0x42]; latchBGP = mmu->io[0x47];
                latchOBP0 = mmu->io[0x48]; latchOBP1 = mmu->io[0x49]; latchLCDC = mmu->io[0x40];
                latchWY = mmu->io[0x4A]; latchWX = (int)mmu->io[0x4B] - 7;
            }
        }
        else if (mode == 3) {
            if (cycleCounter >= 172) { cycleCounter -= 172; mode = 0; RenderScanline(ly); }
        }
        else if (mode == 0) {
            if (cycleCounter >= 204) {
                cycleCounter -= 204; ly++; SetLY(ly);
                if (ly == 144) { mode = 1; mmu->RequestInterrupt(0); windowLine = 0; }
                else { mode = 2; }
            }
        }
        else if (mode == 1) {
            if (cycleCounter >= 456) {
                cycleCounter -= 456; ly++;
                if (ly > 153) { mode = 2; ly = 0; windowLine = 0; }
                SetLY(ly);
            }
        }
        stat = (stat & 0xFC) | (mode & 0x03); SetSTAT(stat);

        bool currentSignal = false;
        if ((stat & 0x40) && lycMatch) currentSignal = true;
        if ((stat & 0x20) && (mode == 2)) currentSignal = true;
        if ((stat & 0x10) && (mode == 1)) currentSignal = true;
        if ((stat & 0x08) && (mode == 0)) currentSignal = true;
        if (currentSignal && !statIntSignal) mmu->RequestInterrupt(1);
        statIntSignal = currentSignal;
    }

    void RenderScanline(int line) {
        if (!screenBuffer) return;
        Byte lcdc = latchLCDC; if (!(lcdc & 0x01)) return;
        Byte scy = latchSCY; Byte scx = latchSCX; Byte bgp = latchBGP; Byte wy = latchWY; int wx = latchWX;
        uint32_t palette[4]; for (int i = 0; i < 4; i++) palette[i] = PALETTE[(bgp >> (i * 2)) & 3];
        Word mapBase = (lcdc & 0x08) ? 0x9C00 : 0x9800; Word tileBase = (lcdc & 0x10) ? 0x8000 : 0x9000; bool unsignedTile = (lcdc & 0x10);

        Byte mapY = line + scy;
        for (int x = 0; x < 160; ++x) {
            Byte mapX = x + scx; Word tileIdxAddr = mapBase + (mapY / 8) * 32 + (mapX / 8); Byte tileIdx = mmu->Read(tileIdxAddr);
            Word tileAddr = unsignedTile ? tileBase + (tileIdx * 16) : tileBase + (static_cast<int8_t>(tileIdx) * 16);
            Byte row = mapY % 8; Byte b1 = mmu->Read(tileAddr + row * 2); Byte b2 = mmu->Read(tileAddr + row * 2 + 1);
            int bit = 7 - (mapX % 8); int colorId = ((b1 >> bit) & 1) | (((b2 >> bit) & 1) << 1);
            screenBuffer[line * 160 + x] = palette[colorId];
        }

        if ((lcdc & 0x20) && (line >= wy) && (wx <= 159)) {
            Word winMapBase = (lcdc & 0x40) ? 0x9C00 : 0x9800; Byte winY = (Byte)windowLine;
            for (int x = 0; x < 160; ++x) {
                if (x >= wx) {
                    int winX = x - wx; Word tileIdxAddr = winMapBase + (winY / 8) * 32 + (winX / 8); Byte tileIdx = mmu->Read(tileIdxAddr);
                    Word tileAddr = unsignedTile ? tileBase + (tileIdx * 16) : tileBase + (static_cast<int8_t>(tileIdx) * 16);
                    Byte row = winY % 8; Byte b1 = mmu->Read(tileAddr + row * 2); Byte b2 = mmu->Read(tileAddr + row * 2 + 1);
                    int bit = 7 - (winX % 8); int colorId = ((b1 >> bit) & 1) | (((b2 >> bit) & 1) << 1);
                    screenBuffer[line * 160 + x] = palette[colorId];
                }
            }
            windowLine++;
        }

        if (!(lcdc & 0x02)) return;
        Byte obp0 = latchOBP0; Byte obp1 = latchOBP1; uint32_t palObj0[4], palObj1[4];
        for (int i = 0; i < 4; i++) { palObj0[i] = PALETTE[(obp0 >> (i * 2)) & 3]; palObj1[i] = PALETTE[(obp1 >> (i * 2)) & 3]; }
        int height = (lcdc & 0x04) ? 16 : 8;
        for (int i = 0; i < 40; i++) {
            Byte y = mmu->oam[i * 4]; Byte x = mmu->oam[i * 4 + 1]; Byte tile = mmu->oam[i * 4 + 2]; Byte attr = mmu->oam[i * 4 + 3];
            int spriteY = line - (y - 16); if (spriteY < 0 || spriteY >= height) continue;
            if (attr & 0x40) spriteY = height - 1 - spriteY; if (height == 16) tile &= 0xFE;
            Word tileAddr = 0x8000 + (tile * 16) + (spriteY * 2); Byte b1 = mmu->Read(tileAddr); Byte b2 = mmu->Read(tileAddr + 1);
            uint32_t* pal = (attr & 0x10) ? palObj1 : palObj0;
            for (int px = 0; px < 8; px++) {
                int screenX = (x - 8) + px; if (screenX < 0 || screenX >= 160) continue;
                if ((attr & 0x80) && screenBuffer[line * 160 + screenX] != PALETTE[0]) continue;
                int bit = (attr & 0x20) ? px : (7 - px); int colorId = ((b1 >> bit) & 1) | (((b2 >> bit) & 1) << 1);
                if (colorId == 0) continue; screenBuffer[line * 160 + screenX] = pal[colorId];
            }
        }
    }
};

// -----------------------------------------------------------------------------
// CPU
// -----------------------------------------------------------------------------
class CPU {
public:
    struct Registers {
        struct { union { struct { Byte f; Byte a; }; Word af; }; } af;
        struct { union { struct { Byte c; Byte b; }; Word bc; }; } bc;
        struct { union { struct { Byte e; Byte d; }; Word de; }; } de;
        struct { union { struct { Byte l; Byte h; }; Word hl; }; } hl;
        Word sp, pc; bool ime; int imeDelay;
    } reg;
    MMU* mmu; bool halted, haltBugTriggered; int currentCycles;

    CPU(MMU* m) : mmu(m) { Reset(); }
    void Reset() { reg.af.af = 0x01B0; reg.bc.bc = 0x0013; reg.de.de = 0x00D8; reg.hl.hl = 0x014D; reg.sp = 0xFFFE; reg.pc = 0x0100; reg.ime = false; reg.imeDelay = 0; halted = false; haltBugTriggered = false; currentCycles = 0; }
    Byte Read(Word addr) { currentCycles += 4; return mmu->Read(addr); }
    void Write(Word addr, Byte val) { currentCycles += 4; mmu->Write(addr, val); }
    Byte Fetch() { Byte val = Read(reg.pc); if (haltBugTriggered) haltBugTriggered = false; else reg.pc++; return val; }
    Word Fetch16() { Byte l = Fetch(); Byte h = Fetch(); return (h << 8) | l; }
    void Push(Word val) { reg.sp--; Write(reg.sp, val >> 8); reg.sp--; Write(reg.sp, val & 0xFF); }
    Word Pop() { Byte l = Read(reg.sp++); Byte h = Read(reg.sp++); return (h << 8) | l; }
    void F_Z(bool z) { if (z) reg.af.f |= 0x80; else reg.af.f &= ~0x80; }
    void F_N(bool n) { if (n) reg.af.f |= 0x40; else reg.af.f &= ~0x40; }
    void F_H(bool h) { if (h) reg.af.f |= 0x20; else reg.af.f &= ~0x20; }
    void F_C(bool c) { if (c) reg.af.f |= 0x10; else reg.af.f &= ~0x10; }
    bool IsZ() const { return reg.af.f & 0x80; } bool IsC() const { return reg.af.f & 0x10; }
    Byte GetR8(int idx) { switch (idx) { case 0: return reg.bc.b; case 1: return reg.bc.c; case 2: return reg.de.d; case 3: return reg.de.e; case 4: return reg.hl.h; case 5: return reg.hl.l; case 6: return Read(reg.hl.hl); case 7: return reg.af.a; } return 0; }
    void SetR8(int idx, Byte val) { switch (idx) { case 0: reg.bc.b = val; break; case 1: reg.bc.c = val; break; case 2: reg.de.d = val; break; case 3: reg.de.e = val; break; case 4: reg.hl.h = val; break; case 5: reg.hl.l = val; break; case 6: Write(reg.hl.hl, val); break; case 7: reg.af.a = val; break; } }
    Word GetR16(int idx, bool af = false) { switch (idx) { case 0: return reg.bc.bc; case 1: return reg.de.de; case 2: return reg.hl.hl; case 3: return af ? reg.af.af : reg.sp; } return 0; }
    void SetR16(int idx, Word val, bool af = false) { switch (idx) { case 0: reg.bc.bc = val; break; case 1: reg.de.de = val; break; case 2: reg.hl.hl = val; break; case 3: if (af) { reg.af.af = val; reg.af.f &= 0xF0; } else { reg.sp = val; } break; } }
                                                                           void ALU_ADD(Byte v) { int r = reg.af.a + v; F_Z((r & 0xFF) == 0); F_N(0); F_H((reg.af.a & 0xF) + (v & 0xF) > 0xF); F_C(r > 0xFF); reg.af.a = (Byte)r; }
                                                                           void ALU_ADC(Byte v) { int c = IsC() ? 1 : 0; int r = reg.af.a + v + c; F_Z((r & 0xFF) == 0); F_N(0); F_H((reg.af.a & 0xF) + (v & 0xF) + c > 0xF); F_C(r > 0xFF); reg.af.a = (Byte)r; }
                                                                           void ALU_SUB(Byte v) { int r = reg.af.a - v; F_Z((r & 0xFF) == 0); F_N(1); F_H((reg.af.a & 0xF) < (v & 0xF)); F_C(reg.af.a < v); reg.af.a = (Byte)r; }
                                                                           void ALU_SBC(Byte v) { int c = IsC() ? 1 : 0; int r = reg.af.a - v - c; F_Z((r & 0xFF) == 0); F_N(1); F_H((reg.af.a & 0xF) < (v & 0xF) + c); F_C(r < 0); reg.af.a = (Byte)r; }
                                                                           void ALU_AND(Byte v) { reg.af.a &= v; F_Z(reg.af.a == 0); F_N(0); F_H(1); F_C(0); }
                                                                           void ALU_XOR(Byte v) { reg.af.a ^= v; F_Z(reg.af.a == 0); F_N(0); F_H(0); F_C(0); }
                                                                           void ALU_OR(Byte v) { reg.af.a |= v; F_Z(reg.af.a == 0); F_N(0); F_H(0); F_C(0); }
                                                                           void ALU_CP(Byte v) { int r = reg.af.a - v; F_Z((r & 0xFF) == 0); F_N(1); F_H((reg.af.a & 0xF) < (v & 0xF)); F_C(reg.af.a < v); }
                                                                           void ALU_INC(Byte& v) { v++; F_Z(v == 0); F_N(0); F_H((v & 0xF) == 0); }
                                                                           void ALU_DEC(Byte& v) { v--; F_Z(v == 0); F_N(1); F_H((v & 0xF) == 0xF); }
                                                                           void RLC(Byte& v) { int c = v >> 7; v = (v << 1) | c; F_Z(v == 0); F_N(0); F_H(0); F_C(c); }
                                                                           void RRC(Byte& v) { int c = v & 1; v = (v >> 1) | (c << 7); F_Z(v == 0); F_N(0); F_H(0); F_C(c); }
                                                                           void RL(Byte& v) { int c = v >> 7; v = (v << 1) | (IsC() ? 1 : 0); F_Z(v == 0); F_N(0); F_H(0); F_C(c); }
                                                                           void RR(Byte& v) { int c = v & 1; v = (v >> 1) | (IsC() ? 0x80 : 0); F_Z(v == 0); F_N(0); F_H(0); F_C(c); }
                                                                           void SLA(Byte& v) { int c = v >> 7; v <<= 1; F_Z(v == 0); F_N(0); F_H(0); F_C(c); }
                                                                           void SRA(Byte& v) { int c = v & 1; v = (v >> 1) | (v & 0x80); F_Z(v == 0); F_N(0); F_H(0); F_C(c); }
                                                                           void SWAP(Byte& v) { v = (v << 4) | (v >> 4); F_Z(v == 0); F_N(0); F_H(0); F_C(0); }
                                                                           void SRL(Byte& v) { int c = v & 1; v >>= 1; F_Z(v == 0); F_N(0); F_H(0); F_C(c); }
                                                                           void BIT(int b, Byte v) { F_Z(!(v & (1 << b))); F_N(0); F_H(1); }
                                                                           void ExecCB() {
                                                                               Byte op = Fetch(); Byte r = op & 0x07; Byte val = GetR8(r);
                                                                               if (op < 0x40) {
    switch ((op >> 3) & 7) { case 0: RLC(val); break; case 1: RRC(val); break; case 2: RL(val); break; case 3: RR(val); break; case 4: SLA(val); break; case 5: SRA(val); break; case 6: SWAP(val); break; case 7: SRL(val); break; } SetR8(r, val);
                                                                               }
                                                                               else {
                                                                                   int bit = (op >> 3) & 7; if (op < 0x80) BIT(bit, val); else if (op < 0xC0) { val &= ~(1 << bit); SetR8(r, val); }
                                                                                   else { val |= (1 << bit); SetR8(r, val); }
                                                                               }
                                                                           }
                                                                           bool HandleInterrupts() {
                                                                               if (reg.ime && (mmu->interruptFlag & mmu->interruptEnable)) {
                                                                                   Byte fired = mmu->interruptFlag & mmu->interruptEnable;
                                                                                   int bit = 0; if (fired & 0x01) bit = 0; else if (fired & 0x02) bit = 1; else if (fired & 0x04) bit = 2; else if (fired & 0x08) bit = 3; else if (fired & 0x10) bit = 4; else return false;
                                                                                   reg.ime = false; mmu->interruptFlag &= ~(1 << bit); Push(reg.pc); reg.pc = 0x0040 + (bit * 8); currentCycles += 20; return true;
                                                                               } return false;
                                                                           }
                                                                           int Step() {
                                                                               if (reg.imeDelay > 0) { reg.imeDelay--; if (reg.imeDelay == 0) reg.ime = true; }
                                                                               if (HandleInterrupts()) return currentCycles;
                                                                               if (halted) {
                                                                                   currentCycles = 4;
                                                                                   if (mmu->interruptFlag & mmu->interruptEnable) { halted = false; if (!reg.ime) haltBugTriggered = true; }
                                                                                   return currentCycles;
                                                                               }
                                                                               currentCycles = 0; Byte op = Fetch();
                                                                               if ((op & 0xC0) == 0x40) { if (op == 0x76) halted = true; else SetR8((op >> 3) & 7, GetR8(op & 7)); }
                                                                               else if ((op & 0xC0) == 0x80) {
    Byte v = GetR8(op & 7); switch ((op >> 3) & 7) { case 0: ALU_ADD(v); break; case 1: ALU_ADC(v); break; case 2: ALU_SUB(v); break; case 3: ALU_SBC(v); break; case 4: ALU_AND(v); break; case 5: ALU_XOR(v); break; case 6: ALU_OR(v); break; case 7: ALU_CP(v); break; }
                                                                               }
                                                                               else {
                                                                                   switch (op) {
                                                                                   case 0x00: break; case 0x01: SetR16(0, Fetch16()); break; case 0x02: Write(reg.bc.bc, reg.af.a); break; case 0x03: SetR16(0, GetR16(0) + 1); break;
                                                                                   case 0x04: { Byte v = GetR8(0); ALU_INC(v); SetR8(0, v); } break; case 0x05: { Byte v = GetR8(0); ALU_DEC(v); SetR8(0, v); } break; case 0x06: SetR8(0, Fetch()); break; case 0x07: RLC(reg.af.a); F_Z(0); break;
                                                                                   case 0x08: { Word a = Fetch16(); Write(a, reg.sp & 0xFF); Write(a + 1, reg.sp >> 8); } break; case 0x09: { Word v = GetR16(0); Word hl = reg.hl.hl; int r = hl + v; F_N(0); F_H((hl & 0xFFF) + (v & 0xFFF) > 0xFFF); F_C(r > 0xFFFF); reg.hl.hl = (Word)r; } break;
                                                                                   case 0x0A: reg.af.a = Read(reg.bc.bc); break; case 0x0B: SetR16(0, GetR16(0) - 1); break; case 0x0C: { Byte v = GetR8(1); ALU_INC(v); SetR8(1, v); } break; case 0x0D: { Byte v = GetR8(1); ALU_DEC(v); SetR8(1, v); } break; case 0x0E: SetR8(1, Fetch()); break; case 0x0F: RRC(reg.af.a); F_Z(0); break;
                                                                                   case 0x10: Fetch(); halted = true; break; case 0x11: SetR16(1, Fetch16()); break; case 0x12: Write(reg.de.de, reg.af.a); break; case 0x13: SetR16(1, GetR16(1) + 1); break;
                                                                                   case 0x14: { Byte v = GetR8(2); ALU_INC(v); SetR8(2, v); } break; case 0x15: { Byte v = GetR8(2); ALU_DEC(v); SetR8(2, v); } break; case 0x16: SetR8(2, Fetch()); break; case 0x17: RL(reg.af.a); F_Z(0); break;
                                                                                   case 0x18: { SignedByte r = (SignedByte)Fetch(); reg.pc += r; } break; case 0x19: { Word v = GetR16(1); Word hl = reg.hl.hl; int r = hl + v; F_N(0); F_H((hl & 0xFFF) + (v & 0xFFF) > 0xFFF); F_C(r > 0xFFFF); reg.hl.hl = (Word)r; } break;
                                                                                   case 0x1A: reg.af.a = Read(reg.de.de); break; case 0x1B: SetR16(1, GetR16(1) - 1); break; case 0x1C: { Byte v = GetR8(3); ALU_INC(v); SetR8(3, v); } break; case 0x1D: { Byte v = GetR8(3); ALU_DEC(v); SetR8(3, v); } break; case 0x1E: SetR8(3, Fetch()); break; case 0x1F: RR(reg.af.a); F_Z(0); break;
                                                                                   case 0x20: { SignedByte r = (SignedByte)Fetch(); if (!IsZ()) reg.pc += r; } break; case 0x21: SetR16(2, Fetch16()); break; case 0x22: Write(reg.hl.hl++, reg.af.a); break; case 0x23: SetR16(2, GetR16(2) + 1); break;
                                                                                   case 0x24: { Byte v = GetR8(4); ALU_INC(v); SetR8(4, v); } break; case 0x25: { Byte v = GetR8(4); ALU_DEC(v); SetR8(4, v); } break; case 0x26: SetR8(4, Fetch()); break; case 0x27: { int a = reg.af.a; if (!(reg.af.f & 0x40)) { if ((reg.af.f & 0x10) || a > 0x99) { a += 0x60; F_C(1); }if ((reg.af.f & 0x20) || (a & 0xF) > 9)a += 6; } else { if (reg.af.f & 0x10)a -= 0x60; if (reg.af.f & 0x20)a -= 6; } F_Z((a & 0xFF) == 0); F_H(0); reg.af.a = a; } break;
                                                                                   case 0x28: { SignedByte r = (SignedByte)Fetch(); if (IsZ()) reg.pc += r; } break; case 0x29: { Word v = GetR16(2); Word hl = reg.hl.hl; int r = hl + v; F_N(0); F_H((hl & 0xFFF) + (v & 0xFFF) > 0xFFF); F_C(r > 0xFFFF); reg.hl.hl = (Word)r; } break;
                                                                                   case 0x2A: reg.af.a = Read(reg.hl.hl++); break; case 0x2B: SetR16(2, GetR16(2) - 1); break; case 0x2C: { Byte v = GetR8(5); ALU_INC(v); SetR8(5, v); } break; case 0x2D: { Byte v = GetR8(5); ALU_DEC(v); SetR8(5, v); } break; case 0x2E: SetR8(5, Fetch()); break; case 0x2F: reg.af.a ^= 0xFF; F_N(1); F_H(1); break;
                                                                                   case 0x30: { SignedByte r = (SignedByte)Fetch(); if (!IsC()) reg.pc += r; } break; case 0x31: SetR16(3, Fetch16(), false); break; case 0x32: Write(reg.hl.hl--, reg.af.a); break; case 0x33: SetR16(3, GetR16(3, false) + 1, false); break;
                                                                                   case 0x34: { Byte v = Read(reg.hl.hl); ALU_INC(v); Write(reg.hl.hl, v); } break; case 0x35: { Byte v = Read(reg.hl.hl); ALU_DEC(v); Write(reg.hl.hl, v); } break; case 0x36: Write(reg.hl.hl, Fetch()); break; case 0x37: F_N(0); F_H(0); F_C(1); break;
                                                                                   case 0x38: { SignedByte r = (SignedByte)Fetch(); if (IsC()) reg.pc += r; } break; case 0x39: { Word v = GetR16(3, false); Word hl = reg.hl.hl; int r = hl + v; F_N(0); F_H((hl & 0xFFF) + (v & 0xFFF) > 0xFFF); F_C(r > 0xFFFF); reg.hl.hl = (Word)r; } break;
                                                                                   case 0x3A: reg.af.a = Read(reg.hl.hl--); break; case 0x3B: SetR16(3, GetR16(3, false) - 1, false); break; case 0x3C: { Byte v = reg.af.a; ALU_INC(v); reg.af.a = v; } break; case 0x3D: { Byte v = reg.af.a; ALU_DEC(v); reg.af.a = v; } break; case 0x3E: reg.af.a = Fetch(); break; case 0x3F: F_N(0); F_H(0); F_C(!IsC()); break;
                                                                                   case 0xC0: if (!IsZ()) reg.pc = Pop(); else currentCycles -= 4; break; case 0xC1: SetR16(0, Pop()); break; case 0xC2: { Word a = Fetch16(); if (!IsZ()) reg.pc = a; } break; case 0xC3: reg.pc = Fetch16(); break;
                                                                                   case 0xC4: { Word a = Fetch16(); if (!IsZ()) Push(reg.pc), reg.pc = a; } break; case 0xC5: Push(reg.bc.bc); break; case 0xC6: ALU_ADD(Fetch()); break; case 0xC7: Push(reg.pc); reg.pc = 0x00; break;
                                                                                   case 0xC8: if (IsZ()) reg.pc = Pop(); else currentCycles -= 4; break; case 0xC9: reg.pc = Pop(); break; case 0xCA: { Word a = Fetch16(); if (IsZ()) reg.pc = a; } break; case 0xCB: ExecCB(); break;
                                                                                   case 0xCC: { Word a = Fetch16(); if (IsZ()) Push(reg.pc), reg.pc = a; } break; case 0xCD: { Word a = Fetch16(); Push(reg.pc); reg.pc = a; } break; case 0xCE: ALU_ADC(Fetch()); break; case 0xCF: Push(reg.pc); reg.pc = 0x08; break;
                                                                                   case 0xD0: if (!IsC()) reg.pc = Pop(); else currentCycles -= 4; break; case 0xD1: SetR16(1, Pop()); break; case 0xD2: { Word a = Fetch16(); if (!IsC()) reg.pc = a; } break; case 0xD4: { Word a = Fetch16(); if (!IsC()) Push(reg.pc), reg.pc = a; } break;
                                                                                   case 0xD5: Push(reg.de.de); break; case 0xD6: ALU_SUB(Fetch()); break; case 0xD7: Push(reg.pc); reg.pc = 0x10; break; case 0xD8: if (IsC()) reg.pc = Pop(); else currentCycles -= 4; break;
                                                                                   case 0xD9: reg.pc = Pop(); reg.imeDelay = 2; break; case 0xDA: { Word a = Fetch16(); if (IsC()) reg.pc = a; } break; case 0xDC: { Word a = Fetch16(); if (IsC()) Push(reg.pc), reg.pc = a; } break; case 0xDE: ALU_SBC(Fetch()); break; case 0xDF: Push(reg.pc); reg.pc = 0x18; break;
                                                                                   case 0xE0: Write(0xFF00 | Fetch(), reg.af.a); break; case 0xE1: SetR16(2, Pop()); break; case 0xE2: Write(0xFF00 | reg.bc.c, reg.af.a); break; case 0xE5: Push(reg.hl.hl); break;
                                                                                   case 0xE6: ALU_AND(Fetch()); break; case 0xE7: Push(reg.pc); reg.pc = 0x20; break; case 0xE8: { SignedByte r = (SignedByte)Fetch(); Word sp = reg.sp; int res = sp + r; F_Z(0); F_N(0); F_H((sp & 0xF) + (r & 0xF) > 0xF); F_C((sp & 0xFF) + (r & 0xFF) > 0xFF); reg.sp = (Word)res; } break;
                                                                                   case 0xE9: reg.pc = reg.hl.hl; break; case 0xEA: Write(Fetch16(), reg.af.a); break; case 0xEE: ALU_XOR(Fetch()); break; case 0xEF: Push(reg.pc); reg.pc = 0x28; break;
                                                                                   case 0xF0: reg.af.a = Read(0xFF00 | Fetch()); break; case 0xF1: SetR16(3, Pop(), true); break; case 0xF2: reg.af.a = Read(0xFF00 | reg.bc.c); break; case 0xF3: reg.ime = false; break;
                                                                                   case 0xF5: Push(reg.af.af); break; case 0xF6: ALU_OR(Fetch()); break; case 0xF7: Push(reg.pc); reg.pc = 0x30; break; case 0xF8: { SignedByte r = (SignedByte)Fetch(); Word sp = reg.sp; int res = sp + r; F_Z(0); F_N(0); F_H((sp & 0xF) + (r & 0xF) > 0xF); F_C((sp & 0xFF) + (r & 0xFF) > 0xFF); reg.hl.hl = (Word)res; } break;
                                                                                   case 0xF9: reg.sp = reg.hl.hl; break; case 0xFA: reg.af.a = Read(Fetch16()); break; case 0xFB: reg.imeDelay = 2; break; case 0xFE: ALU_CP(Fetch()); break; case 0xFF: Push(reg.pc); reg.pc = 0x38; break;
                                                                                   }
                                                                               }
                                                                               return currentCycles;
                                                                           }
};

// -----------------------------------------------------------------------------
// GameBoyCore
// -----------------------------------------------------------------------------
class GameBoyCore
{
public:
    MMU mmu;
    CPU cpu;
    PPU ppu;
    APU apu;
    std::vector<uint32_t> displayBuffer;
    bool isRomLoaded;
    std::wstring m_savePath;

    GameBoyCore() : cpu(&mmu), ppu(&mmu), isRomLoaded(false) {
        displayBuffer.resize(GB_WIDTH * GB_HEIGHT);
        ppu.SetScreenBuffer(displayBuffer.data());
        mmu.SetAPU(&apu);
        Reset(false);
    }

    void Reset(bool loaded) {
        mmu.Reset();
        cpu.Reset();
        ppu.Reset();
        apu.Reset();
        isRomLoaded = loaded;

        if (!isRomLoaded) {
            SetupTestRender();
        }
        else {
            mmu.io[0x40] = 0x91;
            mmu.io[0x47] = 0xE4;
        }
    }

    void SetupTestRender() {
        if (mmu.rom.size() < 0x200) mmu.rom.resize(0x200, 0);
        mmu.io[0x40] = 0x91;
        mmu.io[0x47] = 0xE4;
        for (int i = 0; i < 0x1800; i++) mmu.vram[i] = (i % 2 == 0) ? 0xFF : 0x00;
        mmu.rom[0x0100] = 0x00; mmu.rom[0x0101] = 0xC3; mmu.rom[0x0102] = 0x00; mmu.rom[0x0103] = 0x01;
    }

    bool LoadRom(const std::wstring& path) {
        FILE* fp = NULL;
        _wfopen_s(&fp, path.c_str(), L"rb");
        if (!fp) return false;

        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        std::vector<Byte> buffer(size);
        if (fread(buffer.data(), 1, size, fp) == size) {
            fclose(fp);
            Reset(true);
            mmu.LoadRomData(buffer);

            m_savePath = path;
            size_t dotPos = m_savePath.find_last_of(L'.');
            if (dotPos != std::string::npos) {
                m_savePath = m_savePath.substr(0, dotPos);
            }
            m_savePath += L".sav";
            mmu.LoadRAM(m_savePath);

            return true;
        }
        fclose(fp);
        return false;
    }

    void SaveRAM() {
        if (isRomLoaded && !m_savePath.empty()) {
            mmu.SaveRAM(m_savePath);
        }
    }

    std::string GetTitle() {
        return mmu.GetTitle() + " (" + mmu.GetMBCName() + ")";
    }

    void StepFrame() {
        const int CYCLES_PER_FRAME = 70224;
        int cyclesThisFrame = 0;

        apu.buffer.clear();

        while (cyclesThisFrame < CYCLES_PER_FRAME) {
            int cycles = cpu.Step();
            ppu.Step(cycles);
            mmu.UpdateRTC();
            mmu.UpdateTimers(cycles);
            apu.Step(cycles);

            cyclesThisFrame += cycles;
        }
    }
    const void* GetPixelData() const { return displayBuffer.data(); }
    void InputKey(int key, bool pressed) { mmu.SetKey(key, pressed); }
    const std::vector<int16_t>& GetAudioSamples() { return apu.buffer; }
};

// -----------------------------------------------------------------------------
// App Class
// -----------------------------------------------------------------------------
class App
{
private:
    HWND m_hwnd;
    ID2D1Factory* m_pDirect2dFactory;
    ID2D1HwndRenderTarget* m_pRenderTarget;
    ID2D1Bitmap* m_pBitmap;
    GameBoyCore m_gbCore;
    AudioDriver m_audio;

public:
    App() : m_hwnd(NULL), m_pDirect2dFactory(NULL), m_pRenderTarget(NULL), m_pBitmap(NULL) {}
    ~App() {
        m_gbCore.SaveRAM();
        SafeRelease(&m_pBitmap); SafeRelease(&m_pRenderTarget); SafeRelease(&m_pDirect2dFactory);
    }

    HRESULT Initialize(HINSTANCE hInstance, int nCmdShow) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pDirect2dFactory);
        WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = App::WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = sizeof(LONG_PTR);
        wcex.hInstance = hInstance;
        wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wcex.lpszClassName = L"D2DGameBoyWnd";
        RegisterClassEx(&wcex);

        HMENU hMenu = CreateMenu();
        HMENU hSubMenu = CreatePopupMenu();
        AppendMenu(hSubMenu, MF_STRING, IDM_FILE_OPEN, L"Open ROM...");
        AppendMenu(hSubMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(hSubMenu, MF_STRING, IDM_FILE_EXIT, L"Exit");
        AppendMenu(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hSubMenu, L"File");

        m_hwnd = CreateWindow(L"D2DGameBoyWnd", L"GameBoy Emulator (D2D + DS + Save)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, GB_WIDTH * 4, GB_HEIGHT * 4, NULL, hMenu, hInstance, this);
        if (m_hwnd) {
            ShowWindow(m_hwnd, nCmdShow);
            UpdateWindow(m_hwnd);

            if (!m_audio.Initialize(m_hwnd)) {
                MessageBox(m_hwnd, L"DirectSound Init Failed", L"Error", MB_OK);
            }
            return S_OK;
        }
        return E_FAIL;
    }

    void RunMessageLoop() {
        MSG msg;
        while (true) {
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) break;
                TranslateMessage(&msg); DispatchMessage(&msg);
            }
            else {
                const int BYTES_PER_FRAME = (SAMPLE_RATE / 60) * 2 * sizeof(int16_t);
                int freeSpace = m_audio.GetBufferFreeSpace();

                if (freeSpace > BYTES_PER_FRAME) {
                    m_gbCore.StepFrame();
                    m_audio.PushSamples(m_gbCore.GetAudioSamples());
                    OnRender();
                }
                else {
                    Sleep(1);
                }
            }
        }
    }

    void PauseAudio() { m_audio.Pause(); }
    void ResumeAudio() { m_audio.Resume(); }

private:
    void OnFileOpen() {
        OPENFILENAME ofn; wchar_t szFile[260] = { 0 };
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = m_hwnd; ofn.lpstrFile = szFile; ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = L"GameBoy ROMs\0*.gb;*.gbc\0All Files\0*.*\0"; ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

        PauseAudio();

        if (GetOpenFileName(&ofn) == TRUE) {
            m_gbCore.SaveRAM();

            if (m_gbCore.LoadRom(szFile)) {
                std::string titleStr = m_gbCore.GetTitle();
                std::wstring wTitle(titleStr.begin(), titleStr.end());
                std::wstring winTitle = L"GameBoy Emulator - " + wTitle;
                SetWindowText(m_hwnd, winTitle.c_str());
            }
            else {
                MessageBox(m_hwnd, L"Failed to load ROM file.", L"Error", MB_OK | MB_ICONERROR);
            }
        }

        ResumeAudio();
    }

    HRESULT CreateDeviceResources() {
        if (!m_pRenderTarget) {
            RECT rc; GetClientRect(m_hwnd, &rc);
            D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
            m_pDirect2dFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(m_hwnd, size), &m_pRenderTarget);
            if (m_pRenderTarget) {
                D2D1_BITMAP_PROPERTIES props;
                props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
                props.dpiX = 96.0f; props.dpiY = 96.0f;
                m_pRenderTarget->CreateBitmap(D2D1::SizeU(GB_WIDTH, GB_HEIGHT), props, &m_pBitmap);
            }
        }
        return S_OK;
    }

    void OnRender() {
        CreateDeviceResources();
        if (m_pRenderTarget && !(m_pRenderTarget->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED)) {
            m_pRenderTarget->BeginDraw();
            m_pRenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));
            if (m_pBitmap) {
                m_pBitmap->CopyFromMemory(NULL, m_gbCore.GetPixelData(), GB_WIDTH * sizeof(uint32_t));
                D2D1_SIZE_F rtSize = m_pRenderTarget->GetSize();
                m_pRenderTarget->DrawBitmap(m_pBitmap, D2D1::RectF(0, 0, rtSize.width, rtSize.height), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, NULL);
            }
            if (m_pRenderTarget->EndDraw() == D2DERR_RECREATE_TARGET) { SafeRelease(&m_pBitmap); SafeRelease(&m_pRenderTarget); }
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        App* pApp = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        switch (message) {
        case WM_CREATE: { LPCREATESTRUCT pCreate = reinterpret_cast<LPCREATESTRUCT>(lParam); pApp = reinterpret_cast<App*>(pCreate->lpCreateParams); SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pApp); return 0; }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_FILE_OPEN && pApp) pApp->OnFileOpen();
            if (LOWORD(wParam) == IDM_FILE_EXIT) DestroyWindow(hwnd);
            return 0;
        case WM_SIZE: if (pApp && pApp->m_pRenderTarget) pApp->m_pRenderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam))); return 0;
        case WM_KEYDOWN: case WM_KEYUP:
            if (pApp) {
                bool pressed = (message == WM_KEYDOWN);
                int key = -1;
                switch (wParam) {
                case VK_RIGHT: key = 0; break; case VK_LEFT:  key = 1; break; case VK_UP:    key = 2; break; case VK_DOWN:  key = 3; break;
                case 'Z':      key = 4; break; case 'X':      key = 5; break; case VK_SHIFT: key = 6; break; case VK_RETURN:key = 7; break;
                }
                if (key != -1) pApp->m_gbCore.InputKey(key, pressed);
            }
            return 0;

        case WM_ENTERMENULOOP:
        case WM_ENTERSIZEMOVE:
            if (pApp) pApp->PauseAudio();
            return 0;

        case WM_EXITMENULOOP:
        case WM_EXITSIZEMOVE:
            if (pApp) pApp->ResumeAudio();
            return 0;

        case WM_CLOSE:
            if (pApp) pApp->m_gbCore.SaveRAM();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (pApp) pApp->m_gbCore.SaveRAM();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    CoInitialize(NULL);
    App app;
    if (SUCCEEDED(app.Initialize(hInstance, nCmdShow))) app.RunMessageLoop();
    CoUninitialize();
    return 0;
}