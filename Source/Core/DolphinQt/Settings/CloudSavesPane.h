// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

class QLabel;

class CloudSavesPane final : public QWidget
{
  Q_OBJECT

public:
  CloudSavesPane();

private:
  void RunCheck();

  QLabel* m_status_label;
};
