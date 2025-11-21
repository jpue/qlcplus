/*
  Q Light Controller Plus
  webaccess.h

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

#ifndef WEBACCESS_H
#define WEBACCESS_H

#include <QObject>

#include "virtualconsole.h"

#if defined(Q_WS_X11) || defined(Q_OS_LINUX)
class WebAccessNetwork;
#endif

class WebAccessAuth;

#ifndef QMLUI
  class VCMatrix;
#else
  class VCAnimation;
#endif
class VCAudioTriggers;
class VCSoloFrame;
class SimpleDesk;
class VCCueList;
class VCButton;
class VCWidget;
class VCSlider;
class VCLabel;
class VCFrame;
class VCClock;
class Doc;

class QHttpServer;
class QHttpRequest;
class QHttpResponse;
class QHttpConnection;

class WebAccess : public QObject
{
    Q_OBJECT
public:
    explicit WebAccess(Doc *doc, VirtualConsole *vcInstance, SimpleDesk *sdInstance,
                       int portNumber, bool enableAuth, QString passwdFile = QString(),
                       QObject *parent = 0);
    /** Destructor */
    ~WebAccess();

private:
    bool sendFile(QHttpResponse *response, QString filename, QString contentType);
    void sendWebSocketMessage(const QString &message);

    QString getWidgetBackgroundImage(VCWidget *widget);
    QString getWidgetHTML(VCWidget *widget);
    QString getFrameHTML(VCFrame *frame);
    QString getSoloFrameHTML(VCSoloFrame *frame);
    QString getButtonHTML(VCButton *btn);
    QString getSliderHTML(VCSlider *slider);
    QString getLabelHTML(VCLabel *label);
    QString getAudioTriggersHTML(VCAudioTriggers *triggers);
#ifndef QMLUI
    QString getMatrixHTML(VCMatrix *matrix);
#else
    QString getAnimationHTML(VCAnimation *matrix);
#endif
    QString getCueListHTML(VCCueList *cue);
    QString getClockHTML(VCClock *clock);
#ifndef QMLUI
    QString getGrandMasterSliderHTML();
#endif

    QString getChildrenHTML(VCWidget *frame, int pagesNum, int currentPageIdx);
    QString getVCHTML();

    QString getSimpleDeskHTML();

protected slots:
    void slotHandleHTTPRequest(QHttpRequest *req, QHttpResponse *resp);
    void slotHandleWebSocketRequest(QHttpConnection *conn, QString data);
    void slotHandleWebSocketClose(QHttpConnection *conn);

    void slotFunctionStarted(quint32 fid);
    void slotFunctionStopped(quint32 fid);

  #ifndef QMLUI
    void slotVCLoaded();
  #else
    void slotVCLoadStatusChanged(VirtualConsole::LoadStatus);
  #endif
    void slotButtonStateChanged(int state);
    void slotButtonDisableStateChanged(bool disable);
    void slotLabelDisableStateChanged(bool disable);
  #ifndef QMLUI
    void slotSliderValueChanged(QString val);
  #else
    void slotSliderValueChanged(int value);
  #endif
    void slotSliderDisableStateChanged(bool disable);
  #ifndef QMLUI
    void slotAudioTriggersToggled(bool toggle);
  #else
    void slotAudioTriggersToggled();
  #endif
    void slotCueIndexChanged(int idx);
    void slotCueStepNoteChanged(int idx, QString note);
    void slotCueProgressStateChanged();
  #ifndef QMLUI
    void slotCueShowSideFaderPanel();
  #endif
    void slotCueSideFaderValueChanged();
    void slotCuePlaybackStateChanged();
    void slotCueDisableStateChanged(bool disable);
    void slotClockTimeChanged(quint32 time);
    void slotClockDisableStateChanged(bool disable);
    void slotFramePageChanged(int pageNum);
    void slotFrameDisableStateChanged(bool disable);
  #ifndef QMLUI
    void slotMatrixSliderValueChanged(int value);
    void slotMatrixColorChanged(int);
    void slotMatrixAnimationValueChanged(QString name);
  #else
    void slotAnimationFaderLevelChanged();
    void slotAnimationColorChanged(int);
    void slotAnimationColor1Changed();
    void slotAnimationColor2Changed();
    void slotAnimationColor3Changed();
    void slotAnimationColor4Changed();
    void slotAnimationColor5Changed();
    void slotAnimationAlgorithmChanged();
  #endif
    void slotMatrixControlKnobValueChanged(int controlID, int value);

  #ifndef QMLUI
    void slotGrandMasterValueChanged(uchar value);
  #endif

protected:
    QString m_JScode;
    QString m_CSScode;

protected:
    Doc *m_doc;
    VirtualConsole *m_vc;
    SimpleDesk *m_sd;
    WebAccessAuth *m_auth;
#if defined(Q_WS_X11) || defined(Q_OS_LINUX)
    WebAccessNetwork *m_netConfig;
#endif

    QHttpServer *m_httpServer;
    QList<QHttpConnection *> m_webSocketsList;

    bool m_pendingProjectLoaded;

signals:
  #ifndef QMLUI
    void toggleDocMode();
    void loadProject(QString xmlData);
    void storeAutostartProject(QString filename);
  #else
    void loadProject(QByteArray&);
    // TODO
    //void storeAutostartProject(QString filename);
  #endif

public slots:

};

#endif // WEBACCESS_H
