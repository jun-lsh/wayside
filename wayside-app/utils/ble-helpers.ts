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
    client.writeMessage(messageToSend + '\n', true)
      .catch((err) => {
        if (!hasResolved) {
          hasResolved = true;
          clearTimeout(timer);
          reject(new Error(`Write failed: ${err.message}`));
        }
      });
  });
};