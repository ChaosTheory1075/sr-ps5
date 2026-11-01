// SAINTS ROW: ReInstated (PS5) - PlayStation button prompts (see sr_prompts.h).
#include "sr_prompts.h"

#if REX_PLATFORM_PS5

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_file.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(sr_ps_prompts, true, "SAINTS ROW",
                    "Show the games' button hints as the DualSense's buttons (cross, circle, square, triangle, "
                    "L1 R1 L2 R2, the touch pad and OPTIONS) instead of the Xbox 360's");
REXCVAR_DECLARE(bool, sr_skip_intro);  // (sr_intro.cpp: Saints Row 2's logos are a patch here)

// prompts<N>.bin (tools/prompts/make_prompts.py; empty when the build had no game files to make it from)
#define SR_INCBIN(name, file)                                                        \
  __asm__(".section .rodata\n.global " #name "\n.balign 16\n" #name ":\n.incbin \"" \
          file "\"\n.global " #name "_end\n" #name "_end:\n.byte 0\n.previous\n");    \
  extern "C" const unsigned char name[];                                              \
  extern "C" const unsigned char name##_end[];
SR_INCBIN(srps5_prompts, SRPS5_PROMPTS_BIN)

namespace srps5 {
namespace {

struct Patch {
  char archive[32];  // (the file's name, as "00art.rcf")
  uint64_t offset;
  uint32_t size;
  const uint8_t* data;
  uint32_t kind;  // 0 the button prompts, 1 SKIP INTRO (Saints Row 2's logos)
};
std::vector<Patch> g_patches;  // (set before the hook is, then only read)
std::atomic<uint32_t> g_applied{0};

// (whichever thread reads a game file, after the read)
void OnFileRead(const std::string& name, size_t offset, uint8_t* data, size_t size) {
  if (!size) return;
  const bool on[2] = {REXCVAR_GET(sr_ps_prompts), REXCVAR_GET(sr_skip_intro)};
  if (!on[0] && !on[1]) return;
  const uint64_t begin = offset, end = offset + size;
  for (const Patch& p : g_patches) {
    if (p.offset >= end || p.offset + p.size <= begin || !on[p.kind]) continue;
    if (strcasecmp(p.archive, name.c_str()) != 0) continue;
    const uint64_t from = std::max(begin, p.offset), to = std::min(end, p.offset + p.size);
    std::memcpy(data + (from - begin), p.data + (from - p.offset), size_t(to - from));
    const uint32_t count = g_applied.fetch_add(1) + 1;
    if (count <= 40) {
      REXLOG_INFO("SAINTS ROW: ReInstated: {} in {} at {:X} ({} bytes), {} so far",
                  p.kind ? "SKIP INTRO" : "PlayStation buttons", name, from, to - from, count);
    }
  }
}

}  // namespace

void InstallPrompts() {
  static std::atomic<bool> installed{false};
  if (installed.exchange(true)) return;
  const uint8_t* blob = srps5_prompts;
  const size_t blob_size = size_t(srps5_prompts_end - srps5_prompts);
  uint32_t count = 0;
  if (blob_size < 8 || std::memcmp(blob, "PRPF", 4) != 0 ||
      (std::memcpy(&count, blob + 4, 4), 8 + size_t(count) * 48 > blob_size)) {
    REXLOG_WARN("SAINTS ROW: ReInstated: no PlayStation button prompts in this build (Xbox 360's shown)");
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = blob + 8 + i * 48;
    Patch p{};
    std::memcpy(p.archive, e, 31);
    uint32_t offset, size, at;
    std::memcpy(&offset, e + 32, 4);
    std::memcpy(&size, e + 36, 4);
    std::memcpy(&at, e + 40, 4);
    std::memcpy(&p.kind, e + 44, 4);
    if (size_t(at) + size > blob_size || p.kind > 1) continue;
    p.offset = offset;
    p.size = size;
    p.data = blob + at;
    g_patches.push_back(p);
  }
  rex::filesystem::g_sr_file_read_hook.store(&OnFileRead);
  REXLOG_INFO("SAINTS ROW: ReInstated: PlayStation button prompts, {} pieces of the game's files ({})",
              g_patches.size(), REXCVAR_GET(sr_ps_prompts) ? "on" : "off: the Xbox 360's");
}

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
