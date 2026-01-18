import { BleUartClient } from '@/utils/ble-uart';

/**
 * Sends a message and waits for a specific acknowledgment string.
 */
export const sendAndWaitForAck = (
  client: BleUartClient | null,
  messageToSend: string,
  ackStartString: string,
  timeoutMs: number = 10000
): Promise<void> => {
  return new Promise((resolve, reject) => {
    if (!client || !client.isConnected()) {
      return reject(new Error('Not connected to device'));
    }

    let hasResolved = false;
    let cleanup: (() => void) | null = null;

    // Timeout Logic
    const timer = setTimeout(() => {
      if (!hasResolved) {
        hasResolved = true;
        if (cleanup) cleanup();
        reject(new Error(`Timeout waiting for ack: ${ackStartString}`));
      }
    }, timeoutMs);

    // Listener for ACK
    const handleNotification = (msg: string) => {
      // Check if message starts with or includes the ACK string
      if (msg.trim().includes(ackStartString)) {
        if (!hasResolved) {
          hasResolved = true;
          clearTimeout(timer);
          // We don't remove the listener here because BleUartClient usually 
          // allows only one listener or needs a specific unsubscription method.
          // Ideally, we swap the listener back to a generic one or just resolve.
          resolve();
        }
      }
    };

    // 1. Hook up the listener
    // Note: This assumes startNotifications replaces the previous callback.
    // If your library supports multiple listeners, you'd add/remove here.
    client.startNotifications(
      handleNotification,
      (err) => console.error('Notification error:', err)
    );

    // 2. Send the message
    client.writeMessage(messageToSend + '\r', true)
      .catch((err) => {
        if (!hasResolved) {
          hasResolved = true;
          clearTimeout(timer);
          reject(new Error(`Write failed: ${err.message}`));
        }
      });
  });
};

// Helper to pause execution (allows ESP32 buffer to clear)
const sleep = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));

/**
 * Sends a message in chunks and waits for a specific acknowledgment string.
 */
export const sendAndWaitForAckPairing = (
  client: BleUartClient | null,
  messageToSend: string,
  ackStartString: string,
  timeoutMs: number = 10000
): Promise<void> => {
  return new Promise(async (resolve, reject) => {
    if (!client || !client.isConnected()) {
      return reject(new Error('Not connected to device'));
    }

    let hasResolved = false;

    // 1. Timeout Logic
    const timer = setTimeout(() => {
      if (!hasResolved) {
        hasResolved = true;
        reject(new Error(`Timeout waiting for ack: ${ackStartString}`));
      }
    }, timeoutMs);

    // 2. Listener for ACK
    const handleNotification = (msg: string) => {
      // Check if message starts with or includes the ACK string
      // using .trim() ignores stray newlines from the serial stream
      if (msg.trim().includes(ackStartString)) {
        if (!hasResolved) {
          hasResolved = true;
          clearTimeout(timer);
          resolve();
        }
      }
    };

    // Hook up the listener
    client.startNotifications(
      handleNotification,
      (err) => console.error('Notification error:', err)
    );

    try {
      // 3. Prepare Message (Sanitize)
      // Strip existing newlines from RSA keys to prevent breaking C parser
      // Append the delimiter '\r' strictly at the end
      const rawPayload = messageToSend.replace(/(\r\n|\n|\r)/gm, "") + '\r';

      // 4. CHUNKING LOGIC (Critical for RSA Keys)
      // Send in 100-byte chunks to fit inside negotiated MTU (usually ~240)
      const CHUNK_SIZE = 100;
      
      for (let i = 0; i < rawPayload.length; i += CHUNK_SIZE) {
        // If we already timed out or succeeded, stop sending
        if (hasResolved) break;

        const chunk = rawPayload.slice(i, i + CHUNK_SIZE);
        
        // Write chunk
        // Note: We use write without response usually for throughput, 
        // but write with response (true) is safer for pairing.
        await client.writeMessage(chunk, true);

        // Small delay to prevent flooding ESP32 RTOS buffer
        await sleep(20); 
      }
      
    } catch (err: any) {
      if (!hasResolved) {
        hasResolved = true;
        clearTimeout(timer);
        reject(new Error(`Write failed: ${err.message}`));
      }
    }
  });
};
