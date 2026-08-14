// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#ifndef _WIN32
namespace Memcard
{
// Searches a list of common install locations for the rclone binary, since it may not be on
// PATH depending on how Dolphin was launched. Returns an empty string if rclone could not be
// found.
std::string FindRclonePath();
}  // namespace Memcard
#endif
