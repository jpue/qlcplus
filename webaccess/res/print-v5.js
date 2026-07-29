let connectionType = null;
let gattChar = null;
let usbDevice = null;
let usbEndpoint = null;

let validFixtures = [];


function padRight(str, length) {
    str = String(str);
    if (str.length > length) return str.substring(0, length - 1) + '.';
    return str.padEnd(length, ' ');
}

function padLeft(str, length) {
    str = String(str);
    if (str.length > length) return str.substring(0, length);
    return str.padStart(length, ' ');
}

function render() {
    let tableText = ' ID  NAME        UNIV:DMX  CH\n';
    tableText    += '--------------------------------\n';

    validFixtures.forEach(fixture => {
        const univDmxStr = `U${fixture.universe + 1}:${fixture.address + 1}`;
        tableText += `${padLeft(fixture.id, 3)}  ${padRight(fixture.name, 11)} ${padLeft(univDmxStr, 8)} ${padLeft(fixture.channels, 3)}\n`;
    });

    tableText += '--------------------------------\n';
    tableText += 'Total: ' + validFixtures.length + ' fixture' + ((validFixtures.length != 1) ? 's' : '');

    return tableText;
}

function checkReady() {
    if ((connectionType === 'bt' && gattChar) || (connectionType === 'usb' && usbDevice)) {
        if (validFixtures.length > 0) {
            document.getElementById('btnPrint').disabled = false;
        }
    }
}

async function sendRawData(dataArray) {
    const chunkSize = 20;
    if (connectionType === 'bt') {
        for (let i = 0; i < dataArray.length; i += chunkSize) {
            await gattChar.writeValue(dataArray.slice(i, i + chunkSize));
            await new Promise(r => setTimeout(r, 20));
        }
    } else if (connectionType === 'usb') {
        await usbDevice.transferOut(usbEndpoint, dataArray);
    }
}

document.getElementById('btnConnectBT').addEventListener('click', async () => {
    try {
        const statusEl = document.getElementById('status');
        statusEl.innerText = 'Searching...';
        statusEl.style.color = '#38bdf8';

        const device = await navigator.bluetooth.requestDevice({
            acceptAllDevices: true,
            optionalServices: [
                 '000018f0-0000-1000-8000-00805f9b34fb',
                 'e7810a71-73ae-499d-8c15-faa9aef0c3f2',
                 '00001101-0000-1000-8000-00805f9b34fb'
             ]
        });

        const server = await device.gatt.connect();
        const services = await server.getPrimaryServices();

        for (const service of services) {
            const chars = await service.getCharacteristics();
            for (const c of chars) {
                if (c.properties.write || c.properties.writeWithoutResponse) {
                    gattChar = c;
                    break;
                }
            }
            if (gattChar) break;
        }

        if (gattChar) {
            connectionType = 'bt';
            statusEl.innerText = translations[currentLang].statusConnectedBT;
            statusEl.style.color = '#10b981';
            checkReady();
        }
    } catch (error) {
        document.getElementById('status').innerText = 'Error / Canceled';
        document.getElementById('status').style.color = '#f43f5e';
    }
});

document.getElementById('btnConnectUSB').addEventListener('click', async () => {
    const statusEl = document.getElementById('status');
    if (!navigator.usb) {
        statusEl.innerText = 'WebUSB is not supported on this browser. Try Chrome/Edge.';
        statusEl.style.color = '#f43f5e';
        return;
    }

    try {
        statusEl.innerText = 'Searching...';
        statusEl.style.color = '#38bdf8';

        usbDevice = await navigator.usb.requestDevice({ filters: [] });
        await usbDevice.open();
        await usbDevice.selectConfiguration(1);
        await usbDevice.claimInterface(0);

        const interfaceObj = usbDevice.configuration.interfaces[0];
        const endpoint = interfaceObj.alternate.endpoints.find(e => e.direction === 'out');

        if (endpoint) {
            usbEndpoint = endpoint.endpointNumber;
            connectionType = 'usb';
            statusEl.innerText = translations[currentLang].statusConnectedUSB;
            statusEl.style.color = '#10b981';
            checkReady();
        } else {
            throw new Error('OUT Endpoint not found');
        }
    } catch (error) {
        statusEl.innerText = 'Error / Canceled';
        statusEl.style.color = '#f43f5e';
    }
});

document.getElementById('btnPrint').addEventListener('click', async () => {
    if (!connectionType || validFixtures.length === 0) return;

    const printStatus = document.getElementById('printStatus');
    printStatus.style.color = '#38bdf8';
    printStatus.innerText = 'Printing in progress...';

    try {
        const ESC = '\x1B';

        let ticket = ESC + '\x40';
        ticket    += ESC + '\x61\x01';
        ticket    += ESC + '\x45\x01' + 'DMX Patch Sheet' + '\n' + ESC + '\x45\x00';
        ticket    += '--------------------------------\n';
        ticket    += ESC + '\x61\x00';
        ticket    += render() + '\n\n\n';

        const encoder = new TextEncoder();
        const data = encoder.encode(ticket);

        await sendRawData(data);

        printStatus.style.color = '#10b981';
        printStatus.innerText = translations[currentLang].printSuccess;
        setTimeout(() => { printStatus.innerText = ''; }, 4000);
    } catch (error) {
        printStatus.style.color = '#f43f5e';
        printStatus.innerText = 'Error: ' + error.message;
    }
});

window.addEventListener('load', async (e) => {
    await fetch(`/fixtures.json`)
              .then((fixtures) => fixtures.json())
              .then((fixtures) => fixtures['fixtures'])
              .then((fixtures) => {validFixtures = fixtures; return fixtures;})
              .catch((error) => console.error('JSON error', error));

    document.getElementById('preview').innerText = render();
});