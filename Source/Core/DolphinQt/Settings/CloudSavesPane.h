// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;

class CloudSavesPane final : public QWidget
{
  Q_OBJECT

public:
  CloudSavesPane();

private:
  void RunCheck();

  QCheckBox* m_enable_checkbox;
  QLabel* m_status_label;
  QLineEdit* m_remote_edit;
};
