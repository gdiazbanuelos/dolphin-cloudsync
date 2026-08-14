// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCMemcard/CloudSync.h"

#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GCMemcard/RcloneUtils.h"

namespace Memcard
{
static bool RunRcloneSync(const std::vector<std::string>& args)
{
#ifdef _WIN32
  std::string cmdline = "rclone";
  for (const std::string& arg : args)
    cmdline += fmt::format(" \"{}\"", arg);

  const std::wstring wcmd = UTF8ToWString(cmdline);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, const_cast<wchar_t*>(wcmd.c_str()), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
  {
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return true;
#else
  const std::string rclone_path = FindRclonePath();
  if (rclone_path.empty())
    return false;

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(rclone_path.c_str()));
  for (const std::string& arg : args)
    argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  pid_t pid;
  if (posix_spawnp(&pid, rclone_path.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
    return false;

  int status;
  waitpid(pid, &status, 0);
  return true;
#endif
}

// Runs an rclone list command against a cloud path and returns its stdout. Returns nullopt if
// rclone couldn't be located (non-Windows only — Windows relies on rclone being on PATH and
// can't distinguish "not found" from "ran but produced no output").
static std::optional<std::string> RunRcloneList(const std::string& args)
{
  std::string listing;
#ifdef _WIN32
  {
    const std::wstring wcmd = UTF8ToWString(fmt::format("rclone {}", args));
    HANDLE read_pipe, write_pipe;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (CreatePipe(&read_pipe, &write_pipe, &sa, 0))
    {
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
      si.hStdOutput = write_pipe;
      si.hStdError = write_pipe;
      si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi{};
      if (CreateProcessW(nullptr, const_cast<wchar_t*>(wcmd.c_str()), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
      {
        CloseHandle(write_pipe);
        char buf[512];
        DWORD bytes_read;
        while (ReadFile(read_pipe, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read > 0)
        {
          buf[bytes_read] = '\0';
          listing += buf;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
      }
      else
      {
        CloseHandle(write_pipe);
      }
      CloseHandle(read_pipe);
    }
  }
#else
  {
    const std::string rclone_path = FindRclonePath();
    if (rclone_path.empty())
      return std::nullopt;

    const std::string cmd = fmt::format("\"{}\" {} 2>/dev/null", rclone_path, args);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe)
    {
      char buf[512];
      while (fgets(buf, sizeof(buf), pipe))
        listing += buf;
      pclose(pipe);
    }
  }
#endif
  return listing;
}

static std::string BuildRcloneRemoteDir(u32 game_id)
{
  const u32 swapped = Common::swap32(game_id);
  const std::string game_id_str(reinterpret_cast<const char*>(&swapped), 4);
  const std::string suffix = fmt::format("({})", game_id_str);
  const std::string cloud_root =
      Config::Get(Config::MAIN_CLOUDSYNC_REMOTE) + ":Dolphin Cloud Saves";

  // Scan the cloud root for any existing folder ending in (game_id).
  // This ensures saves are found regardless of title name or platform differences.
  const std::optional<std::string> listing =
      RunRcloneList(fmt::format("lsf \"{}\" --dirs-only", cloud_root));
  if (!listing)
    return fmt::format("{}/{} {}", cloud_root, game_id_str, suffix);

  std::istringstream stream(*listing);
  std::string entry;
  while (std::getline(stream, entry))
  {
    while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r' || entry.back() == '/'))
      entry.pop_back();
    if (entry.size() >= suffix.size() && entry.substr(entry.size() - suffix.size()) == suffix)
      return fmt::format("{}/{}", cloud_root, entry);
  }

  // Nothing found — build a name from title or game ID for first upload.
  const std::string title = SConfig::GetInstance().GetTitleName();
  if (!title.empty())
    return fmt::format("{}/{}", cloud_root,
                       Common::EscapeFileName(fmt::format("{} {}", title, suffix)));

  return fmt::format("{}/{} {}", cloud_root, game_id_str, suffix);
}

// Returns true if the remote directory for this game has no files (new or empty remote).
static bool CloudRemoteIsEmpty(const std::string& remote_dir)
{
  const std::optional<std::string> listing = RunRcloneList(fmt::format("lsf \"{}\"", remote_dir));
  if (!listing)
    return false;
  return listing->find_first_not_of(" \t\r\n") == std::string::npos;
}

void PullSaveFromCloud(u32 game_id, const std::string& local_dir)
{
  if (!Config::Get(Config::MAIN_CLOUDSYNC_ENABLED))
    return;

  const std::string remote_dir = BuildRcloneRemoteDir(game_id);

  // If the remote is empty/new, push this game's local saves first so nothing is lost.
  // local_dir contains all games' .gci files, so filter by the 4-char game code prefix.
  if (CloudRemoteIsEmpty(remote_dir))
  {
    const u32 swapped = Common::swap32(game_id);
    const std::string game_code(reinterpret_cast<const char*>(&swapped), 4);
    const std::string include_pattern = game_code + "*.gci";
    const std::string remote_name = Config::Get(Config::MAIN_CLOUDSYNC_REMOTE);
    std::thread([local_dir, remote_dir, remote_name, include_pattern] {
      if (RunRcloneSync(
              {"copy", local_dir, remote_dir, "--include", include_pattern, "--no-traverse"}))
        Core::DisplayMessage(fmt::format("Initial upload to {}", remote_name), 4000);
    }).detach();
    return;
  }

  if (RunRcloneSync({"copy", remote_dir, local_dir, "--update", "--no-traverse"}))
    Core::DisplayMessage(
        fmt::format("Pulled latest save from {}", Config::Get(Config::MAIN_CLOUDSYNC_REMOTE)),
        4000);
}

void PushSaveToCloud(u32 game_id, const std::string& gci_path)
{
  if (!Config::Get(Config::MAIN_CLOUDSYNC_ENABLED))
    return;

  const std::string remote_dir = BuildRcloneRemoteDir(game_id);
  std::thread([remote_dir, gci_path] {
    if (RunRcloneSync({"copy", gci_path, remote_dir, "--no-traverse"}))
      Core::DisplayMessage(
          fmt::format("Wrote save to {}", Config::Get(Config::MAIN_CLOUDSYNC_REMOTE)), 4000);
  }).detach();
}
}  // namespace Memcard
