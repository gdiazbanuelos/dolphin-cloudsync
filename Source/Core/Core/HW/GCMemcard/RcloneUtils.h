// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#ifndef _WIN32
#include <cstdlib>
#include <unistd.h>
#include <vector>

inline std::string FindRclonePath()
{
  std::vector<std::string> candidates = {
      "/usr/bin/rclone",
      "/usr/local/bin/rclone",
      "/home/deck/bin/rclone",
  };
  if (const char* home = getenv("HOME"))
    candidates.push_back(std::string(home) + "/bin/rclone");

  // When running inside an AppImage the filesystem is a mounted squashfs.
  // Prefix paths with /proc/1/root to access the host filesystem instead.
  const std::vector<std::string> base_candidates = candidates;
  for (const std::string& path : base_candidates)
    candidates.push_back("/proc/1/root" + path);

  for (const std::string& path : candidates)
  {
    if (access(path.c_str(), X_OK) == 0)
      return path;
  }
  return {};
}
#endif
