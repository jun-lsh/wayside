import React, { useEffect, useState, useRef } from 'react';
import { View, ScrollView, StyleSheet, TextInput } from 'react-native';
import { BleUartClient } from '@/utils/ble-uart';
import { ThemedText } from '@/components/themed-text';

interface DashboardStepProps {
  bleClient: BleUartClient;
}

export default function DashboardStep({ bleClient }: DashboardStepProps) {
  const [logs, setLogs] = useState<string[]>([]);
  const scrollViewRef = useRef<ScrollView>(null);

  useEffect(() => {
    // Start passive listening
    // We assume the handshake/ack listeners from previous steps are removed
    // or overwritten by this call.
    bleClient.startNotifications(
      (msg) => {
        const timestamp = new Date().toLocaleTimeString();
        setLogs((prev) => [...prev, `[${timestamp}] ${msg}`]);
      },
      (err) => {
        setLogs((prev) => [...prev, `[ERROR] ${err.message}`]);
      }
    );

    return () => {
      // Optional: stop notifications if you leave this screen
      // bleClient.stopNotifications();
    };
  }, [bleClient]);

  return (
    <View style={styles.container}>
      <ThemedText type="title" style={styles.header}>Device Monitor</ThemedText>
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
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  header: { fontSize: 20, fontWeight: 'bold', marginBottom: 15, textAlign: 'center' },
  terminal: {
    flex: 1,
    backgroundColor: '#1e1e1e',
    borderRadius: 8,
    padding: 10,
    minHeight: 300,
  },
  placeholder: { color: '#666', fontStyle: 'italic', textAlign: 'center', marginTop: 20 },
  logText: { color: '#00ff00', fontFamily: 'monospace', fontSize: 12, marginBottom: 4 }
});