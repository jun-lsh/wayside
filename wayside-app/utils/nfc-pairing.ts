import NfcManager, { NfcTech } from 'react-native-nfc-manager';

export interface BleOobData {
  deviceName?: string;
  macAddress?: string;
}

const BLE_OOB_TYPE = 'application/vnd.bluetooth.le.oob';
const AD_TYPE_LOCAL_NAME = 0x09;
const AD_TYPE_BD_ADDR = 0x1B;

function parseBleOobPayload(payload: number[]): BleOobData {
  const result: BleOobData = {};
  let i = 0;
  
  while (i < payload.length) {
    const len = payload[i];
    if (len === 0 || i + len >= payload.length) break;
    
    const type = payload[i + 1];
    const data = payload.slice(i + 2, i + 1 + len);
    
    if (type === AD_TYPE_LOCAL_NAME) {
      result.deviceName = String.fromCharCode(...data);
    } else if (type === AD_TYPE_BD_ADDR && data.length >= 6) {
      const mac = data.slice(0, 6).reverse();
      result.macAddress = mac.map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(':');
    }
    
    i += 1 + len;
  }
  
  return result;
}

function parseNdefForBleOob(ndefMessage: any[]): BleOobData | null {
  for (const record of ndefMessage) {
    if (record.tnf === 2) {
      const typeStr = String.fromCharCode(...record.type);
      if (typeStr === BLE_OOB_TYPE) {
        return parseBleOobPayload(record.payload);
      }
    }
  }
  return null;
}

export async function initNfc(): Promise<boolean> {
  try {
    const supported = await NfcManager.isSupported();
    if (supported) {
      await NfcManager.start();
    }
    return supported;
  } catch {
    return false;
  }
}

export async function isNfcEnabled(): Promise<boolean> {
  try {
    return await NfcManager.isEnabled();
  } catch {
    return false;
  }
}

export async function scanNfcTag(timeoutMs: number = 30000): Promise<BleOobData> {
  return new Promise(async (resolve, reject) => {
    const timer = setTimeout(() => {
      NfcManager.cancelTechnologyRequest().catch(() => {});
      reject(new Error('NFC scan timeout'));
    }, timeoutMs);
    
    try {
      await NfcManager.requestTechnology(NfcTech.Ndef);
      const tag = await NfcManager.getTag();
      clearTimeout(timer);
      
      if (!tag?.ndefMessage) {
        reject(new Error('No NDEF message on tag'));
        return;
      }
      
      const oobData = parseNdefForBleOob(tag.ndefMessage);
      if (oobData && oobData.deviceName) {
        console.log('BLE OOB data:', oobData);
        resolve(oobData);
      } else {
        reject(new Error('No BLE OOB data found'));
      }
    } catch (error) {
      clearTimeout(timer);
      reject(error);
    } finally {
      NfcManager.cancelTechnologyRequest().catch(() => {});
    }
  });
}

export async function cancelNfcScan(): Promise<void> {
  try {
    await NfcManager.cancelTechnologyRequest();
  } catch {}
}
