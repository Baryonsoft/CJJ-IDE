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

#pragma once

#include <qmakeprojectmanager/qmakestep.h>
#include <qtsupport/baseqtversion.h>
#include <utils/filepath.h>

namespace QmakeProjectManager {
namespace Internal {

struct QMakeAssignment
{
    QString variable;
    QString op;
    QString value;
};

class MakeFileParse
{
public:
    enum class Mode { FilterKnownConfigValues, DoNotFilterKnownConfigValues };
    MakeFileParse(const Utils::FilePath &makefile, Mode mode);

    enum MakefileState { MakefileMissing, CouldNotParse, Okay };

    MakefileState makeFileState() const;
    Utils::FilePath qmakePath() const;
    Utils::FilePath srcProFile() const;
    QMakeStepConfig config() const;

    QString unparsedArguments() const;

    QtSupport::BaseQtVersion::QmakeBuildConfigs
        effectiveBuildConfig(QtSupport::BaseQtVersion::QmakeBuildConfigs defaultBuildConfig) const;

    static const QLoggingCategory &logging();

    void parseCommandLine(const QString &command, const QString &project);

private:
    void parseArgs(const QString &args, const QString &project,
                   QList<QMakeAssignment> *assignments, QList<QMakeAssignment> *afterAssignments);
    QList<QMakeAssignment> parseAssignments(const QList<QMakeAssignment> &assignments);

    class QmakeBuildConfig
    {
    public:
        bool explicitDebug = false;
        bool explicitRelease = false;
        bool explicitBuildAll = false;
        bool explicitNoBuildAll = false;
    };

    const Mode m_mode;
    MakefileState m_state;
    Utils::FilePath m_qmakePath;
    Utils::FilePath m_srcProFile;

    QmakeBuildConfig m_qmakeBuildConfig;
    QMakeStepConfig m_config;
    QString m_unparsedArguments;
};

} // namespace Internal
} // namespace QmakeProjectManager
