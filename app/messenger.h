/* Original work Copyright 2023 joaquimorg
 * https://github.com/joaquimorg
 *
 * Modified work Copyright 2024 kamilsss655
 * https://github.com/kamilsss655
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifndef APP_MSG_H
#define APP_MSG_H

#ifdef ENABLE_MESSENGER

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/keyboard.h"
#include "helper/v2frame.h"

enum {
	PAYLOAD_LENGTH = V2_PAYLOAD_LEN
};

typedef enum KeyboardType {
	UPPERCASE,
  	LOWERCASE,
  	NUMERIC,
  	END_TYPE_KBRD
} KeyboardType;

extern KeyboardType keyboardType;
extern uint16_t gErrorsDuringMSG;
extern char cMessage[PAYLOAD_LENGTH];
// Feature #2 - bounded RAM ring log. 16 entries x 32 bytes = 512 bytes RAM
// against ~8,900 free. RAM ONLY: nothing here is ever written to EEPROM, which
// is the point - a message log that survives a power cycle is a liability under
// the threat model (the security design).
// 16 is a power of two so the wrap is a mask, not a divide - the Cortex-M0 has
// no divide instruction.
#define MSG_LOG_SIZE   16u
#define MSG_LOG_MASK   (MSG_LOG_SIZE - 1u)
// Rows the messenger screen can show at once: the region above the compose
// line is 4 rows at 7px pitch (ui/messenger.c).
#define MSG_LOG_LINES  4u

extern char    rxMessage[MSG_LOG_SIZE][PAYLOAD_LENGTH + 2];
extern uint8_t msgLogHead;      // index of the NEWEST entry
extern uint8_t msgLogCount;     // entries actually used, saturating at MSG_LOG_SIZE
extern uint8_t msgLogScroll;    // 0 = newest page; how far back the display is scrolled

// n = 0 is the newest entry, 1 the one before it, and so on.
#define MSG_LOG_AT(n)  rxMessage[(msgLogHead - (n)) & MSG_LOG_MASK]

void MSG_LogPush(void);         // advance the head and clear the new newest entry
extern uint8_t hasNewMessage;
extern uint8_t keyTickCounter;

typedef enum MsgStatus {
    READY,
    SENDING,
    RECEIVING,
} MsgStatus;

// Protocol v2 replaces the v1 PacketType enum (100..103, carried in a plaintext
// header byte with nothing authenticating it) with the authenticated `type`
// field of the v2 frame - V2_TYPE_* in helper/v2frame.h. The type is covered by
// the MAC, so it can no longer be edited in flight to turn a message into an
// ACK or vice versa.

// Modem Modulation                             // 2024 kamilsss655
typedef enum ModemModulation {
  MOD_FSK_450,   // for bad conditions
  MOD_FSK_700,   // for medium conditions
  MOD_AFSK_1200  // for good conditions
} ModemModulation;

// The v2 wire buffer. 56 bytes, even, so the FSK FIFO's 16-bit writes land
// exactly. Layout and codec live in helper/v2frame.h - deliberately NOT here,
// because that file has to compile on the host for the vector test.
//
// v1 transmitted its 13-byte nonce; v2 does not. The receiver reconstructs the
// nonce from sender_id and counter, which are already in the header. That is
// where the 4 bytes for the 16-byte Poly1305 tag came from.
extern uint8_t gMsgFrame[V2_FRAME_LEN];

// v2 identity, loaded from EEPROM 0x1D00 at boot (the protocol spec section 3).
// Provisioned over UART from the host, where real entropy exists - there is no
// key derivation and nothing is generated on the radio.
#define V2_EEPROM_KEY_ADDR       0x1D00u   // 32 bytes
#define V2_EEPROM_IDENTITY_ADDR  0x1D20u   // sender_id(4) + counter(4), one 8-byte page
#define V2_COUNTER_BLOCK         64u       // counters reserved per EEPROM write

extern uint8_t  gV2Key[V2_KEY_LEN];
extern uint32_t gV2SenderId;
extern bool     gV2Provisioned;

void MSG_V2LoadIdentity(void);

// Destroy K_master, sender_id and the counter - in EEPROM as well as RAM.
// This is the half of the panic wipe that matters under the threat model:
// MSG_Init() clears the messages on the radio, but the key is what decrypts
// every message anyone recorded off the air (the protocol spec section 10).
// After this the radio refuses to transmit until it is re-provisioned over the
// cable (decision #10).
void MSG_V2WipeIdentity(void);

// Replay / duplicate suppression (the protocol spec section 7).
//
// Four entries of (sender_id, counter) = 32 bytes. RAM only, and deliberately:
// the table records who we have been talking to, which is exactly the kind of
// thing a captured radio should not be carrying, and losing it on a reboot
// costs at most one re-displayed message.
//
// Four is enough for a flat broadcast group of this size. When it fills, the
// oldest slot is reused - only frames that have already AUTHENTICATED ever
// reach the table, so an attacker cannot churn it without the key.
#define V2_DEDUP_SIZE  4u

// Feature #1 - auto-retry. Timing measured from the source, not guessed:
// MSG_SendPacket() blocks ~300ms typically and up to ~1.3s, and the far end
// waits a further 700ms before answering so the sender can turn its radio
// around. A round trip is therefore ~1.3-2.6s, so anything under 2s guarantees
// duplicate transmissions on a HEALTHY link. 4 seconds it is.
#define MSG_RETRY_TIMEOUT_500MS  8u   // 4 seconds
// Named for what it actually is. The plan's "bound it at 3" was ambiguous
// between 3 retries and 3 transmissions; this is the total that goes on the
// air, so a failed message costs at most 3 frames of airtime.
#define MSG_MAX_TRANSMISSIONS    3u

// Returns true only if a frame actually reached the FIFO. False means nothing
// was transmitted - not ready, nothing to send, unprovisioned, or the VFO
// refused - and a retry must NOT be counted against the budget for those.
bool MSG_SendPacket();
void MSG_RetryTick(void);       // call once per 500ms timeslice

// Counts down in APP_TimeSlice500ms(). Non-zero means the first press of a
// WIPE+KEY gesture has landed and the second press - within the window - will
// destroy the key. Zero means disarmed.
extern uint8_t gPanicWipeArmed_500ms;
extern bool    gV2WipeFailed;
#define PANIC_WIPE_CONFIRM_500MS  6u   // 3 seconds

// MessengerConfig                            // 2024 kamilsss655
typedef union {
  struct {
    uint8_t
      receive    :1, // determines whether fsk modem will listen for new messages
      ack        :1, // determines whether the radio will automatically respond to messages with ACK
      // Was `encrypt`. v2 has no unencrypted, unauthenticated mode to select,
      // so the setting and its MsgEnc menu entry are gone. The bit is left in
      // place rather than repacked: the byte lives at EEPROM 0x0EA3 and moving
      // the other fields would silently reinterpret every already-programmed
      // radio's settings.
      retry      :1, // was `encrypt` (v1). Feature #1: auto-retry an unacknowledged
                     // message. Bit position deliberately unchanged - the byte
                     // lives at EEPROM 0x0EA3 and repacking would silently
                     // reinterpret every already-programmed radio's settings.
                     // Erased EEPROM reads 0xFF, so retry defaults ON, matching
                     // `receive` and `ack`.
      unused     :1,
      modulation :2, // determines FSK modulation type
      unused2    :2;
  } data;
  uint8_t __val;
} MessengerConfig;

void MSG_EnableRX(const bool enable);
void MSG_StorePacket(const uint16_t interrupt_bits);
void MSG_Init();
void MSG_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);
void MSG_FSKSendData();
void MSG_ClearPacketBuffer();
void MSG_SendAck(uint32_t ackSenderId, uint32_t ackCounter);
void MSG_HandleReceive();
void MSG_Send(const char *cMessage);
void MSG_ConfigureFSK(bool rx);

#endif

#endif
