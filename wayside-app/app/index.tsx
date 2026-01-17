import React, { useState, useEffect, useRef } from 'react';
import { StyleSheet } from 'react-native';
import { ThemedView } from '@/components/themed-view';
import { BleUartClient } from '@/utils/ble-uart';

// Components
import PairingStep from '../components/PairingStep';
import InterestsStep from '../components/InterestsStep';
import DashboardStep from '../components/DashboardStep';
import SelfieUploadStep from '../components/SelfieUploadStep'; // <--- Import

// Updated Types
type AppStep = 'PAIRING' | 'INTERESTS' | 'DASHBOARD' | 'SELFIE_UPLOAD';

export default function App() {
  const [currentStep, setCurrentStep] = useState<AppStep>('PAIRING');
  
  const bleClientRef = useRef<BleUartClient>(new BleUartClient());

  useEffect(() => {
    const client = bleClientRef.current;
    client.initialize().then(() => console.log('BLE Init OK')).catch(console.error);
    return () => {
      client.destroy();
    };
  }, []);

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
            onStartUpload={() => setCurrentStep('SELFIE_UPLOAD')} // <--- Switch to new step
          />
        );
      case 'SELFIE_UPLOAD':
        return (
          <SelfieUploadStep 
            onCancel={() => setCurrentStep('DASHBOARD')} // <--- Return to dashboard
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