// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/CloudSavesPane.h"

#include <QGroupBox>
#include <QLabel>
#include <QProcess>
#include <QString>
#include <QVBoxLayout>

#ifndef _WIN32
#include "Core/HW/GCMemcard/RcloneUtils.h"
#endif

CloudSavesPane::CloudSavesPane()
{
  auto* const main_layout = new QVBoxLayout{this};

  auto* const group = new QGroupBox{tr("Cloud Saves (rclone)")};
  main_layout->addWidget(group);

  auto* const group_layout = new QVBoxLayout{group};

  m_status_label = new QLabel{tr("Checking rclone setup...")};
  m_status_label->setWordWrap(true);
  m_status_label->setTextFormat(Qt::RichText);
  m_status_label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  group_layout->addWidget(m_status_label);

  main_layout->addStretch(1);

  RunCheck();
}

void CloudSavesPane::RunCheck()
{
#ifdef _WIN32
  const QString rclone_exe = QStringLiteral("rclone");
#else
  const std::string rclone_path = FindRclonePath();
  const QString rclone_exe = rclone_path.empty() ? QStringLiteral("rclone") :
                                                    QString::fromStdString(rclone_path);
#endif

  const auto show_not_found = [this]() {
    m_status_label->setOpenExternalLinks(true);
    m_status_label->setText(
        tr("rclone was not found on this machine.<br><br>"
           "<b>Setup Instructions:</b><br><br>"
           "1. <a href=\"https://rclone.org/downloads/\">Download rclone</a> and add it to your PATH.<br><br>"
           "2. Run <b>rclone config</b> in a terminal:<br>"
           "&nbsp;&nbsp;&bull; Choose <b>n</b> for new remote<br>"
           "&nbsp;&nbsp;&bull; Name it <b>Dropbox</b><br>"
           "&nbsp;&nbsp;&bull; Select <b>dropbox</b> as the type<br>"
           "&nbsp;&nbsp;&bull; Authorize via browser when prompted<br><br>"
           "Restart Dolphin after setup is complete."));
  };

#ifndef _WIN32
  if (rclone_path.empty())
  {
    show_not_found();
    return;
  }
#endif

  auto* const version_process = new QProcess(this);

  connect(version_process, &QProcess::errorOccurred, this,
          [this, version_process, show_not_found](QProcess::ProcessError) {
            version_process->deleteLater();
            show_not_found();
          });

  connect(version_process, &QProcess::finished, this,
          [this, version_process, show_not_found, rclone_exe](int exit_code, QProcess::ExitStatus) {
            version_process->deleteLater();

            if (exit_code != 0)
            {
              show_not_found();
              return;
            }

            auto* const lsd_process = new QProcess(this);
            connect(lsd_process, &QProcess::finished, this,
                    [this, lsd_process, rclone_exe](int lsd_exit_code, QProcess::ExitStatus) {
                      if (lsd_exit_code == 0)
                      {
                        m_status_label->setText(
                            tr("rclone setup successfully.<br><br>Path: %1").arg(rclone_exe));
                      }
                      else
                      {
                        m_status_label->setText(
                            tr("rclone is installed (%1), but the Dropbox remote is not configured.<br><br>"
                               "Run <b>rclone config</b> in a terminal:<br>"
                               "&nbsp;&nbsp;&bull; Choose <b>n</b> for new remote<br>"
                               "&nbsp;&nbsp;&bull; Name it <b>Dropbox</b><br>"
                               "&nbsp;&nbsp;&bull; Select <b>dropbox</b> as the type<br>"
                               "&nbsp;&nbsp;&bull; Authorize via browser when prompted")
                                .arg(rclone_exe));
                      }
                      lsd_process->deleteLater();
                    });
            lsd_process->start(rclone_exe, {QStringLiteral("lsd"), QStringLiteral("Dropbox:")});
          });

  version_process->start(rclone_exe, {QStringLiteral("version")});
}
