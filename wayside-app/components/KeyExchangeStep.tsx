/**
 * KeyExchangeStep.tsx - RSA key exchange (standalone, after BLE pairing)
 * 
 * This step handles the RSA public/private key exchange with the device.
 * It runs AFTER BLE pairing is complete (separate from authentication).
 * 
 * Flow:
 * 1. Check if we already have keys stored
 * 2. If not, generate new RSA keypair
 * 3. Send public key to device
 * 4. Wait for acknowledgment
 */

import React, { useState, useEffect } from 'react';
import {
  View,
  Text,
  TouchableOpacity,
  ActivityIndicator,
  StyleSheet,
  Alert,
} from 'react-native';
import * as SecureStore from 'expo-secure-store';
import { RSA } from 'react-native-rsa-native';
import { BleUartClient } from '@/utils/ble-uart';
import { sendAndWaitForAck } from '@/utils/ble-helpers';
import { ThemedText } from '@/components/themed-text';

// Storage keys
const KEYPAIR_STORAGE_KEY = 'rsa_keypair';
const DEVICE_PUBKEY_STORAGE_KEY = 'device_public_key';

// RSA key size (1024 is fast, 2048 for more security)
const RSA_KEY_SIZE = 1024;

interface KeyExchangeStepProps {
  bleClient: BleUartClient;
  onComplete: () => void;
  onSkip?: () => void;  // Optional skip (if keys already exist)
}

type ExchangeStatus =
  | 'checking'
  | 'idle'
  | 'generating'
  | 'sending'
  | 'complete'
  | 'error';

interface StoredKeypair {
  publicKey: string;
  privateKey: string;
  timestamp: number;
}

export default function KeyExchangeStep({
  bleClient,
  onComplete,
  onSkip,
}: KeyExchangeStepProps) {
  const [status, setStatus] = useState<ExchangeStatus>('checking');
  const [hasExistingKeys, setHasExistingKeys] = useState(false);
  const [errorMessage, setErrorMessage] = useState('');

  useEffect(() => {
    checkExistingKeys();
  }, []);

  /**
   * Check if we already have keys stored
   */
  const checkExistingKeys = async () => {
    try {
      const stored = await SecureStore.getItemAsync(KEYPAIR_STORAGE_KEY);
      if (stored) {
        const keypair: StoredKeypair = JSON.parse(stored);
        if (keypair.publicKey && keypair.privateKey) {
          setHasExistingKeys(true);
        }
      }
    } catch (error) {
      console.log('No existing keys found');
    }
    setStatus('idle');
  };

  /**
   * Generate RSA keypair and send public key to device
   */
  const performKeyExchange = async () => {
    setStatus('generating');
    setErrorMessage('');

    try {
      // Generate RSA keypair
      console.log(`Generating ${RSA_KEY_SIZE}-bit RSA keypair...`);
      const keys = await RSA.generateKeys(RSA_KEY_SIZE);

      // Store keypair securely
      const keypair: StoredKeypair = {
        publicKey: keys.public,
        privateKey: keys.private,
        timestamp: Date.now(),
      };
      await SecureStore.setItemAsync(KEYPAIR_STORAGE_KEY, JSON.stringify(keypair));

      // Send public key to device
      setStatus('sending');

      // Normalize line endings for cross-platform compatibility
      const normalizedPublicKey = keys.public
        .replace(/\r\n/g, '\n')
        .replace(/\r/g, '\n');

      // Send and wait for acknowledgment
      await sendAndWaitForAck(
        bleClient,
        `PUBKEY:${normalizedPublicKey}`,
        'PUBKEY_OK',
        10000 // 10 second timeout
      );

      console.log('Key exchange complete!');
      setStatus('complete');

      // Brief delay to show success state
      await new Promise(resolve => setTimeout(resolve, 500));
      
      onComplete();

    } catch (error: any) {
      console.error('Key exchange error:', error);
      setErrorMessage(error.message || 'Key exchange failed');
      setStatus('error');
    }
  };

  /**
   * Re-send existing public key to device
   */
  const resendPublicKey = async () => {
    setStatus('sending');
    setErrorMessage('');

    try {
      const stored = await SecureStore.getItemAsync(KEYPAIR_STORAGE_KEY);
      if (!stored) {
        throw new Error('No keypair found - please generate new keys');
      }

      const keypair: StoredKeypair = JSON.parse(stored);
      const normalizedPublicKey = keypair.publicKey
        .replace(/\r\n/g, '\n')
        .replace(/\r/g, '\n');

      await sendAndWaitForAck(
        bleClient,
        `PUBKEY:${normalizedPublicKey}`,
        'PUBKEY_OK',
        10000
      );

      setStatus('complete');
      await new Promise(resolve => setTimeout(resolve, 500));
      onComplete();

    } catch (error: any) {
      console.error('Resend error:', error);
      setErrorMessage(error.message || 'Failed to send key');
      setStatus('error');
    }
  };

  /**
   * Clear stored keys and generate new ones
   */
  const regenerateKeys = async () => {
    try {
      await SecureStore.deleteItemAsync(KEYPAIR_STORAGE_KEY);
      setHasExistingKeys(false);
      await performKeyExchange();
    } catch (error: any) {
      console.error('Regenerate error:', error);
      setErrorMessage(error.message);
      setStatus('error');
    }
  };

  /**
   * Skip key exchange (use existing keys)
   */
  const handleSkip = () => {
    if (onSkip) {
      onSkip();
    } else {
      onComplete();
    }
  };

  // === Loading States ===
  if (status === 'checking') {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={styles.statusText}>Checking for existing keys...</ThemedText>
      </View>
    );
  }

  if (status === 'generating') {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={styles.statusText}>Generating RSA keypair...</ThemedText>
        <ThemedText style={styles.hint}>This may take a few seconds</ThemedText>
      </View>
    );
  }

  if (status === 'sending') {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={styles.statusText}>Sending public key to device...</ThemedText>
      </View>
    );
  }

  if (status === 'complete') {
    return (
      <View style={styles.center}>
        <Text style={styles.successIcon}>✓</Text>
        <ThemedText style={styles.statusText}>Key exchange complete!</ThemedText>
      </View>
    );
  }

  // === Error State ===
  if (status === 'error') {
    return (
      <View style={styles.container}>
        <View style={styles.errorBox}>
          <Text style={styles.errorIcon}>⚠️</Text>
          <ThemedText style={styles.errorText}>{errorMessage}</ThemedText>
        </View>
        
        <TouchableOpacity style={styles.button} onPress={performKeyExchange}>
          <Text style={styles.buttonText}>Try Again</Text>
        </TouchableOpacity>
        
        {hasExistingKeys && (
          <TouchableOpacity style={styles.secondaryButton} onPress={handleSkip}>
            <Text style={styles.secondaryButtonText}>Skip (Use Existing Keys)</Text>
          </TouchableOpacity>
        )}
      </View>
    );
  }

  // === Idle State ===
  return (
    <View style={styles.container}>
      <ThemedText type="title" style={styles.title}>
        Secure Key Exchange
      </ThemedText>

      <ThemedText style={styles.description}>
        To secure communication with your device, we need to exchange encryption keys.
        This only needs to be done once.
      </ThemedText>

      {hasExistingKeys ? (
        <>
          <View style={styles.infoBox}>
            <ThemedText style={styles.infoText}>
              ✓ You already have encryption keys stored
            </ThemedText>
          </View>

          <TouchableOpacity style={styles.button} onPress={resendPublicKey}>
            <Text style={styles.buttonText}>Send Key to Device</Text>
          </TouchableOpacity>

          <TouchableOpacity style={styles.secondaryButton} onPress={handleSkip}>
            <Text style={styles.secondaryButtonText}>Skip</Text>
          </TouchableOpacity>

          <TouchableOpacity style={styles.linkButton} onPress={regenerateKeys}>
            <Text style={styles.linkButtonText}>Generate New Keys</Text>
          </TouchableOpacity>
        </>
      ) : (
        <>
          <TouchableOpacity style={styles.button} onPress={performKeyExchange}>
            <Text style={styles.buttonText}>Generate & Exchange Keys</Text>
          </TouchableOpacity>

          <ThemedText style={styles.hint}>
            This generates a unique encryption key pair for secure messaging
          </ThemedText>
        </>
      )}
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
    fontSize: 22,
    fontWeight: 'bold',
    textAlign: 'center',
    marginBottom: 16,
  },
  description: {
    fontSize: 15,
    color: '#666',
    textAlign: 'center',
    marginBottom: 24,
    lineHeight: 22,
  },
  statusText: {
    marginTop: 16,
    fontSize: 16,
    textAlign: 'center',
  },
  hint: {
    fontSize: 13,
    color: '#888',
    textAlign: 'center',
    marginTop: 8,
  },
  button: {
    backgroundColor: '#2196F3',
    padding: 16,
    borderRadius: 8,
    alignItems: 'center',
    marginTop: 12,
  },
  buttonText: {
    color: '#fff',
    fontWeight: 'bold',
    fontSize: 16,
  },
  secondaryButton: {
    backgroundColor: '#f5f5f5',
    padding: 14,
    borderRadius: 8,
    alignItems: 'center',
    marginTop: 12,
    borderWidth: 1,
    borderColor: '#ddd',
  },
  secondaryButtonText: {
    color: '#666',
    fontWeight: '600',
    fontSize: 15,
  },
  linkButton: {
    padding: 12,
    alignItems: 'center',
    marginTop: 16,
  },
  linkButtonText: {
    color: '#2196F3',
    fontSize: 14,
  },
  infoBox: {
    backgroundColor: '#e8f5e9',
    padding: 16,
    borderRadius: 8,
    marginBottom: 16,
  },
  infoText: {
    color: '#2e7d32',
    textAlign: 'center',
  },
  errorBox: {
    backgroundColor: '#ffebee',
    padding: 16,
    borderRadius: 8,
    marginBottom: 20,
    alignItems: 'center',
  },
  errorIcon: {
    fontSize: 32,
    marginBottom: 8,
  },
  errorText: {
    color: '#c62828',
    textAlign: 'center',
  },
  successIcon: {
    fontSize: 48,
    color: '#4caf50',
    marginBottom: 8,
  },
});
