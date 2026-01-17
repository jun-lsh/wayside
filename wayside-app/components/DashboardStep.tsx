import React, { useEffect, useState, useRef } from 'react';
import { View, ScrollView, StyleSheet, TouchableOpacity, Text, Image, ActivityIndicator } from 'react-native';
import * as SecureStore from 'expo-secure-store';
import { RSA } from 'react-native-rsa-native';
import CryptoJS from 'crypto-js';
import { BleUartClient } from '@/utils/ble-uart';
import { ThemedText } from '@/components/themed-text';

const PARTNER_KEY_STORAGE = 'partner_public_key';
const KEYPAIR_STORAGE_KEY = 'rsa_keypair';

interface DashboardStepProps {
  bleClient: BleUartClient;
  onStartUpload: () => void;
  selfieUploadComplete: boolean;
}

export default function DashboardStep({ bleClient, onStartUpload, selfieUploadComplete }: DashboardStepProps) {
  const [logs, setLogs] = useState<string[]>([]);
  const scrollViewRef = useRef<ScrollView>(null);
  
  const [receivedEncryptedPayload, setReceivedEncryptedPayload] = useState<string | null>(null);
  const [partnerSelfie, setPartnerSelfie] = useState<string | null>(null);
  const [decrypting, setDecrypting] = useState(false);
  const [decryptError, setDecryptError] = useState<string | null>(null);

  useEffect(() => {
    bleClient.startNotifications(
      async (msg) => {
        const timestamp = new Date().toLocaleTimeString();
        setLogs((prev) => [...prev, `[${timestamp}] ${msg}`]);

        if (msg.startsWith('PARTNER:')) {
          const partnerKey = msg.replace('PARTNER:', '').trim();
          await SecureStore.setItemAsync(PARTNER_KEY_STORAGE, partnerKey);
          setLogs((prev) => [...prev, `[${timestamp}] Partner key stored`]);
        }

        if (msg.startsWith('RECV_URL:')) {
          const payload = msg.replace('RECV_URL:', '').trim();
          setReceivedEncryptedPayload(payload);
          setLogs((prev) => [...prev, `[${timestamp}] Partner's selfie received`]);
        }
      },
      (err) => {
        setLogs((prev) => [...prev, `[ERROR] ${err.message}`]);
      }
    );

    return () => {};
  }, [bleClient]);

  useEffect(() => {
    if (receivedEncryptedPayload && selfieUploadComplete && !partnerSelfie && !decrypting) {
      decryptAndFetchPartnerSelfie();
    }
  }, [receivedEncryptedPayload, selfieUploadComplete]);

  const decryptAndFetchPartnerSelfie = async () => {
    if (!receivedEncryptedPayload) return;
    
    setDecrypting(true);
    setDecryptError(null);
    
    try {
      const keypairJson = await SecureStore.getItemAsync(KEYPAIR_STORAGE_KEY);
      if (!keypairJson) throw new Error('No keypair found');
      const { privateKey } = JSON.parse(keypairJson);

      const decrypted = await RSA.decrypt(receivedEncryptedPayload, privateKey);
      const [url, aesKey] = decrypted.split('|');
      if (!url || !aesKey) throw new Error('Invalid payload format');

      const response = await fetch(url);
      if (!response.ok) throw new Error('Failed to fetch image');
      const encryptedBase64 = await response.text();

      const decryptedBase64 = CryptoJS.AES.decrypt(encryptedBase64, aesKey).toString(CryptoJS.enc.Utf8);
      if (!decryptedBase64) throw new Error('AES decryption failed');

      setPartnerSelfie(`data:image/jpeg;base64,${decryptedBase64}`);
    } catch (err: any) {
      console.error('Failed to decrypt partner selfie:', err);
      setDecryptError(err.message || 'Failed to decrypt');
    } finally {
      setDecrypting(false);
    }
  };

  return (
    <View style={styles.container}>
      
      {/* Header Row */}
      <View style={styles.headerContainer}>
        <ThemedText type="title">Device Monitor</ThemedText>
        
        {/* New Button */}
        <TouchableOpacity style={styles.actionButton} onPress={onStartUpload}>
          <Text style={styles.actionButtonText}>+ Upload Selfie</Text>
        </TouchableOpacity>
      </View>

      <View style={styles.terminal}>
        <ScrollView 
          ref={scrollViewRef}
          onContentSizeChange={() => scrollViewRef.current?.scrollToEnd({ animated: true })}
        >
          {logs.length === 0 ? (
            <ThemedText style={styles.placeholder}>Waiting for device data...</ThemedText>
          ) : (
            logs.map((log, index) => (
              <ThemedText key={index} style={styles.logText}>{log}</ThemedText>
            ))
          )}
        </ScrollView>
      </View>

      {/* Partner Selfie Section */}
      {receivedEncryptedPayload && !partnerSelfie && !selfieUploadComplete && (
        <View style={styles.partnerStatus}>
          <ThemedText style={styles.waitingText}>
            Partner's selfie received - upload yours to view
          </ThemedText>
        </View>
      )}

      {decrypting && (
        <View style={styles.partnerStatus}>
          <ActivityIndicator size="small" color="#2196F3" />
          <ThemedText style={{ marginLeft: 10 }}>Decrypting partner's selfie...</ThemedText>
        </View>
      )}

      {decryptError && (
        <View style={styles.partnerStatus}>
          <ThemedText style={styles.errorText}>{decryptError}</ThemedText>
        </View>
      )}

      {partnerSelfie && (
        <View style={styles.partnerSelfieContainer}>
          <ThemedText style={styles.partnerLabel}>Partner's Selfie</ThemedText>
          <Image source={{ uri: partnerSelfie }} style={styles.partnerSelfie} />
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  headerContainer: { 
    marginBottom: 15, 
    alignItems: 'center', 
    gap: 10 
  },
  actionButton: {
    backgroundColor: '#673AB7', // Different color to distinguish
    paddingHorizontal: 20,
    paddingVertical: 10,
    borderRadius: 20,
  },
  actionButtonText: { color: '#fff', fontWeight: 'bold', fontSize: 14 },
  
  terminal: {
    flex: 1,
    backgroundColor: '#1e1e1e',
    borderRadius: 8,
    padding: 10,
    minHeight: 300,
  },
  placeholder: { color: '#666', fontStyle: 'italic', textAlign: 'center', marginTop: 20 },
  logText: { color: '#00ff00', fontFamily: 'monospace', fontSize: 12, marginBottom: 4 },
  
  partnerStatus: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    marginTop: 15,
    padding: 10,
  },
  waitingText: { color: '#FFA500', textAlign: 'center' },
  errorText: { color: '#F44336', textAlign: 'center' },
  partnerSelfieContainer: {
    alignItems: 'center',
    marginTop: 15,
  },
  partnerLabel: { marginBottom: 10, fontWeight: '600' },
  partnerSelfie: {
    width: 150,
    height: 150,
    borderRadius: 75,
    borderWidth: 3,
    borderColor: '#4CAF50',
  },
});