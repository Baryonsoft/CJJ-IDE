/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "linuxdevice.h"

#include "genericlinuxdeviceconfigurationwidget.h"
#include "genericlinuxdeviceconfigurationwizard.h"
#include "linuxdeviceprocess.h"
#include "linuxdevicetester.h"
#include "publickeydeploymentdialog.h"
#include "remotelinux_constants.h"
#include "remotelinuxsignaloperation.h"
#include "remotelinuxenvironmentreader.h"

#include <coreplugin/icore.h>
#include <coreplugin/messagemanager.h>

#include <projectexplorer/devicesupport/sshdeviceprocesslist.h>
#include <projectexplorer/runcontrol.h>

#include <ssh/sshremoteprocessrunner.h>

#include <utils/algorithm.h>
#include <utils/environment.h>
#include <utils/hostosinfo.h>
#include <utils/port.h>
#include <utils/qtcassert.h>
#include <utils/stringutils.h>

using namespace ProjectExplorer;
using namespace Utils;

namespace RemoteLinux {

const char Delimiter0[] = "x--";
const char Delimiter1[] = "---";

static QString visualizeNull(QString s)
{
    return s.replace(QLatin1Char('\0'), QLatin1String("<null>"));
}

class LinuxDeviceProcessList : public SshDeviceProcessList
{
public:
    LinuxDeviceProcessList(const IDevice::ConstPtr &device, QObject *parent)
            : SshDeviceProcessList(device, parent)
    {
    }

private:
    QString listProcessesCommandLine() const override
    {
        return QString::fromLatin1(
            "for dir in `ls -d /proc/[0123456789]*`; do "
                "test -d $dir || continue;" // Decrease the likelihood of a race condition.
                "echo $dir;"
                "cat $dir/cmdline;echo;" // cmdline does not end in newline
                "cat $dir/stat;"
                "readlink $dir/exe;"
                "printf '%1''%2';"
            "done").arg(QLatin1String(Delimiter0)).arg(QLatin1String(Delimiter1));
    }

    QList<DeviceProcessItem> buildProcessList(const QString &listProcessesReply) const override
    {
        QList<DeviceProcessItem> processes;
        const QStringList lines = listProcessesReply.split(QString::fromLatin1(Delimiter0)
                + QString::fromLatin1(Delimiter1), Qt::SkipEmptyParts);
        foreach (const QString &line, lines) {
            const QStringList elements = line.split(QLatin1Char('\n'));
            if (elements.count() < 4) {
                qDebug("%s: Expected four list elements, got %d. Line was '%s'.", Q_FUNC_INFO,
                       int(elements.count()), qPrintable(visualizeNull(line)));
                continue;
            }
            bool ok;
            const int pid = elements.first().mid(6).toInt(&ok);
            if (!ok) {
                qDebug("%s: Expected number in %s. Line was '%s'.", Q_FUNC_INFO,
                       qPrintable(elements.first()), qPrintable(visualizeNull(line)));
                continue;
            }
            QString command = elements.at(1);
            command.replace(QLatin1Char('\0'), QLatin1Char(' '));
            if (command.isEmpty()) {
                const QString &statString = elements.at(2);
                const int openParenPos = statString.indexOf(QLatin1Char('('));
                const int closedParenPos = statString.indexOf(QLatin1Char(')'), openParenPos);
                if (openParenPos == -1 || closedParenPos == -1)
                    continue;
                command = QLatin1Char('[')
                        + statString.mid(openParenPos + 1, closedParenPos - openParenPos - 1)
                        + QLatin1Char(']');
            }

            DeviceProcessItem process;
            process.pid = pid;
            process.cmdLine = command;
            process.exe = elements.at(3);
            processes.append(process);
        }

        Utils::sort(processes);
        return processes;
    }
};

class LinuxPortsGatheringMethod : public PortsGatheringMethod
{
    Runnable runnable(QAbstractSocket::NetworkLayerProtocol protocol) const override
    {
        // We might encounter the situation that protocol is given IPv6
        // but the consumer of the free port information decides to open
        // an IPv4(only) port. As a result the next IPv6 scan will
        // report the port again as open (in IPv6 namespace), while the
        // same port in IPv4 namespace might still be blocked, and
        // re-use of this port fails.
        // GDBserver behaves exactly like this.

        Q_UNUSED(protocol)

        // /proc/net/tcp* covers /proc/net/tcp and /proc/net/tcp6
        Runnable runnable;
        runnable.command.setExecutable("sed");
        runnable.command.setArguments("-e 's/.*: [[:xdigit:]]*:\\([[:xdigit:]]\\{4\\}\\).*/\\1/g' /proc/net/tcp*");
        return runnable;
    }

    QList<Utils::Port> usedPorts(const QByteArray &output) const override
    {
        QList<Utils::Port> ports;
        QList<QByteArray> portStrings = output.split('\n');
        foreach (const QByteArray &portString, portStrings) {
            if (portString.size() != 4)
                continue;
            bool ok;
            const Utils::Port port(portString.toInt(&ok, 16));
            if (ok) {
                if (!ports.contains(port))
                    ports << port;
            } else {
                qWarning("%s: Unexpected string '%s' is not a port.",
                         Q_FUNC_INFO, portString.data());
            }
        }
        return ports;
    }
};

IDeviceWidget *LinuxDevice::createWidget()
{
    return new GenericLinuxDeviceConfigurationWidget(sharedFromThis());
}

LinuxDevice::LinuxDevice()
{
    setDisplayType(tr("Generic Linux"));
    setDefaultDisplayName(tr("Generic Linux Device"));
    setOsType(OsTypeLinux);

    addDeviceAction({tr("Deploy Public Key..."), [](const IDevice::Ptr &device, QWidget *parent) {
        if (auto d = PublicKeyDeploymentDialog::createDialog(device, parent)) {
            d->exec();
            delete d;
        }
    }});

    setOpenTerminal([this](const Environment &env, const FilePath &workingDir) {
        DeviceProcess * const proc = createProcess(nullptr);
        QObject::connect(proc, &DeviceProcess::finished, [proc] {
            if (!proc->errorString().isEmpty()) {
                Core::MessageManager::writeDisrupting(
                    tr("Error running remote shell: %1").arg(proc->errorString()));
            }
            proc->deleteLater();
        });
        QObject::connect(proc, &DeviceProcess::error, [proc] {
            Core::MessageManager::writeDisrupting(tr("Error starting remote shell."));
            proc->deleteLater();
        });
        Runnable runnable;
        runnable.device = sharedFromThis();
        runnable.environment = env;
        runnable.workingDirectory = workingDir;

        // It seems we cannot pass an environment to OpenSSH dynamically
        // without specifying an executable.
        if (env.size() > 0)
            runnable.command.setExecutable("/bin/sh");

        proc->setRunInTerminal(true);
        proc->start(runnable);
    });

    if (Utils::HostOsInfo::isAnyUnixHost()) {
        addDeviceAction({tr("Open Remote Shell"), [](const IDevice::Ptr &device, QWidget *) {
            device->openTerminal(Environment(), FilePath());
        }});
    }
}

DeviceProcess *LinuxDevice::createProcess(QObject *parent) const
{
    return new LinuxDeviceProcess(sharedFromThis(), parent);
}

bool LinuxDevice::canAutoDetectPorts() const
{
    return true;
}

PortsGatheringMethod::Ptr LinuxDevice::portsGatheringMethod() const
{
    return LinuxPortsGatheringMethod::Ptr(new LinuxPortsGatheringMethod);
}

DeviceProcessList *LinuxDevice::createProcessListModel(QObject *parent) const
{
    return new LinuxDeviceProcessList(sharedFromThis(), parent);
}

DeviceTester *LinuxDevice::createDeviceTester() const
{
    return new GenericLinuxDeviceTester;
}

DeviceProcessSignalOperation::Ptr LinuxDevice::signalOperation() const
{
    return DeviceProcessSignalOperation::Ptr(new RemoteLinuxSignalOperation(sshParameters()));
}

class LinuxDeviceEnvironmentFetcher : public DeviceEnvironmentFetcher
{
public:
    LinuxDeviceEnvironmentFetcher(const IDevice::ConstPtr &device)
        : m_reader(device)
    {
        connect(&m_reader, &Internal::RemoteLinuxEnvironmentReader::finished,
                this, &LinuxDeviceEnvironmentFetcher::readerFinished);
        connect(&m_reader, &Internal::RemoteLinuxEnvironmentReader::error,
                this, &LinuxDeviceEnvironmentFetcher::readerError);
    }

private:
    void start() override { m_reader.start(); }
    void readerFinished() { emit finished(m_reader.remoteEnvironment(), true); }
    void readerError() { emit finished(Utils::Environment(), false); }

    Internal::RemoteLinuxEnvironmentReader m_reader;
};

DeviceEnvironmentFetcher::Ptr LinuxDevice::environmentFetcher() const
{
    return DeviceEnvironmentFetcher::Ptr(new LinuxDeviceEnvironmentFetcher(sharedFromThis()));
}

namespace Internal {

// Factory

LinuxDeviceFactory::LinuxDeviceFactory()
    : IDeviceFactory(Constants::GenericLinuxOsType)
{
    setDisplayName(LinuxDevice::tr("Generic Linux Device"));
    setIcon(QIcon());
    setCanCreate(true);
    setConstructionFunction(&LinuxDevice::create);
}

IDevice::Ptr LinuxDeviceFactory::create() const
{
    GenericLinuxDeviceConfigurationWizard wizard(Core::ICore::dialogParent());
    if (wizard.exec() != QDialog::Accepted)
        return IDevice::Ptr();
    return wizard.device();
}

}
} // namespace RemoteLinux
