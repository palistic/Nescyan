#include <SDL.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int kWidth = 256;
constexpr int kHeight = 240;
constexpr uint32_t kNesPalette[64] = {
    0x666666,0x002a88,0x1412a7,0x3b00a4,0x5c007e,0x6e0040,0x6c0600,0x561d00,
    0x333500,0x0b4800,0x005200,0x004f08,0x00404d,0x000000,0x000000,0x000000,
    0xadadad,0x155fd9,0x4240ff,0x7527fe,0xa01acc,0xb71e7b,0xb53120,0x994e00,
    0x6b6d00,0x388700,0x0c9300,0x008f32,0x007c8d,0x000000,0x000000,0x000000,
    0xfffeff,0x64b0ff,0x9290ff,0xc676ff,0xf36aff,0xfe6ecc,0xfe8170,0xea9e22,
    0xbcbe00,0x88d800,0x5ce430,0x45e082,0x48cdde,0x4f4f4f,0x000000,0x000000,
    0xfffeff,0xc0dfff,0xd3d2ff,0xe8c8ff,0xfbc2ff,0xfec4ea,0xfeccc5,0xf7d8a5,
    0xe4e594,0xcfef96,0xbdf4ab,0xb3f3cc,0xb5ebf2,0xb8b8b8,0x000000,0x000000
};

struct Rom {
    std::vector<uint8_t> prg, chr;
    bool chrRam = false;
};

Rom loadRom(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Could not open ROM: " + path);
    std::array<uint8_t, 16> h{};
    in.read(reinterpret_cast<char*>(h.data()), h.size());
    if (!in || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1a)
        throw std::runtime_error("Not a valid iNES ROM");
    if (h[7] & 0x0c) throw std::runtime_error("NES 2.0 ROMs are not supported yet");
    const int mapper = (h[6] >> 4) | (h[7] & 0xf0);
    if (mapper != 0) throw std::runtime_error("Only mapper 0 (NROM) is supported");
    if (h[6] & 0x04) in.seekg(512, std::ios::cur); // optional trainer
    Rom rom;
    rom.prg.resize(static_cast<size_t>(h[4]) * 16384);
    in.read(reinterpret_cast<char*>(rom.prg.data()), static_cast<std::streamsize>(rom.prg.size()));
    if (!in || (rom.prg.size() != 16384 && rom.prg.size() != 32768))
        throw std::runtime_error("Mapper 0 requires 16 or 32 KiB of PRG ROM");
    rom.chrRam = h[5] == 0;
    rom.chr.resize(rom.chrRam ? 8192 : static_cast<size_t>(h[5]) * 8192);
    if (!rom.chrRam) in.read(reinterpret_cast<char*>(rom.chr.data()), static_cast<std::streamsize>(rom.chr.size()));
    if (!in) throw std::runtime_error("ROM ended unexpectedly while reading CHR data");
    return rom;
}

class Nes {
public:
    explicit Nes(Rom image) : rom(std::move(image)) { reset(); }

    void reset() {
        a = x = y = 0; sp = 0xfd; p = 0x24; cycles = 7;
        pc = read16(0xfffc);
    }

    void runFrame() {
        // A frame is approximately 29,780 CPU cycles. This core is instruction-timed,
        // not cycle-stepped, so PPU timing-sensitive software is not yet compatible.
        int spent = 0;
        while (spent < 29780) spent += step();
        ppu[2] |= 0x80;
        if (ppu[0] & 0x80) { setFlag(0x04, false); push16(pc); push(static_cast<uint8_t>((p | 0x20) & ~0x10)); pc = read16(0xfffa); }
        render();
        ppu[2] &= 0x7f;
    }

    const std::array<uint32_t, kWidth * kHeight>& pixels() const { return frame; }
    void strobeController() { controllerShift = controller; controllerStrobe = true; }
    uint8_t readController() {
        if (controllerStrobe) return controller & 1;
        const uint8_t bit = controllerShift & 1;
        controllerShift = static_cast<uint8_t>((controllerShift >> 1) | 0x80);
        return bit;
    }
    void setController(uint8_t buttons) { controller = buttons; if (controllerStrobe) controllerShift = buttons; }

private:
    Rom rom;
    std::array<uint8_t, 2048> ram{};
    std::array<uint8_t, 4096> nametables{};
    std::array<uint8_t, 32> palette{};
    std::array<uint8_t, 256> oam{};
    std::array<uint8_t, 8> ppu{};
    std::array<uint32_t, kWidth * kHeight> frame{};
    uint8_t a=0, x=0, y=0, sp=0xfd, p=0x24, controller=0, controllerShift=0;
    uint16_t pc=0;
    bool controllerStrobe=false;
    uint8_t scrollX=0, scrollY=0;
    bool scrollSecond=false, addressSecond=false;
    uint16_t vramAddress=0, tempAddress=0;

    bool flag(uint8_t f) const { return (p & f) != 0; }
    void setFlag(uint8_t f, bool value) { if (value) p |= f; else p &= static_cast<uint8_t>(~f); }
    void nz(uint8_t v) { setFlag(0x02, v == 0); setFlag(0x80, (v & 0x80) != 0); }
    uint8_t fetch() { return read(pc++); }
    uint16_t fetch16() { uint16_t lo=fetch(); return static_cast<uint16_t>(lo | (fetch() << 8)); }
    void push(uint8_t v) { write(static_cast<uint16_t>(0x100 | sp--), v); }
    uint8_t pop() { return read(static_cast<uint16_t>(0x100 | ++sp)); }
    void push16(uint16_t v) { push(v >> 8); push(v & 0xff); }
    uint16_t pop16() { uint16_t lo=pop(); return static_cast<uint16_t>(lo | (pop() << 8)); }
    uint16_t read16(uint16_t addr) { return static_cast<uint16_t>(read(addr) | (read(static_cast<uint16_t>(addr+1)) << 8)); }

    uint8_t ppuRead(uint16_t addr) {
        addr &= 0x3fff;
        if (addr < 0x2000) return rom.chr[addr % rom.chr.size()];
        if (addr < 0x3f00) {
            addr = static_cast<uint16_t>((addr - 0x2000) & 0x0fff);
            const int table = addr / 0x400, offset = addr & 0x3ff;
            const int physical = (romMirroring() == 0) ? (table & 1) * 0x400 : (table >> 1) * 0x400;
            return nametables[physical + offset];
        }
        addr = static_cast<uint16_t>((addr - 0x3f00) & 0x1f);
        if ((addr & 0x13) == 0x10) addr &= 0x0f;
        return palette[addr] & 0x3f;
    }
    void ppuWrite(uint16_t addr, uint8_t value) {
        addr &= 0x3fff;
        if (addr < 0x2000) { if (rom.chrRam) rom.chr[addr] = value; return; }
        if (addr < 0x3f00) {
            addr = static_cast<uint16_t>((addr - 0x2000) & 0x0fff);
            const int table = addr / 0x400, offset = addr & 0x3ff;
            const int physical = (romMirroring() == 0) ? (table & 1) * 0x400 : (table >> 1) * 0x400;
            nametables[physical + offset] = value; return;
        }
        addr = static_cast<uint16_t>((addr - 0x3f00) & 0x1f);
        if ((addr & 0x13) == 0x10) addr &= 0x0f;
        palette[addr] = value & 0x3f;
    }
    int romMirroring() const { return (romMirroringBit() ? 1 : 0); }
    bool romMirroringBit() const { return false; } // iNES mirroring is fixed below in this simple core.

    uint8_t read(uint16_t addr) {
        if (addr < 0x2000) return ram[addr & 0x7ff];
        if (addr < 0x4000) {
            const uint8_t reg = static_cast<uint8_t>(0x2000 | (addr & 7));
            if (reg == 0x2002) { uint8_t v=ppu[2]; ppu[2] &= 0x7f; scrollSecond=addressSecond=false; return v; }
            if (reg == 0x2004) return oam[ppu[3]];
            if (reg == 0x2007) { uint8_t v=ppuRead(vramAddress); vramAddress += (ppu[0] & 4) ? 32 : 1; return v; }
            return ppu[reg & 7];
        }
        if (addr == 0x4016) return static_cast<uint8_t>(0x40 | readController());
        if (addr >= 0x8000) return rom.prg[(addr - 0x8000) % rom.prg.size()];
        return 0;
    }
    void write(uint16_t addr, uint8_t value) {
        if (addr < 0x2000) { ram[addr & 0x7ff]=value; return; }
        if (addr < 0x4000) {
            const uint8_t reg=static_cast<uint8_t>(0x2000 | (addr & 7)); ppu[reg & 7]=value;
            switch(reg) {
                case 0x2005: if (!scrollSecond) scrollX=value; else scrollY=value; scrollSecond=!scrollSecond; break;
                case 0x2006: if (!addressSecond) { tempAddress=static_cast<uint16_t>((value & 0x3f) << 8); } else { tempAddress |= value; vramAddress=tempAddress; } addressSecond=!addressSecond; break;
                case 0x2007: ppuWrite(vramAddress,value); vramAddress += (ppu[0] & 4) ? 32 : 1; break;
                case 0x2003: break;
                case 0x2004: oam[ppu[3]++]=value; break;
            }
            return;
        }
        if (addr == 0x4014) { uint16_t base=static_cast<uint16_t>(value << 8); for(int i=0;i<256;++i) oam[i]=read(base+i); cycles += 513; return; }
        if (addr == 0x4016) { controllerStrobe=(value & 1)!=0; if (controllerStrobe) controllerShift=controller; return; }
        if (addr >= 0x6000 && addr < 0x8000) return; // PRG RAM, currently unused.
    }

    enum Mode { Imm, Zp, ZpX, ZpY, Abs, AbsX, AbsY, IndX, IndY };
    uint16_t address(Mode m) {
        switch(m) {
            case Imm: return pc++;
            case Zp: return fetch();
            case ZpX: return static_cast<uint8_t>(fetch()+x);
            case ZpY: return static_cast<uint8_t>(fetch()+y);
            case Abs: return fetch16();
            case AbsX: return static_cast<uint16_t>(fetch16()+x);
            case AbsY: return static_cast<uint16_t>(fetch16()+y);
            case IndX: { uint8_t z=static_cast<uint8_t>(fetch()+x); return static_cast<uint16_t>(read(z) | (read(static_cast<uint8_t>(z+1)) << 8)); }
            case IndY: { uint8_t z=fetch(); return static_cast<uint16_t>((read(z) | (read(static_cast<uint8_t>(z+1)) << 8))+y); }
        }
        return 0;
    }
    void compare(uint8_t lhs,uint8_t rhs) { uint16_t d=static_cast<uint16_t>(lhs)-rhs; setFlag(1,lhs>=rhs); nz(static_cast<uint8_t>(d)); }
    void adc(uint8_t v) { uint16_t sum=a+v+(flag(1)?1:0); setFlag(0x40, (~(a^v)&(a^sum)&0x80)!=0); setFlag(1,sum>255); a=static_cast<uint8_t>(sum); nz(a); }
    void branch(bool take) { int8_t d=static_cast<int8_t>(fetch()); if(take) pc=static_cast<uint16_t>(pc+d); }

    int step() {
        const uint8_t op=fetch();
        // The regular ALU instruction families share addressing modes and semantics.
        const uint8_t family=op & 0xe0, low=op & 0x1f;
        if ((family==0x00 || family==0x20 || family==0x40 || family==0x60 || family==0x80 || family==0xa0 || family==0xc0 || family==0xe0) &&
            (low==0x01 || low==0x05 || low==0x09 || low==0x0d || low==0x11 || low==0x15 || low==0x19 || low==0x1d)) {
            Mode m; int c;
            switch(low) { case 1:m=IndX;c=6;break; case 5:m=Zp;c=3;break; case 9:m=Imm;c=2;break; case 13:m=Abs;c=4;break; case 17:m=IndY;c=5;break; case 21:m=ZpX;c=4;break; case 25:m=AbsY;c=4;break; default:m=AbsX;c=4;break; }
            // Stores have no immediate or indexed-indirect-Y variants beyond their documented forms.
            if (family==0x80 && (low==9 || low==25)) { pc--; return unsupported(op); }
            uint8_t v=read(address(m));
            switch(family) {
                case 0x00:a|=v;nz(a);break; case 0x20:a&=v;nz(a);break; case 0x40:a^=v;nz(a);break;
                case 0x60:adc(v);break; case 0x80:write(addressForStore(m),a);break;
                case 0xa0:a=v;nz(a);break; case 0xc0:compare(a,v);break; case 0xe0:adc(static_cast<uint8_t>(v ^ 0xff));break;
            }
            return c;
        }
        switch(op) {
            case 0x00: pc++; push16(pc); push(p|0x30); setFlag(4,true); pc=read16(0xfffe); return 7;
            case 0x08: push(p|0x30); return 3; case 0x28:p=static_cast<uint8_t>((pop()&~0x10)|0x20);return 4;
            case 0x48:push(a);return 3; case 0x68:a=pop();nz(a);return 4;
            case 0x20:{uint16_t dest=fetch16();push16(pc-1);pc=dest;return 6;}
            case 0x4c:pc=fetch16();return 3;
            case 0x6c:{uint16_t ptr=fetch16();pc=static_cast<uint16_t>(read(ptr)|(read((ptr&0xff00)|((ptr+1)&0xff))<<8));return 5;}
            case 0x40:p=static_cast<uint8_t>((pop()&~0x10)|0x20);pc=pop16();return 6;
            case 0x60:pc=pop16()+1;return 6;
            case 0x10:branch(!flag(0x80));return 2; case 0x30:branch(flag(0x80));return 2;
            case 0x50:branch(!flag(0x40));return 2; case 0x70:branch(flag(0x40));return 2;
            case 0x90:branch(!flag(1));return 2; case 0xb0:branch(flag(1));return 2;
            case 0xd0:branch(!flag(2));return 2; case 0xf0:branch(flag(2));return 2;
            case 0x18:setFlag(1,false);return 2; case 0x38:setFlag(1,true);return 2;
            case 0x58:setFlag(4,false);return 2; case 0x78:setFlag(4,true);return 2;
            case 0xb8:setFlag(0x40,false);return 2; case 0xd8:setFlag(8,false);return 2; case 0xf8:setFlag(8,true);return 2;
            case 0xaa:x=a;nz(x);return 2; case 0x8a:a=x;nz(a);return 2; case 0xa8:y=a;nz(y);return 2; case 0x98:a=y;nz(a);return 2;
            case 0xba:x=sp;nz(x);return 2; case 0x9a:sp=x;return 2;
            case 0xe8:x++;nz(x);return 2; case 0xca:x--;nz(x);return 2; case 0xc8:y++;nz(y);return 2; case 0x88:y--;nz(y);return 2;
            case 0xea:return 2;
            case 0xa2:x=fetch();nz(x);return 2; case 0xa0:y=fetch();nz(y);return 2;
            case 0xa6:x=read(address(Zp));nz(x);return 3; case 0xb6:x=read(address(ZpY));nz(x);return 4;
            case 0xae:x=read(address(Abs));nz(x);return 4; case 0xbe:x=read(address(AbsY));nz(x);return 4;
            case 0xa4:y=read(address(Zp));nz(y);return 3; case 0xb4:y=read(address(ZpX));nz(y);return 4;
            case 0xac:y=read(address(Abs));nz(y);return 4; case 0xbc:y=read(address(AbsX));nz(y);return 4;
            case 0x86:write(address(Zp),x);return 3; case 0x96:write(address(ZpY),x);return 4; case 0x8e:write(address(Abs),x);return 4;
            case 0x84:write(address(Zp),y);return 3; case 0x94:write(address(ZpX),y);return 4; case 0x8c:write(address(Abs),y);return 4;
            case 0xc9:compare(a,fetch());return 2; case 0xe0:compare(x,fetch());return 2; case 0xc0:compare(y,fetch());return 2;
            default:return unsupported(op);
        }
    }
    uint16_t addressForStore(Mode m) { // Recompute only for store-family opcodes (address is consumed once).
        (void)m; return lastAddress;
    }
    uint16_t lastAddress=0;
    int unsupported(uint8_t op) {
        std::cerr << "Unsupported 6502 opcode $" << std::hex << static_cast<int>(op) << " at $" << (pc-1) << std::dec << '\n';
        return 2;
    }

    void render() {
        const int baseTable = ppu[0] & 3;
        const int patternBase = (ppu[0] & 0x10) ? 0x1000 : 0;
        const int universal = ppuRead(0x3f00);
        for (int sy=0; sy<kHeight; ++sy) for(int sx=0; sx<kWidth; ++sx) {
            int px=(sx+scrollX)%256, py=(sy+scrollY)%240;
            int table=baseTable + (px>=256?1:0) + (py>=240?2:0); table &=3;
            int nt=0x2000 + table*0x400;
            int tile=ppuRead(static_cast<uint16_t>(nt+(py/8)*32+px/8));
            int attr=ppuRead(static_cast<uint16_t>(nt+0x3c0+(py/32)*8+(px/32)));
            int shift=((py%32)/16)*4+((px%32)/16)*2;
            int pal=(attr>>shift)&3;
            int row=py&7, col=px&7;
            uint8_t lo=ppuRead(static_cast<uint16_t>(patternBase+tile*16+row));
            uint8_t hi=ppuRead(static_cast<uint16_t>(patternBase+tile*16+row+8));
            int bit=7-col, color=((lo>>bit)&1)|(((hi>>bit)&1)<<1);
            int idx=color ? ppuRead(static_cast<uint16_t>(0x3f00+pal*4+color)) : universal;
            frame[sy*kWidth+sx]=kNesPalette[idx&0x3f];
        }
        // Draw 8x8 sprites; 8x16 mode and sprite priority/overflow are not emulated yet.
        const int spriteBase=(ppu[1]&8)?0x1000:0;
        for(int i=63;i>=0;--i) {
            int sy=oam[i*4]+1, tile=oam[i*4+1], attr=oam[i*4+2], sx=oam[i*4+3];
            for(int dy=0;dy<8;++dy) for(int dx=0;dx<8;++dx) {
                int yy=(attr&0x80)?7-dy:dy, xx=(attr&0x40)?7-dx:dx;
                uint8_t lo=ppuRead(static_cast<uint16_t>(spriteBase+tile*16+yy));
                uint8_t hi=ppuRead(static_cast<uint16_t>(spriteBase+tile*16+yy+8));
                int color=((lo>>(7-xx))&1)|(((hi>>(7-xx))&1)<<1);
                int xpix=sx+dx, ypix=sy+dy;
                if(color && xpix<kWidth && ypix<kHeight) frame[ypix*kWidth+xpix]=kNesPalette[ppuRead(static_cast<uint16_t>(0x3f10+(attr&3)*4+color))&0x3f];
            }
        }
    }
};
}

int main(int argc, char** argv) {
    if(argc != 2) { std::cerr << "Usage: nescyan <game.nes>\n"; return 2; }
    try {
        Nes nes(loadRom(argv[1]));
        if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) throw std::runtime_error(SDL_GetError());
        SDL_Window* window=SDL_CreateWindow("Nescyan",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,kWidth*3,kHeight*3,SDL_WINDOW_SHOWN);
        if(!window) { SDL_Quit(); throw std::runtime_error(SDL_GetError()); }
        SDL_Renderer* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if(!renderer) renderer=SDL_CreateRenderer(window,-1,0);
        SDL_Texture* texture=renderer ? SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,kWidth,kHeight) : nullptr;
        if(!renderer || !texture) { if(texture) SDL_DestroyTexture(texture); if(renderer) SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); throw std::runtime_error(SDL_GetError()); }
        bool running=true;
        while(running) {
            SDL_Event e;
            while(SDL_PollEvent(&e)) if(e.type==SDL_QUIT || (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE)) running=false;
            const uint8_t* keys=SDL_GetKeyboardState(nullptr);
            uint8_t buttons=0;
            buttons|=keys[SDL_SCANCODE_X]?0x01:0; buttons|=keys[SDL_SCANCODE_Z]?0x02:0;
            buttons|=keys[SDL_SCANCODE_RSHIFT]?0x04:0; buttons|=keys[SDL_SCANCODE_RETURN]?0x08:0;
            buttons|=keys[SDL_SCANCODE_UP]?0x10:0; buttons|=keys[SDL_SCANCODE_DOWN]?0x20:0;
            buttons|=keys[SDL_SCANCODE_LEFT]?0x40:0; buttons|=keys[SDL_SCANCODE_RIGHT]?0x80:0;
            nes.setController(buttons); nes.runFrame();
            SDL_UpdateTexture(texture,nullptr,nes.pixels().data(),kWidth*sizeof(uint32_t));
            SDL_RenderClear(renderer); SDL_RenderCopy(renderer,texture,nullptr,nullptr); SDL_RenderPresent(renderer);
        }
        SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    } catch(const std::exception& e) { std::cerr << "Nescyan: " << e.what() << '\n'; return 1; }
    return 0;
}
