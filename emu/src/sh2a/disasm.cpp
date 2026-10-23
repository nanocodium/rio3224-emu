// SH-2A disassembler (for traces / debugger)
#include "cpu.h"
#include <cstdio>
#include <cstdarg>
#include <string>

static std::string fmt(const char* f, ...) {
    char buf[128]; va_list ap; va_start(ap, f); vsnprintf(buf, sizeof buf, f, ap); va_end(ap); return buf;
}

std::string Sh2a::disasm(uint32_t pc, uint16_t op, uint16_t op2) const {
    const int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF, d4 = op & 0xF, d8 = op & 0xFF;
    const int32_t s8 = (int8_t)d8;
    int32_t d12 = op & 0xFFF; if (d12 & 0x800) d12 -= 0x1000;
    const uint32_t pcl = ((pc + 4) & ~3u);
    switch (op >> 12) {
    case 0x0:
        switch (op & 0xF) {
        case 0x0: case 0x1: { int32_t imm = ((op >> 4) & 0xF) << 16 | op2; if (imm & 0x80000) imm -= 0x100000; if (op & 1) imm <<= 8;
                              return fmt("%s #0x%X,r%d", (op & 1) ? "movi20s" : "movi20", imm, n); }
        case 0x2: switch (m) { case 0: return fmt("stc sr,r%d", n); case 1: return fmt("stc gbr,r%d", n); case 2: return fmt("stc vbr,r%d", n); } break;
        case 0x3: switch (m) { case 0: return fmt("bsrf r%d", n); case 2: return fmt("braf r%d", n); case 8: return fmt("pref @r%d", n); } break;
        case 0x4: return fmt("mov.b r%d,@(r0,r%d)", m, n);
        case 0x5: return fmt("mov.w r%d,@(r0,r%d)", m, n);
        case 0x6: return fmt("mov.l r%d,@(r0,r%d)", m, n);
        case 0x7: return fmt("mul.l r%d,r%d", m, n);
        case 0x8: switch (m) { case 0: return "clrt"; case 1: return "sett"; case 2: return "clrmac"; case 4: return "clrs"; case 5: return "sets"; case 6: return "nott"; } break;
        case 0x9: switch (m) { case 0: return "nop"; case 1: return "div0u"; case 2: return fmt("movt r%d", n); case 3: return fmt("movrt r%d", n); } break;
        case 0xA: switch (m) { case 0: return fmt("sts mach,r%d", n); case 1: return fmt("sts macl,r%d", n); case 2: return fmt("sts pr,r%d", n);
                               case 4: return fmt("stc tbr,r%d", n); case 5: return fmt("sts fpul,r%d", n); case 6: return fmt("sts fpscr,r%d", n); } break;
        case 0xB: switch (m) { case 0: return "rts"; case 1: return "sleep"; case 2: return "rte"; case 5: return "resbank"; case 6: return "rts/n"; case 7: return fmt("rtv/n r%d", n); } break;
        case 0xC: return fmt("mov.b @(r0,r%d),r%d", m, n);
        case 0xD: return fmt("mov.w @(r0,r%d),r%d", m, n);
        case 0xE: return fmt("mov.l @(r0,r%d),r%d", m, n);
        case 0xF: return fmt("mac.l @r%d+,@r%d+", m, n);
        }
        break;
    case 0x1: return fmt("mov.l r%d,@(%d,r%d)", m, d4 * 4, n);
    case 0x2: {
        static const char* t[] = { "mov.b r%d,@r%d", "mov.w r%d,@r%d", "mov.l r%d,@r%d", nullptr, "mov.b r%d,@-r%d", "mov.w r%d,@-r%d", "mov.l r%d,@-r%d",
                                   "div0s r%d,r%d", "tst r%d,r%d", "and r%d,r%d", "xor r%d,r%d", "or r%d,r%d", "cmp/str r%d,r%d", "xtrct r%d,r%d", "mulu.w r%d,r%d", "muls.w r%d,r%d" };
        if (t[op & 0xF]) return fmt(t[op & 0xF], m, n);
        break;
    }
    case 0x3: {
        if ((op & 0xF) == 1) {
            static const char* t[] = { "mov.b r%d,@(%d,r%d)", "mov.w r%d,@(%d,r%d)", "mov.l r%d,@(%d,r%d)", "fmov.s fr%d,@(%d,r%d)",
                                       "mov.b @(%d,r%d),r%d", "mov.w @(%d,r%d),r%d", "mov.l @(%d,r%d),r%d", "fmov.s @(%d,r%d),fr%d",
                                       "movu.b @(%d,r%d),r%d", "movu.w @(%d,r%d),r%d" };
            int k = op2 >> 12; int disp = op2 & 0xFFF; int scale = (k & 3) == 0 ? 1 : (k & 3) == 1 ? 2 : 4; if (k == 3 || k == 7) scale = 4; if (k == 8) scale = 1; if (k == 9) scale = 2;
            if (k < 4) return fmt(t[k], m, disp * scale, n);
            if (k < 10) return fmt(t[k], disp * scale, m, n);
            break;
        }
        if ((op & 0xF) == 9) {
            static const char* t[16] = { "bclr.b", "bset.b", "bst.b", "bld.b", "band.b", "bor.b", "bxor.b", 0, 0, 0, 0, "bldnot.b", "bandnot.b", "bornot.b", 0, 0 };
            if (t[op2 >> 12]) return fmt("%s #%d,@(%d,r%d)", t[op2 >> 12], m & 7, op2 & 0xFFF, n);
            break;
        }
        static const char* t[] = { "cmp/eq r%d,r%d", 0, "cmp/hs r%d,r%d", "cmp/ge r%d,r%d", "div1 r%d,r%d", "dmulu.l r%d,r%d", "cmp/hi r%d,r%d", "cmp/gt r%d,r%d",
                                   "sub r%d,r%d", 0, "subc r%d,r%d", "subv r%d,r%d", "add r%d,r%d", "dmuls.l r%d,r%d", "addc r%d,r%d", "addv r%d,r%d" };
        if (t[op & 0xF]) return fmt(t[op & 0xF], m, n);
        break;
    }
    case 0x4: {
        switch (op & 0xFF) {
        case 0x00: return fmt("shll r%d", n);   case 0x10: return fmt("dt r%d", n);       case 0x20: return fmt("shal r%d", n);
        case 0x01: return fmt("shlr r%d", n);   case 0x11: return fmt("cmp/pz r%d", n);   case 0x21: return fmt("shar r%d", n);
        case 0x02: return fmt("sts.l mach,@-r%d", n); case 0x12: return fmt("sts.l macl,@-r%d", n); case 0x22: return fmt("sts.l pr,@-r%d", n);
        case 0x52: return fmt("sts.l fpul,@-r%d", n); case 0x62: return fmt("sts.l fpscr,@-r%d", n);
        case 0x03: return fmt("stc.l sr,@-r%d", n); case 0x13: return fmt("stc.l gbr,@-r%d", n); case 0x23: return fmt("stc.l vbr,@-r%d", n);
        case 0x04: return fmt("rotl r%d", n);   case 0x24: return fmt("rotcl r%d", n);
        case 0x05: return fmt("rotr r%d", n);   case 0x15: return fmt("cmp/pl r%d", n);   case 0x25: return fmt("rotcr r%d", n);
        case 0x06: return fmt("lds.l @r%d+,mach", n); case 0x16: return fmt("lds.l @r%d+,macl", n); case 0x26: return fmt("lds.l @r%d+,pr", n);
        case 0x56: return fmt("lds.l @r%d+,fpul", n); case 0x66: return fmt("lds.l @r%d+,fpscr", n);
        case 0x07: return fmt("ldc.l @r%d+,sr", n); case 0x17: return fmt("ldc.l @r%d+,gbr", n); case 0x27: return fmt("ldc.l @r%d+,vbr", n);
        case 0x08: return fmt("shll2 r%d", n);  case 0x18: return fmt("shll8 r%d", n);    case 0x28: return fmt("shll16 r%d", n);
        case 0x09: return fmt("shlr2 r%d", n);  case 0x19: return fmt("shlr8 r%d", n);    case 0x29: return fmt("shlr16 r%d", n);
        case 0x0A: return fmt("lds r%d,mach", n); case 0x1A: return fmt("lds r%d,macl", n); case 0x2A: return fmt("lds r%d,pr", n);
        case 0x4A: return fmt("ldc r%d,tbr", n); case 0x5A: return fmt("lds r%d,fpul", n); case 0x6A: return fmt("lds r%d,fpscr", n);
        case 0x0B: return fmt("jsr @r%d", n);   case 0x2B: return fmt("jmp @r%d", n);     case 0x4B: return fmt("jsr/n @r%d", n); case 0x1B: return fmt("tas.b @r%d", n);
        case 0x0E: return fmt("ldc r%d,sr", n); case 0x1E: return fmt("ldc r%d,gbr", n);  case 0x2E: return fmt("ldc r%d,vbr", n);
        case 0x80: return fmt("mulr r0,r%d", n); case 0x84: return fmt("divu r0,r%d", n); case 0x94: return fmt("divs r0,r%d", n);
        case 0x81: return fmt("clipu.b r%d", n); case 0x85: return fmt("clipu.w r%d", n); case 0x91: return fmt("clips.b r%d", n); case 0x95: return fmt("clips.w r%d", n);
        case 0x8B: return fmt("mov.b r0,@r%d+", n); case 0x9B: return fmt("mov.w r0,@r%d+", n); case 0xAB: return fmt("mov.l r0,@r%d+", n);
        case 0xCB: return fmt("mov.b @-r%d,r0", n); case 0xDB: return fmt("mov.w @-r%d,r0", n); case 0xEB: return fmt("mov.l @-r%d,r0", n);
        case 0xE1: return fmt("stbank r0,@r%d", n); case 0xE5: return fmt("ldbank @r%d,r0", n);
        case 0xF0: return fmt("movmu.l r%d,@-r15", n); case 0xF1: return fmt("movml.l r%d,@-r15", n);
        case 0xF4: return fmt("movmu.l @r15+,r%d", n); case 0xF5: return fmt("movml.l @r15+,r%d", n);
        }
        switch (op & 0xF) { case 0xC: return fmt("shad r%d,r%d", m, n); case 0xD: return fmt("shld r%d,r%d", m, n); case 0xF: return fmt("mac.w @r%d+,@r%d+", m, n); }
        break;
    }
    case 0x5: return fmt("mov.l @(%d,r%d),r%d", d4 * 4, m, n);
    case 0x6: {
        static const char* t[] = { "mov.b @r%d,r%d", "mov.w @r%d,r%d", "mov.l @r%d,r%d", "mov r%d,r%d", "mov.b @r%d+,r%d", "mov.w @r%d+,r%d", "mov.l @r%d+,r%d", "not r%d,r%d",
                                   "swap.b r%d,r%d", "swap.w r%d,r%d", "negc r%d,r%d", "neg r%d,r%d", "extu.b r%d,r%d", "extu.w r%d,r%d", "exts.b r%d,r%d", "exts.w r%d,r%d" };
        return fmt(t[op & 0xF], m, n);
    }
    case 0x7: return fmt("add #%d,r%d", s8, n);
    case 0x8:
        switch (n) {
        case 0x0: return fmt("mov.b r0,@(%d,r%d)", d4, m);
        case 0x1: return fmt("mov.w r0,@(%d,r%d)", d4 * 2, m);
        case 0x4: return fmt("mov.b @(%d,r%d),r0", d4, m);
        case 0x5: return fmt("mov.w @(%d,r%d),r0", d4 * 2, m);
        case 0x8: return fmt("cmp/eq #%d,r0", s8);
        case 0x9: return fmt("bt 0x%08X", pc + 4 + s8 * 2);
        case 0xB: return fmt("bf 0x%08X", pc + 4 + s8 * 2);
        case 0xD: return fmt("bt/s 0x%08X", pc + 4 + s8 * 2);
        case 0xF: return fmt("bf/s 0x%08X", pc + 4 + s8 * 2);
        case 0x3: return fmt("jsr/n @@(%d,tbr)", d8 * 4);
        case 0x6: return fmt("%s #%d,r%d", (op & 8) ? "bset" : "bclr", op & 7, m);
        case 0x7: return fmt("%s #%d,r%d", (op & 8) ? "bld" : "bst", op & 7, m);
        }
        break;
    case 0x9: return fmt("mov.w @(0x%08X),r%d", pc + 4 + d8 * 2, n);
    case 0xA: return fmt("bra 0x%08X", pc + 4 + d12 * 2);
    case 0xB: return fmt("bsr 0x%08X", pc + 4 + d12 * 2);
    case 0xC:
        switch (n) {
        case 0x0: return fmt("mov.b r0,@(%d,gbr)", d8);
        case 0x1: return fmt("mov.w r0,@(%d,gbr)", d8 * 2);
        case 0x2: return fmt("mov.l r0,@(%d,gbr)", d8 * 4);
        case 0x3: return fmt("trapa #%d", d8);
        case 0x4: return fmt("mov.b @(%d,gbr),r0", d8);
        case 0x5: return fmt("mov.w @(%d,gbr),r0", d8 * 2);
        case 0x6: return fmt("mov.l @(%d,gbr),r0", d8 * 4);
        case 0x7: return fmt("mova @(0x%08X),r0", pcl + d8 * 4);
        case 0x8: return fmt("tst #0x%02X,r0", d8);
        case 0x9: return fmt("and #0x%02X,r0", d8);
        case 0xA: return fmt("xor #0x%02X,r0", d8);
        case 0xB: return fmt("or #0x%02X,r0", d8);
        case 0xC: return fmt("tst.b #0x%02X,@(r0,gbr)", d8);
        case 0xD: return fmt("and.b #0x%02X,@(r0,gbr)", d8);
        case 0xE: return fmt("xor.b #0x%02X,@(r0,gbr)", d8);
        case 0xF: return fmt("or.b #0x%02X,@(r0,gbr)", d8);
        }
        break;
    case 0xD: { uint32_t a = pcl + d8 * 4; uint8_t* p = bus.ramPtr(a); uint32_t v = p ? ((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) : 0;
                return fmt("mov.l @(0x%08X),r%d\t; =0x%08X", a, n, v); }
    case 0xE: return fmt("mov #%d,r%d", s8, n);
    case 0xF: {
        static const char* t[] = { "fadd fr%d,fr%d", "fsub fr%d,fr%d", "fmul fr%d,fr%d", "fdiv fr%d,fr%d", "fcmp/eq fr%d,fr%d", "fcmp/gt fr%d,fr%d",
                                   "fmov.s @(r0,r%d),fr%d", "fmov.s fr%d,@(r0,r%d)", "fmov.s @r%d,fr%d", "fmov.s @r%d+,fr%d", "fmov.s fr%d,@r%d", "fmov.s fr%d,@-r%d", "fmov fr%d,fr%d", 0, "fmac fr0,fr%d,fr%d", 0 };
        if ((op & 0xF) == 0xD) {
            static const char* u[] = { "fsts fpul,fr%d", "flds fr%d,fpul", "float fpul,fr%d", "ftrc fr%d,fpul", "fneg fr%d", "fabs fr%d", "fsqrt fr%d", 0, "fldi0 fr%d", "fldi1 fr%d", "fcnvsd fpul,dr%d", "fcnvds dr%d,fpul", 0, 0, 0, 0 };
            if (op == 0xF3FD) return "fschg";
            if (u[m]) return fmt(u[m], n);
            break;
        }
        if (t[op & 0xF]) return fmt(t[op & 0xF], m, n);
        break;
    }
    }
    return fmt(".word 0x%04X", op);
}
