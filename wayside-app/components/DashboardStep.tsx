import React, { useEffect, useState, useRef } from 'react';
import { 
  View, 
  StyleSheet, 
  TouchableOpacity, 
  Text, 
  Image, 
  ActivityIndicator, 
  Animated, 
  Modal,
  Dimensions
} from 'react-native';
import * as SecureStore from 'expo-secure-store';
import { RSA } from 'react-native-rsa-native';
import CryptoJS from 'crypto-js';
import { BleUartClient } from '@/utils/ble-uart';
import { ThemedText } from '@/components/themed-text';

const PARTNER_KEY_STORAGE = 'partner_public_key';
const KEYPAIR_STORAGE_KEY = 'rsa_keypair';
const SCREEN_WIDTH = Dimensions.get('window').width;

interface DashboardStepProps {
  bleClient: BleUartClient;
  onStartUpload: () => void;
  selfieUploadComplete: boolean;
  onReset: () => void; // New prop to reset flow
}

export default function DashboardStep({ 
  bleClient, 
  onStartUpload, 
  selfieUploadComplete,
  onReset 
}: DashboardStepProps) {
  // Logic State
  const [partnerKeyReceived, setPartnerKeyReceived] = useState(false);
  const [receivedEncryptedPayload, setReceivedEncryptedPayload] = useState<string | null>(null);
  const [partnerSelfie, setPartnerSelfie] = useState<string | null>(null);
  const [decrypting, setDecrypting] = useState(false);
  
  // UI State
  const [showMatchModal, setShowMatchModal] = useState(false);
  
  // Animation Values
  const pulseAnim = useRef(new Animated.Value(1)).current;

  // 1. Setup Pulse Animation for Searching
  useEffect(() => {
    if (!partnerKeyReceived && !selfieUploadComplete) {
      Animated.loop(
        Animated.sequence([
          Animated.timing(pulseAnim, {
            toValue: 1.2,
            duration: 1000,
            useNativeDriver: true,
          }),
          Animated.timing(pulseAnim, {
            toValue: 1,
            duration: 1000,
            useNativeDriver: true,
          }),
        ])
      ).start();
    } else {
      pulseAnim.setValue(1); // Reset if stopped
    }
  }, [partnerKeyReceived, selfieUploadComplete]);

  // 2. BLE Listeners
  useEffect(() => {
    const handleMsg = async (rawMsg: string) => {
      const msg = rawMsg.replace(/^[\s\r\n]+|[\s\r\n]+$/g, '');

      // PARTNER FOUND
      if (msg.startsWith('PARTNER:')) {
        const partnerKey = msg.replace('PARTNER:', '').trim();
        await SecureStore.setItemAsync(PARTNER_KEY_STORAGE, partnerKey);
        setPartnerKeyReceived(true);
        // Only show modal if we haven't uploaded yet
        if (!selfieUploadComplete) {
          setShowMatchModal(true);
        }
      }

      // PARTNER SELFIE RECEIVED
      if (msg.startsWith('RECV_URL:')) {
        const payload = msg.replace('RECV_URL:', '').trim();
        setReceivedEncryptedPayload(payload);
      }
    };

    bleClient.startNotifications(
      handleMsg,
      (err) => console.log('BLE Error:', err)
    );

    return () => {}; // Cleanup if needed
  }, [bleClient, selfieUploadComplete]);

  // 3. Decrypt Trigger
  useEffect(() => {
    if (receivedEncryptedPayload && !partnerSelfie && !decrypting) {
      decryptAndFetchPartnerSelfie();
    }
  }, [receivedEncryptedPayload]);

  const decryptAndFetchPartnerSelfie = async () => {
    if (!receivedEncryptedPayload) return;
    setDecrypting(true);
    
    try {
      const keypairJson = await SecureStore.getItemAsync(KEYPAIR_STORAGE_KEY);
      if (!keypairJson) throw new Error('No local keypair found');
      const { privateKey } = JSON.parse(keypairJson);

      const parts = receivedEncryptedPayload.split('|');
      const publicUrl = parts[0];
      const cleanEncryptedKey = parts[1].trim().replace(/[^A-Za-z0-9+/=]/g, '');

      const decryptedAesKey = await RSA.decrypt(cleanEncryptedKey, privateKey);
      const response = await fetch(publicUrl);
      const encryptedFileContent = await response.text();

      const bytes = CryptoJS.AES.decrypt(encryptedFileContent, decryptedAesKey);
      const originalBase64 = bytes.toString(CryptoJS.enc.Utf8);

      if (originalBase64) {
        setPartnerSelfie(`data:image/jpeg;base64,${originalBase64}`);
      }
    } catch (err) {
      console.error('Decryption failed:', err);
    } finally {
      setDecrypting(false);
    }
  };

  const handleMatchConfirm = () => {
    setShowMatchModal(false);
    onStartUpload();
  };

  // --- RENDER HELPERS ---

  // State 1: Searching
  if (!partnerKeyReceived && !selfieUploadComplete) {
    return (
      <View style={styles.container}>
        <ThemedText type="title" style={styles.title}>Scanning nearby...</ThemedText>
        <View style={styles.pulseContainer}>
          <Animated.View style={[styles.pulseCircle, { transform: [{ scale: pulseAnim }] }]} />
          <Image source={require('@/assets/images/react-logo.png')} style={styles.centerIcon} /> 
          {/* Replace above with a generic user icon or avatar if available */}
        </View>
        <ThemedText style={styles.subText}>Looking for devices matching your interests</ThemedText>
      </View>
    );
  }

  // State 2: Waiting for Partner (We uploaded, they haven't sent back yet)
  if (selfieUploadComplete && !partnerSelfie) {
    return (
      <View style={styles.container}>
        <ActivityIndicator size="large" color="#673AB7" style={{ marginBottom: 20 }} />
        <ThemedText type="subtitle">Waiting for other user...</ThemedText>
        <ThemedText style={styles.subText}>You've shared your location. Waiting for them to share back.</ThemedText>
        
        {/* Debug/Dev: Button to force check if stuck (optional) */}
        {decrypting && <Text style={{marginTop:10, color:'#666'}}>Decrypting incoming data...</Text>}
      </View>
    );
  }

  // State 3: Linked Up (Final State)
  if (partnerSelfie) {
    return (
      <View style={styles.container}>
        <ThemedText type="title" style={styles.title}>Let's link up :)</ThemedText>
        
        <View style={styles.finalImageContainer}>
          <Image source={{ uri: partnerSelfie }} style={styles.finalSelfie} />
        </View>

        <TouchableOpacity style={styles.primaryButton} onPress={onReset}>
          <Text style={styles.primaryButtonText}>Back to Searching</Text>
        </TouchableOpacity>
      </View>
    );
  }

  // Default / Transition State (e.g. Partner found but modal closed and not uploaded yet)
  return (
    <View style={styles.container}>
       <Modal
        animationType="slide"
        transparent={true}
        visible={showMatchModal}
        onRequestClose={() => {}}
      >
        <View style={styles.modalOverlay}>
          <View style={styles.modalContent}>
            <ThemedText type="subtitle" style={{marginBottom: 10}}>Match Found!</ThemedText>
            <ThemedText style={{textAlign: 'center', marginBottom: 20}}>
              Found someone nearby with similar interests! Do you want to share where you're at?
            </ThemedText>
            <TouchableOpacity style={styles.primaryButton} onPress={handleMatchConfirm}>
              <Text style={styles.primaryButtonText}>Yes, Share Selfie</Text>
            </TouchableOpacity>
          </View>
        </View>
      </Modal>

      {/* Background UI while modal is up or transition happens */}
      <ThemedText>Connecting...</ThemedText>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { 
    flex: 1, 
    alignItems: 'center', 
    justifyContent: 'center',
    padding: 20
  },
  title: { marginBottom: 30, textAlign: 'center' },
  subText: { marginTop: 20, textAlign: 'center', color: '#888' },
  
  // Pulse Animation
  pulseContainer: {
    width: 200,
    height: 200,
    alignItems: 'center',
    justifyContent: 'center',
  },
  pulseCircle: {
    position: 'absolute',
    width: 150,
    height: 150,
    borderRadius: 75,
    backgroundColor: 'rgba(103, 58, 183, 0.3)', // Purple tint
  },
  centerIcon: {
    width: 60,
    height: 60,
    tintColor: '#673AB7'
  },

  // Modal
  modalOverlay: {
    flex: 1,
    backgroundColor: 'rgba(0,0,0,0.6)',
    justifyContent: 'center',
    alignItems: 'center',
  },
  modalContent: {
    width: '80%',
    backgroundColor: '#fff',
    borderRadius: 20,
    padding: 25,
    alignItems: 'center',
    elevation: 5,
  },

  // Buttons
  primaryButton: {
    backgroundColor: '#673AB7',
    paddingVertical: 12,
    paddingHorizontal: 30,
    borderRadius: 25,
    marginTop: 20,
    minWidth: 200,
    alignItems: 'center',
  },
  primaryButtonText: { color: '#fff', fontWeight: 'bold', fontSize: 16 },

  // Final State
  finalImageContainer: {
    marginVertical: 30,
    shadowColor: "#000",
    shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.3,
    shadowRadius: 4.65,
    elevation: 8,
  },
  finalSelfie: {
    width: SCREEN_WIDTH * 0.6,
    height: SCREEN_WIDTH * 0.6,
    borderRadius: (SCREEN_WIDTH * 0.6) / 2,
    borderWidth: 4,
    borderColor: '#4CAF50',
  },
});