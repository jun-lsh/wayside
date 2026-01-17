import React, { useState } from 'react';
import { View, Text, TouchableOpacity, StyleSheet, Alert, ActivityIndicator, ScrollView } from 'react-native';
import { BleUartClient } from '@/utils/ble-uart';
import { sendAndWaitForAck } from '@/utils/ble-helpers';
import { ThemedText } from '@/components/themed-text';

// Define your bits structure (Max 16 items for 2 bytes)
const SECTIONS = [
  {
    title: 'Languages',
    options: [
      { id: 0, label: 'C' },
      { id: 1, label: 'Python' },
      { id: 2, label: 'Rust' },
      { id: 3, label: 'Java' },
    ]
  },
  {
    title: 'Domains',
    options: [
      { id: 4, label: 'AI/ML' },
      { id: 5, label: 'Cybersecurity' },
      { id: 6, label: 'Embedded' },
      { id: 7, label: 'Cloud' },
    ]
  }
];

interface InterestsStepProps {
  bleClient: BleUartClient;
  onComplete: () => void;
}

export default function InterestsStep({ bleClient, onComplete }: InterestsStepProps) {
  const [selectedBits, setSelectedBits] = useState<Set<number>>(new Set());
  const [isSending, setIsSending] = useState(false);

  const toggleOption = (id: number) => {
    const next = new Set(selectedBits);
    if (next.has(id)) next.delete(id);
    else next.add(id);
    setSelectedBits(next);
  };

  const handleConfirm = async () => {
    setIsSending(true);
    try {
      // 1. Construct Bitmask (16-bit integer)
      let mask = 0;
      selectedBits.forEach(bitIndex => {
        mask |= (1 << bitIndex);
      });

      // 2. Convert to Hex Bytes (High Byte, Low Byte)
      // Example: mask = 258 (0x0102) -> High: 01, Low: 02
      const highByte = (mask >> 8) & 0xFF;
      const lowByte = mask & 0xFF;

      // Format as Hex string to be safe over UART
      const hexString = [highByte, lowByte]
        .map(b => b.toString(16).padStart(2, '0').toUpperCase())
        .join('');
      
      // Sending: BITMASK:A1B2 (Example)
      const command = `BITMASK:16:${hexString}`;
      
      console.log(`Sending bitmask: ${mask} as ${command}`);

      // 3. Send and Wait for ACK
      await sendAndWaitForAck(bleClient, command, 'BITMASK_OK');

      onComplete();
    } catch (error: any) {
      Alert.alert('Error', 'Failed to send configuration: ' + error.message);
    } finally {
      setIsSending(false);
    }
  };

  if (isSending) {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={{ marginTop: 20 }}>Configuring Device...</ThemedText>
      </View>
    );
  }

  return (
    <ScrollView>
      <ThemedText type="title" style={styles.title}>Select Interests</ThemedText>
      
      {SECTIONS.map((section) => (
        <View key={section.title} style={styles.section}>
          <ThemedText type="subtitle" style={styles.sectionHeader}>{section.title}</ThemedText>
          <View style={styles.optionsGrid}>
            {section.options.map((opt) => {
              const isSelected = selectedBits.has(opt.id);
              return (
                <TouchableOpacity
                  key={opt.id}
                  style={[styles.optionChip, isSelected && styles.optionChipSelected]}
                  onPress={() => toggleOption(opt.id)}
                >
                  <Text style={[styles.optionText, isSelected && styles.optionTextSelected]}>
                    {opt.label}
                  </Text>
                </TouchableOpacity>
              );
            })}
          </View>
        </View>
      ))}

      <TouchableOpacity style={styles.button} onPress={handleConfirm}>
        <Text style={styles.buttonText}>Save Configuration</Text>
      </TouchableOpacity>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  center: { alignItems: 'center', justifyContent: 'center', padding: 50 },
  title: { fontSize: 22, fontWeight: 'bold', marginBottom: 20, textAlign: 'center' },
  section: { marginBottom: 20 },
  sectionHeader: { fontSize: 18, fontWeight: '600', marginBottom: 10 },
  optionsGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 10 },
  optionChip: {
    paddingVertical: 8, paddingHorizontal: 16,
    borderRadius: 20, borderWidth: 1, borderColor: '#ccc',
    backgroundColor: '#f5f5f5'
  },
  optionChipSelected: { backgroundColor: '#2196F3', borderColor: '#2196F3' },
  optionText: { color: '#333' },
  optionTextSelected: { color: '#fff', fontWeight: 'bold' },
  button: { backgroundColor: '#4CAF50', padding: 15, borderRadius: 8, alignItems: 'center', marginTop: 20 },
  buttonText: { color: '#fff', fontSize: 16, fontWeight: 'bold' },
});