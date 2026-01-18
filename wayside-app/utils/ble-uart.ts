/**
 * ble-uart.ts - BLE UART client with passkey authentication support
 * 
 * Provides methods to connect to BLE devices running Nordic UART Service,
 * with optional passkey authentication for secure pairing.
 */

import { BleManager, Device, Characteristic, Subscription, ConnectionOptions } from 'react-native-ble-plx';
import { Platform, PermissionsAndroid, NativeModules, NativeEventEmitter } from 'react-native';

// Nordic UART Service UUIDs
export const UART_SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
export const UART_RX_CHAR_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';  // Write to device
export const UART_TX_CHAR_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';  // Notifications from device

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
  private notificationSubscription: Subscription | null = null;
  private config: Required<BleUartConfig>;
  private pendingPasskey: string | null = null;

  constructor(config: BleUartConfig = {}) {
    this.manager = new BleManager();
    this.config = {
      deviceName: config.deviceName || 'ESP-BLE',
      serviceUUID: config.serviceUUID || UART_SERVICE_UUID,
      rxCharUUID: config.rxCharUUID || UART_RX_CHAR_UUID,
      txCharUUID: config.txCharUUID || UART_TX_CHAR_UUID,
      preferredMTU: config.preferredMTU || 500,
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
   * Request necessary permissions for BLE on Android
   */
  async requestPermissions(): Promise<boolean> {
    if (Platform.OS !== 'android') {
      return true;
    }

    const apiLevel = Platform.Version;

    if (apiLevel >= 31) {
      // Android 12+
      const results = await PermissionsAndroid.requestMultiple([
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
      ]);

      return Object.values(results).every(
        (result) => result === PermissionsAndroid.RESULTS.GRANTED
      );
    } else {
      // Android 11 and below
      const result = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION
      );
      return result === PermissionsAndroid.RESULTS.GRANTED;
    }
  }

  /**
   * Scan and connect to device by name (no passkey / Just Works)
   */
  async connectByName(deviceName?: string): Promise<Device> {
    const targetName = deviceName || this.config.deviceName;

    return new Promise((resolve, reject) => {
      let found = false;

      this.manager.startDeviceScan(
        null,
        { allowDuplicates: false },
        (error, device) => {
          if (error) {
            this.manager.stopDeviceScan();
            reject(error);
            return;
          }

          if (device && device.name === targetName && !found) {
            found = true;
            this.manager.stopDeviceScan();

            this.connectToDevice(device)
              .then(resolve)
              .catch(reject);
          }
        }
      );

      // Timeout if device not found
      setTimeout(() => {
        if (!found) {
          this.manager.stopDeviceScan();
          reject(new Error(`Scan timeout: Device "${targetName}" not found`));
        }
      }, this.config.scanTimeout);
    });
  }

  /**
   * Scan and connect to device by name with passkey authentication
   * 
   * Note: react-native-ble-plx handles pairing automatically. 
   * On Android, if the device requires a passkey, the system will show a pairing dialog.
   * We need to either:
   * 1. Use Android's native pairing API to pre-set the passkey
   * 2. Let the user enter it manually in the system dialog
   * 
   * For iOS, passkey pairing happens automatically when the peripheral requests it.
   */
  async connectByNameWithPasskey(deviceName: string, passkey: string): Promise<Device> {
    this.pendingPasskey = passkey;

    return new Promise((resolve, reject) => {
      let found = false;

      this.manager.startDeviceScan(
        null,
        { allowDuplicates: false },
        async (error, device) => {
          if (error) {
            this.manager.stopDeviceScan();
            this.pendingPasskey = null;
            reject(error);
            return;
          }

          if (device && device.name === deviceName && !found) {
            found = true;
            this.manager.stopDeviceScan();

            try {
              // On Android, we can try to initiate bonding with passkey
              if (Platform.OS === 'android' && passkey) {
                // Note: This requires a native module to set the passkey
                // For now, we rely on the system pairing dialog
                console.log(`Connecting to ${deviceName} with passkey ${passkey}`);
              }

              const connectedDevice = await this.connectToDevice(device);
              this.pendingPasskey = null;
              resolve(connectedDevice);
            } catch (connectError) {
              this.pendingPasskey = null;
              reject(connectError);
            }
          }
        }
      );

      setTimeout(() => {
        if (!found) {
          this.manager.stopDeviceScan();
          this.pendingPasskey = null;
          reject(new Error(`Scan timeout: Device "${deviceName}" not found`));
        }
      }, this.config.scanTimeout);
    });
  }

  /**
   * Connect to a discovered device
   */
  private async connectToDevice(device: Device): Promise<Device> {
    const connectedDevice = await device.connect();
    const discoveredDevice = await connectedDevice.discoverAllServicesAndCharacteristics();

    this.device = discoveredDevice;

    // Request larger MTU
    try {
      const deviceWithMTU = await this.manager.requestMTUForDevice(
        discoveredDevice.id,
        this.config.preferredMTU
      );
      this.mtu = deviceWithMTU.mtu;
      console.log(`MTU negotiated to: ${this.mtu}`);
    } catch (mtuError) {
      console.warn('MTU request failed, using default:', mtuError);
      this.mtu = 23;
    }

    return discoveredDevice;
  }

  /**
   * Write a message to the device
   */
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

  /**
   * Start listening for notifications from the device
   */
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
            while (
              (delimiterIndex = this.receiveBuffer.indexOf(this.messageDelimiter)) >= 0
            ) {
              const message = this.receiveBuffer.substring(0, delimiterIndex);
              this.receiveBuffer = this.receiveBuffer.substring(
                delimiterIndex + this.messageDelimiter.length
              );

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

  /**
   * Stop listening for notifications
   */
  stopNotifications(): void {
    if (this.notificationSubscription) {
      this.notificationSubscription.remove();
      this.notificationSubscription = null;
    }
    this.receiveBuffer = '';
  }

  /**
   * Disconnect from the device
   */
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

  /**
   * Check if connected to a device
   */
  isConnected(): boolean {
    return this.device !== null;
  }

  /**
   * Get the negotiated MTU
   */
  getMTU(): number {
    return this.mtu;
  }

  /**
   * Get the connected device
   */
  getDevice(): Device | null {
    return this.device;
  }

  /**
   * Clean up resources
   */
  destroy(): void {
    this.disconnect();
    this.manager.destroy();
  }

  // Utility: Convert ArrayBuffer to Base64
  private arrayBufferToBase64(buffer: ArrayBuffer | Uint8Array): string {
    const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
    let binary = '';
    for (let i = 0; i < bytes.byteLength; i++) {
      binary += String.fromCharCode(bytes[i]);
    }
    return btoa(binary);
  }

  // Utility: Convert Base64 to ArrayBuffer
  private base64ToArrayBuffer(base64: string): ArrayBuffer {
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes.buffer;
  }
}
