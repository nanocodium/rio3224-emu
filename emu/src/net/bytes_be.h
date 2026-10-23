// Big-endian byte helpers shared by the Dante endpoint translation units.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rio { namespace be {

inline void put16v(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
inline void put32v(std::vector<uint8_t>& v, uint32_t x) { put16v(v, x >> 16); put16v(v, x & 0xFFFF); }
inline void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
inline void put32(uint8_t* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline void set16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = x >> 8; v[at + 1] = x & 0xFF; }
inline uint16_t strz(std::vector<uint8_t>& v, const std::string& s) { uint16_t off = (uint16_t)v.size(); v.insert(v.end(), s.begin(), s.end()); v.push_back(0); return off; }
inline void align(std::vector<uint8_t>& v, size_t a) { while (v.size() % a) v.push_back(0); }
inline std::string readz(const uint8_t* p, size_t n, size_t off) { if (off >= n) return ""; size_t e = off; while (e < n && p[e]) e++; return std::string((const char*)p + off, e - off); }
inline std::string hex(const uint8_t* p, size_t n) { std::string s; char b[4]; for (size_t i = 0; i < n && i < 96; i++) { snprintf(b, sizeof b, "%02x", p[i]); s += b; } if (n > 96) s += "..."; return s; }

} } // namespace rio::be
