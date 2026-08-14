// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Core/HW/GCMemcard/CloudSync.h"

TEST(CloudSync, BuildWindowsCommandLinePlainArg)
{
  EXPECT_EQ(Memcard::BuildWindowsCommandLine({"copy"}), "copy");
}

TEST(CloudSync, BuildWindowsCommandLineArgWithSpace)
{
  EXPECT_EQ(Memcard::BuildWindowsCommandLine({"foo bar"}), "\"foo bar\"");
}

TEST(CloudSync, BuildWindowsCommandLineArgWithEmbeddedQuote)
{
  EXPECT_EQ(Memcard::BuildWindowsCommandLine({"foo\"bar"}), "\"foo\\\"bar\"");
}

TEST(CloudSync, BuildWindowsCommandLineArgWithTrailingBackslash)
{
  // A trailing backslash immediately before the closing quote must be doubled, or it would
  // escape the quote instead of terminating the argument. A backslash with no space/quote
  // elsewhere in the argument doesn't need any escaping at all, so this only bites once the
  // argument already needs quoting for some other reason (the space here).
  EXPECT_EQ(Memcard::BuildWindowsCommandLine({"foo bar\\"}), "\"foo bar\\\\\"");
}

TEST(CloudSync, BuildWindowsCommandLineEmptyArg)
{
  EXPECT_EQ(Memcard::BuildWindowsCommandLine({""}), "\"\"");
}

TEST(CloudSync, BuildWindowsCommandLineMultipleArgs)
{
  EXPECT_EQ(Memcard::BuildWindowsCommandLine({"copy", "foo bar", "--flag"}),
            "copy \"foo bar\" --flag");
}
