#include "patch.h"

#include <windows.h>

#include <cstring>
#include <vector>

#include "log.h"

namespace patch {

namespace {

struct Saved {
    uintptr_t addr;
    uint8_t bytes[16];
    unsigned len;
    void *trampoline;
};
std::vector<Saved> g_saved;

}  // namespace

bool is_sa_10_us() {
    // The same check plugin-sdk and CLEO use: a constant in the 1.0 US code.
    return *reinterpret_cast<const uint32_t *>(0x82457C) == 0x94BF;
}

void write(uintptr_t addr, const void *bytes, unsigned size, void *old) {
    DWORD prot;
    VirtualProtect(reinterpret_cast<void *>(addr), size, PAGE_EXECUTE_READWRITE, &prot);
    if (old) std::memcpy(old, reinterpret_cast<void *>(addr), size);
    std::memcpy(reinterpret_cast<void *>(addr), bytes, size);
    VirtualProtect(reinterpret_cast<void *>(addr), size, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(addr), size);
}

uintptr_t redirect_call(uintptr_t site, uintptr_t expected, void *hook) {
    auto *code = reinterpret_cast<const uint8_t *>(site);
    if (code[0] != 0xE8) {
        fpvlog::write("patch: %08X is not a call (opcode %02X); hook skipped", unsigned(site), code[0]);
        return 0;
    }
    int32_t rel;
    std::memcpy(&rel, code + 1, 4);
    uintptr_t target = site + 5 + rel;
    if (target != expected) {
        HMODULE owner = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(target), &owner))
            GetModuleFileNameA(owner, name, MAX_PATH);
        fpvlog::write("patch: call at %08X already goes to %08X in %s (vanilla %08X); chaining",
                      unsigned(site), unsigned(target), name, unsigned(expected));
    }
    uint8_t bytes[5] = {0xE8};
    int32_t new_rel = int32_t(reinterpret_cast<uintptr_t>(hook) - (site + 5));
    std::memcpy(bytes + 1, &new_rel, 4);
    Saved saved{site, {}, 5, nullptr};
    write(site, bytes, 5, saved.bytes);
    g_saved.push_back(saved);
    return target;
}

void *detour(uintptr_t addr, const uint8_t *expected, unsigned len, void *hook) {
    if (len < 5 || len > sizeof(Saved::bytes) || std::memcmp(reinterpret_cast<void *>(addr), expected, len) != 0) {
        fpvlog::write("patch: unexpected code at %08X (another mod?); detour skipped", unsigned(addr));
        return nullptr;
    }
    auto *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    std::memcpy(tramp, expected, len);
    tramp[len] = 0xE9;
    int32_t back = int32_t((addr + len) - (reinterpret_cast<uintptr_t>(tramp) + len + 5));
    std::memcpy(tramp + len + 1, &back, 4);
    uint8_t jmp[16];
    jmp[0] = 0xE9;
    int32_t rel = int32_t(reinterpret_cast<uintptr_t>(hook) - (addr + 5));
    std::memcpy(jmp + 1, &rel, 4);
    for (unsigned i = 5; i < len; ++i) jmp[i] = 0x90;
    Saved saved{addr, {}, len, tramp};
    write(addr, jmp, len, saved.bytes);
    g_saved.push_back(saved);
    return tramp;
}

uintptr_t replace_pointer(uintptr_t addr, void *value) {
    Saved saved{addr, {}, 4, nullptr};
    write(addr, &value, 4, saved.bytes);
    g_saved.push_back(saved);
    uintptr_t old;
    std::memcpy(&old, saved.bytes, 4);
    return old;
}

void restore_all() {
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) {
        write(it->addr, it->bytes, it->len);
        if (it->trampoline) VirtualFree(it->trampoline, 0, MEM_RELEASE);
    }
    g_saved.clear();
}

}  // namespace patch
