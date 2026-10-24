// Code patches in gta_sa.exe (1.0 US). Every patch checks the bytes it
// replaces first, so another version of the exe (or another mod that got
// there first) is reported in the log instead of crashing the game.
#pragma once

#include <cstdint>

namespace patch {

// The exe is GTA SA 1.0 US, the only version the addresses are for.
bool is_sa_10_us();

// Points the `call rel32` at `site` to `hook` and returns what it called
// before, which the hook must call in turn: `expected` in the vanilla exe,
// or another mod's hook that got there first (logged). Returns 0 and leaves
// the site alone if it is not a call.
uintptr_t redirect_call(uintptr_t site, uintptr_t expected, void *hook);

// Writes `size` bytes at `addr` (code or data) and returns the old bytes in `old`.
void write(uintptr_t addr, const void *bytes, unsigned size, void *old = nullptr);

// Jumps from `addr` to `hook`, whose first `len` bytes (whole instructions,
// at least 5, matching `expected`) move to a trampoline. Returns the
// trampoline, which calls the original, or null if the bytes differ.
void *detour(uintptr_t addr, const uint8_t *expected, unsigned len, void *hook);

// Replaces a pointer (a vtable slot) and returns the old one.
uintptr_t replace_pointer(uintptr_t addr, void *value);

// Patches made through redirect_call, detour and replace_pointer, kept so they can be undone at once
// (the core does this before it is unloaded).
void restore_all();

}  // namespace patch
