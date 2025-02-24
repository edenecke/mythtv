#include "mythconfig.h"
#if CONFIG_SYSTEMD_NOTIFY
    #include <systemd/sd-daemon.h>
#endif

#include <csignal> // for signal
#include <cstdlib>

#include <QtGlobal>
#ifndef _WIN32
#include <QCoreApplication>
#else
#include <QApplication>
#endif

#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QMap>
#ifdef Q_OS_DARWIN
#include <QProcessEnvironment>
#endif

#include "signalhandling.h"
#include "commandlineparser.h"
#include "scheduledrecording.h"
#include "previewgenerator.h"
#include "mythcorecontext.h"
#include "mythsystemevent.h"
#include "mythtranslation.h"
#include "backendcontext.h"
#include "main_helpers.h"
#include "mythmiscutil.h"
#include "storagegroup.h"
#include "mediaserver.h"
#include "loggingserver.h"
#include "mythlogging.h"
#include "mythversion.h"
#include "programinfo.h"
#include "autoexpire.h"
#include "mainserver.h"
#include "remoteutil.h"
#include "exitcodes.h"
#include "scheduler.h"
#include "jobqueue.h"
#include "dbcheck.h"
#include "compat.h"
#include "mythdb.h"
#include "tv_rec.h"
#include "cleanupguard.h"


#define LOC      QString("MythBackend: ")
#define LOC_WARN QString("MythBackend, Warning: ")
#define LOC_ERR  QString("MythBackend, Error: ")

#ifdef Q_OS_MACOS
    // 10.6 uses some file handles for its new Grand Central Dispatch thingy
    #define UNUSED_FILENO 6
#else
    #define UNUSED_FILENO 3
#endif

int main(int argc, char **argv)
{
    MythBackendCommandLineParser cmdline;
    if (!cmdline.Parse(argc, argv))
    {
        cmdline.PrintHelp();
        return GENERIC_EXIT_INVALID_CMDLINE;
    }

    if (cmdline.toBool("showhelp"))
    {
        cmdline.PrintHelp();
        return GENERIC_EXIT_OK;
    }

    if (cmdline.toBool("showversion"))
    {
        MythBackendCommandLineParser::PrintVersion();
        return GENERIC_EXIT_OK;
    }

#ifndef _WIN32
    for (long i = UNUSED_FILENO; i < sysconf(_SC_OPEN_MAX) - 1; ++i)
        close(i);
    QCoreApplication a(argc, argv);
#else
    // MINGW application needs a window to receive messages
    // such as socket notifications :[
    QApplication a(argc, argv);
#endif
    QCoreApplication::setApplicationName(MYTH_APPNAME_MYTHBACKEND);

#ifdef Q_OS_DARWIN
    QString path = QCoreApplication::applicationDirPath();
    setenv("PYTHONPATH",
           QString("%1/../Resources/lib/%2/site-packages:%3")
           .arg(path)
           .arg(QFileInfo(PYTHON_EXE).fileName())
           .arg(QProcessEnvironment::systemEnvironment().value("PYTHONPATH"))
           .toUtf8().constData(), 1);
#endif

    gPidFile = cmdline.toString("pidfile");
    int retval = cmdline.Daemonize();
    if (retval != GENERIC_EXIT_OK)
        return retval;

    bool daemonize = cmdline.toBool("daemon");
    QString mask("general");
    if ((retval = cmdline.ConfigureLogging(mask, daemonize)) != GENERIC_EXIT_OK)
        return retval;

    if (daemonize)
        // Don't listen to console input if daemonized
        close(0);

    CleanupGuard callCleanup(cleanup);

#ifndef _WIN32
    QList<int> signallist;
    signallist << SIGINT << SIGTERM << SIGSEGV << SIGABRT << SIGBUS << SIGFPE
               << SIGILL;
#ifndef Q_OS_DARWIN
    signallist << SIGRTMIN;
#endif
    SignalHandler::Init(signallist);
    SignalHandler::SetHandler(SIGHUP, logSigHup);
#endif

#if CONFIG_SYSTEMD_NOTIFY
    (void)sd_notify(0, "STATUS=Connecting to database.");
#endif
    gContext = new MythContext(MYTH_BINARY_VERSION);
    if (!gContext->Init(false))
    {
        LOG(VB_GENERAL, LOG_CRIT, "Failed to init MythContext.");
        return GENERIC_EXIT_NO_MYTHCONTEXT;
    }

    MythTranslation::load("mythfrontend");

    setHttpProxy();

    cmdline.ApplySettingsOverride();

    if (cmdline.toBool("event")         || cmdline.toBool("systemevent") ||
        cmdline.toBool("setverbose")    || cmdline.toBool("printsched") ||
        cmdline.toBool("testsched")     || cmdline.toBool("resched") ||
        cmdline.toBool("scanvideos")    || cmdline.toBool("clearcache") ||
        cmdline.toBool("printexpire")   || cmdline.toBool("setloglevel"))
    {
        gCoreContext->SetAsBackend(false);
        return handle_command(cmdline);
    }

    gCoreContext->SetAsBackend(true);
    retval = run_backend(cmdline);
    return retval;
}

#ifdef _WIN32 // TODO Needs fixing for Windows
    QString GetPlaybackURL(ProgramInfo *pginfo, bool storePath)
    {
        return "";
    }
#endif

/* vim: set expandtab tabstop=4 shiftwidth=4: */
