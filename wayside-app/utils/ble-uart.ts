import { BleManager, Device, Characteristic, Subscription } from 'react-native-ble-plx';
import { Platform, PermissionsAndroid } from 'react-native';

// ... (UUID constants and Config interface remain the same) ...
export const UART_SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
export const UART_RX_CHAR_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
export const UART_TX_CHAR_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

export interface BleUartConfig {
  deviceName?: string;
  serviceUUID?: string;
  rxCharUUID?: string;
  txCharUUID?: string;
  preferredMTU?: number;
  scanTimeout?: number;
  messageDelimiter?: string;
}

export class BleUartClient {
  private manager: BleManager;
  private device: Device | null = null;
  private mtu: number = 23; 
  private receiveBuffer: string = '';
  private messageDelimiter: string = '\r';
  private notificationSubscription: Subscription | null = null; // Type fix
  private config: Required<BleUartConfig>;

  constructor(config: BleUartConfig = {}) {
    this.manager = new BleManager();
    this.config = {
      deviceName: config.deviceName || 'ESP-BLE-1',
      serviceUUID: config.serviceUUID || UART_SERVICE_UUID,
      rxCharUUID: config.rxCharUUID || UART_RX_CHAR_UUID,
      txCharUUID: config.txCharUUID || UART_TX_CHAR_UUID,
      preferredMTU: config.preferredMTU || 256,
      scanTimeout: config.scanTimeout || 10000,
      messageDelimiter: config.messageDelimiter || '\r',
    };
    this.messageDelimiter = this.config.messageDelimiter;
  }

  /**
   * Initialize BLE manager and check if Bluetooth is enabled
   */
  async initialize(): Promise<void> {
    return new Promise((resolve, reject) => {
      let subscription: Subscription;
      
      subscription = this.manager.onStateChange((state) => {
        if (state === 'PoweredOn') {
            // FIX: Check if subscription is defined before removing 
            // (handles synchronous execution case)
            if (subscription) { 
                subscription.remove();
            }
            resolve();
        } else if (state === 'PoweredOff') {
            if (subscription) {
                subscription.remove();
            }
            reject(new Error('Bluetooth is turned off. Please enable Bluetooth.'));
        } else if (state === 'Unauthorized') {
            if (subscription) {
                subscription.remove();
            }
            reject(new Error('Bluetooth permission denied.'));
        }
      }, true);
    });
  }

  /**
   * Scan for devices and connect to one matching the device name
   */
  async connectByName(deviceName?: string): Promise<Device> {
    const targetName = deviceName || this.config.deviceName;
    
    return new Promise((resolve, reject) => {
      let found = false;
      
      // FIX: startDeviceScan returns void. Do not assign it to a variable.
      this.manager.startDeviceScan(
        null,
        { allowDuplicates: false },
        (error, device) => {
          if (error) {
            // FIX: Just stop scanning, don't remove subscription
            this.manager.stopDeviceScan();
            reject(error);
            return;
          }

          if (device && device.name === targetName && !found) {
            found = true;
            
            // FIX: Just stop scanning
            this.manager.stopDeviceScan();

            device
              .connect()
              .then((connectedDevice) => {
                return connectedDevice.discoverAllServicesAndCharacteristics();
              })
              .then(async (discoveredDevice) => {
                this.device = discoveredDevice;
                
                try {
                  // const deviceWithMTU = await this.manager.requestMTUForDevice(
                  //   discoveredDevice.id,
                  //   this.config.preferredMTU
                  // );
                  this.mtu = 23;
                  console.log(`MTU negotiated to: ${this.mtu}`);
                } catch (mtuError) {
                  console.warn('MTU request failed, using default:', mtuError);
                  this.mtu = 23;
                }

                resolve(discoveredDevice);
              })
              .catch((connectError) => {
                reject(connectError);
              });
          }
        }
      );

      // Timeout if device not found
      setTimeout(() => {
        if (!found) {
          // FIX: Just stop scanning
          this.manager.stopDeviceScan();
          reject(new Error(`Scan timeout: Device "${targetName}" not found`));
        }
      }, this.config.scanTimeout);
    });
  }

  // ... (Rest of the class methods: writeMessage, startNotifications, etc. remain the same) ...
  
  async writeMessage(message: string, withResponse: boolean = true): Promise<void> {
    if (!this.device) {
      throw new Error('Not connected to device');
    }

    const encoder = new TextEncoder();
    const dataBytes = encoder.encode(message);
    
    const chunkSize = this.mtu - 3;
    const totalBytes = dataBytes.length;
    let offset = 0;

    while (offset < totalBytes) {
      const end = Math.min(offset + chunkSize, totalBytes);
      const chunk = dataBytes.slice(offset, end);
      
      const base64Chunk = this.arrayBufferToBase64(chunk);

      try {
        if (withResponse) {
          await this.device.writeCharacteristicWithResponseForService(
            this.config.serviceUUID,
            this.config.rxCharUUID,
            base64Chunk
          );
        } else {
          await this.device.writeCharacteristicWithoutResponseForService(
            this.config.serviceUUID,
            this.config.rxCharUUID,
            base64Chunk
          );
        }
      } catch (error) {
        throw new Error(`Failed to write chunk at offset ${offset}: ${error}`);
      }

      offset = end;
      
      if (offset < totalBytes) {
        await new Promise((resolve) => setTimeout(resolve, 20));
      }
    }
  }

  startNotifications(
    onMessage: (message: string) => void,
    onError?: (error: Error) => void
  ): void {
    if (!this.device) {
      throw new Error('Not connected to device');
    }

    this.receiveBuffer = '';

    this.notificationSubscription = this.device.monitorCharacteristicForService(
      this.config.serviceUUID,
      this.config.txCharUUID,
      (error: any, characteristic: Characteristic | null) => {
        if (error) {
          console.error('Notification error:', error);
          if (onError) {
            onError(error);
          }
          return;
        }

        if (characteristic?.value) {
          try {
            const bytes = this.base64ToArrayBuffer(characteristic.value);
            const decoder = new TextDecoder('utf-8');
            const chunk = decoder.decode(bytes);
            
            this.receiveBuffer += chunk;

            let delimiterIndex: number;
            while ((delimiterIndex = this.receiveBuffer.indexOf(this.messageDelimiter)) >= 0) {
              const message = this.receiveBuffer.substring(0, delimiterIndex);
              this.receiveBuffer = this.receiveBuffer.substring(delimiterIndex + this.messageDelimiter.length);
              
              if (message.length > 0) {
                onMessage(message);
              }
            }
          } catch (decodeError) {
            console.error('Error decoding notification:', decodeError);
            if (onError) {
              onError(new Error(`Decode error: ${decodeError}`));
            }
          }
        }
      }
    );
  }

  stopNotifications(): void {
    if (this.notificationSubscription) {
      this.notificationSubscription.remove();
      this.notificationSubscription = null;
    }
    this.receiveBuffer = '';
  }

  async disconnect(): Promise<void> {
    this.stopNotifications();
    
    if (this.device) {
      try {
        await this.device.cancelConnection();
      } catch (error) {
        console.error('Error disconnecting:', error);
      }
      this.device = null;
    }
  }

  isConnected(): boolean {
    return this.device !== null && !!this.device.isConnected;
  }

  getMTU(): number {
    return this.mtu;
  }

  getDevice(): Device | null {
    return this.device;
  }

  destroy(): void {
    this.disconnect();
    this.manager.destroy();
  }

  private arrayBufferToBase64(buffer: ArrayBuffer): string {
    const bytes = new Uint8Array(buffer);
    let binary = '';
    for (let i = 0; i < bytes.byteLength; i++) {
      binary += String.fromCharCode(bytes[i]);
    }
    return btoa(binary);
  }

  private base64ToArrayBuffer(base64: string): ArrayBuffer {
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes.buffer;
  }
}
