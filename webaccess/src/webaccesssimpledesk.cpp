/*
  Q Light Controller Plus
  webaccesssimpledesk.cpp

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

#include <QSettings>
#include <QDebug>

#include "webaccesssimpledesk.h"
#include "commonjscss.h"
#include "simpledesk.h"
#include "qlcconfig.h"
#include "doc.h"

WebAccessSimpleDesk::WebAccessSimpleDesk(QObject *parent) :
    QObject(parent)
{
}

QString WebAccessSimpleDesk::getHTML(Doc *doc, SimpleDesk *sd)
{
    if ((doc == NULL) || (sd == NULL))
        return QString("");

    int uni = sd->getCurrentUniverseIndex() + 1;
    int page = sd->getCurrentPage();

    QString JScode = "<script src=\"simpledesk.js\"></script>\n";
    JScode += "<script>\n";
    JScode += "var currentUniverse = " + QString::number(uni) + ";\n";
    JScode += "var currentPage = " + QString::number(page) + ";\n";
    JScode += "var channelsPerPage = " + QString::number(sd->getSlidersNumber()) + ";\n";
    JScode += "</script>\n";

    QString CSScode = "<link rel=\"stylesheet\" type=\"text/css\" media=\"screen\" href=\"common.css\">\n";
    CSScode += "<link rel=\"stylesheet\" type=\"text/css\" media=\"screen\" href=\"simpledesk.css\">\n";

    QString bodyHTML = "<div class=\"controlBar\">\n"
                       "<a class=\"button button-blue\" href=\"/\"><span>" + tr("Back") + "</span></a>\n"
                       "<a class=\"button button-blue\" href=\"/keypad\"><span>DMX Keypad</span></a>\n"
                       "<div class=\"swInfo\">" + QString(APPNAME) + " " + QString(APPVERSION) + "</div>"
                       "</div>\n";

    bodyHTML += "<div style=\"margin: 20px; font: bold 27px/1.2em 'Trebuchet MS',Arial, Helvetica; color: #fff;\">\n";
    bodyHTML += tr("Page") + "  <a class=\"sdButton\" href=\"javascript:previousPage();\">\n"
                "<img src=\"back.png\" title=\""+tr("Previous page")+"\" width=\"27\" ></a>\n";

    bodyHTML += "<div style=\"display: inline-block;\">";
    bodyHTML += "<div id=\"pageDiv\" style=\"vertical-align: middle; text-align: center; color: #000;"
                "width: 50px; background-color: #888; border-radius: 6px;\">" +
                QString::number(page) +  "</div></div>\n";

    bodyHTML += "<a class=\"sdButton\" href=\"javascript:nextPage();\">\n"
                "<img src=\"forward.png\" title=\""+tr("Next page")+"\"  width=\"27\"></a>\n";

    bodyHTML += "<a class=\"sdButton\" href=\"javascript:resetUniverse();\">\n"
                "<img src=\"fileclose.png\" title=\""+tr("Reset universe")+"\" width=\"27\"></a>\n";

    bodyHTML += "<div style=\"display: inline-block; margin-left: 50px;\">" + tr("Universe") + "</div>\n"
                "<div class=\"styled-select\" style=\"display: inline-block;\">\n"
                "<select onchange=\"universeChanged(this.value);\">\n";

    QStringList uniList = doc->inputOutputMap()->universeNames();
    for (int i = 0; i < uniList.count(); i++)
    {
        bodyHTML += "<option value=\"" + QString::number(i) + "\">" + uniList.at(i) + "</option>\n";
    }
    bodyHTML += "</select></div>\n";
    bodyHTML += "</div>\n";

    bodyHTML += "<div id=\"slidersContainer\"></div>\n\n";

    QString str = HTML_HEADER + JScode + CSScode + "</head>\n<body>\n" + bodyHTML + "</body>\n</html>";

    return str;
}

QString WebAccessSimpleDesk::getChannelsMessage(Doc *doc, SimpleDesk *sd,
                                                quint32 universe, int startAddr, int chNumber)
{
    if ((doc == NULL) || (sd == NULL))
        return QString("");

    QString message;
    quint32 universeAddr = (universe << 9);
    qDebug() << "Uni addr:" << universeAddr;

    for (int i = startAddr; i < startAddr + chNumber; i++)
    {
        QString type = "";
        uchar value = sd->getAbsoluteChannelValue(universeAddr + i);
        Fixture* fxi = doc->fixture(doc->fixtureForAddress(universeAddr + i));
        if (fxi != NULL)
        {
            const QLCChannel *ch = fxi->channel(universeAddr + i - fxi->universeAddress());
            if (ch != NULL)
            {
                if (ch->group() == QLCChannel::Intensity)
                {
                    QString hexCol;
                    type = QString("%1.#%2")
                            .arg(ch->group())
                            .arg(hexCol.asprintf("%06X", ch->colour()));
                }
                else
                    type = QString::number(ch->group());
            }
        }

        message.append(QString("%1|%2|%3|").arg(i + 1).arg(value).arg(type));
    }
    // remove trailing separator
    message.truncate(message.length() - 1);

    qDebug() << "Message to send:" << message;
    return message;
}

QString WebAccessSimpleDesk::getKeypadHTML(Doc *doc, SimpleDesk *sd)
{
    if ((doc == NULL) || (sd == NULL))
        return QString("");

    QString str = "<!DOCTYPE html>\n"
                  "<html>\n"
                  "  <head>\n"
                  "    <meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\">\n"
                  "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1, minimum-scale=1, maximum-scale=1\">\n"
                  "    <title>QLC+ Webaccess Keypad</title>\n"
                  "    <link rel=\"stylesheet\" type=\"text/css\" media=\"screen\" href=\"common.css\">\n"
                  "    <link rel=\"stylesheet\" type=\"text/css\" media=\"screen\" href=\"keypad.css\">\n"
                  "    <script src=\"keypad.js\"></script>\n"
                  "  </head>\n"
                  "\n"
                  "  <body>\n"
                  "    <div class=\"controlBar\">\n"
                  "      <a class=\"button button-blue\" href=\"/simpleDesk\"><span>Back</span></a>\n"
                  "      <div class=\"swInfo\">Q Light Controller Plus</div>\n"
                  "    </div>\n"
                  "    <div style=\"margin: 20px 12px 0 20px; font: bold 27px/1.2em 'Trebuchet MS', Arial, Helvetica; color: #fff;\">\n"
                  "      Command: <input style=\"height: 22px; vertical-align: middle;\" id=\"commInput\" type=\"text\" value=\"\">\n"
                  "    </div>\n"
                  "    <table style=\"margin: 20px;\">\n"
                  "      <tr>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('7');\">7</div></td>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('8');\">8</div></td>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('9');\">9</div></td>\n"
                  "        <td><div class=\"button button-keypad\" title=\"Sets a value for a specified DMX channel or group of channels (range)\" onclick=\"composeCommand('A');\">AT</div></td>\n"
                  "      </tr>\n"
                  "      <tr>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('4');\">4</div></td>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('5');\">5</div></td>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('6');\">6</div></td>\n"
                  "        <td><div class=\"button button-keypad\" title=\"Selects a range of DMX channels\" onclick=\"composeCommand('T');\">THRU</div></td>\n"
                  "      </tr>\n"
                  "      <tr>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('1');\">1</div></td>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('2');\">2</div></td>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('3');\">3</div></td>\n"
                  "        <td><div class=\"button button-keypad\" title=\"Sets the maximum value (255) to the selected DMX channels or group of channels\" onclick=\"composeCommand('A255');\">FULL</div></td>\n"
                  "      </tr>\n"
                  "      <tr>\n"
                  "        <td><div class=\"button button-keypad\" title=\"Reduces by given percentage the current channel values\" onclick=\"composeCommand('A-');\">-%</div></td>\n"
                  "        <td><div class=\"button button-keypad\" onclick=\"composeCommand('0');\">0</div></td>\n"
                  "        <td><div class=\"button button-keypad\" title=\"Increases by given percentage the current channel values\" onclick=\"composeCommand('A+');\">+%</div></td>\n"
                  "        <td><div class=\"button button-keypad\" title=\"Sets the minimum value (0) to the selected DMX channels or group of channels\" onclick=\"composeCommand('A00');\">ZERO</div></td>\n"
                  "      </tr>\n"
                  "      <tr>\n"
                  "        <td colspan=\"3\">\n"
                  "          <div class=\"button button-enter\" title=\"Send command\" onclick=\"chans_vals(getElementById('commInput').value);\">ENTER</div>\n"
                  "        </td>\n"
                  "        <td><div class=\"button button-keypad\" title=\"Sets a channels gap within a range\" onclick=\"composeCommand('B');\">BY</div></td>\n"
                  "      </tr>\n"
                  "    </table>\n"
                  "    <br><br>\n"
                  "    <table style=\"margin: 20px;\">\n"
                  "      <tr>\n"
                  "        <td style=\"width: 165px\"><div class=\"button button-reset\" title=\"Reset All Channels\" onclick=\"resetUniverse();\">Reset All</div></td>\n"
                  "        <td style=\"width: 165px\"><div class=\"button button-reset\" title=\"Reset Selected Channel\" onclick=\"resetCh(getElementById('commInput').value);\">Reset Channel</div></td>\n"
                  "      </tr>\n"
                  "    </table>\n"
                  "  </body>\n"
                  "</html>";

    return str;
}
