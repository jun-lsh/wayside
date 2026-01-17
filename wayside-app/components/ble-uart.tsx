import React, { useState, useEffect, useRef } from 'react';
import {
  View,
  Text,
  TextInput,
  TouchableOpacity,
  ScrollView,
  StyleSheet,
  ActivityIndicator,
  Alert,
  Platform,
} from 'react-native';
import { BleUartClient, BleUartConfig } from '@/utils/ble-uart';

interface BleUartProps {
  deviceName?: string;
  config?: Partial<BleUartConfig>;
}

export default function BleUart({ deviceName, config }: BleUartProps) {
  const [isConnected, setIsConnected] = useState(false);
  const [isConnecting, setIsConnecting] = useState(false);
  const [isScanning, setIsScanning] = useState(false);
  const [message, setMessage] = useState('');
  const [receivedMessages, setReceivedMessages] = useState<string[]>([]);
  const [mtu, setMtu] = useState<number>(23);
  const [deviceId, setDeviceId] = useState<string>('');
  const bleClientRef = useRef<BleUartClient | null>(null);

  useEffect(() => {
    // Initialize BLE client
    const client = new BleUartClient({
      deviceName: deviceName || 'ESP-BLE-1',
      ...config,
    });
    bleClientRef.current = client;

    // Initialize BLE manager
    client
      .initialize()
      .then(() => {
        console.log('BLE initialized');
      })
      .catch((error) => {
        console.error('BLE initialization error:', error);
        Alert.alert('Bluetooth Error', error.message);
      });

    // Cleanup on unmount
    return () => {
      if (bleClientRef.current) {
        bleClientRef.current.destroy();
      }
    };
  }, [deviceName, config]);

  const handleConnect = async () => {
    if (!bleClientRef.current) return;

    setIsConnecting(true);
    setIsScanning(true);

    try {
      const device = await bleClientRef.current.connectByName(deviceName);
      setIsConnected(true);
      setMtu(bleClientRef.current.getMTU());
      setDeviceId(device.id);

      // Start receiving notifications
      bleClientRef.current.startNotifications(
        (receivedMessage) => {
          setReceivedMessages((prev) => [
            ...prev,
            `[${new Date().toLocaleTimeString()}] ${receivedMessage}`,
          ]);
        },
        (error) => {
          console.error('Notification error:', error);
          Alert.alert('Notification Error', error.message);
        }
      );

      Alert.alert('Connected', `Connected to ${device.name || deviceId}`);
    } catch (error: any) {
      console.error('Connection error:', error);
      Alert.alert('Connection Failed', error.message || 'Failed to connect to device');
      setIsConnected(false);
    } finally {
      setIsConnecting(false);
      setIsScanning(false);
    }
  };

  const handleDisconnect = async () => {
    if (!bleClientRef.current) return;

    try {
      await bleClientRef.current.disconnect();
      setIsConnected(false);
      setDeviceId('');
      setReceivedMessages([]);
      Alert.alert('Disconnected', 'Disconnected from device');
    } catch (error: any) {
      console.error('Disconnect error:', error);
      Alert.alert('Error', error.message || 'Failed to disconnect');
    }
  };

  const handleSend = async () => {
    if (!bleClientRef.current || !message.trim()) return;

    if (!isConnected) {
      Alert.alert('Not Connected', 'Please connect to a device first');
      return;
    }

    try {
      await bleClientRef.current.writeMessage(message, true);
      setMessage(''); // Clear input after sending
    } catch (error: any) {
      console.error('Send error:', error);
      Alert.alert('Send Failed', error.message || 'Failed to send message');
    }
  };

  const clearMessages = () => {
    setReceivedMessages([]);
  };

  return (
    <View style={styles.container}>
      {/* Connection Status */}
      <View style={styles.statusContainer}>
        <View style={styles.statusRow}>
          <View
            style={[
              styles.statusIndicator,
              { backgroundColor: isConnected ? '#4CAF50' : '#F44336' },
            ]}
          />
          <Text style={styles.statusText}>
            {isConnected ? 'Connected' : 'Disconnected'}
          </Text>
        </View>
        {isConnected && (
          <>
            <Text style={styles.statusInfo}>MTU: {mtu} bytes</Text>
            {deviceId && (
              <Text style={styles.statusInfo} numberOfLines={1}>
                ID: {deviceId}
              </Text>
            )}
          </>
        )}
      </View>

      {/* Connect/Disconnect Button */}
      <TouchableOpacity
        style={[
          styles.button,
          isConnected ? styles.disconnectButton : styles.connectButton,
          (isConnecting || isScanning) && styles.buttonDisabled,
        ]}
        onPress={isConnected ? handleDisconnect : handleConnect}
        disabled={isConnecting || isScanning}
      >
        {isConnecting || isScanning ? (
          <ActivityIndicator color="#FFF" />
        ) : (
          <Text style={styles.buttonText}
            numberOfLines={1}
            adjustsFontSizeToFit
            minimumFontScale={0.8}>
            {isConnected ? 'Disconnect' : 'Scan & Connect'}
          </Text>
        )}
      </TouchableOpacity>

      {/* Message Input */}
      <View style={styles.inputContainer}>
        <TextInput
          style={styles.input}
          value={message}
          onChangeText={setMessage}
          placeholder="Enter message to send..."
          placeholderTextColor="#999"
          multiline
          editable={isConnected}
        />
        <TouchableOpacity
          style={[
            styles.sendButton,
            (!isConnected || !message.trim()) && styles.sendButtonDisabled,
          ]}
          onPress={handleSend}
          disabled={!isConnected || !message.trim()}
        >
          <Text style={styles.sendButtonText}
            numberOfLines={1}
            adjustsFontSizeToFit
            minimumFontScale={0.8}
          >Send</Text>
        </TouchableOpacity>
      </View>

      {/* Received Messages */}
      <View style={styles.messagesContainer}>
        <View style={styles.messagesHeader}>
          <Text style={styles.messagesTitle}>Received Messages</Text>
          {receivedMessages.length > 0 && (
            <TouchableOpacity onPress={clearMessages}>
              <Text style={styles.clearButton}>Clear</Text>
            </TouchableOpacity>
          )}
        </View>
        <ScrollView
          style={styles.messagesScrollView}
          contentContainerStyle={styles.messagesContent}
        >
          {receivedMessages.length === 0 ? (
            <Text style={styles.emptyMessage}>No messages received yet</Text>
          ) : (
            receivedMessages.map((msg, index) => (
              <Text key={index} style={styles.messageItem}>
                {msg}
              </Text>
            ))
          )}
        </ScrollView>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    padding: 16,
    backgroundColor: '#F5F5F5',
  },
  statusContainer: {
    backgroundColor: '#FFF',
    padding: 16,
    borderRadius: 8,
    marginBottom: 16,
    ...Platform.select({
      ios: {
        shadowColor: '#000',
        shadowOffset: { width: 0, height: 2 },
        shadowOpacity: 0.1,
        shadowRadius: 4,
      },
      android: {
        elevation: 2,
      },
    }),
  },
  statusRow: {
    flexDirection: 'row',
    alignItems: 'center',
    marginBottom: 8,
  },
  statusIndicator: {
    width: 12,
    height: 12,
    borderRadius: 6,
    marginRight: 8,
  },
  statusText: {
    fontSize: 16,
    fontWeight: '600',
    color: '#333',
  },
  statusInfo: {
    fontSize: 12,
    color: '#666',
    marginTop: 4,
  },
  button: {
    padding: 16,
    borderRadius: 8,
    alignItems: 'center',
    marginBottom: 16,
  },
  connectButton: {
    backgroundColor: '#2196F3',
  },
  disconnectButton: {
    backgroundColor: '#F44336',
  },
  buttonDisabled: {
    opacity: 0.6,
  },
  buttonText: {
    color: '#FFF',
    fontSize: 16,
    fontWeight: '600',
    textAlign: 'center'
  },
  inputContainer: {
    marginBottom: 16,
  },
  input: {
    backgroundColor: '#FFF',
    borderWidth: 1,
    borderColor: '#DDD',
    borderRadius: 8,
    padding: 12,
    minHeight: 80,
    maxHeight: 120,
    fontSize: 14,
    color: '#333',
    textAlignVertical: 'top',
    marginBottom: 8,
  },
  sendButton: {
    backgroundColor: '#4CAF50',
    padding: 12,
    borderRadius: 8,
    alignItems: 'center',
  },
  sendButtonDisabled: {
    backgroundColor: '#CCC',
  },
  sendButtonText: {
    color: '#FFF',
    fontSize: 16,
    fontWeight: '600',
    textAlign: 'center'
  },
  messagesContainer: {
    flex: 1,
    backgroundColor: '#FFF',
    borderRadius: 8,
    padding: 16,
    ...Platform.select({
      ios: {
        shadowColor: '#000',
        shadowOffset: { width: 0, height: 2 },
        shadowOpacity: 0.1,
        shadowRadius: 4,
      },
      android: {
        elevation: 2,
      },
    }),
  },
  messagesHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: 12,
  },
  messagesTitle: {
    fontSize: 16,
    fontWeight: '600',
    color: '#333',
  },
  clearButton: {
    color: '#2196F3',
    fontSize: 14,
  },
  messagesScrollView: {
    flex: 1,
  },
  messagesContent: {
    paddingBottom: 8,
  },
  messageItem: {
    fontSize: 14,
    color: '#333',
    marginBottom: 8,
    padding: 8,
    backgroundColor: '#F5F5F5',
    borderRadius: 4,
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
  },
  emptyMessage: {
    fontSize: 14,
    color: '#999',
    textAlign: 'center',
    marginTop: 32,
  },
});
