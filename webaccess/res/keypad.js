// the WebSocket instance
var websocket;
var isConnected = false;

// Keypad variables
var by='B';
var at='A';
var th='T';
var pl='+';
var mi='-';
var chanl=[];
var vall=[];
var chanlvall=[];
var channels, c, v, first, last, step, range;
var reqchan;
var addval;
var values=[];
function connect()
{
  var url = 'ws://' + window.location.host + '/qlcplusWS';
  websocket = new WebSocket(url);
  websocket.onopen = function() {
    isConnected = true;
  };

  websocket.onclose = function () {
    isConnected = false;
    console.log("QLC+ connection is closed. Reconnect will be attempted in 1 second.");
    setTimeout(function () {
      connect();
    }, 1000);
  };

  websocket.onerror = function () {
    console.error("QLC+ connection encountered error. Closing socket");
    ws.close();
  };

  websocket.onmessage = function(ev) {
    //alert(ev.data);
    var msg=ev.data;
    var msgParams = ev.data.split('|');
    if (msgParams[0] === "QLC+API")
    {
      if (msgParams[1] === "getChannelsValues")
      {
        values=[];
        for (i = 2; i < msgParams.length; i+=3)
        {
            //console.log(msgParams[i+1]);
            values.push(msgParams[i+1]);
        }
        //console.log(values);
      }
    }
  };
};

window.onload = connect();

function composeCommand(cmd)
{
  if (isConnected === true)
  {
    document.getElementById('commInput').value = document.getElementById('commInput').value + cmd;
  }
  else
  {
    alert("You must connect to QLC+ WebSocket first!");
  }
}


function reqChannelsRange(univ, chan, range)
{
  if (isConnected === true)
  {
    websocket.send("QLC+API|" + "getChannelsValues" + "|" + univ + "|" + chan + "|" + range);
  }
  else
  {
    alert("You must connect to QLC+ WebSocket first!");
  }
}


function resetUniverse()
{
  if (isConnected === true)
  {
    websocket.send("QLC+API|sdResetUniverse");
  }
  else
  {
    alert("You must connect to QLC+ WebSocket first!");
  }
}

function resetCh(chNum)
{
  document.getElementById('commInput').value = '';
  if (isConnected === true)
  {
    websocket.send("QLC+API|sdResetChannel|" + chNum);
  }
  else
  {
    alert("You must connect to QLC+ WebSocket first!");
  }
}
// Modified set Channel Function
function setChannel(c, v)
{
  // This function set a channel passed with param c at value passed with param v
  if (isConnected === true)
  {
    websocket.send("CH|" + c + "|" + v);
  }
  else
  {
    alert("You must connect to QLC+ WebSocket first!");
  }
}

function zip()
{
  var args = [].slice.call(arguments);
  var longest = args.reduce(function(a,b)
  {
    return a.length>b.length ? a : b
  }, []);
  return longest.map(function(_,i)
  {
    return args.map(function(array){return array[i]})
  });
}
// Evaluate commands
function chval(c)
{
  // This function evaluates string that could contain
  //  'T' = THRU, 'B' = BY
  // Returns three values first, last and step values
  // first, last and step should be channels number or values.
  //addval=0;
  if (c.split(pl).length>1)
  {
    addval=c.split(pl)[1];
  }
  if (c.split(mi).length>1)
  {
    addval=c.split(mi)[1];
  }

  if (c.split(by).length>1)
  {
    step=c.split(by)[1];
    range=c.split(by)[0];
    first=range.split(th)[0];
    last=range.split(th)[1];
  }
  else
  {
    if (c.split(th).length>1)
    {
      step=1;
      range=c.split(by)[0];
      first=range.split(th)[0];
      last=range.split(th)[1];
    }
    else
    {
      step=1;
      range=c.split(th);
      first=range[0];
      last=range[0];
    }
  }
  return [parseInt(first,10), parseInt(last,10), parseInt(step,10)];
}


function chans_vals(c)
{
  // Main function. Split command at "AT" string.
  // Left part are single channel number, a range of channels and optionally a step value.
  // Right part are a single value, a range of values and optionally a step value.
  // Calls to chval function to evaluate channels numbers and values.
  // Arrange it on an array used later to assign channels values
  // calling setChannel function in a loop
    //console.log(c);
  chanlvall = [];
  chanl = [];
  vall = [];
  if (c.split(at).length>1)
  {
    a=c.split(at);
    // Call to chval function to assing first, last and step number channels
    chans=chval(a[0]);
    // Call to chval function to assing first, last and step value of channels
    vals=chval(a[1]);
    //console.log('canals values', chans, vals);
    // Set a list of channels for a single channel, a range of channels with a given step
    for (var i = chans[0]; i <= chans[1]; i += chans[2])
      chanl.push(i);
    // Set a list of values for a single value, a range of values
    if (vals[0] === vals[1])
    {
      for (i = 1; i <= chanl.length; i++)
      {
        vall.push(vals[0]);
      }
    }
    else
    {
      llarg = chanl.length - 1;
      for (var i = vals[0]; i <= vals[1]; i += Math.round(vals[1] / llarg))
      {
        vall.push(i);
      }
    }
    //for (var i = chans[0]; i <= chans[1]; i += chans[2])
    //    console.log(chans[i])

    // Set an array of channels number with a dmx value for that channel.
    chanlvall = zip(chanl, vall);
    document.getElementById('commInput').value = '';
    // Iterate on array chanvall and set dmx values for every channel number
    //oldvals=zip(values);
    for (i = 0, len = chanlvall.length; i  < len; i++)
    {
      c = chanlvall[i][0];
      v = chanlvall[i][1];
      // console.log(c, v);
      // console.log(parseInt(values[c-1]) + parseInt(v * 2.55));
        if (a.toString().split(pl).length>1)
        {
          if ((parseInt(values[c-1]) + parseInt(v * 2.55)) >= 255)
          {
            v=255
          }
          else
          {
            v = parseInt(values[c-1]) + parseInt(v * 2.55);
          }
        }
        if (a.toString().split(mi).length>1)
        {
          if ((parseInt(values[c-1]) + parseInt(v * 2.55)) <= 0)
          {
            v=0
          }
          else
          {
            v = parseInt(values[c-1]) + parseInt(v * 2.55);
          }
        }
      setChannel(c,v);
    }
    values=[];
    reqChannelsRange(1, 1, 512);
  }
}