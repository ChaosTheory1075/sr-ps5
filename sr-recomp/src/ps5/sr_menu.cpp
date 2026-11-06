// SAINTS ROW: ReInstated (PS5) - the menu (see sr_menu.h): shown before Saints Row is set up
// (SrApp::OnFinalizePaths, asynchronous), driven by the DualSense through scePad, the chosen
// game's song looping under it. L1/R1 change the page; on HOME, up and down choose the game.
// Saints Row carries on in this program; Saints Row 2 is sr2.bin, started in its place.
// (Its pages and trophy list are Crackdown PS5's crackdown_menu.cpp.)
#include "sr_menu.h"
#if REX_PLATFORM_PS5

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/sony_feedback.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "game/sound.h"
#include "imgui.h"
#include "sr_install.h"
#include "sr_prompts.h"
#include "sr_text.h"
#include "sr_trophies.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include "qrcodegen/qrcodegen.hpp"  // (SEND LOGS: the GitHub issue's QR code)

extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
}

// The fonts, built into the app (gta4-ps5's ps5/assets/fonts: Oswald and
// Inter, SIL Open Font License).
#define SR_INCBIN(name, file)                                                        \
  __asm__(".section .rodata\n.global " #name "\n.balign 16\n" #name ":\n.incbin \"" \
          file "\"\n.global " #name "_end\n" #name "_end:\n.byte 0\n.previous\n");    \
  extern "C" const unsigned char name[];                                              \
  extern "C" const unsigned char name##_end[];
SR_INCBIN(srps5_font_title, SRPS5_FONTS_DIR "/oswald-latin-700-normal.ttf")
SR_INCBIN(srps5_font_item, SRPS5_FONTS_DIR "/oswald-latin-600-normal.ttf")
SR_INCBIN(srps5_font_body, SRPS5_FONTS_DIR "/inter-latin-400-normal.ttf")

extern "C" int sceSystemServiceParamGetInt(int param_id, int* value);

namespace srps5 {
namespace {

namespace fs = std::filesystem;

ImFont* g_title = nullptr;
ImFont* g_item = nullptr;
ImFont* g_body = nullptr;

// The menu's pictures: kept for the app's life (the frame PLAY was pressed on
// is rendered after the dialog has gone).
std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>* g_textures =
    new std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>();

// ---- the pad ----

enum : uint32_t {
  kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kCross = 16, kCircle = 32, kL1 = 64, kR1 = 128, kTriangle = 256,
  kSquare = 512, kOptions = 1024
};
constexpr const char* kGameTitles[3] = {"", "SAINTS ROW", "SAINTS ROW 2"};

constexpr uint32_t kPadUp = 0x00000010u, kPadRight = 0x00000020u, kPadDown = 0x00000040u,
                   kPadLeft = 0x00000080u, kPadL1 = 0x00000400u, kPadR1 = 0x00000800u,
                   kPadTriangle = 0x00001000u, kPadCircle = 0x00002000u, kPadCross = 0x00004000u,
                   kPadSquare = 0x00008000u, kPadOptions = 0x00000008u;
struct PadData {
  uint32_t buttons;
  uint8_t lx, ly, rx, ry, l2, r2;
  uint16_t padding;
  uint8_t rest[240];
};

// (a handle of our own: the input driver later gets the same one)
class Pad {
 public:
  // The buttons pressed since the last read (held directions repeat).
  uint32_t Pressed() {
    if (handle_ < 0) {
      sceUserServiceInitialize(nullptr);
      scePadInit();
      int32_t user = -1;
      if (sceUserServiceGetInitialUser(&user) == 0 && user >= 0) {
        handle_ = scePadOpen(user, 0, 0, nullptr);
        if (handle_ < 0) handle_ = scePadGetHandle(user, 0, 0);
      }
      if (handle_ < 0) return 0;
    }
    PadData d{};
    if (scePadReadState(handle_, &d) != 0) return 0;
    uint32_t b = 0;
    if ((d.buttons & kPadUp) || d.ly < 64) b |= kUp;
    if ((d.buttons & kPadDown) || d.ly > 192) b |= kDown;
    if ((d.buttons & kPadLeft) || d.lx < 64) b |= kLeft;
    if ((d.buttons & kPadRight) || d.lx > 192) b |= kRight;
    if (d.buttons & kPadCross) b |= kCross;
    if (d.buttons & kPadCircle) b |= kCircle;
    if (d.buttons & kPadTriangle) b |= kTriangle;
    if (d.buttons & kPadSquare) b |= kSquare;
    if (d.buttons & kPadL1) b |= kL1;
    if (d.buttons & kPadR1) b |= kR1;
    if (d.buttons & kPadOptions) b |= kOptions;
    uint32_t pressed = b & ~last_;
    const auto now = std::chrono::steady_clock::now();
    constexpr uint32_t kDirs = kUp | kDown | kLeft | kRight;
    if (b & kDirs) {
      if ((b & kDirs) != (last_ & kDirs)) {
        repeat_at_ = now + std::chrono::milliseconds(350);
      } else if (now >= repeat_at_) {
        pressed |= b & kDirs;
        repeat_at_ = now + std::chrono::milliseconds(90);
      }
    }
    last_ = b;
    // (the button that started the app is still down on the first read)
    if (first_) {
      first_ = false;
      return 0;
    }
    return pressed;
  }

 private:
  int32_t handle_ = -1;
  uint32_t last_ = 0;
  bool first_ = true;
  std::chrono::steady_clock::time_point repeat_at_{};
};

// ---- the settings (app/menu.ini: key=value lines) ----

struct Settings {
  bool host_rts = false;       // EXPERIMENTAL RENDERER: FAST (host render targets) or ACCURATE (EDRAM, fsi)
  bool cheat_menu = true;      // L3 + R3 opens the in-game quick menu (sr_quick_menu)
  int dualsense_strength = 2;  // 1..3: low, medium, high (gta4_sony_intensity)
  int aa = 2;         // swap_post_effect: 0 none, 1 fxaa, 2 smaa (Max Payne 3 RePrescribed's)
  bool fsr = true;    // the picture's stretch to the TV: FSR 1 (present_effect fsr) or bilinear
  int sharpness = 3;  // FSR 1's sharpening (present_fsr_sharpness_reduction): 1 low, 2 medium, 3 high
  bool fps60 = false;  // FRAME RATE 60: the guest's vblanks twice as fast (guest_vblank_scale 2)
  int filter = 3;     // anisotropic_override 2..5: 2x, 4x, 8x, 16x
  bool lane = true;   // the GPU's second lane (crackdown_gpu_lane)
  int music = 60;     // the menu song's level, 0..100
  int game = 1;       // the game HOME has chosen: 1 Saints Row, 2 Saints Row 2
  int fps = 0;        // sr_fps_counter: 0 off, 1 simple, 2 detailed
  bool dualsense = true;   // the light bar and R2 (gta4_sony_enabled: sr_dualsense.h)
  bool ps_prompts = true;  // PlayStation button prompts (sr_ps_prompts: sr_prompts.h)
  bool skip_intro = true;  // the THQ and Volition logo movies (sr_skip_intro: sr_intro.cpp)
  bool trees = false;      // Saints Row's high-detail trees at any distance (sr_tree_detail: sr_app.cpp)
  int language = 0;  // LANGUAGE: 0 AUTO (the PS5's own: SystemLanguage), or the Xbox 360's language ID: kLanguages

  struct Language {
    int id;
    const char* name;
    bool p1;  // (on Saints Row's disc too: English, French, Spanish, Italian)
  };
  static constexpr Language kLanguages[] = {{0, "AUTO", true}, {1, "ENGLISH", true}, {4, "FRENCH", true}, {3, "GERMAN", true},
                                            {6, "ITALIAN", true}, {5, "SPANISH", true}, {9, "PORTUGUESE (BR)", false}};
  static constexpr int kPortuguese = 9;  // (the Xbox 360's number for it; no disc here has it)
  // AUTO: the PS5's own language as the Xbox 360 numbers it, when the games have it (kLanguages);
  // English otherwise. (Asked once: the PS5's language doesn't change while the app runs.)
  static int SystemLanguage() {
    static const int id = [] {
      int v = 1;
      if (sceSystemServiceParamGetInt(1 /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */, &v) != 0) v = 1;
      switch (v) {
        case 2: case 22: return 4;            // French, French (Canada)
        case 3: case 20: return 5;            // Spanish, Spanish (Latin America)
        case 4: return 3;                     // German
        case 5: return 6;                     // Italian
        case 7: case 17: return kPortuguese;  // Portuguese, Portuguese (Brazil)
        case 16: return 11;                   // Polish
        case 8: return 12;                    // Russian
        default: return 1;
      }
    }();
    for (const auto& l : kLanguages)
      if (l.id == id && id) return id;
    return 1;
  }
  // The language in use: the one chosen, or the PS5's for AUTO.
  int Effective() const { return language ? language : SystemLanguage(); }

  static int LanguageIndex(int id) {
    for (int i = 0; i < int(std::size(kLanguages)); ++i)
      if (kLanguages[i].id == id) return i;
    return 0;
  }

  static constexpr const char* kFilterNames[] = {"", "", "2X", "4X", "8X", "16X"};
  static constexpr const char* kAaNames[] = {"OFF", "FXAA", "SMAA"};
  static constexpr const char* kSharpnessNames[] = {"", "LOW", "MEDIUM", "HIGH"};
  // FSR 1's RCAS sharpness reduction in stops (0 = sharpest) for each sharpness.
  static constexpr const char* kSharpnessStops[] = {"", "1.0", "0.5", "0.2"};
  static constexpr const char* kFpsModes[] = {"off", "simple", "detailed"};

  // The recommended settings (SQUARE on SETTINGS): the user's own, tested (2026-11-03), the same for
  // both games. The menu's song level and the chosen game stay as they are.
  static Settings Recommended(const Settings& now) {
    Settings s;
    s.aa = 2, s.fsr = true, s.sharpness = 3, s.host_rts = false, s.cheat_menu = false, s.dualsense_strength = 3;
    s.filter = 5, s.lane = true, s.fps = 1, s.dualsense = true, s.ps_prompts = true, s.skip_intro = true;
    s.fps60 = true, s.language = 0;
    s.music = now.music, s.game = now.game;
    s.cheat_menu = now.cheat_menu;  // (a choice, not a setting to put back: SQUARE leaves it)
    s.trees = now.trees;
    return s;
  }

  // menu.ini (the last started game's: the game reads it when it starts, and its quick menu writes it),
  // or a game's own, menu1.ini / menu2.ini.
  static Settings Load(const fs::path& app, const char* file = "menu.ini") {
    Settings s;
    if (std::FILE* f = std::fopen((app / file).string().c_str(), "rb")) {
      char line[256];
      while (std::fgets(line, sizeof(line), f)) {
        char key[64] = {};
        int value = 0;
        if (std::sscanf(line, " %63[a-z_] = %d", key, &value) != 2) continue;
        const std::string k(key);
        if (k == "fxaa" && value) s.aa = 1;  // (the earlier on/off row's)
        if (k == "anti_aliasing") s.aa = std::clamp(value, 0, 2);
        if (k == "renderer_fast") s.host_rts = value != 0;
        if (k == "cheat_menu") s.cheat_menu = value != 0;
        if (k == "dualsense_strength") s.dualsense_strength = std::clamp(value, 1, 3);
        if (k == "upscaling_fsr") s.fsr = value != 0;
        if (k == "fsr_sharpness") s.sharpness = std::clamp(value, 1, 3);
        if (k == "frame_rate") s.fps60 = value == 60;
        if (k == "texture_filtering") s.filter = std::clamp(value, 2, 5);
        if (k == "gpu_lane") s.lane = value != 0;
        if (k == "menu_music") s.music = std::clamp(value, 0, 100);
        if (k == "game") s.game = value == 2 ? 2 : 1;
        if (k == "fps_counter") s.fps = std::clamp(value, 0, 2);
        if (k == "dualsense") s.dualsense = value != 0;
        if (k == "ps_prompts") s.ps_prompts = value != 0;
        if (k == "skip_intro") s.skip_intro = value != 0;
        if (k == "tree_detail") s.trees = value != 0;
        if (k == "language") s.language = kLanguages[LanguageIndex(value)].id;
      }
      std::fclose(f);
    }
    return s;
  }

  void Save(const fs::path& app, const char* file = "menu.ini") const {
    if (std::FILE* f = std::fopen((app / file).string().c_str(), "wb")) {
      std::fprintf(f,
                   "anti_aliasing = %d\nupscaling_fsr = %d\nfsr_sharpness = %d\nrenderer_fast = %d\ncheat_menu = %d\n"
                   "dualsense_strength = %d\n"
                   "texture_filtering = %d\ngpu_lane = %d\nmenu_music = %d\ngame = %d\nfps_counter = %d\n"
                   "dualsense = %d\nps_prompts = %d\nskip_intro = %d\nframe_rate = %d\nlanguage = %d\ntree_detail = %d\n",
                   aa, fsr ? 1 : 0, sharpness, host_rts ? 1 : 0, cheat_menu ? 1 : 0, dualsense_strength, filter, lane ? 1 : 0, music, game, fps, dualsense ? 1 : 0, ps_prompts ? 1 : 0,
                   skip_intro ? 1 : 0, fps60 ? 60 : 30, language, trees ? 1 : 0);
      std::fclose(f);
    }
  }

  // The cvars they set, for this start.
  void Apply() const {
    // (the Xbox 360's own resolution, always: 2x cost the PS5 far more than it showed)
    rex::cvar::SetFlagByName("draw_resolution_scale_x", "1");
    rex::cvar::SetFlagByName("draw_resolution_scale_y", "1");
    // (the game paces itself to 60, as on the Xbox 360: the TV's vblanks)
    rex::cvar::SetFlagByName("vsync", "true");
    // (both games wait two vblanks a frame: their 30 fps, or 60 with the vblanks twice as fast)
    rex::cvar::SetFlagByName("guest_vblank_scale", fps60 ? "2" : "1");
    // (Saints Row 2's motion blur took the frame's time: at 60, half as strong. Its 1/30 s then.)
    rex::cvar::SetFlagByName("sr_motion_blur_fix", fps60 ? "true" : "false");
    {
      // (init-only, and the start's flags are final by now; the GPU is set up after this menu)
      rex::cvar::testing::ScopedLifecycleOverride override_init_only;  // (always built, cvar.cpp)
      rex::cvar::SetFlagByName("render_target_path_vulkan", host_rts ? "fbo" : "fsi");
      // (host render targets: an unclipped draw's height from its vertices, run on the CPU - Max Payne 3
      // RePrescribed's FAST renderer lost its HDR scene and depth without it)
      rex::cvar::SetFlagByName("execute_unclipped_draw_vs_on_cpu", host_rts ? "true" : "false");
    }
    rex::cvar::SetFlagByName("sr_quick_menu", cheat_menu ? "true" : "false");
    rex::cvar::SetFlagByName("gta4_sony_intensity",
                             dualsense_strength == 1 ? "0.3" : dualsense_strength == 3 ? "1.0" : "0.6");
    rex::cvar::SetFlagByName("swap_post_effect", aa == 2 ? "smaa" : aa == 1 ? "fxaa" : "none");
    rex::cvar::SetFlagByName("present_effect", fsr ? "fsr" : "bilinear");
    rex::cvar::SetFlagByName("present_fsr_sharpness_reduction", kSharpnessStops[sharpness]);
    rex::cvar::SetFlagByName("anisotropic_override", std::to_string(filter));
    rex::cvar::SetFlagByName("crackdown_gpu_lane", lane ? "true" : "false");
    rex::cvar::SetFlagByName("sr_fps_counter", kFpsModes[fps]);
    rex::input::sony::SetFeaturesEnabled(dualsense);
    rex::cvar::SetFlagByName("sr_ps_prompts", ps_prompts ? "true" : "false");
    rex::cvar::SetFlagByName("sr_skip_intro", skip_intro ? "true" : "false");
    rex::cvar::SetFlagByName("sr_tree_detail", trees ? "true" : "false");
    // (the games' text follows XGetLanguage. Portuguese, which no disc has: the game is told a
    // language it has and reads the Portuguese text in its place when this build has it -
    // sr_prompts.cpp's ptbr<N>.bin, sr_ptbr - English otherwise. The carrier: Saints Row's Spanish,
    // Saints Row 2's German, whose text files are the biggest (its Spanish subtitles are smaller
    // than the English); German there only changes one picture on the activity completion screen.)
    const Language& lang = kLanguages[LanguageIndex(Effective())];
    SetPortuguese(lang.id == kPortuguese);  // (the quick menu's and the trophy pop-up's text)
    const bool pt_text = lang.id == kPortuguese && HasPortuguese();
    rex::cvar::SetFlagByName("sr_ptbr", pt_text ? "true" : "false");
    const int carrier = SR_GAME == 2 ? 3 : 5;
    rex::cvar::SetFlagByName("user_language",
                             std::to_string(pt_text ? carrier : lang.id == kPortuguese || (SR_GAME == 1 && !lang.p1) ? 1 : lang.id));
    REXLOG_INFO("SAINTS ROW: ReInstated menu: frame rate {}, upscaling {} (sharpness {}), anti-aliasing {}, texture filtering {}, the GPU's second lane {}, "
                "frame rate counter {}, DualSense features {}, button prompts {}, skip intro {}, tree detail {}, cheat menu {}, renderer {}, "
                "language {}",
                fps60 ? 60 : 30, fsr ? "FSR 1" : "bilinear", kSharpnessNames[sharpness], kAaNames[aa], kFilterNames[filter], lane ? "on" : "off", kFpsModes[fps],
                dualsense ? "on" : "off", ps_prompts ? "PlayStation" : "Xbox", skip_intro ? "on" : "off", trees ? "high" : "as on the disc", cheat_menu ? "on" : "off", host_rts ? "FAST" : "ACCURATE",
                lang.name);
  }
};

// ---- the look ----

constexpr ImU32 kWhite = IM_COL32(236, 232, 226, 255);
constexpr ImU32 kGrey = IM_COL32(150, 146, 140, 255);
constexpr ImU32 kDim = IM_COL32(90, 88, 84, 255);
constexpr ImU32 kRed = IM_COL32(150, 60, 230, 255);  // (the accent: the Saints' purple)
constexpr ImU32 kText = IM_COL32(205, 202, 196, 255);

struct Image {
  ImTextureID id = 0;
  float w = 0, h = 0;
  explicit operator bool() const { return id != 0; }
};

float Width(ImFont* f, float size, const std::string& s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x; }

// `s` cut to `width` (with "..."), in whole UTF-8 letters.
std::string Fit(ImFont* f, float size, std::string s, float width) {
  if (Width(f, size, s) <= width) return s;
  while (!s.empty() && Width(f, size, s + "...") > width) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  }
  return s + "...";
}

// strftime's, with %a and %b in Portuguese while the menu is (the PS5's libc only has the C locale's English).
std::string Date(int64_t t, const char* format) {
  const std::time_t tt = std::time_t(t);
  const std::tm* tm = std::localtime(&tt);
  if (!tm) return {};
  std::string f = format;
  if (Portuguese()) {
    static constexpr const char* kDays[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sáb"};
    static constexpr const char* kMonths[] = {"jan", "fev", "mar", "abr", "mai", "jun",
                                              "jul", "ago", "set", "out", "nov", "dez"};
    for (size_t at; (at = f.find("%a")) != std::string::npos;) f.replace(at, 2, kDays[tm->tm_wday % 7]);
    for (size_t at; (at = f.find("%b")) != std::string::npos;) f.replace(at, 2, kMonths[tm->tm_mon % 12]);
  }
  char out[64] = {};
  std::strftime(out, sizeof(out), f.c_str(), tm);
  return out;
}

// Letter-spaced text, a whole UTF-8 letter at a time; returns its width.
float Spaced(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* t, float spacing) {
  const float x0 = p.x;
  for (const char* c = t; *c;) {
    const char* next = c + 1;
    while ((static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
    dl->AddText(f, size, p, col, c, next);
    p.x += f->CalcTextSizeA(size, FLT_MAX, 0, c, next).x + spacing;
    c = next;
  }
  return p.x - x0 - spacing;
}

// The footer's button hints, with SQUARE and TRIANGLE (QUADRADO, TRIÂNGULO) drawn as the DualSense's
// symbols, outlined like its buttons (X and O read as theirs already); returns the width (draw false:
// measured only).
float Hints(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const std::string& t, bool draw) {
  static constexpr const char* kWords[] = {"SQUARE", "QUADRADO", "TRIANGLE", "TRIÂNGULO"};
  const float x0 = p.x, icon = size * 0.66f, line = std::max(1.0f, size * 0.075f);
  size_t i = 0;
  while (i < t.size()) {
    size_t at = std::string::npos;
    int word = -1;
    for (int k = 0; k < 4; ++k) {
      const size_t a = t.find(kWords[k], i);
      if (a < at) at = a, word = k;
    }
    const size_t end = at == std::string::npos ? t.size() : at;
    if (end > i) {
      if (draw) dl->AddText(f, size, p, col, t.c_str() + i, t.c_str() + end);
      p.x += f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str() + i, t.c_str() + end).x;
    }
    if (at == std::string::npos) break;
    const float cy = p.y + size * 0.52f, h = icon * 0.5f;
    if (draw && word < 2)
      dl->AddRect(ImVec2(p.x + line, cy - h + line), ImVec2(p.x + icon - line, cy + h - line), col, 0, 0, line * 1.6f);
    else if (draw)
      dl->AddTriangle(ImVec2(p.x + icon * 0.5f, cy - h), ImVec2(p.x + icon, cy + h * 0.8f), ImVec2(p.x, cy + h * 0.8f),
                      col, line * 1.6f);
    p.x += icon;
    i = at + std::char_traits<char>::length(kWords[word]);
  }
  return p.x - x0;
}

// The pages, in the tab bar's order.
enum Page { kHome, kContent, kSaves, kSettings, kTrophies, kCredits, kPages };
// SAVES' rows: BACK UP SAVES, SEND LOGS, then the saves to restore or import
enum SavesRow { kBackupRow, kSendLogsRow, kSavesFirst };
// (in the reports SEND LOGS makes)
constexpr const char* kAppVersion = "1.0.0";
const char* PageName(int page) {
  static constexpr const char* kEn[kPages] = {"HOME", "CONTENT", "SAVES", "SETTINGS", "TROPHIES", "CREDITS"};
  static constexpr const char* kPt[kPages] = {"INÍCIO", "CONTEÚDO", "SAVES", "AJUSTES", "TROFÉUS", "CRÉDITOS"};
  return (Portuguese() ? kPt : kEn)[page];
}

// The settings page's rows, in its order (a new option: a row here, its line
// in SettingsLines and its change in ChangeSetting).
enum SettingsRow {
  kFrameRate, kUpscaling, kSharpness, kAntiAliasing, kFiltering, kLane, kFps, kTrees, kPrompts, kDualSense, kLanguage, kSkipIntro, kCheatMenu, kRenderer, kMusic, kSettingsRows
};

class MenuDialog final : public rex::ui::ImGuiDialog {
 public:
  MenuDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const fs::path& app,
             std::function<void(MenuChoice)> play)
      : ImGuiDialog(drawer), immediate_(immediate), app_(app), play_(std::move(play)) {
    LoadSettings();
    const fs::path hub = app / "hub";
    for (int g = 1; g <= 2; ++g) {
      const std::string n = std::to_string(g);
      for (const char* ext : {".jpg", ".png"})
        if (!background_[g]) background_[g] = Load(hub / ("background" + n + ext));
      logo_[g] = Load(hub / ("logo" + n + ".png"));
    }
    title_ = Load(hub / "title.png");
    game_ = set_game_ = sets_[1].game;
    shown_game_ = float(game_);
    std::error_code ec;
    // (an install the app was closed during: not to be taken for a finished one)
    fs::remove_all(app / "game1.installing", ec);
    fs::remove_all(app / "game2.installing", ec);
    for (const char* dlc : {"dlc1", "dlc2"}) {
      for (fs::directory_iterator it(app / dlc, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".installing") fs::remove_all(it->path(), ec);
      ec.clear();
    }
    fs::remove_all(app / "game1" / "update.installing", ec);
    fs::remove_all(app / "game2" / "update.installing", ec);
    ec.clear();
    Refresh();
    REXLOG_INFO("SAINTS ROW: ReInstated menu: Saints Row {}{}, Saints Row 2 {}{}, {} DLC packs, {} in install/",
                installed_.game[1] ? "installed" : T("MISSING", "FALTANDO"), installed_.update[1] ? " (TU1)" : T(" (no TU1)", " (sem TU1)"),
                installed_.game[2] ? "installed" : T("MISSING", "FALTANDO"), installed_.update[2] ? " (TU3)" : T(" (no TU3)", " (sem TU3)"),
                installed_.dlc, sources_.size());
    StartMusic(game_);
    // (the runtime's user data: /app0/UserData/sr1 and sr2)
    for (int g = 1; g <= 2; ++g) {
      user_data_[g] = app / "UserData" / (g == 2 ? "sr2" : "sr1");
      lists_[g] = trophies::Load(user_data_[g]);
      REXLOG_INFO("SAINTS ROW: ReInstated menu: {} trophies {} ({} of {} earned)", install::kGameNames[g],
                  lists_[g].loaded ? "listed" : T("not listed yet", "ainda sem lista"), lists_[g].earned, lists_[g].earnable);
    }
    ShowTrophies(game_);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (done_) return;
    const float dt = std::min(io.DeltaTime, 0.1f);
    time_ += dt;
    pictures_this_frame_ = 0;
    SetPortuguese(sets_[page_ == kSettings ? set_game_ : game_].Effective() == Settings::kPortuguese);
    HandlePad(pad_.Pressed());
    TickInstaller();
    if (music_) music_->SetVolume(leaving_ ? 0.0f : sets_[1].music / 100.0f);
    tab_x_ += (tab_target_x_ - tab_x_) * std::min(1.0f, dt * 14.0f);
    tab_w_ += (tab_target_w_ - tab_w_) * std::min(1.0f, dt * 14.0f);
    page_in_ = std::min(1.0f, page_in_ + dt * 5.0f);
    Draw(io.DisplaySize, dt);
    if (leaving_ && (leave_t_ += dt) > 0.7f) {
      SaveSettings();
      if (game_ == 2) {
        // Saints Row 2 is its own program: started in this one's place (it applies menu.ini itself)
        REXLOG_INFO("SAINTS ROW: ReInstated menu: starting Saints Row 2 (sr2.bin)");
        rex::FlushLogging();
        char* args[] = {nullptr};
        const int rc = sceSystemServiceLoadExec("/app0/sr2.bin", args);
        REXLOG_ERROR("SAINTS ROW: ReInstated menu: starting sr2.bin failed ({:#x})", uint32_t(rc));
        start_error_ = T("Couldn't start Saints Row 2 (sr2.bin is missing from the app folder, or the PS5 refused it).", "Não deu para iniciar Saints Row 2 (falta o sr2.bin na pasta do app, ou o PS5 o recusou).");
        leaving_ = false;
        leave_t_ = 0;
        if (music_) StartMusic(game_);
        return;
      }
      done_ = true;
      sets_[game_].Apply();
      Close();
      play_(MenuChoice{1});
    }
  }

 private:
  // ---- the installer (sr_install.h) ----

  void Refresh() {
    installed_ = install::Check(app_);
    sources_ = install::Scan(app_);
  }

  // The content rows: the two games (with their title updates), the DLC (in install::Remove's order).
  // (no HD TEXTURES row: there are no texture packs for these games)
  enum ContentRow { kRowGame1, kRowGame2, kRowDlc, kContentRows };

  // What waits in install/ for a content row; nullptr: nothing.
  const install::Source* SourceFor(int row) const {
    for (const auto& s : sources_) {
      const bool game = s.kind == install::Kind::kGameImage || s.kind == install::Kind::kGameFolder;
      const bool update = s.kind == install::Kind::kUpdate && !s.installed;
      if (row == kRowGame1 && (game || update) && s.game == 1) return &s;
      if (row == kRowGame2 && (game || update) && s.game == 2) return &s;
      if (row == kRowDlc && s.kind == install::Kind::kDlc && !s.installed) return &s;
    }
    return nullptr;
  }
  int DlcWaiting() const { return Waiting(install::Kind::kDlc); }
  int Waiting(install::Kind kind) const {
    int n = 0;
    for (const auto& s : sources_) n += s.kind == kind && !s.installed;
    return n;
  }
  bool RowInstalled(int row) const {
    switch (row) {
      case kRowGame1: return installed_.game[1] && installed_.update[1];
      case kRowGame2: return installed_.game[2] && installed_.update[2];
      default: return installed_.dlc > 0;
    }
  }
  // The chosen game's disc and title update: what PLAY needs.
  bool Ready() const { return installed_.game[game_] && installed_.update[game_]; }
  // The saves page's rows after BACK UP: the saves and backups to import or restore.
  std::vector<const install::Source*> SaveSources() const {
    std::vector<const install::Source*> out;
    for (const auto& s : sources_)
      if (s.kind == install::Kind::kSave || s.kind == install::Kind::kBackup) out.push_back(&s);
    return out;
  }

  // Once a frame: a finished job's outcome, and the next of what installs by itself (the discs,
  // the DLC), one at a time.
  void TickInstaller() {
    auto& job = install::TheJob();
    if (job.Generation() != job_generation_) {
      job_generation_ = job.Generation();
      result_t_ = time_;
      Refresh();
    }
    // (not in the first seconds: the menu on screen first, the progress with it)
    if (job.Busy() || leaving_ || time_ < 2.0f) return;
    bool started = false;
    for (const auto& s : sources_) {
      // (one installed already, in an earlier session: only to delete or keep)
      if (s.installed) {
        if (!tried_.count(s.path.string())) tried_.insert(s.path.string()), job.Leftover(s.path);
        continue;
      }
      if (!install::Automatic(s.kind) || tried_.count(s.path.string())) continue;
      if ((s.kind == install::Kind::kGameFolder || s.kind == install::Kind::kGameImage) && installed_.game[s.game]) continue;
      tried_.insert(s.path.string());
      job.Install(app_, s);
      started = true;
      break;
    }
    // all done: delete or keep what they were installed from (after the last outcome has been read)
    if (!started && !ask_leftovers_ && !job.Leftovers().empty() && time_ - result_t_ > 2.5f) {
      ask_leftovers_ = true;
      leftover_bytes_ = job.LeftoverBytes();
    }
  }

  // A second press of the same button on the same row within 3 s: the go-ahead.
  bool Confirmed(int key) {
    if (confirm_key_ == key && time_ - confirm_t_ < 3.0f) {
      confirm_key_ = -1;
      return true;
    }
    confirm_key_ = key;
    confirm_t_ = time_;
    return false;
  }
  bool Confirming(int key) const { return confirm_key_ == key && time_ - confirm_t_ < 3.0f; }

  // ---- input ----

  void HandlePad(uint32_t p) {
    if (leaving_ || !p) return;
    if (ask_leftovers_) {
      // (the question has the pad to itself)
      if (p & kCross) install::TheJob().DeleteLeftovers(app_), ask_leftovers_ = false;
      if (p & kCircle) install::TheJob().KeepLeftovers(app_), ask_leftovers_ = false, Refresh();
      return;
    }
    if (reporting_) {
      HandleReport(p);  // (the keyboard has the pad to itself)
      return;
    }
    if (p & (kL1 | kR1)) {
      page_ = Page((page_ + ((p & kR1) ? 1 : kPages - 1)) % kPages);
      page_in_ = 0;
      // (SETTINGS opens on the game list, at the game HOME has chosen)
      if (page_ == kSettings) set_game_ = game_, in_list_ = false, confirm_key_ = -1;
      return;
    }
    if ((p & kCircle) && page_ == kSettings && in_list_) {
      in_list_ = false;  // (back to the game list first)
      return;
    }
    if ((p & kCircle) && page_ != kHome) {
      page_ = kHome;
      page_in_ = 0;
      return;
    }
    switch (page_) {
      case kHome:
        if (p & (kUp | kDown)) {
          game_ = game_ == 1 ? 2 : 1;
          start_error_.clear();
          StartMusic(game_);
          ShowTrophies(game_);
        }
        if ((p & kCross) && install::TheJob().Busy()) break;  // (PLAY waits for the install)
        if ((p & kCross) && !Ready()) {
          Refresh();  // (copied in since the menu opened?)
          missing_flash_ = 1.0f;
        }
        if ((p & kCross) && Ready()) {
          leaving_ = true;
          if (music_) music_->FadeOut(0.6f);
        }
        break;
      case kContent: {
        if (p & kUp) content_row_ = (content_row_ + kContentRows - 1) % kContentRows;
        if (p & kDown) content_row_ = (content_row_ + 1) % kContentRows;
        auto& job = install::TheJob();
        if (job.Busy()) break;
        if (p & kCross) {
          Refresh();
          if (const install::Source* s = SourceFor(content_row_)) job.Install(app_, *s);
        }
        if ((p & kTriangle) && RowInstalled(content_row_) && Confirmed(100 + content_row_))
          job.Uninstall(app_, install::Remove(content_row_));
        break;
      }
      case kSaves: {
        const int rows = kSavesFirst + int(SaveSources().size());
        if (p & kUp) saves_row_ = (saves_row_ + rows - 1) % rows;
        if (p & kDown) saves_row_ = (saves_row_ + 1) % rows;
        saves_row_ = std::min(saves_row_, rows - 1);
        auto& job = install::TheJob();
        if (job.Busy() || !(p & kCross)) break;
        if (saves_row_ == kBackupRow) {
          job.Backup(app_);
        } else if (saves_row_ == kSendLogsRow) {
          reporting_ = true;  // (what went wrong typed first: HandleReport)
        } else if (Confirmed(200 + saves_row_)) {
          job.Install(app_, *SaveSources()[size_t(saves_row_ - kSavesFirst)]);
        }
        break;
      }
      case kSettings: {
        // SQUARE, twice: the game's recommended settings back
        if ((p & kSquare) && Confirmed(300 + set_game_)) ResetSettings();
        if (!in_list_) {
          // the game list: whose settings
          if (p & (kUp | kDown)) set_game_ = set_game_ == 1 ? 2 : 1, confirm_key_ = -1;
          if (p & (kRight | kCross)) in_list_ = true;
          break;
        }
        if (p & kUp) row_ = (row_ + kSettingsRows - 1) % kSettingsRows;
        if (p & kDown) row_ = (row_ + 1) % kSettingsRows;
        const int step = (p & kRight) ? 1 : (p & kLeft) ? -1 : 0;
        ChangeSetting(row_, step, (p & kCross) != 0);
        break;
      }
      case kTrophies: {
        if (p & kTriangle) {  // (the other game's)
          ShowTrophies(trophy_game_ == 1 ? 2 : 1);
          break;
        }
        const int n = int(items_.size());
        if (!n) break;
        if (p & kUp) trophy_ = (trophy_ + n - 1) % n;
        if (p & kDown) trophy_ = (trophy_ + 1) % n;
        if (p & kLeft) trophy_ = std::max(0, trophy_ - 8);
        if (p & kRight) trophy_ = std::min(n - 1, trophy_ + 8);
        if (p & (kUp | kDown | kLeft | kRight)) detail_in_ = 0;
        break;
      }
      default:
        break;
    }
  }

  // ---- the settings ----

  // Each game's own: menu1.ini / menu2.ini, and menu.ini, the last started game's (newest: its quick
  // menu writes there). Before there were per-game files, menu.ini was both games'.
  void LoadSettings() {
    const Settings last = Settings::Load(app_);
    for (int g = 1; g <= 2; ++g) {
      const std::string file = "menu" + std::to_string(g) + ".ini";
      std::error_code ec;
      sets_[g] = fs::exists(app_ / file, ec) ? Settings::Load(app_, file.c_str()) : last;
      sets_[g].game = last.game, sets_[g].music = last.music;
    }
    sets_[last.game] = last;
    // (CHEAT MENU is one switch for both games: on in either game's settings, it's on for both.
    // It was each game's own, and turning it on in Saints Row 2's settings left Saints Row's off.)
    sets_[1].cheat_menu = sets_[2].cheat_menu = sets_[1].cheat_menu || sets_[2].cheat_menu;
  }

  // At PLAY: each game's file, and menu.ini as the game starting reads it (Saints Row 2's own program
  // too: ApplySettings).
  void SaveSettings() {
    for (int g = 1; g <= 2; ++g) {
      sets_[g].game = game_;
      sets_[g].Save(app_, ("menu" + std::to_string(g) + ".ini").c_str());
    }
    sets_[game_].Save(app_);
  }

  // SQUARE (twice) on SETTINGS: the game's recommended settings back.
  void ResetSettings() {
    sets_[set_game_] = Settings::Recommended(sets_[set_game_]);
    reset_t_ = time_;
    REXLOG_INFO("SAINTS ROW: ReInstated menu: {}'s recommended settings put back", kGameTitles[set_game_]);
  }

  struct SettingLine {
    const char* section;  // the heading it's under
    const char* label;
    std::string value;
    bool enabled;
    const char* help;
    bool slider = false;  // (a level: the bar, not < >)
  };

  std::vector<SettingLine> SettingsLines() const {
    const Settings& s = sets_[set_game_];
    char music[16];
    std::snprintf(music, sizeof(music), "%d%%", s.music);
    std::vector<SettingLine> lines(kSettingsRows);
    lines[kFrameRate] = {T("GRAPHICS", "GRÁFICOS"), T("FRAME RATE", "TAXA DE QUADROS"), s.fps60 ? "60 (EXPERIMENTAL)" : "30", true,
                         T("30 is the frame rate both games were made for on the Xbox 360. 60 lets them run twice as "
                         "often - as their PC versions can - wherever the PS5 keeps up: busy scenes fall between "
                         "the two. Experimental: if anything plays too fast or stutters, set it back to 30.", "30 é a taxa de quadros para a qual os dois jogos foram feitos no Xbox 360. 60 os deixa rodar duas "
                           "vezes mais - como as versões de PC podem - onde o PS5 acompanha: cenas pesadas ficam entre os dois. "
                           "Experimental: se algo ficar rápido demais ou travando, volte para 30.")};
    lines[kUpscaling] = {T("GRAPHICS", "GRÁFICOS"), T("UPSCALING", "AMPLIAÇÃO"), s.fsr ? "FSR 1" : "BILINEAR", true,
                         T("How the picture is enlarged to your TV's resolution (up to 4K). FSR 1, AMD's upscaler, keeps "
                         "edges and detail sharp at almost no cost to the frame rate; BILINEAR is a plain, softer "
                         "stretch.", "Como a imagem é ampliada para a resolução da sua TV (até 4K). O FSR 1, o ampliador da AMD, mantém "
                           "bordas e detalhes nítidos quase sem custo para a taxa de quadros; BILINEAR é uma ampliação simples, "
                           "mais suave.")};
    lines[kSharpness] = {T("GRAPHICS", "GRÁFICOS"), T("SHARPNESS", "NITIDEZ"), s.fsr ? TW(Settings::kSharpnessNames[s.sharpness]) : "-",
                         s.fsr,
                         T("How much FSR 1 sharpens the enlarged picture. HIGH is crispest but can make edges look "
                         "harsh; LOW is gentler.", "Quanto o FSR 1 deixa a imagem ampliada mais nítida. ALTA é a mais definida, mas pode deixar as "
                           "bordas duras; BAIXA é mais suave.")};
    lines[kAntiAliasing] = {T("GRAPHICS", "GRÁFICOS"), T("ANTI-ALIASING", "ANTISSERRILHAMENTO"), TW(Settings::kAaNames[s.aa]), true,
                            T("Smooths jagged edges before the picture is enlarged. SMAA finds edges by their shape and "
                            "keeps the rest sharp; FXAA is a little cheaper but blurs fine detail. Both cost the "
                            "PS5's GPU very little.", "Suaviza bordas serrilhadas antes de a imagem ser ampliada. O SMAA encontra as bordas pela forma e "
                              "mantém o resto nítido; o FXAA é um pouco mais leve, mas borra detalhes finos. Os dois custam muito "
                              "pouco à GPU do PS5.")};
    lines[kFiltering] = {T("GRAPHICS", "GRÁFICOS"), T("TEXTURE FILTERING", "FILTRO DE TEXTURAS"),
                         std::string(Settings::kFilterNames[s.filter]) + T(" ANISOTROPIC", " ANISOTRÓPICO"), true,
                         T("How sharp floors, walls and streets stay when seen at an angle. Higher is sharper; the PS5's "
                         "GPU handles 16X easily.", "O quanto pisos, paredes e ruas continuam nítidos vistos de lado. Mais alto é mais nítido; a GPU do "
                           "PS5 aguenta 16X com facilidade.")};
    lines[kLane] = {"GRAPHICS", "GPU HELPER", s.lane ? T("ON", "LIGADO") : T("OFF", "DESLIGADO"), true,
                    T("A second thread that takes part of the graphics work - copying the game's memory for the GPU and "
                    "writing its texture lists - off the thread that sets the frame rate. Turn it off if something "
                    "draws wrong.", "Uma segunda thread que tira parte do trabalho gráfico - copiar a memória do jogo para a GPU e "
                      "escrever as listas de texturas - da thread que define a taxa de quadros. Desligue se algo for "
                      "desenhado errado.")};
    static constexpr const char* kFpsNames[] = {"OFF", "SIMPLE", "DETAILED"};
    lines[kFps] = {T("GRAPHICS", "GRÁFICOS"), T("FPS COUNTER", "CONTADOR DE FPS"), TW(kFpsNames[s.fps]), true,
                   T("A frame rate counter at the left edge of the screen while you play. Simple shows frames a second "
                   "and the frame time; detailed adds the slowest frames and a graph of the last few seconds.", "Um contador de quadros na borda esquerda da tela enquanto você joga. Simples mostra quadros por "
                     "segundo e o tempo de quadro; detalhado acrescenta os quadros mais lentos e um gráfico dos últimos "
                     "segundos.")};
    lines[kTrees] = {T("GRAPHICS", "GRÁFICOS"), T("TREE DETAIL", "DETALHE DAS ÁRVORES"),
                     set_game_ != 1 ? T("SAINTS ROW ONLY", "SÓ SAINTS ROW") : s.trees ? T("HIGH", "ALTO") : T("AS ON THE DISC", "COMO NO DISCO"),
                     set_game_ == 1,
                     T("Saints Row: the trees always drawn with their high-detail models, never the far-away ones "
                     "(Xenia's \"Tree LODs\" patch). Costs a little speed in busy streets.",
                     "Saints Row: as árvores sempre com os modelos detalhados, nunca os de longe (o patch \"Tree LODs\" "
                     "do Xenia). Custa um pouco de velocidade nas ruas cheias.")};
    lines[kPrompts] = {T("CONTROLLER", "CONTROLE"), T("BUTTON PROMPTS", "ÍCONES DOS BOTÕES"), s.ps_prompts ? "PLAYSTATION" : "XBOX", true,
                       T("The buttons the games show you - in hints, menus and moves - as the DualSense's (cross, "
                       "circle, square, triangle, L1, R1...) or as the Xbox 360's.", "Os botões que os jogos mostram - em dicas, menus e golpes - como os do DualSense (xis, círculo, "
                         "quadrado, triângulo, L1, R1...) ou como os do Xbox 360.")};
    lines[kDualSense] = {"CONTROLLER", "DUALSENSE FEATURES", s.dualsense ? T("ON", "LIGADO") : T("OFF", "DESLIGADO"), true,
                         T("The light bar in the Saints' purple. The games' own rumble plays either way. (In the game: "
                         "L3 + R3 opens the quick menu, with the cheats.)", "A barra de luz no roxo dos Saints. A vibração dos próprios jogos funciona de qualquer jeito. "
                           "(No jogo: L3 + R3 abre o menu rápido, com as trapaças.)")};
    lines[kLanguage] = {T("GAME", "JOGO"), T("LANGUAGE", "IDIOMA"), s.language ? std::string(TW(Settings::kLanguages[Settings::LanguageIndex(s.language)].name))
                   : std::string("AUTO (") + TW(Settings::kLanguages[Settings::LanguageIndex(s.Effective())].name) + ")",
                        true,
                        T("The language of the games' text, menus and subtitles, as the Xbox 360's own language "
                        "setting chooses it: the discs have English, French, German, Italian and Spanish. PORTUGUESE (BR) "
                        "puts this menu and both games' text and subtitles in Brazilian Portuguese (this app's "
                        "translation; the voices stay English).", "O idioma dos textos, menus e legendas dos jogos, como a configuração de idioma do próprio Xbox 360 "
                          "o escolhe: os discos têm inglês, francês, alemão, italiano e espanhol. PORTUGUÊS (BR) deixa este "
                          "menu e os textos e legendas dos dois jogos em português (tradução deste app; as vozes "
                          "continuam em inglês).")};
    lines[kSkipIntro] = {"GAME", "SKIP INTRO", s.skip_intro ? T("ON", "LIGADO") : T("OFF", "DESLIGADO"), true,
                         T("The games start straight at their title screens, without the THQ and Volition logo "
                         "movies.", "Os jogos começam direto nas telas de título, sem os vídeos de logo da THQ e da Volition.")};
    lines[kCheatMenu] = {"GAME", "CHEAT MENU (L3 + R3)", s.cheat_menu ? T("ON", "LIGADO") : T("OFF", "DESLIGADO"), true,
                         T("In the game, press both sticks in (L3 + R3) for the quick menu: the cheats, the FPS "
                         "counter and the DualSense features. OFF: no quick menu, and no cheats.", "No jogo, aperte os dois analógicos (L3 + R3) para o menu rápido: as trapaças, o contador de FPS e "
                           "os recursos do DualSense. DESLIGADO: sem menu rápido e sem trapaças.")};
    // (FEEDBACK STRENGTH isn't a row: it scales only the added pulses - a hit, from the player's
    // health - and the health is read only while a cheat runs (sr_quick_menu.cpp: polling the scripts
    // blocked Saints Row's first mission), so in normal play it changed nothing. The game's own rumble
    // is never scaled. It comes back with health read from the game's memory. dualsense_strength
    // stays in menu.ini and Apply, at MEDIUM.)
    lines[kRenderer] = {"EXPERIMENTAL", T("RENDERER", "RENDERIZADOR"), s.host_rts ? T("FAST", "RÁPIDO") : T("ACCURATE", "PRECISO"), true,
                        T("ACCURATE copies the Xbox 360's video memory exactly, pixel by pixel. FAST draws with the PS5's "
                        "own graphics hardware: quicker, and the light glows hide behind walls and people as they "
                        "should, but some effects may draw wrong. Testing.", "PRECISO copia a memória de vídeo do Xbox 360 exatamente, pixel por pixel. RÁPIDO desenha com o "
                          "hardware gráfico do próprio PS5: mais veloz, e os brilhos de luz se escondem atrás de paredes e "
                          "pessoas como deveriam, mas alguns efeitos podem sair errados. Em teste.")};
    lines[kMusic] = {T("AUDIO", "ÁUDIO"), T("MENU MUSIC", "MÚSICA DO MENU"), music, true, T("How loud this menu's song is.", "O volume da música deste menu."), true};
    return lines;
  }

  void ChangeSetting(int row, int step, bool cross) {
    Settings& s = sets_[set_game_];
    const bool flip = step || cross;
    switch (row) {
      case kFrameRate:
        if (flip) s.fps60 = !s.fps60;
        break;
      case kUpscaling:
        if (flip) s.fsr = !s.fsr;
        break;
      case kSharpness:
        if (!s.fsr) break;
        if (cross) s.sharpness = s.sharpness % 3 + 1;
        s.sharpness = std::clamp(s.sharpness + step, 1, 3);
        break;
      case kAntiAliasing:
        if (cross) s.aa = (s.aa + 1) % 3;
        s.aa = std::clamp(s.aa + step, 0, 2);
        break;
      case kFiltering:
        if (cross) s.filter = s.filter == 5 ? 2 : s.filter + 1;
        s.filter = std::clamp(s.filter + step, 2, 5);
        break;
      case kLane:
        if (flip) s.lane = !s.lane;
        break;
      case kFps:
        if (cross) s.fps = (s.fps + 1) % 3;
        s.fps = std::clamp(s.fps + step, 0, 2);
        break;
      case kPrompts:
        if (flip) s.ps_prompts = !s.ps_prompts;
        break;
      case kDualSense:
        if (flip) s.dualsense = !s.dualsense;
        break;
      case kLanguage: {
        const int n = int(std::size(Settings::kLanguages));
        const int i = (Settings::LanguageIndex(s.language) + (step ? step : cross ? 1 : 0) + n) % n;
        s.language = Settings::kLanguages[i].id;
        break;
      }
      case kSkipIntro:
        if (flip) s.skip_intro = !s.skip_intro;
        break;
      case kTrees:
        if (flip && set_game_ == 1) s.trees = !s.trees;
        break;
      case kCheatMenu:
        if (flip) s.cheat_menu = !s.cheat_menu;
        sets_[1].cheat_menu = sets_[2].cheat_menu = s.cheat_menu;  // (the same for both games)
        break;
      case kRenderer:
        if (flip) s.host_rts = !s.host_rts;
        break;
      case kMusic:
        s.music = std::clamp(s.music + step * 10, 0, 100);
        sets_[1].music = sets_[2].music = s.music;  // (the menu's: the same for both games)
        break;
    }
  }

  // ---- the trophies ----

  // The list's groups.
  enum Group { kEarned, kNotYet, kMultiplayer };

  // A row of the list: a trophy (its index; -1: the platinum), in its group.
  struct Item {
    int index = 0, group = 0;
  };

  // The trophies page shows one game's list at a time.
  void ShowTrophies(int game) {
    trophy_game_ = game;
    trophy_ = 0;
    scroll_ = 0;
    pictures_.clear();
    TakeTrophies(lists_[game]);
  }

  void TakeTrophies(trophies::List list) {
    trophies_ = std::move(list);
    items_.clear();
    if (!trophies_.loaded) return;
    const auto& all = trophies_.trophies;
    std::vector<Item> earned, rest, online;
    for (int i = 0; i < int(all.size()); ++i) {
      const auto& t = all[size_t(i)];
      if (t.online()) online.push_back({i, kMultiplayer});
      else (t.earned ? earned : rest).push_back({i, t.earned ? kEarned : kNotYet});
    }
    std::stable_sort(earned.begin(), earned.end(), [&](const Item& a, const Item& b) {
      return all[size_t(a.index)].earned > all[size_t(b.index)].earned;
    });
    const bool plat = trophies_.Platinum();
    if (plat) items_.push_back({-1, kEarned});
    items_.insert(items_.end(), earned.begin(), earned.end());
    if (!plat) items_.push_back({-1, kNotYet});
    items_.insert(items_.end(), rest.begin(), rest.end());
    items_.insert(items_.end(), online.begin(), online.end());
  }

  bool Earned(const Item& it) const {
    return it.index < 0 ? trophies_.Platinum() : trophies_.trophies[size_t(it.index)].earned != 0;
  }
  trophies::Grade GradeOf(const Item& it) const {
    return it.index < 0 ? trophies::Grade::kPlatinum : trophies_.trophies[size_t(it.index)].grade();
  }
  std::string NameOf(const Item& it) const {
    if (it.index < 0) return std::string(install::kGameNames[trophy_game_]) + T(" Platinum", " Platina");
    const auto& t = trophies_.trophies[size_t(it.index)];
    return (!t.earned && t.hidden()) ? T("Hidden trophy", "Troféu oculto") : t.name;
  }
  std::string TextOf(const Item& it) const {
    if (it.index < 0)
      return T("Earn every other ", "Conquiste todos os outros troféus de ") + std::string(install::kGameNames[trophy_game_]) + T(" trophy (", " (") +
             std::to_string(trophies_.earnable) + ").";
    const auto& t = trophies_.trophies[size_t(it.index)];
    std::string text;
    if (!t.earned && t.hidden()) text = T("Keep playing to find out what this one is.", "Continue jogando para descobrir qual é este.");
    else text = t.earned || t.locked_description.empty() ? t.description : t.locked_description;
    if (t.online())
      text += T("\n\nOnline: it needs a service this port can't reach, so it doesn't count toward the platinum.", "\n\nOnline: precisa de um serviço que este port não alcança, então não conta para a platina.");
    return text;
  }

  // A trophy's picture (the platinum's: the game's own), loaded the first time it's drawn
  // (two a frame at most, so the page doesn't hold the menu up).
  Image Picture(const Item& it) {
    const uint32_t key = it.index < 0 ? 0 : std::max(1u, trophies_.trophies[size_t(it.index)].image_id);
    if (auto found = pictures_.find(key); found != pictures_.end()) return found->second;
    if (pictures_this_frame_ >= 2) return {};
    ++pictures_this_frame_;
    const fs::path& user = user_data_[trophy_game_];
    const Image img = Load(it.index < 0 ? trophies::TitleIconFile(user)
                                        : trophies::IconFile(user, trophies_.trophies[size_t(it.index)].image_id));
    pictures_[key] = img;
    return img;
  }

  // The picture in a square: dimmed until it's earned, with a padlock; a question mark for a hidden one.
  void Icon(ImDrawList* dl, const Item& it, float x, float y, float size, float a) {
    const bool earned = Earned(it);
    const trophies::Trophy* t = it.index < 0 ? nullptr : &trophies_.trophies[size_t(it.index)];
    const float r = size * 0.08f;
    if (t && !earned && t->hidden()) {
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), IM_COL32(38, 36, 36, int(255 * a)), r);
      const char* q = "?";
      const float qs = size * 0.6f;
      dl->AddText(g_item, qs, ImVec2(x + (size - Width(g_item, qs, q)) * 0.5f, y + size * 0.16f),
                  IM_COL32(255, 255, 255, int(128 * a)), q);
      return;
    }
    const Image img = Picture(it);
    if (img) {
      const int tint = earned ? 255 : 90;
      dl->AddImageRounded(img.id, ImVec2(x, y), ImVec2(x + size, y + size), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(tint, tint, tint, int(255 * a)), r);
    } else {
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), IM_COL32(38, 36, 36, int(255 * a)), r);
      trophies::DrawCup(dl, ImVec2(x + size * 0.5f, y + size * 0.5f), size * 0.56f, GradeOf(it), a, earned);
    }
    if (!earned) {  // a padlock in the corner
      const float k = size / 64.0f, lx = x + size - 12 * k, ly = y + size - 12 * k;
      dl->AddCircleFilled(ImVec2(lx, ly), 12 * k, IM_COL32(0, 0, 0, int(190 * a)), 20);
      dl->AddRectFilled(ImVec2(lx - 5 * k, ly - 1.5f * k), ImVec2(lx + 5 * k, ly + 6 * k),
                        IM_COL32(230, 230, 230, int(220 * a)), 1.5f * k);
      dl->PathClear();
      dl->PathArcTo(ImVec2(lx, ly - 2 * k), 3.6f * k, 3.14159f, 6.28318f, 12);
      dl->PathStroke(IM_COL32(230, 230, 230, int(220 * a)), 0, 1.8f * k);
    }
  }

  // ---- drawing ----

  void Draw(const ImVec2& size, float dt) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = size.y / 1080.0f;  // (laid out for 1080p, scaled to the screen)
    const float fade = leaving_ ? std::clamp(1.0f - leave_t_ / 0.6f, 0.0f, 1.0f) : std::min(time_ / 0.8f, 1.0f);
    fade_ = fade;
    u_ = u;

    DrawBackground(dl, size, dt);
    DrawTabs(dl, size);
    const float in = 1 - (1 - page_in_) * (1 - page_in_);
    page_alpha_ = fade * in;
    page_dx_ = (1 - in) * 24 * u;
    switch (page_) {
      case kHome: DrawHome(dl, size); break;
      case kContent: DrawContent(dl, size); break;
      case kSaves: DrawSaves(dl, size); break;
      case kSettings: DrawSettings(dl, size); break;
      case kTrophies: DrawTrophies(dl, size, dt); break;
      default: DrawCredits(dl, size); break;
    }
    DrawFooter(dl, size);
    if (ask_leftovers_) DrawLeftoversQuestion(dl, size);
    if (reporting_) DrawReport(dl, size);
  }

  // ---- SEND LOGS: what went wrong, typed with the pad, and the GitHub issue's QR code ----

  // The keyboard: four rows of keys, then SPACE, DELETE and SEND.
  static constexpr const char* kKeys[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM,.?"};
  static constexpr int kKeyRows = 5, kKeyCols = 10;
  static constexpr size_t kNoteMax = 300;  // (the QR code stays easy to scan off a TV)
  // the bottom row's three keys, by column
  static int BottomKey(int col) { return col < 5 ? 0 : col < 8 ? 1 : 2; }  // SPACE, DELETE, SEND

  void TypeKey(char c) {
    if (note_.size() >= kNoteMax) return;
    note_ += (c >= 'A' && c <= 'Z' && !caps_) ? char(c - 'A' + 'a') : c;
  }

  void HandleReport(uint32_t p) {
    if (p & kCircle) {
      reporting_ = false;
      return;
    }
    if (p & kUp) key_row_ = (key_row_ + kKeyRows - 1) % kKeyRows;
    if (p & kDown) key_row_ = (key_row_ + 1) % kKeyRows;
    if (p & kLeft) key_col_ = (key_col_ + kKeyCols - 1) % kKeyCols;
    if (p & kRight) key_col_ = (key_col_ + 1) % kKeyCols;
    if (p & kL1) caps_ = !caps_;
    if (p & kSquare) {
      if (!note_.empty()) note_.pop_back();
    }
    if (p & kTriangle) TypeKey(' ');
    bool send = (p & kOptions) != 0;
    if (p & kCross) {
      if (key_row_ < 4) {
        TypeKey(kKeys[key_row_][key_col_]);
      } else {
        switch (BottomKey(key_col_)) {
          case 0: TypeKey(' '); break;
          case 1: if (!note_.empty()) note_.pop_back(); break;
          default: send = true; break;
        }
      }
    }
    if (send && !install::TheJob().Busy()) {
      sent_note_ = note_;
      sent_settings_ = SettingsSummary();
      reporting_ = false;
      install::TheJob().SendLogs(app_, note_);
    }
  }

  // The settings, for the report (the game chosen on HOME).
  std::string SettingsSummary() const {
    const Settings& st = sets_[game_];
    std::string s = std::string(kGameTitles[game_]) + ": renderer " + (st.host_rts ? "fast" : "accurate");
    s += std::string(", frame rate ") + (st.fps60 ? "60" : "30");
    s += std::string(", upscaling ") + (st.fsr ? "FSR 1" : "bilinear");
    if (st.fsr) s += std::string(" (sharpness ") + Settings::kSharpnessNames[st.sharpness] + ")";
    s += std::string(", anti-aliasing ") + Settings::kAaNames[st.aa];
    s += std::string(", filtering ") + Settings::kFilterNames[st.filter];
    s += std::string(", GPU helper ") + (st.lane ? "on" : "off");
    s += std::string(", language ") + Settings::kLanguages[Settings::LanguageIndex(st.language)].name;
    s += std::string(", ") + (st.ps_prompts ? "PlayStation" : "Xbox") + " prompts";
    s += std::string(", skip intro ") + (st.skip_intro ? "on" : "off");
    s += std::string(", DualSense ") + (st.dualsense ? "on" : "off");
    s += std::string(", quick menu ") + (st.cheat_menu ? "on" : "off");
    s += std::string(", FPS counter ") + Settings::kFpsModes[st.fps];
    if (game_ == 1) s += std::string(", detailed trees ") + (st.trees ? "on" : "off");
    return s;
  }

  static std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        out += char(c);
      } else {
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
      }
    }
    return out;
  }

  // A new issue on the project's GitHub, filled in with the report, the logs' link and the settings.
  std::string IssueUrl(const std::string& link) const {
    std::string title = "Report from the PS5";
    if (!sent_note_.empty()) title += ": " + sent_note_.substr(0, 60) + (sent_note_.size() > 60 ? "..." : "");
    std::string body = "**What happened**\n" + (sent_note_.empty() ? std::string("(not typed in)") : sent_note_) +
                       "\n\n**Logs:** https://" + link + "\n**Version:** " + kAppVersion +
                       "\n**Settings:** " + sent_settings_ + "\n\n(Sent from the app's SAVES > SEND LOGS.)\n";
    return std::string("https://github.com/ChaosTheory1075/sr-ps5/issues/new?title=") + UrlEncode(title) +
           "&body=" + UrlEncode(body);
  }

  // The QR code for the latest link (made once per link).
  void UpdateQr(const std::string& link) {
    if (link == qr_link_) return;
    qr_link_ = link;
    qr_.clear();
    qr_size_ = 0;
    if (link.empty()) return;
    try {
      const auto qr = qrcodegen::QrCode::encodeText(IssueUrl(link).c_str(), qrcodegen::QrCode::Ecc::LOW);
      qr_size_ = qr.getSize();
      qr_.resize(size_t(qr_size_) * size_t(qr_size_));
      for (int y = 0; y < qr_size_; ++y)
        for (int x = 0; x < qr_size_; ++x) qr_[size_t(y) * size_t(qr_size_) + size_t(x)] = qr.getModule(x, y);
    } catch (const std::exception& e) {
      REXLOG_WARN("SAINTS ROW: ReInstated menu: no QR code for the report ({})", e.what());
      qr_.clear();
      qr_size_ = 0;
    }
  }

  // The QR code, `side` wide with its white margin, at (x, y).
  void DrawQr(ImDrawList* dl, float x, float y, float side) {
    if (!qr_size_) return;
    const int border = 3;
    const float m = std::max(1.0f, std::floor(side / float(qr_size_ + border * 2)));
    const float full = m * float(qr_size_ + border * 2);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + full, y + full), P(IM_COL32(255, 255, 255, 255)));
    for (int row = 0; row < qr_size_; ++row) {
      for (int col = 0; col < qr_size_; ++col) {
        if (!qr_[size_t(row) * size_t(qr_size_) + size_t(col)]) continue;
        const float px = x + float(col + border) * m, py = y + float(row + border) * m;
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + m, py + m), P(IM_COL32(0, 0, 0, 255)));
      }
    }
  }

  // T("What went wrong?", "O que deu errado?"): the text so far and the keyboard, over the page.
  void DrawReport(ImDrawList* dl, const ImVec2& size) {
    const float u = u_;
    dl->AddRectFilled(ImVec2(0, 0), size, A(IM_COL32(0, 0, 0, 190)));
    const float w = 1300 * u, h = 860 * u, x = (size.x - w) * 0.5f, y = (size.y - h) * 0.5f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), A(IM_COL32(14, 12, 12, 250)), 10 * u);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 8 * u, y + h), A(kRed));
    const float l = x + 50 * u;
    dl->AddText(g_item, 44 * u, ImVec2(l, y + 36 * u), A(kWhite), T("SEND LOGS", "ENVIAR LOGS"));
    dl->AddText(g_body, 26 * u, ImVec2(l, y + 96 * u), A(kText),
                T("Say what went wrong (or leave it empty), then SEND. The logs go to catbox.moe, and a QR code "
                "opens a GitHub issue with your words and the logs' link filled in. Anyone with the link can read "
                "the logs.", "Diga o que deu errado (ou deixe vazio) e ENVIE. Os logs vão para o catbox.moe, e um QR code abre "
                  "uma issue no GitHub com suas palavras e o link dos logs preenchidos. Qualquer um com o link pode "
                  "ler os logs."),
                nullptr, w - 100 * u);
    // the text so far, with a cursor
    const float ty = y + 200 * u, tw = w - 100 * u;
    dl->AddRectFilled(ImVec2(l, ty), ImVec2(l + tw, ty + 150 * u), A(IM_COL32(30, 28, 28, 255)), 6 * u);
    std::string shown = note_ + ((int(time_ * 2) % 2) ? "_" : " ");
    dl->AddText(g_body, 28 * u, ImVec2(l + 16 * u, ty + 12 * u), A(kWhite), shown.c_str(), nullptr, tw - 32 * u);
    char count[32];
    std::snprintf(count, sizeof(count), "%zu / %zu", note_.size(), kNoteMax);
    dl->AddText(g_body, 20 * u, ImVec2(l + tw - Width(g_body, 20 * u, count), ty + 158 * u), A(kDim), count);
    // the keys
    const float ky = ty + 200 * u, kw = tw / kKeyCols, kh = 64 * u;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < kKeyCols; ++c) {
        const bool sel = r == key_row_ && c == key_col_;
        const float kx = l + c * kw, yy = ky + r * (kh + 8 * u);
        dl->AddRectFilled(ImVec2(kx + 3 * u, yy), ImVec2(kx + kw - 3 * u, yy + kh),
                          A(sel ? kRed : IM_COL32(40, 38, 38, 255)), 5 * u);
        char key[2] = {kKeys[r][c], 0};
        if (key[0] >= 'A' && key[0] <= 'Z' && !caps_) key[0] = char(key[0] - 'A' + 'a');
        dl->AddText(g_body, 32 * u, ImVec2(kx + (kw - Width(g_body, 32 * u, key)) * 0.5f, yy + 14 * u),
                    A(kWhite), key);
      }
    }
    const char* bottom[3] = {T("SPACE", "ESPAÇO"), T("DELETE", "APAGAR"), T("SEND", "ENVIAR")};
    const int span[3][2] = {{0, 5}, {5, 8}, {8, 10}};
    const float by = ky + 4 * (kh + 8 * u);
    for (int k = 0; k < 3; ++k) {
      const bool sel = key_row_ == 4 && BottomKey(key_col_) == k;
      const float x0 = l + span[k][0] * kw, x1 = l + span[k][1] * kw;
      dl->AddRectFilled(ImVec2(x0 + 3 * u, by), ImVec2(x1 - 3 * u, by + kh),
                        A(sel ? kRed : k == 2 ? IM_COL32(70, 20, 22, 255) : IM_COL32(40, 38, 38, 255)), 5 * u);
      dl->AddText(g_item, 30 * u, ImVec2((x0 + x1 - Width(g_item, 30 * u, bottom[k])) * 0.5f, by + 16 * u),
                  A(kWhite), bottom[k]);
    }
    dl->AddText(g_body, 22 * u, ImVec2(l, y + h - 50 * u), A(kGrey),
                T("X  TYPE      SQUARE  DELETE      TRIANGLE  SPACE      L1  CAPS      OPTIONS  SEND      O  CANCEL", "X  DIGITAR      QUADRADO  APAGAR      TRIÂNGULO  ESPAÇO      L1  MAIÚSC.      OPTIONS  ENVIAR      "
                  "O  CANCELAR"));
  }


  // T("The installs are done: delete the files they came from?", "As instalações terminaram: apagar os arquivos de onde vieram?") over the page.
  void DrawLeftoversQuestion(ImDrawList* dl, const ImVec2& size) {
    const float u = u_;
    dl->AddRectFilled(ImVec2(0, 0), size, A(IM_COL32(0, 0, 0, 170)));
    const float w = 1100 * u, x = (size.x - w) * 0.5f, y = size.y * 0.5f - 200 * u;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 400 * u), A(IM_COL32(14, 12, 12, 250)), 10 * u);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 8 * u, y + 400 * u), A(kRed));
    dl->AddText(g_item, 44 * u, ImVec2(x + 50 * u, y + 40 * u), A(kWhite), T("INSTALL COMPLETE", "INSTALAÇÃO CONCLUÍDA"));
    char line[96];
    std::snprintf(line, sizeof(line), "%.1f GB", double(leftover_bytes_) / (1024.0 * 1024.0 * 1024.0));
    const std::string text =
        std::string(T("The disc images and DLC packages it was installed from are still in "
                    "PPSA54107/install/ (", "As imagens de disco e os pacotes de DLC de onde foi instalado ainda estão em PPSA54107/install/ (")) +
        line +
        T("). The game doesn't need them any more.\n\nDelete them to free the space, or keep them (they won't "
        "be installed again).", "). O jogo não precisa mais deles.\n\nApague-os para liberar espaço, ou guarde-os (eles não serão instalados de novo).");
    dl->AddText(g_body, 28 * u, ImVec2(x + 50 * u, y + 110 * u), A(kText), text.c_str(), nullptr, w - 100 * u);
    dl->AddText(g_item, 34 * u, ImVec2(x + 50 * u, y + 320 * u), A(kWhite), T("X  DELETE", "X  APAGAR"));
    dl->AddText(g_item, 34 * u, ImVec2(x + 380 * u, y + 320 * u), A(kGrey), T("O  KEEP", "O  GUARDAR"));
  }

  ImU32 A(ImU32 c, float a = -1) const {
    if (a < 0) a = fade_;
    return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * std::clamp(a, 0.0f, 1.0f)) << 24);
  }
  ImU32 P(ImU32 c) const { return A(c, page_alpha_); }

  // The picture covering the screen with a slow drift, darkened toward the
  // menu's side, the film's grain over it: a few flickering scratches.
  void DrawBackground(ImDrawList* dl, const ImVec2& size, float dt) {
    const float u = u_;
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(8, 8, 8, 255));
    // (SETTINGS: the picture of the game whose settings they are)
    const int shown = page_ == kSettings ? set_game_ : game_;
    shown_game_ += (float(shown) - shown_game_) * std::min(1.0f, dt * 5.0f);
    for (int g = 1; g <= 2; ++g) {
      const Image& bg = background_[g];
      const float weight = g == 2 ? shown_game_ - 1.0f : 2.0f - shown_game_;
      if (!bg || weight < 0.01f) continue;
      const float s = std::max(size.x / bg.w, size.y / bg.h) * 1.06f;
      const float w = bg.w * s, h = bg.h * s;
      const float drift = std::sin(time_ * 0.05f) * (w - size.x) * 0.5f;
      const ImVec2 p0((size.x - w) * 0.5f + drift, (size.y - h) * 0.5f);
      dl->AddImage(bg.id, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                   A(IM_COL32(255, 255, 255, int(200 * weight))));
    }
    // the page's dark: the side of the menu on HOME, all of it on the others
    const float dark = page_ == kHome ? 0.0f : 0.74f;
    page_dark_ += (dark - page_dark_) * std::min(1.0f, dt * 6.0f);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x * 0.62f, size.y), A(IM_COL32(0, 0, 0, 235)),
                                A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 235)));
    dl->AddRectFilled(ImVec2(0, 0), size, A(IM_COL32(0, 0, 0, int(255 * page_dark_))));
    // top and bottom bars' shade
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, 170 * u), A(IM_COL32(0, 0, 0, 200)),
                                A(IM_COL32(0, 0, 0, 200)), A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 0)));
    dl->AddRectFilledMultiColor(ImVec2(0, size.y - 150 * u), size, A(IM_COL32(0, 0, 0, 0)), A(IM_COL32(0, 0, 0, 0)),
                                A(IM_COL32(0, 0, 0, 210)), A(IM_COL32(0, 0, 0, 210)));
    // the film's grain: a few flickering scratches
    for (int i = 0; i < 6; ++i) {
      const float r = std::fmod(std::sin(float(i) * 12.9898f + std::floor(time_ * 12.0f) * 78.233f) * 43758.5453f, 1.0f);
      const float x = std::fabs(r) * size.x;
      dl->AddLine(ImVec2(x, 0), ImVec2(x + 3 * u, size.y), A(IM_COL32(255, 255, 255, 10)), 1.0f * u);
    }
  }

  void DrawTabs(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, y = 58 * u, ts = 40 * u;
    float x = 140 * u;
    // the L1 / R1 marks either side
    auto button = [&](float bx, const char* label) {
      const float bw = Width(g_body, 20 * u, label) + 18 * u;
      dl->AddRect(ImVec2(bx, y + 9 * u), ImVec2(bx + bw, y + 37 * u), A(kGrey), 6 * u, 0, 1.5f * u);
      dl->AddText(g_body, 20 * u, ImVec2(bx + 9 * u, y + 12 * u), A(kGrey), label);
      return bw;
    };
    x += button(x, "L1") + 30 * u;
    for (int i = 0; i < kPages; ++i) {
      const float w = Width(g_item, ts, PageName(i));
      if (i == page_) {
        tab_target_x_ = x, tab_target_w_ = w;
        if (tab_w_ == 0) tab_x_ = x, tab_w_ = w;
      }
      dl->AddText(g_item, ts, ImVec2(x, y), A(i == page_ ? kWhite : kGrey), PageName(i));
      x += w + 52 * u;
    }
    button(x - 22 * u, "R1");
    // the project's name at the right
    if (title_) {
      const float th = 46 * u, tw = title_.w * th / title_.h;
      dl->AddImage(title_.id, ImVec2(size.x - 140 * u - tw, y - 2 * u), ImVec2(size.x - 140 * u, y - 2 * u + th),
                   ImVec2(0, 0), ImVec2(1, 1), A(kWhite));
    } else {
      const char* name = "SAINTS ROW  REINSTATED";
      const float nw = Width(g_body, 20 * u, name);
      dl->AddText(g_body, 20 * u, ImVec2(size.x - 140 * u - nw, y + 12 * u), A(kDim), name);
    }
    dl->AddRectFilled(ImVec2(tab_x_, y + 56 * u), ImVec2(tab_x_ + tab_w_, y + 61 * u), A(kRed));
    dl->AddRectFilled(ImVec2(140 * u, y + 61 * u), ImVec2(size.x - 140 * u, y + 62 * u), A(IM_COL32(255, 255, 255, 40)));
  }

  void DrawFooter(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, y = size.y - 92 * u;
    std::string hints;
    switch (page_) {
      case kHome: hints = T("UP DOWN  GAME      X  PLAY      L1 R1  PAGES", "CIMA BAIXO  JOGO      X  JOGAR      L1 R1  PÁGINAS"); break;
      case kContent: hints = T("UP DOWN  MOVE      X  INSTALL      TRIANGLE  UNINSTALL      O  BACK      L1 R1  PAGES", "CIMA BAIXO  MOVER      X  INSTALAR      TRIÂNGULO  DESINSTALAR      O  VOLTAR      L1 R1  PÁGINAS"); break;
      case kSaves: hints = T("UP DOWN  MOVE      X  SELECT      O  BACK      L1 R1  PAGES", "CIMA BAIXO  MOVER      X  ESCOLHER      O  VOLTAR      L1 R1  PÁGINAS"); break;
      case kSettings:
        hints = in_list_ ? T("UP DOWN  MOVE      LEFT RIGHT  CHANGE      SQUARE  RECOMMENDED      O  GAMES      L1 R1  PAGES", "CIMA BAIXO  MOVER      ESQUERDA DIREITA  MUDAR      QUADRADO  RECOMENDADO      O  JOGOS      L1 R1  "
                             "PÁGINAS")
                         : T("UP DOWN  GAME      X  SETTINGS      SQUARE  RECOMMENDED      O  BACK      L1 R1  PAGES", "CIMA BAIXO  JOGO      X  AJUSTES      QUADRADO  RECOMENDADO      O  VOLTAR      L1 R1  PÁGINAS");
        break;
      case kTrophies: hints = T("UP DOWN  MOVE      LEFT RIGHT  PAGE UP / DOWN      TRIANGLE  OTHER GAME      O  BACK", "CIMA BAIXO  MOVER      ESQUERDA DIREITA  PULAR PÁGINA      TRIÂNGULO  OUTRO JOGO      O  VOLTAR"); break;
      default: hints = T("O  BACK      L1 R1  PAGES", "O  VOLTAR      L1 R1  PÁGINAS"); break;
    }
    const float hints_w = Hints(dl, g_body, 22 * u, ImVec2(0, 0), 0, hints, false);
    // HOME leaves the picture undimmed: a shade in the corner under the hints,
    // fading out to the left and upward
    const float shade = 1.0f - std::clamp(page_dark_ / 0.74f, 0.0f, 1.0f);
    if (shade > 0.01f) {
      const float xs = size.x - 140 * u - hints_w - 40 * u, ys = size.y - 150 * u, f = 160 * u;
      const ImU32 c = A(IM_COL32(0, 0, 0, int(200 * shade))), z = A(IM_COL32(0, 0, 0, 0));
      dl->AddRectFilled(ImVec2(xs, ys), size, c);
      dl->AddRectFilledMultiColor(ImVec2(xs, ys - f), ImVec2(size.x, ys), z, z, c, c);
      dl->AddRectFilledMultiColor(ImVec2(xs - f, ys), ImVec2(xs, size.y), z, c, c, z);
      dl->AddRectFilledMultiColor(ImVec2(xs - f, ys - f), ImVec2(xs, ys), z, z, c, z);
    }
    dl->AddRectFilled(ImVec2(140 * u, y - 18 * u), ImVec2(size.x - 140 * u, y - 17 * u), A(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 22 * u, ImVec2(140 * u, y), A(kRed),
                Upper(Date(int64_t(std::time(nullptr)), T("%a / %b %d / %I:%M %p", "%a / %d %b / %H:%M"))).c_str());
    Hints(dl, g_body, 22 * u, ImVec2(size.x - 140 * u - hints_w, y), A(kRed), hints, true);
  }

  void DrawHome(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 200 * u;
    // the two games: the chosen one's logo large with PLAY, the other's small
    for (int g = 1; g <= 2; ++g) {
      const bool sel = g == game_;
      const Image& logo = logo_[g];
      const float h = (sel ? 104 : 54) * u;
      if (sel) {
        const float pulse = 0.7f + 0.3f * std::sin(time_ * 5.0f);
        dl->AddRectFilled(ImVec2(left - 38 * u, y + h * 0.12f), ImVec2(left - 24 * u, y + h * 0.88f),
                          P((kRed & 0x00FFFFFFu) | (ImU32(255 * pulse) << 24)));
      }
      if (logo) {
        const float w = logo.w * h / logo.h;
        dl->AddImage(logo.id, ImVec2(left, y), ImVec2(left + w, y + h), ImVec2(0, 0), ImVec2(1, 1),
                     P(sel ? kWhite : IM_COL32(150, 146, 140, 200)));
      } else {
        dl->AddText(g_title, h, ImVec2(left, y - h * 0.1f), P(sel ? kWhite : kGrey), kGameTitles[g]);
      }
      y += h + (sel ? 14 : 26) * u;
      if (!sel) continue;
      const bool installing = install::TheJob().Busy();
      const float ps = 72 * u;
      dl->AddText(g_item, ps, ImVec2(left, y), P(Ready() && !installing ? kWhite : kDim), T("PLAY", "JOGAR"));
      y += ps + 20 * u;
      if (!start_error_.empty()) {
        const float pw = 900 * u, th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw, start_error_.c_str()).y;
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + pw + 16 * u, y + th + 12 * u),
                          P(IM_COL32(40, 6, 6, 230)), 6 * u);
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), start_error_.c_str(), nullptr, pw);
        y += th + 36 * u;
      }
      if (installing) {
        y = DrawJob(dl, left, y, 900 * u) + 20 * u;
      } else if (!Ready()) {
        // what's missing, and where it goes
        missing_flash_ = std::max(0.0f, missing_flash_ - 0.03f);
        const char* what =
            installed_.game[g]
                ? (g == 1 ? T("SAINTS ROW NEEDS TITLE UPDATE 1\nPut Saints Row's Title Update 1 package (TU_...) in "
                            "PPSA54107/install/ and open the app again: it installs by itself.", "SAINTS ROW PRECISA DO TITLE UPDATE 1\nColoque o pacote do Title Update 1 de Saints Row (TU_...) em "
                              "PPSA54107/install/ e abra o app de novo: ele se instala sozinho.")
                          : T("SAINTS ROW 2 NEEDS TITLE UPDATE 3\nPut Saints Row 2's Title Update 3 package (TU_...) in "
                            "PPSA54107/install/ and open the app again: it installs by itself.", "SAINTS ROW 2 PRECISA DO TITLE UPDATE 3\nColoque o pacote do Title Update 3 de Saints Row 2 (TU_...) "
                              "em PPSA54107/install/ e abra o app de novo: ele se instala sozinho."))
            : g == 1 ? T("SAINTS ROW ISN'T INSTALLED\nPut your Saints Row disc image (.iso, any name) or the disc's files, "
                       "and Title Update 1's package, in PPSA54107/install/ and open the app again: they install by "
                       "themselves. The CONTENT page shows what's there.", "SAINTS ROW NÃO ESTÁ INSTALADO\nColoque a imagem do seu disco de Saints Row (.iso, qualquer nome) ou "
                         "os arquivos do disco, e o pacote do Title Update 1, em PPSA54107/install/ e abra o app de novo: "
                         "eles se instalam sozinhos. A página CONTEÚDO mostra o que está lá.")
                     : T("SAINTS ROW 2 ISN'T INSTALLED\nPut your Saints Row 2 disc image (.iso, any name) or the disc's "
                       "files, and Title Update 3's package, in PPSA54107/install/ and open the app again: they "
                       "install by themselves. The CONTENT page shows what's there.", "SAINTS ROW 2 NÃO ESTÁ INSTALADO\nColoque a imagem do seu disco de Saints Row 2 (.iso, qualquer nome) "
                         "ou os arquivos do disco, e o pacote do Title Update 3, em PPSA54107/install/ e abra o app de novo: "
                         "eles se instalam sozinhos. A página CONTEÚDO mostra o que está lá.");
        const float pw = 900 * u, th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw, what).y;
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + pw + 16 * u, y + th + 12 * u),
                          P(IM_COL32(40, 6, 6, ImU32(200 + 55 * missing_flash_))), 6 * u);
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 10 * u, y + th + 12 * u), P(kRed));
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), what, nullptr, pw);
        y += th + 42 * u;
      } else {
        std::string where = T("Your saves load with the game.", "Seus saves carregam com o jogo.");
        if (installed_.dlc)
          where = (installed_.dlc == 1 ? std::string(T("1 DLC pack", "1 pacote de DLC")) : std::to_string(installed_.dlc) + T(" DLC packs", " pacotes de DLC")) +
                  T(" and your saves load with the game.", " e seus saves carregam com o jogo.");
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kGrey), where.c_str());
        y += 54 * u;
      }
      const trophies::List& list = lists_[g];
      if (list.loaded) {
        // (on a dark panel: the picture behind it is busy)
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + 560 * u, y + 48 * u),
                          P(IM_COL32(8, 8, 8, 185)), 6 * u);
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 11 * u, y + 48 * u), P(kRed));
        trophies::DrawCup(dl, ImVec2(left + 14 * u, y + 16 * u), 30 * u,
                          list.Platinum() ? trophies::Grade::kPlatinum : trophies::Grade::kGold, page_alpha_,
                          list.earned > 0);
        char line[96];
        std::snprintf(line, sizeof(line), "%d of %d trophies  -  %d%%", list.earned, list.earnable, list.Percent());
        dl->AddText(g_body, 26 * u, ImVec2(left + 40 * u, y), P(kWhite), line);
        y += 76 * u;
      }
      y += 30 * u;
    }
  }

  // The running job's title and progress bar, or the last one's outcome for a while; the y under it.
  float DrawJob(ImDrawList* dl, float x, float y, float w) {
    const float u = u_;
    auto& job = install::TheJob();
    if (job.Busy()) {
      const float pr = job.Progress();
      char line[64];
      std::snprintf(line, sizeof(line), "%d%%", int(pr * 100));
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x + w + 16 * u, y + 78 * u), P(IM_COL32(8, 8, 8, 200)),
                        6 * u);
      // (a long disc image name cut short of the percentage)
      const float pw = Width(g_body, 26 * u, line);
      dl->AddText(g_body, 26 * u, ImVec2(x, y), P(kWhite),
                  Fit(g_body, 26 * u, Upper(job.Title()), w - pw - 30 * u).c_str());
      dl->AddText(g_body, 26 * u, ImVec2(x + w - pw, y), P(kRed), line);
      dl->AddRectFilled(ImVec2(x, y + 46 * u), ImVec2(x + w, y + 54 * u), P(kDim));
      dl->AddRectFilled(ImVec2(x, y + 46 * u), ImVec2(x + w * pr, y + 54 * u), P(kRed));
      return y + 90 * u;
    }
    const std::string result = job.Result();
    if (!result.empty() && time_ - result_t_ < 12.0f) {
      const float th = g_body->CalcTextSizeA(26 * u, FLT_MAX, w, result.c_str()).y;
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x + w + 16 * u, y + th + 12 * u),
                        P(IM_COL32(8, 8, 8, 200)), 6 * u);
      dl->AddRectFilled(ImVec2(x - 16 * u, y - 12 * u), ImVec2(x - 10 * u, y + th + 12 * u),
                        P(job.Failed() ? IM_COL32(230, 50, 40, 255) : kWhite));
      dl->AddText(g_body, 26 * u, ImVec2(x, y), P(kWhite), result.c_str(), nullptr, w);
      return y + th + 30 * u;
    }
    return y;
  }

  static std::string Short(std::string s, size_t n) {
    if (s.size() > n) s = s.substr(0, n - 3) + "...";
    return s;
  }

  void DrawContent(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 200 * u;
    const std::string dlc_name =
        installed_.dlc ? "DLC  (" + std::to_string(installed_.dlc) + (installed_.dlc == 1 ? T(" PACK)", " PACOTE)") : T(" PACKS)", " PACOTES)")) : "DLC";
    const std::string names[kContentRows] = {"SAINTS ROW", "SAINTS ROW 2", dlc_name};
    const char* help[kContentRows] = {
        T("Saints Row's disc and its Title Update 1 (both needed). Put your disc image (.iso, any name) or a "
        "folder of the disc's files, and the title update's package (TU_...), in PPSA54107/install/: they install "
        "into PPSA54107/game1/ when the app opens, or with X here.", "O disco de Saints Row e o seu Title Update 1 (os dois são necessários). Coloque a imagem do disco "
          "(.iso, qualquer nome) ou uma pasta com os arquivos do disco, e o pacote do title update (TU_...), "
          "em PPSA54107/install/: eles se instalam em PPSA54107/game1/ quando o app abre, ou com X aqui."),
        T("Saints Row 2's disc and its Title Update 3 (both needed). Put your disc image (.iso, any name) or a "
        "folder of the disc's files, and the title update's package (TU_...), in PPSA54107/install/: they install "
        "into PPSA54107/game2/ when the app opens, or with X here.", "O disco de Saints Row 2 e o seu Title Update 3 (os dois são necessários). Coloque a imagem do disco "
          "(.iso, qualquer nome) ou uma pasta com os arquivos do disco, e o pacote do title update (TU_...), "
          "em PPSA54107/install/: eles se instalam em PPSA54107/game2/ quando o app abre, ou com X aqui."),
        T("Optional: either game's DLC (Saints Row's Money Shot and clothing packs; Saints Row 2's Ultor Exposed, "
        "Corporate Warfare and the Unkut pack). Put their packages (any names) in PPSA54107/install/: each "
        "installs by itself. TRIANGLE removes them all.", "Opcional: as DLCs de qualquer um dos jogos (Money Shot e os pacotes de roupas de Saints Row; Ultor "
          "Exposed, Corporate Warfare e o pacote Unkut de Saints Row 2). Coloque os pacotes (qualquer nome) em "
          "PPSA54107/install/: cada um se instala sozinho. TRIÂNGULO remove todos.")};
    for (int r = 0; r < kContentRows; ++r) {
      const bool sel = r == content_row_, in = RowInstalled(r);
      const install::Source* src = SourceFor(r);
      const float fs = 40 * u;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 8 * u), ImVec2(left - 20 * u, y + fs), P(kRed));
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), names[r].c_str());
      std::string status = in && !(r == kRowDlc && src) ? T("INSTALLED", "INSTALADO")
                           : src                        ? T("READY TO INSTALL", "PRONTO PARA INSTALAR")
                                                        : T("NOT INSTALLED", "NÃO INSTALADO");
      // (a disc without the title update the program is built from)
      if ((r == kRowGame1 || r == kRowGame2) && !src && installed_.game[r + 1] && !installed_.update[r + 1])
        status = r == kRowGame1 ? T("NEEDS TITLE UPDATE 1", "PRECISA DO TITLE UPDATE 1") : T("NEEDS TITLE UPDATE 3", "PRECISA DO TITLE UPDATE 3");
      if (r == kRowDlc && src) status = std::to_string(DlcWaiting()) + T(" READY TO INSTALL", " PRONTOS PARA INSTALAR");
      if (sel && Confirming(100 + r)) status = T("PRESS TRIANGLE AGAIN TO UNINSTALL", "TRIÂNGULO DE NOVO: DESINSTALAR");
      dl->AddText(g_body, 28 * u, ImVec2(left + 430 * u, y + 8 * u),
                  P(in ? kWhite : src ? kWhite : kGrey),
                  status.c_str());
      y += fs + 6 * u;
      if (src && !in) {
        dl->AddText(g_body, 22 * u, ImVec2(left + 430 * u, y), P(kGrey), Short(src->label, 44).c_str());
        y += 30 * u;
      }
      y += 18 * u;
    }
    y += 10 * u;
    y = DrawJob(dl, left, y, 820 * u) + 10 * u;
    // the rest of install/: what can't be installed, and why
    std::vector<std::string> notes;
    int avatar_items = 0;
    for (const auto& s : sources_) {
      if (s.kind == install::Kind::kArchive)
        notes.push_back(Short(s.label, 40) + T("  -  extract it on a PC first (7-Zip)", "  -  extraia num PC antes (7-Zip)"));
      else if (s.kind == install::Kind::kOther && s.label == install::kAvatarItemLabel)
        ++avatar_items;
      else if (s.kind == install::Kind::kOther)
        notes.push_back(Short(s.label, 60));
    }
    if (avatar_items)
      notes.push_back(std::to_string(avatar_items) + (avatar_items == 1 ? T(" Xbox 360 avatar item", " item de avatar do Xbox 360") : T(" Xbox 360 avatar items", " itens de avatar do Xbox 360")) +
                      T("  -  not game DLC, not used here", "  -  não são DLC do jogo, não usados aqui"));
    if (!notes.empty()) {
      dl->AddText(g_body, 20 * u, ImVec2(left, y), P(kRed), T("ALSO IN PPSA54107/INSTALL", "TAMBÉM EM PPSA54107/INSTALL"));
      y += 34 * u;
      for (size_t i = 0; i < notes.size() && i < 5; ++i, y += 32 * u)
        dl->AddText(g_body, 24 * u, ImVec2(left, y), P(kGrey), notes[i].c_str());
    }
    // the focused row's help, at the right
    const float px = 1180 * u + page_dx_, pw = size.x - 140 * u - px;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 26 * u, ImVec2(px, 204 * u), P(kText), help[content_row_], nullptr, pw);
  }

  void DrawSaves(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    float y = 200 * u;
    char count[128];
    std::snprintf(count, sizeof(count), "SAINTS ROW  %d %s      SAINTS ROW 2  %d %s", installed_.saves[1],
                  installed_.saves[1] == 1 ? "SAVE" : "SAVES", installed_.saves[2],
                  installed_.saves[2] == 1 ? "SAVE" : "SAVES");
    dl->AddText(g_body, 22 * u, ImVec2(left, y), P(kRed), count);
    y += 48 * u;
    const auto saves = SaveSources();
    const int rows = kSavesFirst + int(saves.size());
    // (an imported save's file deleted after the install: the list shorter under the selected row)
    saves_row_ = std::min(saves_row_, rows - 1);
    for (int r = 0; r < rows; ++r) {
      const bool sel = r == saves_row_;
      const float fs = 36 * u;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 6 * u), ImVec2(left - 20 * u, y + fs), P(kRed));
      std::string label;
      if (r == kBackupRow) {
        label = T("BACK UP SAVES", "FAZER BACKUP DOS SAVES");
      } else if (r == kSendLogsRow) {
        label = T("SEND LOGS", "ENVIAR LOGS");
      } else {
        const install::Source* s = saves[size_t(r - kSavesFirst)];
        label = (s->kind == install::Kind::kBackup ? T("RESTORE  ", "RESTAURAR  ") : T("IMPORT  ", "IMPORTAR  ")) + Short(s->label, 40);
        if (sel && Confirming(200 + r)) label = T("PRESS X AGAIN TO CONFIRM", "APERTE X DE NOVO PARA CONFIRMAR");
      }
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), label.c_str());
      y += fs + 22 * u;
      // (the link the logs went to, kept on screen to be copied down)
      if (r == kSendLogsRow) {
        const std::string link = install::TheJob().LogsLink();
        if (!link.empty()) {
          // (in the body font: the link has small letters, and they count)
          dl->AddText(g_body, 30 * u, ImVec2(left, y - 12 * u), P(kRed), "LINK  ");
          dl->AddText(g_body, 30 * u, ImVec2(left + Width(g_body, 30 * u, "LINK  "), y - 12 * u), P(kWhite),
                      link.c_str());
          y += 40 * u;
        }
      }
    }
    y += 10 * u;
    y = DrawJob(dl, left, y, 820 * u) + 10 * u;
    // the saves the app has, newest first
    if (!installed_.save_list.empty()) {
      Spaced(dl, g_body, 19 * u, ImVec2(left, y), P(kRed), T("YOUR SAVES", "SEUS SAVES"), 3 * u);
      y += 36 * u;
      for (size_t i = 0; i < installed_.save_list.size() && i < 6; ++i, y += 34 * u) {
        const auto& sv = installed_.save_list[i];
        std::string name = sv.name;
        std::replace(name.begin(), name.end(), '_', ' ');
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite),
                    Short((sv.game == 2 ? "P2  " : "P1  ") + name, 34).c_str());
        if (sv.modified)
          dl->AddText(g_body, 26 * u, ImVec2(left + 520 * u, y), P(kGrey),
                      Upper(Date(sv.modified, T("%b %d %Y  %I:%M %p", "%d %b %Y  %H:%M"))).c_str());
      }
    }
    const char* help =
        saves_row_ == kBackupRow
            ? T("Copies your saves, your trophies and your settings to PPSA54107/install/backups/. Copy that folder "
              "to a PC or a USB drive before deleting or reinstalling the app.", "Copia seus saves, seus troféus e seus ajustes para PPSA54107/install/backups/. Copie essa pasta "
                "para um PC ou um pendrive antes de apagar ou reinstalar o app.")
        : saves_row_ == kSendLogsRow
            ? T("For a bug report: type what went wrong, and the app's logs (PPSA54107/logs/, the last few runs) "
                "go to catbox.moe. A link and a QR code come back: the QR code opens a GitHub issue with it all "
                "filled in. Anyone with the link can read the logs; they say how the game ran, not who you are. "
                "Needs the internet and a payload loader (etaHEN or elfldr).",
                "Para relatar um bug: digite o que deu errado, e os logs do app (PPSA54107/logs/, as últimas "
                "execuções) vão para o catbox.moe. Voltam um link e um QR code: o QR code abre uma issue no GitHub "
                "com tudo preenchido. Qualquer um com o link pode ler os logs; eles dizem como o jogo rodou, não "
                "quem você é. Precisa de internet e de um carregador de payload (etaHEN ou elfldr).")
        : saves[size_t(saves_row_ - kSavesFirst)]->kind == install::Kind::kBackup
            ? T("Puts this backup's saves, trophies and settings back, over the ones here.", "Devolve os saves, troféus e ajustes deste backup, por cima dos que estão aqui.")
            : T("Imports this Xbox 360 save (a package copied off a 360, with a USB drive or Horizon) into the app. "
              "A save of the same name here is replaced.", "Importa este save do Xbox 360 (um pacote copiado de um 360, com um pendrive ou o Horizon) para o "
                "app. Um save com o mesmo nome aqui é substituído.");
    const char* where =
        T("\n\nTo import a save, put the Xbox 360 save package (the file copied off the 360's drive: "
        "Content/<profile>/4156084E/00000001/... for Saints Row, .../415608A7/00000001/... for Saints Row 2) in "
        "PPSA54107/install/. A backup made here, copied back into "
        "PPSA54107/install/ (or install/backups/), is listed to restore.", "\n\nPara importar um save, coloque o pacote de save do Xbox 360 (o arquivo copiado do disco do 360: "
          "Content/<perfil>/4156084E/00000001/... para Saints Row, .../415608A7/00000001/... para Saints Row "
          "2) em PPSA54107/install/. Um backup feito aqui, copiado de volta para PPSA54107/install/ (ou "
          "install/backups/), aparece para restaurar.");
    const std::string text = std::string(help) + (saves.empty() ? where : "");
    const float px = 1180 * u + page_dx_, pw = size.x - 140 * u - px;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    // the logs sent: the QR code of the GitHub issue in place of the help
    const std::string link = install::TheJob().LogsLink();
    UpdateQr(link);
    if (saves_row_ == kSendLogsRow && qr_size_) {
      dl->AddText(g_item, 34 * u, ImVec2(px, 204 * u), P(kWhite), T("SCAN TO REPORT IT", "ESCANEIE PARA RELATAR"));
      dl->AddText(g_body, 24 * u, ImVec2(px, 252 * u), P(kText),
                  T("With your phone's camera: a GitHub issue opens with what you typed, the logs' link and your "
                    "settings filled in. Press Submit (a GitHub account is needed). Or send the link above with "
                    "your report.",
                    "Com a câmera do celular: abre uma issue no GitHub com o que você digitou, o link dos logs e seus "
                    "ajustes preenchidos. Aperte Submit (precisa de uma conta no GitHub). Ou envie o link acima com o "
                    "seu relato."),
                  nullptr, pw);
      DrawQr(dl, px, 380 * u, std::min(pw, 560 * u));
      return;
    }
    dl->AddText(g_body, 26 * u, ImVec2(px, 204 * u), P(kText), text.c_str(), nullptr, pw);
  }

  // SETTINGS' left side: the games, as on HOME (the chosen one large with the accent's bar, pulsing
  // while the list has the pad), each with its own settings at the right.
  void DrawSettingsGames(ImDrawList* dl, float left, float top) {
    const float u = u_;
    Spaced(dl, g_body, 19 * u, ImVec2(left, top), P(kRed), T("GAMES", "JOGOS"), 3 * u);
    float y = top + 46 * u;
    for (int g = 1; g <= 2; ++g) {
      const bool sel = g == set_game_;
      float h = (sel ? 53 : 37) * u;
      if (logo_[g]) h = std::min(h, (sel ? 330 : 260) * u * logo_[g].h / logo_[g].w);  // (a wide logo kept to the column)
      if (sel) {
        const float pulse = in_list_ ? 1.0f : 0.7f + 0.3f * std::sin(time_ * 5.0f);
        dl->AddRectFilled(ImVec2(left - 38 * u, y + h * 0.12f), ImVec2(left - 24 * u, y + h * 0.88f),
                          P((kRed & 0x00FFFFFFu) | (ImU32(255 * pulse) << 24)));
      }
      if (const Image& logo = logo_[g]) {
        dl->AddImage(logo.id, ImVec2(left, y), ImVec2(left + logo.w * h / logo.h, y + h), ImVec2(0, 0), ImVec2(1, 1),
                     P(sel ? kWhite : IM_COL32(150, 146, 140, 200)));
      } else {
        dl->AddText(g_item, h * 0.7f, ImVec2(left, y), P(sel ? kWhite : kGrey), kGameTitles[g]);
      }
      y += h + 26 * u;
    }
  }

  void DrawSettings(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 580 * u + page_dx_;
    const auto lines = SettingsLines();
    const float fs = 36 * u, top = 190 * u, bottom = size.y - 120 * u;
    const float px = 1360 * u + page_dx_, pw = size.x - 140 * u - px;  // (the help, at the right)
    DrawSettingsGames(dl, 140 * u + page_dx_, top);
    dl->AddRectFilled(ImVec2(left - 60 * u, 190 * u), ImVec2(left - 59 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    // where each row and section heading goes (scrolled so the focused row stays in view)
    std::vector<float> row_y(kSettingsRows, 0.0f), head_y(kSettingsRows, -1.0f);
    float y = 0;
    for (int r = 0; r < kSettingsRows; ++r) {
      if (!r || std::strcmp(lines[size_t(r - 1)].section, lines[size_t(r)].section) != 0) {
        if (r) y += 14 * u;
        head_y[size_t(r)] = y;
        y += 36 * u;
      }
      row_y[size_t(r)] = y;
      y += fs + 16 * u;
    }
    const float view = bottom - top;
    const float want = std::clamp(row_y[size_t(row_)] - view * 0.45f, 0.0f, std::max(0.0f, y - view));
    settings_scroll_ += (want - settings_scroll_) * 0.25f;
    dl->PushClipRect(ImVec2(0, top - 4 * u), ImVec2(px - 40 * u, bottom), true);
    for (int r = 0; r < kSettingsRows; ++r) {
      const SettingLine& l = lines[size_t(r)];
      if (head_y[size_t(r)] >= 0)
        Spaced(dl, g_body, 19 * u, ImVec2(left, top + head_y[size_t(r)] - settings_scroll_), P(kRed), l.section,
               3 * u);
      y = top + row_y[size_t(r)] - settings_scroll_;
      const bool sel = in_list_ && r == row_;
      const ImU32 col = !l.enabled ? kDim : sel ? kWhite : kGrey;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 8 * u), ImVec2(left - 20 * u, y + fs), P(kRed));
      dl->AddText(g_item, fs, ImVec2(left, y), P(col), l.label);
      const std::string v = l.slider || !l.enabled ? l.value : "<  " + l.value + "  >";
      dl->AddText(g_item, fs, ImVec2(left + 380 * u, y), P(sel ? (l.enabled ? kRed : kDim) : col), v.c_str());
      if (l.slider) {
        const float x0 = left + 480 * u, w = 220 * u, yy = y + fs * 0.6f;
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w, yy + 3 * u), P(kDim));
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w * sets_[1].music / 100.0f, yy + 3 * u),
                          P(sel ? kRed : kGrey));
      }
    }
    dl->PopClipRect();
    // the focused row's help, at the right (in the game list: what the list is for)
    dl->AddRectFilled(ImVec2(px - 40 * u, 190 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    const std::string name = in_list_ ? lines[size_t(row_)].label : kGameTitles[set_game_];
    const std::string help =
        in_list_ ? lines[size_t(row_)].help
                 : std::string(set_game_ == 1 ? "Saints Row" : "Saints Row 2") +
                       T("'s own settings: each game keeps its own. X or RIGHT to change them. SQUARE puts back the "
                       "recommended settings, the ones it was tested with.", ": os ajustes deste jogo, cada jogo tem os seus. X ou DIREITA para mudá-los. QUADRADO volta aos "
                         "ajustes recomendados, os que foram testados.");
    dl->AddText(g_item, 34 * u, ImVec2(px, 190 * u), P(kWhite), name.c_str());
    dl->AddText(g_body, 26 * u, ImVec2(px, 244 * u), P(kText), help.c_str(), nullptr, pw);
    if (Confirming(300 + set_game_))
      dl->AddText(g_body, 22 * u, ImVec2(px, 676 * u), P(kRed), T("PRESS SQUARE AGAIN TO PUT BACK THE RECOMMENDED SETTINGS", "APERTE QUADRADO DE NOVO PARA VOLTAR ÀS CONFIGURAÇÕES RECOMENDADAS"),
                  nullptr, pw);
    else if (time_ - reset_t_ < 3.0f)
      dl->AddText(g_body, 22 * u, ImVec2(px, 700 * u), P(kRed), T("RECOMMENDED SETTINGS PUT BACK", "CONFIGURAÇÕES RECOMENDADAS RESTAURADAS"));
    else
      dl->AddText(g_body, 22 * u, ImVec2(px, 700 * u), P(kDim), T("Saved when you press PLAY.", "Salvo quando você aperta JOGAR."));
  }

  void DrawTrophies(ImDrawList* dl, const ImVec2& size, float dt) {
    const float u = u_;
    // which game's, at the top right (TRIANGLE: the other's)
    {
      const std::string which = std::string(kGameTitles[trophy_game_]) + T("  /  TRIANGLE: ", "  /  TRIÂNGULO: ") + kGameTitles[3 - trophy_game_];
      dl->AddText(g_body, 22 * u, ImVec2(size.x - 140 * u - Width(g_body, 22 * u, which), 140 * u), P(kRed),
                  which.c_str());
    }
    if (!trophies_.loaded) {
      const std::string start = std::string(T("START ", "INICIE ")) + kGameTitles[trophy_game_] + T(" ONCE", " UMA VEZ");
      dl->AddText(g_item, 44 * u, ImVec2(140 * u + page_dx_, 220 * u), P(kWhite), start.c_str());
      dl->AddText(g_body, 28 * u, ImVec2(140 * u + page_dx_, 290 * u), P(kGrey),
                  T("The trophy list comes from the game itself: start it once, and the next time the menu opens "
                  "your trophies are here, with the ones you've earned.", "A lista de troféus vem do próprio jogo: inicie-o uma vez, e da próxima vez que o menu abrir seus "
                    "troféus estarão aqui, com os que você já conquistou."),
                  nullptr, 1000 * u);
      return;
    }
    const int n = int(items_.size());
    if (!n) return;
    trophy_ = std::clamp(trophy_, 0, n - 1);
    detail_in_ = std::min(1.0f, detail_in_ + dt * 6.0f);

    // the list at the left: group headings, then each trophy
    struct Row {
      int item;  // -1: a heading
      int group;
    };
    std::vector<Row> rows;
    int counts[3] = {};
    for (const Item& it : items_) ++counts[it.group];
    int sel_row = 0;
    for (int i = 0; i < n; ++i) {
      if (i == 0 || items_[size_t(i) - 1].group != items_[size_t(i)].group) rows.push_back({-1, items_[size_t(i)].group});
      if (i == trophy_) sel_row = int(rows.size());
      rows.push_back({i, items_[size_t(i)].group});
    }
    const float row_h = 64 * u, top = 172 * u, bottom = size.y - 130 * u;
    const float view = bottom - top;
    const float want = std::clamp(sel_row * row_h - view * 0.4f, 0.0f, std::max(0.0f, rows.size() * row_h - view));
    scroll_ += (want - scroll_) * std::min(1.0f, dt * 12.0f);
    const float x = 140 * u + page_dx_, right = 940 * u + page_dx_;
    static const char* const kGroupNames[] = {T("EARNED", "CONQUISTADOS"), T("NOT EARNED YET", "AINDA NÃO CONQUISTADOS"), T("ONLINE ONLY", "SÓ ONLINE")};
    dl->PushClipRect(ImVec2(0, top), ImVec2(size.x, bottom), true);
    for (size_t r = 0; r < rows.size(); ++r) {
      const float ry = top + r * row_h - scroll_;
      if (ry + row_h < top || ry > bottom) continue;
      if (rows[r].item < 0) {
        const std::string head = std::string(kGroupNames[rows[r].group]) + "   " + std::to_string(counts[rows[r].group]);
        dl->AddText(g_body, 21 * u, ImVec2(x, ry + 30 * u), P(kRed), head.c_str());
        continue;
      }
      const Item& it = items_[size_t(rows[r].item)];
      const bool sel = rows[r].item == trophy_, earned = Earned(it);
      if (sel) dl->AddRectFilled(ImVec2(x - 16 * u, ry + 4 * u), ImVec2(right + 10 * u, ry + row_h - 4 * u),
                                 P(IM_COL32(196, 22, 28, 52)), 6 * u);
      Icon(dl, it, x, ry + 8 * u, 48 * u, page_alpha_);
      const ImU32 col = sel ? kWhite : earned ? IM_COL32(210, 206, 200, 255) : kGrey;
      dl->AddText(g_item, 28 * u, ImVec2(x + 70 * u, ry + 13 * u), P(col),
                  Fit(g_item, 28 * u, NameOf(it), right - x - 130 * u).c_str());
      trophies::DrawCup(dl, ImVec2(right - 18 * u, ry + row_h * 0.5f), 30 * u, GradeOf(it), page_alpha_, earned);
    }
    dl->PopClipRect();

    // how far along, then the focused trophy
    const float px = 1040 * u + page_dx_, pw = size.x - 140 * u - px;
    float py = 180 * u;
    dl->AddRectFilled(ImVec2(px - 50 * u, top), ImVec2(px - 49 * u, bottom), P(IM_COL32(255, 255, 255, 40)));
    const std::string pct = std::to_string(trophies_.Percent()) + "%";
    dl->AddText(g_title, 72 * u, ImVec2(px, py - 10 * u), P(kWhite), pct.c_str());
    const float pctw = Width(g_title, 72 * u, pct);
    const std::string of = std::to_string(trophies_.earned) + T(" of ", " de ") + std::to_string(trophies_.earnable) + T(" earned", " conquistados");
    dl->AddText(g_body, 26 * u, ImVec2(px + pctw + 24 * u, py + 34 * u), P(kGrey), of.c_str());
    float rx = px + pw;
    for (int g = 0; g < 3; ++g) {  // (bronze at the right, gold at the left)
      const std::string c = std::to_string(trophies_.grade_earned[g]) + "/" + std::to_string(trophies_.grade_total[g]);
      const float cw = Width(g_body, 24 * u, c);
      dl->AddText(g_body, 24 * u, ImVec2(rx - cw, py + 34 * u), P(trophies_.grade_earned[g] ? kWhite : kDim), c.c_str());
      trophies::DrawCup(dl, ImVec2(rx - cw - 20 * u, py + 48 * u), 26 * u, trophies::Grade(g), page_alpha_,
                        trophies_.grade_earned[g] > 0);
      rx -= cw + 60 * u;
    }
    py += 92 * u;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 8 * u), P(IM_COL32(255, 255, 255, 30)));
    const float f = trophies_.points ? float(trophies_.points_earned) / float(trophies_.points) : 0.0f;
    if (f > 0) dl->AddRectFilled(ImVec2(px, py), ImVec2(px + std::max(4 * u, pw * f), py + 8 * u),
                                 P(trophies_.Platinum() ? trophies::GradeColor(trophies::Grade::kPlatinum) : kRed));
    py += 54 * u;

    const Item& it = items_[size_t(trophy_)];
    const float da = page_alpha_ * detail_in_;
    const float dx = px + (1 - detail_in_) * 14 * u;
    Icon(dl, it, dx, py, 112 * u, da);
    const trophies::Grade g = GradeOf(it);
    const std::string grade = Portuguese() ? Upper(std::string("Troféu de ") + TW(trophies::GradeName(g)))
                                            : Upper(std::string(trophies::GradeName(g)) + T(" trophy", ""));
    dl->AddText(g_body, 20 * u, ImVec2(dx + 136 * u, py + 8 * u), trophies::GradeColor(g, da), grade.c_str());
    dl->AddText(g_item, 36 * u, ImVec2(dx + 136 * u, py + 34 * u), A(kWhite, da), NameOf(it).c_str(), nullptr,
                pw - 136 * u);
    py += 138 * u;
    const std::string text = TextOf(it);
    dl->AddText(g_body, 27 * u, ImVec2(dx, py), A(kText, da), text.c_str(), nullptr, pw);
    py += g_body->CalcTextSizeA(27 * u, FLT_MAX, pw, text.c_str()).y + 34 * u;
    const trophies::Trophy* t = it.index < 0 ? nullptr : &trophies_.trophies[size_t(it.index)];
    std::string status;
    if (Earned(it)) status = T("Earned ", "Conquistado em ") + Date(t ? t->earned : trophies_.last_earned, T("%b %d, %Y", "%d de %b de %Y"));
    else if (!t) status = std::to_string(trophies_.earnable - trophies_.earned) + T(" to go", " restantes");
    else if (t->online()) status = T("Not available offline", "Indisponível offline");
    else status = T("Not earned yet", "Ainda não conquistado");
    dl->AddText(g_body, 20 * u, ImVec2(dx, py), A(kDim, da), "STATUS");
    dl->AddText(g_body, 24 * u, ImVec2(dx + 170 * u, py - 2 * u), A(kWhite, da), status.c_str());
    if (t) {
      py += 40 * u;
      dl->AddText(g_body, 20 * u, ImVec2(dx, py), A(kDim, da), "GAMERSCORE");
      dl->AddText(g_body, 24 * u, ImVec2(dx + 170 * u, py - 2 * u), A(kWhite, da),
                  (std::to_string(t->gamerscore) + "G").c_str());
    }
  }

  void DrawCredits(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, x = 140 * u + page_dx_, w = 860 * u;
    float y = 180 * u;
    dl->PushClipRect(ImVec2(0, 0), ImVec2(size.x, size.y - 120 * u), true);  // (not into the clock)
    auto entry = [&](const char* name, const char* text) {
      dl->AddText(g_item, 26 * u, ImVec2(x, y), P(kWhite), name);
      y += 34 * u;
      dl->AddText(g_body, 20 * u, ImVec2(x, y), P(kGrey), text, nullptr, w);
      y += g_body->CalcTextSizeA(20 * u, FLT_MAX, w, text).y + 20 * u;
    };
    // the project
    dl->AddText(g_title, 60 * u, ImVec2(x, y - 6 * u), P(kWhite), "SAINTS ROW");
    const float tw = Width(g_title, 60 * u, "SAINTS ROW");
    Spaced(dl, g_item, 30 * u, ImVec2(x + tw + 22 * u, y + 24 * u), P(kRed), "REINSTATED", 6 * u);
    y += 84 * u;
    entry(T("BY CHAOSTHEORY1075", "POR CHAOSTHEORY1075"), "github.com/ChaosTheory1075/sr-ps5");
    dl->AddText(g_body, 20 * u, ImVec2(x, y), P(kRed), T("ALL THE CREDIT GOES TO", "TODO O CRÉDITO VAI PARA"));
    y += 40 * u;
    entry("GTA IV FOR PS5  -  ELRIPALDA",
          T("The PS5 port this one is built on, top to bottom: the PS5 runtime, graphics and sound, the hub this menu "
          "copies, the trophies and their pop-up, and the installer payload. github.com/elripalda/gta4-ps5", "O port de PS5 em que este se baseia, de cima a baixo: o runtime do PS5, gráficos e som, o hub que "
            "este menu copia, os troféus e o aviso deles, e o payload do instalador. "
            "github.com/elripalda/gta4-ps5"));
    entry("LIBERTY RECOMPILED  -  OZORDI  /  VADUUR",
          T("OZORDI's recompilation of GTA IV and vaduur's LibertyRecompAndroid, which GTA IV for PS5 starts from.", "A recompilação de GTA IV do OZORDI e o LibertyRecompAndroid do vaduur, de onde GTA IV for PS5 parte."));
    entry(T("REXGLUE AND XENIA", "REXGLUE E XENIA"),
          T("The static recompiler and runtime under all of it, and the Xenia team's Xbox 360 emulator it grew from "
          "(Xenia Canary's XMA decoding is what plays the game's sound here).", "O recompilador estático e o runtime por baixo de tudo, e o emulador de Xbox 360 da equipe do Xenia "
            "de onde ele nasceu (a decodificação XMA do Xenia Canary é o que toca o som do jogo aqui)."));
    entry("CRACKDOWN RECOMPILED  -  SAULOB",
          T("Whose ReXGlue patches and frame rate work this port's GPU and timing fixes come from. "
          "github.com/saulob/crackdown-xbox360-recompiled", "Dos patches de ReXGlue e do trabalho de taxa de quadros dele vêm as correções de GPU e de tempo "
            "deste port. github.com/saulob/crackdown-xbox360-recompiled"));
    entry("MAX PAYNE 3 REPRESCRIBED  /  CRACKDOWN: REACTIVATED  /  METAL GEAR RISING REPLATINUM",
          T("The sister ports this one grew out of: the same menu, installer, saves and trophies, and their fixes.", "Os ports irmãos de onde este nasceu: o mesmo menu, instalador, saves e troféus, e as suas correções."));

    const float px = 1060 * u + page_dx_, pw = size.x - 140 * u - px;
    float py = 180 * u;
    // the shout-out, on a panel of its own
    {
      const char* thanks =
          T("For Stilwater and the 3rd Street Saints: Julius, Johnny Gat, Troy, Dex, Shaundi, Pierce and Carlos, "
          "the purple flag, and a city to take back block by block.", "Por Stilwater e os 3rd Street Saints: Julius, Johnny Gat, Troy, Dex, Shaundi, Pierce e Carlos, a "
            "bandeira roxa, e uma cidade para retomar quarteirão por quarteirão.");
      const float th = g_body->CalcTextSizeA(22 * u, FLT_MAX, pw - 48 * u, thanks).y;
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px + pw, py + 96 * u + th),
                        P(IM_COL32(8, 8, 8, 170)), 6 * u);
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px - 18 * u, py + 96 * u + th), P(kRed));
      dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), T("SPECIAL THANKS", "AGRADECIMENTOS ESPECIAIS"));
      dl->AddText(g_item, 40 * u, ImVec2(px, py + 30 * u), P(kWhite), "VOLITION");
      dl->AddText(g_body, 22 * u, ImVec2(px, py + 84 * u), P(kGrey), thanks, nullptr, pw - 48 * u);
      py += 96 * u + th + 46 * u;
    }
    if (!song_.empty()) {
      dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), T("MENU SONG", "MÚSICA DO MENU"));
      dl->AddText(g_body, 24 * u, ImVec2(px, py + 32 * u), P(kGrey), Fit(g_body, 24 * u, song_, pw).c_str());
      py += 90 * u;
    }
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), T("PLEASE NOTE", "OBSERVAÇÃO"));
    py += 36 * u;
    const char* note =
        T("An unofficial fan project, not affiliated with or endorsed by Deep Silver, Volition, THQ, Microsoft "
        "or Sony. Saints Row and all related names belong to their owners. No game files are included: you need your "
        "own copies of Saints Row and Saints Row 2 for the Xbox 360.", "Um projeto não oficial de fãs, sem ligação com nem aval da Deep Silver, Volition, THQ, Microsoft ou "
          "Sony. Saints Row e todos os nomes relacionados pertencem aos seus donos. Nenhum arquivo dos jogos "
          "está incluído: você precisa das suas próprias cópias de Saints Row e Saints Row 2 para o Xbox 360.");
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kGrey), note, nullptr, pw);
    py += g_body->CalcTextSizeA(20 * u, FLT_MAX, pw, note).y + 28 * u;
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), T("BUILT WITH", "FEITO COM"));
    py += 36 * u;
    dl->AddText(g_body, 19 * u, ImVec2(px, py), P(kGrey),
                T("PS5 Payload SDK  /  Mesa RADV (Vulkan)  /  Dear ImGui\nstb_image, dr_mp3, zlib, xxHash  /  Oswald and "
                "Inter, Pirata One (SIL Open Font License)\nBlackBearReloaded's PS5 AT9 Converter (the home screen music)\n"
                "QR Code generator (Project Nayuki)  /  libcurl, OpenSSL (SEND LOGS)", "PS5 Payload SDK  /  Mesa RADV (Vulkan)  /  Dear ImGui\nstb_image, dr_mp3, zlib, xxHash  /  Oswald e "
                  "Inter, Pirata One (SIL Open Font License)\nPS5 AT9 Converter de BlackBearReloaded (a música da tela "
                  "inicial)\nGerador de QR Code (Project Nayuki)  /  libcurl, OpenSSL (ENVIAR LOGS)"),
                nullptr, pw);
    dl->PopClipRect();
  }

  // ---- files ----

  // The game's song (hub/music1/ or hub/music2/), in place of the one playing.
  void StartMusic(int game) {
    const fs::path dir = app_ / "hub" / ("music" + std::to_string(game));
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file() || !gta4ps5::sound::CanOpen(e.path())) continue;
      if (!music_) {
        if (!gta4ps5::sound::StartOutput()) {
          REXLOG_WARN("SAINTS ROW: ReInstated menu: no audio port for the menu's song");
          return;
        }
        music_ = gta4ps5::sound::NewVoice();
      }
      if (auto src = gta4ps5::sound::Open(e.path())) {
        music_->Stop();
        music_->SetVolume(sets_[1].music / 100.0f);
        music_->Play(std::move(src), true);
        song_ = e.path().stem().string();
        REXLOG_INFO("SAINTS ROW: ReInstated menu: playing {}", e.path().filename().string());
        return;
      }
    }
  }

  Image Load(const fs::path& file) {
    if (!immediate_) return {};
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &n, 4);
    if (!px) return {};
    auto tex = immediate_->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, px);
    stbi_image_free(px);
    if (!tex) return {};
    Image img{reinterpret_cast<ImTextureID>(tex.get()), float(w), float(h)};
    g_textures->push_back(std::move(tex));
    return img;
  }

  rex::ui::ImmediateDrawer* immediate_ = nullptr;
  fs::path app_, user_data_[3];
  std::function<void(MenuChoice)> play_;
  Settings sets_[3];  // each game's settings (1, 2)
  int set_game_ = 1;      // SETTINGS: whose settings are shown
  bool in_list_ = false;  // SETTINGS: in that game's settings (false: in the game list)
  float reset_t_ = -100;  // (when SQUARE last put the recommended settings back)
  Pad pad_;
  Image background_[3], logo_[3], title_;  // (title_: hub/title.png, the header's logo)
  int game_ = 1, trophy_game_ = 1;
  float shown_game_ = 1;
  std::string start_error_;
  trophies::List lists_[3];
  gta4ps5::sound::Voice* music_ = nullptr;
  std::string song_;
  install::Installed installed_;
  std::vector<install::Source> sources_;
  std::set<std::string> tried_;  // (installed by themselves once a session)
  uint32_t job_generation_ = 0;
  int content_row_ = 0, saves_row_ = 0, confirm_key_ = -1;
  // SEND LOGS: the report being typed (the keyboard over the page), what was sent with the logs,
  // and the QR code of the GitHub issue for the link they went to
  bool reporting_ = false, caps_ = true;
  int key_row_ = 1, key_col_ = 0;
  std::string note_, sent_note_, sent_settings_, qr_link_;
  std::vector<uint8_t> qr_;  // (modules, row by row; 0 when it didn't fit)
  int qr_size_ = 0;
  float result_t_ = -100, confirm_t_ = -100, missing_flash_ = 0;
  bool ask_leftovers_ = false;
  uint64_t leftover_bytes_ = 0;
  Page page_ = kHome;
  int row_ = kAntiAliasing, trophy_ = 0;
  float settings_scroll_ = 0;  // (the settings list's scroll, eased)
  trophies::List trophies_;
  std::vector<Item> items_;
  std::map<uint32_t, Image> pictures_;
  int pictures_this_frame_ = 0;
  float time_ = 0, leave_t_ = 0, fade_ = 0, u_ = 1, page_in_ = 1, page_alpha_ = 1, page_dx_ = 0;
  float tab_x_ = 0, tab_w_ = 0, tab_target_x_ = 0, tab_target_w_ = 0, page_dark_ = 0, scroll_ = 0, detail_in_ = 1;
  bool leaving_ = false, done_ = false;
};

ImFont* AddFont(ImFontAtlas* atlas, const unsigned char* begin, const unsigned char* end, float size) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;  // (built into the app)
  cfg.OversampleH = 2;
  return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(begin), int(end - begin), size, &cfg);
}

}  // namespace

void ConfigureMenuFonts(ImFontAtlas* atlas) {
  // (at the 4K screen's sizes: drawn smaller on a 1080p one)
  g_title = AddFont(atlas, srps5_font_title, srps5_font_title_end, 240.0f);
  g_item = AddFont(atlas, srps5_font_item, srps5_font_item_end, 128.0f);
  g_body = AddFont(atlas, srps5_font_body, srps5_font_body_end, 56.0f);
}

ImFont* MenuBodyFont() { return g_body; }

void ApplySettings(const fs::path& app) { Settings::Load(app).Apply(); }
ImFont* MenuItemFont() { return g_item; }

void ShowMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const fs::path& app,
              std::function<void(MenuChoice)> play) {
  REXLOG_INFO("SAINTS ROW: ReInstated menu: shown");
  new MenuDialog(drawer, immediate, app, std::move(play));  // (the dialog owns itself)
}

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
