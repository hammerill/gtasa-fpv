#include "ear.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "log.h"
#include "patch.h"
#include "sa.h"

namespace ear {
namespace {

// An instruction's 32-bit operand: its address, and the opcode byte right
// before it (the last byte of the opcode or ModRM), checked with the operand.
struct Site {
    uint32_t operand;
    uint8_t before;
};

// Found by disassembling 0x4D0000-0x508000 of the 1.0 US exe: every operand
// that is TheCamera (0xB6F028), its m_matrix pointer (0xB6F03C), its
// placement's position (0xB6F02C, the fallback when m_matrix is null) or
// m_mCameraMatrix (0xB6F99C). Left out: 0x4D7E44, CAEAudioEnvironment::
// GetDopplerRelativeFrequency asking TheCamera a flag (+0x2A), not where it is.
// mov reg, [0xB6F03C]
constexpr Site kMatrixPointer[] = {
    {0x4D705C, 0x0d}, {0x4D7306, 0xa1}, {0x4D8131, 0xa1}, {0x4D81B6, 0x0d}, {0x4D826B, 0xa1}, {0x4DF371, 0xa1},
    {0x4DF48A, 0xa1}, {0x4DF57A, 0xa1}, {0x4DF9D2, 0xa1}, {0x4E51B7, 0x35}, {0x4E61DF, 0xa1}, {0x4E673D, 0xa1},
    {0x4EF8B2, 0xa1}, {0x4EF956, 0xa1}, {0x4EF9C2, 0xa1}, {0x4F8BB0, 0xa1}, {0x4FD3B7, 0xa1}, {0x4FD522, 0xa1},
    {0x4FDAB6, 0xa1}, {0x4FDC2A, 0xa1}, {0x4FE2E9, 0xa1}, {0x4FE53C, 0xa1}, {0x4FE7E7, 0xa1}, {0x4FEA46, 0xa1},
    {0x4FECC1, 0xa1}, {0x4FEF28, 0xa1}, {0x4FF11E, 0xa1}, {0x4FF3DE, 0xa1}, {0x4FF664, 0xa1}, {0x4FFA65, 0xa1},
    {0x505C06, 0xa1}, {0x505D57, 0xa1}, {0x5061A6, 0xa1}, {0x506529, 0x0d},
};
// mov reg, 0xB6F02C
constexpr Site kPlacementPos[] = {
    {0x4D706D, 0xb8}, {0x4D7096, 0xb8}, {0x4D7314, 0xb8}, {0x4D815B, 0xb8}, {0x4D81C2, 0xba}, {0x4D8291, 0xb8},
    {0x4DF37F, 0xb8}, {0x4DF498, 0xb8}, {0x4DF588, 0xb8}, {0x4DF9E0, 0xb8}, {0x4E51C3, 0xb9}, {0x4E5217, 0xb9},
    {0x4E61EB, 0xb9}, {0x4E6749, 0xba}, {0x4EF8C0, 0xb8}, {0x4EF964, 0xb8}, {0x4EF9D0, 0xb8}, {0x4F8BBC, 0xba},
    {0x4FD3C3, 0xb9}, {0x4FD53B, 0xba}, {0x4FDAC2, 0xb9}, {0x4FDC43, 0xba}, {0x4FE2F5, 0xba}, {0x4FE548, 0xb9},
    {0x4FE7F3, 0xba}, {0x4FEA52, 0xb9}, {0x4FECCD, 0xba}, {0x4FEF34, 0xb9}, {0x4FF12A, 0xba}, {0x4FF3EA, 0xb9},
    {0x4FF670, 0xba}, {0x4FFA71, 0xb9}, {0x505C20, 0xb8}, {0x505D65, 0xb8}, {0x5061B4, 0xb8}, {0x506535, 0xb8},
    {0x506557, 0xb8}, {0x506587, 0xb8}, {0x5065B3, 0xb8},
};
// push 0xB6F99C (a CMatrix copied from it)
constexpr Site kCameraMatrix[] = {
    {0x4D8119, 0x68}, {0x4D8255, 0x68}, {0x4FD3A9, 0x68}, {0x4FDAA8, 0x68}, {0x4FE52E, 0x68}, {0x4FEA38, 0x68},
    {0x4FEF1A, 0x68}, {0x4FF3D0, 0x68}, {0x4FFA57, 0x68},
};
// mov ecx, 0xB6F028 (CPlaceable::GetRight / GetForward on TheCamera)
constexpr Site kCamera[] = {
    {0x505C64, 0xb9}, {0x505DDB, 0xb9}, {0x50622A, 0xb9},
};

struct Group {
    const Site *sites;
    int count;
    uint32_t original;
};
constexpr Group kGroups[] = {
    {kMatrixPointer, int(sizeof kMatrixPointer / sizeof kMatrixPointer[0]), 0xB6F03C},
    {kPlacementPos, int(sizeof kPlacementPos / sizeof kPlacementPos[0]), 0xB6F02C},
    {kCameraMatrix, int(sizeof kCameraMatrix / sizeof kCameraMatrix[0]), 0xB6F99C},
    {kCamera, int(sizeof kCamera / sizeof kCamera[0]), 0xB6F028},
};

// The fake camera: a CPlaceable whose matrix is the ear, laid out as TheCamera's
// first 0x18 bytes, and the same matrix again for m_mCameraMatrix.
sa::CMatrix g_matrix{};
sa::CPlaceable g_camera{nullptr, {}, 0, &g_matrix};

enum class State { Unchecked, Ready, Unavailable };
State g_state = State::Unchecked;
bool g_ok[4][64];  // the sites whose bytes were as expected
bool g_on = false;

uint32_t replacement(int group) {
    switch (group) {
    case 0: return uint32_t(reinterpret_cast<uintptr_t>(&g_camera.matrix));
    case 1: return uint32_t(reinterpret_cast<uintptr_t>(&g_camera.placement_pos));
    case 2: return uint32_t(reinterpret_cast<uintptr_t>(&g_matrix));
    default: return uint32_t(reinterpret_cast<uintptr_t>(&g_camera));
    }
}

bool check() {
    if (g_state != State::Unchecked) return g_state == State::Ready;
    static_assert(offsetof(sa::CPlaceable, matrix) == 0x14, "CPlaceable layout");
    static_assert(offsetof(sa::CMatrix, pos) == 0x30, "CMatrix layout");
    int good = 0, total = 0;
    for (int g = 0; g < 4; ++g)
        for (int i = 0; i < kGroups[g].count; ++i, ++total) {
            const Site &s = kGroups[g].sites[i];
            uint32_t v;
            std::memcpy(&v, reinterpret_cast<const void *>(uintptr_t(s.operand)), 4);
            g_ok[g][i] = v == kGroups[g].original && *reinterpret_cast<const uint8_t *>(uintptr_t(s.operand - 1)) == s.before;
            good += g_ok[g][i];
        }
    // Another exe or a mod that rewrote this code: nothing, rather than half.
    g_state = good == total ? State::Ready : State::Unavailable;
    fpvlog::write("ear: %d of %d audio camera reads as expected%s", good, total,
                  g_state == State::Ready ? "" : "; the game's sounds stay at the camera");
    return g_state == State::Ready;
}

void patch_all(bool on) {
    for (int g = 0; g < 4; ++g) {
        uint32_t v = on ? replacement(g) : kGroups[g].original;
        for (int i = 0; i < kGroups[g].count; ++i)
            if (g_ok[g][i]) patch::write(kGroups[g].sites[i].operand, &v, 4);
    }
}

}  // namespace

void set(const float pos[3], const float forward[3], const float up[3]) {
    float f[3] = {forward[0], forward[1], forward[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(fl > 1e-4f)) f[0] = 0, f[1] = 1, f[2] = 0, fl = 1;
    for (float &v : f) v /= fl;
    // up made square to forward; the camera's "right" row is up x forward (screen left)
    float d = up[0] * f[0] + up[1] * f[1] + up[2] * f[2];
    float u[3] = {up[0] - d * f[0], up[1] - d * f[1], up[2] - d * f[2]};
    float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(ul > 1e-4f)) {  // looking straight up or down: any up square to it
        u[0] = 0, u[1] = -f[2], u[2] = f[1];  // forward x world x
        ul = std::sqrt(u[1] * u[1] + u[2] * u[2]);
        if (!(ul > 1e-4f)) u[0] = 0, u[1] = 0, u[2] = 1, ul = 1;
    }
    for (float &v : u) v /= ul;
    g_matrix.right = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
    g_matrix.forward = {f[0], f[1], f[2]};
    g_matrix.up = {u[0], u[1], u[2]};
    g_matrix.pos = {pos[0], pos[1], pos[2]};
    g_camera.placement_pos = g_matrix.pos;
    if (g_on || !check()) return;
    patch_all(true);
    g_on = true;
    fpvlog::write("ear: the game's sounds heard from the pilot at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
}

void clear() {
    if (!g_on) return;
    patch_all(false);
    g_on = false;
    fpvlog::write("ear: the game's sounds heard from the camera again");
}

bool on() { return g_on; }

}  // namespace ear
