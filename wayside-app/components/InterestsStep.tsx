import React, { useEffect, useState } from 'react';
import { 
  View, 
  Text, 
  TouchableOpacity, 
  StyleSheet, 
  Alert, 
  ActivityIndicator, 
  ScrollView 
} from 'react-native';
import { BleUartClient } from '@/utils/ble-uart';
import { sendAndWaitForAck } from '@/utils/ble-helpers';
import { ThemedText } from '@/components/themed-text';
import { supabase } from '@/utils/supabase';

interface InterestsStepProps {
  bleClient: BleUartClient;
  onComplete: () => void;
}

// --- Types ---
interface EventItem {
  id: string;
  name: string;
  description: string;
}

interface Interest {
  id: string;
  name: string;
  bitIndex: number; // 0 to N
}

interface BucketSection {
  title: string;
  options: Interest[];
}

export default function InterestsStep({ bleClient, onComplete }: InterestsStepProps) {
  // State: Navigation & Data
  const [availableEvents, setAvailableEvents] = useState<EventItem[]>([]);
  const [selectedEvent, setSelectedEvent] = useState<EventItem | null>(null);
  const [sections, setSections] = useState<BucketSection[]>([]);
  
  // State: Selections
  const [selectedBits, setSelectedBits] = useState<Set<number>>(new Set());
  const [maxBitIndex, setMaxBitIndex] = useState(0); // Tracks total items for byte calculation

  // State: UI
  const [loading, setLoading] = useState(true);
  const [isSending, setIsSending] = useState(false);

  // 1. Load Events on Mount
  useEffect(() => {
    fetchEvents();
  }, []);

  const fetchEvents = async () => {
    try {
      const { data, error } = await supabase
        .from('events')
        .select('id, name, description')
        .order('created_at', { ascending: false });

      if (error) throw error;
      setAvailableEvents(data || []);
    } catch (err: any) {
      Alert.alert('Error fetching events', err.message);
    } finally {
      setLoading(false);
    }
  };

  // 2. Load Interests when an Event is picked
  const handleSelectEvent = async (event: EventItem) => {
    setSelectedEvent(event);
    setLoading(true);
    setSelectedBits(new Set()); // Reset selections
    setSections([]);

    try {
      const { data, error } = await supabase
        .from('event_buckets')
        .select(`
          id,
          name,
          bucket_interests (
            global_interests (
              id,
              name
            )
          )
        `)
        .eq('event_id', event.id)
        .order('name'); // Deterministic bucket order

      if (error) throw error;

      if (!data) return;

      let globalBitCounter = 0;

      const parsedSections: BucketSection[] = data.map((bucket: any) => {
        // Extract and sort interests
        const rawInterests = bucket.bucket_interests
          .map((bi: any) => bi.global_interests)
          .filter((i: any) => i !== null);
        
        // Deterministic sort by name
        rawInterests.sort((a: any, b: any) => a.name.localeCompare(b.name));

        const options = rawInterests.map((interest: any) => ({
          id: interest.id,
          name: interest.name,
          bitIndex: globalBitCounter++ // Assign 0, 1, 2...
        }));

        return {
          title: bucket.name,
          options: options
        };
      });

      setSections(parsedSections);
      setMaxBitIndex(globalBitCounter);

    } catch (err: any) {
      Alert.alert('Error fetching interests', err.message);
      setSelectedEvent(null); // Go back on error
    } finally {
      setLoading(false);
    }
  };

  const toggleOption = (bitIndex: number) => {
    const next = new Set(selectedBits);
    if (next.has(bitIndex)) next.delete(bitIndex);
    else next.add(bitIndex);
    setSelectedBits(next);
  };

  const handleConfirm = async () => {
    setIsSending(true);
    try {
      // 1. Calculate Byte Count
      // If we have 0 items, send 1 byte. If 9 items, (9/8 ceil) = 2 bytes.
      const totalBitsNeeded = maxBitIndex; 
      const byteCount = Math.max(1, Math.ceil(totalBitsNeeded / 8));

      // 2. Construct Bitmask using BigInt
      // We use BigInt because bitwise shifting 1 << 32 fails in standard JS numbers
      let mask = 0n;
      selectedBits.forEach(bitIndex => {
        mask |= (1n << BigInt(bitIndex));
      });

      // 3. Convert to Hex
      let hexString = mask.toString(16).toUpperCase();
      
      // Pad to the correct length (2 chars per byte)
      const expectedHexChars = byteCount * 2;
      hexString = hexString.padStart(expectedHexChars, '0');

      // 4. Construct Command
      // Format: BITMASK:[BYTE_COUNT]:[HEX_DATA]
      // Example: 20 bits -> 3 bytes -> BITMASK:3:000405
      const command = `BITMASK:${byteCount*8}:${hexString}`;
      
      console.log(`Sending: ${command} (Bits set: ${selectedBits.size}, Total options: ${maxBitIndex})`);

      await sendAndWaitForAck(bleClient, command, 'BITMASK_OK');
      onComplete();

    } catch (error: any) {
      Alert.alert('Error', 'Failed to send configuration: ' + error.message);
    } finally {
      setIsSending(false);
    }
  };

  // --- RENDER HELPERS ---

  if (loading || isSending) {
    return (
      <View style={styles.center}>
        <ActivityIndicator size="large" color="#2196F3" />
        <ThemedText style={{ marginTop: 20 }}>
          {isSending ? 'Configuring Device...' : 'Loading Data...'}
        </ThemedText>
      </View>
    );
  }

  // View 1: Event Selection List (Acts as "Dropdown")
  if (!selectedEvent) {
    return (
      <ScrollView contentContainerStyle={styles.container}>
        <ThemedText type="title" style={styles.title}>Select Event</ThemedText>
        <ThemedText style={styles.subtitle}>Choose an event to load configurations</ThemedText>
        
        {availableEvents.length === 0 ? (
          <ThemedText style={styles.emptyText}>No events found in database.</ThemedText>
        ) : (
          availableEvents.map((event) => (
            <TouchableOpacity 
              key={event.id} 
              style={styles.eventCard}
              onPress={() => handleSelectEvent(event)}
            >
              <ThemedText type="defaultSemiBold" style={{ color: '#000' }}>{event.name}</ThemedText>
              {event.description && (
                <ThemedText style={styles.eventDesc}>{event.description}</ThemedText>
              )}
            </TouchableOpacity>
          ))
        )}
      </ScrollView>
    );
  }

  // View 2: Interest Selection
  return (
    <ScrollView contentContainerStyle={styles.container}>
      <View style={styles.headerRow}>
        <TouchableOpacity onPress={() => setSelectedEvent(null)} style={styles.backButton}>
          <Text style={styles.backButtonText}>← Change Event</Text>
        </TouchableOpacity>
        <ThemedText type="subtitle" style={styles.eventTitle}>{selectedEvent.name}</ThemedText>
      </View>

      <ThemedText style={styles.instructionText}>
        Select options to sync with device
      </ThemedText>
      
      {sections.map((section) => (
        <View key={section.title} style={styles.section}>
          <ThemedText type="defaultSemiBold" style={styles.sectionHeader}>{section.title}</ThemedText>
          <View style={styles.optionsGrid}>
            {section.options.map((opt) => {
              const isSelected = selectedBits.has(opt.bitIndex);
              return (
                <TouchableOpacity
                  key={opt.id}
                  style={[styles.optionChip, isSelected && styles.optionChipSelected]}
                  onPress={() => toggleOption(opt.bitIndex)}
                >
                  <Text style={[styles.optionText, isSelected && styles.optionTextSelected]}>
                    {opt.name}
                  </Text>
                </TouchableOpacity>
              );
            })}
          </View>
        </View>
      ))}

      {sections.length === 0 && (
        <ThemedText style={styles.emptyText}>No interests configured for this event.</ThemedText>
      )}

      <TouchableOpacity 
        style={[styles.button, sections.length === 0 && styles.buttonDisabled]} 
        onPress={handleConfirm}
        disabled={sections.length === 0}
      >
        <Text style={styles.buttonText}>Save to Device</Text>
      </TouchableOpacity>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { padding: 20, paddingBottom: 60 },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center' },
  title: { fontSize: 24, textAlign: 'center', marginBottom: 10 },
  subtitle: { textAlign: 'center', color: '#666', marginBottom: 20 },
  
  // Event Card Styles
  eventCard: {
    backgroundColor: '#fff',
    padding: 16,
    borderRadius: 12,
    marginBottom: 12,
    borderWidth: 1,
    borderColor: '#eee',
    elevation: 2,
    shadowColor: '#000',
    shadowOpacity: 0.1,
    shadowRadius: 4,
    shadowOffset: { width: 0, height: 2 }
  },
  eventDesc: { fontSize: 12, color: '#666', marginTop: 4 },

  // Interest Styles
  headerRow: { flexDirection: 'row', alignItems: 'center', marginBottom: 20, justifyContent: 'space-between' },
  backButton: { padding: 8 },
  backButtonText: { color: '#2196F3', fontSize: 14 },
  eventTitle: { flex: 1, textAlign: 'right', fontSize: 18 },
  instructionText: { textAlign: 'center', marginBottom: 20, color: '#666' },

  section: { marginBottom: 24 },
  sectionHeader: { marginBottom: 12, fontSize: 16 },
  optionsGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 10 },
  
  optionChip: {
    paddingVertical: 8, paddingHorizontal: 16,
    borderRadius: 20, borderWidth: 1, borderColor: '#ddd',
    backgroundColor: '#f9f9f9'
  },
  optionChipSelected: { backgroundColor: '#2196F3', borderColor: '#2196F3' },
  optionText: { color: '#333', fontSize: 14 },
  optionTextSelected: { color: '#fff'},
  
  button: { backgroundColor: '#4CAF50', padding: 16, borderRadius: 12, alignItems: 'center', marginTop: 24 },
  buttonDisabled: { backgroundColor: '#ccc' },
  buttonText: { color: '#fff', fontSize: 16, fontWeight: 'bold' },
  
  emptyText: { textAlign: 'center', color: '#999', marginTop: 40, fontStyle: 'italic' }
});