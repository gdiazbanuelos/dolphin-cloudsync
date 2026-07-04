// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/CloudSavesPane.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QString>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"

#ifndef _WIN32
#include "Core/HW/GCMemcard/RcloneUtils.h"
#endif

CloudSavesPane::CloudSavesPane()
{
  auto* const main_layout = new QVBoxLayout{this};

  // Enable/disable toggle
  m_enable_checkbox = new QCheckBox{tr("Enable cloud save sync")};
  m_enable_checkbox->setChecked(Config::Get(Config::MAIN_CLOUDSYNC_ENABLED));
  main_layout->addWidget(m_enable_checkbox);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(m_enable_checkbox, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state) {
    Config::SetBaseOrCurrent(Config::MAIN_CLOUDSYNC_ENABLED, state == Qt::Checked);
  });
#else
  connect(m_enable_checkbox, &QCheckBox::stateChanged, this, [this](int state) {
    Config::SetBaseOrCurrent(Config::MAIN_CLOUDSYNC_ENABLED, state == Qt::Checked);
  });
#endif

  // Remote config group
  auto* const remote_group = new QGroupBox{tr("rclone Remote")};
  main_layout->addWidget(remote_group);

  auto* const remote_layout = new QVBoxLayout{remote_group};

  auto* const remote_label = new QLabel{
      tr("rclone remote name — must match the name you gave when running <b>rclone config</b>. "
         "Run <b>rclone listremotes</b> in a terminal to see all configured remotes "
         "(enter the name without the colon). "
         "Restart Dolphin after changing this for it to take effect.")};
  remote_label->setWordWrap(true);
  remote_layout->addWidget(remote_label);

  m_remote_edit = new QLineEdit{QString::fromStdString(Config::Get(Config::MAIN_CLOUDSYNC_REMOTE))};
  remote_layout->addWidget(m_remote_edit);

  connect(m_remote_edit, &QLineEdit::editingFinished, this, [this] {
    Config::SetBaseOrCurrent(Config::MAIN_CLOUDSYNC_REMOTE,
                             m_remote_edit->text().toStdString());
  });

  // Status group
  auto* const status_group = new QGroupBox{tr("Status")};
  main_layout->addWidget(status_group);

  auto* const status_layout = new QVBoxLayout{status_group};

  m_status_label = new QLabel{tr("Checking rclone setup...")};
  m_status_label->setWordWrap(true);
  m_status_label->setTextFormat(Qt::RichText);
  m_status_label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  status_layout->addWidget(m_status_label);

  main_layout->addStretch(1);

  RunCheck();
}

void CloudSavesPane::RunCheck()
{
  const QString remote = m_remote_edit->text().isEmpty() ?
                             QStringLiteral("Dropbox") :
                             m_remote_edit->text();
  const QString remote_target = remote + QStringLiteral(":");

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
           "&nbsp;&nbsp;&bull; Name it to match the remote name above<br>"
           "&nbsp;&nbsp;&bull; Select your storage type and authorize<br><br>"
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
          [this, version_process, show_not_found, rclone_exe,
           remote_target](int exit_code, QProcess::ExitStatus) {
            version_process->deleteLater();

            if (exit_code != 0)
            {
              show_not_found();
              return;
            }

#ifdef _WIN32
            auto* const where_process = new QProcess(this);
            connect(where_process, &QProcess::finished, this,
                    [this, where_process, rclone_exe, remote_target](int, QProcess::ExitStatus) {
                      const QString resolved_path =
                          QString::fromUtf8(where_process->readAllStandardOutput()).trimmed();
                      where_process->deleteLater();
                      const QString display_path =
                          resolved_path.isEmpty() ? rclone_exe : resolved_path;

                      auto* const lsd_process = new QProcess(this);
                      connect(lsd_process, &QProcess::finished, this,
                              [this, lsd_process, display_path, remote_target](
                                  int lsd_exit_code, QProcess::ExitStatus) {
                                if (lsd_exit_code == 0)
                                {
                                  m_status_label->setText(
                                      tr("rclone setup successfully.<br><br>Path: %1")
                                          .arg(display_path));
                                }
                                else
                                {
                                  m_status_label->setText(
                                      tr("rclone is installed (%1), but the remote <b>%2</b> is not configured.<br><br>"
                                         "Run <b>rclone config</b> in a terminal:<br>"
                                         "&nbsp;&nbsp;&bull; Choose <b>n</b> for new remote<br>"
                                         "&nbsp;&nbsp;&bull; Name it <b>%3</b><br>"
                                         "&nbsp;&nbsp;&bull; Select your storage type and authorize")
                                          .arg(display_path)
                                          .arg(remote_target)
                                          .arg(m_remote_edit->text()));
                                }
                                lsd_process->deleteLater();
                              });
                      lsd_process->start(rclone_exe, {QStringLiteral("lsd"), remote_target});
                    });
            where_process->start(QStringLiteral("where"), {QStringLiteral("rclone")});
#else
            auto* const lsd_process = new QProcess(this);
            connect(lsd_process, &QProcess::finished, this,
                    [this, lsd_process, rclone_exe, remote_target](int lsd_exit_code,
                                                                   QProcess::ExitStatus) {
                      if (lsd_exit_code == 0)
                      {
                        m_status_label->setText(
                            tr("rclone setup successfully.<br><br>Path: %1").arg(rclone_exe));
                      }
                      else
                      {
                        m_status_label->setText(
                            tr("rclone is installed (%1), but the remote <b>%2</b> is not configured.<br><br>"
                               "Run <b>rclone config</b> in a terminal:<br>"
                               "&nbsp;&nbsp;&bull; Choose <b>n</b> for new remote<br>"
                               "&nbsp;&nbsp;&bull; Name it <b>%3</b><br>"
                               "&nbsp;&nbsp;&bull; Select your storage type and authorize")
                                .arg(rclone_exe)
                                .arg(remote_target)
                                .arg(m_remote_edit->text()));
                      }
                      lsd_process->deleteLater();
                    });
            lsd_process->start(rclone_exe, {QStringLiteral("lsd"), remote_target});
#endif
          });

  version_process->start(rclone_exe, {QStringLiteral("version")});
}
