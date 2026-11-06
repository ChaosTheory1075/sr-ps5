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

#include <SDL3/SDL_timer.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/sony_feedback.h>
#include <rex/logging.h>

#include "sr_lua.h"
#include "sr_menu.h"
#include "sr_text.h"

#include SR_INIT_H

REXCVAR_DECLARE(std::string, sr_fps_counter);
REXCVAR_DEFINE_BOOL(sr_quick_menu, true, "SAINTS ROW",
                    "L3 + R3 opens the in-game quick menu (the cheats); off: no menu, and no cheats");

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
  // (the log: the first read, and the first presses of L3 / R3 - whether the combo reaches here)
  static std::atomic<int> reads{0}, stick_presses{0};
  if (reads.fetch_add(1) == 0) REXLOG_INFO("Quick menu: reading the controller (CHEAT MENU {})", REXCVAR_GET(sr_quick_menu) ? "on" : "off");
  if ((pressed & (kL3 | kR3)) && stick_presses.fetch_add(1) < 20)
    REXLOG_INFO("Quick menu: {}{} pressed (held {:04X})", (pressed & kL3) ? "L3" : "", (pressed & kR3) ? "R3" : "", buttons);
  const bool combo = (buttons & (kL3 | kR3)) == (kL3 | kR3);
  const bool combo_pressed = combo && (pressed & (kL3 | kR3));
  g_previous = buttons;
  if (!g_open.load(std::memory_order_relaxed)) {
    if (combo_pressed && REXCVAR_GET(sr_quick_menu)) {
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
// (their names in the menu's Portuguese, in kCheats' order)
constexpr const char* kCheatsPt[kCheatCount] = {"Modo Deus", "Sem notoriedade com a polícia", "Inimigos ignoram você"};
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

// The player, as the scripts name him: Saints Row's "#PLAYER#"; Saints Row 2's LOCAL_PLAYER
// ("#PLAYER1#", ug_lib.lua). And each game's notoriety teams, as their scripts reset them.
#if SR_GAME == 1
constexpr const char* kPlayer = "#PLAYER#";
constexpr const char* kGangs[] = {"police", "los_carnales", "vice_kings", "rollers"};
#else
constexpr const char* kPlayer = "#PLAYER1#";
constexpr const char* kGangs[] = {"police", "ronin", "brotherhood", "samedi"};
#endif

LuaCall Call(const char* function, std::vector<LuaArg> args = {}) { return {function, std::move(args)}; }
LuaCall OnPlayer(const char* function) { return Call(function, {LuaArg::Str(kPlayer)}); }

// The tick: the cheats held, and the player's state for the DualSense. The games' own script API,
// as their missions call it: get_max_hit_points / get_current_hit_points / character_is_dead,
// turn_invulnerable / turn_vulnerable, notoriety_reset("police"), set_ignore_ai_flag.
// (results: [0] max, [1] health, [2] dead (Saints Row 2 only); then the cheats')
struct Held {
  uint32_t generation = 0;
  bool god = false, ignore = false;
};
Held g_held;  // (the game thread's: the tick's make and done run there)

std::vector<LuaCall> MakeTick() {
  const uint32_t gen = g_generation.load();
  const bool again = g_held.generation != gen;
  const bool god = g_on[kGod].load(), ignore = g_on[kIgnored].load();
  // Nothing held and nothing to undo: no calls at all. (Calling into the scripts five times a
  // second while no cheat was on kept Saints Row's first mission from starting at its marker.)
  if (!god && !ignore && !g_on[kNoCops].load() && !g_held.god && !g_held.ignore) {
    g_held.generation = gen;
    return {};
  }
  // (Saints Row has no character_is_dead)
  std::vector<LuaCall> calls = {OnPlayer("get_max_hit_points"), OnPlayer("get_current_hit_points")};
  if (SR_GAME == 2) calls.push_back(OnPlayer("character_is_dead"));
  if (god && (again || !g_held.god)) calls.push_back(OnPlayer("turn_invulnerable"));
  if (!god && g_held.god) calls.push_back(OnPlayer("turn_vulnerable"));
  if (g_on[kNoCops].load()) calls.push_back(Call("notoriety_reset", {LuaArg::Str("police")}));
  if (ignore != g_held.ignore || (ignore && again))
    calls.push_back(Call("set_ignore_ai_flag", {LuaArg::Str(kPlayer), LuaArg::Bool(ignore)}));
  g_held = {gen, god, ignore};
  return calls;
}

void TickDone(const std::vector<LuaResult>& out) {
  PlayerStatus s;
  s.updated_ms = SDL_GetTicks();
  if (out.size() >= 2 && out[0].ok && out[1].ok && out[0].value > 0) {
    s.max_health = float(out[0].value);
    s.health = float(out[1].value);
    s.in_game = !(SR_GAME == 2 && out.size() > 2 && out[2].ok && out[2].value == 1);
  }
  SetPlayerStatus(s);
}

// ---- the one-off actions ----

enum Action { kCash, kClearNotoriety, kToggleHud, kActionCount };
bool g_hud_hidden = false;  // (the UI thread's)

void LogResults(const char* what, const std::vector<LuaResult>& out) {
  int ok = 0;
  for (const LuaResult& r : out) ok += r.ok ? 1 : 0;
  REXLOG_INFO("Quick menu: {}: {} of {} calls ran", what, ok, out.size());
}

void RunAction(int action) {
  // (each batch starts with the player's health: not run until the player is in the game)
  std::vector<LuaCall> calls = {OnPlayer("get_max_hit_points")};
  switch (action) {
    case kCash:
      calls.push_back(Call("cash_add", {LuaArg::Num(10000)}));
      LuaRunOnce(calls, [](const std::vector<LuaResult>& out) { LogResults("cash", out); });
      break;
    case kClearNotoriety:
      for (const char* gang : kGangs) calls.push_back(Call("notoriety_reset", {LuaArg::Str(gang)}));
      LuaRunOnce(calls, [](const std::vector<LuaResult>& out) { LogResults("notoriety", out); });
      break;
    case kToggleHud:
      g_hud_hidden = !g_hud_hidden;
      calls.push_back(Call("hud_hide", {LuaArg::Bool(g_hud_hidden)}));
      LuaRunOnce(calls, [](const std::vector<LuaResult>& out) { LogResults("HUD", out); });
      break;
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
    rows.push_back({first ? T("CHEATS", "TRAPAÇAS") : nullptr, T(kCheats[i].label, kCheatsPt[i]), kRowCheat, i});
    first = false;
  }
  rows.push_back({T("ACTIONS", "AÇÕES"), T("Clear Notoriety", "Limpar notoriedade"), kRowAction, kClearNotoriety});
#if SR_GAME == 2
  // (Saints Row's scripts have no cash_add or hud_hide)
  rows.push_back({nullptr, T("Give $10,000", "Ganhar $10.000"), kRowAction, kCash});
  rows.push_back({nullptr, T("Show / Hide HUD", "Mostrar / esconder HUD"), kRowAction, kToggleHud});
#endif
  rows.push_back({T("DISPLAY", "TELA"), T("FPS Counter", "Contador de FPS"), kRowFps, 0});
  rows.push_back({T("CONTROLLER", "CONTROLE"), T("DualSense Features", "Recursos do DualSense"), kRowDualSense, 0});
  return rows;
}

int CounterMode() {
  const std::string m = REXCVAR_GET(sr_fps_counter);
  return m == "detailed" ? 2 : m == "simple" ? 1 : 0;
}

std::string ValueOf(const Row& row) {
  switch (row.kind) {
    case kRowCheat: return g_on[row.index].load() ? T("ON", "LIGADO") : T("OFF", "DESLIGADO");
    case kRowAction: return T("PRESS X", "APERTE X");
    case kRowFps: return CounterMode() == 2 ? T("DETAILED", "DETALHADO") : CounterMode() == 1 ? T("SIMPLE", "SIMPLES") : T("OFF", "DESLIGADO");
    case kRowDualSense: return rex::input::sony::GetOptions().enabled ? T("ON", "LIGADO") : T("OFF", "DESLIGADO");
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
  dl->AddText(item, 46 * u, ImVec2(x + 40 * u, y0 + 26 * u), Fade(kWhite, a), T("QUICK MENU", "MENU RÁPIDO"));
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
              T("UP DOWN  MOVE      LEFT RIGHT / X  CHANGE      O / L3+R3  CLOSE", "CIMA BAIXO  MOVER      ESQ. DIR. / X  MUDAR      O / L3+R3  FECHAR"));
}

void StartQuickMenu(rex::ui::ImGuiDrawer* drawer) {
  if (g_menu || !drawer) return;
  LoadCheats();
  g_menu = std::make_unique<QuickMenu>(drawer);
  REXLOG_INFO("Quick menu: ready, CHEAT MENU {} (L3 + R3 in the game)", REXCVAR_GET(sr_quick_menu) ? "on" : "off");
  LuaSetTick(MakeTick, TickDone, 200);
  // (once, in the log: the player's health as the game's own functions give it)
  LuaRunOnce({OnPlayer("get_max_hit_points"), OnPlayer("get_current_hit_points")},
             [](const std::vector<LuaResult>& out) {
               REXLOG_INFO("SAINTS ROW: ReInstated: Lua probe: max health {} ({}), health {} ({})",
                           out.size() > 0 ? out[0].value : 0.0, out.size() > 0 && out[0].ok,
                           out.size() > 1 ? out[1].value : 0.0, out.size() > 1 && out[1].ok);
             });
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
