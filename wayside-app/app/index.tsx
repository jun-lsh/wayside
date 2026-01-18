import 'react-native-get-random-values';
import React, { useState, useEffect, useRef } from 'react';
import { StyleSheet } from 'react-native';
import { ThemedView } from '@/components/themed-view';
import { BleUartClient } from '@/utils/ble-uart';
import * as SecureStore from 'expo-secure-store';

// Components
import PairingStep from '../components/PairingStep';
import InterestsStep from '../components/InterestsStep';
import DashboardStep from '../components/DashboardStep';
import SelfieUploadStep from '../components/SelfieUploadStep';

const PARTNER_KEY_STORAGE = 'partner_public_key';

type AppStep = 'PAIRING' | 'INTERESTS' | 'DASHBOARD' | 'SELFIE_UPLOAD';

export default function App() {
  const [currentStep, setCurrentStep] = useState<AppStep>('PAIRING');
  const [selfieUploadComplete, setSelfieUploadComplete] = useState(false);
  
  // Keep BLE Client alive throughout the app lifecycle
  const bleClientRef = useRef<BleUartClient>(new BleUartClient());

  useEffect(() => {
    const client = bleClientRef.current;
    client.initialize()
      .then(() => console.log('BLE Init OK'))
      .catch(console.error);
    
    return () => { client.destroy(); };
  }, []);

  // Reset function to go back to scanning mode
  const handleReset = async () => {
    // Clear stored partner data so we can find a new one or the same one again
    await SecureStore.deleteItemAsync(PARTNER_KEY_STORAGE);
    
    // Reset State
    setSelfieUploadComplete(false);
    
    // In a real scenario, you might want to tell the Badge to reset too
    // bleClientRef.current.write('RESET'); 
    
    setCurrentStep('DASHBOARD'); // Should trigger the "Searching..." view
  };

  const renderContent = () => {
    switch (currentStep) {
      case 'PAIRING':
        return (
          <PairingStep 
            bleClient={bleClientRef.current} 
            onComplete={() => setCurrentStep('INTERESTS')} 
          />
        );
      case 'INTERESTS':
        return (
          <InterestsStep 
            bleClient={bleClientRef.current} 
            onComplete={() => setCurrentStep('DASHBOARD')} 
          />
        );
      case 'DASHBOARD':
        return (
          <DashboardStep 
            bleClient={bleClientRef.current}
            onStartUpload={() => setCurrentStep('SELFIE_UPLOAD')}
            selfieUploadComplete={selfieUploadComplete}
            onReset={handleReset}
          />
        );
      case 'SELFIE_UPLOAD':
        return (
          <SelfieUploadStep 
            bleClient={bleClientRef.current}
            onCancel={() => setCurrentStep('DASHBOARD')}
            onComplete={() => {
              setSelfieUploadComplete(true);
              setCurrentStep('DASHBOARD');
            }}
          />
        );
      default:
        return null;
    }
  };

  return (
    <ThemedView style={styles.container}>
      {renderContent()}
    </ThemedView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    paddingTop: 60,
    paddingHorizontal: 20,
    paddingBottom: 20,
  },
});