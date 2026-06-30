// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/CloudSavesPane.h"

#include <QGroupBox>
#include <QLabel>
#include <QProcess>
#include <QVBoxLayout>

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
  auto* const version_process = new QProcess(this);

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

  connect(version_process, &QProcess::errorOccurred, this,
          [this, version_process, show_not_found](QProcess::ProcessError) {
            version_process->deleteLater();
            show_not_found();
          });

  connect(version_process, &QProcess::finished, this,
          [this, version_process, show_not_found](int exit_code, QProcess::ExitStatus) {
            version_process->deleteLater();

            if (exit_code != 0)
            {
              show_not_found();
              return;
            }

            auto* const where_process = new QProcess(this);
            connect(where_process, &QProcess::finished, this,
                    [this, where_process](int, QProcess::ExitStatus) {
                      const QString rclone_path =
                          QString::fromUtf8(where_process->readAllStandardOutput()).trimmed();
                      where_process->deleteLater();

                      auto* const lsd_process = new QProcess(this);
                      connect(lsd_process, &QProcess::finished, this,
                              [this, lsd_process, rclone_path](int lsd_exit_code,
                                                               QProcess::ExitStatus) {
                                if (lsd_exit_code == 0)
                                {
                                  m_status_label->setText(
                                      tr("rclone setup successfully.<br><br>Path: %1").arg(rclone_path));
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
                                          .arg(rclone_path));
                                }
                                lsd_process->deleteLater();
                              });
                      lsd_process->start(QStringLiteral("rclone"),
                                         {QStringLiteral("lsd"), QStringLiteral("Dropbox:")});
                    });
#ifdef _WIN32
            where_process->start(QStringLiteral("where"), {QStringLiteral("rclone")});
#else
            where_process->start(QStringLiteral("which"), {QStringLiteral("rclone")});
#endif
          });
  version_process->start(QStringLiteral("rclone"), {QStringLiteral("version")});
}
