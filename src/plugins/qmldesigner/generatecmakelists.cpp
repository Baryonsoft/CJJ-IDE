/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Design Tooling
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

#include "generatecmakelists.h"
#include "cmakegeneratordialog.h"

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/actioncontainer.h>

#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/project.h>
#include <projectexplorer/runcontrol.h>
#include <projectexplorer/session.h>

#include <qmlprojectmanager/qmlprojectmanagerconstants.h>
#include <qmlprojectmanager/qmlmainfileaspect.h>

#include <utils/fileutils.h>

#include <QAction>
#include <QtConcurrent>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

using namespace Utils;

namespace QmlDesigner {

namespace GenerateCmake {

bool operator==(const GeneratableFile &left, const GeneratableFile &right)
{
    return (left.filePath == right.filePath && left.content == right.content);
}

QVector<GeneratableFile> queuedFiles;

void generateMenuEntry()
{
    Core::ActionContainer *buildMenu =
            Core::ActionManager::actionContainer(ProjectExplorer::Constants::M_BUILDPROJECT);
    auto action = new QAction("Generate CMakeLists.txt files");
    QObject::connect(action, &QAction::triggered, GenerateCmake::onGenerateCmakeLists);
    Core::Command *cmd = Core::ActionManager::registerAction(action, "QmlProject.CreateCMakeLists");
    buildMenu->addAction(cmd, ProjectExplorer::Constants::G_BUILD_RUN);

    action->setEnabled(ProjectExplorer::SessionManager::startupProject() != nullptr);
    QObject::connect(ProjectExplorer::SessionManager::instance(),
        &ProjectExplorer::SessionManager::startupProjectChanged, [action]() {
            action->setEnabled(ProjectExplorer::SessionManager::startupProject() != nullptr);
    });
}

void onGenerateCmakeLists()
{
    queuedFiles.clear();
    FilePath rootDir = ProjectExplorer::SessionManager::startupProject()->projectDirectory();
    GenerateCmakeLists::generateMainCmake(rootDir);
    GenerateEntryPoints::generateMainCpp(rootDir);
    GenerateEntryPoints::generateMainQml(rootDir);
    if (showConfirmationDialog(rootDir))
        writeQueuedFiles();
}

void removeUnconfirmedQueuedFiles(const Utils::FilePaths confirmedFiles)
{
    QtConcurrent::blockingFilter(queuedFiles, [confirmedFiles](const GeneratableFile &qf) {
        return confirmedFiles.contains(qf.filePath);
    });
}

bool showConfirmationDialog(const Utils::FilePath &rootDir)
{
    Utils::FilePaths files;
    for (GeneratableFile &file: queuedFiles)
        files.append(file.filePath);

    CmakeGeneratorDialog dialog(rootDir, files);
    if (dialog.exec()) {
        Utils::FilePaths confirmedFiles = dialog.getFilePaths();
        removeUnconfirmedQueuedFiles(confirmedFiles);

        return true;
    }

    return false;
}

bool queueFile(const FilePath &filePath, const QString &fileContent)
{
    GeneratableFile file;
    file.filePath = filePath;
    file.content = fileContent;
    queuedFiles.append(file);

    return true;
}

bool writeQueuedFiles()
{
    for (GeneratableFile &file: queuedFiles)
        if (!writeFile(file))
            return false;

    return true;
}

bool writeFile(const GeneratableFile &file)
{
    QFile fileHandle(file.filePath.toString());
    fileHandle.open(QIODevice::WriteOnly);
    QTextStream stream(&fileHandle);
    stream << file.content;
    fileHandle.close();

    return true;
}

}

namespace GenerateCmakeLists {

const QDir::Filters FILES_ONLY = QDir::Files;
const QDir::Filters DIRS_ONLY = QDir::Dirs|QDir::Readable|QDir::NoDotAndDotDot;

const char CMAKEFILENAME[] = "CMakeLists.txt";
const char QMLDIRFILENAME[] = "qmldir";

QStringList processDirectory(const FilePath &dir)
{
    QStringList moduleNames;

    FilePaths files = dir.dirEntries(FILES_ONLY);
    for (FilePath &file : files) {
        if (!file.fileName().compare(CMAKEFILENAME))
            files.removeAll(file);
    }

    if (files.isEmpty()) {
        generateSubdirCmake(dir);
        FilePaths subDirs = dir.dirEntries(DIRS_ONLY);
        for (FilePath &subDir : subDirs) {
            QStringList subDirModules = processDirectory(subDir);
            moduleNames.append(subDirModules);
        }
    }
    else {
        QString moduleName = generateModuleCmake(dir);
        if (!moduleName.isEmpty()) {
            moduleNames.append(moduleName);
        }
    }

    return moduleNames;
}

const char MAINFILE_REQUIRED_VERSION[] = "cmake_minimum_required(VERSION 3.18)\n\n";
const char MAINFILE_PROJECT[] = "project(%1 LANGUAGES CXX)\n\n";
const char MAINFILE_CMAKE_OPTIONS[] = "set(CMAKE_INCLUDE_CURRENT_DIR ON)\nset(CMAKE_AUTOMOC ON)\n\n";
const char MAINFILE_PACKAGES[] = "find_package(Qt6 COMPONENTS Gui Qml Quick)\n";
const char MAINFILE_LIBRARIES[] = "set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)\n\n";
const char MAINFILE_CPP[] = "add_executable(%1 main.cpp)\n\n";
const char MAINFILE_MAINMODULE[] = "qt6_add_qml_module(%1\n\tURI \"Main\"\n\tVERSION 1.0\n\tNO_PLUGIN\n\tQML_FILES main.qml\n)\n\n";
const char MAINFILE_LINK_LIBRARIES[] = "target_link_libraries(%1 PRIVATE\n\tQt${QT_VERSION_MAJOR}::Core\n\tQt${QT_VERSION_MAJOR}::Gui\n\tQt${QT_VERSION_MAJOR}::Quick\n\tQt${QT_VERSION_MAJOR}::Qml\n)\n\n";

const char ADD_SUBDIR[] = "add_subdirectory(%1)\n";

void generateMainCmake(const FilePath &rootDir)
{
    //TODO startupProject() may be a terrible way to try to get "current project". It's not necessarily the same thing at all.
    QString projectName = ProjectExplorer::SessionManager::startupProject()->displayName();

    FilePaths subDirs = rootDir.dirEntries(DIRS_ONLY);

    QString fileContent;
    fileContent.append(MAINFILE_REQUIRED_VERSION);
    fileContent.append(QString(MAINFILE_PROJECT).arg(projectName));
    fileContent.append(MAINFILE_CMAKE_OPTIONS);
    fileContent.append(MAINFILE_PACKAGES);
    fileContent.append(QString(MAINFILE_CPP).arg(projectName));
    fileContent.append(QString(MAINFILE_MAINMODULE).arg(projectName));
    fileContent.append(MAINFILE_LIBRARIES);

    for (FilePath &subDir : subDirs) {
        QStringList subDirModules = processDirectory(subDir);
        if (!subDirModules.isEmpty())
            fileContent.append(QString(ADD_SUBDIR).arg(subDir.fileName()));
    }
    fileContent.append("\n");

    fileContent.append(QString(MAINFILE_LINK_LIBRARIES).arg(projectName));

    createCmakeFile(rootDir, fileContent);
}

const char MODULEFILE_PROPERTY_SINGLETON[] = "QT_QML_SINGLETON_TYPE";
const char MODULEFILE_PROPERTY_SET[] = "set_source_files_properties(%1\n\tPROPERTIES\n\t\t%2 %3\n)\n\n";
const char MODULEFILE_CREATE_MODULE[] = "qt6_add_qml_module(%1\n\tURI \"%1\"\n\tVERSION 1.0\n%2)\n\n";


QString generateModuleCmake(const FilePath &dir)
{
    QString fileContent;
    const QStringList qmldirFilesOnly(QMLDIRFILENAME);

    FilePaths qmldirFileList = dir.dirEntries(qmldirFilesOnly, FILES_ONLY);
    if (!qmldirFileList.isEmpty()) {
        QStringList singletons = getSingletonsFromQmldirFile(qmldirFileList.first());
        for (QString &singleton : singletons) {
            fileContent.append(QString(MODULEFILE_PROPERTY_SET).arg(singleton).arg(MODULEFILE_PROPERTY_SINGLETON).arg("true"));
        }
    }

    QStringList qmlFileList = getDirectoryTreeQmls(dir);
    QString qmlFiles;
    for (QString &qmlFile : qmlFileList)
        qmlFiles.append(QString("\t\t%1\n").arg(qmlFile));

    QStringList resourceFileList = getDirectoryTreeResources(dir);
    QString resourceFiles;
    for (QString &resourceFile : resourceFileList)
        resourceFiles.append(QString("\t\t%1\n").arg(resourceFile));

    QString moduleContent;
    if (!qmlFiles.isEmpty()) {
        moduleContent.append(QString("\tQML_FILES\n%1").arg(qmlFiles));
    }
    if (!resourceFiles.isEmpty()) {
        moduleContent.append(QString("\tRESOURCES\n%1").arg(resourceFiles));
    }

    QString moduleName = dir.fileName();

    fileContent.append(QString(MODULEFILE_CREATE_MODULE).arg(moduleName).arg(moduleContent));

    createCmakeFile(dir, fileContent);

    return moduleName;
}

void generateSubdirCmake(const FilePath &dir)
{
    QString fileContent;
    FilePaths subDirs = dir.dirEntries(DIRS_ONLY);

    for (FilePath &subDir : subDirs) {
        fileContent.append(QString(ADD_SUBDIR).arg(subDir.fileName()));
    }

    createCmakeFile(dir, fileContent);
}

QStringList getSingletonsFromQmldirFile(const FilePath &filePath)
{
    QStringList singletons;
    QFile f(filePath.toString());
    f.open(QIODevice::ReadOnly);
    QTextStream stream(&f);

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (line.startsWith("singleton", Qt::CaseInsensitive)) {
            QStringList tokenizedLine = line.split(QRegularExpression("\\s+"));
            QString fileName = tokenizedLine.last();
            if (fileName.endsWith(".qml", Qt::CaseInsensitive)) {
                singletons.append(fileName);
            }
        }
    }

    f.close();

    return singletons;
}

QStringList getDirectoryTreeQmls(const FilePath &dir)
{
    const QStringList qmlFilesOnly("*.qml");
    ProjectExplorer::Project *project = ProjectExplorer::SessionManager::startupProject();
    QStringList qmlFileList;

    FilePaths thisDirFiles = dir.dirEntries(qmlFilesOnly, FILES_ONLY);
    for (FilePath &file : thisDirFiles) {
        if (!isFileBlacklisted(file.fileName()) &&
            project->isKnownFile(file)) {
            qmlFileList.append(file.fileName());
        }
    }

    FilePaths subDirsList = dir.dirEntries(DIRS_ONLY);
    for (FilePath &subDir : subDirsList) {
        QStringList subDirQmlFiles = getDirectoryTreeQmls(subDir);
        for (QString &qmlFile : subDirQmlFiles) {
            qmlFileList.append(subDir.fileName().append('/').append(qmlFile));
        }
    }

    return qmlFileList;
}

QStringList getDirectoryTreeResources(const FilePath &dir)
{
    ProjectExplorer::Project *project = ProjectExplorer::SessionManager::startupProject();
    QStringList resourceFileList;

    FilePaths thisDirFiles = dir.dirEntries(FILES_ONLY);
    for (FilePath &file : thisDirFiles) {
        if (!isFileBlacklisted(file.fileName()) &&
            !file.fileName().endsWith(".qml", Qt::CaseInsensitive) &&
            project->isKnownFile(file)) {
            resourceFileList.append(file.fileName());
        }
    }

    FilePaths subDirsList = dir.dirEntries(DIRS_ONLY);
    for (FilePath &subDir : subDirsList) {
        QStringList subDirResources = getDirectoryTreeResources(subDir);
        for (QString &resource : subDirResources) {
            resourceFileList.append(subDir.fileName().append('/').append(resource));
        }

    }

    return resourceFileList;
}

void createCmakeFile(const FilePath &dir, const QString &content)
{
    FilePath filePath = dir.pathAppended(CMAKEFILENAME);
    GenerateCmake::queueFile(filePath, content);
}

bool isFileBlacklisted(const QString &fileName)
{
    return (!fileName.compare(QMLDIRFILENAME) ||
            !fileName.compare(CMAKEFILENAME));
}

}

namespace GenerateEntryPoints {
bool generateEntryPointFiles(const FilePath &dir)
{
    bool cppOk = generateMainCpp(dir);
    bool qmlOk = generateMainQml(dir);

    return cppOk && qmlOk;
}

const char MAIN_CPPFILE_CONTENT[] = ":/boilerplatetemplates/qmlprojectmaincpp.tpl";
const char MAIN_CPPFILE_NAME[] = "main.cpp";

bool generateMainCpp(const FilePath &dir)
{
    QFile templatefile(MAIN_CPPFILE_CONTENT);
    templatefile.open(QIODevice::ReadOnly);
    QTextStream stream(&templatefile);
    QString content = stream.readAll();
    templatefile.close();

    FilePath filePath = dir.pathAppended(MAIN_CPPFILE_NAME);
    return GenerateCmake::queueFile(filePath, content);
}

const char MAIN_QMLFILE_CONTENT[] = "import %1Qml\n\n%2 {\n}\n";
const char MAIN_QMLFILE_NAME[] = "main.qml";

bool generateMainQml(const FilePath &dir)
{
    FilePath filePath = dir.pathAppended(MAIN_QMLFILE_NAME);
    QString projectName = ProjectExplorer::SessionManager::startupProject()->displayName();
    ProjectExplorer::RunConfiguration *runConfiguration = ProjectExplorer::SessionManager::startupRunConfiguration();
    QString mainClass;

    if (const auto aspect = runConfiguration->aspect<QmlProjectManager::QmlMainFileAspect>())
        mainClass = FilePath::fromString(aspect->mainScript()).baseName();

    return GenerateCmake::queueFile(filePath, QString(MAIN_QMLFILE_CONTENT).arg(projectName).arg(mainClass));
}

}

}

