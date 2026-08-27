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

enum {
	NONCE_LENGTH = 13,
	PAYLOAD_LENGTH = 30
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

typedef enum PacketType {
    MESSAGE_PACKET = 100u,
    ENCRYPTED_MESSAGE_PACKET,
    ACK_PACKET,
    INVALID_PACKET
} PacketType;

// Modem Modulation                             // 2024 kamilsss655
typedef enum ModemModulation {
  MOD_FSK_450,   // for bad conditions
  MOD_FSK_700,   // for medium conditions
  MOD_AFSK_1200  // for good conditions
} ModemModulation;

// Data Packet definition                            // 2024 kamilsss655
union DataPacket
{
  struct{
    uint8_t header;
    uint8_t payload[PAYLOAD_LENGTH];
    unsigned char nonce[NONCE_LENGTH];
    // uint8_t signature[SIGNATURE_LENGTH];
  } data;
  // header + payload + nonce = must be an even number
  uint8_t serializedArray[1+PAYLOAD_LENGTH+NONCE_LENGTH];
};

// MessengerConfig                            // 2024 kamilsss655
typedef union {
  struct {
    uint8_t
      receive    :1, // determines whether fsk modem will listen for new messages
      ack        :1, // determines whether the radio will automatically respond to messages with ACK
      encrypt    :1, // determines whether outgoing messages will be encrypted
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
void MSG_SendPacket();
void MSG_FSKSendData();
void MSG_ClearPacketBuffer();
void MSG_SendAck();
void MSG_HandleReceive();
void MSG_Send(const char *cMessage);
void MSG_ConfigureFSK(bool rx);

#endif

#endif
