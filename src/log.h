#pragma once

namespace fpvlog {

// Appends a timestamped line to GTASA.FPV.log in the mod's folder.
void write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace fpvlog
