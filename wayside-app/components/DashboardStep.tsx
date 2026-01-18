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
  
  // Stores the "URL|ENCRYPTED_KEY" string
  const [receivedEncryptedPayload, setReceivedEncryptedPayload] = useState<string | null>(null);
  const [partnerSelfie, setPartnerSelfie] = useState<string | null>(null);
  const [decrypting, setDecrypting] = useState(false);
  const [decryptError, setDecryptError] = useState<string | null>(null);

  useEffect(() => {
    bleClient.startNotifications(
      async (rawMsg) => {
        const timestamp = new Date().toLocaleTimeString();
        const msg = rawMsg.replace(/^[\s\r\n]+|[\s\r\n]+$/g, '');
        // Filter out excessively long log messages to keep UI clean
        const logMsg = msg.length > 50 ? `${msg.substring(0, 50)}...` : msg;
        setLogs((prev) => [...prev, `[${timestamp}] ${logMsg}`]);

        if (msg.startsWith('PARTNER:')) {
          const partnerKey = msg.replace('PARTNER:', '').trim();
          await SecureStore.setItemAsync(PARTNER_KEY_STORAGE, partnerKey);
          setLogs((prev) => [...prev, `[${timestamp}] Partner key stored`]);
        }

        if (msg.startsWith('RECV_URL:')) {
          console.log(msg)
          const payload = msg.replace('RECV_URL:', '').trim();
          setReceivedEncryptedPayload(payload);
          setLogs((prev) => [...prev, `[${timestamp}] Partner's selfie payload received`]);
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
      // 1. Get our Private Key
      const keypairJson = await SecureStore.getItemAsync(KEYPAIR_STORAGE_KEY);
      if (!keypairJson) throw new Error('No local keypair found');
      const { privateKey } = JSON.parse(keypairJson);

      // 2. Parse the Payload: URL | RSA_ENCRYPTED_KEY
      const parts = receivedEncryptedPayload.split('|');
      
      if (parts.length < 2) {
        throw new Error('Invalid payload format. Expected URL|KEY');
      }

      const publicUrl = parts[0];
      const encryptedAesKey = parts[1].trim();
      const cleanEncryptedKey = encryptedAesKey.replace(/[^A-Za-z0-9+/=]/g, '');
      // 3. Decrypt the AES Key using our Private RSA Key
      // Note: RSA.decrypt expects the encrypted string (Base64)
      console.log(`Decrypting Key: Len=${cleanEncryptedKey.length}`);
      const decryptedAesKey = await RSA.decrypt(cleanEncryptedKey, privateKey);
      
      if (!decryptedAesKey) throw new Error('Failed to decrypt AES key');

      // 4. Fetch the Encrypted Image Content
      const response = await fetch(publicUrl);
      if (!response.ok) throw new Error('Failed to fetch image file');
      
      // The file was uploaded as text/plain containing the AES encrypted string
      const encryptedFileContent = await response.text();

      // 5. Decrypt the Image Content using the AES Key
      const bytes = CryptoJS.AES.decrypt(encryptedFileContent, decryptedAesKey);
      const originalBase64 = bytes.toString(CryptoJS.enc.Utf8);

      if (!originalBase64) throw new Error('AES decryption resulted in empty data');

      setPartnerSelfie(`data:image/jpeg;base64,${originalBase64}`);
      setLogs((prev) => [...prev, `[Success] Partner selfie decrypted!`]);

    } catch (err: any) {
      console.error('Failed to decrypt partner selfie:', err);
      setDecryptError(err.message || 'Failed to decrypt');
      setLogs((prev) => [...prev, `[Error] ${err.message}`]);
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