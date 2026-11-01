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
#include "sr_trophies.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

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
  kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kCross = 16, kCircle = 32, kL1 = 64, kR1 = 128, kTriangle = 256
};
constexpr const char* kGameTitles[3] = {"", "SAINTS ROW", "SAINTS ROW 2"};

constexpr uint32_t kPadUp = 0x00000010u, kPadRight = 0x00000020u, kPadDown = 0x00000040u,
                   kPadLeft = 0x00000080u, kPadL1 = 0x00000400u, kPadR1 = 0x00000800u,
                   kPadTriangle = 0x00001000u, kPadCircle = 0x00002000u, kPadCross = 0x00004000u;
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
    if (d.buttons & kPadL1) b |= kL1;
    if (d.buttons & kPadR1) b |= kR1;
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
  bool radnet = true;      // (Prototype 2's Radnet setting, kept in menu.ini: not used here)
  int language = 1;        // the Xbox 360's language ID (user_language, XGetLanguage): kLanguages

  struct Language {
    int id;
    const char* name;
    bool p1;  // (on Saints Row's disc too: English, French, Spanish, Italian)
  };
  static constexpr Language kLanguages[] = {{1, "ENGLISH", true}, {4, "FRENCH", true}, {3, "GERMAN", true},
                                            {6, "ITALIAN", true}, {5, "SPANISH", true}};
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

  static Settings Load(const fs::path& app) {
    Settings s;
    if (std::FILE* f = std::fopen((app / "menu.ini").string().c_str(), "rb")) {
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
        if (k == "radnet") s.radnet = value != 0;
        if (k == "language") s.language = kLanguages[LanguageIndex(value)].id;
      }
      std::fclose(f);
    }
    return s;
  }

  void Save(const fs::path& app) const {
    if (std::FILE* f = std::fopen((app / "menu.ini").string().c_str(), "wb")) {
      std::fprintf(f,
                   "anti_aliasing = %d\nupscaling_fsr = %d\nfsr_sharpness = %d\nrenderer_fast = %d\ncheat_menu = %d\n"
                   "dualsense_strength = %d\n"
                   "texture_filtering = %d\ngpu_lane = %d\nmenu_music = %d\ngame = %d\nfps_counter = %d\n"
                   "dualsense = %d\nps_prompts = %d\nskip_intro = %d\nradnet = %d\nframe_rate = %d\nlanguage = %d\n",
                   aa, fsr ? 1 : 0, sharpness, host_rts ? 1 : 0, cheat_menu ? 1 : 0, dualsense_strength, filter, lane ? 1 : 0, music, game, fps, dualsense ? 1 : 0, ps_prompts ? 1 : 0,
                   skip_intro ? 1 : 0, radnet ? 1 : 0, fps60 ? 60 : 30, language);
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
    // (the games' text follows XGetLanguage; one Saints Row's disc hasn't: English there)
    const Language& lang = kLanguages[LanguageIndex(language)];
    rex::cvar::SetFlagByName("user_language", std::to_string(SR_GAME == 1 && !lang.p1 ? 1 : lang.id));
    REXLOG_INFO("SAINTS ROW: ReInstated menu: frame rate {}, upscaling {} (sharpness {}), anti-aliasing {}, texture filtering {}, the GPU's second lane {}, "
                "frame rate counter {}, DualSense features {}, button prompts {}, skip intro {}, Radnet events {}, "
                "language {}",
                fps60 ? 60 : 30, fsr ? "FSR 1" : "bilinear", kSharpnessNames[sharpness], kAaNames[aa], kFilterNames[filter], lane ? "on" : "off", kFpsModes[fps],
                dualsense ? "on" : "off", ps_prompts ? "PlayStation" : "Xbox", skip_intro ? "on" : "off",
                radnet ? "unlocked" : "as on the disc", lang.name);
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

std::string Date(int64_t t, const char* format) {
  const std::time_t tt = std::time_t(t);
  char out[64] = {};
  if (const std::tm* tm = std::localtime(&tt)) std::strftime(out, sizeof(out), format, tm);
  return out;
}

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Letter-spaced text; returns its width.
float Spaced(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* t, float spacing) {
  const float x0 = p.x;
  for (const char* c = t; *c; ++c) {
    dl->AddText(f, size, p, col, c, c + 1);
    p.x += f->CalcTextSizeA(size, FLT_MAX, 0, c, c + 1).x + spacing;
  }
  return p.x - x0 - spacing;
}

// The pages, in the tab bar's order.
enum Page { kHome, kContent, kSaves, kSettings, kTrophies, kCredits, kPages };
const char* const kPageNames[kPages] = {"HOME", "CONTENT", "SAVES", "SETTINGS", "TROPHIES", "CREDITS"};

// The settings page's rows, in its order (a new option: a row here, its line
// in SettingsLines and its change in ChangeSetting).
enum SettingsRow {
  kFrameRate, kUpscaling, kSharpness, kAntiAliasing, kFiltering, kLane, kFps, kPrompts, kDualSense, kLanguage, kSkipIntro, kCheatMenu, kDualSenseStrength, kRenderer, kMusic, kSettingsRows
};

class MenuDialog final : public rex::ui::ImGuiDialog {
 public:
  MenuDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const fs::path& app,
             std::function<void(MenuChoice)> play)
      : ImGuiDialog(drawer), immediate_(immediate), app_(app), play_(std::move(play)),
        settings_(Settings::Load(app)) {
    const fs::path hub = app / "hub";
    for (int g = 1; g <= 2; ++g) {
      const std::string n = std::to_string(g);
      for (const char* ext : {".jpg", ".png"})
        if (!background_[g]) background_[g] = Load(hub / ("background" + n + ext));
      logo_[g] = Load(hub / ("logo" + n + ".png"));
    }
    game_ = settings_.game;
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
                installed_.game[1] ? "installed" : "MISSING", installed_.update[1] ? " (TU1)" : " (no TU1)",
                installed_.game[2] ? "installed" : "MISSING", installed_.update[2] ? " (TU3)" : " (no TU3)",
                installed_.dlc, sources_.size());
    StartMusic(game_);
    // (the runtime's user data: /app0/UserData/sr1 and sr2)
    for (int g = 1; g <= 2; ++g) {
      user_data_[g] = app / "UserData" / (g == 2 ? "sr2" : "sr1");
      lists_[g] = trophies::Load(user_data_[g]);
      REXLOG_INFO("SAINTS ROW: ReInstated menu: {} trophies {} ({} of {} earned)", install::kGameNames[g],
                  lists_[g].loaded ? "listed" : "not listed yet", lists_[g].earned, lists_[g].earnable);
    }
    ShowTrophies(game_);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (done_) return;
    const float dt = std::min(io.DeltaTime, 0.1f);
    time_ += dt;
    pictures_this_frame_ = 0;
    HandlePad(pad_.Pressed());
    TickInstaller();
    if (music_) music_->SetVolume(leaving_ ? 0.0f : settings_.music / 100.0f);
    tab_x_ += (tab_target_x_ - tab_x_) * std::min(1.0f, dt * 14.0f);
    tab_w_ += (tab_target_w_ - tab_w_) * std::min(1.0f, dt * 14.0f);
    page_in_ = std::min(1.0f, page_in_ + dt * 5.0f);
    Draw(io.DisplaySize, dt);
    if (leaving_ && (leave_t_ += dt) > 0.7f) {
      settings_.game = game_;
      settings_.Save(app_);
      if (game_ == 2) {
        // Saints Row 2 is its own program: started in this one's place (it applies menu.ini itself)
        REXLOG_INFO("SAINTS ROW: ReInstated menu: starting Saints Row 2 (sr2.bin)");
        rex::FlushLogging();
        char* args[] = {nullptr};
        const int rc = sceSystemServiceLoadExec("/app0/sr2.bin", args);
        REXLOG_ERROR("SAINTS ROW: ReInstated menu: starting sr2.bin failed ({:#x})", uint32_t(rc));
        start_error_ = "Couldn't start Saints Row 2 (sr2.bin is missing from the app folder, or the PS5 refused it).";
        leaving_ = false;
        leave_t_ = 0;
        if (music_) StartMusic(game_);
        return;
      }
      done_ = true;
      settings_.Apply();
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
  enum ContentRow { kRowGame1, kRowGame2, kRowDlc, kRowTextures, kContentRows };

  // What waits in install/ for a content row; nullptr: nothing.
  const install::Source* SourceFor(int row) const {
    for (const auto& s : sources_) {
      const bool game = s.kind == install::Kind::kGameImage || s.kind == install::Kind::kGameFolder;
      const bool update = s.kind == install::Kind::kUpdate && !s.installed;
      if (row == kRowGame1 && (game || update) && s.game == 1) return &s;
      if (row == kRowGame2 && (game || update) && s.game == 2) return &s;
      if (row == kRowDlc && s.kind == install::Kind::kDlc && !s.installed) return &s;
      if (row == kRowTextures && s.kind == install::Kind::kTexturePack && !s.installed) return &s;
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
      case kRowTextures: return installed_.textures > 0;
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
    if (p & (kL1 | kR1)) {
      page_ = Page((page_ + ((p & kR1) ? 1 : kPages - 1)) % kPages);
      page_in_ = 0;
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
        const int rows = 1 + int(SaveSources().size());
        if (p & kUp) saves_row_ = (saves_row_ + rows - 1) % rows;
        if (p & kDown) saves_row_ = (saves_row_ + 1) % rows;
        saves_row_ = std::min(saves_row_, rows - 1);
        auto& job = install::TheJob();
        if (job.Busy() || !(p & kCross)) break;
        if (saves_row_ == 0) {
          job.Backup(app_);
        } else if (Confirmed(200 + saves_row_)) {
          job.Install(app_, *SaveSources()[size_t(saves_row_ - 1)]);
        }
        break;
      }
      case kSettings: {
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

  struct SettingLine {
    const char* section;  // the heading it's under
    const char* label;
    std::string value;
    bool enabled;
    const char* help;
    bool slider = false;  // (a level: the bar, not < >)
  };

  std::vector<SettingLine> SettingsLines() const {
    char music[16];
    std::snprintf(music, sizeof(music), "%d%%", settings_.music);
    std::vector<SettingLine> lines(kSettingsRows);
    lines[kFrameRate] = {"GRAPHICS", "FRAME RATE", settings_.fps60 ? "60 (EXPERIMENTAL)" : "30", true,
                         "30 is the frame rate both games were made for on the Xbox 360. 60 lets them run twice as "
                         "often - as their PC versions can - wherever the PS5 keeps up: busy scenes fall between "
                         "the two. Experimental: if anything plays too fast or stutters, set it back to 30."};
    lines[kUpscaling] = {"GRAPHICS", "UPSCALING", settings_.fsr ? "FSR 1" : "BILINEAR", true,
                         "How the picture is enlarged to your TV's resolution (up to 4K). FSR 1, AMD's upscaler, keeps "
                         "edges and detail sharp at almost no cost to the frame rate; BILINEAR is a plain, softer "
                         "stretch."};
    lines[kSharpness] = {"GRAPHICS", "SHARPNESS", settings_.fsr ? Settings::kSharpnessNames[settings_.sharpness] : "-",
                         settings_.fsr,
                         "How much FSR 1 sharpens the enlarged picture. HIGH is crispest but can make edges look "
                         "harsh; LOW is gentler."};
    lines[kAntiAliasing] = {"GRAPHICS", "ANTI-ALIASING", Settings::kAaNames[settings_.aa], true,
                            "Smooths jagged edges before the picture is enlarged. SMAA finds edges by their shape and "
                            "keeps the rest sharp; FXAA is a little cheaper but blurs fine detail. Both cost the "
                            "PS5's GPU very little."};
    lines[kFiltering] = {"GRAPHICS", "TEXTURE FILTERING",
                         std::string(Settings::kFilterNames[settings_.filter]) + " ANISOTROPIC", true,
                         "How sharp floors, walls and streets stay when seen at an angle. Higher is sharper; the PS5's "
                         "GPU handles 16X easily."};
    lines[kLane] = {"GRAPHICS", "GPU HELPER", settings_.lane ? "ON" : "OFF", true,
                    "A second thread that takes part of the graphics work - copying the game's memory for the GPU and "
                    "writing its texture lists - off the thread that sets the frame rate. Turn it off if something "
                    "draws wrong."};
    static constexpr const char* kFpsNames[] = {"OFF", "SIMPLE", "DETAILED"};
    lines[kFps] = {"GRAPHICS", "FPS COUNTER", kFpsNames[settings_.fps], true,
                   "A frame rate counter at the left edge of the screen while you play. Simple shows frames a second "
                   "and the frame time; detailed adds the slowest frames and a graph of the last few seconds."};
    lines[kPrompts] = {"CONTROLLER", "BUTTON PROMPTS", settings_.ps_prompts ? "PLAYSTATION" : "XBOX", true,
                       "The buttons the games show you - in hints, menus and moves - as the DualSense's (cross, "
                       "circle, square, triangle, L1, R1...) or as the Xbox 360's."};
    lines[kDualSense] = {"CONTROLLER", "DUALSENSE FEATURES", settings_.dualsense ? "ON" : "OFF", true,
                         "The light bar in the Saints' purple, dimming as you're hurt and blinking when you're close "
                         "to death, and a pulse when you're hit. The games' own rumble plays either way. (In the game: "
                         "L3 + R3 opens the quick menu, with the cheats.)"};
    lines[kLanguage] = {"GAME", "LANGUAGE", Settings::kLanguages[Settings::LanguageIndex(settings_.language)].name,
                        true,
                        "The language of the games' text, menus and subtitles, as the Xbox 360's own language "
                        "setting chooses it: the discs have English, French, German, Italian and Spanish."};
    lines[kSkipIntro] = {"GAME", "SKIP INTRO", settings_.skip_intro ? "ON" : "OFF", true,
                         "The games start straight at their title screens, without the THQ and Volition logo "
                         "movies."};
    lines[kCheatMenu] = {"GAME", "CHEAT MENU (L3 + R3)", settings_.cheat_menu ? "ON" : "OFF", true,
                         "In the game, press both sticks in (L3 + R3) for the quick menu: the cheats, the FPS "
                         "counter and the DualSense features. OFF: no quick menu, and no cheats."};
    static constexpr const char* kStrengths[] = {"", "LOW", "MEDIUM", "HIGH"};
    lines[kDualSenseStrength] = {"CONTROLLER", "FEEDBACK STRENGTH", kStrengths[settings_.dualsense_strength],
                                 settings_.dualsense,
                                 "How strong the DualSense's added rumble and pulses are."};
    lines[kRenderer] = {"EXPERIMENTAL", "RENDERER", settings_.host_rts ? "FAST" : "ACCURATE", true,
                        "ACCURATE copies the Xbox 360's video memory exactly, pixel by pixel. FAST draws with the PS5's "
                        "own graphics hardware: quicker, and the light glows hide behind walls and people as they "
                        "should, but some effects may draw wrong. Testing."};
    lines[kMusic] = {"AUDIO", "MENU MUSIC", music, true, "How loud this menu's song is.", true};
    return lines;
  }

  void ChangeSetting(int row, int step, bool cross) {
    const bool flip = step || cross;
    switch (row) {
      case kFrameRate:
        if (flip) settings_.fps60 = !settings_.fps60;
        break;
      case kUpscaling:
        if (flip) settings_.fsr = !settings_.fsr;
        break;
      case kSharpness:
        if (!settings_.fsr) break;
        if (cross) settings_.sharpness = settings_.sharpness % 3 + 1;
        settings_.sharpness = std::clamp(settings_.sharpness + step, 1, 3);
        break;
      case kAntiAliasing:
        if (cross) settings_.aa = (settings_.aa + 1) % 3;
        settings_.aa = std::clamp(settings_.aa + step, 0, 2);
        break;
      case kFiltering:
        if (cross) settings_.filter = settings_.filter == 5 ? 2 : settings_.filter + 1;
        settings_.filter = std::clamp(settings_.filter + step, 2, 5);
        break;
      case kLane:
        if (flip) settings_.lane = !settings_.lane;
        break;
      case kFps:
        if (cross) settings_.fps = (settings_.fps + 1) % 3;
        settings_.fps = std::clamp(settings_.fps + step, 0, 2);
        break;
      case kPrompts:
        if (flip) settings_.ps_prompts = !settings_.ps_prompts;
        break;
      case kDualSense:
        if (flip) settings_.dualsense = !settings_.dualsense;
        break;
      case kLanguage: {
        const int n = int(std::size(Settings::kLanguages));
        const int i = (Settings::LanguageIndex(settings_.language) + (step ? step : cross ? 1 : 0) + n) % n;
        settings_.language = Settings::kLanguages[i].id;
        break;
      }
      case kSkipIntro:
        if (flip) settings_.skip_intro = !settings_.skip_intro;
        break;
      case kCheatMenu:
        if (flip) settings_.cheat_menu = !settings_.cheat_menu;
        break;
      case kDualSenseStrength:
        if (!settings_.dualsense) break;
        if (cross) settings_.dualsense_strength = settings_.dualsense_strength % 3 + 1;
        settings_.dualsense_strength = std::clamp(settings_.dualsense_strength + step, 1, 3);
        break;
      case kRenderer:
        if (flip) settings_.host_rts = !settings_.host_rts;
        break;
      case kMusic:
        settings_.music = std::clamp(settings_.music + step * 10, 0, 100);
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
    if (it.index < 0) return std::string(install::kGameNames[trophy_game_]) + " Platinum";
    const auto& t = trophies_.trophies[size_t(it.index)];
    return (!t.earned && t.hidden()) ? "Hidden trophy" : t.name;
  }
  std::string TextOf(const Item& it) const {
    if (it.index < 0)
      return "Earn every other " + std::string(install::kGameNames[trophy_game_]) + " trophy (" +
             std::to_string(trophies_.earnable) + ").";
    const auto& t = trophies_.trophies[size_t(it.index)];
    std::string text;
    if (!t.earned && t.hidden()) text = "Keep playing to find out what this one is.";
    else text = t.earned || t.locked_description.empty() ? t.description : t.locked_description;
    if (t.online())
      text += "\n\nOnline: it needs a service this port can't reach, so it doesn't count toward the platinum.";
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
  }

  // "The installs are done: delete the files they came from?" over the page.
  void DrawLeftoversQuestion(ImDrawList* dl, const ImVec2& size) {
    const float u = u_;
    dl->AddRectFilled(ImVec2(0, 0), size, A(IM_COL32(0, 0, 0, 170)));
    const float w = 1100 * u, x = (size.x - w) * 0.5f, y = size.y * 0.5f - 200 * u;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 400 * u), A(IM_COL32(14, 12, 12, 250)), 10 * u);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 8 * u, y + 400 * u), A(kRed));
    dl->AddText(g_item, 44 * u, ImVec2(x + 50 * u, y + 40 * u), A(kWhite), "INSTALL COMPLETE");
    char line[96];
    std::snprintf(line, sizeof(line), "%.1f GB", double(leftover_bytes_) / (1024.0 * 1024.0 * 1024.0));
    const std::string text =
        std::string("The disc images and DLC packages it was installed from are still in "
                    "PPSA54107/install/ (") +
        line +
        "). The game doesn't need them any more.\n\nDelete them to free the space, or keep them (they won't "
        "be installed again).";
    dl->AddText(g_body, 28 * u, ImVec2(x + 50 * u, y + 110 * u), A(kText), text.c_str(), nullptr, w - 100 * u);
    dl->AddText(g_item, 34 * u, ImVec2(x + 50 * u, y + 320 * u), A(kWhite), "X  DELETE");
    dl->AddText(g_item, 34 * u, ImVec2(x + 380 * u, y + 320 * u), A(kGrey), "O  KEEP");
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
    shown_game_ += (float(game_) - shown_game_) * std::min(1.0f, dt * 5.0f);
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
      const float w = Width(g_item, ts, kPageNames[i]);
      if (i == page_) {
        tab_target_x_ = x, tab_target_w_ = w;
        if (tab_w_ == 0) tab_x_ = x, tab_w_ = w;
      }
      dl->AddText(g_item, ts, ImVec2(x, y), A(i == page_ ? kWhite : kGrey), kPageNames[i]);
      x += w + 52 * u;
    }
    button(x - 22 * u, "R1");
    // the project's name at the right
    const char* name = "SAINTS ROW  REINSTATED";
    const float nw = Width(g_body, 20 * u, name);
    dl->AddText(g_body, 20 * u, ImVec2(size.x - 140 * u - nw, y + 12 * u), A(kDim), name);
    dl->AddRectFilled(ImVec2(tab_x_, y + 56 * u), ImVec2(tab_x_ + tab_w_, y + 61 * u), A(kRed));
    dl->AddRectFilled(ImVec2(140 * u, y + 61 * u), ImVec2(size.x - 140 * u, y + 62 * u), A(IM_COL32(255, 255, 255, 40)));
  }

  void DrawFooter(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, y = size.y - 92 * u;
    dl->AddRectFilled(ImVec2(140 * u, y - 18 * u), ImVec2(size.x - 140 * u, y - 17 * u), A(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 22 * u, ImVec2(140 * u, y), A(kRed),
                Upper(Date(int64_t(std::time(nullptr)), "%a / %b %d / %I:%M %p")).c_str());
    std::string hints;
    switch (page_) {
      case kHome: hints = "UP DOWN  GAME      X  PLAY      L1 R1  PAGES"; break;
      case kContent: hints = "UP DOWN  MOVE      X  INSTALL      TRIANGLE  UNINSTALL      O  BACK      L1 R1  PAGES"; break;
      case kSaves: hints = "UP DOWN  MOVE      X  SELECT      O  BACK      L1 R1  PAGES"; break;
      case kSettings: hints = "UP DOWN  MOVE      LEFT RIGHT  CHANGE      O  BACK      L1 R1  PAGES"; break;
      case kTrophies: hints = "UP DOWN  MOVE      LEFT RIGHT  PAGE UP / DOWN      TRIANGLE  OTHER GAME      O  BACK"; break;
      default: hints = "O  BACK      L1 R1  PAGES"; break;
    }
    dl->AddText(g_body, 22 * u, ImVec2(size.x - 140 * u - Width(g_body, 22 * u, hints), y), A(kGrey), hints.c_str());
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
      dl->AddText(g_item, ps, ImVec2(left, y), P(Ready() && !installing ? kWhite : kDim), "PLAY");
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
                ? (g == 1 ? "SAINTS ROW NEEDS TITLE UPDATE 1\nPut Saints Row's Title Update 1 package (TU_...) in "
                            "PPSA54107/install/ and open the app again: it installs by itself."
                          : "SAINTS ROW 2 NEEDS TITLE UPDATE 3\nPut Saints Row 2's Title Update 3 package (TU_...) in "
                            "PPSA54107/install/ and open the app again: it installs by itself.")
            : g == 1 ? "SAINTS ROW ISN'T INSTALLED\nPut your Saints Row disc image (.iso, any name) or the disc's files, "
                       "and Title Update 1's package, in PPSA54107/install/ and open the app again: they install by "
                       "themselves. The CONTENT page shows what's there."
                     : "SAINTS ROW 2 ISN'T INSTALLED\nPut your Saints Row 2 disc image (.iso, any name) or the disc's "
                       "files, and Title Update 3's package, in PPSA54107/install/ and open the app again: they "
                       "install by themselves. The CONTENT page shows what's there.";
        const float pw = 900 * u, th = g_body->CalcTextSizeA(26 * u, FLT_MAX, pw, what).y;
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left + pw + 16 * u, y + th + 12 * u),
                          P(IM_COL32(40, 6, 6, ImU32(200 + 55 * missing_flash_))), 6 * u);
        dl->AddRectFilled(ImVec2(left - 16 * u, y - 12 * u), ImVec2(left - 10 * u, y + th + 12 * u), P(kRed));
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite), what, nullptr, pw);
        y += th + 42 * u;
      } else {
        std::string where = "Your saves load with the game.";
        if (installed_.dlc)
          where = (installed_.dlc == 1 ? std::string("1 DLC pack") : std::to_string(installed_.dlc) + " DLC packs") +
                  " and your saves load with the game.";
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
        installed_.dlc ? "DLC  (" + std::to_string(installed_.dlc) + (installed_.dlc == 1 ? " PACK)" : " PACKS)") : "DLC";
    const std::string textures_name =
        installed_.textures ? "HD TEXTURES  (" + std::to_string(installed_.textures) + ")" : "HD TEXTURES";
    const std::string names[kContentRows] = {"SAINTS ROW", "SAINTS ROW 2", dlc_name, textures_name};
    const char* help[kContentRows] = {
        "Saints Row's disc and its Title Update 1 (both needed). Put your disc image (.iso, any name) or a "
        "folder of the disc's files, and the title update's package (TU_...), in PPSA54107/install/: they install "
        "into PPSA54107/game1/ when the app opens, or with X here.",
        "Saints Row 2's disc and its Title Update 3 (both needed). Put your disc image (.iso, any name) or a "
        "folder of the disc's files, and the title update's package (TU_...), in PPSA54107/install/: they install "
        "into PPSA54107/game2/ when the app opens, or with X here.",
        "Optional: either game's DLC (Saints Row's Money Shot and clothing packs; Saints Row 2's Ultor Exposed, "
        "Corporate Warfare and the Unkut pack). Put their packages (any names) in PPSA54107/install/: each "
        "installs by itself. TRIANGLE removes them all.",
        "Optional: HD texture packs (none known for these games yet). TRIANGLE removes any installed."};
    for (int r = 0; r < kContentRows; ++r) {
      const bool sel = r == content_row_, in = RowInstalled(r);
      const install::Source* src = SourceFor(r);
      const float fs = 40 * u;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 8 * u), ImVec2(left - 20 * u, y + fs), P(kRed));
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), names[r].c_str());
      std::string status = in && !(r == kRowDlc && src) ? "INSTALLED"
                           : src                        ? "READY TO INSTALL"
                                                        : "NOT INSTALLED";
      // (a disc without the title update the program is built from)
      if ((r == kRowGame1 || r == kRowGame2) && !src && installed_.game[r + 1] && !installed_.update[r + 1])
        status = r == kRowGame1 ? "NEEDS TITLE UPDATE 1" : "NEEDS TITLE UPDATE 3";
      if (r == kRowDlc && src) status = std::to_string(DlcWaiting()) + " READY TO INSTALL";
      if (r == kRowTextures && src)
        status = std::to_string(Waiting(install::Kind::kTexturePack)) + " PACKAGES READY TO INSTALL";
      if (sel && Confirming(100 + r)) status = "PRESS TRIANGLE AGAIN TO UNINSTALL";
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
        notes.push_back(Short(s.label, 40) + "  -  extract it on a PC first (7-Zip)");
      else if (s.kind == install::Kind::kOther && s.label == install::kAvatarItemLabel)
        ++avatar_items;
      else if (s.kind == install::Kind::kOther)
        notes.push_back(Short(s.label, 60));
    }
    if (avatar_items)
      notes.push_back(std::to_string(avatar_items) + (avatar_items == 1 ? " Xbox 360 avatar item" : " Xbox 360 avatar items") +
                      "  -  not game DLC, not used here");
    if (!notes.empty()) {
      dl->AddText(g_body, 20 * u, ImVec2(left, y), P(kRed), "ALSO IN PPSA54107/INSTALL");
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
    const int rows = 1 + int(saves.size());
    // (an imported save's file deleted after the install: the list shorter under the selected row)
    saves_row_ = std::min(saves_row_, rows - 1);
    for (int r = 0; r < rows; ++r) {
      const bool sel = r == saves_row_;
      const float fs = 36 * u;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 6 * u), ImVec2(left - 20 * u, y + fs), P(kRed));
      std::string label;
      if (r == 0) {
        label = "BACK UP SAVES";
      } else {
        const install::Source* s = saves[size_t(r - 1)];
        label = (s->kind == install::Kind::kBackup ? "RESTORE  " : "IMPORT  ") + Short(s->label, 40);
        if (sel && Confirming(200 + r)) label = "PRESS X AGAIN TO CONFIRM";
      }
      dl->AddText(g_item, fs, ImVec2(left, y), P(sel ? kWhite : kGrey), label.c_str());
      y += fs + 22 * u;
    }
    y += 10 * u;
    y = DrawJob(dl, left, y, 820 * u) + 10 * u;
    // the saves the app has, newest first
    if (!installed_.save_list.empty()) {
      Spaced(dl, g_body, 19 * u, ImVec2(left, y), P(kRed), "YOUR SAVES", 3 * u);
      y += 36 * u;
      for (size_t i = 0; i < installed_.save_list.size() && i < 6; ++i, y += 34 * u) {
        const auto& sv = installed_.save_list[i];
        std::string name = sv.name;
        std::replace(name.begin(), name.end(), '_', ' ');
        dl->AddText(g_body, 26 * u, ImVec2(left, y), P(kWhite),
                    Short((sv.game == 2 ? "P2  " : "P1  ") + name, 34).c_str());
        if (sv.modified)
          dl->AddText(g_body, 26 * u, ImVec2(left + 520 * u, y), P(kGrey),
                      Upper(Date(sv.modified, "%b %d %Y  %I:%M %p")).c_str());
      }
    }
    const char* help =
        saves_row_ == 0
            ? "Copies your saves, your trophies and your settings to PPSA54107/install/backups/. Copy that folder "
              "to a PC or a USB drive before deleting or reinstalling the app."
        : saves[size_t(saves_row_ - 1)]->kind == install::Kind::kBackup
            ? "Puts this backup's saves, trophies and settings back, over the ones here."
            : "Imports this Xbox 360 save (a package copied off a 360, with a USB drive or Horizon) into the app. "
              "A save of the same name here is replaced.";
    const char* where =
        "\n\nTo import a save, put the Xbox 360 save package (the file copied off the 360's drive: "
        "Content/<profile>/4156084E/00000001/... for Saints Row, .../415608A7/00000001/... for Saints Row 2) in "
        "PPSA54107/install/. A backup made here, copied back into "
        "PPSA54107/install/ (or install/backups/), is listed to restore.";
    const std::string text = std::string(help) + (saves.empty() ? where : "");
    const float px = 1180 * u + page_dx_, pw = size.x - 140 * u - px;
    dl->AddRectFilled(ImVec2(px - 40 * u, 200 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_body, 26 * u, ImVec2(px, 204 * u), P(kText), text.c_str(), nullptr, pw);
  }

  void DrawSettings(ImDrawList* dl, const ImVec2& size) {
    const float u = u_, left = 140 * u + page_dx_;
    const auto lines = SettingsLines();
    const float fs = 36 * u, top = 190 * u, bottom = size.y - 120 * u;
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
    dl->PushClipRect(ImVec2(0, top - 4 * u), ImVec2(1000 * u + page_dx_, bottom), true);
    for (int r = 0; r < kSettingsRows; ++r) {
      const SettingLine& l = lines[size_t(r)];
      if (head_y[size_t(r)] >= 0)
        Spaced(dl, g_body, 19 * u, ImVec2(left, top + head_y[size_t(r)] - settings_scroll_), P(kRed), l.section,
               3 * u);
      y = top + row_y[size_t(r)] - settings_scroll_;
      const bool sel = r == row_;
      const ImU32 col = !l.enabled ? kDim : sel ? kWhite : kGrey;
      if (sel) dl->AddRectFilled(ImVec2(left - 30 * u, y + 8 * u), ImVec2(left - 20 * u, y + fs), P(kRed));
      dl->AddText(g_item, fs, ImVec2(left, y), P(col), l.label);
      const std::string v = l.slider || !l.enabled ? l.value : "<  " + l.value + "  >";
      dl->AddText(g_item, fs, ImVec2(left + 420 * u, y), P(sel ? (l.enabled ? kRed : kDim) : col), v.c_str());
      if (l.slider) {
        const float x0 = left + 540 * u, w = 260 * u, yy = y + fs * 0.6f;
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w, yy + 3 * u), P(kDim));
        dl->AddRectFilled(ImVec2(x0, yy - 3 * u), ImVec2(x0 + w * settings_.music / 100.0f, yy + 3 * u),
                          P(sel ? kRed : kGrey));
      }
    }
    dl->PopClipRect();
    // the focused row's help, at the right
    const float px = 1060 * u + page_dx_, pw = 720 * u;
    dl->AddRectFilled(ImVec2(px - 40 * u, 190 * u), ImVec2(px - 39 * u, 760 * u), P(IM_COL32(255, 255, 255, 40)));
    dl->AddText(g_item, 34 * u, ImVec2(px, 190 * u), P(kWhite), lines[size_t(row_)].label);
    dl->AddText(g_body, 28 * u, ImVec2(px, 244 * u), P(kText), lines[size_t(row_)].help, nullptr, pw);
    dl->AddText(g_body, 22 * u, ImVec2(px, 700 * u), P(kDim), "Saved when you press PLAY.");
  }

  void DrawTrophies(ImDrawList* dl, const ImVec2& size, float dt) {
    const float u = u_;
    // which game's, at the top right (TRIANGLE: the other's)
    {
      const std::string which = std::string(kGameTitles[trophy_game_]) + "  /  TRIANGLE: " + kGameTitles[3 - trophy_game_];
      dl->AddText(g_body, 22 * u, ImVec2(size.x - 140 * u - Width(g_body, 22 * u, which), 140 * u), P(kRed),
                  which.c_str());
    }
    if (!trophies_.loaded) {
      const std::string start = std::string("START ") + kGameTitles[trophy_game_] + " ONCE";
      dl->AddText(g_item, 44 * u, ImVec2(140 * u + page_dx_, 220 * u), P(kWhite), start.c_str());
      dl->AddText(g_body, 28 * u, ImVec2(140 * u + page_dx_, 290 * u), P(kGrey),
                  "The trophy list comes from the game itself: start it once, and the next time the menu opens "
                  "your trophies are here, with the ones you've earned.",
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
    static const char* const kGroupNames[] = {"EARNED", "NOT EARNED YET", "ONLINE ONLY"};
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
    const std::string of = std::to_string(trophies_.earned) + " of " + std::to_string(trophies_.earnable) + " earned";
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
    const std::string grade = Upper(std::string(trophies::GradeName(g)) + " trophy");
    dl->AddText(g_body, 20 * u, ImVec2(dx + 136 * u, py + 8 * u), trophies::GradeColor(g, da), grade.c_str());
    dl->AddText(g_item, 36 * u, ImVec2(dx + 136 * u, py + 34 * u), A(kWhite, da), NameOf(it).c_str(), nullptr,
                pw - 136 * u);
    py += 138 * u;
    const std::string text = TextOf(it);
    dl->AddText(g_body, 27 * u, ImVec2(dx, py), A(kText, da), text.c_str(), nullptr, pw);
    py += g_body->CalcTextSizeA(27 * u, FLT_MAX, pw, text.c_str()).y + 34 * u;
    const trophies::Trophy* t = it.index < 0 ? nullptr : &trophies_.trophies[size_t(it.index)];
    std::string status;
    if (Earned(it)) status = "Earned " + Date(t ? t->earned : trophies_.last_earned, "%b %d, %Y");
    else if (!t) status = std::to_string(trophies_.earnable - trophies_.earned) + " to go";
    else if (t->online()) status = "Not available offline";
    else status = "Not earned yet";
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
    entry("BY CHAOSTHEORY1075", "github.com/ChaosTheory1075/sr-ps5");
    dl->AddText(g_body, 20 * u, ImVec2(x, y), P(kRed), "ALL THE CREDIT GOES TO");
    y += 40 * u;
    entry("GTA IV FOR PS5  -  ELRIPALDA",
          "The PS5 port this one is built on, top to bottom: the PS5 runtime, graphics and sound, the hub this menu "
          "copies, the trophies and their pop-up, and the installer payload. github.com/elripalda/gta4-ps5");
    entry("LIBERTY RECOMPILED  -  OZORDI  /  VADUUR",
          "OZORDI's recompilation of GTA IV and vaduur's LibertyRecompAndroid, which GTA IV for PS5 starts from.");
    entry("REXGLUE AND XENIA",
          "The static recompiler and runtime under all of it, and the Xenia team's Xbox 360 emulator it grew from "
          "(Xenia Canary's XMA decoding is what plays the game's sound here).");
    entry("CRACKDOWN RECOMPILED  -  SAULOB",
          "Whose ReXGlue patches and frame rate work this port's GPU and timing fixes come from. "
          "github.com/saulob/crackdown-xbox360-recompiled");
    entry("MAX PAYNE 3 REPRESCRIBED  /  CRACKDOWN: REACTIVATED  /  METAL GEAR RISING REPLATINUM",
          "The sister ports this one grew out of: the same menu, installer, saves and trophies, and their fixes.");

    const float px = 1060 * u + page_dx_, pw = size.x - 140 * u - px;
    float py = 180 * u;
    // the shout-out, on a panel of its own
    {
      const char* thanks =
          "For Stilwater and the 3rd Street Saints: Julius, Johnny Gat, Troy, Dex, Shaundi, Pierce and Carlos, "
          "the purple flag, and a city to take back block by block.";
      const float th = g_body->CalcTextSizeA(22 * u, FLT_MAX, pw - 48 * u, thanks).y;
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px + pw, py + 96 * u + th),
                        P(IM_COL32(8, 8, 8, 170)), 6 * u);
      dl->AddRectFilled(ImVec2(px - 24 * u, py - 16 * u), ImVec2(px - 18 * u, py + 96 * u + th), P(kRed));
      dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), "SPECIAL THANKS");
      dl->AddText(g_item, 40 * u, ImVec2(px, py + 30 * u), P(kWhite), "VOLITION");
      dl->AddText(g_body, 22 * u, ImVec2(px, py + 84 * u), P(kGrey), thanks, nullptr, pw - 48 * u);
      py += 96 * u + th + 46 * u;
    }
    if (!song_.empty()) {
      dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), "MENU SONG");
      dl->AddText(g_body, 24 * u, ImVec2(px, py + 32 * u), P(kGrey), Fit(g_body, 24 * u, song_, pw).c_str());
      py += 90 * u;
    }
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), "PLEASE NOTE");
    py += 36 * u;
    const char* note =
        "An unofficial fan project, not affiliated with or endorsed by Deep Silver, Volition, THQ, Microsoft "
        "or Sony. Saints Row and all related names belong to their owners. No game files are included: you need your "
        "own copies of Saints Row and Saints Row 2 for the Xbox 360.";
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kGrey), note, nullptr, pw);
    py += g_body->CalcTextSizeA(20 * u, FLT_MAX, pw, note).y + 28 * u;
    dl->AddText(g_body, 20 * u, ImVec2(px, py), P(kRed), "BUILT WITH");
    py += 36 * u;
    dl->AddText(g_body, 19 * u, ImVec2(px, py), P(kGrey),
                "PS5 Payload SDK  /  Mesa RADV (Vulkan)  /  Dear ImGui\nstb_image, dr_mp3, zlib, xxHash  /  Oswald and "
                "Inter, Pirata One (SIL Open Font License)\nBlackBearReloaded's PS5 AT9 Converter (the home screen music)",
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
        music_->SetVolume(settings_.music / 100.0f);
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
  Settings settings_;
  Pad pad_;
  Image background_[3], logo_[3];
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
