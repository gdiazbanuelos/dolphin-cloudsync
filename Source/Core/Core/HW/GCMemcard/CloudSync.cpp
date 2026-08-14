// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCMemcard/CloudSync.h"

#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "Common/CommonTypes.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GCMemcard/RcloneUtils.h"

namespace Memcard
{
// Maximum time to wait for a single rclone invocation before giving up and forcibly killing it,
// so a stalled network connection can't hang save writes or background uploads indefinitely.
constexpr std::chrono::milliseconds RCLONE_TIMEOUT{30000};

// Maximum time to wait for each rclone invocation made synchronously on the boot path (see
// PullSaveFromCloud), kept much shorter than RCLONE_TIMEOUT so a slow or unreachable remote can't
// stall starting a game for the full 30 seconds.
constexpr std::chrono::milliseconds RCLONE_BOOT_TIMEOUT{8000};

// Not gated behind _WIN32: this is pure string logic with no platform dependency, so it can be
// unit tested on any host, even though only the Windows process-spawning code below calls it.
std::string EscapeWindowsArg(const std::string& arg)
{
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
    return arg;

  std::string escaped = "\"";
  for (auto it = arg.begin();; ++it)
  {
    size_t backslashes = 0;
    while (it != arg.end() && *it == '\\')
    {
      ++it;
      ++backslashes;
    }

    if (it == arg.end())
    {
      escaped.append(backslashes * 2, '\\');
      break;
    }
    else if (*it == '"')
    {
      escaped.append(backslashes * 2 + 1, '\\');
      escaped += *it;
    }
    else
    {
      escaped.append(backslashes, '\\');
      escaped += *it;
    }
  }
  escaped += '"';
  return escaped;
}

std::string BuildWindowsCommandLine(const std::vector<std::string>& args)
{
  std::string cmdline;
  for (const std::string& arg : args)
  {
    if (!cmdline.empty())
      cmdline += ' ';
    cmdline += EscapeWindowsArg(arg);
  }
  return cmdline;
}

#ifndef _WIN32
// Waits for a child process to exit, forcibly killing it if it doesn't finish within timeout. On
// success, writes the process's exit code to *exit_code and returns true.
static bool WaitForChildWithTimeout(pid_t pid, int* exit_code, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    int status;
    if (waitpid(pid, &status, WNOHANG) == pid)
    {
      *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
  return false;
}
#endif

static bool RunRcloneSync(const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout = RCLONE_TIMEOUT)
{
#ifdef _WIN32
  std::vector<std::string> full_args{"rclone"};
  full_args.insert(full_args.end(), args.begin(), args.end());
  const std::wstring wcmd = UTF8ToWString(BuildWindowsCommandLine(full_args));
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
  const DWORD wait_result = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()));
  bool succeeded = false;
  if (wait_result == WAIT_OBJECT_0)
  {
    DWORD exit_code = 1;
    succeeded = GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0;
  }
  else
  {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return succeeded;
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

  int exit_code = -1;
  return WaitForChildWithTimeout(pid, &exit_code, timeout) && exit_code == 0;
#endif
}

// Runs an rclone list command against a cloud path and returns its stdout. Returns nullopt if
// rclone couldn't be located (non-Windows only — Windows relies on rclone being on PATH and
// can't distinguish "not found" from "ran but produced no output").
static std::optional<std::string> RunRcloneList(const std::vector<std::string>& args,
                                                std::chrono::milliseconds timeout = RCLONE_TIMEOUT)
{
  std::string listing;
#ifdef _WIN32
  {
    std::vector<std::string> full_args{"rclone"};
    full_args.insert(full_args.end(), args.begin(), args.end());
    const std::wstring wcmd = UTF8ToWString(BuildWindowsCommandLine(full_args));
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
        // Poll rather than call ReadFile directly so a stalled rclone process (e.g. hung on a
        // dead network connection) can't block this call forever.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char buf[512];
        while (true)
        {
          DWORD bytes_available = 0;
          if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &bytes_available, nullptr) &&
              bytes_available > 0)
          {
            DWORD bytes_read = 0;
            if (!ReadFile(read_pipe, buf, sizeof(buf) - 1, &bytes_read, nullptr) || bytes_read == 0)
              break;
            buf[bytes_read] = '\0';
            listing += buf;
            continue;
          }

          if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0 ||
              std::chrono::steady_clock::now() >= deadline)
          {
            break;
          }
          Sleep(20);
        }
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0)
          TerminateProcess(pi.hProcess, 1);
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

    int pipe_fds[2];
    if (pipe(pipe_fds) == 0)
    {
      std::vector<std::string> full_args{rclone_path};
      full_args.insert(full_args.end(), args.begin(), args.end());
      std::vector<char*> argv;
      for (const std::string& arg : full_args)
        argv.push_back(const_cast<char*>(arg.c_str()));
      argv.push_back(nullptr);

      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
      posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
      posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

      pid_t pid;
      const int spawn_result =
          posix_spawnp(&pid, rclone_path.c_str(), &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      close(pipe_fds[1]);

      if (spawn_result == 0)
      {
        // Poll with a deadline rather than calling read() directly so a stalled rclone process
        // (e.g. hung on a dead network connection) can't block this call forever.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char buf[512];
        while (true)
        {
          const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        deadline - std::chrono::steady_clock::now())
                                        .count();
          if (remaining_ms <= 0)
            break;

          pollfd pfd{pipe_fds[0], POLLIN, 0};
          if (poll(&pfd, 1, static_cast<int>(remaining_ms)) <= 0)
            break;

          const ssize_t bytes_read = read(pipe_fds[0], buf, sizeof(buf));
          if (bytes_read <= 0)
            break;
          listing.append(buf, bytes_read);
        }
        int exit_code;
        WaitForChildWithTimeout(pid, &exit_code, timeout);
      }
      close(pipe_fds[0]);
    }
  }
#endif
  return listing;
}

static std::string BuildRcloneRemoteDir(u32 game_id,
                                        std::chrono::milliseconds timeout = RCLONE_TIMEOUT)
{
  const u32 swapped = Common::swap32(game_id);
  const std::string game_id_str(reinterpret_cast<const char*>(&swapped), 4);
  const std::string suffix = fmt::format("({})", game_id_str);
  const std::string cloud_root = Config::Get(Config::MAIN_CLOUDSYNC_REMOTE) + ":" +
                                 Config::Get(Config::MAIN_CLOUDSYNC_REMOTE_FOLDER);

  // Scan the cloud root for any existing folder ending in (game_id).
  // This ensures saves are found regardless of title name or platform differences.
  const std::optional<std::string> listing =
      RunRcloneList({"lsf", cloud_root, "--dirs-only"}, timeout);
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
static bool CloudRemoteIsEmpty(const std::string& remote_dir,
                               std::chrono::milliseconds timeout = RCLONE_TIMEOUT)
{
  const std::optional<std::string> listing = RunRcloneList({"lsf", remote_dir}, timeout);
  if (!listing)
    return false;
  return listing->find_first_not_of(" \t\r\n") == std::string::npos;
}

void PullSaveFromCloud(u32 game_id, const std::string& local_dir)
{
  if (!Config::Get(Config::MAIN_CLOUDSYNC_ENABLED))
    return;

  const std::string remote_name = Config::Get(Config::MAIN_CLOUDSYNC_REMOTE);
  // This function runs synchronously on the boot path (see GCMemcardDirectory's constructor), so
  // every rclone call made directly here uses the shorter RCLONE_BOOT_TIMEOUT rather than
  // RCLONE_TIMEOUT, to cap how long a slow or unreachable remote can delay starting a game.
  const std::string remote_dir = BuildRcloneRemoteDir(game_id, RCLONE_BOOT_TIMEOUT);

  // If the remote is empty/new, push this game's local saves first so nothing is lost.
  // local_dir contains all games' .gci files, so filter by the 4-char game code prefix.
  if (CloudRemoteIsEmpty(remote_dir, RCLONE_BOOT_TIMEOUT))
  {
    const u32 swapped = Common::swap32(game_id);
    const std::string game_code(reinterpret_cast<const char*>(&swapped), 4);
    const std::string include_pattern = game_code + "*.gci";
    // Backgrounded rather than awaited, so it isn't bound by RCLONE_BOOT_TIMEOUT: an initial
    // upload can involve far more data than a routine sync and shouldn't be cut short just
    // because it's slower than the boot-path budget allows.
    std::thread([local_dir, remote_dir, remote_name, include_pattern] {
      if (RunRcloneSync(
              {"copy", local_dir, remote_dir, "--include", include_pattern, "--no-traverse"}))
        Core::DisplayMessage(fmt::format("Initial upload to {}", remote_name), 4000);
      else
        Core::DisplayMessage(fmt::format("Failed to upload saves to {}", remote_name), 8000);
    }).detach();
    return;
  }

  if (RunRcloneSync({"copy", remote_dir, local_dir, "--update", "--no-traverse"},
                    RCLONE_BOOT_TIMEOUT))
    Core::DisplayMessage(fmt::format("Pulled latest save from {}", remote_name), 4000);
  else
    Core::DisplayMessage(fmt::format("Failed to pull latest save from {}", remote_name), 8000);
}

void PushSaveToCloud(u32 game_id, const std::string& gci_path)
{
  if (!Config::Get(Config::MAIN_CLOUDSYNC_ENABLED))
    return;

  const std::string remote_dir = BuildRcloneRemoteDir(game_id);
  const std::string remote_name = Config::Get(Config::MAIN_CLOUDSYNC_REMOTE);
  std::thread([remote_dir, gci_path, remote_name] {
    if (RunRcloneSync({"copy", gci_path, remote_dir, "--no-traverse"}))
      Core::DisplayMessage(fmt::format("Wrote save to {}", remote_name), 4000);
    else
      Core::DisplayMessage(fmt::format("Failed to write save to {}", remote_name), 8000);
  }).detach();
}
}  // namespace Memcard
