// Getting a UF2 onto Melon from the browser: a 1200 baud touch over Web Serial
// restarts the XIAO into its bootloader, which shows up as a drive called
// RP2350. The File System Access API then writes the UF2 onto that drive.

import { load, save } from './store.js';

const USB_VENDORS = [0x2886, 0x2e8a]; // Seeed, Raspberry Pi

export const canSerial = 'serial' in navigator;
export const canWrite = 'showDirectoryPicker' in window;

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export async function restartIntoBootloader() {
  const known = (await navigator.serial.getPorts()).filter(port => USB_VENDORS.includes(port.getInfo().usbVendorId));
  let port = known.length === 1 ? known[0] : null;
  if (!port) {
    port = await navigator.serial.requestPort({ filters: USB_VENDORS.map(usbVendorId => ({ usbVendorId })) });
  }
  try {
    await port.open({ baudRate: 1200 });
    await port.setSignals({ dataTerminalReady: false }).catch(() => {});
    await sleep(120);
  } catch {
    // The board may already be gone, which is what we want.
  }
  await port.close().catch(() => {});
}

// INFO_UF2.TXT tells us which bootloader we are looking at.
export async function checkDrive(dir) {
  let info;
  try {
    const file = await (await dir.getFileHandle('INFO_UF2.TXT')).getFile();
    info = await file.text();
  } catch {
    return { ok: false, reason: `“${dir.name}” is not a bootloader drive. Pick the drive called RP2350.` };
  }
  if (/RP2350/i.test(info)) return { ok: true };
  if (/RP2040|RPI-RP2/i.test(info)) return { ok: false, reason: 'This is an RP2040 board. Melon runs on the XIAO RP2350.' };
  return { ok: false, reason: `“${dir.name}” is a bootloader drive for another board.` };
}

// A drive picked on an earlier visit, if the browser still lets us use it.
export async function rememberedDrive() {
  const dir = await load('drive');
  if (!dir) return null;
  try {
    if ((await dir.queryPermission({ mode: 'readwrite' })) !== 'granted') return { dir, granted: false };
    return { dir, granted: true };
  } catch {
    return null;
  }
}

export async function allow(dir) {
  return (await dir.requestPermission({ mode: 'readwrite' })) === 'granted';
}

export async function waitForDrive(dir, timeout = 15000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) {
    if ((await checkDrive(dir)).ok) return true;
    await sleep(500);
  }
  return false;
}

export async function pickDrive() {
  const options = { id: 'melon-rp2350', mode: 'readwrite' };
  const dir = await window.showDirectoryPicker(options);
  const check = await checkDrive(dir);
  if (check.ok) await save('drive', dir);
  return { dir, ...check };
}

export async function writeUf2(dir, name, bytes, progress) {
  const handle = await dir.getFileHandle(name, { create: true });
  const writable = await handle.createWritable();
  const chunk = 64 * 1024;
  for (let offset = 0; offset < bytes.length; offset += chunk) {
    await writable.write(bytes.subarray(offset, offset + chunk));
    progress(Math.min(1, (offset + chunk) / bytes.length));
  }
  try {
    await writable.close();
  } catch (error) {
    // Melon restarts as soon as the last block arrives, which can pull the
    // drive away before the browser has finished closing the file.
    return { closed: false, error };
  }
  return { closed: true };
}
