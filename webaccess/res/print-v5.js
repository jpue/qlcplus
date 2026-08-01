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


function loadLogo() {
    const file = document.getElementById('logoInput')?.files[0];

    if (!file) return null;

    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = (event) => {
            resolve(event.target.result);
        };
        reader.onerror = reject;
        reader.readAsDataURL(file);
    });
}

document.getElementById('logoInput').addEventListener('change', async (event) => await loadLogo());

function drawDipSwitchGraphic(doc, x, y, dmxAddr) {
    const dipStates = [];
    for (let i = 0; i < 9; i++) {
        dipStates.push((dmxAddr & (1 << i)) !== 0);
    }
    dipStates.push(false);

    const blockW = 48;
    const blockH = 14;

    doc.setFillColor(0, 82, 204);
    doc.rect(x, y, blockW, blockH, 'F');

    doc.setTextColor(255, 255, 255);
    doc.setFontSize(5);
    doc.setFont("helvetica", "bold");
    doc.text("ON", x + 1.2, y + 3.0);
    doc.text("DIP", x + blockW - 4.2, y + 3.0);

    const startX = x + 1.8;
    const switchY = y + 3.8;
    const switchW = 3.8;
    const switchH = 6.4;
    const gap = 0.8;

    for (let i = 0; i < 10; i++) {
        const swX = startX + i * (switchW + gap);

        doc.setFillColor(45, 45, 45);
        doc.rect(swX, switchY, switchW, switchH, 'F');

        doc.setFillColor(255, 255, 255);
        if (dipStates[i]) {
            doc.rect(swX + 0.4, switchY + 0.4, switchW - 0.8, (switchH - 0.8) / 2, 'F');
        } else {
            doc.rect(swX + 0.4, switchY + (switchH - 0.8) / 2 + 0.4, switchW - 0.8, (switchH - 0.8) / 2 - 0.4, 'F');
        }

        doc.setTextColor(255, 255, 255);
        doc.setFontSize(4);
        doc.setFont("helvetica", "normal");
        const numText = (i + 1).toString();
        const textOffset = (i === 9) ? 0.8 : 1.3;
        doc.text(numText, swX + textOffset, y + 13.0);
    }
}

document.getElementById('btnGeneratePDF').addEventListener('click', async (event) => {
    event.preventDefault();
    if (validFixtures.length === 0) return;

    const { jsPDF } = window.jspdf;
    const doc = new jsPDF();
    doc.setDocumentProperties({'creator': document.getElementsByClassName('brand-sub')?.item(0)?.innerText.trim() ?? 'Q Light Controller Plus'});

    let titleX = 14;

    try {
        const customLogoDataUrl = await loadLogo();
        if (customLogoDataUrl) {
            doc.addImage(customLogoDataUrl, 'JPEG', 14, 10, 12, 12);
            titleX = 30;
        }
    } catch (error) {
        console.error(error);
    }

    doc.setFontSize(14);
    doc.setFont('helvetica', 'bold');
    doc.setTextColor(17, 24, 39);
    doc.text('DMX Addressing Plan', titleX, 18);

    const tableRows = validFixtures.map(fix => [
        fix.id,
        fix.name,
        `Universe ${fix.universe + 1} : ${fix.address + 1}`,
        fix.channels,
        ''
    ]);

    doc.autoTable({
        startY: 25,
        head: [['ID', 'Fixture Name', 'DMX Address', 'Channels', 'DIP Switch (1-10)']],
        body: tableRows,
        theme: 'striped',
        headStyles: { fillColor: [15, 23, 42], textColor: [255, 255, 255], fontStyle: 'bold', fontSize: 9 },
        bodyStyles: { fontSize: 8.5, textColor: [30, 41, 59], cellPadding: 4 },
        columnStyles: {
            0: { cellWidth: 15, halign: 'center' },
            1: { cellWidth: 55 },
            2: { cellWidth: 38, fontStyle: 'bold' },
            3: { cellWidth: 20, halign: 'center' },
            4: { cellWidth: 52, halign: 'center' }
        },
        didDrawCell: (data) => {
            if (data.section === 'body' && data.column.index === 4) {
                const fixIndex = data.row.index;
                const fixture = validFixtures[fixIndex];
                if (fixture) {
                    const dim = data.cell;
                    const x = dim.x + (dim.width - 48) / 2;
                    const y = dim.y + (dim.height - 14) / 2;
                    drawDipSwitchGraphic(doc, x, y, fixture.address + 1);
                }
            }
        },
        didDrawPage: (data) => {
            const pageCount = doc.internal.getNumberOfPages();
            doc.setFontSize(8);
            doc.setTextColor(170, 170, 170);
            doc.text(`Page ${data.pageNumber} / ${pageCount}`, doc.internal.pageSize.width - 25, doc.internal.pageSize.height - 10);
        }
    });

    doc.save('DMX_Patch.pdf');
});

window.addEventListener('load', async (e) => {
    await fetch(`/fixtures.json`)
              .then((fixtures) => fixtures.json())
              .then((fixtures) => fixtures['fixtures'])
              .then((fixtures) => {validFixtures = fixtures; return fixtures;})
              .catch((error) => console.error('JSON error', error));

    document.getElementById('preview').innerText = render();
});