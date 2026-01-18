/**
 * SetupScreen.tsx - Example showing KeyExchangeStep in the app flow
 * 
 * The flow is:
 * 1. PairingStep - NFC tap → BLE connection with OTP passkey
 * 2. KeyExchangeStep - RSA key generation and exchange (after BLE connected)
 * 3. Main app / dashboard
 */

import React, { useState } from 'react';
import { View, StyleSheet } from 'react-native';
import { BleUartClient } from '@/utils/ble-uart';
import PairingStep from '@/components/PairingStep';
import KeyExchangeStep from '@/components/KeyExchangeStep';

type SetupPhase = 'pairing' | 'key_exchange' | 'complete';

export default function SetupScreen() {
  const [phase, setPhase] = useState<SetupPhase>('pairing');
  const [bleClient, setBleClient] = useState<BleUartClient | null>(null);

  /**
   * Called when BLE pairing completes successfully
   */
  const handlePairingComplete = (client: BleUartClient) => {
    console.log('BLE pairing complete, moving to key exchange');
    setBleClient(client);
    setPhase('key_exchange');
  };

  /**
   * Called when key exchange completes
   */
  const handleKeyExchangeComplete = () => {
    console.log('Key exchange complete, setup finished!');
    setPhase('complete');
    // Navigate to main app screen
    // navigation.replace('Dashboard');
  };

  /**
   * Called if user skips key exchange (already has keys)
   */
  const handleKeyExchangeSkip = () => {
    console.log('Key exchange skipped (using existing keys)');
    setPhase('complete');
  };

  return (
    <View style={styles.container}>
      {phase === 'pairing' && (
        <PairingStep
          onPairingComplete={handlePairingComplete}
        />
      )}

      {phase === 'key_exchange' && bleClient && (
        <KeyExchangeStep
          bleClient={bleClient}
          onComplete={handleKeyExchangeComplete}
          onSkip={handleKeyExchangeSkip}
        />
      )}

      {phase === 'complete' && (
        <View style={styles.complete}>
          {/* Show success or navigate away */}
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    padding: 16,
  },
  complete: {
    flex: 1,
    justifyContent: 'center',
    alignItems: 'center',
  },
});
