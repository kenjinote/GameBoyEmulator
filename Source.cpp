#ifndef UNICODE
#define UNICODE
#endif 

#define _CRT_SECURE_NO_WARNINGS 

#include <windows.h>
#include <commdlg.h>
#include <d2d1.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

#pragma comment(lib, "d2d1.lib")

using Byte = uint8_t;
using Word = uint16_t;
using SignedByte = int8_t;

const int GB_WIDTH = 160;
const int GB_HEIGHT = 144;

#define IDM_FILE_OPEN 1001
#define IDM_FILE_EXIT 1002

template <class T> void SafeRelease(T** ppT) {
    if (*ppT) { (*ppT)->Release(); *ppT = NULL; }
}

// -----------------------------------------------------------------------------
// MMU (MBC1/MBC2 Auto-Detection Fix)
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

    int mbcType; // 0:None, 1:MBC1, 2:MBC2

    bool ramEnable;
    int romBank;
    int ramBank;
    int bankingMode;

    Byte joypadButtons;
    Byte joypadDir;

    MMU() { Reset(); }

    void Reset() {
        vram.assign(0x2000, 0);
        wram.assign(0x2000, 0);
        hram.assign(0x80, 0);
        io.assign(0x80, 0);
        oam.assign(0xA0, 0);
        sram.assign(0x8000, 0);

        if (rom.size() < 0x8000) rom.resize(0x8000, 0);

        interruptFlag = 0;
        interruptEnable = 0;

        mbcType = 0;
        ramEnable = false;
        romBank = 1;
        ramBank = 0;
        bankingMode = 0;

        joypadButtons = 0x0F;
        joypadDir = 0x0F;
    }

    void LoadRomData(const std::vector<Byte>& data) {
        rom = data;
        if (rom.size() < 0x8000) rom.resize(0x8000, 0);

        // SRAM初期化（MBC2の場合はゴミデータが必要なこともあるが、まずは0埋め）
        std::fill(sram.begin(), sram.end(), 0);

        // --- MBC判定ロジック強化 ---
        Byte type = rom[0x0147];

        // タイトル読み取り
        char title[17] = { 0 };
        for (int i = 0; i < 16; i++) {
            if (0x0134 + i < rom.size()) title[i] = rom[0x0134 + i];
        }

        if (type == 0x05 || type == 0x06) {
            mbcType = 2; // MBC2
        }
        else if (type >= 0x01 && type <= 0x03) {
            mbcType = 1; // MBC1
        }
        else {
            mbcType = 0; // ROM ONLY
        }

        // Reset MBC State
        ramEnable = false;
        romBank = 1;
        ramBank = 0;
        bankingMode = 0;
    }

    void RequestInterrupt(int bit) { interruptFlag |= (1 << bit); }

    void DoDMA(Byte value) {
        Word srcBase = value << 8;
        for (int i = 0; i < 0xA0; i++) {
            oam[i] = Read(srcBase + i);
        }
    }

    Byte GetJoypadState() {
        Byte select = io[0x00];
        Byte result = 0xCF | select;
        if (!(select & 0x10)) result &= (0xF0 | joypadDir);
        if (!(select & 0x20)) result &= (0xF0 | joypadButtons);
        return result;
    }

    Byte Read(Word addr) {
        if (addr < 0x4000) {
            return rom[addr];
        }
        if (addr < 0x8000) {
            int bank = romBank;
            if (mbcType == 1 && bankingMode == 0) bank |= (ramBank << 5);
            int maxBanks = (int)(rom.size() / 0x4000);
            if (maxBanks == 0) maxBanks = 1;
            bank %= maxBanks;
            return rom[(bank * 0x4000) + (addr - 0x4000)];
        }
        if (addr < 0xA000) return vram[addr - 0x8000];

        // External RAM
        if (addr < 0xC000) {
            if (!ramEnable) return 0xFF;

            if (mbcType == 2) {
                // MBC2: 512x4 bits RAM (A000-A1FF)
                // 上位4ビットは "Undefined" だが、実機では通常 1 (0xF0) が返る
                // SAGA2はこれをチェックしている可能性がある
                if (addr < 0xA200) {
                    return 0xF0 | (sram[addr - 0xA000] & 0x0F);
                }
                return 0xFF;
            }

            // MBC1
            int bank = (bankingMode == 1) ? ramBank : 0;
            return sram[(bank * 0x2000) + (addr - 0xA000)];
        }

        if (addr < 0xE000) return wram[addr - 0xC000];
        if (addr < 0xFE00) return wram[addr - 0xE000];
        if (addr < 0xFEA0) return oam[addr - 0xFE00];
        if (addr < 0xFF00) return 0xFF;

        if (addr == 0xFF00) return GetJoypadState();
        if (addr == 0xFF0F) return interruptFlag;
        if (addr < 0xFF80) return io[addr - 0xFF00];
        if (addr < 0xFFFF) return hram[addr - 0xFF80];
        if (addr == 0xFFFF) return interruptEnable;
        return 0xFF;
    }

    void Write(Word addr, Byte value) {
        if (addr < 0x8000) {
            if (mbcType == 1) {
                if (addr < 0x2000) { ramEnable = ((value & 0x0F) == 0x0A); return; }
                if (addr < 0x4000) { romBank = value & 0x1F; if (romBank == 0) romBank = 1; return; }
                if (addr < 0x6000) { ramBank = value & 0x03; return; }
                if (addr < 0x8000) { bankingMode = value & 0x01; return; }
            }
            else if (mbcType == 2) {
                // MBC2: Address bit 8 determines command
                if (addr < 0x4000) {
                    if (addr & 0x0100) {
                        romBank = value & 0x0F;
                        if (romBank == 0) romBank = 1;
                    }
                    else {
                        ramEnable = ((value & 0x0F) == 0x0A);
                    }
                    return;
                }
            }
            return;
        }

        if (addr < 0xA000) { vram[addr - 0x8000] = value; return; }

        if (addr < 0xC000) {
            if (ramEnable) {
                if (mbcType == 2) {
                    if (addr < 0xA200) {
                        // 下位4ビットのみ保存
                        sram[addr - 0xA000] = value & 0x0F;
                    }
                }
                else {
                    int bank = (bankingMode == 1) ? ramBank : 0;
                    sram[(bank * 0x2000) + (addr - 0xA000)] = value;
                }
            }
            return;
        }

        if (addr < 0xE000) { wram[addr - 0xC000] = value; return; }
        if (addr < 0xFE00) { wram[addr - 0xE000] = value; return; }
        if (addr < 0xFEA0) { oam[addr - 0xFE00] = value; return; }
        if (addr < 0xFF00) return;

        if (addr == 0xFF00) { io[0x00] = value; return; }
        if (addr == 0xFF0F) { interruptFlag = value; return; }
        if (addr == 0xFF46) { DoDMA(value); return; }

        if (addr < 0xFF80) { io[addr - 0xFF00] = value; return; }
        if (addr < 0xFFFF) { hram[addr - 0xFF80] = value; return; }
        if (addr == 0xFFFF) { interruptEnable = value; return; }
    }

    void SetKey(int keyId, bool pressed) {
        Byte* target = (keyId < 4) ? &joypadDir : &joypadButtons;
        int bit = keyId % 4;
        if (pressed) *target &= ~(1 << bit);
        else         *target |= (1 << bit);
    }

    std::string GetTitle() {
        if (rom.size() < 0x143) return "";
        char buf[17] = { 0 };
        for (int i = 0; i < 16; i++) {
            char c = rom[0x0134 + i];
            if (c == 0) break;
            buf[i] = c;
        }
        return std::string(buf);
    }

    std::string GetMBCName() {
        if (mbcType == 1) return "MBC1";
        if (mbcType == 2) return "MBC2";
        return "ROM ONLY";
    }
};

// -----------------------------------------------------------------------------
// PPU (No Change)
// -----------------------------------------------------------------------------
class PPU
{
private:
    MMU* mmu;
    uint32_t* screenBuffer;
    int cycleCounter;
    const uint32_t PALETTE[4] = { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 };

public:
    PPU(MMU* m) : mmu(m), screenBuffer(nullptr), cycleCounter(0) {}
    void Reset() { cycleCounter = 0; }
    void SetScreenBuffer(uint32_t* buffer) { screenBuffer = buffer; }
    Byte GetLY() { return mmu->io[0x44]; }
    void SetLY(Byte v) { mmu->io[0x44] = v; }
    Byte GetLCDC() { return mmu->io[0x40]; }

    void Step(int cycles) {
        if (!(GetLCDC() & 0x80)) { SetLY(0); cycleCounter = 0; return; }
        cycleCounter += cycles;
        if (cycleCounter >= 456) {
            cycleCounter -= 456;
            Byte ly = GetLY();
            if (ly < 144) RenderScanline(ly);
            ly++;
            if (ly == 144) mmu->RequestInterrupt(0);
            if (ly > 153) ly = 0;
            SetLY(ly);
        }
    }
    void RenderScanline(int line) {
        if (!screenBuffer) return;
        Byte lcdc = GetLCDC();
        if (!(lcdc & 0x01)) return;
        Byte bgp = mmu->io[0x47];
        uint32_t palette[4];
        for (int i = 0; i < 4; i++) palette[i] = PALETTE[(bgp >> (i * 2)) & 3];
        Byte scy = mmu->io[0x42];
        Byte scx = mmu->io[0x43];
        Word mapBase = (lcdc & 0x08) ? 0x9C00 : 0x9800;
        Word tileBase = (lcdc & 0x10) ? 0x8000 : 0x9000;
        bool unsignedTile = (lcdc & 0x10);
        Byte mapY = line + scy;
        for (int x = 0; x < 160; ++x) {
            Byte mapX = x + scx;
            Word tileIdxAddr = mapBase + (mapY / 8) * 32 + (mapX / 8);
            Byte tileIdx = mmu->Read(tileIdxAddr);
            Word tileAddr = unsignedTile ? tileBase + (tileIdx * 16) : tileBase + (static_cast<int8_t>(tileIdx) * 16);
            Byte row = mapY % 8;
            Byte b1 = mmu->Read(tileAddr + row * 2);
            Byte b2 = mmu->Read(tileAddr + row * 2 + 1);
            int bit = 7 - (mapX % 8);
            int colorId = ((b1 >> bit) & 1) | (((b2 >> bit) & 1) << 1);
            screenBuffer[line * 160 + x] = palette[colorId];
        }
    }
};

// -----------------------------------------------------------------------------
// CPU (No Change)
// -----------------------------------------------------------------------------
class CPU
{
public:
    struct Registers {
        struct { union { struct { Byte f; Byte a; }; Word af; }; } af;
        struct { union { struct { Byte c; Byte b; }; Word bc; }; } bc;
        struct { union { struct { Byte e; Byte d; }; Word de; }; } de;
        struct { union { struct { Byte l; Byte h; }; Word hl; }; } hl;
        Word sp, pc;
        bool ime, imeScheduled;
    } reg;
    MMU* mmu;
    bool halted, haltBugTriggered;
    int currentCycles;

    CPU(MMU* m) : mmu(m) { Reset(); }
    void Reset() {
        reg.af.af = 0x01B0; reg.bc.bc = 0x0013; reg.de.de = 0x00D8; reg.hl.hl = 0x014D;
        reg.sp = 0xFFFE; reg.pc = 0x0100; reg.ime = false; reg.imeScheduled = false;
        halted = false; haltBugTriggered = false; currentCycles = 0;
    }
    Byte Read(Word addr) { currentCycles += 4; return mmu->Read(addr); }
    void Write(Word addr, Byte val) { currentCycles += 4; mmu->Write(addr, val); }
    Byte Fetch() {
        Byte val = Read(reg.pc);
        if (haltBugTriggered) haltBugTriggered = false; else reg.pc++;
        return val;
    }
    Word Fetch16() { Byte l = Fetch(); Byte h = Fetch(); return (h << 8) | l; }
    void Push(Word val) { reg.sp--; Write(reg.sp, val >> 8); reg.sp--; Write(reg.sp, val & 0xFF); }
    Word Pop() { Byte l = Read(reg.sp++); Byte h = Read(reg.sp++); return (h << 8) | l; }
    void F_Z(bool z) { if (z) reg.af.f |= 0x80; else reg.af.f &= ~0x80; }
    void F_N(bool n) { if (n) reg.af.f |= 0x40; else reg.af.f &= ~0x40; }
    void F_H(bool h) { if (h) reg.af.f |= 0x20; else reg.af.f &= ~0x20; }
    void F_C(bool c) { if (c) reg.af.f |= 0x10; else reg.af.f &= ~0x10; }
    bool IsZ() const { return reg.af.f & 0x80; }
    bool IsC() const { return reg.af.f & 0x10; }
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
    switch ((op >> 3) & 7) { case 0: RLC(val); break; case 1: RRC(val); break; case 2: RL(val); break; case 3: RR(val); break; case 4: SLA(val); break; case 5: SRA(val); break; case 6: SWAP(val); break; case 7: SRL(val); break; }
                                   SetR8(r, val);
                                                                               }
                                                                               else {
                                                                                   int bit = (op >> 3) & 7;
                                                                                   if (op < 0x80) BIT(bit, val); else if (op < 0xC0) { val &= ~(1 << bit); SetR8(r, val); }
                                                                                   else { val |= (1 << bit); SetR8(r, val); }
                                                                               }
                                                                           }
                                                                           bool HandleInterrupts() {
                                                                               if (reg.ime && (mmu->interruptFlag & mmu->interruptEnable)) {
                                                                                   Byte fired = mmu->interruptFlag & mmu->interruptEnable;
                                                                                   int bit = 0;
                                                                                   if (fired & 0x01) bit = 0; else if (fired & 0x02) bit = 1; else if (fired & 0x04) bit = 2; else if (fired & 0x08) bit = 3; else if (fired & 0x10) bit = 4; else return false;
                                                                                   reg.ime = false; mmu->interruptFlag &= ~(1 << bit);
                                                                                   Push(reg.pc); reg.pc = 0x0040 + (bit * 8);
                                                                                   currentCycles += 20; return true;
                                                                               }
                                                                               return false;
                                                                           }
                                                                           int Step() {
                                                                               if (reg.imeScheduled) { reg.ime = true; reg.imeScheduled = false; }
                                                                               if (HandleInterrupts()) return currentCycles;
                                                                               if (halted) {
                                                                                   currentCycles = 4;
                                                                                   if (mmu->interruptFlag & mmu->interruptEnable) { halted = false; if (!reg.ime) haltBugTriggered = true; }
                                                                                   return currentCycles;
                                                                               }
                                                                               currentCycles = 0;
                                                                               Byte op = Fetch();
                                                                               if ((op & 0xC0) == 0x40) { if (op == 0x76) halted = true; else SetR8((op >> 3) & 7, GetR8(op & 7)); }
                                                                               else if ((op & 0xC0) == 0x80) {
                                                                                   Byte v = GetR8(op & 7);
    switch ((op >> 3) & 7) { case 0: ALU_ADD(v); break; case 1: ALU_ADC(v); break; case 2: ALU_SUB(v); break; case 3: ALU_SBC(v); break; case 4: ALU_AND(v); break; case 5: ALU_XOR(v); break; case 6: ALU_OR(v); break; case 7: ALU_CP(v); break; }
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
                                                                                   case 0xD9: reg.pc = Pop(); reg.ime = true; break; case 0xDA: { Word a = Fetch16(); if (IsC()) reg.pc = a; } break; case 0xDC: { Word a = Fetch16(); if (IsC()) Push(reg.pc), reg.pc = a; } break; case 0xDE: ALU_SBC(Fetch()); break; case 0xDF: Push(reg.pc); reg.pc = 0x18; break;
                                                                                   case 0xE0: Write(0xFF00 | Fetch(), reg.af.a); break; case 0xE1: SetR16(2, Pop()); break; case 0xE2: Write(0xFF00 | reg.bc.c, reg.af.a); break; case 0xE5: Push(reg.hl.hl); break;
                                                                                   case 0xE6: ALU_AND(Fetch()); break; case 0xE7: Push(reg.pc); reg.pc = 0x20; break; case 0xE8: { SignedByte r = (SignedByte)Fetch(); Word sp = reg.sp; int res = sp + r; F_Z(0); F_N(0); F_H((sp & 0xF) + (r & 0xF) > 0xF); F_C((sp & 0xFF) + (r & 0xFF) > 0xFF); reg.sp = (Word)res; } break;
                                                                                   case 0xE9: reg.pc = reg.hl.hl; break; case 0xEA: Write(Fetch16(), reg.af.a); break; case 0xEE: ALU_XOR(Fetch()); break; case 0xEF: Push(reg.pc); reg.pc = 0x28; break;
                                                                                   case 0xF0: reg.af.a = Read(0xFF00 | Fetch()); break; case 0xF1: SetR16(3, Pop(), true); break; case 0xF2: reg.af.a = Read(0xFF00 | reg.bc.c); break; case 0xF3: reg.ime = false; break;
                                                                                   case 0xF5: Push(reg.af.af); break; case 0xF6: ALU_OR(Fetch()); break; case 0xF7: Push(reg.pc); reg.pc = 0x30; break; case 0xF8: { SignedByte r = (SignedByte)Fetch(); Word sp = reg.sp; int res = sp + r; F_Z(0); F_N(0); F_H((sp & 0xF) + (r & 0xF) > 0xF); F_C((sp & 0xFF) + (r & 0xFF) > 0xFF); reg.hl.hl = (Word)res; } break;
                                                                                   case 0xF9: reg.sp = reg.hl.hl; break; case 0xFA: reg.af.a = Read(Fetch16()); break; case 0xFB: reg.imeScheduled = true; break; case 0xFE: ALU_CP(Fetch()); break; case 0xFF: Push(reg.pc); reg.pc = 0x38; break;
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
    std::vector<uint32_t> displayBuffer;
    bool isRomLoaded;
    int divCounter;

    GameBoyCore() : cpu(&mmu), ppu(&mmu), isRomLoaded(false), divCounter(0) {
        displayBuffer.resize(GB_WIDTH * GB_HEIGHT);
        ppu.SetScreenBuffer(displayBuffer.data());
        Reset(false);
    }

    void Reset(bool loaded) {
        mmu.Reset();
        cpu.Reset();
        ppu.Reset();
        isRomLoaded = loaded;
        divCounter = 0;
        if (!isRomLoaded) { SetupTestRender(); }
        else { mmu.io[0x40] = 0x91; mmu.io[0x47] = 0xE4; }
    }

    void SetupTestRender() {
        if (mmu.rom.size() < 0x200) mmu.rom.resize(0x200, 0);
        mmu.io[0x40] = 0x91; mmu.io[0x47] = 0xE4;
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
            return true;
        }
        fclose(fp);
        return false;
    }

    std::string GetTitle() { return mmu.GetTitle() + " (" + mmu.GetMBCName() + ")"; }

    void StepFrame() {
        const int CYCLES_PER_FRAME = 70224;
        int cyclesThisFrame = 0;
        while (cyclesThisFrame < CYCLES_PER_FRAME) {
            int cycles = cpu.Step();
            ppu.Step(cycles);
            divCounter += cycles;
            if (divCounter >= 256) { mmu.io[0x04]++; divCounter -= 256; }
            cyclesThisFrame += cycles;
        }
    }
    const void* GetPixelData() const { return displayBuffer.data(); }
    void InputKey(int key, bool pressed) { mmu.SetKey(key, pressed); }
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

public:
    App() : m_hwnd(NULL), m_pDirect2dFactory(NULL), m_pRenderTarget(NULL), m_pBitmap(NULL) {}
    ~App() { SafeRelease(&m_pBitmap); SafeRelease(&m_pRenderTarget); SafeRelease(&m_pDirect2dFactory); }

    HRESULT Initialize(HINSTANCE hInstance, int nCmdShow) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pDirect2dFactory);
        WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = App::WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = sizeof(LONG_PTR);
        wcex.hInstance = hInstance;
        wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcex.hbrBackground = NULL;
        wcex.lpszClassName = L"D2DGameBoyWnd";
        RegisterClassEx(&wcex);

        HMENU hMenu = CreateMenu();
        HMENU hSubMenu = CreatePopupMenu();
        AppendMenu(hSubMenu, MF_STRING, IDM_FILE_OPEN, L"Open ROM...");
        AppendMenu(hSubMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(hSubMenu, MF_STRING, IDM_FILE_EXIT, L"Exit");
        AppendMenu(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hSubMenu, L"File");

        m_hwnd = CreateWindow(L"D2DGameBoyWnd", L"GameBoy Emulator (SAGA2 Fixed)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, GB_WIDTH * 4, GB_HEIGHT * 4, NULL, hMenu, hInstance, this);
        if (m_hwnd) { ShowWindow(m_hwnd, nCmdShow); UpdateWindow(m_hwnd); return S_OK; }
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
                m_gbCore.StepFrame();
                OnRender();
            }
        }
    }

private:
    void OnFileOpen() {
        OPENFILENAME ofn; wchar_t szFile[260] = { 0 };
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = m_hwnd; ofn.lpstrFile = szFile; ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = L"GameBoy ROMs\0*.gb;*.gbc\0All Files\0*.*\0"; ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
        if (GetOpenFileName(&ofn) == TRUE) {
            if (m_gbCore.LoadRom(szFile)) {
                std::string titleStr = m_gbCore.GetTitle();
                std::wstring wTitle(titleStr.begin(), titleStr.end());
                std::wstring winTitle = L"GameBoy Emulator - " + wTitle;
                SetWindowText(m_hwnd, winTitle.c_str());
            }
        }
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
        case WM_COMMAND: if (LOWORD(wParam) == IDM_FILE_OPEN && pApp) pApp->OnFileOpen(); if (LOWORD(wParam) == IDM_FILE_EXIT) DestroyWindow(hwnd); return 0;
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
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    App app;
    if (SUCCEEDED(app.Initialize(hInstance, nCmdShow))) app.RunMessageLoop();
    return 0;
}