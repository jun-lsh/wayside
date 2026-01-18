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
const SCREEN_WIDTH = Dimensions.get('window').width;
const CAMERA_SIZE = SCREEN_WIDTH * 0.8;

interface SelfieUploadStepProps {
    bleClient: BleUartClient;
    onCancel: () => void;
    onComplete: () => void;
}

export default function SelfieUploadStep({ bleClient, onCancel, onComplete }: SelfieUploadStepProps) {
    const [permission, requestPermission] = useCameraPermissions();
    const cameraRef = useRef<CameraView>(null);
    const [capturedImage, setCapturedImage] = useState<string | null>(null);
    const [loading, setLoading] = useState(false);
    const [loadingMessage, setLoadingMessage] = useState('');

    if (!permission) return <View />;
    if (!permission.granted) {
        return (
            <View style={styles.center}>
                <ThemedText style={styles.textCenter}>We need camera access</ThemedText>
                <TouchableOpacity onPress={requestPermission} style={styles.button}>
                    <Text style={styles.buttonText}>Grant Permission</Text>
                </TouchableOpacity>
            </View>
        );
    }

    const takePicture = async () => {
        if (cameraRef.current) {
            const photo = await cameraRef.current.takePictureAsync({
                quality: 0.4,
                skipProcessing: true
            });
            setCapturedImage(photo?.uri || null);
        }
    };

    const processAndUpload = async () => {
        if (!capturedImage) return;
        setLoading(true);
        try {
            setLoadingMessage('Processing...');
            
            // 1. Compress
            const manipulated = await ImageManipulator.manipulateAsync(
                capturedImage,
                [{ resize: { width: 400 } }], // Smaller width for speed
                { compress: 0.5, format: ImageManipulator.SaveFormat.JPEG, base64: true }
            );

            if (!manipulated.base64) throw new Error("Image processing failed");

            // 2. AES Encrypt Image
            setLoadingMessage('Encrypting...');
            const secretKey = CryptoJS.lib.WordArray.random(32).toString();
            const encryptedImg = CryptoJS.AES.encrypt(manipulated.base64, secretKey).toString();

            // 3. Upload to Supabase
            setLoadingMessage('Uploading...');
            const fileName = `selfie_${Date.now()}.txt`;
            const arrayBuffer = new Uint8Array(encryptedImg.length);
            for (let i = 0; i < encryptedImg.length; i++) arrayBuffer[i] = encryptedImg.charCodeAt(i);

            const { error } = await supabase.storage.from('wayside-transfers').upload(fileName, arrayBuffer.buffer, { contentType: 'text/plain' });
            if (error) throw error;

            const { data: urlData } = supabase.storage.from('wayside-transfers').getPublicUrl(fileName);

            // 4. Encrypt Key via RSA & Send via BLE
            setLoadingMessage('Linking...');
            const partnerKey = await SecureStore.getItemAsync(PARTNER_KEY_STORAGE);
            if (!partnerKey) {
                Alert.alert('No Partner', 'No partner key found.');
                return;
            }

            // === FIX STARTS HERE ===

            // 1. Encrypt ONLY the Secret Key (64 bytes). 
            // This fits easily inside the 117-byte limit of a 1024-bit RSA key.
            const encryptedKey = await RSA.encrypt(result.key, partnerKey);

            // 2. Combine the PLAIN URL and the ENCRYPTED KEY.
            // Format: PUBLIC_URL|RSA_ENCRYPTED_KEY
            // Note: The recipient (badge) needs to split this string by '|', 
            // use the URL as is, and decrypt the second part to get the AES key.
            let payload = `${result.publicUrl}|${encryptedKey}`;
            payload = payload.replace(/\r\n/g, '\n').replace(/\n/g, '\n');
            // 3. Send via BLE
            // Ensure your BLE client handles long messages (chunking) if this exceeds ~20-500 bytes
            await sendAndWaitForAck(bleClient, `ENC_URL:${payload}`, 'ENC_URL_OK');

            // 5. Done - Return to Dashboard immediately
            onComplete();

        } catch (err: any) {
            Alert.alert('Error', err.message);
        } finally {
            setLoading(false);
        }
    };

    if (loading) {
        return (
            <View style={styles.center}>
                <ActivityIndicator size="large" color="#2196F3" />
                <ThemedText style={{ marginTop: 20 }}>{loadingMessage}</ThemedText>
            </View>
        );
    }

    return (
        <View style={styles.container}>
            <ThemedText type="subtitle" style={styles.title}>
                {capturedImage ? 'Send this one?' : 'Show where you are'}
            </ThemedText>

            <View style={styles.cameraContainer}>
                {capturedImage ? (
                    <Image source={{ uri: capturedImage }} style={styles.camera} />
                ) : (
                    <CameraView ref={cameraRef} style={styles.camera} facing="front" />
                )}
            </View>

            <View style={styles.controls}>
                {capturedImage ? (
                    <>
                        <TouchableOpacity onPress={processAndUpload} style={styles.button}>
                            <Text style={styles.buttonText}>Send Selfie</Text>
                        </TouchableOpacity>
                        <TouchableOpacity onPress={() => setCapturedImage(null)} style={styles.textLink}>
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
    center: { flex: 1, alignItems: 'center', justifyContent: 'center' },
    title: { marginBottom: 20 },
    textCenter: { textAlign: 'center', marginBottom: 20 },
    cameraContainer: {
        width: CAMERA_SIZE,
        height: CAMERA_SIZE,
        borderRadius: CAMERA_SIZE / 2,
        overflow: 'hidden',
        borderWidth: 5,
        borderColor: '#eee',
        marginBottom: 30,
        backgroundColor: '#000'
    },
    camera: { flex: 1 },
    controls: { alignItems: 'center', gap: 15, width: '100%' },
    button: {
        backgroundColor: '#673AB7',
        paddingVertical: 14,
        paddingHorizontal: 40,
        borderRadius: 30,
        width: 200,
        alignItems: 'center'
    },
    buttonText: { color: '#fff', fontSize: 16, fontWeight: 'bold' },
    captureBtn: {
        width: 70, height: 70, borderRadius: 35,
        borderWidth: 4, borderColor: '#ccc',
        alignItems: 'center', justifyContent: 'center'
    },
    captureBtnInner: {
        width: 56, height: 56, borderRadius: 28, backgroundColor: '#F44336'
    },
    textLink: { padding: 10 },
});