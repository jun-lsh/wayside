import React, { useState, useEffect, useRef } from 'react';
import { View, Text, TextInput, TouchableOpacity, ActivityIndicator, StyleSheet, Alert } from 'react-native';
import * as SecureStore from 'expo-secure-store';
import { RSA } from 'react-native-rsa-native';
import { BleUartClient } from '@/utils/ble-uart';
import { sendAndWaitForAck, sendAndWaitForAckPairing } from '@/utils/ble-helpers';
import { ThemedText } from '@/components/themed-text';

const KEYPAIR_STORAGE_KEY = 'rsa_keypair';
const DEVICE_NAME_STORAGE_KEY = 'paired_device_name';

interface PairingStepProps {
  bleClient: BleUartClient;
  onComplete: () => void;
}

export default function PairingStep({ bleClient, onComplete }: PairingStepProps) {
  const [deviceName, setDeviceName] = useState('');
  const [status, setStatus] = useState<'input' | 'connecting' | 'generating' | 'sending'>('input');
  
  useEffect(() => {
    SecureStore.getItemAsync(DEVICE_NAME_STORAGE_KEY).then(name => {
      if (name) setDeviceName(name);
    });
  }, []);

  const handleStartPairing = async () => {
    if (!deviceName.trim()) return Alert.alert('Error', 'Enter device name');
    
    setStatus('connecting');

    try {
      // 1. Connect
      await bleClient.connectByName(deviceName);
      await SecureStore.setItemAsync(DEVICE_NAME_STORAGE_KEY, deviceName);

      // 2. Generate Keys
      setStatus('generating');
      // 1024 bits is good for embedded. 2048 might be too slow for ESP32 decryption.
      const keys = await RSA.generateKeys(1024);
      
      await SecureStore.setItemAsync(KEYPAIR_STORAGE_KEY, JSON.stringify({
        publicKey: keys.public,
        privateKey: keys.private,
      }));

      // 3. Send Public Key
      setStatus('sending');
      // const flattenedKey = keys.public.replace(/(\r\n|\n|\r)/gm, "");
      const normalizedPublicKey = keys.public.replace(/\r\n/g, '\n').replace(/\n/g, '\n');

      // Wait for "PUBKEY_OK"
      await sendAndWaitForAck(bleClient, `PUBKEY:${normalizedPublicKey}`, 'PUBKEY_OK');
      // Wait for "PUBKEY_OK"
      // await sendAndWaitForAck(bleClient, `PUBKEY:${keys.public}`, 'PUBKEY_OK');

      // Done
      onComplete();

    } catch (error: any) {
      console.error(error);
      Alert.alert('Pairing Failed', error.message);
      setStatus('input');
      bleClient.disconnect().catch(() => {});
    }
  };

  if (status !== 'input') {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={{ marginTop: 20 }}>
          {status === 'connecting' && 'Connecting to BLE Device...'}
          {status === 'generating' && 'Generating RSA Keys...'}
          {status === 'sending' && 'Exchanging Keys...'}
        </ThemedText>
      </View>
    );
  }

  return (
    <View>
      <ThemedText type="title" style={styles.title}>Pair Device</ThemedText>
      <ThemedText style={styles.label}>Device Name</ThemedText>
      <TextInput
        style={styles.input}
        value={deviceName}
        onChangeText={setDeviceName}
        placeholder="e.g. ESP-BLE-1"
        placeholderTextColor="#999"
      />
      <TouchableOpacity style={styles.button} onPress={handleStartPairing}>
        <Text style={styles.buttonText}>Connect & Pair</Text>
      </TouchableOpacity>
    </View>
  );
}

const styles = StyleSheet.create({
  center: { alignItems: 'center', justifyContent: 'center', padding: 30 },
  title: { fontSize: 24, fontWeight: 'bold', textAlign: 'center', marginBottom: 30 },
  label: { marginBottom: 8, fontWeight: '600' },
  input: { borderWidth: 1, borderColor: '#ddd', padding: 12, borderRadius: 8, backgroundColor: '#fff', marginBottom: 20 },
  button: { backgroundColor: '#2196F3', padding: 15, borderRadius: 8, alignItems: 'center' },
  buttonText: { color: '#fff', fontWeight: 'bold' }
});