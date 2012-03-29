// Konstruktor - An interactive LDraw modeler for KDE
// Copyright (c)2006-2011 Park "segfault" J. K. <mastermind@planetmono.org>

#include <cstdlib>

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QPixmapCache>
#include <QProcess>
#include <QProgressDialog>

#include <libldr/color.h>
#include <libldr/model.h>
#include <libldr/part_library.h>

#include <renderer/parameters.h>

#include "colormanager.h"
#include "dbmanager.h"
#include "mainwindow.h"
#include "pixmaprenderer.h"

#include "application.h"

namespace Konstruktor
{

Application* Application::instance_ = 0L;

Application::Application(QObject *parent)
    : QObject(parent)
{
  renderer_ = 0L;
  instance_ = this;
  
  ldraw::color::init();
  
  config_ = new Config;
  db_ = new DBManager(this);
  colorManager_ = new ColorManager;
  
  dbDialog_ = 0L;
  
  if (!initialize())
    qApp->exit();
}

Application::~Application()
{
  instance_ = 0L;
  
  delete colorManager_;
  delete library_;
  
  config_->writeConfig();
  delete config_;
}

bool Application::initialize()
{
  int attempt = 0;
  bool retry;
  std::string path = config_->path().toLocal8Bit().data();
  
  do {
    retry = false;
    
    // try to load LDraw part library
    try {
      if (path.empty())
        library_ = new ldraw::part_library;
      else
        library_ = new ldraw::part_library(path);
    } catch (const ldraw::exception &) {
      QMessageBox::critical(0L, tr("Error"), tr("<qt>Unable to find LDraw part library. If you have installed LDraw, please specify your installation path.  If you have not installed it, it can be obtained from <a href=\"http://www.ldraw.org\">http://www.ldraw.org</a>.</qt>"));
      
      QString newpath = QFileDialog::getExistingDirectory(0L, tr("Choose LDraw installation directory"));
      if (newpath.isEmpty()) {
        // Last attempt
        if (!config_->path().isEmpty()) {
          try {
            library_ = new ldraw::part_library;
          } catch (...) {
            return false;
          }
          
          config_->setPath("");
          config_->writeConfig();
        } else {
          return false;
        }
      } else {
        retry = true;
        path = newpath.toLocal8Bit().data();
        ++attempt;
      }
    }
  } while (retry);
  
  if (attempt) {
    config_->setPath(path.data());
    config_->writeConfig();
  }
  
  db_->initialize(saveLocation("")+"parts.db");
  
  params_ = new ldraw_renderer::parameters();
  params_->set_shading(true);
  params_->set_shader(false);
  params_->set_vbuffer_criteria(ldraw_renderer::parameters::vbuffer_parts);
  
  configUpdated();
  
  testPovRay(true);
  
  startDBUpdater();
  
  return true;
}

void Application::startDBUpdater()
{
  QStringList args;
  args << library_->ldrawpath().c_str();

  dbUpdater_ = new QProcess(this);
  
  connect(dbUpdater_, SIGNAL(readyReadStandardOutput()), this, SLOT(dbUpdateStatus()));
  connect(dbUpdater_, SIGNAL(error(QProcess::ProcessError)), this, SLOT(dbUpdateError(QProcess::ProcessError)));
  connect(dbUpdater_, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(dbUpdateFinished(int, QProcess::ExitStatus)));
  
  dbUpdater_->start("konstruktor_db_updater", args);
}

void Application::startup()
{
  window_ = new MainWindow();
  
  window_->show();
}

QString Application::saveLocation(const QString &directory)
{
  globalDirsMutex_.lock();
  QString result;

#if defined(Q_WS_MAC)
  result = QDir::homePath() +
      "/Library/Application Support/Konstruktor/" + directory;
#elif defined(Q_OS_UNIX)
  result = qgetenv("XDG_DATA_HOME");
  if (result.isEmpty())
    result = QDir::homePath() + "/.local/share/konstruktor/" + directory;
#elif defined(Q_OS_WIN32)
  result = QDir::homePath() +
      "/Application Data/Konstruktor";
#endif

  QDir().mkpath(result);
  globalDirsMutex_.unlock();
  
  return result;
}

QWidget* Application::rootWindow()
{
  return static_cast<QWidget *>(window_);
}

void Application::testPovRay(bool overrideconfig)
{
  if (!config_->povRayExecutablePath().isEmpty()) {
    QStringList args;
    
    args << "--version";
    if (QProcess::execute(config_->povRayExecutablePath(), args) != 0) {
      QMessageBox::critical(0L, tr("Error"), tr("Could not execute POV-Ray. Raytracing feature is temporarily disabled. Please make sure that POV-Ray is properly installed."));
      
      if (overrideconfig) {
        config_->setPovRayExecutablePath("");
        config_->writeConfig();
      }
      
      hasPovRay_ = false;
    } else {
      hasPovRay_ = true;
    }
  } else {
    if (QProcess::execute("povray") >= 0) {
      config_->setPovRayExecutablePath("povray");
      config_->writeConfig();
      hasPovRay_ = true;
    } else {
      hasPovRay_ = false;
    }
  }			
}

void Application::initializeRenderer(QGLWidget *glBase)
{
  if (!renderer_)
    renderer_ = new PixmapRenderer(256, 256, glBase);
}

void Application::configUpdated()
{
  switch (config_->studMode()) {
    case Config::Normal:
      params_->set_stud_rendering_mode(ldraw_renderer::parameters::stud_regular);
      break;
    case Config::Line:
      params_->set_stud_rendering_mode(ldraw_renderer::parameters::stud_line);
      break;
    case Config::Square:
      params_->set_stud_rendering_mode(ldraw_renderer::parameters::stud_square);
  }
}

void Application::dbUpdateStatus()
{
  if (!dbDialog_) {
    dbDialog_ = new QProgressDialog();

    dbDialog_->setWindowTitle(tr("Scanning"));
    dbDialog_->setAutoClose(true);
    dbDialog_->show();
  }
  
  dbUpdater_->setReadChannel(QProcess::StandardOutput);
  
  QStringList message = QString(dbUpdater_->readAll()).trimmed().split('\n');
  QString lastLine = message[message.size() - 1].trimmed();
  
  int cur = lastLine.section(' ', 0, 0).toInt();
  int max = lastLine.section(' ', 1, 1).toInt();
  
  dbDialog_->setMaximum(max);
  dbDialog_->setValue(cur);
  dbDialog_->setLabelText(tr("<qt><p align=center>Building indexes from the LDraw part library. Please wait...<br/>%1</p></qt>").arg(lastLine.section(' ', 2)));
}

void Application::dbUpdateFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
  if (dbDialog_) {
    delete dbDialog_;
    dbDialog_ = 0L;
  }
  
  delete dbUpdater_;
  dbUpdater_ = 0L;

  config_->reloadConfig();
  
  if (exitCode || exitStatus == QProcess::CrashExit) {
    QMessageBox::critical(0L, tr("Error"), tr("Could not scan LDraw part library."));
    startup();
    
    qApp->exit();
  } else {
    startup();
  }
}

void Application::dbUpdateError(QProcess::ProcessError error)
{
  QString errorMsg;
  
  switch (error) {
    case QProcess::FailedToStart:
      errorMsg = tr("Failed to start part database updater. Your installation might be broken.");
      break;
    case QProcess::Crashed:
      errorMsg = tr("Part database updater is stopped unexpectedly.");
      break;
    default:
      errorMsg = tr("Unknown error occurred while scanning parts.");
  }
  
  QMessageBox::critical(0L, tr("Error in Database Updater"), errorMsg);
  qApp->exit();
}

}
