// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Memcard
{
// Escapes a single argument for inclusion in a Windows command line, following the quoting rules
// used by CommandLineToArgvW so that arguments containing spaces, quotes, or backslashes survive
// intact.
std::string EscapeWindowsArg(const std::string& arg);

// Joins args into a single Windows command line, individually escaping each argument.
std::string BuildWindowsCommandLine(const std::vector<std::string>& args);

// Pulls the newest save for game_id from the configured cloud remote into local_dir, uploading
// local_dir's existing saves first if the remote is empty. Does nothing if cloud sync is
// disabled.
void PullSaveFromCloud(u32 game_id, const std::string& local_dir);

// Pushes gci_path, a save just written for game_id, to the configured cloud remote in the
// background. Does nothing if cloud sync is disabled.
void PushSaveToCloud(u32 game_id, const std::string& gci_path);
}  // namespace Memcard
