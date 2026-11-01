// SAINTS ROW: ReInstated (PS5) - what the app draws over the game: the frame
// rate counter (GTA IV PS5's), when it's on in the menu's SETTINGS - on the left
// edge, frames a second and the frame time, and in its detailed form also the
// slowest frames and a graph of the last 120.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <cstdint>

#include <rex/ui/imgui_dialog.h>

namespace srps5 {

class GameOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit GameOverlay(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;
  // (drawn with each game frame, not every vblank: GTA IV PS5 found repaints at
  // every vblank queued repeats ahead of each new frame)
  bool WantsContinuousRepaint() const override { return false; }

 private:
  void Sample();

  // the counter: averaged over half a second
  uint64_t window_frames_ = 0;
  int64_t window_start_ns_ = 0;
  float fps_ = 0, ms_ = 0;
  float low_fps_ = 0, worst_ms_ = 0;  // (the 1% slowest frames, and the slowest)
};

}  // namespace srps5

#endif  // REX_PLATFORM_PS5
