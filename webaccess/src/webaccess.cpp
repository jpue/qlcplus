/*
  Q Light Controller Plus
  webaccess.cpp

  Copyright (c) Massimo Callegari

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <QDebug>
#include <QProcess>
#include <QSettings>
#include <QMap>
#include <qmath.h>

#include "webaccess.h"

#include "webaccessauth.h"
#include "webaccessconfiguration.h"
#include "webaccesssimpledesk.h"
#include "webaccessnetwork.h"

#include "virtualconsole.h"
#include "rgbalgorithm.h"
#include "commonjscss.h"
#include "vcsoloframe.h"
#include "outputpatch.h"
#include "inputpatch.h"
#include "simpledesk.h"
#include "qlcconfig.h"
#include "webaccess.h"
#include "vccuelist.h"
#include "vcbutton.h"
#include "vcslider.h"
#include "function.h"
#include "vclabel.h"
#include "vcframe.h"
#include "vcclock.h"
#include "vcaudiotriggers.h"
#include "vcxypad.h"
#include "qlcfile.h"
#include "chaser.h"
#include "doc.h"
#include "grandmaster.h"

#ifndef QMLUI
  #include "vcmatrix.h"
  #include "vcframepageshortcut.h"
#else
  #include "vcanimation.h"
  #include "vcpage.h"
#endif

#include "audiocapture.h"
#include "audiorenderer.h"

#include "qhttpserver.h"
#include "qhttprequest.h"
#include "qhttpresponse.h"
#include "qhttpconnection.h"

#define DEFAULT_PORT_NUMBER    9999
#define AUTOSTART_PROJECT_NAME "autostart.qxw"

WebAccess::WebAccess(Doc *doc, VirtualConsole *vcInstance, SimpleDesk *sdInstance,
                     int portNumber, bool enableAuth, QString passwdFile, QObject *parent) :
    QObject(parent)
  , m_doc(doc)
  , m_vc(vcInstance)
  , m_sd(sdInstance)
  , m_auth(NULL)
  , m_pendingProjectLoaded(false)
{
    Q_ASSERT(m_doc != NULL);
    Q_ASSERT(m_vc != NULL);

    if (enableAuth)
    {
        m_auth = new WebAccessAuth(QString("QLC+ web access"));
        m_auth->loadPasswordsFile(passwdFile);
    }

    m_httpServer = new QHttpServer(this);
    connect(m_httpServer, SIGNAL(newRequest(QHttpRequest*, QHttpResponse*)),
            this, SLOT(slotHandleHTTPRequest(QHttpRequest*, QHttpResponse*)));
    connect(m_httpServer, SIGNAL(webSocketDataReady(QHttpConnection*,QString)),
            this, SLOT(slotHandleWebSocketRequest(QHttpConnection*,QString)));
    connect(m_httpServer, SIGNAL(webSocketConnectionClose(QHttpConnection*)),
            this, SLOT(slotHandleWebSocketClose(QHttpConnection*)));

    m_httpServer->listen(QHostAddress::Any, portNumber ? portNumber : DEFAULT_PORT_NUMBER);

#if defined(Q_WS_X11) || defined(Q_OS_LINUX)
    m_netConfig = new WebAccessNetwork();
#endif

    connect(m_doc->masterTimer(), SIGNAL(functionStarted(quint32)),
            this, SLOT(slotFunctionStarted(quint32)));
    connect(m_doc->masterTimer(), SIGNAL(functionStopped(quint32)),
            this, SLOT(slotFunctionStopped(quint32)));

  #ifndef QMLUI
    connect(m_vc, SIGNAL(loaded()),
            this, SLOT(slotVCLoaded()));
  #endif
}

WebAccess::~WebAccess()
{
#if defined(Q_WS_X11) || defined(Q_OS_LINUX)
    delete m_netConfig;
#endif
    foreach (QHttpConnection *conn, m_webSocketsList)
        delete conn;

    if (m_auth)
        delete m_auth;
}

void WebAccess::slotHandleHTTPRequest(QHttpRequest *req, QHttpResponse *resp)
{
    WebAccessUser user;

    if (m_auth)
    {
        user = m_auth->authenticateRequest(req, resp);

        if (user.level < LOGGED_IN_LEVEL)
        {
            m_auth->sendUnauthorizedResponse(resp);
            return;
        }
    }

    QString reqUrl = req->url().toString();
    QString content;

    qDebug() << Q_FUNC_INFO << req->methodString() << req->url();

    if (reqUrl == "/qlcplusWS")
    {
        QHttpConnection *conn = resp->enableWebSocket();
        if (conn != NULL)
        {
            // Allocate user for WS on heap so it doesn't go out of scope
            conn->userData = new WebAccessUser(user);
            m_webSocketsList.append(conn);
        }

        return;
    }
    else if (reqUrl == "/loadProject")
    {
        if (m_auth && user.level < SUPER_ADMIN_LEVEL)
        {
            m_auth->sendUnauthorizedResponse(resp);
            return;
        }
        QByteArray projectXML = req->body();

        projectXML.remove(0, projectXML.indexOf("\n\r\n") + 3);
        projectXML.truncate(projectXML.lastIndexOf("\n\r\n"));

        //qDebug() << "Project XML:\n\n" << QString(projectXML) << "\n\n";
        qDebug() << "Workspace XML received. Content-Length:" << req->headers().value("content-length") << projectXML.size();

        QByteArray postReply =
                QString("<html><head>\n<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\" />\n"
                "<script type=\"text/javascript\">\n" PROJECT_LOADED_JS
                "</script></head><body style=\"background-color: #45484d;\">"
                "<div style=\"position: absolute; width: 100%; height: 30px; top: 50%; background-color: #888888;"
                "text-align: center; font:bold 24px/1.2em sans-serif;\">"
                + tr("Loading project...") +
                "</div></body></html>").toUtf8();

        resp->setHeader("Content-Type", "text/html");
        resp->setHeader("Content-Length", QString::number(postReply.size()));
        resp->writeHead(200);
        resp->end(postReply);

        m_pendingProjectLoaded = false;

        emit loadProject(QString(projectXML).toUtf8());

        return;
    }
    else if (reqUrl == "/loadFixture")
    {
        if (m_auth && user.level < SUPER_ADMIN_LEVEL)
        {
            m_auth->sendUnauthorizedResponse(resp);
            return;
        }
        QByteArray fixtureXML = req->body();
        int fnamePos = fixtureXML.indexOf("filename=") + 10;
        QString fxName = fixtureXML.mid(fnamePos, fixtureXML.indexOf("\"", fnamePos) - fnamePos);

        fixtureXML.remove(0, fixtureXML.indexOf("\n\r\n") + 3);
        fixtureXML.truncate(fixtureXML.lastIndexOf("\n\r\n"));

        qDebug() << "Fixture name:" << fxName;
        qDebug() << "Fixture XML:\n\n" << fixtureXML << "\n\n";

        m_doc->fixtureDefCache()->storeFixtureDef(fxName, QString(fixtureXML).toUtf8());

        QByteArray postReply =
                      QString("<html><head>\n<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\" />\n"
                      "<script type=\"text/javascript\">\n"
                      " alert(\"" + tr("Fixture stored and loaded") + "\");"
                      " window.location = \"/config\"\n"
                      "</script></head></html>").toUtf8();

        resp->setHeader("Content-Type", "text/html");
        resp->setHeader("Content-Length", QString::number(postReply.size()));
        resp->writeHead(200);
        resp->end(postReply);

        return;
    }
    else if (reqUrl == "/config")
    {
        if (m_auth && user.level < SUPER_ADMIN_LEVEL)
        {
            m_auth->sendUnauthorizedResponse(resp);
            return;
        }
        content = WebAccessConfiguration::getHTML(m_doc, m_auth);
    }
    else if (reqUrl == "/simpleDesk")
    {
        if (m_auth && user.level < SIMPLE_DESK_AND_VC_LEVEL)
        {
            m_auth->sendUnauthorizedResponse(resp);
            return;
        }
        content = WebAccessSimpleDesk::getHTML(m_doc, m_sd);
    }
    else if (reqUrl == "/keypad")
    {
        if (m_auth && user.level < SIMPLE_DESK_AND_VC_LEVEL)
        {
            m_auth->sendUnauthorizedResponse(resp);
            return;
        }
        content = WebAccessSimpleDesk::getKeypadHTML();
    }
#if defined(Q_WS_X11) || defined(Q_OS_LINUX)
    else if (reqUrl == "/system")
    {
        if (m_auth && user.level < SUPER_ADMIN_LEVEL)
        {
            m_auth->sendUnauthorizedResponse(resp);
            return;
        }
        content = m_netConfig->getHTML();
    }
#endif
    else if (reqUrl.endsWith(".png"))
    {
        // is this an internal resource?
        QString localFilePath = QString(":%1").arg(reqUrl);
        QFile resFile(localFilePath);
        if (!resFile.exists())
        {
            // is this an absolute path?
            localFilePath = reqUrl;
            resFile.setFileName(localFilePath);
            if (!resFile.exists())
            {
                // is this a webaccess file?
                localFilePath = QString("%1%2%3").arg(QLCFile::systemDirectory(WEBFILESDIR).path())
                    .arg(QDir::separator()).arg(reqUrl.mid(1));
            }
        }
        if (sendFile(resp, localFilePath, "image/png") == true)
            return;
    }
    else if (reqUrl.endsWith(".jpg") || reqUrl.endsWith(".jpeg"))
    {
        if (sendFile(resp, reqUrl, "image/jpg") == true)
            return;
    }
    else if (reqUrl.endsWith(".bmp"))
    {
        if (sendFile(resp, reqUrl, "image/bmp") == true)
            return;
    }
    else if (reqUrl.endsWith(".svg"))
    {
        if (sendFile(resp, reqUrl, "image/svg+xml") == true)
            return;
    }
    else if (reqUrl.endsWith(".ico"))
    {
        QString clUri = reqUrl.mid(1);
        if (sendFile(resp, QString("%1%2%3").arg(QLCFile::systemDirectory(WEBFILESDIR).path())
                     .arg(QDir::separator()).arg(clUri), "image/x-icon") == true)
            return;
    }
    else if (reqUrl.endsWith(".css"))
    {
        QString clUri = reqUrl.mid(1);
        if (sendFile(resp, QString("%1%2%3").arg(QLCFile::systemDirectory(WEBFILESDIR).path())
                     .arg(QDir::separator()).arg(clUri), "text/css") == true)
            return;
    }
    else if (reqUrl.endsWith(".js"))
    {
        QString clUri = reqUrl.mid(1);
        if (sendFile(resp, QString("%1%2%3").arg(QLCFile::systemDirectory(WEBFILESDIR).path())
                     .arg(QDir::separator()).arg(clUri), "text/javascript") == true)
            return;
    }
    else if (reqUrl.endsWith(".html"))
    {
        QString clUri = reqUrl.mid(1);
        if (sendFile(resp, QString("%1%2%3").arg(QLCFile::systemDirectory(WEBFILESDIR).path())
                     .arg(QDir::separator()).arg(clUri), "text/html") == true)
            return;
    }
    else if (reqUrl != "/")
    {
        resp->writeHead(404);
        resp->setHeader("Content-Type", "text/plain");
        resp->setHeader("Content-Length", "14");
        resp->end(QByteArray("404 Not found"));
        return;
    }
    else
        content = getVCHTML();

    // Prepare the message we're going to send
    QByteArray contentArray = content.toUtf8();

    // Send HTTP reply to the client
    resp->setHeader("Content-Type", "text/html");
    resp->setHeader("Content-Length", QString::number(contentArray.size()));
    resp->writeHead(200);
    resp->end(contentArray);

    return;
}

void WebAccess::slotHandleWebSocketRequest(QHttpConnection *conn, QString data)
{
    if (conn == NULL)
        return;

    WebAccessUser *user = static_cast<WebAccessUser*>(conn->userData);

    qDebug() << "[websocketDataHandler]" << data;

    QStringList cmdList = data.split("|");
    if (cmdList.isEmpty())
        return;

    if (cmdList[0] == "QLC+CMD")
    {
        if (cmdList.count() < 2)
            return;

        if (cmdList[1] == "opMode")
            emit toggleDocMode();

        return;
    }
    else if (cmdList[0] == "QLC+IO")
    {
        if (m_auth && user && user->level < SUPER_ADMIN_LEVEL)
            return;

        if (cmdList.count() < 3)
            return;

        int universe = cmdList[2].toInt();

        if (cmdList[1] == "INPUT")
        {
            m_doc->inputOutputMap()->setInputPatch(universe, cmdList[3], "", cmdList[4].toUInt());
            m_doc->inputOutputMap()->saveDefaults();
        }
        else if (cmdList[1] == "OUTPUT")
        {
            m_doc->inputOutputMap()->setOutputPatch(universe, cmdList[3], "", cmdList[4].toUInt(), false);
            m_doc->inputOutputMap()->saveDefaults();
        }
        else if (cmdList[1] == "FB")
        {
            m_doc->inputOutputMap()->setOutputPatch(universe, cmdList[3], "", cmdList[4].toUInt(), true);
            m_doc->inputOutputMap()->saveDefaults();
        }
        else if (cmdList[1] == "PROFILE")
        {
            InputPatch *inPatch = m_doc->inputOutputMap()->inputPatch(universe);
            if (inPatch != NULL)
            {
                m_doc->inputOutputMap()->setInputPatch(universe, inPatch->pluginName(), "", inPatch->input(), cmdList[3]);
                m_doc->inputOutputMap()->saveDefaults();
            }
        }
        else if (cmdList[1] == "PASSTHROUGH")
        {
            quint32 uniIdx = cmdList[2].toUInt();
            if (cmdList[3] == "true")
                m_doc->inputOutputMap()->setUniversePassthrough(uniIdx, true);
            else
                m_doc->inputOutputMap()->setUniversePassthrough(uniIdx, false);
            m_doc->inputOutputMap()->saveDefaults();
        }
        else if (cmdList[1] == "AUDIOIN")
        {
            QSettings settings;
            if (cmdList[2] == "__qlcplusdefault__")
                settings.remove(SETTINGS_AUDIO_INPUT_DEVICE);
            else
            {
                settings.setValue(SETTINGS_AUDIO_INPUT_DEVICE, cmdList[2]);
                m_doc->destroyAudioCapture();
            }
        }
        else if (cmdList[1] == "AUDIOOUT")
        {
            QSettings settings;
            if (cmdList[2] == "__qlcplusdefault__")
                settings.remove(SETTINGS_AUDIO_OUTPUT_DEVICE);
            else
                settings.setValue(SETTINGS_AUDIO_OUTPUT_DEVICE, cmdList[2]);
        }
        else
            qDebug() << "[webaccess] Command" << cmdList[1] << "not supported!";

        return;
    }
    else if (cmdList[0] == "QLC+AUTH" && m_auth)
    {
        if (user && user->level < SUPER_ADMIN_LEVEL)
            return;

        if (cmdList.at(1) == "ADD_USER")
        {
            QString username = cmdList.at(2);
            QString password = cmdList.at(3);
            int level = cmdList.at(4).toInt();
            if (username.isEmpty() || password.isEmpty())
            {
                QString wsMessage = QString("ALERT|" + tr("Username and password are required fields."));
                conn->webSocketWrite(wsMessage);
                return;
            }
            if (level <= 0)
            {
                QString wsMessage = QString("ALERT|" + tr("User level has to be a positive integer."));
                conn->webSocketWrite(wsMessage);
                return;
            }

            m_auth->addUser(username, password, (WebAccessUserLevel)level);
        }
        else if (cmdList.at(1) == "DEL_USER")
        {
            QString username = cmdList.at(2);
            if (! username.isEmpty())
                m_auth->deleteUser(username);
        }
        else if (cmdList.at(1) == "SET_USER_LEVEL")
        {
            QString username = cmdList.at(2);
            int level = cmdList.at(3).toInt();
            if (username.isEmpty())
            {
                QString wsMessage = QString("ALERT|" + tr("Username is required."));
                conn->webSocketWrite(wsMessage);
                return;
            }
            if (level <= 0)
            {
                QString wsMessage = QString("ALERT|" + tr("User level has to be a positive integer."));
                conn->webSocketWrite(wsMessage);
                return;
            }

            m_auth->setUserLevel(username, (WebAccessUserLevel)level);
        }
        else
            qDebug() << "[webaccess] Command" << cmdList[1] << "not supported!";

        if (!m_auth->savePasswordsFile())
        {
            QString wsMessage = QString("ALERT|" + tr("Error while saving passwords file."));
            conn->webSocketWrite(wsMessage);
            return;
        }
    }
#if defined(Q_WS_X11) || defined(Q_OS_LINUX)
    else if (cmdList[0] == "QLC+SYS")
    {
        if (m_auth && user && user->level < SUPER_ADMIN_LEVEL)
            return;

        if (cmdList.at(1) == "NETWORK")
        {
            QString wsMessage;
            if (m_netConfig->updateNetworkSettings(cmdList) == true)
                wsMessage = QString("ALERT|" + tr("Network configuration changed. Reboot to apply the changes."));
            else
                wsMessage = QString("ALERT|" + tr("An error occurred while updating the network configuration."));

            conn->webSocketWrite(wsMessage);
            return;
        }
        else if (cmdList.at(1) == "HOTSPOT")
        {
            QString wsMessage;
            if (cmdList.count() < 5)
                return;

            bool enable = cmdList.at(2).toInt();

            if (enable)
            {
                if (m_netConfig->createWiFiHotspot(cmdList.at(3), cmdList.at(4)) == true)
                    wsMessage = QString("ALERT|" + tr("Wi-Fi hotspot successfully activated."));
                else
                    wsMessage = QString("ALERT|" + tr("An error occurred while creating a Wi-Fi hotspot."));
            }
            else
            {
                m_netConfig->deleteWiFiHotspot();
                wsMessage = QString("ALERT|" + tr("Wi-Fi hotspot successfully deactivated."));
            }

            conn->webSocketWrite(wsMessage);
            return;
        }
        else if (cmdList.at(1) == "AUTOSTART")
        {
            if (cmdList.count() < 3)
                return;

            QString asName = QString("%1/%2/%3").arg(getenv("HOME")).arg(USERQLCPLUSDIR).arg(AUTOSTART_PROJECT_NAME);
            if (cmdList.at(2) == "none")
                QFile::remove(asName);
            else
                emit storeAutostartProject(asName);
            QString wsMessage = QString("ALERT|" + tr("Autostart configuration changed"));
            conn->webSocketWrite(wsMessage);
            return;
        }
        else if (cmdList.at(1) == "REBOOT")
        {
            QProcess *rebootProcess = new QProcess();
            rebootProcess->start("sudo", QStringList() << "shutdown" << "-r" << "now");
        }
        else if (cmdList.at(1) == "HALT")
        {
            QProcess *haltProcess = new QProcess();
            haltProcess->start("sudo", QStringList() << "shutdown" << "-h" << "now");
        }
    }
#endif
    else if (cmdList[0] == "QLC+API")
    {
        if (m_auth && user && user->level < VC_ONLY_LEVEL)
            return;

        if (cmdList.count() < 2)
            return;

        QString apiCmd = cmdList[1];
        // compose the basic API reply messages
        QString wsAPIMessage = QString("QLC+API|%1|").arg(apiCmd);

        if (apiCmd == "isProjectLoaded")
        {
            if (m_pendingProjectLoaded)
            {
                wsAPIMessage.append("true");
                m_pendingProjectLoaded = false;
            }
            else
                wsAPIMessage.append("false");
        }
        else if (apiCmd == "getFunctionsNumber")
        {
            wsAPIMessage.append(QString::number(m_doc->functions().count()));
        }
        else if (apiCmd == "getFunctionsList")
        {
            foreach (Function *f, m_doc->functions())
                wsAPIMessage.append(QString("%1|%2|").arg(f->id()).arg(f->name()));
            // remove trailing separator
            wsAPIMessage.truncate(wsAPIMessage.length() - 1);
        }
        else if (apiCmd == "getFunctionType")
        {
            if (cmdList.count() < 3)
                return;

            quint32 fID = cmdList[2].toUInt();
            Function *f = m_doc->function(fID);
            if (f != NULL)
                wsAPIMessage.append(m_doc->function(fID)->typeString());
            else
                wsAPIMessage.append(Function::typeToString(Function::Undefined));
        }
        else if (apiCmd == "getFunctionStatus")
        {
            if (cmdList.count() < 3)
                return;

            quint32 fID = cmdList[2].toUInt();
            Function *f = m_doc->function(fID);
            if (f != NULL)
            {
                if (f->isRunning())
                    wsAPIMessage.append("Running");
                else
                    wsAPIMessage.append("Stopped");
            }
            else
                wsAPIMessage.append(Function::typeToString(Function::Undefined));
        }
        else if (apiCmd == "setFunctionStatus")
	{
            if (cmdList.count() < 4)
                return;

            quint32 fID = cmdList[2].toUInt();
            quint32 newStatus = cmdList[3].toUInt();
            Function *f = m_doc->function(fID);

            if (f != NULL)
            {
                if (!f->isRunning() && newStatus)
                    f->start(m_doc->masterTimer(), FunctionParent::master());
                else if (f->isRunning() && !newStatus)
                    f->stop(FunctionParent::master());
            }
            return;
        }
        else if (apiCmd == "getWidgetsNumber")
        {
          #ifndef QMLUI
            VCFrame *mainFrame = m_vc->contents();
            QList<VCWidget *> chList = mainFrame->findChildren<VCWidget*>();
            wsAPIMessage.append(QString::number(chList.count()));
          #else
            int count = 0;
            if (m_vc->pagesCount() > m_vc->selectedPage())
            {
                VCPage *currentPage = m_vc->page(m_vc->selectedPage());
                count = currentPage->children(true).count();
            }
            wsAPIMessage.append(QString::number(count));
          #endif
        }
        else if (apiCmd == "getWidgetsList")
        {
          #ifndef QMLUI
            VCFrame *mainFrame = m_vc->contents();
            foreach (VCWidget *widget, mainFrame->findChildren<VCWidget*>())
                wsAPIMessage.append(QString("%1|%2|").arg(widget->id()).arg(widget->caption()));
            // remove trailing separator
            wsAPIMessage.truncate(wsAPIMessage.length() - 1);
          #else
            VCPage *currentPage = m_vc->page(m_vc->selectedPage());
            for (const VCWidget* widget : currentPage->children(true))
            {
                wsAPIMessage.append(QString("%1|%2|").arg(widget->id()).arg(widget->caption()));
            }
            wsAPIMessage.truncate(wsAPIMessage.length() - 1);
          #endif
        }
        else if (apiCmd == "getWidgetType")
        {
            if (cmdList.count() < 3)
                return;

            quint32 wID = cmdList[2].toUInt();
            VCWidget *widget = m_vc->widget(wID);
            if (widget != NULL)
                wsAPIMessage.append(QString("%1|%2").arg(wID).arg(widget->typeToString(widget->type())));
            else
                wsAPIMessage.append(QString("%1|%2").arg(wID).arg(widget->typeToString(VCWidget::UnknownWidget)));
        }
        else if (apiCmd == "getWidgetStatus")
        {
            if (cmdList.count() < 3)
                return;

            quint32 wID = cmdList[2].toUInt();
            VCWidget *widget = m_vc->widget(wID);
            if (widget != NULL)
            {
                // add widget ID to the response
                wsAPIMessage.append(QString("%1|").arg(wID));

                switch(widget->type())
                {
                    case VCWidget::ButtonWidget:
                    {
                        VCButton *button = qobject_cast<VCButton*>(widget);
                        if (button->state() == VCButton::Active)
                            wsAPIMessage.append("255");
                        else if (button->state() == VCButton::Monitoring)
                            wsAPIMessage.append("127");
                        else
                            wsAPIMessage.append("0");
                    }
                    break;
                    case VCWidget::SliderWidget:
                    {
                        VCSlider *slider = qobject_cast<VCSlider*>(widget);
                      #ifndef QMLUI
                        wsAPIMessage.append(QString::number(slider->sliderValue()));
                      #else
                        wsAPIMessage.append(QString::number(slider->value()));
                      #endif
                    }
                    break;
                    case VCWidget::CueListWidget:
                    {
                        VCCueList *cue = qobject_cast<VCCueList*>(widget);
                        quint32 chaserID = cue->chaserID();
                        Function *f = m_doc->function(chaserID);
                        if (f != NULL && f->isRunning())
                          #ifndef QMLUI
                            wsAPIMessage.append(QString("PLAY|%2|").arg(cue->getCurrentIndex()));
                          #else
                            wsAPIMessage.append(QString("PLAY|%2|").arg(cue->playbackIndex()));
                          #endif
                        else
                            wsAPIMessage.append("STOP");
                    }
                    break;
                    case VCWidget::AnimationWidget:
                    {
                      #ifndef QMLUI
                        VCMatrix *animation = qobject_cast<VCMatrix*>(widget);
                        wsAPIMessage.append(QString::number(animation->sliderValue()));
                      #else
                        VCAnimation *animation = qobject_cast<VCAnimation*>(widget);
                        wsAPIMessage.append(QString::number(animation->faderLevel()));
                      #endif
                    }
                    break;
                    default:
                    {
                        wsAPIMessage.append("0");
                    }
                    break;
                }
            }
        }
        else if (apiCmd == "getWidgetSubIdList")
        {
            if (cmdList.count() < 3)
                return;

            quint32 wID = cmdList[2].toUInt();
            VCWidget *widget = m_vc->widget(wID);
            switch(widget->type())
            {
                case VCWidget::AnimationWidget:
                {
                  #ifndef QMLUI
                    VCMatrix *animation = qobject_cast<VCMatrix*>(widget);

                    QMapIterator <quint32,QString> it(animation->customControlsMap());
                    while (it.hasNext() == true)
                    {
                        it.next();
                        wsAPIMessage.append(QString("%1|%2|").arg(it.key()).arg(it.value()));
                    }
                    // remove trailing separator
                    wsAPIMessage.truncate(wsAPIMessage.length() - 1);
                  #else
                    // TODO
                    //VCAnimation *animation = qobject_cast<VCAnimation*>(widget);
                  #endif
                }
                break;
                case VCWidget::XYPadWidget:
                {
                    VCXYPad *xypad = qobject_cast<VCXYPad*>(widget);
                  #ifndef QMLUI
                    QMapIterator <quint32,QString> it(xypad->presetsMap());
                    while (it.hasNext() == true)
                    {
                        it.next();
                        wsAPIMessage.append(QString("%1|%2|").arg(it.key()).arg(it.value()));
                    }
                    // remove trailing separator
                    wsAPIMessage.truncate(wsAPIMessage.length() - 1);
                  #else
                    // TODO
                    Q_UNUSED(xypad);
                  #endif
                }
                break;
            }
        }
        else if (apiCmd == "getChannelsValues")
        {
            if (m_auth && user && user->level < SIMPLE_DESK_AND_VC_LEVEL)
                return;

            if (cmdList.count() < 4)
                return;

            quint32 universe = cmdList[2].toUInt() - 1;
            int startAddr = cmdList[3].toInt() - 1;
            int count = 1;
            if (cmdList.count() == 5)
                count = cmdList[4].toInt();

            wsAPIMessage.append(WebAccessSimpleDesk::getChannelsMessage(m_doc, m_sd, universe, startAddr, count));
        }
        else if (apiCmd == "sdResetChannel")
        {
            if (m_auth && user && user->level < SIMPLE_DESK_AND_VC_LEVEL)
                return;

            if (cmdList.count() < 3)
                return;

            quint32 chNum = cmdList[2].toUInt() - 1;

          #ifndef QMLUI
            m_sd->resetChannel(chNum);
          #else
            const quint32 universe = chNum >> 9;
            if (m_sd->universeFilter() != universe)
                m_sd->setUniverseFilter(universe);
            m_sd->resetChannel(chNum % 512);
          #endif

            wsAPIMessage = "QLC+API|getChannelsValues|";
          #ifndef QMLUI
            wsAPIMessage.append(WebAccessSimpleDesk::getChannelsMessage(
                                m_doc, m_sd, m_sd->getCurrentUniverseIndex(),
                                (m_sd->getCurrentPage() - 1) * m_sd->getSlidersNumber(), m_sd->getSlidersNumber()));
          #else
            wsAPIMessage.append(WebAccessSimpleDesk::getChannelsMessage(m_doc, m_sd, universe, 0, 512));
          #endif
        }
        else if (apiCmd == "sdResetUniverse")
        {
            if (m_auth && user && user->level < SIMPLE_DESK_AND_VC_LEVEL)
                return;

            if (cmdList.count() < 3)
                return;

            quint32 universeIndex = cmdList[2].toUInt() - 1;
            m_sd->resetUniverse(universeIndex);
            wsAPIMessage = "QLC+API|getChannelsValues|";
          #ifndef QMLUI
            wsAPIMessage.append(WebAccessSimpleDesk::getChannelsMessage(
                                m_doc, m_sd, m_sd->getCurrentUniverseIndex(),
                                0, m_sd->getSlidersNumber()));
          #else
            wsAPIMessage.append(WebAccessSimpleDesk::getChannelsMessage(m_doc, m_sd, universeIndex, 0, 512));
          #endif
        }
        //qDebug() << "Simple desk channels:" << wsAPIMessage;

        conn->webSocketWrite(wsAPIMessage);
        return;
    }
    else if (cmdList[0] == "CH")
    {
        if (m_auth && user && user->level < SIMPLE_DESK_AND_VC_LEVEL)
            return;

        if (cmdList.count() < 3)
            return;

        uint absAddress = cmdList[1].toInt() - 1;
        int value = cmdList[2].toInt();
      #ifndef QMLUI
        m_sd->setAbsoluteChannelValue(absAddress, uchar(value));
      #else
        quint32 fxID = m_doc->fixtureForAddress(absAddress);
        Fixture* fxi = m_doc->fixture(fxID);

        const quint32 universe = absAddress >> 9;
        if (m_sd->universeFilter() != universe)
            m_sd->setUniverseFilter(universe);

        if (fxi != NULL)
            m_sd->setValue(fxID, absAddress - fxi->universeAddress(), value);
        else
            m_sd->setValue(-1, absAddress % 512, value);
      #endif
        return;
    }
    else if (cmdList[0] == "GM_VALUE")
    {
        uchar value = cmdList[1].toInt();
        m_doc->inputOutputMap()->setGrandMasterValue(value);
        return;
    }
    else if (cmdList[0] == "POLL")
        return;

    if (data.contains("|") == false)
        return;

    if (m_auth && user && user->level < VC_ONLY_LEVEL)
        return;

    /** Handle direct widget operations;
     *  Commands start with the widget ID,
     *  followed by specific parameters */

    quint32 widgetID = cmdList[0].toUInt();
    VCWidget *widget = m_vc->widget(widgetID);
    uchar value = 0;
    if (cmdList.count() > 1)
        value = (uchar)cmdList[1].toInt();

    if (widget != NULL)
    {
        switch(widget->type())
        {
            case VCWidget::ButtonWidget:
            {
                VCButton *button = qobject_cast<VCButton*>(widget);
              #ifndef QMLUI
                if (value)
                    button->pressFunction();
                else
                    button->releaseFunction();
              #else
                if ((button->actionType() == VCButton::ButtonAction::Toggle) || (button->actionType() == VCButton::ButtonAction::Blackout))
                {
                    if (value == 0)
                        button->requestStateChange((button->state() == VCButton::ButtonState::Active) ? false : true);
                }
                else if (button->actionType() == VCButton::ButtonAction::Flash)
                {
                    button->requestStateChange((value == 255));
                }
                else if (button->actionType() == VCButton::ButtonAction::StopAll)
                {
                    button->requestStateChange(true);
                }
              #endif
            }
            break;
            case VCWidget::SliderWidget:
            {
                VCSlider *slider = qobject_cast<VCSlider*>(widget);
              #ifndef QMLUI
                slider->setSliderValue(value, false, true);
              #else
                slider->setValue(value, false, true);
              #endif
                slider->updateFeedback();
            }
            break;
            case VCWidget::AudioTriggersWidget:
            {
                VCAudioTriggers *triggers = qobject_cast<VCAudioTriggers*>(widget);
              #ifndef QMLUI
                triggers->toggleEnableButton(value ? true : false);
              #else
                triggers->setCaptureEnabled((value == 255) ? true : false);
              #endif
            }
            break;
            case VCWidget::CueListWidget:
            {
                if (cmdList.count() < 2)
                    return;

                VCCueList *cue = qobject_cast<VCCueList*>(widget);
                if (cmdList[1] == "PLAY")
                  #ifndef QMLUI
                    cue->slotPlayback();
                  #else
                    cue->playClicked();
                  #endif
                else if (cmdList[1] == "STOP")
                  #ifndef QMLUI
                    cue->slotStop();
                  #else
                    cue->stopClicked();
                  #endif
                else if (cmdList[1] == "PREV")
                  #ifndef QMLUI
                    cue->slotPreviousCue();
                  #else
                    cue->previousClicked();
                  #endif
                else if (cmdList[1] == "NEXT")
                  #ifndef QMLUI
                    cue->slotNextCue();
                  #else
                    cue->nextClicked();
                  #endif
                else if (cmdList[1] == "STEP")
                  #ifndef QMLUI
                    cue->slotCurrentStepChanged(cmdList[2].toInt());
                  #else
                    cue->setPlaybackIndex(cmdList[2].toInt());
                  #endif
                else if (cmdList[1] == "CUE_STEP_NOTE")
                  #ifndef QMLUI
                    cue->slotStepNoteChanged(cmdList[2].toInt(), cmdList[3]);
                  #else
                    cue->setStepNote(cmdList[2].toInt(), cmdList[3]);
                  #endif
              #ifndef QMLUI
                else if (cmdList[1] == "CUE_SHOWPANEL")
                    cue->slotSideFaderButtonChecked(cmdList[2] == "1" ? false : true);
              #endif
                else if (cmdList[1] == "CUE_SIDECHANGE")
                  #ifndef QMLUI
                    cue->slotSetSideFaderValue((cmdList[2]).toInt());
                  #else
                    cue->setSideFaderLevel((cmdList[2]).toInt());
                  #endif
            }
            break;
            case VCWidget::FrameWidget:
            case VCWidget::SoloFrameWidget:
            {
                VCFrame *frame = qobject_cast<VCFrame*>(widget);
                if (cmdList[1] == "NEXT_PG")
                  #ifndef QMLUI
                    frame->slotNextPage();
                  #else
                    frame->gotoNextPage();
                  #endif
                else if (cmdList[1] == "PREV_PG")
                  #ifndef QMLUI
                    frame->slotPreviousPage();
                  #else
                    frame->gotoPreviousPage();
                  #endif
                else if (cmdList[1] == "FRAME_DISABLE")
                  #ifndef QMLUI
                    frame->setDisableState(cmdList[2] == "1" ? false : true);
                  #else
                    frame->setDisabled(cmdList[2] == "1" ? false : true);
                  #endif
            }
            break;
            case VCWidget::ClockWidget:
            {
                VCClock *clock = qobject_cast<VCClock*>(widget);
                if (cmdList[1] == "S")
                  #ifndef QMLUI
                    clock->playPauseTimer();
                  #else
                    QMetaObject::invokeMethod(clock->m_item, "startStopReset", Q_ARG(QVariant, false));
                  #endif
                else if (cmdList[1] == "R")
                  #ifndef QMLUI
                    clock->resetTimer();
                  #else
                    QMetaObject::invokeMethod(clock->m_item, "startStopReset", Q_ARG(QVariant, true));
                  #endif
            }
            break;
            case VCWidget::AnimationWidget:
            {
              #ifndef QMLUI
                VCMatrix *matrix = qobject_cast<VCMatrix*>(widget);
              #else
                VCAnimation *matrix = qobject_cast<VCAnimation*>(widget);
              #endif
                if (cmdList[1] == "MATRIX_SLIDER_CHANGE")
                  #ifndef QMLUI
                    matrix->slotSetSliderValue(cmdList[2].toInt());
                  #else
                    matrix->setFaderLevel(cmdList[2].toInt());
                  #endif
                if (cmdList[1] == "MATRIX_COMBO_CHANGE")
                  #ifndef QMLUI
                    matrix->slotSetAnimationValue(cmdList[2]);
                  #else
                    // FIXME: does not work if the algorithm has been changed via the UI combobox since the VC initialisation
                    matrix->setAlgorithmIndex(matrix->algorithms().indexOf(cmdList[2]));
                  #endif
                if (cmdList[1] == "MATRIX_COLOR_CHANGE" && cmdList[2] == "COLOR_1")
                  #ifndef QMLUI
                    matrix->slotColor1Changed(cmdList[3].toInt());
                  #else
                    matrix->setColor1(QColor(cmdList[3].toInt()));
                  #endif
                if (cmdList[1] == "MATRIX_COLOR_CHANGE" && cmdList[2] == "COLOR_2")
                  #ifndef QMLUI
                    matrix->slotColor2Changed(cmdList[3].toInt());
                  #else
                    matrix->setColor2(QColor(cmdList[3].toInt()));
                  #endif
                if (cmdList[1] == "MATRIX_COLOR_CHANGE" && cmdList[2] == "COLOR_3")
                  #ifndef QMLUI
                    matrix->slotColor3Changed(cmdList[3].toInt());
                  #else
                    matrix->setColor3(QColor(cmdList[3].toInt()));
                  #endif
                if (cmdList[1] == "MATRIX_COLOR_CHANGE" && cmdList[2] == "COLOR_4")
                  #ifndef QMLUI
                    matrix->slotColor4Changed(cmdList[3].toInt());
                  #else
                    matrix->setColor4(QColor(cmdList[3].toInt()));
                  #endif
                if (cmdList[1] == "MATRIX_COLOR_CHANGE" && cmdList[2] == "COLOR_5")
                  #ifndef QMLUI
                    matrix->slotColor5Changed(cmdList[3].toInt());
                  #else
                    matrix->setColor5(QColor(cmdList[3].toInt()));
                  #endif
                if (cmdList[1] == "MATRIX_KNOB")
                  #ifndef QMLUI
                    matrix->slotMatrixControlKnobValueChanged(cmdList[2].toInt(), cmdList[3].toInt());
                  #else
                {
                    // TODO
                }
                  #endif
                if (cmdList[1] == "MATRIX_PUSHBUTTON")
                  #ifndef QMLUI
                    matrix->slotMatrixControlPushButtonClicked(cmdList[2].toInt());
                  #else
                {
                    // TODO
                }
                  #endif
            }
            break;
            default:
            break;
        }
    }
}

void WebAccess::slotHandleWebSocketClose(QHttpConnection *conn)
{
    qDebug() << "Websocket Connection closed";
    if (conn->userData)
    {
        WebAccessUser* user = static_cast<WebAccessUser*>(conn->userData);
        delete user;
        conn->userData = 0;
    }
    conn->deleteLater();

    m_webSocketsList.removeOne(conn);
}

void WebAccess::slotFunctionStarted(quint32 fid)
{
    QString wsMessage = QString("FUNCTION|%1|Running").arg(fid);

    sendWebSocketMessage(wsMessage.toUtf8());
}

void WebAccess::slotFunctionStopped(quint32 fid)
{
    QString wsMessage = QString("FUNCTION|%1|Stopped").arg(fid);

    sendWebSocketMessage(wsMessage.toUtf8());
}

bool WebAccess::sendFile(QHttpResponse *response, QString filename, QString contentType)
{
    QFile resFile(filename);
#if defined(WIN32) || defined(Q_OS_WIN)
    // If coming from a Windows hack, restore a path like
    // /c//tmp/pic.jpg back to C:\tmp\pic.jpg
    if (resFile.exists() == false)
    {
        filename.remove(0, 1);
        filename.replace("//", ":\\");
        filename.replace('/', '\\');
        resFile.setFileName(filename);
    }
#endif
    if (resFile.open(QIODevice::ReadOnly))
    {
        QByteArray resContent = resFile.readAll();
        //qDebug() << "Resource file length:" << resContent.length();
        resFile.close();

        response->setHeader("Content-Type", contentType);
        response->setHeader("Content-Length", QString::number(resContent.size()));
        response->writeHead(200);
        response->end(resContent);

        return true;
    }
    else
        qDebug() << "Failed to open file:" << filename;

    return false;
}

void WebAccess::sendWebSocketMessage(const QString &message)
{
    foreach (QHttpConnection *conn, m_webSocketsList)
        conn->webSocketWrite(message);
}

QString WebAccess::getWidgetBackgroundImage(VCWidget *widget)
{
    if (widget == NULL || widget->backgroundImage().isEmpty())
        return QString();

    QString imgPath = widget->backgroundImage();
#if defined(WIN32) || defined(Q_OS_WIN)
    // Hack for Windows to cheat the browser
    // Turn a path like C:\tmp\pic.jpg to /c//tmp/pic.jpg
    if (imgPath.contains(':'))
    {
        imgPath.prepend('/');
        imgPath.replace(':', '/');
    }
#endif
    QString str = QString("background-image: url(%1); ").arg(imgPath);
    str += "background-position: center; ";
    str += "background-repeat: no-repeat; ";
    str += "background-size: cover; "; // or contain

    return str;
}

QString WebAccess::getWidgetHTML(VCWidget *widget)
{
    QString str = "<div class=\"vcwidget\" style=\""
            "left: " + QString::number(
              #ifndef QMLUI
                widget->x()
              #else
                widget->geometry().x()
              #endif
              ) + "px; "
            "top: " + QString::number(
              #ifndef QMLUI
                widget->y()
              #else
                widget->geometry().y()
              #endif
              ) + "px; "
            "width: " + QString::number(
              #ifndef QMLUI
                widget->width()
              #else
                widget->geometry().width()
              #endif
              ) + "px; "
            "height: " + QString::number(
              #ifndef QMLUI
                widget->height()
              #else
                widget->geometry().height()
              #endif
              ) + "px; " +
            "background-color: " + widget->backgroundColor().name() + ";" +
            getWidgetBackgroundImage(widget) + "\">\n";

    str +=  tr("Widget not supported (yet) for web access") + "</div>\n";

    return str;
}

void WebAccess::slotFramePageChanged(int pageNum)
{
    VCWidget *frame = qobject_cast<VCWidget *>(sender());
    if (frame == NULL)
        return;

    QString wsMessage = QString("%1|FRAME|%2").arg(frame->id()).arg(pageNum);
    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotFrameDisableStateChanged(bool disable)
{
    VCWidget *frame = qobject_cast<VCWidget *>(sender());
    if (frame == NULL)
        return;

    QString wsMessage = QString("%1|FRAME_DISABLE|%2").arg(frame->id()).arg(disable);
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getFrameHTML(VCFrame *frame)
{
    QColor border(90, 90, 90);

  #ifndef QMLUI
    QSize origSize = frame->originalSize();
  #else
    QRectF origSize = frame->geometry();
  #endif
    int w = frame->isCollapsed() ? 200 : origSize.width();
    int h = frame->isCollapsed() ? 36 : origSize.height();

    // page select component width + margin
    int pw =
      #ifndef QMLUI
        frame->multipageMode()
      #else
        frame->multiPageMode()
      #endif
      ? (frame->isCollapsed() ? 64 : 168) : 0;

    // enable button width + margin
    int ew =
      #ifndef QMLUI
        frame->isEnableButtonVisible()
      #else
        frame->showEnable()
      #endif
      ? 36 : 0;

    // collapse button width + margin
    int cw = 36;
    // header width
    int hw = w - pw - ew - cw;

    QString str = "<div class=\"vcframe\" id=\"fr" + QString::number(frame->id()) + "\" "
                  "style=\"left: " + QString::number(
                    #ifndef QMLUI
                      frame->x()
                    #else
                      frame->geometry().x()
                    #endif
                    ) +
                  "px; top: " + QString::number(
                    #ifndef QMLUI
                      frame->y()
                    #else
                      frame->geometry().y()
                    #endif
                    ) + "px; width: " + QString::number(w) +
                  "px; height: " + QString::number(h) + "px; "
                  "background-color: " + frame->backgroundColor().name() + "; " + getWidgetBackgroundImage(frame) +
                  "border: 1px solid " + border.name() + ";\">\n";

    str += getChildrenHTML(frame, frame->totalPagesNumber(), frame->currentPage());

  #ifndef QMLUI
    if (frame->isHeaderVisible())
  #else
    if (frame->showHeader())
  #endif
    {
        // header caption
        QString caption = QString(frame->caption());
        QString currentPageName = "";

      #ifndef QMLUI
        if (frame->multipageMode())
      #else
        if (frame->multiPageMode())
      #endif
        {
            m_JScode += "framesPageNames[" + QString::number(frame->id()) + "] = new Array();\n";

          #ifndef QMLUI
            const QList<VCFramePageShortcut*> shortcuts = frame->shortcuts();
            int index = 0;
            for (const VCFramePageShortcut* shortcut : shortcuts)
            {
                m_JScode += "framesPageNames[" + QString::number(frame->id()) + "][" + QString::number(index) + "] = \"" +
                            QString(shortcut->name()).replace("\\", "\\\\").replace("\"", "\\\"") + "\";\n";
                index++;
            }
            currentPageName = QString(shortcuts[frame->currentPage()]->name());
          #else
            int index = 0;
            for (const QString& pageLabel : frame->pageLabels())
            {
                m_JScode += "framesPageNames[" + QString::number(frame->id()) + "][" + QString::number(index) + "] = \"" +
                            QString(pageLabel).replace("\\", "\\\\").replace("\"", "\\\"") + "\";\n";
                index++;
            }
            currentPageName = QString(frame->pageLabels()[frame->currentPage()]);
          #endif

            if (caption != "")
                caption += " - ";

            if (currentPageName == "")
                currentPageName = tr("Page: %1").arg(frame->currentPage() + 1);

            caption += currentPageName;
        }

        str += "<div style=\"position: absolute; display: flex; align-items: center; justify-content: center; flex-direction: row; width: 100%;\">";
        str += "<a class=\"vcframeButton\" href=\"javascript:frameToggleCollapse(" +
               QString::number(frame->id()) + ");\"><img src=\"expand.png\" width=\"27\"></a>\n";

        str += "<div class=\"vcframeHeader\" id=\"vcframeHeader" + QString::number(frame->id()) + "\" style=\"color:" +
               frame->foregroundColor().name() + "; width: "+ QString::number(hw) +"px \">";
        str += "<div class=\"vcFrameText\" id=\"fr" + QString::number(frame->id()) + "Caption\">" + caption + "</div>\n";
        str += "</div>\n";

        m_JScode += "frameCaption[" + QString::number(frame->id()) + "] = \"" +
                    QString(frame->caption()).replace("\\", "\\\\").replace("\"", "\\\"") + "\";\n";

      #ifndef QMLUI
        if (frame->isEnableButtonVisible())
      #else
        if (frame->showEnable())
      #endif
        {
            str += "<a class=\"vcframeButton\" id=\"frEnBtn" + QString::number(frame->id()) + "\" " +
                   "style=\" background-color: " + QString((frame->isDisabled() ? "#E0DFDF" : "#D7DE75")) + "; \" " +
                   "href=\"javascript:frameDisableStateChange(" + QString::number(frame->id()) + ");\">" +
                   "<img src=\"check.png\" width=\"27\"></a>\n";

            m_JScode += "frameDisableState[" + QString::number(frame->id()) + "] = " + QString::number(frame->isDisabled() ? 1 : 0) + ";\n";
          #ifndef QMLUI
            connect(frame, SIGNAL(disableStateChanged(bool)), this, SLOT(slotFrameDisableStateChanged(bool)));
          #else
            connect(frame, SIGNAL(disabledStateChanged(bool)), this, SLOT(slotFrameDisableStateChanged(bool)));
          #endif
        }

        m_JScode += "framesWidth[" + QString::number(frame->id()) + "] = " + QString::number(origSize.width()) + ";\n";
        m_JScode += "framesHeight[" + QString::number(frame->id()) + "] = " + QString::number(origSize.height()) + ";\n";

      #ifndef QMLUI
        if (frame->multipageMode())
      #else
        if (frame->multiPageMode())
      #endif
        {
            str += "<div id=\"frMpHdr" + QString::number(frame->id()) + "\" style=\"display:flex; align-items:center; justify-content:center; flex-direction:row; margin-right: 2px;\">\n";

            str += "<a class=\"vcframeButton\" id=\"frMpHdrPrev" + QString::number(frame->id()) + "\" href=\"javascript:framePreviousPage(" +
                   QString::number(frame->id()) + ");\" style=\"display: " + QString(!frame->isCollapsed() ? "block" : "none") + "\">" +
                   "<img src=\"back.png\" width=\"27\"></a>";

            str += "<div class=\"vcframePageLabel\" id=\"frPglbl" + QString::number(frame->id()) + "\" style=\"width: " + QString::number(frame->isCollapsed() ? 60 : 100)+"px; \" >" +
                   "<div class=\"vcFrameText\" id=\"fr" + QString::number(frame->id()) + "Page\">" + currentPageName + "</div></div>\n";

            str += "<a class=\"vcframeButton\" id=\"frMpHdrNext" + QString::number(frame->id()) + "\" href=\"javascript:frameNextPage(" +
                   QString::number(frame->id()) + ");\" style=\"display: " + QString(!frame->isCollapsed() ? "block" : "none") + "\">" +
                   "<img src=\"forward.png\" width=\"27\"></a>\n";

            str += "</div>\n";

            m_JScode += "framesCurrentPage[" + QString::number(frame->id()) + "] = " + QString::number(frame->currentPage()) + ";\n";
            m_JScode += "framesTotalPages[" + QString::number(frame->id()) + "] = " + QString::number(frame->totalPagesNumber()) + ";\n";

            connect(frame, SIGNAL(pageChanged(int)), this, SLOT(slotFramePageChanged(int)));
        }
        m_JScode += "\n";
        str += "</div>\n";
    }

    str += "</div>\n";

    return str;
}

QString WebAccess::getSoloFrameHTML(VCSoloFrame *frame)
{
    QColor border(255, 0, 0);

  #ifndef QMLUI
    QSize origSize = frame->originalSize();
  #else
    QRectF origSize = frame->geometry();
  #endif
    int w = frame->isCollapsed() ? 200 : origSize.width();
    int h = frame->isCollapsed() ? 36 : origSize.height();

    // page select component width + margin
    int pw =
      #ifndef QMLUI
        frame->multipageMode()
      #else
        frame->multiPageMode()
      #endif
      ? (frame->isCollapsed() ? 64 : 168) : 0;

    // enable button width + margin
    int ew =
      #ifndef QMLUI
        frame->isEnableButtonVisible()
      #else
        frame->showEnable()
      #endif
      ? 36 : 0;

    // collapse button width + margin
    int cw = 36;
    // header width
    int hw = w - pw - ew - cw;

    QString str = "<div class=\"vcframe\" id=\"fr" + QString::number(frame->id()) + "\" "
                  "style=\"left: " + QString::number(
                    #ifndef QMLUI
                      frame->x()
                    #else
                      frame->geometry().x()
                    #endif
                    ) +
                  "px; top: " + QString::number(
                    #ifndef QMLUI
                      frame->y()
                    #else
                      frame->geometry().y()
                    #endif
                    ) + "px; width: " + QString::number(w) +
                  "px; height: " + QString::number(h) + "px; "
                  "background-color: " + frame->backgroundColor().name() + "; " + getWidgetBackgroundImage(frame) +
                  "border: 1px solid " + border.name() + ";\">\n";

    str += getChildrenHTML(frame, frame->totalPagesNumber(), frame->currentPage());

  #ifndef QMLUI
    if (frame->isHeaderVisible())
  #else
    if (frame->showHeader())
  #endif
    {
        // header caption
        QString caption = QString(frame->caption());
        QString currentPageName = "";

      #ifndef QMLUI
        if (frame->multipageMode())
      #else
        if (frame->multiPageMode())
      #endif
        {
          #ifndef QMLUI
            m_JScode += "framesPageNames[" + QString::number(frame->id()) + "] = new Array();\n";

            const QList<VCFramePageShortcut*> shortcuts = frame->shortcuts();
            int index = 0;
            for (const VCFramePageShortcut* shortcut : shortcuts)
            {
                m_JScode += "framesPageNames[" + QString::number(frame->id()) + "][" + QString::number(index) + "] = \"" +
                            QString(shortcut->name()).replace("\\", "\\\\").replace("\"", "\\\"") + "\";\n";
                index++;
            }
            currentPageName = QString(shortcuts[frame->currentPage()]->name());
          #else
            int index = 0;
            for (const QString& pageLabel : frame->pageLabels())
            {
                m_JScode += "framesPageNames[" + QString::number(frame->id()) + "][" + QString::number(index) + "] = \"" +
                            QString(pageLabel).replace("\\", "\\\\").replace("\"", "\\\"") + "\";\n";
                index++;
            }
            currentPageName = QString(frame->pageLabels()[frame->currentPage()]);
          #endif

            if (caption != "")
                caption += " - ";

            if (currentPageName == "")
                currentPageName = tr("Page: %1").arg(frame->currentPage() + 1);

            caption += currentPageName;
        }

        str += "<div style=\"position: absolute; display: flex; align-items: center; justify-content: center; flex-direction: row; width: 100%;\">";
        str += "<a class=\"vcframeButton\" href=\"javascript:frameToggleCollapse(" +
               QString::number(frame->id()) + ");\"><img src=\"expand.png\" width=\"27\"></a>\n";

        str += "<div class=\"vcsoloframeHeader\" id=\"vcframeHeader" + QString::number(frame->id()) + "\" style=\"color:" +
               frame->foregroundColor().name() + "; width: "+ QString::number(hw) +"px \">";
        str += "<div class=\"vcFrameText\" id=\"fr" + QString::number(frame->id()) + "Caption\">" + caption + "</div>\n";
        str += "</div>\n";

        m_JScode += "frameCaption[" + QString::number(frame->id()) + "] = \"" +
                    QString(frame->caption()).replace("\\", "\\\\").replace("\"", "\\\"") + "\";\n";

      #ifndef QMLUI
        if (frame->isEnableButtonVisible())
      #else
        if (frame->showEnable())
      #endif
        {
            str += "<a class=\"vcframeButton\" id=\"frEnBtn" + QString::number(frame->id()) + "\" " +
                   "style=\" background-color: " + QString((frame->isDisabled() ? "#E0DFDF" : "#D7DE75")) + "; \" " +
                   "href=\"javascript:frameDisableStateChange(" + QString::number(frame->id()) + ");\">" +
                   "<img src=\"check.png\" width=\"27\"></a>\n";

            m_JScode += "frameDisableState[" + QString::number(frame->id()) + "] = " + QString::number(frame->isDisabled() ? 1 : 0) + ";\n";
          #ifndef QMLUI
            connect(frame, SIGNAL(disableStateChanged(bool)), this, SLOT(slotFrameDisableStateChanged(bool)));
          #else
            connect(frame, SIGNAL(disabledStateChanged(bool)), this, SLOT(slotFrameDisableStateChanged(bool)));
          #endif
        }

        m_JScode += "framesWidth[" + QString::number(frame->id()) + "] = " + QString::number(origSize.width()) + ";\n";
        m_JScode += "framesHeight[" + QString::number(frame->id()) + "] = " + QString::number(origSize.height()) + ";\n";

      #ifndef QMLUI
        if (frame->multipageMode())
      #else
        if (frame->multiPageMode())
      #endif
        {
            str += "<div id=\"frMpHdr" + QString::number(frame->id()) + "\" style=\"display:flex; align-items:center; justify-content:center; flex-direction:row; margin-right: 2px;\">\n";

            str += "<a class=\"vcframeButton\" id=\"frMpHdrPrev" + QString::number(frame->id()) + "\" href=\"javascript:framePreviousPage(" +
                   QString::number(frame->id()) + ");\" style=\"display: " + QString(!frame->isCollapsed() ? "block" : "none") + "\">" +
                   "<img src=\"back.png\" width=\"27\"></a>";

            str += "<div class=\"vcframePageLabel\" id=\"frPglbl" + QString::number(frame->id()) + "\" style=\"width: " + QString::number(frame->isCollapsed() ? 60 : 100) + "px; \" >" +
                   "<div class=\"vcFrameText\" id=\"fr" + QString::number(frame->id()) + "Page\">" + currentPageName + "</div></div>\n";

            str += "<a class=\"vcframeButton\" id=\"frMpHdrNext" + QString::number(frame->id()) + "\" href=\"javascript:frameNextPage(" +
                   QString::number(frame->id()) + ");\" style=\"display: " + QString(!frame->isCollapsed() ? "block" : "none") + "\">" +
                   "<img src=\"forward.png\" width=\"27\"></a>\n";


            str += "</div>\n";

            m_JScode += "framesCurrentPage[" + QString::number(frame->id()) + "] = " + QString::number(frame->currentPage()) + ";\n";
            m_JScode += "framesTotalPages[" + QString::number(frame->id()) + "] = " + QString::number(frame->totalPagesNumber()) + ";\n";
            connect(frame, SIGNAL(pageChanged(int)), this, SLOT(slotFramePageChanged(int)));
        }
        m_JScode += "\n";
        str += "</div>\n";
    }

    str += "</div>\n";

    return str;
}

void WebAccess::slotButtonStateChanged(int state)
{
    VCButton *btn = qobject_cast<VCButton *>(sender());
    if (btn == NULL)
        return;

    qDebug() << "Button state changed" << state;

    QString wsMessage = QString::number(btn->id());
    if (state == VCButton::Active)
        wsMessage.append("|BUTTON|255");
    else if (state == VCButton::Monitoring)
        wsMessage.append("|BUTTON|127");
    else
        wsMessage.append("|BUTTON|0");

    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotButtonDisableStateChanged(bool disable)
{
    VCButton *btn = qobject_cast<VCButton *>(sender());
    if (btn == NULL)
        return;

    QString wsMessage = QString("%1|BUTTON_DISABLE|%2").arg(btn->id()).arg(disable);
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getButtonHTML(VCButton *btn)
{
    QString onCSS = "";
    if (btn->state() == VCButton::Active)
        onCSS = "border: 3px solid #00E600;";
    else if (btn->state() == VCButton::Monitoring)
        onCSS = "border: 3px solid #FFAA00;";

    QString str = "<div class=\"vcbutton-wrapper\" style=\""
            "left: " + QString::number(
              #ifndef QMLUI
                btn->x()
              #else
                btn->geometry().x()
              #endif
              ) + "px; "
            "top: " + QString::number(
              #ifndef QMLUI
                btn->y()
              #else
                btn->geometry().y()
              #endif
              ) + "px;\">\n";

    str +=  "<a class=\"vcbutton" + QString(btn->isDisabled() ? " vcbutton-disabled" : "") + "\" "
            " id=\"" + QString::number(btn->id()) + "\" href=\"javascript:void(0);\" ";
    if (!btn->isDisabled()) {
        str += "onmousedown=\"buttonPress(" + QString::number(btn->id()) + ");\" "
               "onmouseup=\"buttonRelease(" + QString::number(btn->id()) + ");\" ";
    }
    str +=  "style=\""
            "width: " + QString::number(
              #ifndef QMLUI
                btn->width()
              #else
                btn->geometry().width()
              #endif
              ) + "px; "
            "height: " + QString::number(
              #ifndef QMLUI
                btn->height()
              #else
                btn->geometry().height()
              #endif
              ) + "px; " +
            "color: " + btn->foregroundColor().name() + "; " +
            getWidgetBackgroundImage(btn) +
            "background-color: " + btn->backgroundColor().name() + "; " + onCSS + "\">" +
            btn->caption()
              #ifdef QMLUI
                .toHtmlEscaped()
              #endif
            + "</a>\n</div>\n";

    connect(btn, SIGNAL(stateChanged(int)),
            this, SLOT(slotButtonStateChanged(int)));
  #ifndef QMLUI
    connect(btn, SIGNAL(disableStateChanged(bool)),
            this, SLOT(slotButtonDisableStateChanged(bool)));
  #else
    connect(btn, SIGNAL(disabledStateChanged(bool)),
            this, SLOT(slotButtonDisableStateChanged(bool)));
  #endif

    return str;
}

#ifndef QMLUI
void WebAccess::slotSliderValueChanged(QString val)
#else
void WebAccess::slotSliderValueChanged(int value)
#endif
{
    VCSlider *slider = qobject_cast<VCSlider *>(sender());
    if (slider == NULL)
        return;

    // <ID>|SLIDER|<SLIDER VALUE>|<DISPLAY VALUE>
    QString wsMessage = QString("%1|SLIDER|%2|%3").arg(slider->id())
      #ifndef QMLUI
        .arg(slider->sliderValue()).arg(val);
      #else
        .arg(value)
        .arg((slider->valueDisplayStyle() == VCSlider::ValueDisplayStyle::DMXValue) ? QString::number(value).rightJustified(3, '0') : QString::number(value * 100 / 255.0, 'f', 0).rightJustified(3, '0').append('%'));
      #endif
    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotSliderDisableStateChanged(bool disable)
{
    VCSlider *slider = qobject_cast<VCSlider *>(sender());
    if (slider == NULL)
        return;

    QString wsMessage = QString("%1|SLIDER_DISABLE|%2").arg(slider->id()).arg(disable);
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getSliderHTML(VCSlider *slider)
{
    QString slID = QString::number(slider->id());

    QString str = "<div class=\"vcslider\" style=\""
            "left: " + QString::number(
              #ifndef QMLUI
                slider->x()
              #else
                slider->geometry().x()
              #endif
              ) + "px; " +
            "top: " + QString::number(
              #ifndef QMLUI
                slider->y()
              #else
                slider->geometry().y()
              #endif
              ) + "px; " +
            "width: " + QString::number(
              #ifndef QMLUI
                slider->width()
              #else
                slider->geometry().width()
              #endif
              ) + "px; "
            "height: " + QString::number(
              #ifndef QMLUI
                slider->height()
              #else
                slider->geometry().height()
              #endif
              ) + "px; " +
            "background-color: " + slider->backgroundColor().name() + ";" +
            getWidgetBackgroundImage(slider) + "\">\n";

    str += "<div style=\"height: 100%; display: flex; flex-direction: column; justify-content: space-between; \">";

    str += "<div id=\"slv" + slID + "\" class=\"vcslLabel" + QString(slider->isDisabled() ? " vcslLabel-disabled" : "") + "\">" +
      #ifndef QMLUI
        slider->topLabelText()
      #else
        ((slider->valueDisplayStyle() == VCSlider::ValueDisplayStyle::DMXValue) ? QString::number(slider->value()) : QString::number((slider->value() * 100.0) / 255.0, 10, 0) + "%")
      #endif
      + "</div>\n";

  #ifndef QMLUI
    int mt = slider->invertedAppearance() ? -slider->height() + 50 : slider->height() - 50;
  #else
    int mt = slider->invertedAppearance() ? -slider->geometry().height() + 50 : slider->geometry().height() - 50;
  #endif
    int rotate = slider->invertedAppearance() ? 90 : 270;
    int min = 0;
    int max = 255;
    if (slider->sliderMode() == VCSlider::Level) {
      #ifndef QMLUI
        min = slider->levelLowLimit();
        max = slider->levelHighLimit();
      #else
        min = slider->rangeLowLimit();
        max = slider->rangeHighLimit();
      #endif
    }

    str +=  "<input type=\"range\" class=\"vVertical" + QString(slider->isDisabled() ? " vVertical-disabled" : "") + "\" "
            "id=\"" + slID + "\" "
            "oninput=\"slVchange(" + slID + ");\" ontouchmove=\"slVchange(" + slID + ");\" "
            "style=\"display: "+(slider->widgetStyle() == VCSlider::SliderWidgetStyle::WSlider ? "block" : "none") +"; "
            "width: " + QString::number(
              #ifndef QMLUI
                slider->height()
              #else
                slider->geometry().height()
              #endif
              - 50) + "px; " +
            "margin-top: " + QString::number(mt) + "px; " +
            "margin-left: " + QString::number(
              #ifndef QMLUI
                slider->width()
              #else
                slider->geometry().width()
              #endif
              / 2) + "px; " +
            "--rotate: "+QString::number(rotate)+"\" "
            "min=\""+QString::number(min)+"\" max=\""+QString::number(max)+"\" "
            "step=\"1\" value=\"" + QString::number(
              #ifndef QMLUI
                slider->sliderValue()
              #else
                slider->value()
              #endif
              ) + "\"";
    if (slider->isDisabled())
        str += " disabled ";
    str += ">\n";

    if (slider->widgetStyle() == VCSlider::SliderWidgetStyle::WKnob) {
      #ifndef QMLUI
        int shortSide = slider->width() > slider->height() ? slider->height() : slider->width();
      #else
        QRectF geometry = slider->geometry();
        int shortSide = geometry.width() > geometry.height() ? geometry.height() : geometry.width();
      #endif
        shortSide = shortSide - 50;
        float arcWidth = shortSide / 15;
        float pieWidth = shortSide - (arcWidth * 2);
        float knobWrapperWidth = pieWidth - arcWidth;
        float knobWidth = knobWrapperWidth - (arcWidth * 3);
        float spotWidth = knobWrapperWidth * 2 / 15;
        if (spotWidth < 6) spotWidth = 6;

        str += "<div class=\"pieWrapper\" data=\"" + slID + "\">";
        str += "<div class=\"pie\" id=\"pie" + slID + "\" style=\"--degValue:0;--color1:" + QString(slider->isDisabled() ? "#c0c0c0" : "lime") + ";--pieWidth: " + QString::number(pieWidth) + "px;\">";
        str += "<div class=\"knobWrapper\" id=\"knobWrapper" + slID + "\" style=\"--knobWrapperWidth: " + QString::number(knobWrapperWidth) + "px;\">";
        str += "<div class=\"knob\" id=\"knob" + slID + "\" style=\"--knobWidth: " + QString::number(knobWidth) + "px;\">";
        str += "<div class=\"spot\" id=\"spot" + slID + "\" style=\"--spotWidth: " + QString::number(spotWidth) + "px;\"></div>";
        str += "</div>\n</div>\n</div>\n</div>\n";

        m_JScode += "maxVal[" + slID + "] = " + QString::number(max) + "; \n";
        m_JScode += "minVal[" + slID + "] = " + QString::number(min) + "; \n";
        m_JScode += "initVal[" + slID + "] = " + QString::number(
          #ifndef QMLUI
            slider->sliderValue()
          #else
            slider->value()
          #endif
          ) + "; \n";
        m_JScode += "inverted[" + slID + "] = " + QString::number(slider->invertedAppearance()) + "; \n";
        m_JScode += "isDragging[" + slID + "] = false;\n";
        m_JScode += "isDisableKnob[" + slID + "] = "+QString::number(slider->isDisabled() ? 1 : 0)+";\n";
    }

    str += "<div id=\"sln" + slID + "\" class=\"vcslLabel" + QString(slider->isDisabled() ? " vcslLabel-disabled" : "") + "\">" + slider->caption()
             #ifdef QMLUI
               .toHtmlEscaped()
             #endif
           + "</div>";

    str += "</div>\n";
    str += "</div>\n";

  #ifndef QMLUI
    connect(slider, SIGNAL(valueChanged(QString)),
            this, SLOT(slotSliderValueChanged(QString)));
    connect(slider, SIGNAL(disableStateChanged(bool)),
            this, SLOT(slotSliderDisableStateChanged(bool)));
  #else
    connect(slider, SIGNAL(valueChanged(int)),
            this, SLOT(slotSliderValueChanged(int)));
    connect(slider, SIGNAL(disabledStateChanged(bool)),
            this, SLOT(slotSliderDisableStateChanged(bool)));
  #endif

    return str;
}

void WebAccess::slotLabelDisableStateChanged(bool disable)
{
    VCLabel *label = qobject_cast<VCLabel *>(sender());
    if (label == NULL)
        return;

    QString wsMessage = QString("%1|LABEL_DISABLE|%2").arg(label->id()).arg(disable);
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getLabelHTML(VCLabel *label)
{
    QString str = "<div class=\"vclabel-wrapper\" style=\""
            "left: " + QString::number(
              #ifndef QMLUI
                label->x()
              #else
                label->geometry().x()
              #endif
              ) + "px; "
            "top: " + QString::number(
              #ifndef QMLUI
                label->y()
              #else
                label->geometry().y()
              #endif
              ) + "px;\">\n";
    str +=  "<div id=\"lbl" + QString::number(label->id()) + "\" "
            "class=\"vclabel" + QString(label->isDisabled() ? " vclabel-disabled" : "") + "\" "
            "style=\"width: " + QString::number(
              #ifndef QMLUI
                label->width()
              #else
                label->geometry().width()
              #endif
              ) + "px; ";
    if (m_doc->mode() != Doc::Design)
        str += "border: none!important; ";
    str +=  "height: " + QString::number(
              #ifndef QMLUI
                label->height()
              #else
                label->geometry().height()
              #endif
              ) + "px; " +
            "color: " + label->foregroundColor().name() + "; "
            "background-color: " + label->backgroundColor().name() + "; " +
            getWidgetBackgroundImage(label) + "\">" +
            label->caption()
              #ifdef QMLUI
                .toHtmlEscaped()
              #endif
            + "</div>\n</div>\n";

  #ifndef QMLUI
    connect(label, SIGNAL(disableStateChanged(bool)),
            this, SLOT(slotLabelDisableStateChanged(bool)));
  #else
    connect(label, SIGNAL(disabledStateChanged(bool)),
            this, SLOT(slotLabelDisableStateChanged(bool)));
  #endif

    return str;
}

#ifndef QMLUI
void WebAccess::slotAudioTriggersToggled(bool toggle)
#else
void WebAccess::slotAudioTriggersToggled()
#endif
{
    VCAudioTriggers *triggers = qobject_cast<VCAudioTriggers*>(sender());
    if (triggers == NULL)
        return;

  #ifndef QMLUI
    qDebug() << "AudioTriggers state changed " << toggle;

    QString wsMessage = QString("%1|AUDIOTRIGGERS|%2").arg(triggers->id()).arg(toggle ? 255 : 0);
  #else
    qDebug() << "AudioTriggers state changed " << triggers->captureEnabled();

    QString wsMessage = QString("%1|AUDIOTRIGGERS|%2").arg(triggers->id()).arg(triggers->captureEnabled() ? 255 : 0);
  #endif
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getAudioTriggersHTML(VCAudioTriggers *triggers)
{
    const int width =
                      #ifndef QMLUI
                        triggers->width();
                      #else
                        triggers->geometry().width();
                      #endif
    const int height =
                       #ifndef QMLUI
                         triggers->height();
                       #else
                         triggers->geometry().height();
                       #endif

    QString str = "<div class=\"vcaudiotriggers\" style=\"left: " + QString::number(
                    #ifndef QMLUI
                      triggers->x()
                    #else
                      triggers->geometry().x()
                    #endif
                  ) + "px; top: " + QString::number(
                    #ifndef QMLUI
                      triggers->y()
                    #else
                      triggers->geometry().y()
                    #endif
                  ) + "px; width: " + QString::number(width) + "px; height: " + QString::number(height) + "px;"
                  "background-color: " + triggers->backgroundColor().name() + ";\">\n";

    str += "<div class=\"vcaudioHeader\" style=\"color:" +
            triggers->foregroundColor().name() + "\">" + triggers->caption()
              #ifdef QMLUI
                .toHtmlEscaped()
              #endif
            + "</div>\n";

    str += "<div class=\"vcatbutton-wrapper\">\n";
    str += "<a  class=\"vcatbutton\" id=\"" + QString::number(triggers->id()) + "\" "
            "href=\"javascript:atButtonClick(" + QString::number(triggers->id()) + ");\" "
            "style=\""
            "width: " + QString::number(width - 2) + "px; "
            "height: " + QString::number(height - 42) + "px;\">"
            + tr("Enable") + "</a>\n";

    str += "</div></div>\n";

  #ifndef QMLUI
    connect(triggers, SIGNAL(captureEnabled(bool)),
            this, SLOT(slotAudioTriggersToggled(bool)));
  #else
    connect(triggers, SIGNAL(captureEnabledChanged()),
            this, SLOT(slotAudioTriggersToggled()));
  #endif

    return str;
}

void WebAccess::slotCueIndexChanged(int idx)
{
    VCCueList *cue = qobject_cast<VCCueList *>(sender());
    if (cue == NULL)
        return;

    QString wsMessage = QString("%1|CUE|%2").arg(cue->id()).arg(idx);
    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotCueStepNoteChanged(int idx, QString note)
{
    VCCueList *cue = qobject_cast<VCCueList *>(sender());
    if (cue == NULL)
        return;

    QString wsMessage = QString("%1|CUE_STEP_NOTE|%2|%3").arg(cue->id()).arg(idx).arg(note);
    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotCueProgressStateChanged()
{
    VCCueList *cue = qobject_cast<VCCueList *>(sender());
    if (cue == NULL)
        return;

    QString wsMessage = QString("%1|CUE_PROGRESS|%2|%3").arg(cue->id()).arg(
      #ifndef QMLUI
        QString::number(cue->progressPercent())
      #else
        QString::number(cue->m_item->property("progressValue").toDouble() * 100)
      #endif
      ).arg(
      #ifndef QMLUI
        cue->progressText()
      #else
        cue->m_item->property("progressText").toString().toHtmlEscaped()
      #endif
      );

    sendWebSocketMessage(wsMessage);
}

#ifndef QMLUI
void WebAccess::slotCueShowSideFaderPanel()
{
    VCCueList *cue = qobject_cast<VCCueList *>(sender());
    if (cue == NULL)
        return;

    QString wsMessage = QString("%1|CUE_SHOWPANEL|%2").arg(cue->id()).arg(cue->sideFaderButtonIsChecked());
    sendWebSocketMessage(wsMessage);
}
#endif

void WebAccess::slotCueSideFaderValueChanged()
{
    VCCueList *cue = qobject_cast<VCCueList *>(sender());
    if (cue == NULL)
        return;

    QString wsMessage = QString("%1|CUE_SIDECHANGE|%2|%3|%4|%5|%6|%7|%8")
                            .arg(cue->id())
                          #ifndef QMLUI
                            .arg(cue->topPercentageValue())
                            .arg(cue->bottomPercentageValue())
                            .arg(cue->topStepValue())
                            .arg(cue->bottomStepValue())
                            .arg(cue->primaryTop())
                            .arg(cue->sideFaderValue())
                          #else
                            .arg(QString::number(cue->sideFaderLevel()) + "%")
                            .arg(QString::number(100 - cue->sideFaderLevel()) + "%")
                            .arg((cue->playbackStatus() != VCCueList::PlaybackStatus::Stopped) ? ("#" + QString::number(cue->primaryTop() ? cue->playbackIndex()+1 : cue->nextStepIndex()+1)) : "")
                            .arg((cue->playbackStatus() != VCCueList::PlaybackStatus::Stopped) ? ("#" + QString::number(cue->primaryTop() ? cue->nextStepIndex()+1 : cue->playbackIndex()+1)) : "")
                            .arg(cue->primaryTop())
                            .arg(cue->sideFaderLevel())
                          #endif
                            .arg(cue->sideFaderMode() == VCCueList::FaderMode::Steps);

    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotCuePlaybackStateChanged()
{
    VCCueList *cue = qobject_cast<VCCueList *>(sender());
    if (cue == NULL)
        return;
    Chaser *chaser = cue->chaser();
    QString playbackButtonImage = "player_play.png";
    bool playbackButtonPaused = false;
    QString stopButtonImage = "player_stop.png";
    bool stopButtonPaused = false;

    if (chaser->isRunning()) {
        if (cue->playbackLayout() == VCCueList::PlayPauseStop) {
            if (chaser->isPaused()) {
                playbackButtonImage = "player_play.png";
                playbackButtonPaused = true;
            } else {
                playbackButtonImage  = "player_pause.png";
            }
        } else if (cue->playbackLayout() == VCCueList::PlayStopPause) {
            playbackButtonImage = "player_stop.png";
            stopButtonImage = "player_pause.png";
            if (chaser->isPaused()) {
                stopButtonPaused = true;
            }
        }
    } else {
        if (cue->playbackLayout() == VCCueList::PlayStopPause) {
            stopButtonImage = "player_pause.png";
        }
    }

    QString wsMessage = QString("%1|CUE_CHANGE|%2|%3|%4|%5")
                            .arg(cue->id())
                            .arg(playbackButtonImage)
                            .arg(QString::number(playbackButtonPaused))
                            .arg(stopButtonImage)
                            .arg(QString::number(stopButtonPaused));

    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotCueDisableStateChanged(bool disable)
{
    VCCueList *cue = qobject_cast<VCCueList *>(sender());
    if (cue == NULL)
        return;

    QString wsMessage = QString("%1|CUE_DISABLE|%2").arg(cue->id()).arg(disable);
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getCueListHTML(VCCueList *cue)
{
    const qreal height =
                       #ifndef QMLUI
                         cue->height();
                       #else
                         cue->geometry().height();
                       #endif

    QString str = "<div id=\"" + QString::number(cue->id()) + "\" "
            "class=\"vccuelist\" style=\""
            "left: " + QString::number(
              #ifndef QMLUI
                cue->x()
              #else
                cue->geometry().x()
              #endif
              ) + "px; "
            "top: " + QString::number(
              #ifndef QMLUI
                cue->y()
              #else
                cue->geometry().y()
              #endif
              ) + "px; " +
            "width: " + QString::number(
              #ifndef QMLUI
                cue->width()
              #else
                cue->geometry().width()
              #endif
              ) + "px; "
            "height: " + QString::number(height) + "px; " +
            "background-color: " + cue->backgroundColor().name() + ";\">\n";

    QString topStepBgColor = "inherit";
    QString bottomStepBgColor = "inherit";
    QString playbackButtonImage = "player_play.png";
    bool playbackButtonPaused = false;
    QString stopButtonImage = "player_stop.png";
    bool stopButtonPaused = false;

    const QString topStepValue =
                               #ifndef QMLUI
                                 cue->topStepValue();
                               #else
                                 (cue->playbackStatus() != VCCueList::PlaybackStatus::Stopped) ? QString::number(cue->primaryTop() ? cue->playbackIndex()+1 : cue->nextStepIndex()+1) : "";
                               #endif
    const QString topPercentageValue =
                                     #ifndef QMLUI
                                       cue->topPercentageValue();
                                     #else
                                       QString::number(cue->sideFaderLevel()) + "%";
                                     #endif
    const QString bottomStepValue =
                                  #ifndef QMLUI
                                    cue->bottomStepValue();
                                  #else
                                    (cue->playbackStatus() != VCCueList::PlaybackStatus::Stopped) ? QString::number(cue->primaryTop() ? cue->nextStepIndex()+1 : cue->playbackIndex()+1) : "";
                                  #endif
    const QString bottomPercentageValue =
                                        #ifndef QMLUI
                                          cue->bottomPercentageValue();
                                        #else
                                          QString::number(100 - cue->sideFaderLevel()) + "%";
                                        #endif
    const QString sideFaderValue =
                                 #ifndef QMLUI
                                   QString::number(cue->sideFaderValue());
                                 #else
                                   QString::number(cue->sideFaderLevel());
                                 #endif

    if (cue->primaryTop())
    {
        topStepBgColor = topStepValue != "" ? "#4E8DDE" : "inherit";
        bottomStepBgColor = cue->sideFaderMode() == VCCueList::FaderMode::Steps && bottomStepValue != "" ? "#4E8DDE" : bottomStepValue != "" ? "orange" : "inherit";
    }
    else
    {
        topStepBgColor = topStepValue != "" ? "orange" : "inherit";
        bottomStepBgColor = cue->sideFaderMode() == VCCueList::FaderMode::Steps || bottomStepValue != "" ? "#4E8DDE" : "inherit";
    }

    // fader mode
    if (cue->sideFaderMode() != VCCueList::FaderMode::None)
    {
        str += "<div style=\"display: flex; flex-direction: row; align-items: center; justify-content: space-between; \">";
        str += "<div id=\"fadePanel"+QString::number(cue->id())+"\" "
               "style=\"display: " + (
                 #ifndef QMLUI
                   cue->isSideFaderVisible()
                 #else
                   (cue->sideFaderMode() != VCCueList::FaderMode::None)
                 #endif
               ? "block" : "none") + "; width: 45px; height: " +
               QString::number(height - 2) + "px;\">";
        if (cue->sideFaderMode() == VCCueList::FaderMode::Crossfade)
        {
            str += "<div style=\"position: relative;\">";
            str += "<div id=\"cueCTP"+QString::number(cue->id())+"\" class=\"vcslLabel" + QString(cue->isDisabled() ? " vcslLabel-disabled" : "") + "\" style=\"top:0px;\">" +
                   topPercentageValue + "</div>\n";
            str += "<div id=\"cueCTS"+QString::number(cue->id())+"\" class=\"vcslLabel\" "
                   "style=\"top:25px; border: solid 1px #aaa; background-color: "+ topStepBgColor +" \">" +
                   topStepValue + "</div>\n";

            str += "<input type=\"range\" class=\"vVertical" + QString(cue->isDisabled() ? " vVertical-disabled" : "") + "\" id=\"cueC"+QString::number(cue->id())+"\" "
                   "oninput=\"cueCVchange("+QString::number(cue->id())+");\" ontouchmove=\"cueCVchange("+QString::number(cue->id())+");\" "
                   "style=\"width: " + QString::number(height - 100) + "px; margin-top: " +
                   QString::number(height - 100) + "px; margin-left: 22px;\" ";
            str += "min=\"0\" max=\"100\" step=\"1\" value=\"" + sideFaderValue + "\" " + QString(cue->isDisabled() ? "disabled" : "") + " >\n";

            str += "<div id=\"cueCBS"+QString::number(cue->id())+"\" class=\"vcslLabel\" "
                   "style=\"bottom:25px; border: solid 1px #aaa;  background-color: "+ bottomStepBgColor +"\">" +
                   bottomStepValue + "</div>\n";
            str += "<div id=\"cueCBP"+QString::number(cue->id())+"\" class=\"vcslLabel" + QString(cue->isDisabled() ? " vcslLabel-disabled" : "") + "\" style=\"bottom:0px;\">" +
                   bottomPercentageValue + "</div>\n";
            str += "</div>";
        }
        if (cue->sideFaderMode() == VCCueList::FaderMode::Steps)
        {
            str += "<div style=\"position: relative;\">";
            str += "<div id=\"cueCTP"+QString::number(cue->id())+"\" class=\"vcslLabel" + QString(cue->isDisabled() ? " vcslLabel-disabled" : "") + "\" style=\"top:0px;\">" +
                   topPercentageValue + "</div>\n";

            str += "<input type=\"range\" class=\"vVertical" + QString(cue->isDisabled() ? " vVertical-disabled" : "") + "\" id=\"cueC" + QString::number(cue->id()) + "\" "
                   "oninput=\"cueCVchange(" + QString::number(cue->id()) + ");\" ontouchmove=\"cueCVchange(" + QString::number(cue->id())+");\" "
                   "style=\"width: " + QString::number(height - 50) + "px; margin-top: " +
                   QString::number(height - 50) + "px; margin-left: 22px;\" ";
            str += "min=\"0\" max=\"255\" step=\"1\" value=\"" + sideFaderValue + "\" " + QString(cue->isDisabled() ? "disabled" : "") + " >\n";

            str += "<div id=\"cueCBS"+QString::number(cue->id())+"\" class=\"vcslLabel\" style=\"bottom:25px; border: solid 1px #aaa; \">" +
                   bottomStepValue + "</div>\n";
            str += "</div>";
        }
        str += "</div>";
      #ifndef QMLUI
        m_JScode += "showPanel[" + QString::number(cue->id()) + "] = " + QString::number(cue->sideFaderButtonIsChecked()) + ";\n";
      #endif
    }

    str += "<div style=\"width: 100%;\"><div style=\"width: 100%; height: " + QString::number(height - 54) + "px; overflow: scroll;\" >\n";

    str += "<table class=\"hovertable" + QString(cue->isDisabled() ? " cell-disabled" : "") + "\" id=\"cueTable" + QString::number(cue->id()) + "\" style=\"width: 100%;\">\n";
    str += "<tr><th>#</th><th>" + tr("Name") + "</th>";
    str += "<th>" + tr("Fade In") + "</th>";
    str += "<th>" + tr("Fade Out") + "</th>";
    str += "<th>" + tr("Duration") + "</th>";
    str += "<th>" + tr("Notes") + "</th></tr>\n";

    Chaser *chaser = cue->chaser();
  #ifndef QMLUI
    Doc *doc = m_vc->getDoc();
  #else
    Doc *doc = m_doc;
  #endif

    if (chaser != NULL)
    {
        for (int i = 0; i < chaser->stepsCount(); i++)
        {
            QString stepID = QString::number(cue->id()) + "_" + QString::number(i);
            str += "<tr id=\"" + stepID + "\" "
                   "onclick=\"enableCue(" + QString::number(cue->id()) + ", " + QString::number(i) + ");\">\n";

            ChaserStep *step = chaser->stepAt(i);
            str += "<td>" + QString::number(i + 1) + "</td>";
            Function* function = doc->function(step->fid);
            if (function != NULL)
            {
              #ifndef QMLUI
                str += "<td>" + function->name() + "</td>";
              #else
                str += "<td>" + function->name().toHtmlEscaped() + "</td>";
              #endif

                switch (chaser->fadeInMode())
                {
                    case Chaser::Common:
                    {
                        if (chaser->fadeInSpeed() == Function::infiniteSpeed())
                            str += "<td>&#8734;</td>";
                        else
                            str += "<td>" + Function::speedToString(chaser->fadeInSpeed()) + "</td>";
                    }
                    break;
                    case Chaser::PerStep:
                    {
                        if (step->fadeIn == Function::infiniteSpeed())
                            str += "<td>&#8734;</td>";
                        else
                            str += "<td>" + Function::speedToString(step->fadeIn) + "</td>";
                    }
                    break;
                    default:
                    case Chaser::Default:
                        str += "<td></td>";
                }

                //if (step.hold != 0)
                //    str +=  "<td>" + Function::speedToString(step.hold) + "</td>";
                //else str += "<td></td>";

                switch (chaser->fadeOutMode())
                {
                    case Chaser::Common:
                    {
                        if (chaser->fadeOutSpeed() == Function::infiniteSpeed())
                            str += "<td>&#8734;</td>";
                        else
                            str += "<td>" + Function::speedToString(chaser->fadeOutSpeed()) + "</td>";
                    }
                    break;
                    case Chaser::PerStep:
                    {
                        if (step->fadeOut == Function::infiniteSpeed())
                            str += "<td>&#8734;</td>";
                        else
                            str += "<td>" + Function::speedToString(step->fadeOut) + "</td>";
                    }
                    break;
                    default:
                    case Chaser::Default:
                        str += "<td></td>";
                }

                switch (chaser->durationMode())
                {
                    case Chaser::Common:
                    {
                        if (chaser->duration() == Function::infiniteSpeed())
                            str += "<td>&#8734;</td>";
                        else
                            str += "<td>" + Function::speedToString(chaser->duration()) + "</td>";
                    }
                    break;
                    case Chaser::PerStep:
                    {
                        if (step->fadeOut == Function::infiniteSpeed())
                            str += "<td>&#8734;</td>";
                        else
                            str += "<td>" + Function::speedToString(step->duration) + "</td>";
                    }
                    break;
                    default:
                    case Chaser::Default:
                        str += "<td></td>";
                }

                str += "<td ondblclick=\"changeCueNoteToEditMode(" + QString::number(cue->id()) + ", " + QString::number(i) + ");\">" +
                       "<span id=\"cueNoteSpan" + stepID + "\" style=\"display: block;\">" + step->note
                         #ifdef QMLUI
                           .toHtmlEscaped()
                         #endif
                       + "</span>" +
                       "<input type=\"text\" id=\"cueNoteInput" + stepID + "\" value=\"" + step->note
                         #ifdef QMLUI
                           .toHtmlEscaped()
                         #endif
                       + "\" style=\"display: none; width: 60px;\" " +
                       "onfocusout=\"changeCueNoteToTextMode(" + QString::number(cue->id()) + ", " + QString::number(i) + ");\" />"
                       "</td>\n";
            }
            str += "</td>\n";
        }
    }
    str += "</table>\n";
    str += "</div>\n";

    // progress bar
    str += "<div class=\"vccuelistProgress\">";
    str += "<div class=\"vccuelistProgressBar\" id=\"vccuelistPB" + QString::number(cue->id()) + "\" style=\"width: " +
             #ifndef QMLUI
               QString::number(cue->progressPercent())
             #else
               QString::number(cue->m_item->property("progressValue").toDouble() * 100)
             #endif
           + "%; \"></div>";
    str += "<div class=\"vccuelistProgressVal\" id=\"vccuelistPV" + QString::number(cue->id())+"\">" +
             #ifndef QMLUI
               cue->progressText()
             #else
               cue->m_item->property("progressText").toString().toHtmlEscaped()
             #endif
           + "</div>";
    str += "</div>";

    // play, stop, next, and preview buttons
    if (cue->sideFaderMode() != VCCueList::FaderMode::None)
    {
        str += "<div style=\"width: 100%; display: flex; flex-direction: row; align-items: center; justify-content: space-between; \">\n";
      #ifndef QMLUI
        str += "<a class=\"vccuelistFadeButton"+QString(cue->isDisabled() ? " vccuelistFadeButton-disabled" : "") + "\" id=\"fade" + QString::number(cue->id()) + "\" ";
        str += "href=\"javascript:wsShowCrossfadePanel(" + QString::number(cue->id()) + ");\">\n";
        str += "<img src=\"slider.png\" width=\"27\"></a>\n";
      #endif
    }
    str += "<div style=\"width: 100%; display: flex; flex-direction: row; align-items: center; justify-content: space-between; \">";

    if (chaser != NULL && chaser->isRunning())
    {
        if (cue->playbackLayout() == VCCueList::PlayPauseStop)
        {
            if (chaser->isPaused())
            {
                playbackButtonImage = "player_play.png";
                playbackButtonPaused = true;
            }
            else
            {
                playbackButtonImage  = "player_pause.png";
            }
        }
        else if (cue->playbackLayout() == VCCueList::PlayStopPause)
        {
            playbackButtonImage = "player_stop.png";
            stopButtonImage = "player_pause.png";
            if (chaser->isPaused())
                stopButtonPaused = true;
        }
    }
    else
    {
        if (cue->playbackLayout() == VCCueList::PlayStopPause)
            stopButtonImage = "player_pause.png";
    }

    str += "<a class=\"vccuelistButton" + QString(cue->isDisabled() ? " vccuelistButton-disabled" : "") + QString(playbackButtonPaused ? " vccuelistButtonPaused" : "")+"\" id=\"play" + QString::number(cue->id()) + "\" ";
    str += "href=\"javascript:sendCueCmd(" + QString::number(cue->id()) + ", 'PLAY');\">\n";
    str += "<img src=\""+playbackButtonImage+"\" width=\"27\"></a>\n";

    str += "<a class=\"vccuelistButton" + QString(cue->isDisabled() ? " vccuelistButton-disabled" : "") + QString(stopButtonPaused ? " vccuelistButtonPaused" : "")+"\" id=\"stop" + QString::number(cue->id()) + "\" ";
    str += "href=\"javascript:sendCueCmd(" + QString::number(cue->id()) + ", 'STOP');\">\n";
    str += "<img src=\""+stopButtonImage+"\" width=\"27\"></a>\n";

    str += "<a class=\"vccuelistButton" + QString(cue->isDisabled() ? " vccuelistButton-disabled" : "") + "\" id=\"prev" + QString::number(cue->id()) + "\" href=\"javascript:sendCueCmd(";
    str += QString::number(cue->id()) + ", 'PREV');\">\n";
    str += "<img src=\"back.png\" width=\"27\"></a>\n";

    str += "<a class=\"vccuelistButton" + QString(cue->isDisabled() ? " vccuelistButton-disabled" : "") + "\" id=\"next" + QString::number(cue->id()) + "\" href=\"javascript:sendCueCmd(";
    str += QString::number(cue->id()) + ", 'NEXT');\" style=\"margin-right: 0px!important;\">\n";
    str += "<img src=\"forward.png\" width=\"27\"></a>\n";

    if (cue->sideFaderMode() != VCCueList::FaderMode::None) {
        str += "</div>\n";
    }
    str +="</div></div>";
    if (cue->sideFaderMode() != VCCueList::FaderMode::None) {
        str += "</div>\n";
    }

    str += "</div>\n";

    m_JScode += "isDisableCue[" + QString::number(cue->id()) + "] = " + QString::number(cue->isDisabled()) + ";\n";

  #ifndef QMLUI
    connect(cue, SIGNAL(stepChanged(int)),
            this, SLOT(slotCueIndexChanged(int)));
    connect(cue, SIGNAL(progressStateChanged()),
            this, SLOT(slotCueProgressStateChanged()));
    connect(cue, SIGNAL(sideFaderButtonChecked()),
            this, SLOT(slotSideFaderButtonChecked(bool)));
    connect(cue, SIGNAL(sideFaderButtonToggled()),
            this, SLOT(slotCueShowSideFaderPanel()));
    connect(cue, SIGNAL(sideFaderValueChanged()),
            this, SLOT(slotCueSideFaderValueChanged()));
    connect(cue, SIGNAL(playbackButtonClicked()),
            this, SLOT(slotCuePlaybackStateChanged()));
    connect(cue, SIGNAL(stopButtonClicked()),
            this, SLOT(slotCuePlaybackStateChanged()));
    connect(cue, SIGNAL(disableStateChanged(bool)),
            this, SLOT(slotCueDisableStateChanged(bool)));
  #else
    connect(cue, SIGNAL(playbackIndexChanged(int)),
            this, SLOT(slotCueIndexChanged(int)));
    connect(cue, SIGNAL(sideFaderLevelChanged()),
            this, SLOT(slotCueSideFaderValueChanged()));
    connect(cue, SIGNAL(disabledStateChanged(bool)),
            this, SLOT(slotCueDisableStateChanged(bool)));
  #endif
    connect(cue, SIGNAL(stepNoteChanged(int, QString)),
            this, SLOT(slotCueStepNoteChanged(int, QString)));
    connect(cue, SIGNAL(progressStateChanged()),
            this, SLOT(slotCueProgressStateChanged()));
    connect(cue, SIGNAL(playbackStatusChanged()),
            this, SLOT(slotCuePlaybackStateChanged()));

    return str;
}

void WebAccess::slotClockTimeChanged(quint32 time)
{
    VCClock *clock = qobject_cast<VCClock *>(sender());
    if (clock == NULL)
        return;

  #ifndef QMLUI
    QString wsMessage = QString("%1|CLOCK|%2").arg(clock->id()).arg(time);
  #else
    QString wsMessage = QString("%1|CLOCK_MS|%2").arg(clock->id()).arg(time);
  #endif
    sendWebSocketMessage(wsMessage);
}

void WebAccess::slotClockDisableStateChanged(bool disable)
{
    VCClock *clock = qobject_cast<VCClock *>(sender());
    if (clock == NULL)
        return;

    QString wsMessage = QString("%1|CLOCK_DISABLE|%2").arg(clock->id()).arg(disable);
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getClockHTML(VCClock *clock)
{
    QString str = "<div class=\"vclabel-wrapper\" style=\""
            "left: " + QString::number(
              #ifndef QMLUI
                clock->x()
              #else
                clock->geometry().x()
              #endif
              ) + "px; "
            "top: " + QString::number(
              #ifndef QMLUI
                clock->y()
              #else
                clock->geometry().y()
              #endif
              ) + "px;\">\n";

    str +=  "<a id=\"" + QString::number(clock->id()) + "\" class=\"vclabel" + QString(clock->isDisabled() ? " vclabel-disabled" : "") + "";

    if (clock->clockType() == VCClock::Stopwatch ||
        clock->clockType() == VCClock::Countdown)
    {
        str += " vcclockcount\" href=\"javascript:controlWatch(";
        str += QString::number(clock->id()) + ", 'S')\" ";
        str += "oncontextmenu=\"javascript:controlWatch(";
        str += QString::number(clock->id()) + ", 'R'); return false;\"";
        connect(clock, SIGNAL(timeChanged(quint32)),
                this, SLOT(slotClockTimeChanged(quint32)));
    }
    else
    {
        str += " vcclock\"";
    }

    str +=  "style=\"width: " + QString::number(
              #ifndef QMLUI
                clock->width()
              #else
                clock->geometry().width()
              #endif
              ) + "px; ";

    if (m_doc->mode() != Doc::Design)
        str += "border: none!important; ";

    str +=  "height: " + QString::number(
              #ifndef QMLUI
                clock->height()
              #else
                clock->geometry().height()
              #endif
              ) + "px; " +
            "color: " + clock->foregroundColor().name() + "; "
            "background-color: " + clock->backgroundColor().name() + ";" +
            getWidgetBackgroundImage(clock) + "\">";


    if (clock->clockType() == VCClock::Stopwatch)
    {
      #ifndef QMLUI
        str += "00:00:00";
      #else
        str += "00:00:00.0";
      #endif
    }
    else if (clock->clockType() == VCClock::Countdown)
    {
      #ifndef QMLUI
        QString curTime = QString("%1:%2:%3").arg(clock->getHours(), 2, 10, QChar('0'))
                                             .arg(clock->getMinutes(), 2, 10, QChar('0'))
                                             .arg(clock->getSeconds(), 2, 10, QChar('0'));
      #else
        int milliseconds = clock->targetTime();
        if (milliseconds < 0)
          milliseconds = 0;

        int seconds = milliseconds / 1000;
        milliseconds %= 1000;

        int minutes = seconds / 60;
        seconds %= 60;

        int hours = minutes / 60;
        minutes %= 60;

        QString curTime = QString("%1:%2:%3.%4").arg(hours, 2, 10, QChar('0'))
                                                .arg(minutes, 2, 10, QChar('0'))
                                                .arg(seconds, 2, 10, QChar('0'))
                                                .arg(milliseconds, 1, 10, QChar('0'));
      #endif
        str += curTime;
    }


    str += "</a></div>\n";

  #ifndef QMLUI
    connect(clock, SIGNAL(disableStateChanged(bool)),
            this, SLOT(slotClockDisableStateChanged(bool)));
  #else
    connect(clock, SIGNAL(disabledStateChanged(bool)),
            this, SLOT(slotClockDisableStateChanged(bool)));
  #endif

    return str;
}

#ifndef QMLUI
void WebAccess::slotMatrixSliderValueChanged(int value)
{
    VCMatrix *matrix = qobject_cast<VCMatrix *>(sender());
    if (matrix == NULL)
        return;

    QString wsMessage = QString("%1|MATRIX_SLIDER|%2").arg(matrix->id()).arg(value);
    sendWebSocketMessage(wsMessage);
}
#else
void WebAccess::slotAnimationFaderLevelChanged()
{
    VCAnimation *animation = qobject_cast<VCAnimation*>(sender());
    if (animation == NULL)
        return;

    QString wsMessage = QString("%1|MATRIX_SLIDER|%2").arg(animation->id()).arg(animation->faderLevel());
    sendWebSocketMessage(wsMessage);
}
#endif

#ifndef QMLUI
void WebAccess::slotMatrixColorChanged(int index)
#else
void WebAccess::slotAnimationColorChanged(int index)
#endif
{
  #ifndef QMLUI
    VCMatrix *matrix = qobject_cast<VCMatrix *>(sender());
  #else
    VCAnimation *matrix = qobject_cast<VCAnimation*>(sender());
  #endif
    if ((matrix == NULL) || (index < 1) || (index > 5))
        return;

  #ifdef QMLUI
    const QString color;
    switch (index) {
        case 1:
          color = matrix->getColor1();
          break;
        case 2:
          color = matrix->getColor1();
          break;
        case 3:
          color = matrix->getColor1();
          break;
        case 4:
          color = matrix->getColor1();
          break;
        case 5:
          color = matrix->getColor1();
          break;
        default:
          color = QStringLiteral("#000000");
    }
  #endif

    QString wsMessage = QString("%1|MATRIX_COLOR_%2|%3").arg(matrix->id()).arg(index).arg(
      #ifndef QMLUI
        matrix->mtxColor(index-1).name()
      #else
        color
      #endif
      );
    sendWebSocketMessage(wsMessage.toUtf8());
}

#ifndef QMLUI
void WebAccess::slotMatrixAnimationValueChanged(QString name)
{
    VCMatrix *matrix = qobject_cast<VCMatrix *>(sender());
    if (matrix == NULL)
        return;

    QString wsMessage = QString("%1|MATRIX_COMBO|%2").arg(matrix->id()).arg(name);
    sendWebSocketMessage(wsMessage);
}
#else
void WebAccess::slotAnimationAlgorithmChanged()
{
    VCAnimation *animation = qobject_cast<VCAnimation*>(sender());
    if (animation == NULL)
        return;

    QString wsMessage = QString("%1|MATRIX_COMBO|%2").arg(animation->id()).arg(animation->algorithms()[animation->algorithmIndex()]);
    sendWebSocketMessage(wsMessage);
}
#endif

void WebAccess::slotMatrixControlKnobValueChanged(int controlID, int value)
{
  #ifndef QMLUI
    VCMatrix *matrix = qobject_cast<VCMatrix *>(sender());
  #else
    VCAnimation *matrix = qobject_cast<VCAnimation*>(sender());
  #endif
    if (matrix == NULL)
        return;

    QString wsMessage = QString("%1|MATRIX_KNOB|%2|%3").arg(matrix->id()).arg(controlID).arg(value);
    sendWebSocketMessage(wsMessage);
}

#ifndef QMLUI
QString WebAccess::getMatrixHTML(VCMatrix *matrix)
#else
QString WebAccess::getAnimationHTML(VCAnimation *matrix)
#endif
{
    const qreal height =
                       #ifndef QMLUI
                         matrix->height();
                       #else
                         matrix->geometry().height();
                       #endif

    QString str = "<div id=\"" + QString::number(matrix->id()) + "\" "
                  "class=\"vcmatrix\" style=\"left: " +
                    #ifndef QMLUI
                      QString::number(matrix->x())
                    #else
                      QString::number(matrix->geometry().x())
                    #endif
                  + "px; top: " +
                    #ifndef QMLUI
                      QString::number(matrix->y())
                    #else
                      QString::number(matrix->geometry().y())
                    #endif
                  + "px; width: " +
                    #ifndef QMLUI
                      QString::number(matrix->width())
                    #else
                      QString::number(matrix->geometry().width())
                    #endif
                  + "px; height: " + QString::number(height) + "px; "
                  "background-color: " + matrix->backgroundColor().name() + ";\">\n";

    str += "<div style=\"display: flex; flex-direction: row; align-items: center; width: 100%; height: 100%; \">";

  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowSlider) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::Fader) {
  #endif
        str +=  "<div style=\"height: 100%; width: 50px; \">";
        str +=  "<input type=\"range\" class=\"vVertical\" "
                "id=\"msl" + QString::number(matrix->id()) + "\" "
                "oninput=\"matrixSliderValueChange(" + QString::number(matrix->id()) + ");\" "
                "ontouchmove=\"matrixSliderValueChange(" + QString::number(matrix->id()) + ");\" "
                "style=\"width: " + QString::number(height - 20) + "px; "
                "margin-top: " + QString::number(height - 10) + "px; margin-left: 25px; \""
                "min=\"1\" max=\"255\" step=\"1\" value=\"" +
                  #ifndef QMLUI
                    QString::number(matrix->sliderValue())
                  #else
                    QString::number(matrix->faderLevel())
                  #endif
                + "\">\n";
        str +=  "</div>";
    }
    str +=  "<div style=\"display: flex; flex-direction: column; align-items: center; justify-content: space-around; height: 100%; width: 100%; margin: 8px; \">";

  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowLabel) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::Label) {
  #endif
        str += "<div style=\"text-align: center; width: 100%; margin-top: 4px; margin-bottom: 4px; \">" + matrix->caption()
                 #ifdef QMLUI
                   .toHtmlEscaped()
                 #endif
               + "</div>";
    }
    str += "<div style=\"display: flex; flex-direction: row; align-items: center; justify-content: space-around; width: 100%; margin-top: 4px; margin-bottom: 4px; \">";
  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowColor1Button) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::Color1) {
  #endif
        str += "<input type=\"color\" id=\"mc1i" + QString::number(matrix->id()) + "\" class=\"vMatrix\" value=\"" +
                 #ifndef QMLUI
                   matrix->mtxColor(0).name()
                 #else
                   matrix->getColor1().name()
                 #endif
               + "\" oninput=\"matrixColorChanged(1, " + QString::number(matrix->id()) + ");\" ontouchmove=\"matrixColorChanged(1, " + QString::number(matrix->id()) + ");\" "
               " />";
    }
  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowColor2Button) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::Color2) {
  #endif
        str += "<input type=\"color\" id=\"mc2i" + QString::number(matrix->id()) + "\" class=\"vMatrix\" value=\"" +
                 #ifndef QMLUI
                   matrix->mtxColor(1).name()
                 #else
                   matrix->getColor2().name()
                 #endif
               + "\" oninput=\"matrixColorChanged(2, " + QString::number(matrix->id()) + ");\" ontouchmove=\"matrixColorChanged(2, " + QString::number(matrix->id()) + ");\" "
               " />";
    }
  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowColor3Button) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::Color3) {
  #endif
        str += "<input type=\"color\" id=\"mc3i" + QString::number(matrix->id()) + "\" class=\"vMatrix\" value=\"" +
                 #ifndef QMLUI
                   matrix->mtxColor(2).name()
                 #else
                   matrix->getColor3().name()
                 #endif
               + "\" oninput=\"matrixColorChanged(3, " + QString::number(matrix->id()) + ");\" ontouchmove=\"matrixColorChanged(3, " + QString::number(matrix->id()) + ");\" "
               " />";
    }
  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowColor4Button) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::Color4) {
  #endif
        str += "<input type=\"color\" id=\"mc4i" + QString::number(matrix->id()) + "\" class=\"vMatrix\" value=\"" +
                 #ifndef QMLUI
                   matrix->mtxColor(3).name()
                 #else
                   matrix->getColor4().name()
                 #endif
               + "\" oninput=\"matrixColorChanged(4, " + QString::number(matrix->id()) + ");\" ontouchmove=\"matrixColorChange(4, " + QString::number(matrix->id()) + ");\" "
               " />";
    }
  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowColor5Button) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::Color5) {
  #endif
        str += "<input type=\"color\" id=\"mc5i" + QString::number(matrix->id()) + "\" class=\"vMatrix\" value=\"" +
                 #ifndef QMLUI
                   matrix->mtxColor(4).name()
                 #else
                   matrix->getColor5().name()
                 #endif
               + "\" oninput=\"matrixColorChanged(5, " + QString::number(matrix->id()) + ");\" ontouchmove=\"matrixColorChanged(5, " + QString::number(matrix->id()) + ");\" "
               " />";
    }
    str += "</div>";
  #ifndef QMLUI
    if (matrix->visibilityMask() & VCMatrix::Visibility::ShowPresetCombo) {
  #else
    if (matrix->visibilityMask() & VCAnimation::Visibility::PresetCombo) {
  #endif
        QStringList list = RGBAlgorithm::algorithms(m_doc);

        str += "<div style=\"width: 100%; margin-top: 4px; margin-bottom: 4px; \"><select class=\"matrixSelect\" id=\"mcb" + QString::number(matrix->id()) + "\" onchange=\"matrixComboChanged("+QString::number(matrix->id())+");\">";
        for (int i = 0; i < list.length(); i++) {
            str += "<option value=\""+list[i]+"\" "+(list[i] ==
                 #ifndef QMLUI
                   matrix->animationValue()
                 #else
                   matrix->algorithms()[matrix->algorithmIndex()].toHtmlEscaped()
                 #endif
                   ? "selected" : "")+" >"+list[i]+"</option>";
        }
        str += "</select></div>";
    }
  #ifndef QMLUI  // TODO
    QList<VCMatrixControl *> customControls = matrix->customControls();
    if (customControls.length() > 0) {
        m_JScode += "matrixID = "+QString::number(matrix->id())+"; \n";
        str += "<div style=\"display: flex; flex-direction: row; flex-wrap: wrap; align-content: flex-start; width: 100%; height: 100%; margin-top: 4px; margin-bottom: 4px; \">";
        for (int i = 0; i < customControls.length(); i++) {
            VCMatrixControl *control = customControls[i];
            if (control->m_type == VCMatrixControl::Color1) {
                str += "<div class=\"pushButton\" style=\"width: 32px; height: 32px; "
                       "background-color: "+(control->m_color.name())+"; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">1</div>";
            } else if (control->m_type == VCMatrixControl::Color2) {
                str += "<div class=\"pushButton\" style=\"width: 32px; height: 32px; "
                       "background-color: "+(control->m_color.name())+"; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">2</div>";
            } else if (control->m_type == VCMatrixControl::Color2Reset) {
                QString btnLabel = tr("Color 2 Reset");
                str += "<div class=\"pushButton\" style=\"width: 66px; justify-content: flex-start!important; height: 32px; "
                       "background-color: #bbbbbb; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">"+btnLabel+"</div>";
            } else if (control->m_type == VCMatrixControl::Color3) {
                str += "<div class=\"pushButton\" style=\"width: 32px; height: 32px; "
                       "background-color: "+(control->m_color.name())+"; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">3</div>";
            } else if (control->m_type == VCMatrixControl::Color3Reset) {
                QString btnLabel = tr("Color 3 Reset");
                str += "<div class=\"pushButton\" style=\"width: 66px; justify-content: flex-start!important; height: 32px; "
                       "background-color: #bbbbbb; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">"+btnLabel+"</div>";
            } else if (control->m_type == VCMatrixControl::Color4) {
                str += "<div class=\"pushButton\" style=\"width: 32px; height: 32px; "
                       "background-color: "+(control->m_color.name())+"; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">4</div>";
            } else if (control->m_type == VCMatrixControl::Color4Reset) {
                QString btnLabel = tr("Color 4 Reset");
                str += "<div class=\"pushButton\" style=\"width: 66px; justify-content: flex-start!important; height: 32px; "
                       "background-color: #bbbbbb; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">"+btnLabel+"</div>";
            } else if (control->m_type == VCMatrixControl::Color5) {
                str += "<div class=\"pushButton\" style=\"width: 32px; height: 32px; "
                       "background-color: "+(control->m_color.name())+"; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">5</div>";
            } else if (control->m_type == VCMatrixControl::Color5Reset) {
                QString btnLabel = tr("Color 5 Reset");
                str += "<div class=\"pushButton\" style=\"width: 66px; justify-content: flex-start!important; height: 32px; "
                       "background-color: #bbbbbb; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">"+btnLabel+"</div>";
            } else if (control->m_type == VCMatrixControl::Animation || control->m_type == VCMatrixControl::Text) {
                QString btnLabel = control->m_resource;
                if (!control->m_properties.isEmpty())
                {
                        btnLabel += " (";
                        QMapIterator<QString, QString> it(control->m_properties);
                        while (it.hasNext())
                        {
                            it.next();
                          #ifndef QMLUI
                            btnLabel += it.value();
                          #else
                            btnLabel += it.value().toHtmlEscaped();
                          #endif
                            if (it.hasNext())
                                btnLabel += ",";
                        }
                        btnLabel += ")";
                }
                str += "<div class=\"pushButton\" style=\"max-width: 66px; justify-content: flex-start!important; height: 32px; "
                       "background-color: #BBBBBB; margin-right: 4px; margin-bottom: 4px; \" "
                       "onclick=\"wcMatrixPushButtonClicked("+(QString::number(control->m_id))+")\">"+btnLabel+"</div>";
            } else if (control->m_type == VCMatrixControl::Color1Knob
                    || control->m_type == VCMatrixControl::Color2Knob
                    || control->m_type == VCMatrixControl::Color3Knob
                    || control->m_type == VCMatrixControl::Color4Knob
                    || control->m_type == VCMatrixControl::Color5Knob) {
                KnobWidget *knob = qobject_cast<KnobWidget*>(matrix->getWidget(control));
                QString slID = QString::number(control->m_id);
                QColor color = control->m_type == VCMatrixControl::Color1Knob ? control->m_color : control->m_color.darker(250);

                str += "<div class=\"mpieWrapper\" data=\"" + slID + "\" style=\"margin-right: 4px; margin-bottom: 4px; \">";
                str += "<div class=\"mpie\" id=\"mpie" + slID + "\" style=\"--degValue:0; \">";
                str += "<div class=\"mknobWrapper\" id=\"mknobWrapper" + slID + "\">";
                str += "<div class=\"mknob\" id=\"mknob" + slID + "\" style=\"background-color: "+(color.name())+";\">";
                str += "<div class=\"mspot\" id=\"mspot" + slID + "\"></div>";
                str += "</div>\n</div>\n</div>\n</div>\n";

                m_JScode += "m_initVal[" + slID + "] = "+QString::number(knob->value())+"; \n";
                m_JScode += "m_isDragging[" + slID + "] = false;\n";
                connect(matrix, SIGNAL(matrixControlKnobValueChanged(int, int)),
                        this, SLOT(slotMatrixControlKnobValueChanged(int, int)));

            }
        }
        str += "</div>";
    }
  #endif
    str += "</div>";

    str += "</div>";
    str += "</div>\n";

  #ifndef QMLUI
    connect(matrix, SIGNAL(sliderValueChanged(int)),
            this, SLOT(slotMatrixSliderValueChanged(int)));
    connect(matrix, SIGNAL(mtxColorChanged(int)),
            this, SLOT(slotMatrixColorChanged(int)));
    connect(matrix, SIGNAL(animationValueChanged(QString)),
            this, SLOT(slotMatrixAnimationValueChanged(QString)));
  #else
    connect(matrix, SIGNAL(faderLevelChanged()),
            this, SLOT(slotAnimationFaderLevelChanged()));
    connect(matrix, SIGNAL(color1Changed()),
            this, SLOT(slotAnimationColor1Changed()));
    connect(matrix, SIGNAL(color2Changed()),
            this, SLOT(slotAnimationColor2Changed()));
    connect(matrix, SIGNAL(color3Changed()),
            this, SLOT(slotAnimationColor3Changed()));
    connect(matrix, SIGNAL(color4Changed()),
            this, SLOT(slotAnimationColor4Changed()));
    connect(matrix, SIGNAL(color5Changed()),
            this, SLOT(slotAnimationColor5Changed()));
    connect(matrix, SIGNAL(algorithmIndexChanged()),
            this, SLOT(slotAnimationAlgorithmChanged()));
  #endif

    return str;
}

QString WebAccess::getChildrenHTML(VCWidget *frame, int pagesNum, int currentPageIdx)
{
    if (frame == NULL)
        return QString();

    QString unifiedHTML;
    QStringList pagesHTML;
    VCFrame *lframe = qobject_cast<VCFrame *>(frame);
    if (lframe == NULL)
        return "";

  #ifndef QMLUI
    if (lframe->multipageMode())
  #else
    if (lframe->multiPageMode())
  #endif
    {
        for (int i = 0; i < pagesNum; i++)
        {
            QString fpID = QString("fp%1_%2").arg(frame->id()).arg(i);
            QString pg = "<div class=\"vcframePage\" id=\"" + fpID + "\"";
            if (i == currentPageIdx)
                pg += " style=\"visibility: inherit;\"";
            pg += ">\n";
            pagesHTML << pg;
        }
    }

    QList<VCWidget *> chList = frame->findChildren<VCWidget*>();

    qDebug () << "getChildrenHTML: found " << chList.count() << " children";

    foreach (VCWidget *widget, chList)
    {
      #ifndef QMLUI
        if (widget->parentWidget() != frame)
      #else
        if (widget->parent() != frame)
      #endif
            continue;

        QString str;
        bool restoreDisable = false;


      #ifndef QMLUI
        if (pagesNum > 0 && !widget->isEnabled())
      #else
        if (pagesNum > 0 && widget->isDisabled())
      #endif
        {
          #ifndef QMLUI
            widget->setEnabled(true);
          #else
            widget->setDisabled(false);
          #endif
            restoreDisable = true;
        }

        switch (widget->type())
        {
            case VCWidget::FrameWidget:
                str = getFrameHTML(qobject_cast<VCFrame *>(widget));
            break;
            case VCWidget::SoloFrameWidget:
                str = getSoloFrameHTML(qobject_cast<VCSoloFrame *>(widget));
            break;
            case VCWidget::ButtonWidget:
                str = getButtonHTML(qobject_cast<VCButton *>(widget));
            break;
            case VCWidget::SliderWidget:
                str = getSliderHTML(qobject_cast<VCSlider *>(widget));
            break;
            case VCWidget::LabelWidget:
                str = getLabelHTML(qobject_cast<VCLabel *>(widget));
            break;
            case VCWidget::AudioTriggersWidget:
                str = getAudioTriggersHTML(qobject_cast<VCAudioTriggers *>(widget));
            break;
            case VCWidget::CueListWidget:
                str = getCueListHTML(qobject_cast<VCCueList *>(widget));
            break;
            case VCWidget::ClockWidget:
                str = getClockHTML(qobject_cast<VCClock *>(widget));
            break;
            case VCWidget::AnimationWidget:
              #ifndef QMLUI
                str = getMatrixHTML(qobject_cast<VCMatrix *>(widget));
              #else
                str = getAnimationHTML(qobject_cast<VCAnimation *>(widget));
              #endif
            break;
            default:
                str = getWidgetHTML(widget);
            break;
        }
      #ifndef QMLUI
        if (lframe->multipageMode() && pagesNum > 0)
      #else
        if (lframe->multiPageMode() && pagesNum > 0)
      #endif
        {
            if (widget->page() < pagesHTML.count())
            {
                pagesHTML[widget->page()] += str;
                if (restoreDisable)
                  #ifndef QMLUI
                    widget->setEnabled(false);
                  #else
                    widget->setDisabled(true);
                  #endif
            }
        }
        else
            unifiedHTML += str;
    }

    if (pagesNum > 0)
    {
        for (int i = 0; i < pagesHTML.count(); i++)
        {
            unifiedHTML += pagesHTML.at(i);
            unifiedHTML += "</div>\n";
        }
    }
    return unifiedHTML;
}

#ifndef QMLUI
void WebAccess::slotGrandMasterValueChanged(uchar value)
{
    GrandMaster::ValueMode gmValueMode = m_vc->properties().grandMasterValueMode();

    QString gmDisplayValue;
    if (gmValueMode == GrandMaster::Limit)
    {
        gmDisplayValue = QString("%1").arg(value, 3, 10, QChar('0'));
    }
    else
    {
        int p = qFloor(((double(value) / double(UCHAR_MAX)) * double(100)) + 0.5);
        gmDisplayValue = QString("%1%").arg(p, 2, 10, QChar('0'));
    }
    QString wsMessage = QString("GM_VALUE|%1|%2").arg(value).arg(gmDisplayValue);
    sendWebSocketMessage(wsMessage);
}

QString WebAccess::getGrandMasterSliderHTML()
{
    if (!m_vc->properties().grandMasterVisible())
        return "";

    InputOutputMap* inputOutputMap = m_doc->inputOutputMap();
    GrandMaster::ValueMode gmValueMode = m_vc->properties().grandMasterValueMode();
    GrandMaster::SliderMode gmSliderMode = m_vc->properties().grandMasterSliderMode();
    uchar gmValue = inputOutputMap->grandMasterValue();

    QString gmDisplayValue;
    if (gmValueMode == GrandMaster::Limit)
    {
        gmDisplayValue = QString("%1").arg(gmValue, 3, 10, QChar('0'));
    }
    else
    {
        int p = qFloor(((double(gmValue) / double(UCHAR_MAX)) * double(100)) + 0.5);
        gmDisplayValue = QString("%1%").arg(p, 2, 10, QChar('0'));
    }

    QString str = "<div id=\"vcGM\">";
    str += "<div class=\"vcslLabel\" id=\"vcGMSliderLabel\">"+gmDisplayValue+"</div>\n";

    int rotate = gmSliderMode == GrandMaster::SliderMode::Inverted ? 90 : 270;
    QString mt = gmSliderMode == GrandMaster::SliderMode::Inverted ? "calc(-100vh + 120px)" : "calc(100vh - 120px)";
    int min = 0;
    int max = 255;

    str +=  "<input type=\"range\" class=\"vVertical\" id=\"vcGMSlider\" "
                "oninput=\"grandMasterValueChange();\" ontouchmove=\"grandMasterValueChange();\" "
                "style=\"width: calc(100vh - 120px); margin-top: " + mt + ";"
                "margin-left: 20px; "
                "--rotate: "+QString::number(rotate)+"\" "
                "min=\""+QString::number(min)+"\" max=\""+QString::number(max)+"\" "
                "step=\"1\" value=\"" + QString::number(gmValue) + "\">\n";
    str += "<div class=\"vcslLabel\">GM</div>";
    str += "</div>\n";

    connect(m_doc->inputOutputMap(), SIGNAL(grandMasterValueChanged(uchar)),
            this, SLOT(slotGrandMasterValueChanged(uchar)));

    return str;
}
#endif

QString WebAccess::getVCHTML()
{
    m_CSScode = "<link href=\"common.css\" rel=\"stylesheet\" type=\"text/css\" media=\"screen\">\n";
    m_CSScode += "<link href=\"virtualconsole.css\" rel=\"stylesheet\" type=\"text/css\" media=\"screen\">\n";
    m_JScode = "<script type=\"text/javascript\" src=\"virtualconsole.js\"></script>\n"
               "<script type=\"text/javascript\" src=\"websocket.js\"></script>\n"
               "<script type=\"text/javascript\">\n";

  #ifndef QMLUI
    VCFrame *mainFrame = m_vc->contents();
    QSize mfSize = mainFrame->size();
  #else
    VCPage *currentPage = m_vc->page(m_vc->selectedPage());
    QRectF cpSize = currentPage->geometry();
  #endif
    QString widgetsHTML =
            "<form action=\"/loadProject\" method=\"POST\" enctype=\"multipart/form-data\">\n"
				"<input id=\"loadTrigger\" type=\"file\" "
				"onchange=\"document.getElementById('submitTrigger').click();\" name=\"qlcprj\" />\n"
				"<input id=\"submitTrigger\" type=\"submit\"/>\n"
            "</form>\n"

            "<div class=\"controlBar\">\n"
            "<a class=\"button button-blue\" href=\"javascript:document.getElementById('loadTrigger').click();\">\n"
            "<span>" + tr("Load project") + "</span></a>\n"

            "<a class=\"button button-blue\" href=\"/simpleDesk\"><span>" + tr("Simple Desk") + "</span></a>\n"

            "<a class=\"button button-blue\" href=\"/config\"><span>" + tr("Configuration") + "</span></a>\n"

            "<div class=\"swInfo\">" + QString(APPNAME) + " " + QString(APPVERSION) + "</div>"
            "</div>\n";

    widgetsHTML += "<div id=\"vc\">\n";
  #ifndef QMLUI
    widgetsHTML += getGrandMasterSliderHTML();
  #endif
    widgetsHTML += "<div id=\"vcScrollContainer\">\n";
    widgetsHTML += "<div style=\"position: relative; "
            "width: " +
              #ifndef QMLUI
                QString::number(mfSize.width())
              #else
                QString::number(cpSize.width())
              #endif
            + "px; height: " +
              #ifndef QMLUI
                QString::number(mfSize.height())
              #else
                QString::number(cpSize.height())
              #endif
            + "px; background-color: " +
              #ifndef QMLUI
                mainFrame->backgroundColor().name()
              #else
                currentPage->backgroundColor().name()
              #endif
            + ";\">\n";

  #ifndef QMLUI
    widgetsHTML += getChildrenHTML(mainFrame, 0, 0);
  #else
    widgetsHTML += getChildrenHTML(currentPage, 0, 0);
  #endif

    widgetsHTML += "</div>\n";
    widgetsHTML += "</div>\n";
    m_JScode += "\n</script>\n";

    QString str = HTML_HEADER + m_CSScode + "</head>\n<body>\n" + widgetsHTML + "</div>\n" + m_JScode + "\n</body></html>";
    return str;
}

QString WebAccess::getSimpleDeskHTML()
{
    QString str = HTML_HEADER;
    return str;
}

void WebAccess::slotVCLoaded()
{
    m_pendingProjectLoaded = true;
}
