// SAINTS ROW: ReInstated (PS5) - the app's own text in English or Brazilian Portuguese: the menu, the quick menu, the
// installer's messages, the trophy pop-up. Portuguese while SETTINGS > LANGUAGE is PORTUGUESE (BR)
// (the PS5's language picks it on a first run); the two side by side where they're used,
// T("English", "Português"), as Crackdown: ReActivated's cd_text.h. No disc has Portuguese: the
// games themselves stay in English.
#pragma once

#include <atomic>
#include <cstring>
#include <string>

namespace srps5 {

inline std::atomic<bool> g_portuguese{false};

inline void SetPortuguese(bool on) { g_portuguese.store(on, std::memory_order_relaxed); }
inline bool Portuguese() { return g_portuguese.load(std::memory_order_relaxed); }
inline const char* T(const char* en, const char* pt) { return Portuguese() ? pt : en; }

// A word from the menus' fixed tables (values, languages, trophy grades) in Portuguese while it is.
inline const char* TW(const char* en) {
  static constexpr const char* kWords[][2] = {
      {"ON", "LIGADO"}, {"OFF", "DESLIGADO"}, {"LOW", "BAIXA"}, {"MEDIUM", "MÉDIA"}, {"HIGH", "ALTA"},
      {"SIMPLE", "SIMPLES"}, {"DETAILED", "DETALHADO"}, {"ENGLISH", "INGLÊS"}, {"FRENCH", "FRANCÊS"},
      {"GERMAN", "ALEMÃO"}, {"ITALIAN", "ITALIANO"}, {"SPANISH", "ESPANHOL"}, {"POLISH", "POLONÊS"},
      {"RUSSIAN", "RUSSO"}, {"PORTUGUESE (BR)", "PORTUGUÊS (BR)"}, {"Platinum", "Platina"}, {"Gold", "Ouro"},
      {"Silver", "Prata"}, {"Bronze", "Bronze"}};
  if (Portuguese())
    for (const auto& w : kWords)
      if (!std::strcmp(w[0], en)) return w[1];
  return en;
}

// Upper case in UTF-8, the accented letters too (Latin-1's: á -> Á, ç -> Ç, õ -> Õ).
inline std::string Upper(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char& c = reinterpret_cast<unsigned char&>(s[i]);
    if (c >= 'a' && c <= 'z') {
      c = static_cast<unsigned char>(c - 32);
    } else if (c == 0xC3 && i + 1 < s.size()) {
      unsigned char& n = reinterpret_cast<unsigned char&>(s[i + 1]);
      if (n >= 0xA0 && n <= 0xBE && n != 0xB7) n = static_cast<unsigned char>(n - 0x20);
      ++i;
    }
  }
  return s;
}

}  // namespace srps5
