import React, { useEffect, useState, useRef } from 'react';
import { View, ScrollView, StyleSheet, TouchableOpacity, Text } from 'react-native';
import { BleUartClient } from '@/utils/ble-uart';
import { ThemedText } from '@/components/themed-text';

interface DashboardStepProps {
  bleClient: BleUartClient;
  onStartUpload: () => void; // <--- New Prop
}

export default function DashboardStep({ bleClient, onStartUpload }: DashboardStepProps) {
  const [logs, setLogs] = useState<string[]>([]);
  const scrollViewRef = useRef<ScrollView>(null);

  useEffect(() => {
    // Start passive listening
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
      // Cleanup handled by parent unmount or keep listening
    };
  }, [bleClient]);

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
  logText: { color: '#00ff00', fontFamily: 'monospace', fontSize: 12, marginBottom: 4 }
});