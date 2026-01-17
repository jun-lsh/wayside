import React, { useState, useRef } from 'react';
import { 
  View, 
  Text, 
  TouchableOpacity, 
  StyleSheet, 
  Alert, 
  ActivityIndicator, 
  Image,
  Dimensions
} from 'react-native';
import { CameraView, useCameraPermissions } from 'expo-camera';
import * as ImageManipulator from 'expo-image-manipulator';
import * as SecureStore from 'expo-secure-store';
import { RSA } from 'react-native-rsa-native';
import CryptoJS from 'crypto-js';
import { ThemedText } from '@/components/themed-text';
import { supabase } from '@/utils/supabase';
import { BleUartClient } from '@/utils/ble-uart';
import { sendAndWaitForAck } from '@/utils/ble-helpers';

const PARTNER_KEY_STORAGE = 'partner_public_key';

interface SelfieUploadStepProps {
  bleClient: BleUartClient;
  onCancel: () => void;
  onComplete: () => void;
}

const SCREEN_WIDTH = Dimensions.get('window').width;
const CAMERA_SIZE = SCREEN_WIDTH * 0.7;

export default function SelfieUploadStep({ bleClient, onCancel, onComplete }: SelfieUploadStepProps) {
  const [permission, requestPermission] = useCameraPermissions();
  const cameraRef = useRef<CameraView>(null);
  
  // State
  const [capturedImage, setCapturedImage] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [loadingMessage, setLoadingMessage] = useState('');
  
  // Result State
  const [uploadResult, setUploadResult] = useState<{ key: string; publicUrl: string } | null>(null);
  const [bleSent, setBleSent] = useState(false);

  if (!permission) {
    // Camera permissions are still loading
    return <View />;
  }

  if (!permission.granted) {
    return (
      <View style={styles.center}>
        <ThemedText style={styles.textCenter}>We need your permission to show the camera</ThemedText>
        <TouchableOpacity onPress={requestPermission} style={styles.button}>
          <Text style={styles.buttonText}>Grant Permission</Text>
        </TouchableOpacity>
        <TouchableOpacity onPress={onCancel} style={[styles.textLink, { marginTop: 20 }]}>
          <Text style={{ color: '#666' }}>Cancel</Text>
        </TouchableOpacity>
      </View>
    );
  }

  // --- Logic ---

  const takePicture = async () => {
    if (cameraRef.current) {
      try {
        const photo = await cameraRef.current.takePictureAsync({
          quality: 0.5, // Initial quality reduction
          skipProcessing: true // Faster capture
        });
        setCapturedImage(photo?.uri || null);
      } catch (error) {
        Alert.alert('Error', 'Failed to take picture');
      }
    }
  };

// Inside SelfieUploadStep.tsx

const processAndUpload = async () => {
    if (!capturedImage) return;

    setLoading(true);
    try {
      setLoadingMessage('Compressing...');
      const manipulated = await ImageManipulator.manipulateAsync(
        capturedImage,
        [{ resize: { width: 500 } }],
        { compress: 0.5, format: ImageManipulator.SaveFormat.JPEG, base64: true }
      );

      if (!manipulated.base64) throw new Error("Failed to process image data");

      setLoadingMessage('Encrypting...');
      const secretKey = CryptoJS.lib.WordArray.random(32).toString();
      const encrypted = CryptoJS.AES.encrypt(manipulated.base64, secretKey).toString();
      
      setLoadingMessage('Uploading...');
      
      const fileName = `selfie_${Date.now()}.txt`; // Changed to .txt since it's encrypted text

      // --- FIX: Use ArrayBuffer instead of Blob ---
      // Convert the encrypted string to a raw buffer manually
      const arrayBuffer = new Uint8Array(encrypted.length);
      for (let i = 0; i < encrypted.length; i++) {
        arrayBuffer[i] = encrypted.charCodeAt(i);
      }
      // ---------------------------------------------

      const { data, error } = await supabase.storage
        .from('wayside-transfers')
        .upload(fileName, arrayBuffer.buffer, { // Send the buffer
          contentType: 'text/plain',
        //   upsert: true
        });

      if (error) {
        // Log deep details if it still fails
        console.error("Supabase Upload Error:", JSON.stringify(error, null, 2));
        throw error;
      }

      const { data: urlData } = supabase.storage
        .from('wayside-transfers')
        .getPublicUrl(fileName);

      const result = { key: secretKey, publicUrl: urlData.publicUrl };
      setUploadResult(result);

      // Now encrypt and send to badge
      setLoadingMessage('Sending to badge...');
      
      const partnerKey = await SecureStore.getItemAsync(PARTNER_KEY_STORAGE);
      if (!partnerKey) {
        Alert.alert('No Partner', 'No partner key found. Wait for badge pairing to complete.');
        return;
      }

      // Encrypt URL|key payload with partner's RSA public key
      const payload = `${result.publicUrl}|${result.key}`;
      const encryptedPayload = await RSA.encrypt(payload, partnerKey);
      
      // Send over BLE and wait for ack
      await sendAndWaitForAck(bleClient, `ENC_URL:${encryptedPayload}`, 'ENC_URL_OK');
      setBleSent(true);

    } catch (err: any) {
      console.error("Full Error Object:", err);
      Alert.alert('Upload Failed', err.message || "Unknown network error");
    } finally {
      setLoading(false);
    }
  };

  const resetFlow = () => {
    setCapturedImage(null);
    setUploadResult(null);
    setBleSent(false);
  };

  // --- Render ---

  if (loading) {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={{ marginTop: 20 }}>{loadingMessage}</ThemedText>
      </View>
    );
  }

  // View: Success / Result
  if (uploadResult) {
    return (
      <View style={styles.container}>
        <ThemedText type="title" style={styles.title}>
          {bleSent ? 'Sent to Partner!' : 'Upload Complete'}
        </ThemedText>
        
        <View style={styles.resultBox}>
          <ThemedText type="defaultSemiBold" style={{color:'#000'}}>Decryption Key:</ThemedText>
          <View style={styles.codeBlock}>
            <Text style={styles.codeText}>{uploadResult.key}</Text>
          </View>

          <ThemedText type="defaultSemiBold" style={{marginTop: 15, color:'#000'}}>File URL:</ThemedText>
          <View style={styles.codeBlock}>
            <Text style={styles.codeText}>{uploadResult.publicUrl}</Text>
          </View>
        </View>

        <ThemedText style={styles.infoText}>
          {bleSent 
            ? 'Your encrypted selfie URL has been sent to your partner via the badge.'
            : 'Upload complete but failed to send to badge.'}
        </ThemedText>

        <TouchableOpacity onPress={bleSent ? onComplete : onCancel} style={styles.button}>
          <Text style={styles.buttonText}>Done</Text>
        </TouchableOpacity>
      </View>
    );
  }

  // View: Camera / Preview
  return (
    <View style={styles.container}>
      <ThemedText type="subtitle" style={styles.title}>
        {capturedImage ? 'Review Selfie' : 'Take a Selfie'}
      </ThemedText>

      <View style={styles.cameraContainer}>
        {capturedImage ? (
          <Image source={{ uri: capturedImage }} style={styles.camera} />
        ) : (
          <CameraView 
            ref={cameraRef}
            style={styles.camera} 
            facing="front"
          />
        )}
      </View>

      <View style={styles.controls}>
        {capturedImage ? (
          <>
            <TouchableOpacity onPress={processAndUpload} style={styles.button}>
              <Text style={styles.buttonText}>Encrypt & Upload</Text>
            </TouchableOpacity>
            
            <TouchableOpacity onPress={resetFlow} style={styles.textLink}>
              <Text style={{ color: '#F44336' }}>Retake</Text>
            </TouchableOpacity>
          </>
        ) : (
          <>
            <TouchableOpacity onPress={takePicture} style={styles.captureBtn}>
              <View style={styles.captureBtnInner} />
            </TouchableOpacity>
            
            <TouchableOpacity onPress={onCancel} style={styles.textLink}>
              <Text style={{ color: '#666' }}>Cancel</Text>
            </TouchableOpacity>
          </>
        )}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, alignItems: 'center', paddingTop: 20 },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 20 },
  title: { marginBottom: 30, textAlign: 'center' },
  textCenter: { textAlign: 'center', marginBottom: 20 },
  
  // Camera Circle
  cameraContainer: {
    width: CAMERA_SIZE,
    height: CAMERA_SIZE,
    borderRadius: CAMERA_SIZE / 2,
    overflow: 'hidden',
    borderWidth: 4,
    borderColor: '#2196F3',
    elevation: 5,
    backgroundColor: '#000',
    marginBottom: 40,
  },
  camera: { flex: 1 },

  // Controls
  controls: { width: '100%', alignItems: 'center', gap: 15 },
  
  captureBtn: {
    width: 70, height: 70, borderRadius: 35,
    borderWidth: 4, borderColor: '#ccc',
    alignItems: 'center', justifyContent: 'center'
  },
  captureBtnInner: {
    width: 56, height: 56, borderRadius: 28,
    backgroundColor: '#F44336'
  },

  button: { 
    backgroundColor: '#2196F3', 
    paddingVertical: 14, 
    paddingHorizontal: 40, 
    borderRadius: 30,
    minWidth: 200,
    alignItems: 'center'
  },
  buttonText: { color: '#fff', fontSize: 16, fontWeight: 'bold' },
  textLink: { padding: 10 },
  
  // Result Styling
  resultBox: {
    width: '100%',
    backgroundColor: '#f5f5f5',
    padding: 15,
    borderRadius: 10,
    marginBottom: 20,
    borderWidth: 1,
    borderColor: '#ddd'
  },
  codeBlock: {
    backgroundColor: '#1e1e1e',
    padding: 10,
    borderRadius: 6,
    marginTop: 5
  },
  codeText: { color: '#00ff00', fontFamily: 'monospace', fontSize: 12 },
  infoText: { textAlign: 'center', color: '#666', marginBottom: 30, paddingHorizontal: 20 }
});