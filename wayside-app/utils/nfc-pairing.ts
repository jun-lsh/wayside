import NfcManager, { NfcTech, Ndef, NfcEvents } from 'react-native-nfc-manager';

// ble oob mime type per nfc forum spec
const BLE_OOB_MIME_TYPE = 'application/vnd.bluetooth.le.oob';

// ble eir data types
const EIR_TYPE_COMPLETE_LOCAL_NAME = 0x09;
const EIR_TYPE_SHORT_LOCAL_NAME = 0x08;
const EIR_TYPE_LE_BD_ADDR = 0x1b;
const EIR_TYPE_LE_ROLE = 0x1c;

export interface BleOobData {
  deviceName: string;
  macAddress: string;  // format: "AA:BB:CC:DD:EE:FF"
  macAddressType: number;  // 0 = public, 1 = random
  leRole: number;
}

/**
 * Parse BLE OOB NDEF payload (EIR format)
 */
function parseBleOobPayload(payload: number[]): BleOobData | null {
  let deviceName = '';
  let macAddress = '';
  let macAddressType = 0;
  let leRole = 0;

  let offset = 0;
  while (offset < payload.length) {
    const len = payload[offset];
    if (len === 0 || offset + len >= payload.length) break;

    const type = payload[offset + 1];
    const data = payload.slice(offset + 2, offset + 1 + len);

    switch (type) {
      case EIR_TYPE_COMPLETE_LOCAL_NAME:
      case EIR_TYPE_SHORT_LOCAL_NAME:
        deviceName = String.fromCharCode(...data);
        break;

      case EIR_TYPE_LE_BD_ADDR:
        // 6 bytes MAC + 1 byte address type
        if (data.length >= 7) {
          const mac = data.slice(0, 6);
          macAddress = mac
            .map((b) => b.toString(16).padStart(2, '0').toUpperCase())
            .join(':');
          macAddressType = data[6];
        }
        break;

      case EIR_TYPE_LE_ROLE:
        if (data.length >= 1) {
          leRole = data[0];
        }
        break;
    }

    offset += len + 1;
  }

  if (!deviceName && !macAddress) {
    return null;
  }

  return { deviceName, macAddress, macAddressType, leRole };
}

/**
 * Parse NDEF message looking for BLE OOB record
 */
function parseNdefForBleOob(ndefMessage: any[]): BleOobData | null {
  for (const record of ndefMessage) {
    // check for mime type record
    if (record.tnf === Ndef.TNF_MIME_MEDIA) {
      const mimeType = Ndef.text.decodePayload(new Uint8Array(record.type));
      
      // check if it matches ble oob mime type (case insensitive)
      const typeStr = String.fromCharCode(...record.type);
      if (typeStr.toLowerCase() === BLE_OOB_MIME_TYPE) {
        return parseBleOobPayload(record.payload);
      }
    }
  }
  return null;
}

/**
 * Initialize NFC manager
 */
export async function initNfc(): Promise<boolean> {
  const supported = await NfcManager.isSupported();
  if (supported) {
    await NfcManager.start();
  }
  return supported;
}

/**
 * Read NFC tag and extract BLE pairing info
 * Returns null if no BLE OOB data found
 */
export async function readBleOobFromNfc(): Promise<BleOobData | null> {
  try {
    // request ndef technology
    await NfcManager.requestTechnology(NfcTech.Ndef);

    // get the tag
    const tag = await NfcManager.getTag();
    
    if (!tag?.ndefMessage) {
      console.log('No NDEF message on tag');
      return null;
    }

    // parse for ble oob data
    const oobData = parseNdefForBleOob(tag.ndefMessage);
    
    return oobData;
  } catch (error) {
    console.error('NFC read error:', error);
    throw error;
  } finally {
    // always cancel technology request
    NfcManager.cancelTechnologyRequest().catch(() => {});
  }
}

/**
 * Scan for NFC tag with timeout
 */
export async function scanNfcTag(timeoutMs: number = 30000): Promise<BleOobData> {
  return new Promise(async (resolve, reject) => {
    const timer = setTimeout(() => {
      NfcManager.cancelTechnologyRequest().catch(() => {});
      reject(new Error('NFC scan timeout'));
    }, timeoutMs);

    try {
      const oobData = await readBleOobFromNfc();
      clearTimeout(timer);
      
      if (oobData) {
        resolve(oobData);
      } else {
        reject(new Error('No BLE pairing data found on NFC tag'));
      }
    } catch (error) {
      clearTimeout(timer);
      reject(error);
    }
  });
}

/**
 * Cancel any ongoing NFC operation
 */
export async function cancelNfcScan(): Promise<void> {
  try {
    await NfcManager.cancelTechnologyRequest();
  } catch (error) {
    // ignore
  }
}

/**
 * Check if NFC is enabled
 */
export async function isNfcEnabled(): Promise<boolean> {
  try {
    return await NfcManager.isEnabled();
  } catch {
    return false;
  }
}
