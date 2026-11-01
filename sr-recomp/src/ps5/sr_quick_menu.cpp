// SAINTS ROW: ReInstated (PS5) - the in-game quick menu (see sr_quick_menu.h).
//
// The controller is read where the game reads it: the title's XInputGetState (Saints Row 8272CE88,
// Saints Row 2 82AA61D0: r3 the user, r4 the X_INPUT_STATE; they call XamInputGetState). After it runs, the menu
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

enum Cheat { kGod, kNoCops, kIgnored, kCheatCount };
struct CheatInfo {
  const char* key;    // in cheats<N>.ini
  const char* label;  // in the menu
  bool p1;            // (Saints Row has it too)
};
constexpr CheatInfo kCheats[kCheatCount] = {
    {"god_mode", "God Mode", true},
    {"no_cops", "No Police Notoriety", true},
    {"enemies_ignore_you", "Enemies Ignore You", true},
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

// The player, in the chunks (Lua): the scripts' LOCAL_PLAYER ("#PLAYER1#", ug_lib.lua).
constexpr const char* kPlayerLua = "local P = LOCAL_PLAYER or '#PLAYER1#'\n";

// The tick: the cheats held, and the player's state for the DualSense ("<health> <max> <in game>").
// The games' own script API, as their missions call it: turn_invulnerable / turn_vulnerable,
// notoriety_reset("police"), set_ignore_ai_flag, get_current_hit_points / get_max_hit_points.
std::string MakeTick() {
  char head[160];
  std::snprintf(head, sizeof(head), "local god, nocops, ignore, gen = %d, %d, %d, %u\n", g_on[kGod].load() ? 1 : 0,
                g_on[kNoCops].load() ? 1 : 0, g_on[kIgnored].load() ? 1 : 0, g_generation.load());
  return std::string(head) + "if get_current_hit_points == nil or get_max_hit_points == nil then return 'skip' end\n" +
         kPlayerLua + R"LUA(local function try(f, ...) if f then local ok, r = pcall(f, ...) if ok then return r end end end
local m = try(get_max_hit_points, P) or 0
local h = try(get_current_hit_points, P) or 0
if m <= 0 then return 'none' end
local R = __ReInstated
if R == nil then R = {} __ReInstated = R end
local again = R.gen ~= gen
R.gen = gen
if god == 1 then
  if again or not R.god then try(turn_invulnerable, P) R.god = true end
elseif R.god then
  try(turn_vulnerable, P) R.god = nil
end
if nocops == 1 then try(notoriety_reset, 'police') end
if ignore == 1 then
  if again or not R.ignore then try(set_ignore_ai_flag, P, true) R.ignore = true end
elseif R.ignore then
  try(set_ignore_ai_flag, P, false) R.ignore = nil
end
local g = 1
if try(character_is_dead, P) then g = 0 end
return string.format('%.3f %.3f %d', h, m, g)
)LUA";
}

// ---- the one-off actions ----

enum Action { kCash, kClearNotoriety, kToggleHud, kActionCount };
bool g_hud_hidden = false;  // (the UI thread's)

void LogResult(const char* what, const std::string& out) {
  REXLOG_INFO("Quick menu: {}: {}", what, out);
}

void RunAction(int action) {
  const std::string player = std::string(kPlayerLua) + "if get_max_hit_points == nil then return 'skip' end\n";
  switch (action) {
    case kCash:
      LuaRunOnce(player + "if cash_add == nil then return 'no cash_add' end\n"
                          "local ok, e = pcall(cash_add, 10000)\nreturn ok and 'cash +10000' or tostring(e)\n",
                 [](const std::string& out) { LogResult("cash", out); });
      break;
    case kClearNotoriety:
      LuaRunOnce(player + R"LUA(if notoriety_reset == nil then return 'no notoriety_reset' end
for _, t in ipairs({'police', 'ronin', 'brotherhood', 'samedi', 'los carnales', 'rollerz', 'vice kings', 'westsiders'}) do
  pcall(notoriety_reset, t)
end
return 'cleared'
)LUA",
                 [](const std::string& out) { LogResult("notoriety", out); });
      break;
    case kToggleHud: {
      g_hud_hidden = !g_hud_hidden;
      LuaRunOnce(player + "if hud_hide == nil then return 'no hud_hide' end\nlocal ok, e = pcall(hud_hide, " +
                     std::string(g_hud_hidden ? "true" : "false") + ")\nreturn ok and 'done' or tostring(e)\n",
                 [](const std::string& out) { LogResult("HUD", out); });
      break;
    }
  }
}

// ---- the rows ----

enum RowKind { kRowCheat, kRowAction, kRowFps, kRowDualSense };
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
  rows.push_back({"ACTIONS", "Give $10,000", kRowAction, kCash});
  rows.push_back({nullptr, "Clear Notoriety", kRowAction, kClearNotoriety});
  rows.push_back({nullptr, "Show / Hide HUD", kRowAction, kToggleHud});
  rows.push_back({"DISPLAY", "FPS Counter", kRowFps, 0});
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
    case kRowAction: return "PRESS X";
    case kRowFps: return CounterMode() == 2 ? "DETAILED" : CounterMode() == 1 ? "SIMPLE" : "OFF";
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
      if (select) RunAction(row.index);
      break;
    case kRowFps: {
      static constexpr const char* kModes[] = {"off", "simple", "detailed"};
      const int mode = (CounterMode() + (back ? 2 : 1)) % 3;
      rex::cvar::SetFlagByName("sr_fps_counter", kModes[mode]);
      SetMenuValue("fps_counter", mode);
      break;
    }
    case kRowDualSense: {
      const bool on = !rex::input::sony::GetOptions().enabled;
      rex::input::sony::SetFeaturesEnabled(on);
      SetMenuValue("dualsense", on ? 1 : 0);
      break;
    }
  }
}

constexpr ImU32 kWhite = IM_COL32(236, 232, 226, 255), kGrey = IM_COL32(150, 146, 140, 255),
                kAccent = IM_COL32(150, 60, 230, 255), kAccentDim = IM_COL32(150, 60, 230, 50);

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
    const std::string v = sel && r.kind != kRowAction ? "<  " + ValueOf(r) + "  >" : ValueOf(r);
    const float vw = item->CalcTextSizeA(28 * u, FLT_MAX, 0, v.c_str()).x;
    dl->AddText(item, 28 * u, ImVec2(x + w - 40 * u - vw, y + 5 * u), Fade(sel ? kWhite : kGrey, a), v.c_str());
    y += row_h;
  }
  dl->AddText(body, 20 * u, ImVec2(x + 40 * u, y0 + h - 46 * u), Fade(kGrey, a),
              "UP DOWN  MOVE      LEFT RIGHT / X  CHANGE      O / L3+R3  CLOSE");
}

void StartQuickMenu(rex::ui::ImGuiDrawer* drawer) {
  if (g_menu || !drawer) return;
  LoadCheats();
  g_menu = std::make_unique<QuickMenu>(drawer);
  LuaSetTick(MakeTick, 200);
  // (once, in the log: what the game's Lua has, with the player in the game)
  LuaRunOnce(std::string("if get_max_hit_points == nil then return 'skip' end\n") + kPlayerLua +
                 R"LUA(local function v(f, ...) if f == nil then return 'none' end local ok, r = pcall(f, ...) return tostring(ok) .. ':' .. tostring(r) end
return 'player ' .. tostring(P) .. ', health ' .. v(get_current_hit_points, P) .. '/' .. v(get_max_hit_points, P) ..
  ', turn_invulnerable ' .. type(turn_invulnerable) .. ', notoriety_reset ' .. type(notoriety_reset) ..
  ', set_ignore_ai_flag ' .. type(set_ignore_ai_flag) .. ', cash_add ' .. type(cash_add) .. ', hud_hide ' .. type(hud_hide)
)LUA",
             [](const std::string& out) { REXLOG_INFO("SAINTS ROW: ReInstated: Lua probe: {}", out); });
}

}  // namespace srps5

// The title's XInputGetState: r3 the user, r4 the state; returns 0 when read.
#if SR_GAME == 1
extern "C" void sub_8272CE88(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_8272CE88(ctx, base);
  if (user == 0 && state && ctx.r3.u32 == 0) srps5::Filter(base, state);
}
#else
extern "C" void sub_82AA61D0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_82AA61D0(ctx, base);
  if (user == 0 && state && ctx.r3.u32 == 0) srps5::Filter(base, state);
}
#endif

#endif  // REX_PLATFORM_PS5
