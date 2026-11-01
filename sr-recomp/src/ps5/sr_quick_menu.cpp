// SAINTS ROW: ReInstated (PS5) - the in-game quick menu (see sr_quick_menu.h).
//
// The controller is read where the game reads it: the title's XInputGetState (P1 82B3DDA8, P2
// 822C7630: r3 the user, r4 the X_INPUT_STATE; they call XamInputGetState). After it runs, the menu
// takes what was pressed and, while open, blanks the state the game gets.
#include "sr_quick_menu.h"
#if REX_PLATFORM_PS5

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/sony_feedback.h>
#include <rex/logging.h>

#include "sr_lua.h"
#include "sr_menu.h"

#include SR_INIT_H

REXCVAR_DECLARE(std::string, sr_fps_counter);

namespace srps5 {
namespace {

#if SR_GAME == 1
constexpr const char* kCheatsFile = "/app0/cheats1.ini";
#else
constexpr const char* kCheatsFile = "/app0/cheats2.ini";
#endif
constexpr const char* kMenuFile = "/app0/menu.ini";

// X_INPUT_GAMEPAD in the X_INPUT_STATE (big-endian): buttons, the triggers, the sticks.
constexpr uint32_t kGamepad = 4, kGamepadSize = 12, kThumbLY = 6;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kL3 = 0x0040, kR3 = 0x0080,
                   kCross = 0x1000, kCircle = 0x2000;
constexpr int kStickDeadzone = 16000;

enum Nav : uint8_t { kNavUp = 1, kNavDown, kNavLeft, kNavRight, kNavSelect, kNavClose };

std::unique_ptr<QuickMenu> g_menu;
std::atomic<bool> g_open{false};
std::mutex g_mutex;
std::deque<uint8_t> g_events;  // guarded by g_mutex
uint16_t g_previous = 0;       // the game thread's
bool g_swallow = false;        // blank the pad until everything is let go (after closing)

void Push(uint8_t event) {
  std::lock_guard lock(g_mutex);
  if (g_events.size() < 16) g_events.push_back(event);
}

uint8_t Take() {
  std::lock_guard lock(g_mutex);
  if (g_events.empty()) return 0;
  const uint8_t e = g_events.front();
  g_events.pop_front();
  return e;
}

uint16_t LoadBe16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, 2);
  return std::byteswap(v);
}

// One read of the controller, on the game thread.
void Filter(uint8_t* base, uint32_t state) {
  uint16_t buttons = LoadBe16(base, state + kGamepad);
  const int ly = int16_t(LoadBe16(base, state + kGamepad + kThumbLY));
  if (ly > kStickDeadzone) buttons |= kUp;  // (the left stick as the D-pad)
  if (ly < -kStickDeadzone) buttons |= kDown;
  const uint16_t pressed = buttons & ~g_previous;
  const bool combo = (buttons & (kL3 | kR3)) == (kL3 | kR3);
  const bool combo_pressed = combo && (pressed & (kL3 | kR3));
  g_previous = buttons;
  if (!g_open.load(std::memory_order_relaxed)) {
    if (combo_pressed) {
      { std::lock_guard lock(g_mutex); g_events.clear(); }
      g_open.store(true, std::memory_order_release);
      REXLOG_INFO("Quick menu: open");
    } else if (g_swallow && !buttons) {
      g_swallow = false;
    }
    if (!g_open.load(std::memory_order_relaxed) && !g_swallow) return;
  } else {
    if (combo_pressed || (pressed & kCircle)) {
      g_open.store(false, std::memory_order_release);
      g_swallow = true;
      Push(kNavClose);
    } else {
      if (pressed & kUp) Push(kNavUp);
      if (pressed & kDown) Push(kNavDown);
      if (pressed & kLeft) Push(kNavLeft);
      if (pressed & kRight) Push(kNavRight);
      if (pressed & kCross) Push(kNavSelect);
    }
  }
  std::memset(base + state + kGamepad, 0, kGamepadSize);
}

// ---- the cheats ----

enum Cheat { kGod, kAmmo, kIgnored, kMass, kInstakill, kCheatCount };
struct CheatInfo {
  const char* key;    // in cheats<N>.ini
  const char* label;  // in the menu
  bool p1;            // (Saints Row has it too)
};
constexpr CheatInfo kCheats[kCheatCount] = {
    {"god_mode", "God Mode", true},
    {"infinite_ammo", "Infinite Ammo", true},
    {"enemies_ignore_you", "Enemies Ignore You", true},
    {"infinite_mass", "Infinite Mass", false},
    {"one_hit_kills", "One-Hit Kills", false},
};
std::array<std::atomic<bool>, kCheatCount> g_on{};
std::atomic<uint32_t> g_generation{1};  // (changed with any cheat: the tick puts them all again)

bool Available(int cheat) { return SR_GAME == 2 || kCheats[cheat].p1; }

void LoadCheats() {
  std::FILE* f = std::fopen(kCheatsFile, "rb");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char key[64] = {};
    int value = 0;
    if (std::sscanf(line, " %63[a-z_] = %d", key, &value) != 2) continue;
    for (int i = 0; i < kCheatCount; ++i)
      if (key == std::string(kCheats[i].key)) g_on[i].store(value != 0 && Available(i));
  }
  std::fclose(f);
}

void SaveCheats() {
  std::FILE* f = std::fopen(kCheatsFile, "wb");
  if (!f) {
    REXLOG_WARN("Cheats: cannot write {}", kCheatsFile);
    return;
  }
  for (int i = 0; i < kCheatCount; ++i) std::fprintf(f, "%s = %d\n", kCheats[i].key, g_on[i].load() ? 1 : 0);
  std::fclose(f);
}

// ---- Saints Row 2's look ----
//
// COLOUR FILTER off: the zone tints gone. Saints Row 2 tints each zone (the yellow one most of all):
// 822C3E08 picks the colour grade for the zone the player's in and blends to it with the time of
// day's tod_BlendColourGrade (824B9468: r4 the grade, 0..9; below 0 none). With the filter off, any
// grade asked for is none; the last one asked for comes back when it's on again.
// FOG: tod_FogDensityScaler (822D0478, f1 the scale the scripts set, 1 until they do) scaled down.
// The quick menu's changes reach them through the same two functions, from the Lua tick, with
// kAgain for "the last one asked for".
constexpr int kAgain = -777;
std::atomic<bool> g_colour_off{false};
std::atomic<int> g_fog{0};             // 0 as the game, 1 reduced, 2 light
std::atomic<uint32_t> g_look_generation{1};
std::atomic<int> g_grade{INT32_MIN};   // (the last grade the game asked for; INT32_MIN: none yet)
std::atomic<double> g_fog_scale{1.0};  // (and the last fog scale)
constexpr const char* kFogNames[] = {"AS THE GAME", "REDUCED", "LIGHT"};
constexpr double kFogScales[] = {1.0, 0.45, 0.15};

// The player, in the chunks (Lua): Saints Row 2's PLAYER global; Saints Row's game object "alex",
// the name its own code looks him up by (823CE4A8).
#if SR_GAME == 1
constexpr const char* kPlayerLua =
    "local P = PLAYER\n"
    "if P == nil and go_FindGOHByName ~= nil then local ok, r = pcall(go_FindGOHByName, 'alex') if ok then P = r end end\n";
#else
constexpr const char* kPlayerLua = "local P = PLAYER\n";
#endif

// The tick: the cheats held, and the player's state for the DualSense ("<health> <max> <in game>").
std::string MakeTick() {
  char head[256];
  std::snprintf(head, sizeof(head), "local god, ammo, ignore, mass, insta, gen, look = %d, %d, %d, %d, %d, %u, %u\n",
                g_on[kGod].load() ? 1 : 0, g_on[kAmmo].load() ? 1 : 0, g_on[kIgnored].load() ? 1 : 0,
                g_on[kMass].load() ? 1 : 0, g_on[kInstakill].load() ? 1 : 0, g_generation.load(),
                SR_GAME == 2 ? g_look_generation.load() : 0u);
  return std::string(head) + "if go_GetHealth == nil or go_GetMaxHealth == nil then return 'skip' end\n" + kPlayerLua +
         R"LUA(if P == nil then return 'none' end
local function try(f, ...) if f then local ok, r = pcall(f, ...) if ok then return r end end end
local m = try(go_GetMaxHealth, P) or 0
local h = try(go_GetHealth, P) or 0
if m <= 0 then return 'none' end
local R = __ReInstated
if R == nil then R = {} __ReInstated = R end
local again = R.gen ~= gen
R.gen = gen
if look ~= 0 and R.look ~= look then
  R.look = look
  try(tod_BlendColourGrade, -777, 0.5)
  try(tod_FogDensityScaler, -777.0)
end
if god == 1 then
  try(go_SetDamageable, P, false)
  if h > 0 and h < m then try(go_SetHealth, P, m) h = m end
  R.god = P
elseif R.god ~= nil then
  try(go_SetDamageable, R.god, true) R.god = nil
end
if again then try(go_SetGlobalInfiniteClip, ammo == 1) end
if ignore == 1 then
  if again or R.ignore ~= P then try(ai_SetTargetable, P, false) R.ignore = P end
elseif R.ignore ~= nil then
  try(ai_SetTargetable, R.ignore, true) R.ignore = nil
end
if go_SetMassMeterInfinite ~= nil then
  if mass == 1 then
    if again or R.mass ~= P then try(go_SetMassMeterInfinite, P, true) R.mass = P end
  elseif R.mass ~= nil then
    try(go_SetMassMeterInfinite, R.mass, false) R.mass = nil
  end
end
if fes_SetInstakill ~= nil and (again or insta ~= (R.insta or 0)) and (insta == 1 or R.insta == 1) then
  try(fes_SetInstakill, insta) R.insta = insta
end
local g = 1
if fe_IsMoviePlaying ~= nil and try(fe_IsMoviePlaying) then g = 0 end
if fe_GetHudLoaded ~= nil and not try(fe_GetHudLoaded) then g = 0 end
return string.format('%.3f %.3f %d', h, m, g)
)LUA";
}

// ---- the one-off actions ----

enum Action { kGiveEp, kLoseAlert, kToggleHud, kTimeOfDay, kRadnet, kActionCount };

// Saints Row 2's debug menu's times of day (tod_SetTimeOfDay's values).
struct TimeOfDay {
  const char* label;
  float value;
};
constexpr TimeOfDay kTimes[] = {{"DAWN", 15.5f}, {"DAY", 16.5f}, {"NIGHT", 17.0f},
                                {"DAWN, RAIN", 15.0f}, {"DAY, RAIN", 16.0f}, {"NIGHT, RAIN", 17.5f}};
int g_time = 1;  // (the UI thread's)

void LogResult(const char* what, const std::string& out) {
  REXLOG_INFO("Quick menu: {}: {}", what, out);
}

void RunAction(int action) {
  const std::string player = std::string(kPlayerLua) + "if P == nil then return 'skip' end\n";
  switch (action) {
    case kGiveEp:
#if SR_GAME == 1
      // (Saints Row's EP, kept in its save as Saints Row 2's: "Sr_Save_EvolutionPoints")
      LuaRunOnce("if db_GetInt == nil or db_SetInt == nil or go_GetHealth == nil then return 'skip' end\n" + player +
                     R"LUA(local k = 'Sr_Save_EvolutionPoints'
local ok, a = pcall(db_GetInt, k, 0, 0)
if not ok or type(a) ~= 'number' then return 'no EP: ' .. tostring(a) end
pcall(db_SetInt, k, 0, a + 10000, 0)
local _, b = pcall(db_GetInt, k, 0, 0)
return 'EP ' .. tostring(a) .. ' -> ' .. tostring(b)
)LUA",
                 [](const std::string& out) { LogResult("give EP", out); });
#else
      LuaRunOnce("if ep_AwardEP == nil then return 'skip' end\n" + player +
                     "local ok, e = pcall(ep_AwardEP, 5000)\nreturn ok and 'EP +5000' or tostring(e)\n",
                 [](const std::string& out) { LogResult("give EP", out); });
#endif
      break;
    case kLoseAlert:
      LuaRunOnce("if ai_ClearAllBlownDisguise == nil then return 'skip' end\n" + player +
                     "local ok, e = pcall(ai_ClearAllBlownDisguise, P)\nreturn ok and 'cleared' or tostring(e)\n",
                 [](const std::string& out) { LogResult("lose alert", out); });
      break;
    case kToggleHud:
      LuaRunOnce("if fe_ToggleHideHUDOption == nil then return 'skip' end\n" + player +
                     "local ok, e = pcall(fe_ToggleHideHUDOption)\nreturn ok and 'toggled' or tostring(e)\n",
                 [](const std::string& out) { LogResult("HUD", out); });
      break;
    case kTimeOfDay: {
      char chunk[160];
      std::snprintf(chunk, sizeof(chunk), "local ok, e = pcall(tod_SetTimeOfDay, %.1f)\nreturn ok and 'set' or tostring(e)\n",
                    kTimes[g_time].value);
      LuaRunOnce("if tod_SetTimeOfDay == nil then return 'skip' end\n" + player + chunk,
                 [](const std::string& out) { LogResult("time of day", out); });
      break;
    }
    case kRadnet:
      UnlockRadnet();
      break;
  }
}

// ---- the rows ----

enum RowKind { kRowCheat, kRowAction, kRowFps, kRowColour, kRowFog, kRowDualSense };
struct Row {
  const char* section;  // a heading over the row, or nullptr
  const char* label;
  RowKind kind;
  int index;  // the cheat or the action
};

std::vector<Row> Rows() {
  std::vector<Row> rows;
  bool first = true;
  for (int i = 0; i < kCheatCount; ++i) {
    if (!Available(i)) continue;
    rows.push_back({first ? "CHEATS" : nullptr, kCheats[i].label, kRowCheat, i});
    first = false;
  }
  rows.push_back({"ACTIONS", SR_GAME == 2 ? "Give 5,000 EP" : "Give 10,000 EP", kRowAction, kGiveEp});
  rows.push_back({nullptr, "Lose The Alert", kRowAction, kLoseAlert});
  rows.push_back({nullptr, "Show / Hide HUD", kRowAction, kToggleHud});
#if SR_GAME == 2
  rows.push_back({nullptr, "Time Of Day", kRowAction, kTimeOfDay});
  rows.push_back({nullptr, "Unlock Radnet Events", kRowAction, kRadnet});
#endif
  rows.push_back({"DISPLAY", "FPS Counter", kRowFps, 0});
#if SR_GAME == 2
  rows.push_back({nullptr, "Colour Filter", kRowColour, 0});
  rows.push_back({nullptr, "Fog", kRowFog, 0});
#endif
  rows.push_back({"CONTROLLER", "DualSense Features", kRowDualSense, 0});
  return rows;
}

int CounterMode() {
  const std::string m = REXCVAR_GET(sr_fps_counter);
  return m == "detailed" ? 2 : m == "simple" ? 1 : 0;
}

std::string ValueOf(const Row& row) {
  switch (row.kind) {
    case kRowCheat: return g_on[row.index].load() ? "ON" : "OFF";
    case kRowAction: return row.index == kTimeOfDay ? kTimes[g_time].label : "PRESS X";
    case kRowFps: return CounterMode() == 2 ? "DETAILED" : CounterMode() == 1 ? "SIMPLE" : "OFF";
    case kRowColour: return g_colour_off.load() ? "OFF" : "ON";
    case kRowFog: return kFogNames[g_fog.load()];
    case kRowDualSense: return rex::input::sony::GetOptions().enabled ? "ON" : "OFF";
  }
  return "";
}

// menu.ini's `key` set to `value`, the other lines kept.
void SetMenuValue(const char* key, int value) {
  std::vector<std::string> lines;
  if (std::FILE* f = std::fopen(kMenuFile, "rb")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) lines.emplace_back(line);
    std::fclose(f);
  }
  const std::string prefix = std::string(key) + " =";
  const std::string entry = prefix + " " + std::to_string(value) + "\n";
  bool found = false;
  for (std::string& l : lines)
    if (l.rfind(prefix, 0) == 0) l = entry, found = true;
  if (!found) lines.push_back(entry);
  if (std::FILE* f = std::fopen(kMenuFile, "wb")) {
    for (const std::string& l : lines) std::fprintf(f, "%s", l.c_str());
    std::fclose(f);
  }
}

// One step of the row's value (back: left); `select` is X.
void Change(const Row& row, bool back, bool select) {
  switch (row.kind) {
    case kRowCheat:
      g_on[row.index].store(!g_on[row.index].load());
      g_generation.fetch_add(1);
      REXLOG_INFO("Quick menu: {} {}", kCheats[row.index].label, g_on[row.index].load() ? "on" : "off");
      SaveCheats();
      break;
    case kRowAction:
      if (row.index == kTimeOfDay && !select) {
        constexpr int n = int(std::size(kTimes));
        g_time = (g_time + (back ? n - 1 : 1)) % n;
      }
      if (select || row.index == kTimeOfDay) RunAction(row.index);
      break;
    case kRowFps: {
      static constexpr const char* kModes[] = {"off", "simple", "detailed"};
      const int mode = (CounterMode() + (back ? 2 : 1)) % 3;
      rex::cvar::SetFlagByName("sr_fps_counter", kModes[mode]);
      SetMenuValue("fps_counter", mode);
      break;
    }
    case kRowColour:
      g_colour_off.store(!g_colour_off.load());
      g_look_generation.fetch_add(1);
      SetMenuValue("colour_filter", g_colour_off.load() ? 0 : 1);
      REXLOG_INFO("Quick menu: colour filter {}", g_colour_off.load() ? "off" : "on");
      break;
    case kRowFog:
      g_fog.store((g_fog.load() + (back ? 2 : 1)) % 3);
      g_look_generation.fetch_add(1);
      SetMenuValue("fog", g_fog.load());
      REXLOG_INFO("Quick menu: fog {}", kFogNames[g_fog.load()]);
      break;
    case kRowDualSense: {
      const bool on = !rex::input::sony::GetOptions().enabled;
      rex::input::sony::SetFeaturesEnabled(on);
      SetMenuValue("dualsense", on ? 1 : 0);
      break;
    }
  }
}

constexpr ImU32 kWhite = IM_COL32(236, 232, 226, 255), kGrey = IM_COL32(150, 146, 140, 255),
#if SR_GAME == 1
                kAccent = IM_COL32(200, 16, 16, 255), kAccentDim = IM_COL32(200, 16, 16, 50);
#else
                kAccent = IM_COL32(255, 64, 0, 255), kAccentDim = IM_COL32(255, 64, 0, 50);
#endif

ImU32 Fade(ImU32 c, float a) { return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * a) << 24); }

}  // namespace

QuickMenu::QuickMenu(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

bool QuickMenu::WantsContinuousRepaint() const { return g_open.load(std::memory_order_relaxed) || shown_ > 0; }

void QuickMenu::OnDraw(ImGuiIO& io) {
  const bool open = g_open.load(std::memory_order_acquire);
  shown_ = std::clamp(shown_ + (open ? 1.0f : -1.0f) * std::min(io.DeltaTime, 0.1f) * 8.0f, 0.0f, 1.0f);
  const std::vector<Row> rows = Rows();
  for (uint8_t e; (e = Take());) {
    if (e == kNavUp) row_ = (row_ + int(rows.size()) - 1) % int(rows.size());
    if (e == kNavDown) row_ = (row_ + 1) % int(rows.size());
    if (e == kNavLeft) Change(rows[size_t(row_)], true, false);
    if (e == kNavRight) Change(rows[size_t(row_)], false, false);
    if (e == kNavSelect) Change(rows[size_t(row_)], false, true);
  }
  if (shown_ <= 0) return;

  const ImVec2 size = io.DisplaySize;
  const float u = std::min(size.x / 1920.0f, size.y / 1080.0f), a = shown_;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* item = MenuItemFont() ? MenuItemFont() : ImGui::GetFont();
  ImFont* body = MenuBodyFont() ? MenuBodyFont() : ImGui::GetFont();
  int headings = 0;
  for (const Row& r : rows) headings += r.section ? 1 : 0;
  const float row_h = 40 * u, head_h = 44 * u, w = 760 * u;
  const float h = 110 * u + rows.size() * row_h + headings * head_h + 70 * u;
  const float x = (size.x - w) * 0.5f, y0 = (size.y - h) * 0.5f;
  dl->AddRectFilled(ImVec2(0, 0), size, Fade(IM_COL32(0, 0, 0, 110), a));
  dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, y0 + h), Fade(IM_COL32(10, 9, 9, 235), a), 10 * u);
  dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + 8 * u, y0 + h), Fade(kAccent, a), 10 * u, ImDrawFlags_RoundCornersLeft);
  dl->AddText(item, 46 * u, ImVec2(x + 40 * u, y0 + 26 * u), Fade(kWhite, a), "QUICK MENU");
  dl->AddText(body, 20 * u, ImVec2(x + 330 * u, y0 + 44 * u), Fade(kGrey, a),
              SR_GAME == 2 ? "SAINTS ROW 2" : "SAINTS ROW");
  float y = y0 + 100 * u;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    if (r.section) {
      y += 10 * u;
      dl->AddText(body, 20 * u, ImVec2(x + 40 * u, y), Fade(kAccent, a), r.section);
      dl->AddRectFilled(ImVec2(x + 40 * u, y + 28 * u), ImVec2(x + w - 40 * u, y + 29 * u),
                        Fade(IM_COL32(255, 255, 255, 40), a));
      y += head_h - 10 * u;
    }
    const bool sel = int(i) == row_;
    if (sel)
      dl->AddRectFilled(ImVec2(x + 24 * u, y + 2 * u), ImVec2(x + w - 24 * u, y + row_h - 2 * u), Fade(kAccentDim, a),
                        4 * u);
    dl->AddText(item, 28 * u, ImVec2(x + 40 * u, y + 5 * u), Fade(sel ? kWhite : kGrey, a), r.label);
    const std::string v = sel && r.kind != kRowAction ? "<  " + ValueOf(r) + "  >"
                          : sel && r.index == kTimeOfDay ? "<  " + ValueOf(r) + "  >"
                                                         : ValueOf(r);
    const float vw = item->CalcTextSizeA(28 * u, FLT_MAX, 0, v.c_str()).x;
    dl->AddText(item, 28 * u, ImVec2(x + w - 40 * u - vw, y + 5 * u), Fade(sel ? kWhite : kGrey, a), v.c_str());
    y += row_h;
  }
  dl->AddText(body, 20 * u, ImVec2(x + 40 * u, y0 + h - 46 * u), Fade(kGrey, a),
              "UP DOWN  MOVE      LEFT RIGHT / X  CHANGE      O / L3+R3  CLOSE");
}

void UnlockRadnet() {
#if SR_GAME == 2
  // Saints Row 2's debug menu's "Unlock Radnet Events" (its scripts' fes_UnlockRadnetEventsDbg: the
  // five weeks' events' unlock keys posted, their collectible sets unlocked), once the game is
  // under way; its mission prerequisite override put back after, so events still wait for the
  // story missions they need.
  LuaRunOnce(R"LUA(if fes_UnlockRadnetEventsDbg == nil or mm_PostUnlockKey == nil or PLAYER == nil then return 'skip' end
if fe_GetHudLoaded ~= nil and not fe_GetHudLoaded() then return 'skip' end
if fe_IsMoviePlaying ~= nil and fe_IsMoviePlaying() then return 'skip' end
local ok, e = pcall(fes_UnlockRadnetEventsDbg)
if fes_SetRadnetMissionReqDbg ~= nil then pcall(fes_SetRadnetMissionReqDbg, false) end
return ok and 'unlocked' or ('failed: ' .. tostring(e))
)LUA",
             [](const std::string& out) {
               REXLOG_INFO("SAINTS ROW: ReInstated: Radnet events {}", out);
               if (out == "skip") UnlockRadnet();  // (still not in the game after 10 minutes: wait on)
             });
#endif
}

void StartQuickMenu(rex::ui::ImGuiDrawer* drawer) {
  if (g_menu || !drawer) return;
  LoadCheats();
  g_menu = std::make_unique<QuickMenu>(drawer);
  LuaSetTick(MakeTick, 200);
  // (once, in the log: what the game's Lua has, with the player in the game)
  LuaRunOnce(std::string("if go_GetHealth == nil then return 'skip' end\n") + kPlayerLua +
                 R"LUA(if P == nil then return 'skip' end
local function v(f, ...) if f == nil then return 'none' end local ok, r = pcall(f, ...) return tostring(ok) .. ':' .. tostring(r) end
return 'player ' .. type(P) .. ' ' .. tostring(P) .. ', health ' .. v(go_GetHealth, P) .. '/' .. v(go_GetMaxHealth, P) ..
  ', build ' .. v(fe_GetBuildType) .. ', platform ' .. v(fe_GetPlatformType) .. ', hud ' .. v(fe_GetHudLoaded) ..
  ', radnet pack ' .. v(dlc_IsRadnetPackageDownloaded) .. ', debug lib ' .. type(debug) ..
  ', EP ' .. v(db_GetInt, 'Sr_Save_EvolutionPoints', 0, 0) .. ', tod ' .. v(tod_GetTimeOfDay)
)LUA",
             [](const std::string& out) { REXLOG_INFO("SAINTS ROW: ReInstated: Lua probe: {}", out); });
#if SR_GAME == 2
  int radnet = 1, colour = 1, fog = 0;
  if (std::FILE* f = std::fopen(kMenuFile, "rb")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      std::sscanf(line, " radnet = %d", &radnet);
      std::sscanf(line, " colour_filter = %d", &colour);
      std::sscanf(line, " fog = %d", &fog);
    }
    std::fclose(f);
  }
  g_colour_off.store(colour == 0);
  g_fog.store(std::clamp(fog, 0, 2));
  if (radnet) UnlockRadnet();
#endif
}

}  // namespace srps5

// The title's XInputGetState: r3 the user, r4 the state; returns 0 when read.
#if SR_GAME == 1
extern "C" void sub_82B3DDA8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_82B3DDA8(ctx, base);
  if (user == 0 && state && ctx.r3.u32 == 0) srps5::Filter(base, state);
}
#else
extern "C" void sub_822C7630(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_822C7630(ctx, base);
  if (user == 0 && state && ctx.r3.u32 == 0) srps5::Filter(base, state);
}

// The time of day's tod_BlendColourGrade(grade, blend time): see "Saints Row 2's look".
extern "C" void sub_824B9468(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace srps5;
  int32_t grade = ctx.r4.s32;
  if (grade == kAgain) {
    grade = g_grade.load();
    if (grade == INT32_MIN) {
      if (!g_colour_off.load()) return;  // (none asked for yet: nothing to put back)
      grade = -1;
    }
  } else {
    g_grade.store(grade);
  }
  ctx.r4.s64 = g_colour_off.load() ? -1 : grade;
  __imp__sub_824B9468(ctx, base);
}

// The time of day's tod_FogDensityScaler(scale).
extern "C" void sub_822D0478(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace srps5;
  double scale = ctx.f1.f64;
  if (scale == double(kAgain)) {
    scale = g_fog_scale.load();
  } else {
    g_fog_scale.store(scale);
  }
  ctx.f1.f64 = double(float(scale * kFogScales[g_fog.load()]));
  __imp__sub_822D0478(ctx, base);
}
#endif

#endif  // REX_PLATFORM_PS5
