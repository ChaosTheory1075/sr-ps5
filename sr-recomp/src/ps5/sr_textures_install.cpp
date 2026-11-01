// SAINTS ROW: ReInstated (PS5) - the HD textures installed from the PC texture packs the user puts
// in install/ (Max Payne 3 RePrescribed's way): the TexMod packages (.tpf) of Saints Row's and
// Saints Row 2's 4K packs, as extracted from their downloads. hub/sr-textures.txt
// (tools/sr_texture_manifest.py) lists each package by name and size, and for every texture
// in it the Xbox 360 texture it replaces (the key the HD texture replacer looks for) and where its
// bytes are in the package: the app unpacks those into <app>/textures/<key>.dds.
//
// A .tpf is a zip archive XOR-ed with 0x3FA43FA4 (by little-endian word: the bytes at even offsets
// with A4, odd ones with 3F, a last partial word's all with A4), its entries ZipCrypto-encrypted
// with TexMod's password and deflated.
#include "sr_install.h"
#if REX_PLATFORM_PS5

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

// (deflate by stb_image's own decoder, a private copy as sr_menu.cpp's: no library linked in)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace srps5::install {

namespace {

fs::path ListPath(const fs::path& app) { return app / "hub" / "sr-textures.txt"; }


struct Entry {
  uint64_t offset = 0;
  uint32_t stored = 0, size = 0, crc = 0;
  int method = 0;
  std::string key;
};
struct Pack {
  int game = 0;
  uint64_t size = 0;
  std::string name;
  std::vector<Entry> entries;
};

std::mutex g_mutex;
std::map<uint32_t, Pack> g_packs;  // by id (guarded by g_mutex)
bool g_loaded = false;

void Load(const fs::path& app) {
  std::lock_guard lock(g_mutex);
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream in(ListPath(app));
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 2 || line[0] == '#') continue;
    if (line[0] == 'P') {
      unsigned id = 0, game = 0;
      unsigned long long size = 0;
      int n = 0;
      if (std::sscanf(line.c_str(), "P %x %u %llx %n", &id, &game, &size, &n) < 3 || !n) continue;
      Pack& p = g_packs[id];
      p.game = int(game);
      p.size = size;
      p.name = line.substr(size_t(n));
    } else if (line[0] == 'T') {
      unsigned id = 0, stored = 0, size = 0, crc = 0;
      unsigned long long offset = 0;
      int method = 0;
      char key[17] = {};
      if (std::sscanf(line.c_str(), "T %x %llx %x %x %d %x %16s", &id, &offset, &stored, &size, &method, &crc, key) !=
          7)
        continue;
      g_packs[id].entries.push_back({offset, stored, size, crc, method, key});
    }
  }
  size_t textures = 0;
  for (const auto& [id, p] : g_packs) textures += p.entries.size();
  REXLOG_INFO("SAINTS ROW: ReInstated HD textures: {} packages, {} textures in the list", g_packs.size(), textures);
}

const Pack* Find(const fs::path& app, const fs::path& file) {
  Load(app);
  std::error_code ec;
  const uint64_t size = fs::file_size(file, ec);
  if (ec) return nullptr;
  const std::string name = file.filename().string();
  std::lock_guard lock(g_mutex);
  for (const auto& [id, p] : g_packs)
    if (p.size == size && strcasecmp(p.name.c_str(), name.c_str()) == 0) return &p;
  return nullptr;
}

// (a package installed with this list's textures; a newer list's are installed again)
fs::path Marker(const fs::path& app, const Pack& p) {
  uint64_t h = 1469598103934665603ull;
  for (const Entry& e : p.entries)
    for (char c : e.key) h = (h ^ uint8_t(c)) * 1099511628211ull;
  char tag[24];
  std::snprintf(tag, sizeof(tag), "%016llx", (unsigned long long)h);
  return app / "textures" / (".installed-" + p.name + "-" + std::to_string(p.size) + "-" + tag);
}

// ZipCrypto (PKWARE's traditional encryption).
struct ZipCrypto {
  uint32_t k[3] = {0x12345678u, 0x23456789u, 0x34567890u};
  explicit ZipCrypto(const uint8_t* pw, size_t n) {
    for (size_t i = 0; i < n; ++i) Update(pw[i]);
  }
  static uint32_t Crc(uint32_t c, uint8_t b) {
    static const std::array<uint32_t, 256> table = [] {
      std::array<uint32_t, 256> t{};
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        t[i] = c;
      }
      return t;
    }();
    return table[(c ^ b) & 0xFF] ^ (c >> 8);
  }
  void Update(uint8_t b) {
    k[0] = Crc(k[0], b);
    k[1] = (k[1] + (k[0] & 0xFF)) * 134775813u + 1;
    k[2] = Crc(k[2], uint8_t(k[1] >> 24));
  }
  void Decrypt(uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      const uint16_t t = uint16_t(k[2] | 2);
      d[i] ^= uint8_t((t * (t ^ 1)) >> 8);
      Update(d[i]);
    }
  }
};

constexpr uint8_t kPassword[] = {0x73, 0x2A, 0x63, 0x7D, 0x5F, 0x0A, 0xA6, 0xBD, 0x7D, 0x65, 0x7E, 0x67, 0x61, 0x2A,
                                 0x7F, 0x7F, 0x74, 0x61, 0x67, 0x5B, 0x60, 0x70, 0x45, 0x74, 0x5C, 0x22, 0x74, 0x5D,
                                 0x6E, 0x6A, 0x73, 0x41, 0x77, 0x6E, 0x46, 0x47, 0x77, 0x49, 0x0C, 0x4B, 0x46, 0x6F};

// One texture's DDS from the package (`file`, `file_size` bytes), or false.
bool Unpack(std::FILE* f, uint64_t file_size, const Entry& e, std::vector<uint8_t>& out, std::string& error) {
  std::vector<uint8_t> raw(e.stored);
  if (std::fseek(f, long(e.offset), SEEK_SET) != 0 || std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
    error = "couldn't read the package";
    return false;
  }
  // (TexMod's XOR, by the bytes' place in the file)
  const uint64_t words_end = file_size / 4 * 4;
  for (size_t i = 0; i < raw.size(); ++i) {
    const uint64_t at = e.offset + i;
    raw[i] ^= at >= words_end || !(at & 1) ? 0xA4 : 0x3F;
  }
  ZipCrypto(kPassword, sizeof(kPassword)).Decrypt(raw.data(), raw.size());
  if (raw.size() < 12) {
    error = "a texture's entry is too short";
    return false;
  }
  out.resize(e.size);
  if (e.method == 0) {
    if (raw.size() - 12 != e.size) {
      error = "a texture's size is off";
      return false;
    }
    std::memcpy(out.data(), raw.data() + 12, e.size);
  } else {
    const int n = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), int(out.size()),
                                                   reinterpret_cast<const char*>(raw.data() + 12),
                                                   int(raw.size() - 12));
    if (n != int(e.size)) {
      error = "a texture didn't unpack";
      return false;
    }
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t b : out) crc = ZipCrypto::Crc(crc, b);
  if (~crc != e.crc) {
    error = "a texture's checksum is off (a different version of the pack?)";
    return false;
  }
  return true;
}

}  // namespace

bool TexturePackOf(const fs::path& app, const fs::path& file, int& game, bool& installed) {
  const Pack* p = Find(app, file);
  if (!p) return false;
  game = p->game;
  std::error_code ec;
  installed = fs::exists(Marker(app, *p), ec);
  return true;
}

uint64_t TexturePackBytes(const fs::path& app, const fs::path& file) {
  const Pack* p = Find(app, file);
  uint64_t n = 0;
  if (p)
    for (const Entry& e : p->entries) n += e.size;
  return n;
}

int TexturesInstalled(const fs::path& app) {
  int n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(app / "textures", ec), end; !ec && it != end; it.increment(ec))
    n += it->path().extension() == ".dds";
  return n;
}

bool InstallTexturePack(const fs::path& app, const fs::path& file, std::atomic<uint64_t>& done,
                        std::atomic<uint64_t>& total, std::string& result) {
  const Pack* p = Find(app, file);
  if (!p) {
    result = file.filename().string() + " isn't one of the texture packs this app knows.";
    return false;
  }
  std::error_code ec;
  const fs::path dir = app / "textures";
  fs::create_directories(dir, ec);
  uint64_t all = 0;
  for (const Entry& e : p->entries) all += e.stored;
  total = all;
  std::error_code size_ec;
  const uint64_t file_size = fs::file_size(file, size_ec);
  // Each replacement once (several of the game's textures can share one: consecutive entries at the
  // same offset), unpacked by four threads, each with its own handle on the package.
  std::vector<std::pair<size_t, size_t>> groups;  // [first, end) of p->entries
  for (size_t i = 0; i < p->entries.size();) {
    size_t j = i + 1;
    while (j < p->entries.size() && p->entries[j].offset == p->entries[i].offset) ++j;
    groups.push_back({i, j});
    i = j;
  }
  std::atomic<size_t> next{0}, written{0};
  std::atomic<uint64_t> unpack_us{0}, write_us{0};
  std::mutex error_mutex;
  std::string error;  // (the first one, under error_mutex)
  auto now_us = [] {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  };
  const uint64_t start = now_us();
  auto work = [&] {
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) {
      std::lock_guard lock(error_mutex);
      if (error.empty()) error = "couldn't open the package";
      return;
    }
    std::vector<uint8_t> dds;
    std::string e_error;
    for (size_t g; (g = next.fetch_add(1)) < groups.size();) {
      {
        std::lock_guard lock(error_mutex);
        if (!error.empty()) break;
      }
      const uint64_t t0 = now_us();
      const Entry& first = p->entries[groups[g].first];
      if (!Unpack(f, file_size, first, dds, e_error)) {
        std::lock_guard lock(error_mutex);
        if (error.empty()) error = e_error;
        break;
      }
      const uint64_t t1 = now_us();
      for (size_t k = groups[g].first; k < groups[g].second; ++k) {
        const std::string& key = p->entries[k].key;
        const fs::path out = dir / (key + ".dds"), tmp = dir / (key + ".dds.part");
        std::error_code wec;
        if (std::FILE* o = std::fopen(tmp.c_str(), "wb")) {
          const bool ok = std::fwrite(dds.data(), 1, dds.size(), o) == dds.size();
          std::fclose(o);
          if (ok) fs::rename(tmp, out, wec), ++written;
          else fs::remove(tmp, wec);
        }
        done += p->entries[k].stored;
      }
      unpack_us += t1 - t0;
      write_us += now_us() - t1;
    }
    std::fclose(f);
  };
  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) workers.emplace_back(work);
  for (auto& t : workers) t.join();
  REXLOG_INFO("SAINTS ROW: ReInstated HD textures: {} unpacked in {:.1f} s (threads' time: unpacking {:.1f} s, "
              "writing {:.1f} s)",
              p->name, double(now_us() - start) / 1e6, double(unpack_us) / 1e6, double(write_us) / 1e6);
  if (!error.empty()) {
    result = "Installing the HD textures from " + p->name + " failed: " + error + ".";
    return false;
  }
  if (std::FILE* m = std::fopen(Marker(app, *p).c_str(), "wb")) std::fclose(m);
  result = std::string(kGameNames[p->game]) + "'s HD textures from " + p->name + ": " + std::to_string(written.load()) +
           " installed.";
  return written.load() > 0;
}

}  // namespace srps5::install

#endif  // REX_PLATFORM_PS5
