import React, { useState, useEffect } from 'react';
import { View, Text, TextInput, TouchableOpacity, ActivityIndicator, StyleSheet, Alert } from 'react-native';
import * as SecureStore from 'expo-secure-store';
import { RSA } from 'react-native-rsa-native';
import { BleUartClient } from '@/utils/ble-uart';
import { sendAndWaitForAck } from '@/utils/ble-helpers';
import { ThemedText } from '@/components/themed-text';
import { initNfc, scanNfcTag, cancelNfcScan, isNfcEnabled, BleOobData } from '@/utils/nfc-pairing';

const KEYPAIR_STORAGE_KEY = 'rsa_keypair';
const DEVICE_NAME_STORAGE_KEY = 'paired_device_name';
const DEVICE_MAC_STORAGE_KEY = 'paired_device_mac';

interface PairingStepProps {
  bleClient: BleUartClient;
  onComplete: () => void;
}

type PairingStatus = 'idle' | 'nfc_scanning' | 'connecting' | 'generating' | 'sending';

export default function PairingStep({ bleClient, onComplete }: PairingStepProps) {
  const [deviceName, setDeviceName] = useState('');
  const [status, setStatus] = useState<PairingStatus>('idle');
  const [nfcSupported, setNfcSupported] = useState(false);
  const [nfcEnabled, setNfcEnabled] = useState(false);

  useEffect(() => {
    SecureStore.getItemAsync(DEVICE_NAME_STORAGE_KEY).then(name => {
      if (name) setDeviceName(name);
    });

    initNfc().then(supported => {
      console.log('NFC supported:', supported);  // Add this
      setNfcSupported(supported);
      if (supported) {
        isNfcEnabled().then(enabled => {
          console.log('NFC enabled:', enabled);  // Add this
          setNfcEnabled(enabled);
        });
      }
    }).catch(err => {
      console.log('NFC init error:', err);  // Add this
    });

    return () => {
      cancelNfcScan();
    };
  }, []);

  const completePairing = async (connectedDeviceName: string) => {
    try {
      await SecureStore.setItemAsync(DEVICE_NAME_STORAGE_KEY, connectedDeviceName);

      setStatus('generating');
      const keys = await RSA.generateKeys(1024);
      await SecureStore.setItemAsync(KEYPAIR_STORAGE_KEY, JSON.stringify({
        publicKey: keys.public,
        privateKey: keys.private,
      }));

      setStatus('sending');
      const normalizedPublicKey = keys.public.replace(/\r\n/g, '\n').replace(/\n/g, '\n');
      await sendAndWaitForAck(bleClient, `PUBKEY:${normalizedPublicKey}`, 'PUBKEY_OK');

      onComplete();
    } catch (error: any) {
      console.error('Pairing error:', error);
      Alert.alert('Pairing Failed', error.message);
      setStatus('idle');
      bleClient.disconnect().catch(() => {});
    }
  };

  const handleNfcPair = async () => {
    if (!nfcEnabled) {
      Alert.alert('NFC Disabled', 'Please enable NFC in your device settings.');
      return;
    }

    setStatus('nfc_scanning');

    try {
      const oobData: BleOobData = await scanNfcTag(60000);
      console.log('NFC BLE OOB data:', oobData);

      setStatus('connecting');

      if (oobData.deviceName) {
        if (oobData.macAddress) {
          await SecureStore.setItemAsync(DEVICE_MAC_STORAGE_KEY, oobData.macAddress);
        }
        await bleClient.connectByName(oobData.deviceName);
        await completePairing(oobData.deviceName);
      } else if (oobData.macAddress) {
        Alert.alert('Error', 'Device name not found in NFC data');
        setStatus('idle');
      }
    } catch (error: any) {
      console.error('NFC pairing error:', error);
      if (error.message !== 'NFC scan timeout') {
        Alert.alert('NFC Pairing Failed', error.message);
      }
      setStatus('idle');
    }
  };

  const handleManualPair = async () => {
    if (!deviceName.trim()) {
      Alert.alert('Error', 'Please enter the device name');
      return;
    }

    setStatus('connecting');

    try {
      await bleClient.connectByName(deviceName);
      await completePairing(deviceName);
    } catch (error: any) {
      console.error('Manual pairing error:', error);
      Alert.alert('Connection Failed', error.message);
      setStatus('idle');
      bleClient.disconnect().catch(() => {});
    }
  };

  const handleCancel = () => {
    cancelNfcScan();
    bleClient.disconnect().catch(() => {});
    setStatus('idle');
  };

  if (status !== 'idle') {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={styles.statusText}>
          {status === 'nfc_scanning' && 'Tap your phone on the device...'}
          {status === 'connecting' && 'Connecting to BLE Device...'}
          {status === 'generating' && 'Generating RSA Keys...'}
          {status === 'sending' && 'Exchanging Keys...'}
        </ThemedText>
        {(status === 'nfc_scanning' || status === 'connecting') && (
          <TouchableOpacity style={styles.cancelButton} onPress={handleCancel}>
            <Text style={styles.cancelButtonText}>Cancel</Text>
          </TouchableOpacity>
        )}
      </View>
    );
  }

  return (
    <View style={styles.container}>
      <ThemedText type="title" style={styles.title}>Pair Device</ThemedText>

      {nfcSupported && (
        <View style={styles.section}>
          <ThemedText style={styles.sectionTitle}>Quick Pair with NFC</ThemedText>
          <ThemedText style={styles.hint}>
            Tap your phone on the device's NFC tag to connect automatically
          </ThemedText>
          <TouchableOpacity
            style={[styles.nfcButton, !nfcEnabled && styles.disabledButton]}
            onPress={handleNfcPair}
            disabled={!nfcEnabled}
          >
            <Text style={styles.buttonText}>
              {nfcEnabled ? '📱 Tap to Pair' : 'NFC Disabled'}
            </Text>
          </TouchableOpacity>
          {!nfcEnabled && (
            <ThemedText style={styles.warning}>
              Enable NFC in Settings to use tap-to-pair
            </ThemedText>
          )}
        </View>
      )}

      {nfcSupported && (
        <View style={styles.divider}>
          <View style={styles.dividerLine} />
          <ThemedText style={styles.dividerText}>OR</ThemedText>
          <View style={styles.dividerLine} />
        </View>
      )}

      <View style={styles.section}>
        <ThemedText style={styles.sectionTitle}>Manual Pairing</ThemedText>
        <ThemedText style={styles.label}>Device Name</ThemedText>
        <TextInput
          style={styles.input}
          value={deviceName}
          onChangeText={setDeviceName}
          placeholder="Enter device name"
          placeholderTextColor="#999"
          autoCapitalize="none"
          autoCorrect={false}
        />
        <TouchableOpacity style={styles.button} onPress={handleManualPair}>
          <Text style={styles.buttonText}>Connect & Pair</Text>
        </TouchableOpacity>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    padding: 20,
  },
  center: {
    alignItems: 'center',
    justifyContent: 'center',
    padding: 30,
  },
  title: {
    fontSize: 24,
    fontWeight: 'bold',
    textAlign: 'center',
    marginBottom: 30,
  },
  section: {
    marginBottom: 20,
  },
  sectionTitle: {
    fontSize: 16,
    fontWeight: '600',
    marginBottom: 8,
  },
  hint: {
    fontSize: 14,
    color: '#666',
    marginBottom: 12,
  },
  label: {
    marginBottom: 8,
    fontWeight: '600',
  },
  input: {
    borderWidth: 1,
    borderColor: '#ddd',
    padding: 12,
    borderRadius: 8,
    backgroundColor: '#fff',
    marginBottom: 16,
    fontSize: 16,
  },
  button: {
    backgroundColor: '#2196F3',
    padding: 15,
    borderRadius: 8,
    alignItems: 'center',
  },
  nfcButton: {
    backgroundColor: '#4CAF50',
    padding: 18,
    borderRadius: 12,
    alignItems: 'center',
  },
  disabledButton: {
    backgroundColor: '#ccc',
  },
  buttonText: {
    color: '#fff',
    fontWeight: 'bold',
    fontSize: 16,
  },
  cancelButton: {
    marginTop: 20,
    padding: 12,
  },
  cancelButtonText: {
    color: '#666',
    fontSize: 16,
  },
  statusText: {
    marginTop: 20,
    fontSize: 16,
  },
  warning: {
    color: '#ff9800',
    fontSize: 12,
    marginTop: 8,
    textAlign: 'center',
  },
  divider: {
    flexDirection: 'row',
    alignItems: 'center',
    marginVertical: 20,
  },
  dividerLine: {
    flex: 1,
    height: 1,
    backgroundColor: '#ddd',
  },
  dividerText: {
    marginHorizontal: 10,
    color: '#999',
  },
});
