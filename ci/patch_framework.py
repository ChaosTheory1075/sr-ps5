#!/usr/bin/env python3
"""Adapt a gta4-ps5 checkout to build Saints Row instead of GTA IV.

    patch_framework.py <gta4-ps5 checkout> [--host]

- glue/CMakeLists.txt adds sr-recomp (copied in by the workflow) instead of
  gta4-recomp.
- src/graphics/CMakeLists.txt: on the PS5, the generic Xenos renderer
  (rexgpu-xenos) is built as a static library and linked in, and GTA IV's own
  renderer (rexgpu-gta4-native, its shader caches and FSR archive) is left out.
- src/filesystem/devices/disc_image_device.cpp includes direct_io.h on every
  non-Windows host (only the PS5 build had it).

Each edit checks that it applied, so a change upstream fails the build here
instead of somewhere later.
"""
import pathlib
import os
import re
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        if os.environ.get("PATCH_KEEP_GOING"):  # (to list every mismatch at once)
            print(f"MISMATCH {path.name}: {old[:150]!r}")
            return
        sys.exit(f"patch_framework: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"

    edit(fw / "glue" / "CMakeLists.txt",
         'add_subdirectory("${REXSDK_DIR}/gta4-recomp" "${CMAKE_CURRENT_BINARY_DIR}/gta4-recomp")',
         'add_subdirectory("${REXSDK_DIR}/sr-recomp" "${CMAKE_CURRENT_BINARY_DIR}/sr-recomp")')

    gfx = sdk / "src" / "graphics" / "CMakeLists.txt"
    edit(gfx,
         "if(NOT PS5)\nadd_library(rexgpu-xenos SHARED\n",
         "if(PS5)\n"
         "# [sr] the generic Xenos renderer, linked into the PS5 program\n"
         "add_library(rexgpu-xenos STATIC\n"
         "    plugin_main.cpp\n"
         "    ${REXGPU_XENOS_SOURCES}\n"
         ")\n"
         "add_library(rex::gpu-xenos ALIAS rexgpu-xenos)\n"
         "rexgpu_configure_plugin(rexgpu-xenos gpu-xenos)\n"
         "else()\n"
         "add_library(rexgpu-xenos SHARED\n")
    # GTA IV's renderer block: off for this game.
    edit(gfx,
         "if(REXGLUE_USE_VULKAN)\n    if(REXGLUE_BUILD_TESTS)\n",
         "if(REXGLUE_USE_VULKAN AND NOT SR_NO_GTA4_NATIVE)\n    if(REXGLUE_BUILD_TESTS)\n")

    # STFS packages: a contiguous file's blocks follow one another, as the
    # Xbox 360 reads them; its hash entries' next-block links aren't needed.
    # The Blade Wolf DLC's are wrong partway through movie/evd020.usm (the
    # reader then asked for a hash table 4.7 GB into an 857 MB package), so
    # the walk only follows the links for fragmented files.
    edit(sdk / "src" / "filesystem" / "devices" / "stfs_container_device.cpp",
         "          remaining_size -= block_size;\n"
         "          auto block_hash = GetBlockHash(block_index);\n",
         "          remaining_size -= block_size;\n"
         "          if (dir_entry.flags.contiguous) {  // [sr]\n"
         "            ++block_index;\n"
         "            continue;\n"
         "          }\n"
         "          auto block_hash = GetBlockHash(block_index);\n")

    # Bring-up: every file the game opens, and every one it looks for and
    # doesn't find, in the log (the game's data is a few CRI archives, so few
    # lines). The framework had them at debug and trace only.
    io = sdk / "src" / "kernel" / "xboxkrnl" / "xboxkrnl_io.cpp"
    edit(io,
         "    REXKRNL_DEBUG(\"[NtCreateFile] not found: path='{}' -> {:#x}\", target_path, result);\n",
         "    REXKRNL_INFO(\"[NtCreateFile] not found: path='{}' -> {:#x}\", target_path, result);  // [sr]\n")
    edit(io,
         "    REXKRNL_IMPORT_RESULT(\"NtCreateFile\", \"{:#x} handle={:#x}\", result, handle);\n",
         "    REXKRNL_IMPORT_RESULT(\"NtCreateFile\", \"{:#x} handle={:#x}\", result, handle);\n"
         "    REXKRNL_INFO(\"[NtCreateFile] {} -> handle {:#x}\", target_path, handle);  // [sr]\n")

    # A guest crash's own state in the crash line: the app's
    # rex_guest_fault_report (sr_app.cpp) writes the faulting thread's guest
    # registers and callers into a static buffer (no heap in the handler).
    diag = sdk / "src" / "core" / "ps5_diag.cpp"
    edit(diag, "struct RawLine {\n  char text[2048];\n", "struct RawLine {\n  char text[4096];  // [sr] (with the guest's state)\n")
    edit(diag,
         "std::atomic<uint64_t> crashed_at_ms{0};\n",
         "std::atomic<uint64_t> crashed_at_ms{0};\n"
         "// [sr] the faulting thread's guest state, from the app (no heap)\n"
         "extern \"C\" __attribute__((weak)) int rex_guest_fault_report(char* out, int capacity);\n")
    edit(diag,
         "  line.Add(\"\\n\");\n"
         "  const char* path = log_path[0] ? log_path : \"/app0/logs/crash.log\";\n",
         "  if (rex_guest_fault_report) {  // [sr]\n"
         "    static char guest[1536];\n"
         "    if (rex_guest_fault_report(guest, int(sizeof(guest))) > 0) {\n"
         "      line.Add(\"\\n[ps5 crash] guest: \");\n"
         "      line.Add(guest);\n"
         "    }\n"
         "  }\n"
         "  line.Add(\"\\n\");\n"
         "  const char* path = log_path[0] ? log_path : \"/app0/logs/crash.log\";\n")

    # A thread created without a processor (proc_mask 0) stays on its
    # creator's hardware thread, as on the Xbox 360 ("all child threads stay
    # on the same hardware thread as the parent"), and the title's first
    # thread starts on hardware thread 0. The framework handed out 1, 2, ...
    # round-robin, the host's own threads included: the main thread came out
    # as 4, and Saints Row's job system, which maps hardware threads
    # 0, 1, 2, 3 and 5 to its workers (the PCR's current CPU at 0x10C), looked
    # up -1 for it and crashed before the title screen. (Only what the guest
    # sees: ignore_thread_affinities keeps the host's scheduling as it is.)
    edit(sdk / "src" / "system" / "xthread.cpp",
         "  if (!proc_mask) {\n"
         "    next_cpu = (next_cpu + 1) % 6;\n"
         "    return next_cpu;  // is this reasonable?\n",
         "  if (!proc_mask) {\n"
         "    // [sr] the creator's hardware thread; 0 for the title's first thread\n"
         "    (void)next_cpu;\n"
         "    auto* creator = XThread::GetCurrentThread();\n"
         "    return (creator && creator->is_guest_thread()) ? creator->active_cpu() : 0;\n")

    # File writes from physical memory through the raw mapping, as the reads
    # already are (XFile::ReadInternalLocked): the guest's view of a physical
    # page can be protected for the GPU's write watches, which the host's
    # write() can't fault through as the game's own code does. Metal Gear
    # Rising builds its save in physical memory: its saves came out as mostly
    # zeros, and the game couldn't read them back ("Failed to read save data").
    edit(sdk / "src" / "system" / "xfile.cpp",
         "  size_t bytes_written = 0;\n"
         "  X_STATUS result = file_->WriteSync(\n"
         "      std::span<const uint8_t>(memory()->TranslateVirtual(buffer_guest_address), buffer_length),\n"
         "      size_t(byte_offset), &bytes_written);\n",
         "  size_t bytes_written = 0;\n"
         "  // [sr] physical memory through the raw mapping (no GPU watch protection)\n"
         "  rex::memory::BaseHeap* write_heap = memory()->LookupHeap(buffer_guest_address);\n"
         "  const uint8_t* write_source =\n"
         "      write_heap && write_heap->heap_type() == memory::HeapType::kGuestPhysical\n"
         "          ? memory()->TranslatePhysical(\n"
         "                static_cast<rex::memory::PhysicalHeap*>(write_heap)->GetPhysicalAddress(buffer_guest_address))\n"
         "          : memory()->TranslateVirtual(buffer_guest_address);\n"
         "  X_STATUS result = file_->WriteSync(std::span<const uint8_t>(write_source, buffer_length),\n"
         "                                     size_t(byte_offset), &bytes_written);\n")

    # The physical pages' stale-protection recovery is right, but it logged
    # every one: 120,000 lines in a session (some 350 a second in play), each
    # a write to the log file from the faulting thread. The first 20, then a
    # count every 10,000.
    edit(sdk / "src" / "system" / "xmemory.cpp",
         "                               rex::memory::PageAccess::kReadWrite, nullptr)) {\n"
         "        REXSYS_WARN(\n",
         "                               rex::memory::PageAccess::kReadWrite, nullptr)) {\n"
         "        // [sr] The page was protected because the GPU holds a copy of it\n"
         "        // (a texture): the write about to happen makes that copy stale, so\n"
         "        // the GPU is told, as TriggerCallbacks does for a watched page.\n"
         "        // Without it a video frame decoded through this alias never reached\n"
         "        // the screen: Saints Row's cutscene movies froze on a frame\n"
         "        // while their sound went on.\n"
         "        if (const uint32_t phys = GetPhysicalAddress(virtual_address); phys != UINT32_MAX) {\n"
         "          const uint32_t page = uint32_t(host_page_size);\n"
         "          auto invalidation_lock = global_critical_region_.Acquire();\n"
         "          for (auto callback : physical_memory_invalidation_callbacks_)\n"
         "            callback->first(callback->second, phys & ~(page - 1), page, true);\n"
         "        }\n"
         "        static std::atomic<uint64_t> recovered{0};  // [sr]\n"
         "        const uint64_t n = ++recovered;\n"
         "        if (n % 10000 == 0) REXSYS_INFO(\"Recovered stale physical page protection: {} times\", n);\n"
         "        if (n <= 20) REXSYS_WARN(\n")

    # Cutscene movies (CRI Sofdec, decoded by the CPU into the same few
    # textures every frame) froze for up to 9 s while their sound went on:
    # the frames reached the GPU only when a write watch happened to fire or
    # the stale page scan (128 pages a frame: all memory in about 8.5 s at
    # 30 FPS) came round to them. sr_004.log: the title movie's planes
    # reloaded every frame on its first showing, and the scan found the
    # cutscene movie's pages (08B74000-08B7C000) changed with no watch fired.
    # Whatever leaves a page unwatched (the 16 KB host pages hold four guest
    # pages, the E0 view is 4 KB off the others, the hot page list fills), a
    # texture the CPU rewrites no longer depends on the watches: the first
    # draw of each frame that binds it compares its pages with the hashes
    # taken at their upload, and a changed page is invalidated as a fired
    # watch would, so the texture is reloaded for that draw. Checked: linear
    # textures (the movie's planes; the game's other textures are tiled) and
    # textures on pages the CPU changed in the last 300 frames; at most 16 MB
    # hashed a frame.
    sm_h = sdk / "include" / "rex" / "graphics" / "shared_memory.h"
    edit(sm_h,
         """  uint32_t CheckStalePages(uint32_t pages_per_frame);
""",
         """  uint32_t CheckStalePages(uint32_t pages_per_frame);

  // [sr] The pages of a texture's range compared with their hashes taken at upload (once a
  // frame a texture, from TextureCache::RequestTextures); a changed page is invalidated as a
  // fired write watch would. always: checked even if the CPU didn't change its pages lately.
  // Returns whether any page was invalidated.
  bool CheckDynamicRange(uint32_t start, uint32_t length, bool always);
  uint32_t frame() const { return hot_frame_.load(std::memory_order_relaxed); }
  // [sr] A linear texture (a movie frame's plane): its guest data hashed (the movie log), and
  // its range invalidated so that it's reloaded from guest memory, whatever the watches say.
  uint64_t HashGuestRange(uint32_t start, uint32_t length);
  void InvalidateRangeNow(uint32_t start, uint32_t length);
""")
    edit(sm_h,
         """  std::vector<uint32_t> hot_page_checked_batch_;
""",
         """  std::vector<uint32_t> hot_page_checked_batch_;
  // [sr] CheckDynamicRange's budget
  static constexpr uint32_t kDynamicCheckBytesPerFrame = 16u << 20;
  uint32_t dynamic_check_frame_ = 0;
  uint32_t dynamic_check_bytes_ = 0;
""")
    edit(sdk / "src" / "graphics" / "shared_memory.cpp",
         """namespace {
thread_local bool invalidation_from_write = false;  // [crackdown]
""",
         """bool SharedMemory::CheckDynamicRange(uint32_t start, uint32_t length, bool always) {
  // [sr]
  if (!length || start >= kBufferSize || page_hashes_.size() != (kBufferSize >> page_size_log2_)) {
    return false;
  }
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  const uint32_t frame = hot_frame_.load(std::memory_order_relaxed);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = std::min(uint32_t((uint64_t(start) + length - 1) >> page_size_log2_),
                                      uint32_t(page_hashes_.size() - 1));
  if (!always) {
    bool recent = false;
    for (uint32_t page = page_first; page <= page_last && !recent; ++page) {
      const uint32_t last = page_invalidation_frame_[page];
      recent = last && frame - last <= kHotPageQuietFrames;
    }
    if (!recent) {
      return false;
    }
  }
  if (dynamic_check_frame_ != frame) {
    dynamic_check_frame_ = frame;
    dynamic_check_bytes_ = 0;
  }
  bool invalidated = false;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (dynamic_check_bytes_ >= kDynamicCheckBytesPerFrame) {
      break;
    }
    const uint64_t stored = page_hashes_[page];
    if (!stored) {
      continue;
    }
    const uint32_t address = page << page_size_log2_;
    const uint32_t block = page >> 6;
    const uint64_t bit = uint64_t(1) << (page & 63);
    {
      // (as CheckStalePages: a valid page is committed; GPU-written ones and those the guest made
      // unreadable are left alone)
      auto global_lock = global_critical_region_.Acquire();
      const uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
      if (!valid_flags || !(valid_flags[block] & bit) ||
          (system_page_flags_valid_and_gpu_written_[block] & bit)) {
        continue;
      }
      uint32_t protect = 0;
      auto* physical_heap = memory_.GetPhysicalHeap();
      if (!physical_heap || !physical_heap->QueryProtect(address, &protect) ||
          !(protect & memory::kMemoryProtectRead)) {
        continue;
      }
      dynamic_check_bytes_ += page_size;
      if (XXH3_64bits(memory_.TranslatePhysical(address), page_size) == stored) {
        continue;
      }
    }
    page_hashes_[page] = 0;
    memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           0xA0000000u + address, page_size, true, true);
    memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           0xC0000000u + address, page_size, true, true);
    if (address >= 0x1000) {
      memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                             0xE0000000u + address - 0x1000, page_size, true, true);
    }
    MemoryInvalidationCallback(address, page_size, true);
    invalidated = true;
  }
  if (invalidated) {
    static std::atomic<uint64_t> dynamic_reloads{0};
    const uint64_t n = ++dynamic_reloads;
    if (n <= 5 || !(n & (n - 1))) {
      REXGPU_INFO("sr: a CPU-written texture at {:08X} changed with no write watch fired ({} so far)",
                  start, n);
    }
  }
  return invalidated;
}

uint64_t SharedMemory::HashGuestRange(uint32_t start, uint32_t length) {
  // [sr]
  if (!length || start >= kBufferSize) {
    return 0;
  }
  length = std::min(length, kBufferSize - start);
  return XXH3_64bits(memory_.TranslatePhysical(start), length);
}

void SharedMemory::InvalidateRangeNow(uint32_t start, uint32_t length) {
  // [sr] (as CheckStalePages does for a changed page, each physical view's watches included)
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_size = uint32_t(1) << page_size_log2_;
  const uint32_t first = start & ~(page_size - 1);
  const uint32_t size = ((start + length - first) + page_size - 1) & ~(page_size - 1);
  for (uint32_t page = first >> page_size_log2_; page < ((first + size) >> page_size_log2_) &&
                                                 page < page_hashes_.size();
       ++page) {
    page_hashes_[page] = 0;
  }
  memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                         0xA0000000u + first, size, true, true);
  memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                         0xC0000000u + first, size, true, true);
  if (first >= 0x1000) {
    memory_.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           0xE0000000u + first - 0x1000, size, true, true);
  }
  MemoryInvalidationCallback(first, size, true);
}

namespace {
thread_local bool invalidation_from_write = false;  // [crackdown]
""")
    tc_h = sdk / "include" / "rex" / "graphics" / "pipeline" / "texture" / "cache.h"
    edit(tc_h,
         """    void MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);
""",
         """    void MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);
    // [sr] the frame its pages were last compared with their upload hashes; a linear one's data
    // hash then (the movie log)
    uint32_t dynamic_check_frame = 0;
    uint64_t dynamic_content_hash = 0;
""")
    edit(tc_h,
         """  uint64_t current_submission_index_ = 0;
""",
         """  uint64_t current_submission_index_ = 0;
  uint32_t dynamic_check_frame_ = 0;  // [sr]
""")
    tc = sdk / "src" / "graphics" / "pipeline" / "texture" / "cache.cpp"
    # The movie log: each second a linear texture is drawn, how many frames drew it and in how
    # many of them the game had written new data into it. A freeze with new data each frame is
    # the GPU's side; one with none, the game's (its decoder or its clock).
    edit(tc,
         """void TextureCache::RequestTextures(uint32_t used_texture_mask) {
""",
         """namespace {
// [sr] the movie log
struct MovieLogEntry {
  uint32_t base = 0, frames = 0, changed = 0, last_frame = 0;
};
MovieLogEntry g_movie_log[24];
std::chrono::steady_clock::time_point g_movie_log_time;
void MovieLogFrame(uint32_t base, uint32_t frame, bool changed) {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_movie_log_time >= std::chrono::seconds(1)) {
    std::string line;
    for (MovieLogEntry& e : g_movie_log) {
      if (e.frames) {
        line += fmt::format(" {:08X}: {} new of {}", e.base, e.changed, e.frames);
      }
      e = MovieLogEntry();
    }
    if (!line.empty()) {
      REXGPU_INFO("sr movie 1 s (linear, tiled k_8 (+1), 256 KB+ (+2, new: outdated by a write watch) textures: frames with new data of frames drawn):{}", line);
    }
    g_movie_log_time = now;
  }
  MovieLogEntry* slot = nullptr;
  for (MovieLogEntry& e : g_movie_log) {
    if (e.frames && e.base == base) {
      slot = &e;
      break;
    }
    if (!slot && !e.frames) {
      slot = &e;
    }
  }
  if (!slot || slot->last_frame == frame + 1) {
    return;
  }
  slot->base = base;
  slot->last_frame = frame + 1;
  ++slot->frames;
  slot->changed += changed ? 1 : 0;
}
}  // namespace

void TextureCache::RequestTextures(uint32_t used_texture_mask) {
""")
    edit(tc, "#include <mutex>\n", "#include <chrono>\n#include <mutex>\n#include <string>\n\n#include <fmt/format.h>\n")
    edit(tc,
         """    ResetTextureBindings();
  }

  // Update the texture keys and the textures.
""",
         """    ResetTextureBindings();
  }

  // [sr] each frame's first draws look at every binding again (CheckDynamicRange)
  const uint32_t dynamic_frame = shared_memory().frame();
  if (dynamic_frame != dynamic_check_frame_) {
    dynamic_check_frame_ = dynamic_frame;
    texture_bindings_in_sync_ = 0;
  }

  // Update the texture keys and the textures.
""")
    edit(tc,
         """    if (g_before_texture_read) {
      Texture* loading = load_unsigned_data ? binding.texture
""",
         """    // [sr] a texture the CPU rewrites (a cutscene movie's frame): its pages compared with their
    // upload hashes once a frame, reloaded if they changed with no write watch fired
    for (Texture* dynamic : {binding.texture, binding.texture_signed}) {
      // [sr] every texture of 256 KB or more in the movie log too, whether a write watch
      // outdated it this frame (+2): a cutscene's planes, whatever their format
      if (dynamic && dynamic->dynamic_check_frame != dynamic_frame && dynamic->GetGuestBaseSize() >= (256u << 10)) {
        const TextureKey big_key = dynamic->key();
        if (big_key.tiled && big_key.format == xenos::TextureFormat::k_8) {
        } else if (big_key.tiled || (dynamic->outdated_mask() & Texture::kOutdatedBitBase)) {
          MovieLogFrame((big_key.base_page << 12) | 2u | (big_key.tiled ? 1u : 0u), dynamic_frame,
                        (dynamic->outdated_mask() & Texture::kOutdatedBitBase) != 0);
        }
      }
      if (!dynamic || dynamic->dynamic_check_frame == dynamic_frame ||
          (dynamic->outdated_mask() & Texture::kOutdatedBitBase)) {
        continue;
      }
      dynamic->dynamic_check_frame = dynamic_frame;
      const TextureKey dynamic_key = dynamic->key();
      if (dynamic_key.scaled_resolve || !dynamic->GetGuestBaseSize()) {
        continue;
      }
      // [sr] Bink's planes on the 360 are tiled k_8 textures the CPU decodes into (Saints Row 2's
      // cutscenes stayed black: the first, black, frame was all the GPU ever saw): those too are
      // compared with their last data once a frame, and reloaded when it changed
      const bool movie_plane = !dynamic_key.tiled || (dynamic_key.format == xenos::TextureFormat::k_8 &&
                                                      dynamic->GetGuestBaseSize() <= (8u << 20));
      if (movie_plane) {
        // [sr] a linear texture (the movie's planes): reloaded every frame, and whether its data
        // changed counted for the movie log (a tiled one: only when it changed; logged with +1)
        const uint32_t base = dynamic_key.base_page << 12;
        const uint64_t hash = shared_memory().HashGuestRange(base, dynamic->GetGuestBaseSize());
        // (only when the guest's data changed, a linear one too, and not on the first look: a frame
        // the GPU resolved into it (Saints Row 2's Bink cutscenes) isn't in guest memory, and
        // reloading from there every frame put back the black one under it)
        const bool first = dynamic->dynamic_content_hash == 0;
        const bool changed = hash != dynamic->dynamic_content_hash;
        MovieLogFrame(base | (dynamic_key.tiled ? 1u : 0u), dynamic_frame, changed && !first);
        dynamic->dynamic_content_hash = hash;
        if (changed && !first) {
          shared_memory().InvalidateRangeNow(base, dynamic->GetGuestBaseSize());
          (dynamic == binding.texture ? load_unsigned_data : load_signed_data) = true;
        }
        continue;
      }
      if (shared_memory().CheckDynamicRange(dynamic_key.base_page << 12, dynamic->GetGuestBaseSize(),
                                            !dynamic_key.tiled)) {
        (dynamic == binding.texture ? load_unsigned_data : load_signed_data) = true;
      }
    }
    if (g_before_texture_read) {
      Texture* loading = load_unsigned_data ? binding.texture
""")

    # NtReadFile on an asynchronous file (opened without FILE_SYNCHRONOUS_IO)
    # returns STATUS_PENDING, its outcome in the I/O status block, as Xenia's
    # and NtWriteFile here do - also when the read was done on the spot. The
    # fork returned STATUS_SUCCESS then: Saints Row's save loading
    # takes only the pending path as a read, and showed "Failed to read save
    # data" for every save.
    edit(io,
         "    completion.Complete(result, bytes_read);\n"
         "    REXKRNL_IMPORT_RESULT(\n"
         "        \"NtReadFile\", \"{:#x} inline=1 synchronous={} completion_port={} wait={} transferred={}\",\n"
         "        result, file->is_synchronous(), has_completion_port, ev ? \"event\" : \"file\", bytes_read);\n"
         "    return result;\n",
         "    completion.Complete(result, bytes_read);\n"
         "    REXKRNL_IMPORT_RESULT(\n"
         "        \"NtReadFile\", \"{:#x} inline=1 synchronous={} completion_port={} wait={} transferred={}\",\n"
         "        result, file->is_synchronous(), has_completion_port, ev ? \"event\" : \"file\", bytes_read);\n"
         "    if (XSUCCEEDED(result) && !file->is_synchronous()) return X_STATUS_PENDING;  // [sr]\n"
         "    return result;\n")

    # XamSwapDisc reaches an app that defines rex_app_swap_disc (Max Payne 3's
    # disc 2); without one (this game has one disc), the stub as before.
    xam_info = sdk / "src" / "kernel" / "xam" / "xam_info.cpp"
    edit(xam_info,
         "u32 XamSwapDisc_entry(u32 disc_number) {\n",
         "extern \"C\" __attribute__((weak)) void rex_app_swap_disc(uint32_t disc_number);\n\n"
         "u32 XamSwapDisc_entry(u32 disc_number) {\n"
         "  REXKRNL_INFO(\"XamSwapDisc({})\", (uint32_t)disc_number);\n"
         "  if (rex_app_swap_disc) rex_app_swap_disc(disc_number);\n")
    # XGetLanguage returned English whatever user_language said (the Xbox 360's
    # console language, which the games' text follows): the menu's LANGUAGE
    # setting sets it (sr_menu.cpp), as Metal Gear Rising PS5's.
    edit(xam_info,
         "#include <rex/system/kernel_state.h>\n",
         "#include <rex/system/flags.h>\n#include <rex/system/kernel_state.h>\n")
    edit(xam_info,
         "u32 XGetLanguage_entry() {\n",
         "u32 XGetLanguage_entry() {\n"
         "  if (const uint32_t language = REXCVAR_GET(user_language); language >= 1 && language <= 12)\n"
         "    return language;\n")

    # HD texture replacement in the Vulkan texture cache (ci/fw-files/
    # src/graphics/vulkan/hd_textures.cpp): its members, and three hooks.
    files = pathlib.Path(__file__).resolve().parent / "fw-files"
    for f in files.rglob("*"):
        if f.is_file():
            dst = sdk / f.relative_to(files)
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(f.read_bytes())
            print(f"copied   {dst}")
    edit(gfx,
         "    vulkan/texture_cache.cpp\n",
         "    vulkan/texture_cache.cpp\n    vulkan/hd_textures.cpp\n")
    # [sr] Max Payne 3 RePrescribed's (its 1.0.5): the presenter for FSR 1, SMAA 1x.
    # The graphics system (its presenter), for the app: the menu's UPSCALING
    # setting goes to the presenter, which took present_effect at its start,
    # before the menu (sr_app.cpp).
    edit(sdk / "include" / "rex" / "rex_app.h",
         "  ui::Window* window() const { return window_.get(); }\n",
         "  ui::Window* window() const { return window_.get(); }\n"
         "  system::IGraphicsSystem* graphics_system() const { return config_.graphics.get(); }  // [mp3]\n")
    # SMAA 1x as a swap post effect (swap_post_effect smaa: ci/fw-files/src/graphics/vulkan/
    # mp3_smaa.cpp, shaders from tools/smaa). It runs where FXAA does, on the same gamma-corrected
    # source image, before the presenter's upscaling (FSR 1): SMAA cleans the edges with less blur
    # than FXAA, so the upscaler doesn't enlarge the jaggies. FXAA stays the fallback.
    edit(sdk / "include" / "rex" / "graphics" / "command_processor.h",
         "    kFxaaExtreme,\n  };\n",
         "    kFxaaExtreme,\n    kSmaa,  // [mp3]\n  };\n")
    edit(sdk / "src" / "graphics" / "graphics_system.cpp",
         "    .allowed({\"none\", \"fxaa\", \"fxaa_extreme\"})\n",
         "    .allowed({\"none\", \"fxaa\", \"fxaa_extreme\", \"smaa\"})  // [mp3]\n")
    edit(sdk / "src" / "graphics" / "graphics_system.cpp",
         "  if (lowered == \"fxaa_extreme\" || lowered == \"extreme\") {\n",
         "  if (lowered == \"smaa\") {  // [mp3]\n"
         "    return rex::graphics::CommandProcessor::SwapPostEffect::kSmaa;\n"
         "  }\n"
         "  if (lowered == \"fxaa_extreme\" || lowered == \"extreme\") {\n")
    vcph_smaa = sdk / "include" / "rex" / "graphics" / "vulkan" / "command_processor.h"
    edit(vcph_smaa,
         "  void BindExternalComputePipeline(VkPipeline pipeline);\n",
         "  void BindExternalComputePipeline(VkPipeline pipeline);\n"
         "  // [mp3] SMAA (mp3_smaa.cpp): false if unavailable (FXAA then).\n"
         "  bool Mp3SmaaRecord(VkImageView color_view, VkImage dest_image, VkImageView dest_view,\n"
         "                     uint32_t width, uint32_t height, uint32_t frame_index);\n"
         "  bool Mp3SmaaInitialize();\n"
         "  void Mp3SmaaShutdown();\n")
    edit(vcph_smaa,
         "  VkImageLayout swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;\n",
         "  VkImageLayout swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;\n"
         "  struct Mp3Smaa;\n"
         "  Mp3Smaa* mp3_smaa_ = nullptr;  // [mp3] created on first use\n")
    vcp_smaa = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    edit(vcp_smaa,
         "        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||\n"
         "                        swap_post_effect == SwapPostEffect::kFxaaExtreme;\n",
         "        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||\n"
         "                        swap_post_effect == SwapPostEffect::kFxaaExtreme ||\n"
         "                        swap_post_effect == SwapPostEffect::kSmaa;  // [mp3] FXAA's source image\n")
    edit(vcp_smaa,
         "            BindExternalComputePipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme\n"
         "                                            ? swap_fxaa_extreme_pipeline_\n"
         "                                            : swap_fxaa_pipeline_);\n"
         "            deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);\n",
         "            if (swap_post_effect != SwapPostEffect::kSmaa ||\n"
         "                !Mp3SmaaRecord(swap_fxaa_source_image_view_, vulkan_context.image(),\n"
         "                               vulkan_context.image_view(), guest_output_width,\n"
         "                               guest_output_height, swap_frame_index)) {  // [mp3]\n"
         "              BindExternalComputePipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme\n"
         "                                              ? swap_fxaa_extreme_pipeline_\n"
         "                                              : swap_fxaa_pipeline_);\n"
         "              deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);\n"
         "            }\n")
    edit(vcp_smaa,
         "void VulkanCommandProcessor::ShutdownContext() {\n"
         "  GpuLane::Get().Drain();  // [crackdown]\n"
         "  AwaitAllQueueOperationsCompletion();\n",
         "void VulkanCommandProcessor::ShutdownContext() {\n"
         "  GpuLane::Get().Drain();  // [crackdown]\n"
         "  AwaitAllQueueOperationsCompletion();\n"
         "  Mp3SmaaShutdown();  // [mp3]\n")
    edit(gfx,
         "    vulkan/texture_cache.cpp\n    vulkan/hd_textures.cpp\n",
         "    vulkan/texture_cache.cpp\n    vulkan/hd_textures.cpp\n    vulkan/mp3_smaa.cpp\n")
    tc_h = sdk / "include" / "rex" / "graphics" / "vulkan" / "texture_cache.h"
    edit(tc_h,
         "class VulkanTextureCache final : public TextureCache {\n",
         "struct HdReplacement;\nstruct HdReplacer;\nstruct HdDds;\n\n"
         "class VulkanTextureCache final : public TextureCache {\n")
    edit(tc_h,
         "    VkImage image() const { return image_; }\n",
         "    VkImage image() const { return image_; }\n\n"
         "    // HD textures (hd_textures.cpp): the replacement this texture's views come from.\n"
         "    std::shared_ptr<HdReplacement> hd_replacement;\n"
         "    uint64_t hd_pending = 0;  // (the replacement being read in)\n"
         "    uint64_t hd_last_load = 0;  // (submission)\n"
         "    uint8_t hd_fast_reloads = 0;\n"
         "    uint64_t hd_head = 0;  // (its data's first 4 KB when it was last hashed)\n"
         "    uint64_t hd_seen_head = 0;  // (and at its last load)\n"
         "    friend class VulkanTextureCache;\n")
    edit(tc_h,
         "  std::pair<uint64_t, uint64_t> scaled_resolve_last_written_range_{0, 0};\n};\n",
         "  std::pair<uint64_t, uint64_t> scaled_resolve_last_written_range_{0, 0};\n\n"
         "  // HD textures (hd_textures.cpp)\n"
         "  HdReplacer* Hd() const;\n"
         "  void HdBeginSubmission();\n"
         "  void HdAfterLoad(VulkanTexture& texture, bool load_base);\n"
         "  std::shared_ptr<HdReplacement> HdLoad(uint64_t hash);\n"
         "  std::shared_ptr<HdReplacement> HdUpload(uint64_t hash, HdDds& dds);\n"
         "  void HdPoll();\n"
         "  VkImageView HdView(VulkanTexture& texture, bool is_signed, uint32_t host_swizzle, bool is_array) const;\n"
         "  mutable HdReplacer* hd_ = nullptr;\n"
         "  mutable bool hd_disabled_ = false;\n};\n")
    tc = sdk / "src" / "graphics" / "vulkan" / "texture_cache.cpp"
    edit(tc,
         "  TextureCache::RequestTextures(used_texture_mask);\n",
         "  HdPoll();\n  TextureCache::RequestTextures(used_texture_mask);\n")
    edit(tc,
         "  TextureCache::BeginSubmission(new_submission_index);\n",
         "  TextureCache::BeginSubmission(new_submission_index);\n  HdBeginSubmission();\n")
    edit(tc,
         "    copy_region.imageExtent.depth = std::max(depth >> level, UINT32_C(1));\n  }\n\n  return true;\n}\n",
         "    copy_region.imageExtent.depth = std::max(depth >> level, UINT32_C(1));\n  }\n\n"
         "  HdAfterLoad(vulkan_texture, load_base);\n  return true;\n}\n")
    edit(tc,
         "VkImageView VulkanTextureCache::VulkanTexture::GetView(bool is_signed, uint32_t host_swizzle,\n"
         "                                                       bool is_array) {\n",
         "VkImageView VulkanTextureCache::VulkanTexture::GetView(bool is_signed, uint32_t host_swizzle,\n"
         "                                                       bool is_array) {\n"
         "  if (hd_replacement) {\n"
         "    const auto& cache = static_cast<const VulkanTextureCache&>(texture_cache());\n"
         "    if (VkImageView view = cache.HdView(*this, is_signed, host_swizzle, is_array)) return view;\n"
         "  }\n")

    # REX_DUMP_IMAGE=<file>: codegen writes the loaded (decrypted) image
    # there, for tools/find_rpf_key.py (the archives' AES key is in it).
    bv = sdk / "src" / "codegen" / "binary_view.cpp"
    edit(bv,
         "  return view;\n}\n",
         "  if (const char* dump = std::getenv(\"REX_DUMP_IMAGE\")) {\n"
         "    if (std::FILE* f = std::fopen(dump, \"wb\")) {\n"
         "      for (const auto& s : view.sections_) {\n"
         "        if (!s.data || s.baseAddress < view.baseAddress_) continue;\n"
         "        std::fseek(f, long(s.baseAddress - view.baseAddress_), SEEK_SET);\n"
         "        std::fwrite(s.data, 1, s.size, f);\n"
         "      }\n"
         "      std::fclose(f);\n"
         "    }\n"
         "  }\n"
         "  return view;\n}\n")
    edit(bv, "#include <limits>\n", "#include <cstdio>\n#include <cstdlib>\n#include <limits>\n")

    edit(sdk / "src" / "filesystem" / "devices" / "disc_image_device.cpp",
         "#include <rex/platform.h>\n#if REX_PLATFORM_PS5\n#include <rex/filesystem/direct_io.h>\n#endif\n",
         "#include <rex/platform.h>\n#include <rex/filesystem/direct_io.h>\n")

    # The bring-up counters (rex/ps5_diag.h) are counted outside PS5-only code
    # too; elsewhere than the PS5 nothing defines them (ps5_diag.cpp is PS5's).
    (sdk / "src" / "core" / "ps5_diag_host.cpp").write_text(
        "// [sr] rex/ps5_diag.h's counters off the PS5 (ps5_diag.cpp is PS5-only).\n"
        "#include <rex/ps5_diag.h>\n\n"
        "namespace rex::ps5diag {\n"
        "std::atomic<uint64_t> counters[kCount];\n"
        "uintptr_t guest_arena_lo = 0, guest_arena_hi = 0;\n"
        "void RegisterThread(const char*) {}\n"
        "}  // namespace rex::ps5diag\n")
    edit(sdk / "src" / "core" / "CMakeLists.txt",
         "    target_sources(rexcore PRIVATE ps5_libc_extras.c ps5_diag.cpp)\n",
         "    target_sources(rexcore PRIVATE ps5_libc_extras.c ps5_diag.cpp)\n"
         "else()\n"
         "    target_sources(rexcore PRIVATE ps5_diag_host.cpp)  # [sr]\n"
         "endif()\n"
         "if(PS5)\n")

    # PS5 audio: a sample the game's mixer left NaN / infinite (std::clamp lets
    # NaN through, and its int16 cast is garbage: loud crackle) is played as 0,
    # and every 10 s with any, the count and the loudest sample are logged.
    drv = sdk / "src" / "audio" / "ps5" / "ps5_audio_driver.cpp"
    edit(drv,
         "  bool started = false, in_gap = false;\n  uint32_t gaps = 0, silent = 0, played = 0;\n",
         "  bool started = false, in_gap = false;\n  uint32_t gaps = 0, silent = 0, played = 0;\n"
         "  uint32_t bad = 0, clipped = 0;  // [sr]\n  float peak = 0.0f;\n")
    edit(drv,
         "      gaps = silent = played = 0;\n",
         "      if (bad || clipped) {\n"
         "        REXAPU_INFO(\"PS5 audio: {} NaN/inf samples (played as 0), {} clipped, peak {:.2f} in 10 s\", bad,\n"
         "                    clipped, peak);\n"
         "      }\n"
         "      bad = clipped = 0, peak = 0.0f;\n"
         "      gaps = silent = played = 0;\n")
    edit(drv,
         "        float s = std::clamp(stereo[i], -1.0f, 1.0f);\n",
         "        float s = stereo[i];\n"
         "        if (!std::isfinite(s)) s = 0.0f, ++bad;\n"
         "        peak = std::max(peak, std::fabs(s));\n"
         "        if (std::fabs(s) > 1.0f) ++clipped;\n"
         "        s = std::clamp(s, -1.0f, 1.0f);\n")
    edit(drv, "#include <algorithm>\n", "#include <algorithm>\n#include <cmath>\n")

    # Audio diagnostics: what the XMA decoder did (contexts decoded, decode
    # errors, output buffers full, input run dry) every 10 s, and the PS5
    # output's first 90 s of sound in logs/sr_audio.raw (48 kHz s16 stereo;
    # only with logs/capture.txt)
    # with a count of frames identical to the one before (a stuck voice
    # buzzes) -- the noise, to look at instead of guessing.
    xc = sdk / "src" / "audio" / "xma_context.cpp"
    edit(xc,
         "namespace rex::audio {\n\nusing stream::BitStream;\n",
         "namespace rex::audio {\n\n"
         "// [sr] decoded, errors, output full, input dry, kicks of a disabled context\n"
         "std::atomic<uint64_t> sr_xma_stats[5];\n"
         "static void SrXmaReport() {\n"
         "  static std::atomic<int64_t> last{0};\n"
         "  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(\n"
         "      std::chrono::steady_clock::now().time_since_epoch()).count();\n"
         "  int64_t prev = last.load();\n"
         "  if (now - prev < 10 || !last.compare_exchange_strong(prev, now)) return;\n"
         "  REXAPU_INFO(\"XMA 10 s: {} decodes, {} errors, {} output full, {} input dry\",\n"
         "              sr_xma_stats[0].exchange(0), sr_xma_stats[1].exchange(0),\n"
         "              sr_xma_stats[2].exchange(0), sr_xma_stats[3].exchange(0));\n"
         "}\n\n"
         "using stream::BitStream;\n")
    edit(xc, "#include <algorithm>\n#include <cstring>\n",
         "#include <algorithm>\n#include <atomic>\n#include <chrono>\n#include <cstring>\n")
    edit(xc,
         "  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {\n",
         "  SrXmaReport();\n"
         "  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {\n"
         "    ++sr_xma_stats[2];\n")
    edit(xc,
         "    Decode(&data);\n    Consume(&output_rb, &data);\n",
         "    Decode(&data);\n    Consume(&output_rb, &data);\n    ++sr_xma_stats[0];\n")
    edit(xc,
         "    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {\n      break;\n",
         "    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {\n"
         "      ++sr_xma_stats[data.error_status == 4 ? 1 : 3];\n      break;\n")

    drv = sdk / "src" / "audio" / "ps5" / "ps5_audio_driver.cpp"
    edit(drv,
         "  uint32_t bad = 0, clipped = 0;  // [sr]\n",
         "  uint32_t bad = 0, clipped = 0, repeats = 0;  // [sr]\n"
         "  std::vector<int16_t> previous(kChannelSamples * 2, 0);\n"
         "  FILE* capture = nullptr;\n  uint64_t captured = 0;  // (frames)\n")
    edit(drv,
         "      if (bad || clipped) {\n",
         "      REXAPU_INFO(\"PS5 audio 10 s: {} frames repeated exactly, peak {:.2f}\", repeats, peak);\n"
         "      repeats = 0;\n"
         "      if (bad || clipped) {\n")
    edit(drv,
         "    sceAudioOutOutput(port_, pcm);\n  }\n",
         "    if (frame) {\n"
         "      bool nonzero = false;\n"
         "      for (uint32_t i = 0; i < kChannelSamples * 2 && !nonzero; ++i) nonzero = pcm[i] != 0;\n"
         "      if (nonzero && std::memcmp(previous.data(), pcm, sizeof(pcm)) == 0) ++repeats;\n"
         "      std::memcpy(previous.data(), pcm, sizeof(pcm));\n"
         "      // (only with logs/capture.txt, as the frame captures)\n"
         "      static const bool capture_wanted = [] {\n"
         "        std::FILE* f = std::fopen(\"/app0/logs/capture.txt\", \"rb\");\n"
         "        if (f) std::fclose(f);\n"
         "        return f != nullptr;\n"
         "      }();\n"
         "      if (!capture && !captured && !capture_wanted) captured = 1;\n"
         "      if (!capture && !captured) {\n"
         "        capture = std::fopen(\"/app0/logs/sr_audio.raw\", \"wb\");\n"
         "        if (!capture) captured = 1;\n"
         "      }\n"
         "      if (capture) {\n"
         "        std::fwrite(pcm, sizeof(pcm), 1, capture);\n"
         "        if (++captured >= uint64_t(kFrequency) * 90 / kChannelSamples) {\n"
         "          std::fclose(capture);\n          capture = nullptr;\n"
         "          REXAPU_INFO(\"PS5 audio: 90 s captured to logs/sr_audio.raw\");\n"
         "        }\n"
         "      }\n"
         "    }\n"
         "    sceAudioOutOutput(port_, pcm);\n  }\n"
         "  if (capture) std::fclose(capture);\n")
    edit(drv, "#include <cmath>\n", "#include <cmath>\n#include <cstdio>\n#include <vector>\n")

    # XMA decoder: where the framework's decoder differs from Xenia canary's
    # (xma_context_new.cc), each feeding the codec a wrong frame. Gameplay had
    # 30-50% of decode passes end in error_status 4 and the capture a 62.5 Hz
    # buzz (one 512-sample frame at 32 kHz):
    # - a packet that only continues an earlier frame was skipped by moving to
    #   the next packet in memory, which in an interleaved (multi-stream:
    #   cutscene and gameplay surround) buffer belongs to another stream;
    #   Xenia follows the packet's own skip count.
    # - a packet past the end of the current buffer was always taken as the
    #   next buffer's first packet; it is (index - count) of the next buffer.
    # - the search for the next frame stopped at the end of the current
    #   buffer instead of going on into the next one.
    # - Work set error_status 4 when one pass made no progress (waiting for
    #   the game's next buffer is not an error) and dropped the rest of a
    #   decoded frame on an error; Xenia stops without an error and drains
    #   the frame first.
    # Plus a count per error site, logged with the 10 s report.
    edit(xc,
         "std::atomic<uint64_t> sr_xma_stats[5];\n",
         "std::atomic<uint64_t> sr_xma_stats[5];\n"
         "std::atomic<uint64_t> sr_xma_err[16];  // per error_status = 4 site in Decode\n"
         "std::atomic<uint64_t> sr_xma_stale;  // kicks that began with error_status 4\n")
    edit(xc,
         "              sr_xma_stats[2].exchange(0), sr_xma_stats[3].exchange(0));\n",
         "              sr_xma_stats[2].exchange(0), sr_xma_stats[3].exchange(0));\n"
         "  std::string sites;\n"
         "  for (auto& e : sr_xma_err) sites += std::to_string(e.exchange(0)) + \" \";\n"
         "  REXAPU_INFO(\"XMA errors by site: {}| begun in error {}, stalled {}\", sites,\n"
         "              sr_xma_stale.exchange(0), sr_xma_stats[4].exchange(0));\n")
    edit(xc, "#include <chrono>\n#include <cstring>\n",
         "#include <chrono>\n#include <cstring>\n#include <string>\n")
    edit(xc,
         "  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);\n",
         "  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);\n"
         "  if (data.error_status == 4) ++sr_xma_stale;\n")
    edit(xc,
         "    // Invalid packets must not leave the decoder worker spinning forever.\n"
         "    if (data.IsAnyInputBufferValid() && data.error_status != 4 &&\n"
         "        data.input_buffer_read_offset == old_offset && data.current_buffer == old_buffer &&\n"
         "        data.IsAnyInputBufferValid() == old_valid &&\n"
         "        current_frame_remaining_subframes_ == old_remaining &&\n"
         "        remaining_subframe_blocks_in_output_buffer_ == old_capacity) {\n"
         "      data.error_status = 4;\n"
         "      handoff::Record(\"xma-error\", id_, {old_offset, old_buffer}, \"decode-no-progress\");\n"
         "    }\n"
         "    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {\n"
         "      ++sr_xma_stats[data.error_status == 4 ? 1 : 3];\n      break;\n    }\n",
         "    // [sr] As Xenia: finish delivering a decoded frame before stopping\n"
         "    // on an error or on running out of input, and stop without an error\n"
         "    // when a pass made no progress (waiting for the game's next buffer).\n"
         "    if ((!data.IsAnyInputBufferValid() || data.error_status == 4) &&\n"
         "        current_frame_remaining_subframes_ == 0) {\n"
         "      ++sr_xma_stats[data.error_status == 4 ? 1 : 3];\n      break;\n    }\n"
         "    if (data.input_buffer_read_offset == old_offset && data.current_buffer == old_buffer &&\n"
         "        data.IsAnyInputBufferValid() == old_valid &&\n"
         "        current_frame_remaining_subframes_ == old_remaining &&\n"
         "        remaining_subframe_blocks_in_output_buffer_ == old_capacity) {\n"
         "      ++sr_xma_stats[4];\n      break;\n    }\n")
    # The next buffer's packet (index - count), within that buffer's length.
    edit(xc,
         "  const uint8_t next_buffer_index = data->current_buffer ^ 1;\n"
         "  if (!data->IsInputBufferValid(next_buffer_index)) {\n"
         "    return nullptr;\n  }\n",
         "  const uint8_t next_buffer_index = data->current_buffer ^ 1;\n"
         "  if (!data->IsInputBufferValid(next_buffer_index)) {\n"
         "    return nullptr;\n  }\n"
         "  next_packet_index -= current_input_packet_count;  // [sr]\n"
         "  if (next_packet_index >= data->GetInputBufferPacketCount(next_buffer_index)) {\n"
         "    return nullptr;\n  }\n")
    edit(xc,
         "  return memory()->TranslatePhysical(next_buffer_address);\n",
         "  return memory()->TranslatePhysical(next_buffer_address) + next_packet_index * kBytesPerPacket;\n")
    # A continuation-only packet: follow its skip count to this stream's next.
    edit(xc,
         "      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;\n"
         "    }\n"
         "    next_packet_index++;\n",
         "      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;\n"
         "    }\n"
         "    const uint8_t next_skip = xma::GetPacketSkipCount(next_packet);  // [sr]\n"
         "    if (next_skip == 0xFF) {\n      break;\n    }\n"
         "    next_packet_index += next_skip + 1;\n")
    # readback_resolve "fast" (Saints Row's HDR adaptation reads the
    # luminance it resolved on the CPU, Xenia game-compatibility #42) hands
    # the game the previous resolve's buffer without checking that the GPU
    # has written it: the slot can still be queued, or be fresh memory never
    # written at all. The game's exposure then adapts to garbage (NaN sticks)
    # and gameplay goes dark. As the memexport readback beside it does:
    # remember the submission that wrote each slot, wait for it when it is in
    # flight, and when it is this very submission (a second resolve to the
    # same place this frame) fall back to a full sync of the current one.
    vcp = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    edit(vcp,
         "  PushBufferMemoryBarrier(readback.buffers[write_index], 0, written_length,\n"
         "                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,\n"
         "                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);\n",
         "  PushBufferMemoryBarrier(readback.buffers[write_index], 0, written_length,\n"
         "                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,\n"
         "                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);\n"
         "  readback.submission_written[write_index] = GetCurrentSubmission();  // [sr]\n"
         "  readback.written_size[write_index] = written_length;\n")
    edit(vcp,
         "  bool is_cache_miss = false;\n"
         "  if (use_delayed_sync && (readback.buffers[read_index] == VK_NULL_HANDLE ||\n"
         "                           written_length > readback.sizes[read_index] ||\n"
         "                           readback.mapped_data[read_index] == nullptr)) {\n",
         "  bool is_cache_miss = false;\n"
         "  // [sr] The previous slot is usable once the GPU has written it; until\n"
         "  // then the guest keeps the older data (waiting here held the CPU to\n"
         "  // the GPU's previous frame, every frame).\n"
         "  if (use_delayed_sync && readback.buffers[read_index] != VK_NULL_HANDLE &&\n"
         "      readback.submission_written[read_index] &&\n"
         "      readback.submission_written[read_index] > submission_completed_ &&\n"
         "      written_length <= readback.written_size[read_index]) {\n"
         "    CheckSubmissionFenceAndDeviceLoss(0);\n"
         "    if (readback.submission_written[read_index] > submission_completed_) {\n"
         "      rex::sr_perf::g_n[rex::sr_perf::kReadbackLate].fetch_add(1, std::memory_order_relaxed);\n"
         "      // (the same slot takes the next copy too: the older one gets time)\n"
         "      return true;\n"
         "    }\n"
         "  }\n"
         "  if (use_delayed_sync && (readback.buffers[read_index] == VK_NULL_HANDLE ||\n"
         "                           written_length > readback.sizes[read_index] ||\n"
         "                           readback.mapped_data[read_index] == nullptr ||\n"
         "                           written_length > readback.written_size[read_index] ||\n"
         "                           !readback.submission_written[read_index] ||\n"
         "                           readback.submission_written[read_index] > submission_completed_)) {\n")

    # XMA loops: every gameplay decode error (site 2, 22k per 10 s) was the
    # framework's own "invalid loop interval" check, which Xenia doesn't
    # have. It rejects legal loop data (loop_subframe_end is a 3-bit field,
    # 4..7 meaning the whole frame) and stops every looping voice (ambience,
    # music, gunfire tails) at its loop end; the error then sticks ("begun
    # in error"). The loop code below copes with any values (the output
    # limit is clamped to the frame). The loop-start skip is applied as
    # Xenia does: only a skip shorter than the frame.
    edit(xc,
         "  if (is_loop_end_frame &&\n"
         "      (data->loop_subframe_end > 3 || data->loop_subframe_skip > 4 ||\n",
         "  if (false && is_loop_end_frame &&  // [sr] as Xenia: no such check\n"
         "      (data->loop_subframe_end > 3 || data->loop_subframe_skip > 4 ||\n")
    edit(xc,
         "      current_frame_remaining_subframes_ -=\n"
         "          std::min<uint8_t>(skip, current_frame_remaining_subframes_);\n",
         "      if (skip < current_frame_remaining_subframes_) {  // [sr] as Xenia\n"
         "        current_frame_remaining_subframes_ -= skip;\n      }\n")

    # Frame capture (with a logs/capture.txt file): at 90, 150, 210 and 300 s
    # after the first draw, one whole frame goes to logs/sr_frame_<s>.bin:
    # every draw (render target, depth, blend registers, shaders, the pixel
    # shader's float constants, the texture fetch constants it samples),
    # every resolve (copy registers and the resolved bytes as the CPU sees
    # them) in order, and each shader's ucode once. tools/sr_frame.py turns
    # it into images of every pass.
    edit(vcp, "#include <cstring>\n#include <iterator>\n",
         "#include <chrono>\n#include <cstdio>\n#include <cstring>\n#include <iterator>\n"
         "#include <unordered_set>\n")
    edit(vcp,
         "namespace rex::graphics::vulkan {\n\nnamespace {\n",
         "namespace rex::graphics::vulkan {\n\nnamespace {\n\n"
         "// [sr] Frame capture (see ci/patch_framework.py).\n"
         "struct SrCapture {\n"
         "  std::FILE* file = nullptr;\n"
         "  uint64_t frame = 0, armed_frame = 0;\n"
         "  uint32_t taken = 0, seq = 0;\n"
         "  std::chrono::steady_clock::time_point start{};\n"
         "  std::unordered_set<uint64_t> shaders;\n"
         "};\n"
         "SrCapture sr_cap;\n"
         "constexpr uint32_t kSrCaptureSeconds[] = {90, 150, 210, 300};\n"
         "void SrCapWrite(const void* p, size_t n) {\n"
         "  if (sr_cap.file) std::fwrite(p, 1, n, sr_cap.file);\n"
         "}\n"
         "template <typename T>\n"
         "void SrCapPut(const T& v) {\n  SrCapWrite(&v, sizeof(v));\n}\n"
         "bool SrCapActive(uint64_t frame) {\n"
         "  const auto now = std::chrono::steady_clock::now();\n"
         "  if (sr_cap.start == std::chrono::steady_clock::time_point{}) sr_cap.start = now;\n"
         "  if (sr_cap.file && frame != sr_cap.frame) {\n"
         "    std::fclose(sr_cap.file);\n    sr_cap.file = nullptr;\n"
         "    REXGPU_INFO(\"sr capture: frame {} written ({} records)\", sr_cap.frame, sr_cap.seq);\n"
         "  }\n"
         "  // (only with logs/capture.txt: a capture holds its frame up)\n"
         "  static const bool enabled = [] {\n"
         "    std::FILE* f = std::fopen(\"/app0/logs/capture.txt\", \"rb\");\n"
         "    if (f) std::fclose(f);\n"
         "    return f != nullptr;\n"
         "  }();\n"
         "  if (!sr_cap.file && enabled && sr_cap.taken < std::size(kSrCaptureSeconds)) {\n"
         "    const auto seconds =\n"
         "        std::chrono::duration_cast<std::chrono::seconds>(now - sr_cap.start).count();\n"
         "    if (!sr_cap.armed_frame && seconds >= kSrCaptureSeconds[sr_cap.taken]) {\n"
         "      sr_cap.armed_frame = frame + 1;\n"
         "    }\n"
         "    if (sr_cap.armed_frame && frame >= sr_cap.armed_frame) {\n"
         "      char path[64];\n"
         "      std::snprintf(path, sizeof(path), \"/app0/logs/sr_frame_%u.bin\",\n"
         "                    unsigned(kSrCaptureSeconds[sr_cap.taken]));\n"
         "      sr_cap.file = std::fopen(path, \"wb\");\n"
         "      sr_cap.frame = frame;\n      sr_cap.armed_frame = 0;\n      sr_cap.seq = 0;\n"
         "      ++sr_cap.taken;\n      sr_cap.shaders.clear();\n"
         "      if (sr_cap.file) {\n"
         "        SrCapWrite(\"SRF\", 4);\n        SrCapPut(uint32_t(1));\n        SrCapPut(frame);\n"
         "      }\n"
         "    }\n"
         "  }\n"
         "  return sr_cap.file != nullptr;\n"
         "}\n"
         "void SrCapShader(const Shader* shader) {\n"
         "  if (!shader || !sr_cap.shaders.insert(shader->ucode_data_hash()).second) return;\n"
         "  SrCapWrite(\"S\", 1);\n"
         "  SrCapPut(shader->ucode_data_hash());\n"
         "  SrCapPut(uint32_t(shader->type()));\n"
         "  SrCapPut(uint32_t(shader->ucode_dword_count()));\n"
         "  SrCapWrite(shader->ucode_dwords(), shader->ucode_dword_count() * 4);\n"
         "}\n"
         "void SrCapRegs(const RegisterFile& regs) {\n"
         "  static const uint32_t kRegs[] = {\n"
         "      XE_GPU_REG_RB_SURFACE_INFO,  XE_GPU_REG_RB_COLOR_INFO,     XE_GPU_REG_RB_COLOR1_INFO,\n"
         "      XE_GPU_REG_RB_COLOR2_INFO,   XE_GPU_REG_RB_COLOR3_INFO,    XE_GPU_REG_RB_DEPTH_INFO,\n"
         "      XE_GPU_REG_RB_COLOR_MASK,    XE_GPU_REG_RB_DEPTHCONTROL,   XE_GPU_REG_RB_MODECONTROL,\n"
         "      XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_COPY_CONTROL,   XE_GPU_REG_RB_COPY_DEST_INFO,\n"
         "      XE_GPU_REG_RB_COPY_DEST_PITCH, XE_GPU_REG_RB_COPY_DEST_BASE};\n"
         "  for (uint32_t r : kRegs) SrCapPut(regs[r]);\n"
         "}\n")
    edit(vcp,
         "  bool memexport_writes_possible = memexport_used_vertex || memexport_used_pixel;\n",
         "  bool memexport_writes_possible = memexport_used_vertex || memexport_used_pixel;\n"
         "  if (SrCapActive(frame_current_)) {  // [sr] frame capture: the draw\n"
         "    SrCapShader(vertex_shader);\n    SrCapShader(pixel_shader);\n"
         "    SrCapWrite(\"D\", 1);\n    SrCapPut(sr_cap.seq++);\n"
         "    SrCapRegs(regs);\n"
         "    SrCapPut(uint32_t(prim_type));\n    SrCapPut(index_count);\n"
         "    SrCapPut(vertex_shader->ucode_data_hash());\n"
         "    SrCapPut(pixel_shader ? pixel_shader->ucode_data_hash() : uint64_t(0));\n"
         "    SrCapWrite(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + 256 * 4], 256 * 16);\n"
         "    const auto* bindings = pixel_shader ? &pixel_shader->texture_bindings() : nullptr;\n"
         "    SrCapPut(uint32_t(bindings ? bindings->size() : 0));\n"
         "    if (bindings) {\n"
         "      for (const auto& binding : *bindings) {\n"
         "        SrCapPut(binding.fetch_constant);\n"
         "        SrCapWrite(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 6 * binding.fetch_constant],\n"
         "                    6 * 4);\n"
         "      }\n"
         "    }\n"
         "  }\n")
    edit(vcp,
         "    resolve_succeeded = IssueCopy_ReadbackResolvePath();\n  }\n",
         "    resolve_succeeded = IssueCopy_ReadbackResolvePath();\n  }\n"
         "  if (SrCapActive(frame_current_)) {  // [sr] frame capture: the resolve\n"
         "    SrCapWrite(\"C\", 1);\n    SrCapPut(sr_cap.seq++);\n"
         "    SrCapRegs(*register_file_);\n"
         "    SrCapPut(uint32_t(resolve_succeeded));\n"
         "    SrCapPut(frame_flow_resolve_address_);\n"
         "    const uint8_t* resolved = frame_flow_resolve_length_ <= (16u << 20)\n"
         "        ? memory_->TranslatePhysical(frame_flow_resolve_address_) : nullptr;\n"
         "    SrCapPut(uint32_t(resolved ? frame_flow_resolve_length_ : 0));\n"
         "    if (resolved) SrCapWrite(resolved, frame_flow_resolve_length_);\n"
         "  }\n")

    # A draw whose depth test can never pass and that has stencil off writes
    # nothing (no color, depth or stencil): skip it before it binds render
    # targets. Saints Row issues one (depth only, zfunc NEVER) on its depth
    # buffer reinterpreted as 640-wide 2x MSAA between the G-buffer and the
    # stencil-tested deferred lighting; on host render targets binding it
    # converts the depth/stencil away and back (lossy) for nothing.
    edit(vcp,
         "  if (surface_pitch_is_zero && is_rasterization_done) {\n",
         "  if (is_rasterization_done && !memexport_used_vertex) {  // [sr]\n"
         "    const auto depth_control = draw_util::GetNormalizedDepthControl(regs);\n"
         "    if (depth_control.z_enable && !depth_control.stencil_enable &&\n"
         "        depth_control.zfunc == xenos::CompareFunction::kNever) {\n"
         "      ++frame_flow_no_effect_draws_;\n      return true;\n    }\n"
         "  }\n"
         "  if (surface_pitch_is_zero && is_rasterization_done) {\n")
    vrt = sdk / "src" / "graphics" / "vulkan" / "render_target_cache.cpp"
    edit(vrt,
         "  if (GetPath() == Path::kPixelShaderInterlock) {\n"
         "    // The first operation will likely be drawing.\n",
         "  REXGPU_INFO(\"VulkanRenderTargetCache: {} path (fragment shader interlock {})\",  // [sr]\n"
         "              path_ == Path::kPixelShaderInterlock ? \"fragment shader interlock (EDRAM)\"\n"
         "                                                   : \"host render targets\",\n"
         "              fsi_path_supported ? \"supported\" : \"unsupported\");\n"
         "  if (GetPath() == Path::kPixelShaderInterlock) {\n"
         "    // The first operation will likely be drawing.\n")

    # Integer number format (Xenia canary packs and fetches it, this
    # framework's resolve shaders and texture fetch only know fractions).
    # Saints Row resolves its HDR scene (7e3) to k_16_16_16_16 with number
    # format "unsigned integer" and exponent bias +11 (a 16-bit integer with 11
    # fractional bits) and samples it with num_format integer and exp_adjust
    # -13. Packed as a 0..1 fraction every lit pixel saturated (all white);
    # fetched as a fraction it would read 65535 times too dark.
    # - resolve: for 16-bit components, 2^(bias - 16) through the fraction
    #   packer writes the same integers as 2^bias through an integer packer
    #   (to 1/65536).
    # - fetch: an integer-format 16-bit texture gets exp_adjust + 16 in the
    #   fetch constants the shaders read, the fraction times 65535 (to 1/65536).
    drw = sdk / "src" / "graphics" / "util" / "draw.cpp"
    edit(drw,
         "  // Handle k_16_16 and k_16_16_16_16 range.\n"
         "  info_out.copy_dest_info.copy_dest_exp_bias = exp_bias;\n",
         "  // [sr] Unsigned integer destinations through the fraction packer.\n"
         "  if (!is_depth && rb_copy_dest_info.copy_dest_number ==\n"
         "                       xenos::SurfaceNumberFormat::kUnsignedInteger) {\n"
         "    switch (xenos::ColorFormat(dest_format)) {\n"
         "      case xenos::ColorFormat::k_16:\n"
         "      case xenos::ColorFormat::k_16_16:\n"
         "      case xenos::ColorFormat::k_16_16_16_16:\n"
         "        exp_bias = std::max(exp_bias - int32_t(16), int32_t(-32));\n"
         "        break;\n"
         "      default:\n"
         "        break;\n"
         "    }\n"
         "  }\n"
         "  // Handle k_16_16 and k_16_16_16_16 range.\n"
         "  info_out.copy_dest_info.copy_dest_exp_bias = exp_bias;\n")
    edit(vcp,
         "      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);\n",
         "      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);\n"
         "      // [sr] Integer-format 16-bit textures: fraction * 65535 (see\n"
         "      // ci/patch_framework.py).\n"
         "      for (uint32_t i = 0; i < 32; ++i) {\n"
         "        uint32_t* fetch = reinterpret_cast<uint32_t*>(mapping) + 6 * i;\n"
         "        const uint32_t type = fetch[0] & 3, format = fetch[1] & 0x3F;\n"
         "        if ((type != 2 && type != 0) || !(fetch[3] & 1) ||\n"
         "            (format != 24 && format != 25 && format != 26)) {\n"
         "          continue;\n"
         "        }\n"
         "        const int32_t exp_adjust =\n"
         "            std::min((int32_t(fetch[3] << 13) >> 26) + 16, int32_t(31));\n"
         "        fetch[3] = (fetch[3] & ~(UINT32_C(0x3F) << 13)) | ((uint32_t(exp_adjust) & 0x3F) << 13);\n"
         "      }\n")

    # Readback only what the CPU reads: "fast" copied every resolve of every
    # frame back (55 a frame, ~40 MB with the 7.5 MB HDR copies) and memcpy'd
    # it into guest memory on the GPU thread. The game's CPU reads are its
    # luminance / exposure resolves (a few KB); the GPU never needs these
    # copies. Up to 256 KB, as Xenia's purpose for it (HDR adaptation).
    edit(vcp,
         "  if (!memory_->TranslatePhysical(written_address)) {\n    return true;\n  }\n",
         "  if (!memory_->TranslatePhysical(written_address)) {\n    return true;\n  }\n"
         "  if (written_length > (256u << 10) && !sr_cap.file) {  // [sr] only what the CPU reads\n"
         "    return true;\n  }\n")
    edit(vcp,
         "  uint32_t read_index = write_index;\n  if (use_delayed_sync) {\n",
         "  if (sr_cap.file) use_delayed_sync = false;  // [sr] captured frames: current data\n"
         "  uint32_t read_index = write_index;\n  if (use_delayed_sync) {\n")

    # Texture fetch result exponent bias (exp_adjust) from the fetch constant
    # word 3, bits 13:18 (xenos.h, and the DXBC translator). The SPIR-V
    # translator took bits 13:18 of word 4 - part of the LOD bias - so on
    # Vulkan no texture got its exp_adjust, and textures with a LOD bias got a
    # random scale. Saints Row reads its HDR scene, bloom and luminance with
    # exp_adjust -13 (+16 above for the integer format): every post-processing
    # pass saw the scene 8 times too dark (the dark, grey gameplay).
    sft = sdk / "src" / "graphics" / "pipeline" / "shader" / "spirv_translator_fetch.cpp"
    edit(sft,
         "        spv::Id result_exponent_bias = builder_->createBinBuiltinCall(\n"
         "            type_float_, ext_inst_glsl_std_450_, GLSLstd450Ldexp, const_float_1_,\n"
         "            builder_->createTriOp(spv::OpBitFieldSExtract, type_int_, fetch_constant_word_4_signed,\n"
         "                                  builder_->makeUintConstant(13), builder_->makeUintConstant(6)));\n",
         "        // [sr] Word 3 (see ci/patch_framework.py), not word 4.\n"
         "        id_vector_temp_.clear();\n"
         "        id_vector_temp_.push_back(const_int_0_);\n"
         "        id_vector_temp_.push_back(\n"
         "            builder_->makeIntConstant(int((fetch_constant_word_0_index + 3) >> 2)));\n"
         "        id_vector_temp_.push_back(\n"
         "            builder_->makeIntConstant(int((fetch_constant_word_0_index + 3) & 3)));\n"
         "        spv::Id fetch_constant_word_3_signed = builder_->createUnaryOp(\n"
         "            spv::OpBitcast, type_int_,\n"
         "            builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,\n"
         "                                                             uniform_fetch_constants_,\n"
         "                                                             id_vector_temp_),\n"
         "                                 spv::NoPrecision));\n"
         "        spv::Id result_exponent_bias = builder_->createBinBuiltinCall(\n"
         "            type_float_, ext_inst_glsl_std_450_, GLSLstd450Ldexp, const_float_1_,\n"
         "            builder_->createTriOp(spv::OpBitFieldSExtract, type_int_, fetch_constant_word_3_signed,\n"
         "                                  builder_->makeUintConstant(13), builder_->makeUintConstant(6)));\n")

    # Where the GPU thread's time goes (include/rex/sr_perf.h, from
    # fw-files): timers around the stalls and the big steps, printed per frame
    # every 10 s as "sr perf". The framework's "ps5 timing" line only counts
    # the GTA IV renderer's frames (all zeros here).
    def include_perf(path):
        text = path.read_text()
        if "#include <rex/sr_perf.h>" in text:
            return
        i = text.index("\n#include ") + 1
        path.write_text(text[:i] + "#include <rex/sr_perf.h>\n" + text[i:])
    cpb = sdk / "src" / "graphics" / "command_processor.cpp"
    vtc = sdk / "src" / "graphics" / "vulkan" / "texture_cache.cpp"
    vpc = sdk / "src" / "graphics" / "vulkan" / "pipeline_cache.cpp"
    for f in (cpb, vcp, vtc):
        include_perf(f)
    edit(vcp, "#include <rex/sr_perf.h>\n",
         "#include <rex/sr_perf.h>\n#include <rex/ps5_diag.h>\n#include <rex/frame_stats.h>\n"
         "#if defined(__PROSPERO__)\n#include <pthread.h>\n#endif\n")
    edit(cpb,
         "      PrepareForWait();\n      bool poll_background_work = PollBackgroundWork();\n",
         "      rex::sr_perf::Scope sr_perf_idle(rex::sr_perf::kIdle);\n"
         "      PrepareForWait();\n      bool poll_background_work = PollBackgroundWork();\n")
    edit(cpb,
         "      if (wait >= 0x100) {\n        PrepareForWait();\n",
         "      if (wait >= 0x100) {\n"
         "        rex::sr_perf::Scope sr_perf_wait(rex::sr_perf::kWaitRegMem);\n"
         "        PrepareForWait();\n")
    edit(vcp,
         "    VkResult wait_result =\n        vulkan_device->WaitForFences(",
         "    rex::sr_perf::Scope sr_perf_fence(rex::sr_perf::kFenceWait);\n"
         "    VkResult wait_result =\n        vulkan_device->WaitForFences(")
    edit(vcp,
         "bool VulkanCommandProcessor::EndSubmission(bool is_swap) {\n",
         "bool VulkanCommandProcessor::EndSubmission(bool is_swap) {\n"
         "  rex::sr_perf::Scope sr_perf_submit(rex::sr_perf::kSubmit);\n")
    edit(vcp,
         "                                       bool major_mode_explicit) {\n",
         "                                       bool major_mode_explicit) {\n"
         "  rex::sr_perf::g_n[rex::sr_perf::kDraw].fetch_add(1, std::memory_order_relaxed);\n")
    edit(vcp,
         "bool VulkanCommandProcessor::IssueCopy() {\n",
         "bool VulkanCommandProcessor::IssueCopy() {\n"
         "  rex::sr_perf::Scope sr_perf_copy(rex::sr_perf::kCopy);\n")
    edit(vtc,
         "bool VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,\n"
         "                                                               bool load_mips) {\n",
         "bool VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,\n"
         "                                                               bool load_mips) {\n"
         "  rex::sr_perf::Scope sr_perf_texture(rex::sr_perf::kTextureLoad);\n")
    edit(vcp,
         "bool VulkanCommandProcessor::EndGuestOcclusionQuery(uint32_t sample_count_address) {\n",
         "bool VulkanCommandProcessor::EndGuestOcclusionQuery(uint32_t sample_count_address) {\n"
         "  rex::sr_perf::Scope sr_perf_occlusion(rex::sr_perf::kOcclusion);\n")
    edit(vcp,
         "                                       uint32_t frontbuffer_height) {\n  SCOPE_profile_cpu_f(\"gpu\");\n",
         "                                       uint32_t frontbuffer_height) {\n  SCOPE_profile_cpu_f(\"gpu\");\n"
         "  {\n"
         "    // [sr] the GPU thread's time per frame, every 10 s (rex/sr_perf.h)\n"
         "    using namespace rex::sr_perf;\n"
         "    static uint64_t last_us = 0, frames = 0, prev_ns[kCount], prev_n[kCount];\n"
         "    const uint64_t now = NowUs();\n"
         "    {\n"
         "      // [sr] a frame over 100 ms: when, and where the GPU thread's time went in it\n"
         "      static uint64_t swap_us = 0, swap_ns[kCount];\n"
         "      uint64_t cur[kCount];\n"
         "      for (uint32_t c = 0; c < kCount; ++c) cur[c] = g_ns[c].load(std::memory_order_relaxed);\n"
         "      if (swap_us && now - swap_us > 100000) {\n"
         "        auto d = [&](Counter c) { return double(cur[c] - swap_ns[c]) / 1e6; };\n"
         "        REXGPU_INFO(\"sr frame hitch: {:.0f} ms; GPU thread: idle (waiting for the game) {:.0f}, wait_reg_mem {:.0f}, \"\n"
         "                    \"fences {:.0f}, submit {:.0f}, draws {:.0f} (pipelines {:.0f}, texture loads {:.0f}), copies {:.0f}, \"\n"
         "                    \"occlusion {:.0f}\",\n"
         "                    double(now - swap_us) / 1000.0, d(kIdle), d(kWaitRegMem), d(kFenceWait), d(kSubmit), d(kDraw),\n"
         "                    d(kPipeline), d(kTextureLoad), d(kCopy), d(kOcclusion));\n"
         "      }\n"
         "      swap_us = now;\n"
         "      for (uint32_t c = 0; c < kCount; ++c) swap_ns[c] = cur[c];\n"
         "    }\n"
         "    rex::frame_stats::NotePresent();  // [sr] the frame rate counter (ps5/sr_overlay.cpp)\n"
         "    ++frames;\n"
         "    if (!last_us) {\n"
         "      last_us = now;\n"
         "      frames = 0;\n"
         "    } else if (now - last_us >= 10000000) {\n"
         "      double ms[kCount], n[kCount];\n"
         "      for (uint32_t c = 0; c < kCount; ++c) {\n"
         "        const uint64_t ns = g_ns[c].load(std::memory_order_relaxed), cn = g_n[c].load(std::memory_order_relaxed);\n"
         "        ms[c] = double(ns - prev_ns[c]) / 1e6 / double(frames);\n"
         "        n[c] = double(cn - prev_n[c]) / double(frames);\n"
         "        prev_ns[c] = ns;\n"
         "        prev_n[c] = cn;\n"
         "      }\n"
         "      const double frame_ms = double(now - last_us) / 1000.0 / double(frames);\n"
         "      REXGPU_INFO(\"sr perf: {:.1f} fps, {:.1f} ms a frame; GPU thread per frame: idle (waiting for the game) {:.1f}, \"\n"
         "                  \"wait_reg_mem {:.1f} ({:.1f}x), GPU fences {:.1f}, submit {:.1f} ({:.1f}x), swap {:.1f}, \"\n"
         "                  \"draws (the rest) {:.1f} ({:.0f}; texture loads {:.1f} ({:.1f}), HD {:.1f} ({:.1f})), \"\n"
         "                  \"copies {:.1f} ({:.1f}), late readbacks {:.2f}, occlusion queries {:.1f} ({:.1f})\",\n"
         "                  1000.0 / frame_ms, frame_ms, ms[kIdle], ms[kWaitRegMem], n[kWaitRegMem], ms[kFenceWait],\n"
         "                  ms[kSubmit], n[kSubmit], ms[kSwap],\n"
         "                  std::max(0.0, frame_ms - ms[kIdle] - ms[kWaitRegMem] - ms[kFenceWait] - ms[kSubmit] -\n"
         "                                    ms[kSwap] - ms[kCopy] - ms[kOcclusion]),\n"
         "                  n[kDraw], ms[kTextureLoad], n[kTextureLoad], ms[kHd], n[kHd], ms[kCopy], n[kCopy],\n"
         "                  n[kReadbackLate], ms[kOcclusion], n[kOcclusion]);\n"
         "      last_us = now;\n"
         "      frames = 0;\n"
         "    }\n"
         "  }\n"
         "#if defined(__PROSPERO__)\n"
         "  {\n"
         "    // [sr] The GPU thread is the game's limit: a core of its own (CPU 0,\n"
         "    // its twin CPU 1 left empty), the lane on CPU 4, the rest on 2-3 and 6-12 (moved\n"
         "    // when it is new: KeepOtherThreadsOn only touches changed ones).\n"
         "    static uint64_t cpus_us = 0;\n"
         "    const uint64_t now = rex::sr_perf::NowUs();\n"
         "    if (!cpus_us) {\n"
         "      rex::frame_stats::scePthreadSetaffinity(pthread_self(), 1);\n"
         "      REXGPU_INFO(\"sr: GPU thread on CPU 0, the lane on 4, the others on CPUs 2-3 and 6-12\");\n"
         "    }\n"
         "    if (!cpus_us || now - cpus_us >= 5000000) {\n"
         "      cpus_us = now;\n"
         "      rex::ps5diag::KeepOtherThreadsOn(0x1FCC, reinterpret_cast<uintptr_t>(pthread_self()), 0, 0);\n"
         "    }\n"
         "  }\n"
         "#endif\n"
         "  rex::sr_perf::Scope sr_perf_swap(rex::sr_perf::kSwap);\n")

    # Startup: the shader storage's pipelines were all created before the
    # game started (InitializeShaderStorage blocks the launch), one storage
    # that grows every session: 40 s and more of black screen at 5 a second.
    # With async_shader_compilation they go to the creation threads instead
    # (lowest priority) while the game starts; one drawn before it's built gets
    # what a new one gets: the placeholder at once, the real one queued.
    vpc2 = sdk / "src" / "graphics" / "vulkan" / "pipeline_cache.cpp"
    edit(vpc2,
         "  if (!pipeline_creations.empty()) {\n"
         "    {\n"
         "      std::lock_guard<std::mutex> lock(creation_request_lock_);\n"
         "      startup_loading_ = true;\n",
         "  // [sr] async: created at first use (see ci/patch_framework.py)\n"
         "  if (!pipeline_creations.empty() && REXCVAR_GET(async_shader_compilation) &&\n"
         "      !creation_threads_.empty()) {\n"
         "    REXGPU_INFO(\"{} stored graphics pipelines queued for the creation threads\", pipeline_creations.size());\n"
         "    {\n"
         "      std::lock_guard<std::mutex> lock(creation_request_lock_);\n"
         "      for (PipelineCreationArguments& creation : pipeline_creations) {\n"
         "        creation.priority = pipeline_util::kPriorityLowest;\n"
         "        creation_queue_.push(creation);\n"
         "      }\n"
         "    }\n"
         "    creation_request_cond_.notify_all();\n"
         "    pipeline_creations.clear();\n"
         "  }\n"
         "  if (!pipeline_creations.empty()) {\n"
         "    {\n"
         "      std::lock_guard<std::mutex> lock(creation_request_lock_);\n"
         "      startup_loading_ = true;\n")
    edit(vpc2,
         "    if (found_pipeline == VK_NULL_HANDLE) {\n"
         "      PipelineCreationArguments creation_arguments;\n",
         "    if (found_pipeline == VK_NULL_HANDLE && use_async) {\n"
         "      // [sr] a stored one's first use: the placeholder, the real one queued\n"
         "      PipelineCreationArguments creation_arguments_placeholder, creation_arguments_real;\n"
         "      if (TryGetPipelineCreationArgumentsForDescription(description, &*it, creation_arguments_real) &&\n"
         "          TryGetPipelineCreationArgumentsForDescription(description, &*it,\n"
         "                                                        creation_arguments_placeholder, true) &&\n"
         "          EnsurePipelineCreated(creation_arguments_placeholder, placeholder_pixel_shader_)) {\n"
         "        creation_arguments_real.priority = async_priority;\n"
         "        {\n"
         "          std::lock_guard<std::mutex> lock(creation_request_lock_);\n"
         "          creation_queue_.push(creation_arguments_real);\n"
         "        }\n"
         "        creation_request_cond_.notify_one();\n"
         "        found_pipeline = it->second.pipeline.load(std::memory_order_acquire);\n"
         "      }\n"
         "    }\n"
         "    if (found_pipeline == VK_NULL_HANDLE) {\n"
         "      PipelineCreationArguments creation_arguments;\n")

    # Every submission's end built the whole creation queue on the GPU thread
    # (CreateQueuedPipelinesOnProcessorThread) and then waited for the
    # creation threads' pipelines in flight: the async path's draws (placeholder
    # recorded, the real pipeline read when the command buffer runs) waited for
    # their compiles anyway, and so did the stored ones queued at startup.
    # Every gameplay hitch in both games (run 8: 100-650 ms, all of it in
    # "submit", each after a burst of "Creating graphics pipeline state") was
    # that. With async compilation the queue is left to the creation threads.
    edit(vpc2,
         "    if (startup_loading) {\n"
         "      creation_request_cond_.notify_one();\n"
         "    } else {\n",
         "    if (startup_loading || REXCVAR_GET(async_shader_compilation)) {  // [sr]\n"
         "      creation_request_cond_.notify_one();\n"
         "    } else {\n")

    # The driver's compiled pipelines kept across sessions (a VkPipelineCache
    # in <cache>/shaders, as gta4-ps5's own renderer keeps one): without it
    # every session compiled every pipeline again, and the stutters in
    # gameplay were those compiles (sr_008.log: each drop a burst of
    # "Creating graphics pipeline state"). Saved from the GPU thread's 10 s
    # report when new pipelines were made, written on a thread of its own.
    edit(vpc2,
         "namespace rex::graphics::vulkan {\n",
         "namespace rex::graphics::vulkan {\n\n"
         "// [sr] the driver's pipeline cache, kept across sessions (ci/patch_framework.py)\n"
         "namespace {\n"
         "VkPipelineCache sr_vk_cache = VK_NULL_HANDLE;\n"
         "const ui::vulkan::VulkanDevice* sr_vk_cache_device = nullptr;\n"
         "std::string sr_vk_cache_path;\n"
         "std::atomic<uint32_t> sr_vk_cache_new{0};\n"
         "std::atomic<bool> sr_vk_cache_saving{false};\n"
         "void SrOpenPipelineCache(const ui::vulkan::VulkanDevice* device, const std::filesystem::path& path) {\n"
         "  if (sr_vk_cache != VK_NULL_HANDLE || !device) return;\n"
         "  std::vector<uint8_t> data;\n"
         "  if (std::FILE* f = std::fopen(path.string().c_str(), \"rb\")) {\n"
         "    std::fseek(f, 0, SEEK_END);\n"
         "    const long size = std::ftell(f);\n"
         "    std::fseek(f, 0, SEEK_SET);\n"
         "    if (size > 0 && size < (256l << 20)) {\n"
         "      data.resize(size_t(size));\n"
         "      if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();\n"
         "    }\n"
         "    std::fclose(f);\n"
         "  }\n"
         "  VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};\n"
         "  info.initialDataSize = data.size();\n"
         "  info.pInitialData = data.empty() ? nullptr : data.data();\n"
         "  const auto& dfn = device->functions();\n"
         "  if (dfn.vkCreatePipelineCache(device->device(), &info, nullptr, &sr_vk_cache) != VK_SUCCESS) {\n"
         "    info.initialDataSize = 0;\n"
         "    info.pInitialData = nullptr;\n"
         "    if (dfn.vkCreatePipelineCache(device->device(), &info, nullptr, &sr_vk_cache) != VK_SUCCESS)\n"
         "      sr_vk_cache = VK_NULL_HANDLE;\n"
         "  }\n"
         "  sr_vk_cache_device = device;\n"
         "  sr_vk_cache_path = path.string();\n"
         "  REXGPU_INFO(\"sr: Vulkan pipeline cache {} ({} KB) at {}\", sr_vk_cache ? \"open\" : \"unavailable\",\n"
         "              data.size() >> 10, sr_vk_cache_path);\n"
         "}\n"
         "}  // namespace\n\n"
         "// From the GPU thread's 10 s report: saved when pipelines were made since.\n"
         "void SrSavePipelineCache() {\n"
         "  // (at most once a minute, and all of it on a thread of its own: reading the\n"
         "  // 13-18 MB cache on the GPU thread every 10 s was a hitch in itself)\n"
         "  static uint32_t reports = 0;\n"
         "  if (sr_vk_cache == VK_NULL_HANDLE || !sr_vk_cache_new.load() || ++reports < 6) return;\n"
         "  if (sr_vk_cache_saving.exchange(true)) return;\n"
         "  reports = 0;\n"
         "  const uint32_t made = sr_vk_cache_new.exchange(0);\n"
         "  std::thread([made]() {\n"
         "    const auto& dfn = sr_vk_cache_device->functions();\n"
         "    size_t size = 0;\n"
         "    std::vector<uint8_t> data;\n"
         "    if (dfn.vkGetPipelineCacheData(sr_vk_cache_device->device(), sr_vk_cache, &size, nullptr) == VK_SUCCESS &&\n"
         "        size) {\n"
         "      data.resize(size);\n"
         "      if (dfn.vkGetPipelineCacheData(sr_vk_cache_device->device(), sr_vk_cache, &size, data.data()) !=\n"
         "          VK_SUCCESS)\n"
         "        data.clear();\n"
         "      data.resize(std::min(size, data.size()));\n"
         "    }\n"
         "    bool ok = false;\n"
         "    if (!data.empty()) {\n"
         "      const std::string tmp = sr_vk_cache_path + \".tmp\";\n"
         "      if (std::FILE* f = std::fopen(tmp.c_str(), \"wb\")) {\n"
         "        ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();\n"
         "        ok = std::fclose(f) == 0 && ok;\n"
         "      }\n"
         "      if (ok) ok = std::rename(tmp.c_str(), sr_vk_cache_path.c_str()) == 0;\n"
         "    }\n"
         "    REXGPU_INFO(\"sr: Vulkan pipeline cache saved ({} KB, {} new pipelines){}\", data.size() >> 10, made,\n"
         "                ok ? \"\" : \" - FAILED\");\n"
         "    sr_vk_cache_saving.store(false);\n"
         "  }).detach();\n"
         "}\n")
    edit(vpc2, "#include <algorithm>\n", "#include <algorithm>\n#include <cstdio>\n#include <filesystem>\n#include <thread>\n")
    edit(vpc2,
         "  auto shader_storage_root = cache_root / \"shaders\";\n"
         "  auto shader_storage_shareable_root = shader_storage_root / \"shareable\";\n",
         "  auto shader_storage_root = cache_root / \"shaders\";\n"
         "  auto shader_storage_shareable_root = shader_storage_root / \"shareable\";\n"
         "  {\n"
         "    std::error_code sr_ec;\n"
         "    std::filesystem::create_directories(shader_storage_root, sr_ec);\n"
         "    SrOpenPipelineCache(command_processor_.GetVulkanDevice(),\n"
         "                         shader_storage_root / fmt::format(\"{:08X}.vk-pipelines.bin\", title_id));\n"
         "  }\n")
    edit(vpc2,
         "  VkResult create_result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,\n"
         "                                                         &pipeline_create_info, nullptr, &pipeline);\n",
         "  VkResult create_result = dfn.vkCreateGraphicsPipelines(device, sr_vk_cache, 1,\n"
         "                                                         &pipeline_create_info, nullptr, &pipeline);\n"
         "  if (create_result == VK_SUCCESS && sr_vk_cache != VK_NULL_HANDLE) sr_vk_cache_new.fetch_add(1);\n")
    edit(vcp,
         "      last_us = now;\n      frames = 0;\n    }\n  }\n",
         "      last_us = now;\n      frames = 0;\n      SrSavePipelineCache();  // (pipeline_cache.cpp)\n    }\n  }\n")
    edit(vcp,
         "namespace rex::graphics::vulkan {\n\nnamespace {\n",
         "namespace rex::graphics::vulkan {\n\nvoid SrSavePipelineCache();  // [sr] pipeline_cache.cpp\n\nnamespace {\n")

    # The pipeline description storage (<title>.fsi.vk.xpso) held 229 pipelines
    # in every session since the first ones, though each session made new ones
    # (sr_001.log, 20:22): so every session translated and built those again
    # mid-game. How many were read, kept and written, for the log; and the
    # files seeked to their end before the writer appends.
    edit(vpc2,
         "      pipeline_stored_descriptions.resize(pipeline_storage_kept_count);\n",
         "      pipeline_stored_descriptions.resize(pipeline_storage_kept_count);\n"
         "      REXGPU_INFO(\"sr: pipeline storage: {} read, {} kept, {} unsupported, {} corrupted\",\n"
         "                  pipeline_storage_read_count, pipeline_storage_kept_count,\n"
         "                  pipeline_storage_skipped_unsupported_count, pipeline_storage_skipped_corrupted_count);\n")
    edit(vpc2,
         "    rex::filesystem::TruncateStdioFile(shader_storage_file_, shader_storage_valid_bytes);\n",
         "    rex::filesystem::TruncateStdioFile(shader_storage_file_, shader_storage_valid_bytes);\n"
         "    rex::filesystem::Seek(shader_storage_file_, 0, SEEK_END);  // [sr] (for the writer)\n"
         "    REXGPU_INFO(\"sr: shader storage: {} KB of shaders\", shader_storage_valid_bytes >> 10);\n")
    edit(vpc2,
         "  shader_storage_cache_root_ = cache_root;\n  shader_storage_title_id_ = title_id;\n",
         "  rex::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);  // [sr] (for the writer)\n"
         "  shader_storage_cache_root_ = cache_root;\n  shader_storage_title_id_ = title_id;\n")
    edit(vpc2,
         "    if (write_pipeline) {\n"
         "      assert_not_null(pipeline_storage_file_);\n"
         "      fwrite(&pipeline_description, sizeof(pipeline_description), 1, pipeline_storage_file_);\n"
         "    }\n",
         "    if (write_pipeline) {\n"
         "      assert_not_null(pipeline_storage_file_);\n"
         "      const size_t sr_written =\n"
         "          fwrite(&pipeline_description, sizeof(pipeline_description), 1, pipeline_storage_file_);\n"
         "      static uint32_t sr_writes = 0, sr_failed = 0;\n"
         "      sr_failed += sr_written != 1;\n"
         "      if ((++sr_writes % 50) == 1 || sr_written != 1)\n"
         "        REXGPU_INFO(\"sr: pipeline storage: {} written this session ({} failed)\", sr_writes, sr_failed);\n"
         "    }\n")

    # The storage files stay open all session, and the PS5 ends the game
    # without closing them: what they gained never reached the drive
    # (sr_002.log: 101 pipelines written, 229 read at the next start, as in
    # every session since the first). Each flush now goes to the drive.
    edit(vpc2, "#include <algorithm>\n#include <cstdio>\n",
         "#include <algorithm>\n#include <cstdio>\n#include <unistd.h>\n")
    edit(vpc2,
         "      fflush(shader_storage_file_);\n",
         "      fflush(shader_storage_file_);\n      fsync(fileno(shader_storage_file_));  // [sr]\n")
    edit(vpc2,
         "      fflush(pipeline_storage_file_);\n",
         "      fflush(pipeline_storage_file_);\n      fsync(fileno(pipeline_storage_file_));  // [sr]\n")

    # The next frame may be in the next buffer (Xenia's data-aware search).
    xt = xc.read_text()
    a = xt.index("void XmaContext::Decode(XMA_CONTEXT_DATA* data) {\n")
    b = xt.index("void XmaContext::ConvertFrame(")
    body = xt[a:b]
    if "sr_next_read_offset" in body:
        print(f"already  {xc} (Decode)")
        return
    old_buf = "  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);\n"
    new_buf = (old_buf +
        "  // [sr] Read offset of the next frame from packet `index` on, which\n"
        "  // past the end of this buffer is in the next one (as Xenia).\n"
        "  const auto sr_next_read_offset = [&](uint32_t index, uint32_t count) -> uint32_t {\n"
        "    uint8_t buffer = data->current_buffer;\n"
        "    if (index >= count) {\n"
        "      buffer ^= 1;\n      index -= count;\n"
        "    }\n"
        "    if (!data->IsInputBufferValid(buffer) || !data->GetInputBufferAddress(buffer)) {\n"
        "      return kBitsPerPacketHeader;\n    }\n"
        "    return GetNextPacketReadOffset(\n"
        "        memory()->TranslatePhysical(data->GetInputBufferAddress(buffer)), index,\n"
        "        data->GetInputBufferPacketCount(buffer));\n"
        "  };\n")
    old_ff = ("    uint32_t next_input_offset =\n"
              "        GetNextPacketReadOffset(current_input_buffer, packet_index + 1, current_input_packet_count);\n"
              "    if (next_input_offset == kBitsPerPacketHeader) {\n"
              "      SwapInputBuffer(data);\n    }\n")
    new_ff = ("    uint32_t next_input_offset =\n"
              "        sr_next_read_offset(packet_index + 1, current_input_packet_count);\n"
              "    if (uint32_t(packet_index + 1) >= current_input_packet_count ||\n"
              "        next_input_offset == kBitsPerPacketHeader) {\n"
              "      SwapInputBuffer(data);\n    }\n")
    old_last = ("  uint32_t next_input_offset =\n"
                "      GetNextPacketReadOffset(current_input_buffer, next_packet_index, current_input_packet_count);\n\n"
                "  if (next_input_offset == kBitsPerPacketHeader) {\n"
                "    SwapInputBuffer(data);\n")
    new_last = ("  uint32_t next_input_offset =\n"
                "      sr_next_read_offset(next_packet_index, current_input_packet_count);\n\n"
                "  if (next_packet_index >= current_input_packet_count ||\n"
                "      next_input_offset == kBitsPerPacketHeader) {\n"
                "    SwapInputBuffer(data);\n  }\n"
                "  if (next_input_offset == kBitsPerPacketHeader) {\n")
    for old, new in ((old_buf, new_buf), (old_ff, new_ff), (old_last, new_last)):
        if body.count(old) != 1:
            sys.exit(f"patch_framework: expected 1 of {old!r} in Decode, found {body.count(old)}")
        body = body.replace(old, new)
    n = [0]
    def site(m):
        n[0] += 1
        return f"data->error_status = 4, ++sr_xma_err[{n[0] - 1}];"
    body = re.sub(r"data->error_status = 4;", site, body)
    if not 8 <= n[0] <= 16:
        sys.exit(f"patch_framework: {n[0]} error sites in Decode")
    xc.write_text(xt[:a] + body + xt[b:])
    print(f"patched  {xc} (Decode: next-buffer search, {n[0]} error sites counted)")

    # Texture layouts as Xenia Canary has them now (2026: bc6b7a0 "Fix array and volume mip layout",
    # 81e3dea / ace153c / 344fca7 packed mip tails). Saints Row's buildings turned to stripes in
    # the distance (their mips read from the wrong place): a 2D array's levels past the base hold
    # a multiple of 4 slices, as D3D's FindTextureSize lays them out; a volume's levels have their
    # own depth; a packed level 0 uses power-of-two strides and the base address for its whole
    # tail; a 2x1 texture packs like a square one.
    tu = sdk / "src" / "graphics" / "pipeline" / "texture" / "util.cpp"
    edit(tu,
         "  uint32_t base_page = fetch.base_address & 0x1FFFF;\n"
         "  uint32_t mip_page = fetch.mip_address & 0x1FFFF;\n\n"
         "  uint32_t mip_min_level, mip_max_level;\n",
         "  uint32_t base_page = fetch.base_address & 0x1FFFF;\n"
         "  uint32_t mip_page = fetch.mip_address & 0x1FFFF;\n\n"
         "  // [sr] If level 0 is packed, the whole tail uses base_address, even if mip_address is\n"
         "  // nonzero (Xenia 344fca7).\n"
         "  if (base_page != 0 && fetch.packed_mips && fetch.dimension != xenos::DataDimension::k1D &&\n"
         "      GetPackedMipLevel(width_minus_1 + 1, height_minus_1 + 1) == 0) {\n"
         "    mip_page = base_page;\n"
         "  }\n\n"
         "  uint32_t mip_min_level, mip_max_level;\n")
    edit(tu,
         "      if (log2_depth > 1 + packed_mip) {\n"
         "        z_blocks = (log2_depth - packed_mip) * 4;\n",
         "      if (log2_depth > 1 + mip) {  // [sr] (Xenia: the mip, not the packed mip)\n"
         "        z_blocks = (log2_depth - mip) * 4;\n")
    edit(tu, "    if (log2_width > log2_height) {\n", "    if (is_wide) {  // [sr]\n", count=2)
    edit(tu,
         "  if (packed_mip < 3) {\n",
         "  // [sr] A 2x1 texture is packed along X, like a square texture (Xenia 344fca7).\n"
         "  bool is_wide = log2_width > std::max(log2_height, uint32_t(1));\n"
         "  if (packed_mip < 3) {\n")
    edit(tu,
         "  layout.packed_level =\n"
         "      has_packed_levels ? GetPackedMipLevel(width_texels, height_texels) : UINT32_MAX;\n",
         "  // [sr] D3D's FindTextureSize aligns non-base 2D array levels to four slices (Xenia bc6b7a0).\n"
         "  uint32_t mip_array_size =\n"
         "      dimension == xenos::DataDimension::k2DOrStacked && layout.array_size > 1\n"
         "          ? rex::align(layout.array_size, xenos::kTextureTileDepth)\n"
         "          : layout.array_size;\n\n"
         "  layout.packed_level =\n"
         "      has_packed_levels ? GetPackedMipLevel(width_texels, height_texels) : UINT32_MAX;\n")
    edit(tu,
         "      row_pitch_texels_unaligned = base_pitch_texels_div_32 << 5;\n"
         "      z_slice_stride_texel_rows_unaligned = height_texels;\n",
         "      row_pitch_texels_unaligned = base_pitch_texels_div_32 << 5;\n"
         "      // [sr] Level 0 packed tails use power-of-two dimensions like the other mips (Xenia 81e3dea).\n"
         "      z_slice_stride_texel_rows_unaligned =\n"
         "          layout.packed_level == 0 ? rex::next_pow2(height_texels) : height_texels;\n")
    edit(tu,
         "      level_layout.array_slice_stride_bytes *=\n"
         "          rex::align(depth_or_array_size, xenos::kTextureTileDepth);\n",
         "      level_layout.array_slice_stride_bytes *= rex::align(  // [sr] each level's own depth (Xenia bc6b7a0)\n"
         "          is_base && layout.packed_level != 0 ? depth\n"
         "                                              : std::max(rex::next_pow2(depth) >> level, uint32_t(1)),\n"
         "          xenos::kTextureTileDepth);\n")
    edit(tu,
         "      mip_offset_bytes += level_layout.array_slice_stride_bytes * layout.array_size;\n",
         "      mip_offset_bytes += level_layout.array_slice_stride_bytes * mip_array_size;  // [sr]\n")

    # Off the PS5 (the desktop build used to look at frames without the console): SR_DUMP_DIR
    # set, the frontbuffer's guest memory (with readback_resolve on, the shown frame a frame late)
    # is written there every 2 s, tiled as the GPU keeps it, for tools/sr_frame.py.
    edit(sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp",
         "      &frontbuffer_width_unscaled, &frontbuffer_height_unscaled, &swap_source_needs_rb_swap);\n",
         "      &frontbuffer_width_unscaled, &frontbuffer_height_unscaled, &swap_source_needs_rb_swap);\n"
         "#if !REX_PLATFORM_PS5\n"
         "  {  // [sr]\n"
         "    static const char* dump_dir = std::getenv(\"SR_DUMP_DIR\");\n"
         "    static auto last_dump = std::chrono::steady_clock::time_point();\n"
         "    const auto now = std::chrono::steady_clock::now();\n"
         "    if (dump_dir && now - last_dump >= std::chrono::seconds(2)) {\n"
         "      last_dump = now;\n"
         "      const uint32_t pitch = (frontbuffer_width + 31) & ~31u, rows = (frontbuffer_height + 31) & ~31u;\n"
         "      const auto* data = memory_->TranslatePhysical(frontbuffer_ptr);\n"
         "      const std::string base = std::string(dump_dir) + \"/frame_\" + std::to_string(frame_flow_swap_count_);\n"
         "      if (std::FILE* f = std::fopen((base + \".raw\").c_str(), \"wb\")) {\n"
         "        std::fwrite(data, 1, size_t(pitch) * rows * 4, f);\n"
         "        std::fclose(f);\n"
         "      }\n"
         "      if (std::FILE* f = std::fopen((base + \".txt\").c_str(), \"wb\")) {\n"
         "        std::fprintf(f, \"%u %u %u %u\\n\", frontbuffer_width, frontbuffer_height, pitch, uint32_t(frontbuffer_format));\n"
         "        std::fclose(f);\n"
         "      }\n"
         "    }\n"
         "  }\n"
         "#endif\n")

    # (and so that frames are in guest memory to dump: every resolve read back then, not only
    # the small ones)
    edit(sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp",
         "  if (written_length > (256u << 10) && !sr_cap.file) {  // [sr] only what the CPU reads\n",
         "#if !REX_PLATFORM_PS5\n"
         "  static const bool sr_dump_all = std::getenv(\"SR_DUMP_DIR\") != nullptr;  // [sr]\n"
         "#else\n"
         "  constexpr bool sr_dump_all = false;\n"
         "#endif\n"
         "  if (written_length > (256u << 10) && !sr_cap.file && !sr_dump_all) {  // [sr] only what the CPU reads\n")

    # The frame summary ([FrameFlow]) once a second too, not only at powers of two: what a
    # cutscene's frames draw, copy and show.
    edit(sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp",
         "      frame_flow_swap_count_ == 512 || frame_flow_swap_count_ == 1024;",
         "      frame_flow_swap_count_ == 512 || frame_flow_swap_count_ == 1024 ||\n"
         "      frame_flow_swap_count_ % 60 == 0;  // [sr]")

    # Xenia Canary 9781a75 "Avoid stale texture state after invalidation" (2026-06-29): a texture
    # is marked up to date and watched only if its shared memory range is still valid; one
    # invalidated meanwhile (the CPU rewrote it as it loaded: a streamed mip, a movie frame) stays
    # outdated and is loaded again, instead of being kept stale with a watch that won't fire.
    smh = sdk / "include" / "rex" / "graphics" / "shared_memory.h"
    edit(smh, "  bool RequestRange(uint32_t start, uint32_t length);\n",
         "  bool RequestRange(uint32_t start, uint32_t length);\n"
         "  // [sr] Whether every page of the range is valid in the GPU's copy (Xenia 9781a75).\n"
         "  bool IsRangeValid(uint32_t start, uint32_t length) const;\n")
    smc = sdk / "src" / "graphics" / "shared_memory.cpp"
    edit(smc, "bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {\n",
         "bool SharedMemory::IsRangeValid(uint32_t start, uint32_t length) const {  // [sr]\n"
         "  if (!length) return true;\n"
         "  if (start > kBufferSize || (kBufferSize - start) < length) return false;\n"
         "  const uint64_t* valid = active_valid_flags_.load(std::memory_order_acquire);\n"
         "  if (!valid) return false;\n"
         "  const uint32_t page_first = start >> page_size_log2_;\n"
         "  const uint32_t page_last = (start + length - 1) >> page_size_log2_;\n"
         "  for (uint32_t i = page_first >> 6; i <= (page_last >> 6); ++i) {\n"
         "    uint64_t mask = UINT64_MAX;\n"
         "    if (i == (page_first >> 6)) mask &= ~((uint64_t(1) << (page_first & 63)) - 1);\n"
         "    if (i == (page_last >> 6) && (page_last & 63) != 63) mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;\n"
         "    if ((valid[i] & mask) != mask) return false;\n"
         "  }\n"
         "  return true;\n"
         "}\n\n"
         "bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {\n")
    edit(sdk / "include" / "rex" / "graphics" / "pipeline" / "texture" / "cache.h",
         "    void MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);\n",
         "    bool MakeUpToDateAndWatch(const rex::thread::global_lock_t& global_lock);  // [sr]\n")
    tcc0 = sdk / "src" / "graphics" / "pipeline" / "texture" / "cache.cpp"
    edit(tcc0,
         "void TextureCache::Texture::MakeUpToDateAndWatch(\n"
         "    const rex::thread::global_lock_t& global_lock) {\n"
         "  SharedMemory& shared_memory = texture_cache().shared_memory();\n"
         "  if (base_outdated_) {\n",
         "bool TextureCache::Texture::MakeUpToDateAndWatch(  // [sr] (Xenia 9781a75)\n"
         "    const rex::thread::global_lock_t& global_lock) {\n"
         "  SharedMemory& shared_memory = texture_cache().shared_memory();\n"
         "  if (base_outdated_ &&\n"
         "      !shared_memory.IsRangeValid(key().base_page << 12, rex::align(GetGuestBaseSize(), UINT32_C(16)))) {\n"
         "    return false;\n"
         "  }\n"
         "  if (mips_outdated_ &&\n"
         "      !shared_memory.IsRangeValid(key().mip_page << 12, rex::align(GetGuestMipsSize(), UINT32_C(16)))) {\n"
         "    return false;\n"
         "  }\n"
         "  if (base_outdated_) {\n")
    edit(tcc0,
         "    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);\n"
         "  }\n"
         "}\n",
         "    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);\n"
         "  }\n"
         "  return true;\n"
         "}\n")
    edit(tcc0,
         "  texture.MakeUpToDateAndWatch(global_critical_region_.Acquire());\n"
         "  texture.LogAction(\"Loaded\");\n",
         "  if (!texture.MakeUpToDateAndWatch(global_critical_region_.Acquire())) {  // [sr]\n"
         "    return false;\n"
         "  }\n"
         "  texture.LogAction(\"Loaded\");\n")

    # Xenia Canary d119505 "Texture integer scaling fetches" (2026-07-06, the build Xenia's
    # Saints Row 2 report was confirmed fixed on: "video is blacked out after intro"): a fixed-point
    # texture fetched with num_format integer comes back from the host sampler normalized (0..1)
    # where the guest expects its integer values (0..255): the shader is told each component's
    # scale through a system constant and multiplies it back. (k_16, k_16_16, k_16_16_16_16 keep
    # the exp_adjust + 16 above instead.)
    sth = sdk / "include" / "rex" / "graphics" / "pipeline" / "shader" / "spirv_translator.h"
    edit(sth, "    float edram_blend_constant[4];\n",
         "    float edram_blend_constant[4];\n\n"
         "    // [sr] Integer num_format on fixed textures: per fetch constant, 5 bits a component\n"
         "    // (bits 0:3 = component bits - 1, bit 4 = signed), 0 = no scale.\n"
         "    uint32_t texture_integer_scale_bits[32];\n")
    edit(sth, "    kSystemConstantEdramBlendConstant,\n",
         "    kSystemConstantEdramBlendConstant,\n    kSystemConstantTextureIntegerScaleBits,  // [sr]\n")
    edit(sth, "    static constexpr uint32_t kVersion = 13;\n", "    static constexpr uint32_t kVersion = 14;  // [sr] (integer scaling)\n")
    stc = sdk / "src" / "graphics" / "pipeline" / "shader" / "spirv_translator.cpp"
    edit(stc,
         "  builder_->addDecoration(type_uint4_array_4, spv::DecorationArrayStride, sizeof(uint32_t) * 4);\n",
         "  builder_->addDecoration(type_uint4_array_4, spv::DecorationArrayStride, sizeof(uint32_t) * 4);\n"
         "  spv::Id type_uint4_array_8 =  // [sr]\n"
         "      builder_->makeArrayType(type_uint4_, builder_->makeUintConstant(8), sizeof(uint32_t) * 4);\n"
         "  builder_->addDecoration(type_uint4_array_8, spv::DecorationArrayStride, sizeof(uint32_t) * 4);\n")
    edit(stc,
         "      {\"edram_blend_constant\", offsetof(SystemConstants, edram_blend_constant), type_float4_},\n",
         "      {\"edram_blend_constant\", offsetof(SystemConstants, edram_blend_constant), type_float4_},\n"
         "      {\"texture_integer_scale_bits\", offsetof(SystemConstants, texture_integer_scale_bits),\n"
         "       type_uint4_array_8},  // [sr]\n")
    stf = sdk / "src" / "graphics" / "pipeline" / "shader" / "spirv_translator_fetch.cpp"
    edit(stf,
         "        // Apply the exponent bias from the bits 13:18 of the fetch constant\n        // word 4.\n",
         "        // [sr] Integer num_format on a fixed texture: the normalized host sample scaled back\n"
         "        // to the guest's integer units (Xenia d119505), after the signs and gamma.\n"
         "        {\n"
         "          id_vector_temp_.clear();\n"
         "          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantTextureIntegerScaleBits));\n"
         "          id_vector_temp_.push_back(builder_->makeIntConstant(int32_t(fetch_constant_index >> 2)));\n"
         "          id_vector_temp_.push_back(builder_->makeIntConstant(int32_t(fetch_constant_index & 3)));\n"
         "          spv::Id scale_packed = builder_->createLoad(\n"
         "              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_, id_vector_temp_),\n"
         "              spv::NoPrecision);\n"
         "          spv::Id const_uint_1 = builder_->makeUintConstant(1);\n"
         "          uint32_t scale_remaining = used_result_nonzero_components;\n"
         "          uint32_t scale_component;\n"
         "          while (rex::bit_scan_forward(scale_remaining, &scale_component)) {\n"
         "            scale_remaining &= ~(UINT32_C(1) << scale_component);\n"
         "            spv::Id bits = builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, scale_packed,\n"
         "                                                 builder_->makeUintConstant(scale_component * 5),\n"
         "                                                 builder_->makeUintConstant(5));\n"
         "            spv::Id shift = builder_->createBinOp(\n"
         "                spv::OpISub, type_uint_,\n"
         "                builder_->createBinOp(spv::OpIAdd, type_uint_,\n"
         "                                      builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, bits,\n"
         "                                                            builder_->makeUintConstant(0xF)),\n"
         "                                      const_uint_1),\n"
         "                builder_->createTriOp(spv::OpBitFieldUExtract, type_uint_, bits, builder_->makeUintConstant(4),\n"
         "                                      const_uint_1));\n"
         "            spv::Id scale = builder_->createUnaryOp(\n"
         "                spv::OpConvertUToF, type_float_,\n"
         "                builder_->createBinOp(spv::OpISub, type_uint_,\n"
         "                                      builder_->createBinOp(spv::OpShiftLeftLogical, type_uint_, const_uint_1, shift),\n"
         "                                      const_uint_1));\n"
         "            spv::Id active = builder_->createBinOp(spv::OpINotEqual, type_bool_, bits, builder_->makeUintConstant(0));\n"
         "            spv::Id factor = builder_->createTriOp(spv::OpSelect, type_float_, active, scale, const_float_1_);\n"
         "            result[scale_component] = builder_->createNoContractionBinOp(spv::OpFMul, type_float_,\n"
         "                                                                        result[scale_component], factor);\n"
         "          }\n"
         "        }\n\n"
         "        // Apply the exponent bias from the bits 13:18 of the fetch constant\n        // word 4.\n")
    tch = sdk / "include" / "rex" / "graphics" / "pipeline" / "texture" / "cache.h"
    edit(tch,
         "  uint8_t GetActiveTextureSwizzledSigns(uint32_t fetch_constant_index) const {\n",
         "  uint32_t GetActiveIntegerScaleBits(uint32_t fetch_constant_index) const {  // [sr]\n"
         "    const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);\n"
         "    return binding ? binding->integer_scale_bits : 0;\n"
         "  }\n"
         "  uint8_t GetActiveTextureSwizzledSigns(uint32_t fetch_constant_index) const {\n")
    edit(tch, "    uint32_t host_swizzle;\n    // ", "    uint32_t integer_scale_bits;  // [sr] packed, 5 bits a component\n    uint32_t host_swizzle;\n    // ")
    edit(tch, "  bool LoadTextureData(Texture& texture);\n",
         "  static uint32_t GetIntegerScaleBits(xenos::TextureFormat guest_format, uint32_t num_format,\n"
         "                                      uint32_t host_swizzle, uint8_t swizzled_signs);  // [sr]\n"
         "  bool LoadTextureData(Texture& texture);\n")
    tcc = sdk / "src" / "graphics" / "pipeline" / "texture" / "cache.cpp"
    edit(tcc,
         "    uint8_t old_swizzled_signs = binding.swizzled_signs;\n"
         "    BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);\n",
         "    uint8_t old_swizzled_signs = binding.swizzled_signs;\n"
         "    uint32_t old_integer_scale_bits = binding.integer_scale_bits;  // [sr]\n"
         "    BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);\n")
    edit(tcc,
         "    binding.host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));\n",
         "    binding.host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));\n"
         "    binding.integer_scale_bits = GetIntegerScaleBits(fetch.format, fetch.num_format, binding.host_swizzle,\n"
         "                                                     binding.swizzled_signs);  // [sr]\n")
    edit(tcc,
         "    if (key_changed || binding.host_swizzle != old_host_swizzle ||\n",
         "    if (key_changed || binding.integer_scale_bits != old_integer_scale_bits ||  // [sr]\n"
         "        binding.host_swizzle != old_host_swizzle ||\n")
    edit(tcc,
         "void TextureCache::RequestTextures(uint32_t used_texture_mask) {\n",
         "// [sr] Xenia d119505's GetIntegerScaleBits, with its table's component widths of the fixed\n"
         "// formats (k_16, k_16_16, k_16_16_16_16 left to the exp_adjust + 16 patch).\n"
         "uint32_t TextureCache::GetIntegerScaleBits(xenos::TextureFormat guest_format, uint32_t num_format,\n"
         "                                           uint32_t host_swizzle, uint8_t swizzled_signs) {\n"
         "  using F = xenos::TextureFormat;\n"
         "  uint8_t bits[4] = {};\n"
         "  auto set = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) { bits[0] = a, bits[1] = b, bits[2] = c, bits[3] = d; };\n"
         "  switch (guest_format) {\n"
         "    case F::k_8: case F::k_8_A: case F::k_8_B: case F::k_DXT3A: case F::k_DXT5A: set(8, 0, 0, 0); break;\n"
         "    case F::k_1_5_5_5: set(5, 5, 5, 1); break;\n"
         "    case F::k_5_6_5: set(5, 6, 5, 0); break;\n"
         "    case F::k_6_5_5: set(5, 5, 6, 0); break;\n"
         "    case F::k_8_8_8_8: case F::k_8_8_8_8_A: case F::k_8_8_8_8_AS_16_16_16_16:\n"
         "    case F::k_8_8_8_8_GAMMA_EDRAM: set(8, 8, 8, 8); break;\n"
         "    case F::k_2_10_10_10: case F::k_2_10_10_10_AS_16_16_16_16: set(10, 10, 10, 2); break;\n"
         "    case F::k_8_8: case F::k_DXN: case F::k_CTX1: set(8, 8, 0, 0); break;\n"
         "    case F::k_16_16_EDRAM: set(16, 16, 0, 0); break;\n"
         "    case F::k_4_4_4_4: set(4, 4, 4, 4); break;\n"
         "    case F::k_10_11_11: case F::k_10_11_11_AS_16_16_16_16: set(11, 11, 10, 0); break;\n"
         "    case F::k_11_11_10: case F::k_11_11_10_AS_16_16_16_16: set(10, 11, 11, 0); break;\n"
         "    case F::k_16_16_16_16_EDRAM: set(16, 16, 16, 16); break;\n"
         "    default: return 0;\n"
         "  }\n"
         "  if (!num_format) return 0;\n"
         "  uint32_t scale_bits = 0;\n"
         "  for (uint32_t i = 0; i < 4; ++i) {\n"
         "    uint32_t source_component = (host_swizzle >> (i * 3)) & 0b111;\n"
         "    if (source_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) continue;\n"
         "    xenos::TextureSign sign = xenos::TextureSign((swizzled_signs >> (i * 2)) & 0b11);\n"
         "    uint8_t width = bits[source_component];\n"
         "    if (!width || width > 16 || sign == xenos::TextureSign::kGamma) continue;\n"
         "    uint32_t component_scale = uint32_t(width - 1);\n"
         "    if (sign == xenos::TextureSign::kSigned) component_scale |= UINT32_C(1) << 4;\n"
         "    scale_bits |= component_scale << (i * 5);\n"
         "  }\n"
         "  return scale_bits;\n"
         "}\n\n"
         "void TextureCache::RequestTextures(uint32_t used_texture_mask) {\n")
    vcp2 = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    edit(vcp2,
         "      texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;\n",
         "      texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;\n"
         "      {  // [sr] integer num_format scales (Xenia d119505)\n"
         "        const uint32_t scale_bits = texture_cache_->GetActiveIntegerScaleBits(texture_index);\n"
         "        dirty |= system_constants_.texture_integer_scale_bits[texture_index] != scale_bits;\n"
         "        system_constants_.texture_integer_scale_bits[texture_index] = scale_bits;\n"
         "      }\n")

    # The command processor's interrupts (PM4_INTERRUPT, source 1) ran the game's interrupt
    # callback on the GPU thread itself, under the global lock: Saints Row 2's callback does real
    # work (its D3D callbacks) and fights the game's threads for the lock, and the GPU thread was
    # in it about half of every frame. On the Xbox 360 the interrupt runs on a CPU while the GPU
    # goes on: here on a thread of its own, "GPU Interrupts", in order. (Vblank interrupts already
    # come from the GPU VSync thread.)
    gsh = sdk / "include" / "rex" / "graphics" / "graphics_system.h"
    edit(gsh,
         "  std::atomic<bool> vsync_worker_running_;\n"
         "  system::object_ref<system::XHostThread> vsync_worker_thread_;\n",
         "  std::atomic<bool> vsync_worker_running_;\n"
         "  system::object_ref<system::XHostThread> vsync_worker_thread_;\n\n"
         "  // [sr] the command processor's interrupts, run on a thread of their own\n"
         "  void RunInterruptCallback(uint32_t source, uint32_t cpu);\n"
         "  std::mutex sr_interrupt_mutex_;\n"
         "  std::condition_variable sr_interrupt_cv_;\n"
         "  std::deque<std::pair<uint32_t, uint32_t>> sr_interrupts_;\n"
         "  std::atomic<bool> sr_interrupts_running_{false};\n"
         "  system::object_ref<system::XHostThread> sr_interrupt_thread_;\n")
    edit(gsh, "#pragma once\n", "#pragma once\n\n#include <condition_variable>  // [sr]\n#include <deque>\n#include <mutex>\n#include <utility>\n")
    gsc = sdk / "src" / "graphics" / "graphics_system.cpp"
    edit(gsc,
         "  vsync_worker_thread_->set_name(\"GPU VSync\");\n"
         "  vsync_worker_thread_->Create();\n",
         "  vsync_worker_thread_->set_name(\"GPU VSync\");\n"
         "  vsync_worker_thread_->Create();\n\n"
         "  // [sr] the command processor's interrupts, in order, off the GPU thread\n"
         "  sr_interrupts_running_ = true;\n"
         "  sr_interrupt_thread_ = system::object_ref<system::XHostThread>(\n"
         "      new system::XHostThread(kernel_state_, 256 * 1024, 0, [this]() {\n"
         "        while (sr_interrupts_running_) {\n"
         "          std::pair<uint32_t, uint32_t> next;\n"
         "          {\n"
         "            std::unique_lock<std::mutex> lock(sr_interrupt_mutex_);\n"
         "            sr_interrupt_cv_.wait_for(lock, std::chrono::milliseconds(5), [this] {\n"
         "              return !sr_interrupts_.empty() || !sr_interrupts_running_;\n"
         "            });\n"
         "            if (sr_interrupts_.empty()) continue;\n"
         "            next = sr_interrupts_.front();\n"
         "            sr_interrupts_.pop_front();\n"
         "          }\n"
         "          RunInterruptCallback(next.first, next.second);\n"
         "        }\n"
         "        return 0;\n"
         "      }));\n"
         "  sr_interrupt_thread_->set_name(\"GPU Interrupts\");\n"
         "  sr_interrupt_thread_->Create();\n")
    edit(gsc,
         "  if (vsync_worker_thread_) {\n"
         "    vsync_worker_running_ = false;\n",
         "  if (sr_interrupt_thread_) {  // [sr]\n"
         "    sr_interrupts_running_ = false;\n"
         "    sr_interrupt_cv_.notify_all();\n"
         "    sr_interrupt_thread_->Wait(0, 0, 0, nullptr);\n"
         "    sr_interrupt_thread_.reset();\n"
         "  }\n\n"
         "  if (vsync_worker_thread_) {\n"
         "    vsync_worker_running_ = false;\n")
    # The menu's FRAME RATE 60: both games wait two of the guest's 60 Hz vblanks a frame (their 30
    # fps, as on the Xbox 360); with the vblanks twice as fast that's 60 (their game time runs on
    # the clock, as on PC, where they run at any frame rate).
    edit(gsc,
         "REXCVAR_DEFINE_BOOL(vsync_display_source, true, \"GPU\",",
         "REXCVAR_DEFINE_INT32(guest_vblank_scale, 1, \"GPU\",  // [sr]\n"
         "                     \"The guest's vblanks this many times faster than the video mode's refresh (2: a title \"\n"
         "                     \"that waits two vblanks a frame runs at 60)\");\n"
         "REXCVAR_DEFINE_BOOL(vsync_display_source, true, \"GPU\",")
    edit(gsc,
         "          uint64_t interval_ticks =\n"
         "              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;\n",
         "          uint64_t interval_ticks =\n"
         "              REXCVAR_GET(vsync)\n"
         "                  ? std::max<uint64_t>(1, vsync_interval_ticks /\n"
         "                                              uint64_t(std::clamp(REXCVAR_GET(guest_vblank_scale), 1, 4)))\n"
         "                  : no_vsync_interval_ticks;  // [sr] guest_vblank_scale\n")
    edit(gsc,
         "void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {\n"
         "  if (!interrupt_callback_) {\n"
         "    return;\n"
         "  }\n",
         "void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {\n"
         "  if (!interrupt_callback_) {\n"
         "    return;\n"
         "  }\n"
         "  // [sr] the command processor's: queued for the GPU Interrupts thread\n"
         "  if (source == 1 && sr_interrupts_running_) {\n"
         "    {\n"
         "      std::lock_guard<std::mutex> lock(sr_interrupt_mutex_);\n"
         "      sr_interrupts_.emplace_back(source, cpu);\n"
         "    }\n"
         "    sr_interrupt_cv_.notify_one();\n"
         "    return;\n"
         "  }\n"
         "  RunInterruptCallback(source, cpu);\n"
         "}\n\n"
         "void GraphicsSystem::RunInterruptCallback(uint32_t source, uint32_t cpu) {  // [sr]\n"
         "  if (!interrupt_callback_) {\n"
         "    return;\n"
         "  }\n")

    # The ring's read pointer was written back for the game only once a whole primary batch was
    # done. The Xbox 360's CP writes it back as it fetches. Saints Row 2's D3D waits for that
    # progress (ring space) before writing what the GPU's WAIT_REG_MEM in the same batch waits
    # for: each side waited on the other until a timeout, and gameplay ran at 8 fps with the GPU
    # thread asleep in WAIT_REG_MEM. Now a WAIT_REG_MEM that has to wait writes back where the
    # primary ring has been read to first.
    cph = sdk / "include" / "rex" / "graphics" / "command_processor.h"
    edit(cph, "  uint32_t read_ptr_writeback_ptr_ = 0;\n",
         "  uint32_t read_ptr_writeback_ptr_ = 0;\n"
         "  memory::RingBuffer* sr_primary_reader_ = nullptr;  // [sr] (while a primary batch runs)\n")
    cpc = sdk / "src" / "graphics" / "command_processor.cpp"
    edit(cpc,
         "  reader.set_read_offset(read_index * sizeof(uint32_t));\n"
         "  reader.set_write_offset(write_index * sizeof(uint32_t));\n"
         "  do {\n"
         "    if (!ExecutePacket(&reader)) {\n"
         "      // This probably should be fatal - but we're going to continue anyways.\n",
         "  reader.set_read_offset(read_index * sizeof(uint32_t));\n"
         "  reader.set_write_offset(write_index * sizeof(uint32_t));\n"
         "  sr_primary_reader_ = &reader;  // [sr]\n"
         "  do {\n"
         "    if (!ExecutePacket(&reader)) {\n"
         "      // This probably should be fatal - but we're going to continue anyways.\n")
    edit(cpc,
         "  } while (reader.read_count());\n\n  OnPrimaryBufferEnd();\n",
         "  } while (reader.read_count());\n  sr_primary_reader_ = nullptr;  // [sr]\n\n  OnPrimaryBufferEnd();\n")
    edit(cpc,
         "  constexpr uint32_t kYieldPollCount = 16;\n"
         "  uint32_t unmatched_poll_count = 0;\n"
         "  bool matched = false;\n",
         "  constexpr uint32_t kYieldPollCount = 16;\n"
         "  uint32_t unmatched_poll_count = 0;\n"
         "  bool matched = false;\n"
         "  bool sr_wrote_back = false;  // [sr]\n")
    edit(cpc,
         "    if (!matched) {\n"
         "      PollBackgroundWork();\n",
         "    if (!matched) {\n"
         "      // [sr] the game sees how far the ring has been read before it's waited on\n"
         "      if (!sr_wrote_back && sr_primary_reader_ && read_ptr_writeback_ptr_) {\n"
         "        sr_wrote_back = true;\n"
         "        static std::atomic<uint64_t> sr_writebacks{0};\n"
         "        if (sr_writebacks.fetch_add(1) == 0)\n"
         "          REXGPU_INFO(\"sr: WAIT_REG_MEM writes the read pointer back before waiting\");\n"
         "        GpuLane::Get().Drain();\n"
         "        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),\n"
         "                                         uint32_t(sr_primary_reader_->read_offset() / sizeof(uint32_t)));\n"
         "      }\n"
         "      PollBackgroundWork();\n")

    # Both games run work on XAPI fibers (sr*_config.toml's [rexcrt] puts the runtime's host
    # fibers in their place). A host fiber's own stack was max(guest, 256 KB): the recompiled
    # code's frames are bigger than the 360's, so 2 MB at least.
    edit(sdk / "src" / "kernel" / "crt" / "threading.cpp",
         "std::max(static_cast<size_t>(guest_stack_size), static_cast<size_t>(256u * 1024u));",
         "std::max(static_cast<size_t>(guest_stack_size) * 4, static_cast<size_t>(2u * 1024u * 1024u));  // [sr]")

    # Saints Row stops loading with its audio engine's master thread waiting (KeWaitForSingleObject
    # on an event in the game's own data, 82DF7508) for a signal that never comes. The audio
    # clients the game registers, how often their callbacks run, and the first 300 waits and
    # signals on dispatcher objects in the game's image (where that barrier's live), with who.
    thr = sdk / "src" / "kernel" / "xboxkrnl" / "xboxkrnl_threading.cpp"
    edit(thr,
         "u32 KeSetEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, u32 increment, u32 wait) {\n"
         "  return xeKeSetEvent(event_ptr, increment, wait);\n",
         "// [sr] waits and signals on objects in the game's image, the first 300\n"
         "static void SrObjLog(const char* what, uint32_t object, uint32_t a, uint32_t b) {\n"
         "  if (object < 0x82000000u || object >= 0x83000000u) return;\n"
         "  static std::atomic<int> logged{0};\n"
         "  if (logged.fetch_add(1) >= 300) return;\n"
         "  auto* t = XThread::GetCurrentThread();\n"
         "  const auto* c = t && t->thread_state() ? t->thread_state()->context() : nullptr;\n"
         "  REXKRNL_INFO(\"sr obj: {} {:08X} ({}, {}) thread {:X} cpu {} lr {:08X}\", what, object, a, b,\n"
         "               t ? t->handle() : 0, t ? uint32_t(t->active_cpu()) : 0, c ? uint32_t(c->lr) : 0);\n"
         "}\n\n"
         "u32 KeSetEvent_entry(ppc_ptr_t<X_KEVENT> event_ptr, u32 increment, u32 wait) {\n"
         "  SrObjLog(\"KeSetEvent\", event_ptr.guest_address(), increment, wait);  // [sr]\n"
         "  return xeKeSetEvent(event_ptr, increment, wait);\n")
    edit(thr,
         "                             u32 wait) {\n"
         "  return xeKeReleaseSemaphore(semaphore_ptr, increment, adjustment, wait);\n",
         "                             u32 wait) {\n"
         "  SrObjLog(\"KeReleaseSemaphore\", semaphore_ptr.guest_address(), increment, adjustment);  // [sr]\n"
         "  return xeKeReleaseSemaphore(semaphore_ptr, increment, adjustment, wait);\n")
    edit(thr,
         "  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;\n"
         "  // REXKRNL_IMPORT_TRACE(\"KeWaitForSingleObject\"",
         "  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;\n"
         "  SrObjLog(\"KeWaitForSingleObject\", object_ptr.guest_address(), uint32_t(wait_reason),\n"
         "              timeout_ptr ? 1u : 0u);  // [sr]\n"
         "  // REXKRNL_IMPORT_TRACE(\"KeWaitForSingleObject\"")
    aud = sdk / "src" / "audio" / "audio_system.cpp"
    edit(aud,
         "  REXAPU_DEBUG(\"AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}\", callback,\n"
         "               callback_arg);\n",
         "  REXAPU_INFO(\"AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}\", callback,\n"
         "              callback_arg);  // [sr]\n")
    edit(aud,
         "        diag_pump_count++;\n",
         "        diag_pump_count++;\n"
         "        {  // [sr] the callbacks run, per client, every 10 s\n"
         "          static uint64_t calls[8] = {}, last_ms = 0;\n"
         "          ++calls[index & 7];\n"
         "          const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(\n"
         "              std::chrono::steady_clock::now().time_since_epoch()).count());\n"
         "          if (!last_ms) last_ms = now_ms;\n"
         "          if (now_ms - last_ms >= 10000) {\n"
         "            last_ms = now_ms;\n"
         "            REXAPU_INFO(\"sr audio callbacks: {} {} {} {} (clients 0-3, so far)\", calls[0], calls[1],\n"
         "                        calls[2], calls[3]);\n"
         "          }\n"
         "        }\n")

    # Saints Row's first run: every frame's resolve came out empty (a black screen). What it was
    # given, the first few times: the rectangle's vertices as the guest wrote them, the scissor,
    # the window offset.
    edit(sdk / "src" / "graphics" / "util" / "draw.cpp",
         '  if (x0 >= x1 || y0 >= y1) {\n    REXGPU_ERROR("Resolve region is empty");\n',
         '  if (x0 >= x1 || y0 >= y1) {\n'
         '    static std::atomic<int> told{0};  // [sr]\n'
         '    if (told.fetch_add(1) < 20) {\n'
         '      REXGPU_ERROR(\n'
         '          "sr: empty resolve: vertices at {:08X} (endian {}) {} {} / {} {} / {} {}, pix_center {}, "\n'
         '          "scissor {},{} {}x{}, window offset {},{} ({}), surface pitch {}, region {},{} - {},{}",\n'
         '          fetch.address * 4, uint32_t(fetch.endian), xenos::GpuSwap(vertices_guest[0], fetch.endian),\n'
         '          xenos::GpuSwap(vertices_guest[1], fetch.endian), xenos::GpuSwap(vertices_guest[2], fetch.endian),\n'
         '          xenos::GpuSwap(vertices_guest[3], fetch.endian), xenos::GpuSwap(vertices_guest[4], fetch.endian),\n'
         '          xenos::GpuSwap(vertices_guest[5], fetch.endian), half_pixel_offset, scissor.offset[0],\n'
         '          scissor.offset[1], scissor.extent[0], scissor.extent[1],\n'
         '          int32_t(pa_sc_window_offset.window_x_offset), int32_t(pa_sc_window_offset.window_y_offset),\n'
         '          regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable ? "on" : "off",\n'
         '          uint32_t(rb_surface_info.surface_pitch), x0, y0, x1, y1);\n'
         '    }\n')
    # (Saints Row's two-tile frames resolve each tile's rectangle in both tiles: one of each pair is
    # empty, every frame. Logged 20 times, not 60 times a second.)
    edit(sdk / "src" / "graphics" / "command_processor.cpp",
         "        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();\n"
         "        REXGPU_ERROR(\n"
         "            \"{}({}, {}, {}): Failed in backend \"\n",
         "        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();\n"
         "        static std::atomic<int> sr_failed_draws{0};  // [sr]\n"
         "        if (sr_failed_draws.fetch_add(1) < 20) REXGPU_ERROR(\n"
         "            \"{}({}, {}, {}): Failed in backend \"\n")

    # The DualSense features (ps5/sr_dualsense.cpp): GTA IV's feedback service with a game's own
    # light bar colour and a light resistance on R2 (Saints Row's sprint), instead of its episode
    # colours and car/gun triggers.
    edit(sdk / "include" / "rex" / "input" / "sony_feedback.h",
         "  uint32_t context_identity = 0;\n};\n",
         "  uint32_t context_identity = 0;\n"
         "  // [sr] a game's own feel: this light bar colour, and R2's resistance (0..1)\n"
         "  bool custom = false;\n"
         "  std::array<uint8_t, 3> color{};\n"
         "  float sprint = 0;\n"
         "};\n")
    # ---- From Max Payne 3 RePrescribed (its ci/patch_framework.py), missing here ----
    vcp = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    # Occlusion queries: when the 8192 host queries wrapped, AcquireOcclusionQueryIndex waited
    # for every pending one (the GPU drained) - every few frames in scenes full of light flares,
    # up to a quarter of the GPU thread's time in the chapter 1 log. Only a pending query on the
    # index being reused has to be waited for: the queue is in allocation order, so that is its
    # front, and it is 8192 queries old (long done) in practice.
    edit(vcp,
         "  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {\n"
         "    // Results still in flight must not be overwritten by the reused indices.\n"
         "    CompleteOcclusionQueries(true);\n"
         "    occlusion_query_cursor_ = 0;\n"
         "  }\n"
         "  host_index_out = occlusion_query_cursor_++;\n",
         "  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {\n"
         "    occlusion_query_cursor_ = 0;\n"
         "  }\n"
         "  // [sr] A result still in flight must not be overwritten by the reused index: only the\n"
         "  // oldest pending query can hold it (allocation order), so wait for that one alone.\n"
         "  while (!pending_occlusion_queries_.empty() &&\n"
         "         pending_occlusion_queries_.front().host_index == occlusion_query_cursor_ && !device_lost_) {\n"
         "    const uint64_t submission = pending_occlusion_queries_.front().submission;\n"
         "    if (submission_completed_ < submission) {\n"
         "      CheckSubmissionFenceAndDeviceLoss(submission);\n"
         "      if (submission_completed_ < submission) break;\n"
         "    }\n"
         "    CompleteOcclusionQueries(false);\n"
         "  }\n"
         "  host_index_out = occlusion_query_cursor_++;\n")
    # Alpha to coverage on the EDRAM path (fragment shader interlock) did nothing on the PS5. The
    # GPU supports demote-to-helper, and on that path the coverage it computed (in its own branch,
    # never merged out of it) wasn't merged into the pixel's sample mask, only on the other path:
    # every alpha-to-coverage draw came out fully covered. The grass around Max fades out by
    # alpha to coverage (its cards' alpha down to a quarter within a metre of him): drawn solid,
    # the cards turned into big grass-textured triangles across the screen, worst near the camera
    # (Chapter V; reported on 1.0.3, there since the EDRAM path). As Xenia canary does it now:
    # the coverage merged out of its branch, and into the mask on both paths.
    srb = sdk / "src" / "graphics" / "pipeline" / "shader" / "spirv_translator_rb.cpp"
    edit(srb,
         "      SpirvBuilder::IfBuilder if_alpha_to_coverage_enabled(\n"
         "          builder_->createBinOp(spv::OpINotEqual, type_bool_, alpha_to_mask, const_uint_0_),\n"
         "          spv::SelectionControlDontFlattenMask, *builder_);\n",
         "      // [sr] the mask without alpha to coverage, for the merge after its branch\n"
         "      const spv::Id fsi_sample_mask_before_alpha_to_coverage = fsi_sample_mask_in_rt_0_alpha_tests;\n"
         "      SpirvBuilder::IfBuilder if_alpha_to_coverage_enabled(\n"
         "          builder_->createBinOp(spv::OpINotEqual, type_bool_, alpha_to_mask, const_uint_0_),\n"
         "          spv::SelectionControlDontFlattenMask, *builder_);\n")
    edit(srb,
         "      if_alpha_to_coverage_enabled.makeEndIf();\n"
         "    }\n",
         "      if_alpha_to_coverage_enabled.makeEndIf();\n"
         "      if (edram_fragment_shader_interlock_) {\n"
         "        // [sr] out of the branch it was computed in\n"
         "        fsi_sample_mask_in_rt_0_alpha_tests = if_alpha_to_coverage_enabled.createMergePhi(\n"
         "            fsi_sample_mask_in_rt_0_alpha_tests, fsi_sample_mask_before_alpha_to_coverage);\n"
         "      }\n"
         "    }\n")
    edit(srb,
         "      if (!features_.demote_to_helper_invocation) {\n"
         "        // The tests might have modified the sample mask via\n"
         "        // fsi_sample_mask_in_rt_0_alpha_tests.\n",
         "      {  // [sr] with demote too: alpha to coverage changes the mask either way\n"
         "        // The tests might have modified the sample mask via\n"
         "        // fsi_sample_mask_in_rt_0_alpha_tests.\n")
    # [sr] A vertex buffer bound at the address and size it had last time was never requested from
    # the shared memory again (vertex_buffer_states_ outlived the frame), so the GPU kept its old copy
    # after the game rewrote it: the grass that bends around Max (rebuilt each frame in buffers the
    # game reuses) drew another frame's vertices - triangles stretched across the screen (Chapter V,
    # reported on 1.0.3). Each frame they're requested once again, as in Xenia.
    edit(vcp,
         "    shared_memory_->CheckStalePages(uint32_t(REXCVAR_GET(gpu_stale_check_pages_per_frame)));\n"
         "  }\n"
         "  vertex_buffers_in_sync_[0] = 0;\n"
         "  vertex_buffers_in_sync_[1] = 0;\n",
         "    shared_memory_->CheckStalePages(uint32_t(REXCVAR_GET(gpu_stale_check_pages_per_frame)));\n"
         "  }\n"
         "  InvalidateAllVertexBufferResidency();  // [sr] (the states too: requested again each frame)\n")
    # [sr] Occlusion queries on the EDRAM path (fragment shader interlock): the depth / stencil
    # test is done in the shader, and a pixel failing it only skipped its colour writes - the host
    # query still counted it, as no hardware test had rejected it. Saints Row 2 hides its lights'
    # glows with queries: every glow counted as visible and drew through walls and people. Now a
    # pixel whose samples all failed is demoted to a helper once the interlock has ended, which a
    # query doesn't count (with demote-to-helper, as the PS5 has).
    sth = sdk / "include" / "rex" / "graphics" / "pipeline" / "shader" / "spirv_translator.h"
    edit(sth,
         "  spv::Id main_fsi_sample_mask_;\n",
         "  spv::Id main_fsi_sample_mask_;\n"
         "  // [sr] the sample mask after the depth / stencil test, for occlusion queries\n"
         "  spv::Id var_sr_fsi_depth_mask_ = spv::NoResult;\n")
    stc = sdk / "src" / "graphics" / "pipeline" / "shader" / "spirv_translator.cpp"
    edit(stc,
         "  var_main_kill_pixel_ = spv::NoResult;\n",
         "  var_main_kill_pixel_ = spv::NoResult;\n"
         "  var_sr_fsi_depth_mask_ = spv::NoResult;  // [sr]\n")
    edit(stc,
         "    builder_->createNoResultOp(spv::OpBeginInvocationInterlockEXT);\n"
         "    FSI_DepthStencilTest(msaa_samples, false);\n",
         "    builder_->createNoResultOp(spv::OpBeginInvocationInterlockEXT);\n"
         "    FSI_DepthStencilTest(msaa_samples, false);\n"
         "    if (features_.demote_to_helper_invocation) {  // [sr] (for occlusion queries)\n"
         "      var_sr_fsi_depth_mask_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction,\n"
         "                                                        type_uint_, \"xe_var_sr_fsi_depth_mask\",\n"
         "                                                        builder_->makeUintConstant(UINT32_MAX));\n"
         "      builder_->createStore(main_fsi_sample_mask_, var_sr_fsi_depth_mask_);\n"
         "    }\n")
    edit(srb,
         "  if (edram_fragment_shader_interlock_ && !FSI_IsDepthStencilEarly()) {\n"
         "    if (msaa_samples == spv::NoResult) {\n"
         "      msaa_samples = LoadMsaaSamplesFromFlags();\n"
         "    }\n",
         "  if (edram_fragment_shader_interlock_ && !FSI_IsDepthStencilEarly() &&\n"
         "      features_.demote_to_helper_invocation) {  // [sr] (for occlusion queries)\n"
         "    var_sr_fsi_depth_mask_ = builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction,\n"
         "                                                      type_uint_, \"xe_var_sr_fsi_depth_mask\",\n"
         "                                                      builder_->makeUintConstant(UINT32_MAX));\n"
         "  }\n"
         "  if (edram_fragment_shader_interlock_ && !FSI_IsDepthStencilEarly()) {\n"
         "    if (msaa_samples == spv::NoResult) {\n"
         "      msaa_samples = LoadMsaaSamplesFromFlags();\n"
         "    }\n")
    edit(srb,
         "      FSI_DepthStencilTest(msaa_samples,\n"
         "                           fsi_pixel_potentially_killed || (color_targets_written & 0b1));\n",
         "      FSI_DepthStencilTest(msaa_samples,\n"
         "                           fsi_pixel_potentially_killed || (color_targets_written & 0b1));\n"
         "      if (var_sr_fsi_depth_mask_ != spv::NoResult) {  // [sr]\n"
         "        builder_->createStore(main_fsi_sample_mask_, var_sr_fsi_depth_mask_);\n"
         "      }\n")
    edit(srb,
         "    builder_->createNoResultOp(spv::OpEndInvocationInterlockEXT);\n"
         "  }\n"
         "}\n",
         "    builder_->createNoResultOp(spv::OpEndInvocationInterlockEXT);\n"
         "    if (var_sr_fsi_depth_mask_ != spv::NoResult) {\n"
         "      // [sr] every sample failed the depth / stencil test: not counted by occlusion queries\n"
         "      SpirvBuilder::IfBuilder sr_depth_failed(\n"
         "          builder_->createBinOp(spv::OpIEqual, type_bool_,\n"
         "                                builder_->createLoad(var_sr_fsi_depth_mask_, spv::NoPrecision),\n"
         "                                const_uint_0_),\n"
         "          spv::SelectionControlDontFlattenMask, *builder_);\n"
         "      builder_->addExtension(\"SPV_EXT_demote_to_helper_invocation\");\n"
         "      builder_->addCapability(spv::CapabilityDemoteToHelperInvocationEXT);\n"
         "      builder_->createNoResultOp(spv::OpDemoteToHelperInvocationEXT);\n"
         "      sr_depth_failed.makeEndIf();\n"
         "    }\n"
         "  }\n"
         "}\n")

    # The PlayStation button prompts (ps5/sr_prompts.cpp): a texture about to be loaded is shown
    # to the app first (its guest data, physical address and size), which may write the
    # DualSense's glyph over the Xbox 360's before the GPU reads it.
    edit(sdk / "include" / "rex" / "graphics" / "pipeline" / "texture" / "cache.h",
         "namespace rex::graphics {\n",
         "namespace rex::graphics {\n\n"
         "// [sr] Called (on the GPU thread) before a texture's base level is uploaded from guest\n"
         "// memory: its physical address, size and format (xenos::TextureFormat). Returns whether it\n"
         "// changed the guest data (the range is then invalidated, so that the GPU's copy is redone).\n"
         "inline std::atomic<bool (*)(uint32_t physical, uint32_t size, uint32_t format)>\n"
         "    g_sr_texture_load_hook{nullptr};\n")
    # (in PrepareTextureLoad: RequestTextures' batched loads and LoadTextureData both pass there,
    # before the shared memory's ranges are requested)
    edit(sdk / "src" / "graphics" / "pipeline" / "texture" / "cache.cpp",
         "  TextureKey texture_key = texture.key();\n"
         "  if (base_outdated) {\n"
         "    PendingSharedMemoryRange pending_range;\n",
         "  TextureKey texture_key = texture.key();\n"
         "  if (base_outdated) {\n"
         "    if (auto hook = g_sr_texture_load_hook.load(std::memory_order_relaxed);  // [sr]\n"
         "        hook && texture_key.base_page && !texture_key.scaled_resolve &&\n"
         "        hook(texture_key.base_page << 12, texture.GetGuestBaseSize(), uint32_t(texture_key.format))) {\n"
         "      shared_memory().InvalidateRangeNow(texture_key.base_page << 12, texture.GetGuestBaseSize());\n"
         "    }\n"
         "    PendingSharedMemoryRange pending_range;\n")
    # The PlayStation button prompts again (ps5/sr_prompts.cpp): what a game reads from its own
    # files is shown to the app, which writes the DualSense's glyphs over the parts of an archive
    # that hold the Xbox 360's (Saints Row's font glyphs, Saints Row 2's HUD images).
    edit(sdk / "include" / "rex" / "filesystem" / "devices" / "host_path_file.h",
         "#include <span>\n",
         "#include <atomic>\n#include <span>\n")
    edit(sdk / "include" / "rex" / "filesystem" / "devices" / "host_path_file.h",
         "namespace rex::filesystem {\n",
         "namespace rex::filesystem {\n\n"
         "// [sr] Called after a read from a file in the game's folders: the file's name, where the\n"
         "// read started, and what was read (which it may change).\n"
         "inline std::atomic<void (*)(const std::string& name, size_t offset, uint8_t* data, size_t size)>\n"
         "    g_sr_file_read_hook{nullptr};\n")
    edit(sdk / "src" / "filesystem" / "devices" / "host_path_file.cpp",
         "  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {\n"
         "    return X_STATUS_SUCCESS;\n",
         "  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {\n"
         "    if (auto hook = g_sr_file_read_hook.load(std::memory_order_relaxed)) {  // [sr]\n"
         "      hook(entry()->name(), byte_offset, buffer.data(), out_bytes_read ? *out_bytes_read : buffer.size());\n"
         "    }\n"
         "    return X_STATUS_SUCCESS;\n")
    sfb = sdk / "src" / "input" / "sony_feedback.cpp"
    edit(sfb,
         "  if (slot.game.wanted > 0 && now_ms >= slot.wanted_started_ms) {\n",
         "  if (slot.game.custom) {  // [sr]\n"
         "    output.color = slot.game.color;\n"
         "  } else if (slot.game.wanted > 0 && now_ms >= slot.wanted_started_ms) {\n")
    edit(sfb,
         "    if (slot.game.trigger_context == TriggerContext::kVehicle && slot.game.driver) {\n",
         "    if (slot.game.custom) {  // [sr] R2: the sprint\n"
         "      if (slot.game.sprint > 0) {\n"
         "        const float right = intensity * slot.game.sprint;\n"
         "        output.right = Resistance(20, Force(right));\n"
         "        output.right_feel = {Feel::kPedal, right};\n"
         "      }\n"
         "    } else if (slot.game.trigger_context == TriggerContext::kVehicle && slot.game.driver) {\n")

    # The start-up trace (sr_app.cpp's rex_boot_mark: logs/boot.txt): every stage before the
    # log file exists, so an app that closes before it says where.
    mark = 'extern "C" __attribute__((weak)) void rex_boot_mark(const char*);  // [sr] start-up trace\n'
    wam = sdk / "src" / "ui" / "windowed_app_main_sdl.cpp"
    edit(wam,
         "int RunWindowedApp(int argc, char** argv) {\n  auto remaining = rex::cvar::Init(argc, argv);\n",
         mark + "#define REX_BOOT_MARK(what) (rex_boot_mark ? rex_boot_mark(what) : void())\n"
         "int RunWindowedApp(int argc, char** argv) {\n  REX_BOOT_MARK(\"RunWindowedApp\");\n"
         "  auto remaining = rex::cvar::Init(argc, argv);\n  REX_BOOT_MARK(\"cvar::Init done\");\n")
    edit(wam, "  rex::InitLoggingEarly();\n", "  rex::InitLoggingEarly();\n  REX_BOOT_MARK(\"early logging\");\n")
    edit(wam, "    if (!app_context.Initialize()) {\n",
         "    REX_BOOT_MARK(\"SDL starting\");\n    if (!app_context.Initialize()) {\n")
    edit(wam,
         "    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);\n",
         "    REX_BOOT_MARK(\"SDL started; creating the app\");\n"
         "    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);\n"
         "    REX_BOOT_MARK(\"app created\");\n")
    edit(wam, "  chdir(\"/app0\");\n  rex::ps5diag::RegisterThread(\"main\");\n",
         "  chdir(\"/app0\");\n  REX_BOOT_MARK(\"main\");\n  rex::ps5diag::RegisterThread(\"main\");\n")
    edit(wam, "  int result = RunWindowedApp(argc, argv);\n",
         "  REX_BOOT_MARK(\"main: threads started\");\n  int result = RunWindowedApp(argc, argv);\n")
    rex_app = sdk / "src" / "ui" / "rex_app.cpp"
    edit(rex_app, "bool ReXApp::OnInitialize() {\n",
         mark + "#define REX_BOOT_MARK(what) (rex_boot_mark ? rex_boot_mark(what) : void())\n"
         "bool ReXApp::OnInitialize() {\n  REX_BOOT_MARK(\"OnInitialize\");\n")
    edit(rex_app, "  OnConfigurePaths(path_config);\n",
         "  REX_BOOT_MARK(\"paths\");\n  OnConfigurePaths(path_config);\n")
    edit(rex_app, "  // Load config FIRST so log cvars have final values\n",
         "  REX_BOOT_MARK(\"loading the config\");\n  // Load config FIRST so log cvars have final values\n")
    edit(rex_app, "  rex::InitLogging(log_config);\n",
         "  REX_BOOT_MARK(\"opening the log\");\n  rex::InitLogging(log_config);\n  REX_BOOT_MARK(\"log open\");\n")

    # [sr] XamContentCreate's async job (an overlapped one: Saints Row 2 opens its DLC this way) kept
    # the root name as a view of guest memory and read it when it ran - by then the game had used
    # that memory for something else: the DLC mounted under a garbage name ("p.\xff..." where the game
    # asked for "sr2bundle0"), the game couldn't find it and said the DLC was corrupt. Copied first.
    edit(sdk / "src" / "kernel" / "xam" / "xam_content.cpp",
         "  auto run = [content_manager, xuid, root_name = root_name.value(), flags, content_data,\n",
         "  auto run = [content_manager, xuid, root_name = std::string(root_name.value()), flags, content_data,  // [sr]\n")
    # [sr] Paths aren't always UTF-8: Saints Row 2 handed a host task (an async file or content job) a
    # name with a byte that isn't, utfcpp's checked iterator threw "Invalid UTF-8" and the task
    # worker took the game down (REX_FATAL) on the way to the main menu. A byte that doesn't start a
    # valid sequence is read as itself (Latin-1, as the 360's code pages mostly are), one byte on.
    edit(sdk / "thirdparty" / "utfcpp" / "source" / "utf8" / "checked.h",
         "            case internal::NOT_ENOUGH_ROOM :\n"
         "                throw not_enough_room();\n"
         "            case internal::INVALID_LEAD :\n"
         "            case internal::INCOMPLETE_SEQUENCE :\n"
         "            case internal::OVERLONG_SEQUENCE :\n"
         "                throw invalid_utf8(static_cast<utfchar8_t>(*it));\n"
         "            case internal::INVALID_CODE_POINT :\n"
         "                throw invalid_code_point(cp);\n",
         "            case internal::NOT_ENOUGH_ROOM :\n"
         "            case internal::INVALID_LEAD :\n"
         "            case internal::INCOMPLETE_SEQUENCE :\n"
         "            case internal::OVERLONG_SEQUENCE :\n"
         "            case internal::INVALID_CODE_POINT :\n"
         "                // [sr] the byte as itself (Latin-1), one byte on: no throw\n"
         "                if (it == end) return 0xFFFD;\n"
         "                cp = static_cast<utfchar8_t>(*it);\n"
         "                ++it;\n"
         "                break;\n")


if __name__ == "__main__":
    main()
