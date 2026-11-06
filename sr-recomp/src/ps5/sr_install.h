// SAINTS ROW: ReInstated (PS5) - the installer the menu's CONTENT and SAVES pages run (Crackdown
// PS5's cd_install, by way of Max Payne 3 RePrescribed's and Metal Gear Rising RePlatinum's), for
// two games: what's in the app's install folder (<app>/install), what's installed, and the jobs
// that install, uninstall, import and back up, on a thread of their own.
//
//  - The discs: a disc image (.iso, any name) or a folder of its files (anywhere in install/),
//    told apart by their default.xex's title id: Saints Row's (545107D1) into <app>/game1,
//    Saints Row 2's (545107FC) into <app>/game2.
//  - The title updates the programs were built from (Saints Row's Title Update 1, Saints Row 2's
//    Title Update 3): default.xexp beside the disc's default.xex, the rest into <game>/update/.
//    Another update of either is listed as the wrong one.
//  - The DLC packages (any number, either game): each into <app>/dlc1|dlc2/<its content id>/, the
//    name the Xbox 360 gives it, whatever the file is called.
//  - The PC 4K texture packs (their TexMod packages, .tpf, as extracted): the HD textures in them,
//    into <app>/textures (sr_textures_install.cpp).
//  - Saves: Xbox 360 save packages of either game, into <app>/saves the way the runtime keeps
//    them; and the app's own backups (install/backups/...), restored.
//
// Everything is found from what it is (an image's file system, a package's header), never from
// its name. An install is written next to its place and moved in when complete, so an interrupted
// one leaves nothing half done. Archives (.7z, .rar, .zip) are listed to be extracted on a PC
// first.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace srps5::install {

namespace fs = std::filesystem;

enum class Kind {
  kGameImage,    // a disc image
  kGameFolder,   // a folder with default.xex
  kUpdate,      // a title update package (the one the program needs)
  kDlc,
  kSave,         // an Xbox 360 save package
  kBackup,       // one of this app's backups
  kTexturePack,  // a PC texture pack's TexMod package (.tpf) the list knows: HD textures from it
  kArchive,      // .7z / .rar / .zip: extract on a PC
  kOther,        // another game's package, or not known
};

struct Source {
  Kind kind = Kind::kOther;
  fs::path path;
  std::string label;  // for the menu
  int game = 0;            // whose: 1 Saints Row, 2 Saints Row 2 (0: not known yet, a disc image)
  bool installed = false;  // (kDlc) already installed from it: only to delete or keep
};

struct SaveInfo {
  std::string name;   // the save's folder
  int game = 1;
  int64_t modified = 0;  // (seconds since 1970)
};

struct Installed {
  bool game[3] = {};    // [1] game1/, [2] game2/
  bool update[3] = {};  // its title update (default.xexp) beside it
  int dlc = 0;          // DLC packs in dlc1/ and dlc2/
  int textures = 0;   // HD textures in textures/
  int saves[3] = {};  // each game's
  std::vector<SaveInfo> save_list;  // (the newest first)
};

// The games' names, as the menu shows them ([1], [2]).
inline constexpr const char* kGameNames[3] = {"", "Saints Row", "Saints Row 2"};

fs::path Folder(const fs::path& app);  // <app>/install (made if missing)
Installed Check(const fs::path& app);
std::vector<Source> Scan(const fs::path& app);
// The ones installed without being asked, as the menu opens: the discs, the DLC.
bool Automatic(Kind kind);

// The label of an Xbox 360 avatar item's package (Kind::kOther): the menu counts them in one line.
inline constexpr const char* kAvatarItemLabel = "avatar item";

enum class Remove { kGame1, kGame2, kDlc, kTextures };

// The HD texture packs (sr_textures_install.cpp, from hub/sr-textures.txt): whether `file`
// is a package the list knows (its game, and whether it's been installed), the bytes its textures
// take, how many are installed, and the install itself.
bool TexturePackOf(const fs::path& app, const fs::path& file, int& game, bool& installed);
uint64_t TexturePackBytes(const fs::path& app, const fs::path& file);
int TexturesInstalled(const fs::path& app);
bool InstallTexturePack(const fs::path& app, const fs::path& file, std::atomic<uint64_t>& done,
                        std::atomic<uint64_t>& total, std::string& result);

// One job at a time, on its own thread.
class Job {
 public:
  bool Busy() const { return busy_.load(std::memory_order_acquire); }
  // (0..1; with Busy)
  float Progress() const;
  std::string Title() const;
  // The last finished job's outcome ("" none yet) and whether it failed.
  std::string Result() const;
  bool Failed() const { return failed_.load(std::memory_order_acquire); }
  // Bumped when a job finishes (the menu looks at the folders again).
  uint32_t Generation() const { return generation_.load(std::memory_order_acquire); }

  bool Install(const fs::path& app, const Source& source);
  bool Uninstall(const fs::path& app, Remove what);
  bool Backup(const fs::path& app);
  // The logs (<app>/logs) sent for a bug report by the logs' payload, `note` (what went wrong, one
  // line) at their top; the result has the link.
  bool SendLogs(const fs::path& app, const std::string& note);
  // The last link SEND LOGS got (empty: none yet).
  std::string LogsLink() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return logs_link_;
  }

  // What the installs took from install/ (disc images and DLC packages): kept there
  // until the user says to delete them (DeleteLeftovers) or to keep them (KeepLeftovers: noted in
  // install/kept.txt, so they aren't installed again).
  std::vector<fs::path> Leftovers() const;
  uint64_t LeftoverBytes() const;
  bool DeleteLeftovers(const fs::path& app);
  void KeepLeftovers(const fs::path& app);

 private:
  template <typename F>
  bool Start(std::string title, F&& work);

  std::atomic<bool> busy_{false}, failed_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint64_t> done_{0}, total_{0};
  mutable std::mutex mutex_;
  std::string title_, result_, logs_link_;
  std::vector<fs::path> leftovers_;

 public:
  void Leftover(const fs::path& path);
};

Job& TheJob();

// The installer payload (sr_install.cpp): one job run by it (kUnavailable:
// no payload loader), what it staged moved into place, a path as one inside the app's folder.
namespace payload {
enum class Ran { kUnavailable, kDone, kFailed };
inline constexpr const char* kJobRel = "UserData/sr/.install-job";
inline constexpr const char* kStagingRel = "UserData/sr/.install-staging";
Ran Run(const fs::path& app, const std::string& body, std::atomic<uint64_t>& done, std::atomic<uint64_t>& total,
        std::string& error);
bool Take(const fs::path& app, const std::string& rel, const fs::path& to, std::string& error);
std::string Inside(const fs::path& app, const fs::path& path);
inline fs::path JobFolder(const fs::path& app) { return app / kJobRel; }
}  // namespace payload

// Saves imported before the game made its profile folder wait in
// saves/_import/<title>/; at the game's start (`game`'s) they move into saves/<profile>/.
void PlacePendingSaves(const fs::path& saves_root, uint64_t xuid, int game);

}  // namespace srps5::install

#endif  // REX_PLATFORM_PS5
