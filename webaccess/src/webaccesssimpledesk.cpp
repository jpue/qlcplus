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

#ifndef QMLUI
    int uni = sd->getCurrentUniverseIndex() + 1;
    int page = sd->getCurrentPage();
    int slidersNumber = sd->getSlidersNumber();
#else
    const int uni = sd->universeFilter() + 1;
    const int page = 1; // TODO
    const int slidersNumber = 512; // TODO
#endif

    QString JScode = "<script type=\"text/javascript\" src=\"simpledesk.js\"></script>\n";
    JScode += "<script type=\"text/javascript\">\n";
    JScode += "var currentUniverse = " + QString::number(uni) + ";\n";
    JScode += "var currentPage = " + QString::number(page) + ";\n";
    JScode += "var channelsPerPage = " + QString::number(slidersNumber) + ";\n";
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

#ifndef QMLUI
    QStringList uniList = doc->inputOutputMap()->universeNames();
    for (int i = 0; i < uniList.count(); i++)
    {
        bodyHTML += "<option value=\"" + QString::number(i) + "\">" + uniList.at(i) + "</option>\n";
    }
#else
    for (const QVariant& universe : sd->universesListModel().toList())
    {
        const QVariantMap& universeMap = universe.toMap();
        bodyHTML += "<option value=\"" + QString::number(universeMap.value("mValue", 0).toInt()) + "\">" +
                    universeMap.value("mLabel", "").toString().toHtmlEscaped() + "</option>\n";
    }
#endif

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

    qDebug() << "getChannelsMessage" << universe << startAddr << chNumber;

    QString message;
    quint32 universeAddr = (universe << 9);
    qDebug() << "Uni addr:" << universeAddr;

  #ifdef QMLUI
    if (sd->universeFilter() != universe)
        sd->setUniverseFilter(universe);
  #endif

    for (int i = startAddr; i < startAddr + chNumber; i++)
    {
        QString type = "";

      #ifndef QMLUI
        uchar value = sd->getAbsoluteChannelValue(universeAddr + i);
      #else
        uchar value = sd->value(i);
      #endif

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

    QString str = "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
                  "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>\n"
                  "<meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\">\n"
                  "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, minimum-scale=1, maximum-scale=1\">\n"
                  "<link rel=\"stylesheet\" type=\"text/css\" media=\"screen\" href=\"common.css\">\n"
                  "\n"
                  "<style>\n"
                  "html { height: 100%; background-color: #222; }\n"
                  "\n"
                  "body {\n"
                  " margin: 0px;\n"
                  " background-color: #222;\n"
                  "}\n"
                  "\n"
                  "td { width: 80px; height: 60px; }\n"
                  "\n"
                  ".button-keypad {\n"
                  " display: table-cell;\n"
                  " width: 80px;\n"
                  " height: 60px;\n"
                  " cursor: pointer;\n"
                  " vertical-align: middle;\n"
                  " background: #4477a1;\n"
                  " border: 0;\n"
                  "}\n"
                  "\n"
                  ".button-keypad:hover {\n"
                  " background: #81a8cb;\n"
                  "}\n"
                  "\n"
                  ".button-enter {\n"
                  " display: table-cell;\n"
                  " width: 320px;\n"
                  " height: 60px;\n"
                  " cursor: pointer;\n"
                  " vertical-align: middle;\n"
                  " background: #4CA145;\n"
                  " border: 0;\n"
                  "}\n"
                  "\n"
                  ".button-enter:hover {\n"
                  " background: #81a8cb;\n"
                  "}\n"
                  "\n"
                  ".button-reset {\n"
                  " display: table-cell;\n"
                  " width: 160px;\n"
                  " height: 60px;\n"
                  " cursor: pointer;\n"
                  " vertical-align: middle;\n"
                  " background: #c53e3e;\n"
                  " border: 0;\n"
                  "}\n"
                  "\n"
                  ".button-reset:hover {\n"
                  " background: #cb7e7e;\n"
                  "}\n"
                  "</style>\n"
                  "\n"
                  "<script language=\"javascript\" type=\"text/javascript\">\n"
                  "// the WebSocket instance\n"
                  "var websocket;\n"
                  "var isConnected = false;\n"
                  "\n"
                  "// Keypad variables\n"
                  "var by='B';\n"
                  "var at='A';\n"
                  "var th='T';\n"
                  "var pl='+';\n"
                  "var mi='-';\n"
                  "var chanl=[];\n"
                  "var vall=[];\n"
                  "var chanlvall=[];\n"
                  "var channels, c, v, first, last, step, range;\n"
                  "var reqchan;\n"
                  "var addval;\n"
                  "var values=[];\n"
                  "function connect()\n"
                  "{\n"
                  "  var url = 'ws://' + window.location.host + '/qlcplusWS';\n"
                  "  websocket = new WebSocket(url);\n"
                  "  websocket.onopen = function() {\n"
                  "    isConnected = true;\n"
                  "  };\n"
                  "\n"
                  "  websocket.onclose = function () {\n"
                  "    isConnected = false;\n"
                  "    console.log(\"QLC+ connection is closed. Reconnect will be attempted in 1 second.\");\n"
                  "    setTimeout(function () {\n"
                  "      connect();\n"
                  "    }, 1000);\n"
                  "  };\n"
                  "\n"
                  "  websocket.onerror = function () {\n"
                  "    console.error(\"QLC+ connection encountered error. Closing socket\");\n"
                  "    ws.close();\n"
                  "  };\n"
                  "\n"
                  "  websocket.onmessage = function(ev) {\n"
                  "    //alert(ev.data);\n"
                  "    var msg=ev.data;\n"
                  "    var msgParams = ev.data.split('|');\n"
                  "    if (msgParams[0] === \"QLC+API\")\n"
                  "    {\n"
                  "      if (msgParams[1] === \"getChannelsValues\")\n"
                  "      {\n"
                  "        values=[];\n"
                  "        for (i = 2; i < msgParams.length; i+=3)\n"
                  "        {\n"
                  "            //console.log(msgParams[i+1]);\n"
                  "            values.push(msgParams[i+1]);\n"
                  "        }\n"
                  "        //console.log(values);\n"
                  "      }\n"
                  "    }\n"
                  "  };\n"
                  "};\n"
                  "\n"
                  "window.onload = connect();\n"
                  "\n"
                  "function composeCommand(cmd)\n"
                  "{\n"
                  "  if (isConnected === true)\n"
                  "  {\n"
                  "    document.getElementById('commInput').value = document.getElementById('commInput').value + cmd;\n"
                  "  }\n"
                  "  else\n"
                  "  {\n"
                  "    alert(\"You must connect to QLC+ WebSocket first!\");\n"
                  "  }\n"
                  "}\n"
                  "\n"
                  "\n"
                  "function reqChannelsRange(univ, chan, range)\n"
                  "{\n"
                  "  if (isConnected === true)\n"
                  "  {\n"
                  "    websocket.send(\"QLC+API|\" + \"getChannelsValues\" + \"|\" + univ + \"|\" + chan + \"|\" + range);\n"
                  "  }\n"
                  "  else\n"
                  "  {\n"
                  "    alert(\"You must connect to QLC+ WebSocket first!\");\n"
                  "  }\n"
                  "}\n"
                  "\n"
                  "\n"
                  "function resetUniverse()\n"
                  "{\n"
                  "  if (isConnected === true)\n"
                  "  {\n"
                  "    websocket.send(\"QLC+API|sdResetUniverse\");\n"
                  "  }\n"
                  "  else\n"
                  "  {\n"
                  "    alert(\"You must connect to QLC+ WebSocket first!\");\n"
                  "  }\n"
                  "}\n"
                  "\n"
                  "function resetCh(chNum)\n"
                  "{\n"
                  "  document.getElementById('commInput').value = '';\n"
                  "  if (isConnected === true)\n"
                  "  {\n"
                  "    websocket.send(\"QLC+API|sdResetChannel|\" + chNum);\n"
                  "  }\n"
                  "  else\n"
                  "  {\n"
                  "    alert(\"You must connect to QLC+ WebSocket first!\");\n"
                  "  }\n"
                  "}\n"
                  "// Modified set Channel Function\n"
                  "function setChannel(c, v)\n"
                  "{\n"
                  "  // This function set a channel passed with param c at value passed with param v\n"
                  "  if (isConnected === true)\n"
                  "  {\n"
                  "    websocket.send(\"CH|\" + c + \"|\" + v);\n"
                  "  }\n"
                  "  else\n"
                  "  {\n"
                  "    alert(\"You must connect to QLC+ WebSocket first!\");\n"
                  "  }\n"
                  "}\n"
                  "\n"
                  "function zip()\n"
                  "{\n"
                  "  var args = [].slice.call(arguments);\n"
                  "  var longest = args.reduce(function(a,b)\n"
                  "  {\n"
                  "    return a.length>b.length ? a : b\n"
                  "  }, []);\n"
                  "  return longest.map(function(_,i)\n"
                  "  {\n"
                  "    return args.map(function(array){return array[i]})\n"
                  "  });\n"
                  "}\n"
                  "// Evaluate commands\n"
                  "function chval(c)\n"
                  "{\n"
                  "  // This function evaluates string that could contain\n"
                  "  //  'T' = THRU, 'B' = BY\n"
                  "  // Returns three values first, last and step values\n"
                  "  // first, last and step should be channels number or values.\n"
                  "  //addval=0;\n"
                  "  if (c.split(pl).length>1)\n"
                  "  {\n"
                  "    addval=c.split(pl)[1];\n"
                  "  }\n"
                  "  if (c.split(mi).length>1)\n"
                  "  {\n"
                  "    addval=c.split(mi)[1];\n"
                  "  }\n"
                  "\n"
                  "  if (c.split(by).length>1)\n"
                  "  {\n"
                  "    step=c.split(by)[1];\n"
                  "    range=c.split(by)[0];\n"
                  "    first=range.split(th)[0];\n"
                  "    last=range.split(th)[1];\n"
                  "  }\n"
                  "  else\n"
                  "  {\n"
                  "    if (c.split(th).length>1)\n"
                  "    {\n"
                  "      step=1;\n"
                  "      range=c.split(by)[0];\n"
                  "      first=range.split(th)[0];\n"
                  "      last=range.split(th)[1];\n"
                  "    }\n"
                  "    else\n"
                  "    {\n"
                  "      step=1;\n"
                  "      range=c.split(th);\n"
                  "      first=range[0];\n"
                  "      last=range[0];\n"
                  "    }\n"
                  "  }\n"
                  "  return [parseInt(first,10), parseInt(last,10), parseInt(step,10)];\n"
                  "}\n"
                  "\n"
                  "\n"
                  "function chans_vals(c)\n"
                  "{\n"
                  "  // Main function. Split command at \"AT\" string.\n"
                  "  // Left part are single channel number, a range of channels and optionally a step value.\n"
                  "  // Right part are a single value, a range of values and optionally a step value.\n"
                  "  // Calls to chval function to evaluate channels numbers and values.\n"
                  "  // Arrange it on an array used later to assign channels values\n"
                  "  // calling setChannel function in a loop\n"
                  "    //console.log(c);\n"
                  "  chanlvall = [];\n"
                  "  chanl = [];\n"
                  "  vall = [];\n"
                  "  if (c.split(at).length>1)\n"
                  "  {\n"
                  "    a=c.split(at);\n"
                  "    // Call to chval function to assing first, last and step number channels\n"
                  "    chans=chval(a[0]);\n"
                  "    // Call to chval function to assing first, last and step value of channels\n"
                  "    vals=chval(a[1]);\n"
                  "    //console.log('canals values', chans, vals);\n"
                  "    // Set a list of channels for a single channel, a range of channels with a given step\n"
                  "    for (var i = chans[0]; i <= chans[1]; i += chans[2])\n"
                  "      chanl.push(i);\n"
                  "    // Set a list of values for a single value, a range of values\n"
                  "    if (vals[0] === vals[1])\n"
                  "    {\n"
                  "      for (i = 1; i <= chanl.length; i++)\n"
                  "      {\n"
                  "        vall.push(vals[0]);\n"
                  "      }\n"
                  "    }\n"
                  "    else\n"
                  "    {\n"
                  "      llarg = chanl.length - 1;\n"
                  "      for (var i = vals[0]; i <= vals[1]; i += Math.round(vals[1] / llarg))\n"
                  "      {\n"
                  "        vall.push(i);\n"
                  "      }\n"
                  "    }\n"
                  "    //for (var i = chans[0]; i <= chans[1]; i += chans[2])\n"
                  "    //    console.log(chans[i])\n"
                  "\n"
                  "    // Set an array of channels number with a dmx value for that channel.\n"
                  "    chanlvall = zip(chanl, vall);\n"
                  "    document.getElementById('commInput').value = '';\n"
                  "    // Iterate on array chanvall and set dmx values for every channel number\n"
                  "    //oldvals=zip(values);\n"
                  "    for (i = 0, len = chanlvall.length; i  < len; i++)\n"
                  "    {\n"
                  "      c = chanlvall[i][0];\n"
                  "      v = chanlvall[i][1];\n"
                  "      // console.log(c, v);\n"
                  "      // console.log(parseInt(values[c-1]) + parseInt(v * 2.55));\n"
                  "        if (a.toString().split(pl).length>1)\n"
                  "        {\n"
                  "          if ((parseInt(values[c-1]) + parseInt(v * 2.55)) >= 255)\n"
                  "          {\n"
                  "            v=255\n"
                  "          }\n"
                  "          else\n"
                  "          {\n"
                  "            v = parseInt(values[c-1]) + parseInt(v * 2.55);\n"
                  "          }\n"
                  "        }\n"
                  "        if (a.toString().split(mi).length>1)\n"
                  "        {\n"
                  "          if ((parseInt(values[c-1]) + parseInt(v * 2.55)) <= 0)\n"
                  "          {\n"
                  "            v=0\n"
                  "          }\n"
                  "          else\n"
                  "          {\n"
                  "            v = parseInt(values[c-1]) + parseInt(v * 2.55);\n"
                  "          }\n"
                  "        }\n"
                  "      setChannel(c,v);\n"
                  "    }\n"
                  "    values=[];\n"
                  "    reqChannelsRange(1, 1, 512);\n"
                  "  }\n"
                  "}\n"
                  "\n"
                  "</script>\n"
                  "\n"
                  "</head>\n"
                  "\n"
                  "<body>\n"
                  "<div class=\"controlBar\">\n"
                  "<a class=\"button button-blue\" href=\"/simpleDesk\"><span>Back</span></a>\n"
                  "<div class=\"swInfo\">Q Light Controller Plus</div></div>\n"
                  "<div style=\"margin: 20px 12px 0 20px; font: bold 27px/1.2em 'Trebuchet MS',Arial, Helvetica; color: #fff;\">\n"
                  "Command: <input style=\"height: 22px; vertical-align: middle;\" id=\"commInput\" value='' type=\"input\">\n"
                  "</div>\n"
                  "<table style=\"margin: 20px;\">\n"
                  "<tr>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('7');\">7</div></td>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('8');\">8</div></td>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('9');\">9</div></td>\n"
                  "  <td><div class=\"button button-keypad\" title=\"Sets a value for a specified DMX channel or group of channels (range)\" onclick=\"composeCommand('A');\">AT</div></td>\n"
                  "</tr>\n"
                  "<tr>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('4');\">4</div></td>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('5');\">5</div></td>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('6');\">6</div></td>\n"
                  "  <td><div class=\"button button-keypad\" title=\"Selects a range of DMX channels\" onclick=\"composeCommand('T');\">THRU</div></td>\n"
                  "</tr>\n"
                  "<tr>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('1');\">1</div></td>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('2');\">2</div></td>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('3');\">3</div></td>\n"
                  "  <td><div class=\"button button-keypad\" title=\"Sets the maximum value (255) to the selected DMX channels or group of channels\" onclick=\"composeCommand('A255');\">FULL</div></td>\n"
                  "</tr>\n"
                  "<tr>\n"
                  "  <td><div class=\"button button-keypad\" title=\"Reduces by given percentage the current channel values\" onclick=\"composeCommand('A-');\">-%</div></td>\n"
                  "  <td><div class=\"button button-keypad\" onclick=\"composeCommand('0');\">0</div></td>\n"
                  "  <td><div class=\"button button-keypad\" title=\"Increases by given percentage the current channel values\" onclick=\"composeCommand('A+');\">+%</div></td>\n"
                  "  <td><div class=\"button button-keypad\" title=\"Sets the minimum value (0) to the selected DMX channels or group of channels\" onclick=\"composeCommand('A00');\">ZERO</div></td>\n"
                  "</tr>\n"
                  "<tr>\n"
                  " <td colspan=\"3\">\n"
                  "   <div class=\"button button-enter\" title=\"Send command\" onclick=\"chans_vals(getElementById('commInput').value);\">ENTER</div>\n"
                  "  </td>\n"
                  "  <td><div class=\"button button-keypad\" title=\"Sets a channels gap within a range\" onclick=\"composeCommand('B');\">BY</div></td>\n"
                  "</tr>\n"
                  "</table>\n"
                  "<br><br>\n"
                  "<table style=\"margin: 20px;\">\n"
                  "<tr>\n"
                  " <td style=\"width: 165px\"><div class=\"button button-reset\" title=\"Reset All Channels\" onclick=\"resetUniverse();\">Reset All</div></td>\n"
                  " <td style=\"width: 165px\"><div class=\"button button-reset\" title=\"Reset Selected Channel\" onclick=\"resetCh(getElementById('commInput').value);\">Reset Channel</div></td>\n"
                  "</tr>\n"
                  "</table>\n"
                  "\n"
                  "</body>\n"
                  "</html>";

    return str;
}
