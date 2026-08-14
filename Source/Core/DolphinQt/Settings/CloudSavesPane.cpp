// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/CloudSavesPane.h"

#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigText.h"

#ifndef _WIN32
#include "Core/HW/GCMemcard/RcloneUtils.h"
#endif

// How long to wait for a network-touching rclone command (checking or moving a remote folder)
// before giving up, so an offline or unreachable remote doesn't leave the UI stuck with no
// feedback.
constexpr int RCLONE_NETWORK_TIMEOUT_MS = 15000;

CloudSavesPane::CloudSavesPane()
{
  auto* const main_layout = new QVBoxLayout{this};

  // Enable/disable toggle
  m_enable_checkbox = new ConfigBool(tr("Enable cloud save sync"), Config::MAIN_CLOUDSYNC_ENABLED);
  main_layout->addWidget(m_enable_checkbox);

  // Remote config group
  auto* const remote_group = new QGroupBox{tr("rclone Remote")};
  main_layout->addWidget(remote_group);

  auto* const remote_layout = new QVBoxLayout{remote_group};

  auto* const folder_label = new QLabel{tr("<b>Remote Folder Name</b>")};
  remote_layout->addWidget(folder_label);

  auto* const folder_description =
      new QLabel{tr("Name of the folder created in your cloud storage where saves are kept.")};
  folder_description->setWordWrap(true);
  remote_layout->addWidget(folder_description);

  m_remote_folder_edit = new ConfigText(Config::MAIN_CLOUDSYNC_REMOTE_FOLDER);
  remote_layout->addWidget(m_remote_folder_edit);

  // ConfigText already saves the new value to Config on editingFinished (connected in its own
  // constructor, before this connection), so the old value has to be tracked separately here
  // rather than read back from Config.
  m_previous_remote_folder = m_remote_folder_edit->text();
  connect(m_remote_folder_edit, &QLineEdit::editingFinished, this, [this] {
    const QString new_folder = m_remote_folder_edit->text();
    if (new_folder != m_previous_remote_folder && !new_folder.isEmpty())
    {
      const auto choice = QMessageBox::question(
          this, tr("Move Cloud Saves?"),
          tr("Move existing saves from the cloud folder \"%1\" to \"%2\"?<br><br>"
             "If you skip this, saves already synced under the old folder name won't be found "
             "until you rename it back.")
              .arg(m_previous_remote_folder, new_folder),
          QMessageBox::Yes | QMessageBox::No);
      if (choice == QMessageBox::Yes)
        MigrateRemoteFolder(m_previous_remote_folder, new_folder);
    }
    m_previous_remote_folder = new_folder;
  });

  auto* const provider_label = new QLabel{tr("<b>Cloud Provider</b>")};
  remote_layout->addWidget(provider_label);

  auto* const remote_label = new QLabel{
      tr("rclone remote name — must match the name you gave when running <b>rclone config</b>. "
         "Run <b>rclone listremotes</b> in a terminal to see all configured remotes "
         "(enter the name without the colon). "
         "Restart Dolphin after changing this for it to take effect.")};
  remote_label->setWordWrap(true);
  remote_layout->addWidget(remote_label);

  m_remote_edit = new ConfigText(Config::MAIN_CLOUDSYNC_REMOTE);
  remote_layout->addWidget(m_remote_edit);

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
  const QString remote =
      m_remote_edit->text().isEmpty() ? QStringLiteral("Dropbox") : m_remote_edit->text();
  const QString remote_target = remote + QStringLiteral(":");

#ifdef _WIN32
  const QString rclone_exe = QStringLiteral("rclone");
#else
  const std::string rclone_path = Memcard::FindRclonePath();
  const QString rclone_exe =
      rclone_path.empty() ? QStringLiteral("rclone") : QString::fromStdString(rclone_path);
#endif

  const auto show_not_found = [this]() {
    m_status_label->setOpenExternalLinks(true);
    m_status_label->setText(tr("rclone was not found on this machine.<br><br>"
                               "<b>Setup Instructions:</b><br><br>"
                               "1. <a href=\"https://rclone.org/downloads/\">Download rclone</a> "
                               "and add it to your PATH.<br><br>"
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
          [this, version_process, show_not_found, rclone_exe, remote_target](int exit_code,
                                                                             QProcess::ExitStatus) {
            version_process->deleteLater();

            if (exit_code != 0)
            {
              show_not_found();
              return;
            }

#ifdef _WIN32
            auto* const where_process = new QProcess(this);
            connect(
                where_process, &QProcess::finished, this,
                [this, where_process, rclone_exe, remote_target](int, QProcess::ExitStatus) {
                  const QString resolved_path =
                      QString::fromUtf8(where_process->readAllStandardOutput()).trimmed();
                  where_process->deleteLater();
                  const QString display_path = resolved_path.isEmpty() ? rclone_exe : resolved_path;

                  auto* const lsd_process = new QProcess(this);
                  auto* const lsd_timeout = new QTimer(this);
                  lsd_timeout->setSingleShot(true);
                  connect(lsd_timeout, &QTimer::timeout, this, [lsd_process] {
                    if (lsd_process->state() != QProcess::NotRunning)
                      lsd_process->kill();
                  });
                  connect(
                      lsd_process, &QProcess::finished, this,
                      [this, lsd_process, lsd_timeout, display_path,
                       remote_target](int lsd_exit_code, QProcess::ExitStatus lsd_exit_status) {
                        lsd_timeout->stop();
                        lsd_timeout->deleteLater();
                        if (lsd_exit_status == QProcess::CrashExit)
                        {
                          m_status_label->setText(
                              tr("rclone is installed (%1), but timed out trying to reach the "
                                 "remote <b>%2</b>. Check that it's online and reachable.")
                                  .arg(display_path)
                                  .arg(remote_target));
                        }
                        else if (lsd_exit_code == 0)
                        {
                          m_status_label->setText(
                              tr("rclone setup successfully.<br><br>Path: %1").arg(display_path));
                        }
                        else
                        {
                          m_status_label->setText(
                              tr("rclone is installed (%1), but the remote <b>%2</b> is not "
                                 "configured.<br><br>"
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
                  lsd_timeout->start(RCLONE_NETWORK_TIMEOUT_MS);
                  lsd_process->start(rclone_exe, {QStringLiteral("lsd"), remote_target});
                });
            where_process->start(QStringLiteral("where"), {QStringLiteral("rclone")});
#else
            auto* const lsd_process = new QProcess(this);
            auto* const lsd_timeout = new QTimer(this);
            lsd_timeout->setSingleShot(true);
            connect(lsd_timeout, &QTimer::timeout, this, [lsd_process] {
              if (lsd_process->state() != QProcess::NotRunning)
                lsd_process->kill();
            });
            connect(lsd_process, &QProcess::finished, this,
                    [this, lsd_process, lsd_timeout, rclone_exe,
                     remote_target](int lsd_exit_code, QProcess::ExitStatus lsd_exit_status) {
                      lsd_timeout->stop();
                      lsd_timeout->deleteLater();
                      if (lsd_exit_status == QProcess::CrashExit)
                      {
                        m_status_label->setText(
                            tr("rclone is installed (%1), but timed out trying to reach the "
                               "remote <b>%2</b>. Check that it's online and reachable.")
                                .arg(rclone_exe)
                                .arg(remote_target));
                      }
                      else if (lsd_exit_code == 0)
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
            lsd_timeout->start(RCLONE_NETWORK_TIMEOUT_MS);
            lsd_process->start(rclone_exe, {QStringLiteral("lsd"), remote_target});
#endif
          });

  version_process->start(rclone_exe, {QStringLiteral("version")});
}

void CloudSavesPane::MigrateRemoteFolder(const QString& old_folder, const QString& new_folder)
{
  const QString remote =
      m_remote_edit->text().isEmpty() ? QStringLiteral("Dropbox") : m_remote_edit->text();

#ifdef _WIN32
  const QString rclone_exe = QStringLiteral("rclone");
#else
  const std::string rclone_path = Memcard::FindRclonePath();
  if (rclone_path.empty())
    return;
  const QString rclone_exe = QString::fromStdString(rclone_path);
#endif

  m_status_label->setText(tr("Moving saves from \"%1\" to \"%2\"...").arg(old_folder, new_folder));

  auto* const move_process = new QProcess(this);
  auto* const move_timeout = new QTimer(this);
  move_timeout->setSingleShot(true);
  connect(move_timeout, &QTimer::timeout, this, [move_process] {
    if (move_process->state() != QProcess::NotRunning)
      move_process->kill();
  });

  connect(move_process, &QProcess::finished, this,
          [this, move_process, move_timeout, old_folder,
           new_folder](int exit_code, QProcess::ExitStatus exit_status) {
            move_timeout->stop();
            move_timeout->deleteLater();

            if (exit_status == QProcess::CrashExit)
            {
              m_status_label->setText(tr("Timed out moving saves from \"%1\" to \"%2\". You may "
                                         "need to move them manually with rclone.")
                                          .arg(old_folder, new_folder));
            }
            else if (exit_code == 0)
            {
              m_status_label->setText(
                  tr("Moved saves from \"%1\" to \"%2\".").arg(old_folder, new_folder));
            }
            else
            {
              m_status_label->setText(
                  tr("Could not move saves from \"%1\" to \"%2\" (the old folder may not have "
                     "existed yet). New saves will be written to \"%2\".")
                      .arg(old_folder, new_folder));
            }
            move_process->deleteLater();
          });

  move_timeout->start(RCLONE_NETWORK_TIMEOUT_MS);
  move_process->start(rclone_exe,
                      {QStringLiteral("moveto"), remote + QStringLiteral(":") + old_folder,
                       remote + QStringLiteral(":") + new_folder});
}
