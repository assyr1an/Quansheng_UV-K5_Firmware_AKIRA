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

#ifdef ENABLE_MESSENGER

#include <string.h>
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/bk4819.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "settings.h"
#include "radio.h"
#include "app.h"
#include "audio.h"
#include "functions.h"
#include "frequencies.h"
#include "driver/system.h"
#include "app/activity.h"
#include "app/messenger.h"
#include "ui/ui.h"
#include "driver/eeprom.h"
#include "helper/v2frame.h"
#ifdef ENABLE_MESSENGER_UART
    #include "driver/uart.h"
#endif

const uint8_t MSG_BUTTON_STATE_HELD = 1 << 1;

const uint8_t MSG_BUTTON_EVENT_SHORT =  0;
const uint8_t MSG_BUTTON_EVENT_LONG =  MSG_BUTTON_STATE_HELD;

const uint8_t MAX_MSG_LENGTH = PAYLOAD_LENGTH - 1;

uint16_t TONE2_FREQ;

#define NEXT_CHAR_DELAY 100 // 10ms tick

char T9TableLow[9][4] = { {',', '.', '?', '!'}, {'a', 'b', 'c', '\0'}, {'d', 'e', 'f', '\0'}, {'g', 'h', 'i', '\0'}, {'j', 'k', 'l', '\0'}, {'m', 'n', 'o', '\0'}, {'p', 'q', 'r', 's'}, {'t', 'u', 'v', '\0'}, {'w', 'x', 'y', 'z'} };
char T9TableUp[9][4] = { {',', '.', '?', '!'}, {'A', 'B', 'C', '\0'}, {'D', 'E', 'F', '\0'}, {'G', 'H', 'I', '\0'}, {'J', 'K', 'L', '\0'}, {'M', 'N', 'O', '\0'}, {'P', 'Q', 'R', 'S'}, {'T', 'U', 'V', '\0'}, {'W', 'X', 'Y', 'Z'} };
unsigned char numberOfLettersAssignedToKey[9] = { 4, 3, 3, 3, 3, 3, 4, 3, 4 };

char T9TableNum[9][4] = { {'1', '\0', '\0', '\0'}, {'2', '\0', '\0', '\0'}, {'3', '\0', '\0', '\0'}, {'4', '\0', '\0', '\0'}, {'5', '\0', '\0', '\0'}, {'6', '\0', '\0', '\0'}, {'7', '\0', '\0', '\0'}, {'8', '\0', '\0', '\0'}, {'9', '\0', '\0', '\0'} };
unsigned char numberOfNumsAssignedToKey[9] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };

char cMessage[PAYLOAD_LENGTH];
char lastcMessage[PAYLOAD_LENGTH];
char    rxMessage[MSG_LOG_SIZE][PAYLOAD_LENGTH + 2];
uint8_t msgLogHead;
uint8_t msgLogCount;
uint8_t msgLogScroll;
// Index of the message we are awaiting an ACK for - a stored index, not "the
// last line". See MSG_LogPush().
uint8_t msgPendingAckIdx;
unsigned char cIndex = 0;
unsigned char prevKey = 0, prevLetter = 0;
KeyboardType keyboardType = UPPERCASE;

MsgStatus msgStatus = READY;

// The v2 wire buffer, and the plaintext a verified frame decodes into. They are
// separate on purpose: nothing from a frame reaches the display, the log, or
// the UART until the Poly1305 tag has verified. v1 had no such gate - a wrong
// key produced garbage on screen and a forged frame was indistinguishable from
// a real one.
uint8_t     gMsgFrame[V2_FRAME_LEN];
static V2Message_t rxDecoded;

// Plaintext staged for transmission. Held apart from gMsgFrame because
// V2_Encode() writes the ciphertext, and we still want the cleartext for the
// local "sent" log line.
static uint8_t txPayload[V2_PAYLOAD_LEN];
static uint8_t txType;

// ---- v2 identity (the protocol spec section 3) ------------------------
// K_master, sender_id and the counter live at EEPROM 0x1D00 - the one gap the
// CHIRP driver never writes (PROG_SIZE = 0x1d00), so a CHIRP upload cannot
// clobber them. Provisioned over UART from the host; nothing here generates a
// key, because this radio has no entropy source worth trusting
// (the RNG measurements).
uint8_t  gV2Key[V2_KEY_LEN];
uint32_t gV2SenderId;
bool     gV2Provisioned;

// gV2Counter is the next counter to hand out. gV2CounterLimit is the value
// already committed to EEPROM: every counter below it is spent as far as a
// future boot is concerned. Writing EEPROM once per message would destroy the
// cell, so counters are reserved 64 at a time and a reboot simply skips the
// remainder of the block. Losing up to 63 counter values costs nothing -
// uniqueness is the only requirement a nonce has.
static uint32_t gV2Counter;
static uint32_t gV2CounterLimit;

uint8_t gPanicWipeArmed_500ms;

uint16_t gErrorsDuringMSG;

uint8_t hasNewMessage = 0;

uint8_t keyTickCounter = 0;

// -----------------------------------------------------

void MSG_FSKSendData() {

	// turn off CTCSS/CDCSS during FFSK
	const uint16_t css_val = BK4819_ReadRegister(BK4819_REG_51);
	BK4819_WriteRegister(BK4819_REG_51, 0);

	// set the FM deviation level
	const uint16_t dev_val = BK4819_ReadRegister(BK4819_REG_40);

	{
		uint16_t deviation;
		switch (gEeprom.VfoInfo[gEeprom.TX_VFO].CHANNEL_BANDWIDTH)
		{
			case BK4819_FILTER_BW_WIDE:            deviation =  1300; break; // 20k // measurements by kamilsss655
			case BK4819_FILTER_BW_NARROW:          deviation =  1200; break; // 10k
			// case BK4819_FILTER_BW_NARROWAVIATION:  deviation =  850; break;  // 5k
			// case BK4819_FILTER_BW_NARROWER:        deviation =  850; break;  // 5k
			// case BK4819_FILTER_BW_NARROWEST:	      deviation =  850; break;  // 5k
			default:                               deviation =  850;  break;  // 5k
		}

		//BK4819_WriteRegister(0x40, (3u << 12) | (deviation & 0xfff));
		BK4819_WriteRegister(BK4819_REG_40, (dev_val & 0xf000) | (deviation & 0xfff));
	}

	// REG_2B   0
	//
	// <15> 1 Enable CTCSS/CDCSS DC cancellation after FM Demodulation   1 = enable 0 = disable
	// <14> 1 Enable AF DC cancellation after FM Demodulation            1 = enable 0 = disable
	// <10> 0 AF RX HPF 300Hz filter     0 = enable 1 = disable
	// <9>  0 AF RX LPF 3kHz filter      0 = enable 1 = disable
	// <8>  0 AF RX de-emphasis filter   0 = enable 1 = disable
	// <2>  0 AF TX HPF 300Hz filter     0 = enable 1 = disable
	// <1>  0 AF TX LPF filter           0 = enable 1 = disable
	// <0>  0 AF TX pre-emphasis filter  0 = enable 1 = disable
	//
	// disable the 300Hz HPF and FM pre-emphasis filter
	//
	const uint16_t filt_val = BK4819_ReadRegister(BK4819_REG_2B);
	BK4819_WriteRegister(BK4819_REG_2B, (1u << 2) | (1u << 0));
	
	MSG_ConfigureFSK(false);



	SYSTEM_DelayMs(100);

	{	// load the entire packet data into the TX FIFO buffer
		for (size_t i = 0; i < sizeof(gMsgFrame); i += 2) {
        	BK4819_WriteRegister(BK4819_REG_5F, (gMsgFrame[i + 1] << 8) | gMsgFrame[i]);
    	}
	}

	// enable FSK TX
	BK4819_FskEnableTx();

	{
		// Allow up to 2000ms for the TX to complete; if it takes longer then
		// something has gone wrong and we shut the TX down.
		//
		// v1 used 1000ms - and its comment claimed 310ms, which was simply
		// wrong (the protocol spec section 2). A 56-byte v2 frame needs 996ms
		// of payload airtime at FSK-450 before preamble and the 4-byte sync are
		// counted, so 1000ms would have cut the slowest modulation off mid-frame.
		unsigned int timeout = 2000 / 5;

		while (timeout-- > 0)
		{
			SYSTEM_DelayMs(5);
			if (BK4819_ReadRegister(BK4819_REG_0C) & (1u << 0))
			{	// we have interrupt flags
				BK4819_WriteRegister(BK4819_REG_02, 0);
				if (BK4819_ReadRegister(BK4819_REG_02) & BK4819_REG_02_FSK_TX_FINISHED)
					timeout = 0;       // TX is complete
			}
		}
	}
	//BK4819_WriteRegister(BK4819_REG_02, 0);

	SYSTEM_DelayMs(100);

	// disable TX
	MSG_ConfigureFSK(true);

	// restore FM deviation level
	BK4819_WriteRegister(BK4819_REG_40, dev_val);

	// restore TX/RX filtering
	BK4819_WriteRegister(BK4819_REG_2B, filt_val);

	// restore the CTCSS/CDCSS setting
	BK4819_WriteRegister(BK4819_REG_51, css_val);

}

void MSG_EnableRX(const bool enable) {

	if (enable) {
		MSG_ConfigureFSK(true);

		if(gEeprom.MESSENGER_CONFIG.data.receive)
			BK4819_FskEnableRx();
	} else {
		BK4819_WriteRegister(BK4819_REG_70, 0);
		BK4819_WriteRegister(BK4819_REG_58, 0);
	}
}


// -----------------------------------------------------

static void MSG_PutLE32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 0);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t MSG_GetLE32(const uint8_t *p)
{
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Load K_master, sender_id and the counter from EEPROM 0x1D00. Called from
// BOARD_EEPROM_Init(), so a UART write to that range followed by the existing
// EEPROM reload picks up a freshly provisioned key without a reboot.
void MSG_V2LoadIdentity(void)
{
	uint8_t identity[8];
	uint8_t seen = 0;
	uint8_t nonzero = 0;
	uint8_t i;

	EEPROM_ReadBuffer(V2_EEPROM_KEY_ADDR, gV2Key, V2_KEY_LEN);
	EEPROM_ReadBuffer(V2_EEPROM_IDENTITY_ADDR, identity, sizeof(identity));

	gV2SenderId     = MSG_GetLE32(&identity[0]);
	gV2CounterLimit = MSG_GetLE32(&identity[4]);
	gV2Counter      = gV2CounterLimit;

	// An erased cell reads 0xFF; a wiped one could read 0x00. Reject BOTH -
	// an all-zero key is not a key, and treating it as one would let a failed
	// wipe or a blanked EEPROM look provisioned. Accumulated with OR rather
	// than compared, so nothing about the key leaks through an early exit.
	for (i = 0; i < V2_KEY_LEN; i++) {
		seen    |= (uint8_t)(gV2Key[i] ^ 0xFFu);   // non-zero unless all 0xFF
		nonzero |= gV2Key[i];                      // non-zero unless all 0x00
	}

	gV2Provisioned = (seen != 0) && (nonzero != 0) &&
	                 (gV2SenderId != 0xFFFFFFFFu) && (gV2SenderId != 0u) &&
	                 (gV2CounterLimit != 0xFFFFFFFFu);

	if (!gV2Provisioned) {
		// Never leave a half-loaded key in RAM to be used by accident.
		memset(gV2Key, 0, sizeof(gV2Key));
		gV2SenderId = 0;
	}
}

// Hand out the next transmit counter, reserving a block in EEPROM when the
// current one runs out. Returns false when the radio must NOT transmit:
// unprovisioned, or the counter space is exhausted.
static bool MSG_V2ReserveCounter(void)
{
	uint8_t identity[8];

	if (!gV2Provisioned)
		return false;

	if (gV2Counter >= gV2CounterLimit) {
		// the protocol spec section 4, hard rule: the counter must never go
		// backwards, and it must never wrap. Once the space is gone the radio
		// stops transmitting rather than reusing a nonce - reuse under ChaCha20
		// leaks the XOR of two plaintexts and destroys the Poly1305 key.
		if (gV2CounterLimit > (0xFFFFFFFFu - V2_COUNTER_BLOCK))
			return false;

		gV2CounterLimit += V2_COUNTER_BLOCK;
		MSG_PutLE32(&identity[0], gV2SenderId);
		MSG_PutLE32(&identity[4], gV2CounterLimit);
		EEPROM_WriteBuffer(V2_EEPROM_IDENTITY_ADDR, identity, true);
	}

	return true;
}

// Destroy the v2 identity in EEPROM and in RAM. See messenger.h.
//
// Written as 0xFF, not 0x00, so the result is indistinguishable from a radio
// that was never provisioned - both to the firmware's own check and to anyone
// who dumps the EEPROM afterwards. A block of zeros in a field of 0xFF would
// announce that something used to be here.
void MSG_V2WipeIdentity(void)
{
	static const uint8_t blank[8] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	uint8_t i;

	// EEPROM first. If the operator pulls the battery halfway through this,
	// the key must already be the part that is gone.
	for (i = 0; i < (V2_KEY_LEN / 8u); i++)
		EEPROM_WriteBuffer(V2_EEPROM_KEY_ADDR + (i * 8u), blank, true);
	EEPROM_WriteBuffer(V2_EEPROM_IDENTITY_ADDR, blank, true);

	memset(gV2Key, 0, sizeof(gV2Key));
	gV2SenderId     = 0;
	gV2Counter      = 0;
	gV2CounterLimit = 0;
	gV2Provisioned  = false;
}

// Was moveUP(): three unrolled strcpy's that copied the entire log on every
// message. At 16 entries that would have been 15 strcpy's and several hundred
// bytes of flash for nothing. A head index does the same job in O(1).
void MSG_LogPush(void)
{
	msgLogHead = (msgLogHead + 1u) & MSG_LOG_MASK;
	memset(rxMessage[msgLogHead], 0, sizeof(rxMessage[msgLogHead]));
	if (msgLogCount < MSG_LOG_SIZE)
		msgLogCount++;
	// A new arrival jumps the view back to the newest entry, otherwise a message
	// can land off-screen while the user is reading history.
	msgLogScroll = 0;
}

void MSG_SendPacket() {

	if ( msgStatus != READY ) return;

	const bool isAck = (txType == V2_TYPE_ENC_ACK);

	// An ACK's payload is the (sender_id, counter) it acknowledges and may
	// legitimately start with a zero byte, so only a real message is required
	// to be non-empty.
	if ( !isAck && txPayload[0] == 0 ) {
		AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
		return;
	}

	// v2 has no unauthenticated fallback, by design. Without a provisioned key,
	// sender_id and counter there is nothing safe to transmit, so we refuse and
	// beep rather than emitting something forgeable (the protocol spec 3, 4).
	//
	// Reserving here, before the VFO check, can burn a counter block if the TX
	// is then refused. That is deliberate: counters only have to be UNIQUE, and
	// simple, obviously-monotonic reservation is worth more than saving an
	// EEPROM write that happens once per 64 messages.
	if ( !MSG_V2ReserveCounter() ) {
		AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
		return;
	}

	RADIO_PrepareTX();

	if(RADIO_GetVfoState() != VFO_STATE_NORMAL){
		gRequestDisplayScreen = DISPLAY_MAIN;
		return;
	} 

	{
		msgStatus = SENDING;

		RADIO_SetVfoState(VFO_STATE_NORMAL);
		BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
		BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);

		// display sent message (the staged plaintext, before it is encrypted
		// into the frame)
		if (!isAck) {
			MSG_LogPush();
			// Remember WHICH entry this is so the ACK marker lands on the right
			// line even if a message arrives between sending and the ACK.
			msgPendingAckIdx = msgLogHead;
			// BOUNDS: sprintf() is unbounded on BOTH read and write. The entry is
			// PAYLOAD_LENGTH+2 bytes and txPayload[] has no guaranteed NUL. Bound both.
			snprintf(rxMessage[msgLogHead], PAYLOAD_LENGTH + 2, "> %.*s", (int)PAYLOAD_LENGTH, (const char *)txPayload);
			memset(lastcMessage, 0, sizeof(lastcMessage));
			memcpy(lastcMessage, txPayload, PAYLOAD_LENGTH);
			cIndex = 0;
			prevKey = 0;
			prevLetter = 0;
			memset(cMessage, 0, sizeof(cMessage));
		}

		// Build the authenticated frame. Every frame is encrypted and MACed -
		// v2 has no plaintext mode to fall back to, and the header is the AAD so
		// type, sender_id and counter are authenticated along with the payload.
		V2_Encode(gMsgFrame, gV2Key, txType, gV2SenderId, gV2Counter, txPayload, V2_PAYLOAD_LEN);
		gV2Counter++;

		// The staged plaintext has served its purpose. Do not leave a copy of it
		// sitting in RAM for a capture to find (the security design).
		memset(txPayload, 0, sizeof(txPayload));

		BK4819_DisableDTMF();

		// mute the mic during TX
		gMuteMic = true;

		SYSTEM_DelayMs(50);

		MSG_FSKSendData();

		SYSTEM_DelayMs(50);

		APP_EndTransmission(false);
		// this must be run after end of TX, otherwise radio will still TX transmit without even RED LED on
		FUNCTION_Select(FUNCTION_FOREGROUND);

		RADIO_SetVfoState(VFO_STATE_NORMAL);

		// disable mic mute after TX
		gMuteMic = false;

		BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);

		MSG_EnableRX(true);

		// clear packet buffer
		MSG_ClearPacketBuffer();

		msgStatus = READY;
	}
}

uint8_t validate_char( uint8_t rchar ) {
	if ( (rchar == 0x1b) || (rchar >= 32 && rchar <= 127) ) {
		return rchar;
	}
	return 32;
}

void MSG_StorePacket(const uint16_t interrupt_bits) {

	//const uint16_t rx_sync_flags   = BK4819_ReadRegister(BK4819_REG_0B);

	const bool rx_sync             = (interrupt_bits & BK4819_REG_02_FSK_RX_SYNC) ? true : false;
	const bool rx_fifo_almost_full = (interrupt_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL) ? true : false;
	const bool rx_finished         = (interrupt_bits & BK4819_REG_02_FSK_RX_FINISHED) ? true : false;

	//UART_printf("\nMSG : S%i, F%i, E%i | %i", rx_sync, rx_fifo_almost_full, rx_finished, interrupt_bits);

	if (rx_sync) {
		#ifdef ENABLE_MESSENGER_FSK_MUTE
			// prevent listening to fsk data and squelch (kamilsss655)
			// CTCSS codes seem to false trigger the rx_sync
			if(gCurrentCodeType == CODE_TYPE_OFF)
				AUDIO_AudioPathOff();
		#endif
		gFSKWriteIndex = 0;
		MSG_ClearPacketBuffer();
		msgStatus = RECEIVING;
	}

	if (rx_fifo_almost_full && msgStatus == RECEIVING) {

		const uint16_t count = BK4819_ReadRegister(BK4819_REG_5E) & (7u << 0);  // almost full threshold
		for (uint16_t i = 0; i < count; i++) {
			const uint16_t word = BK4819_ReadRegister(BK4819_REG_5F);
			if (gFSKWriteIndex < sizeof(gMsgFrame))
				gMsgFrame[gFSKWriteIndex++] = (word >> 0) & 0xff;
			if (gFSKWriteIndex < sizeof(gMsgFrame))
				gMsgFrame[gFSKWriteIndex++] = (word >> 8) & 0xff;
		}

		SYSTEM_DelayMs(10);

	}

	if (rx_finished) {
		// turn off green LED
		BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, 0);
		BK4819_FskClearFifo();
		BK4819_FskEnableRx();
		msgStatus = READY;

		// v2 frames are a fixed 56 bytes. A short frame cannot be authenticated,
		// so it is not worth parsing - v1 handed anything over 2 bytes to the
		// display path.
		if (gFSKWriteIndex >= V2_FRAME_LEN) {
			MSG_HandleReceive();
		}
		gFSKWriteIndex = 0;
	}
}

void MSG_Init() {
	// Feature #3: this IS the panic wipe. Everything holding plaintext is cleared
	// here, so any new plaintext buffer must be added to this list as well.
	memset(rxMessage, 0, sizeof(rxMessage));
	memset(cMessage, 0, sizeof(cMessage));
	memset(lastcMessage, 0, sizeof(lastcMessage));
	// v2 added two more places plaintext lives: the staged transmit payload and
	// the buffer a verified frame decodes into. Both must be wiped here, or the
	// panic wipe leaves the last message sent and the last message received
	// sitting in RAM.
	memset(txPayload, 0, sizeof(txPayload));
	memset(&rxDecoded, 0, sizeof(rxDecoded));
	memset(gMsgFrame, 0, sizeof(gMsgFrame));
	txType = V2_TYPE_ENC_MSG;
	msgLogHead       = 0;
	msgLogCount      = 0;
	msgLogScroll     = 0;
	msgPendingAckIdx = 0;
	// Feature #4 section 4.5: the activity log is plaintext intelligence about
	// what we monitored and when. The wipe must reach it too.
	ACTIVITY_Clear();
	hasNewMessage = 0;
	msgStatus = READY;
	prevKey = 0;
    prevLetter = 0;
	cIndex = 0;
	// NOTE: this does NOT wipe K_master. The key lives in EEPROM 0x1D00 and
	// survives, so a captured radio still yields every message ever recorded off
	// the air. That is a deliberate open question, not an oversight - see
	// decision #10.
}

void MSG_SendAck(uint32_t ackSenderId, uint32_t ackCounter) {
	// An authenticated ACK. Its payload names the exact (sender_id, counter)
	// being acknowledged and is covered by the MAC, so a stale or replayed ACK
	// can no longer be taken as confirmation of a different transaction - the
	// v1 failure the crypto review flagged (the crypto review).
	//
	// The ACK gets its own counter and its own type, so its nonce can never
	// collide with the message it acknowledges.
	//
	// Matching the ACK to the pending message, and suppressing duplicates, is
	// step 5 of the build order. This commit makes the ACK unforgeable; it does
	// not yet make it selective.
	MSG_ClearPacketBuffer();
	memset(txPayload, 0, sizeof(txPayload));
	MSG_PutLE32(&txPayload[0], ackSenderId);
	MSG_PutLE32(&txPayload[4], ackCounter);
	txType = V2_TYPE_ENC_ACK;
	MSG_SendPacket();
}

void MSG_HandleReceive(){

	// THE AUTHENTICATION GATE. Nothing from the air reaches the log, the
	// display, or the UART until the Poly1305 tag verifies over the header and
	// the ciphertext together.
	//
	// v1 had no gate at all: it decrypted whatever arrived and printed the
	// result, so a forged frame was indistinguishable from a real one and a
	// wrong key produced garbage on screen with no indication anything was
	// wrong. A frame that fails here is forged, corrupted, or from a radio
	// holding a different key - in every case there is nothing useful to show,
	// so it is counted and dropped silently rather than displayed as an error.
	// Displaying it would hand an attacker a free way to write to our screen.
	if ( !gV2Provisioned || !V2_Decode(&rxDecoded, gV2Key, gMsgFrame) ) {
		gErrorsDuringMSG++;
		return;
	}

	if (rxDecoded.type == V2_TYPE_ACK || rxDecoded.type == V2_TYPE_ENC_ACK) {
	#ifdef ENABLE_MESSENGER_DELIVERY_NOTIFICATION
		#ifdef ENABLE_MESSENGER_UART
			UART_printf("SVC<RCPT\r\n");
		#endif
		// Feature #2 bug fix: this was rxMessage[3][0] - "the last line". Only
		// correct if nothing arrived between sending and the ACK.
		//
		// The ACK now carries the (sender_id, counter) it acknowledges, in
		// rxDecoded.payload[0..7], and it is authenticated. Step 5 will match
		// that against the pending message instead of trusting arrival order.
		rxMessage[msgPendingAckIdx][0] = '+';
		gUpdateStatus = true;
		gUpdateDisplay = true;
	#endif
		return;
	}

	MSG_LogPush();
	// BOUNDS: the payload is 30 bytes off the wire with no guaranteed NUL, so
	// the %s conversion is bounded on the READ as well as the write
	// (external/printf/printf.c:798, _strnlen_s(p, precision)).
	snprintf(rxMessage[msgLogHead], PAYLOAD_LENGTH + 2, "< %.*s", (int)PAYLOAD_LENGTH, (const char *)rxDecoded.payload);
	#ifdef ENABLE_MESSENGER_UART
		UART_printf("SMS<%.*s\r\n", (int)PAYLOAD_LENGTH, (const char *)rxDecoded.payload);
	#endif

	if ( gScreenToDisplay != DISPLAY_MSG ) {
		hasNewMessage = 1;
		gUpdateStatus = true;
		gUpdateDisplay = true;
	#ifdef ENABLE_MESSENGER_NOTIFICATION
		gPlayMSGRing = true;
	#endif
	}
	else {
		gUpdateDisplay = true;
	}

	// Acknowledge the message we just authenticated. Naming its sender and
	// counter is what lets the far end tell this ACK from any other.
	if(gEeprom.MESSENGER_CONFIG.data.ack)
	{
		const uint32_t ackSenderId = rxDecoded.sender_id;
		const uint32_t ackCounter  = rxDecoded.counter;
		// wait so the correspondent radio can properly receive it
		SYSTEM_DelayMs(700);
		MSG_SendAck(ackSenderId, ackCounter);
	}
}

// ---------------------------------------------------------------------------------

void insertCharInMessage(uint8_t key) {
	if ( key == KEY_0 ) {
		if ( keyboardType == NUMERIC ) {
			cMessage[cIndex] = '0';
		} else {
			cMessage[cIndex] = ' ';
		}
		if ( cIndex < MAX_MSG_LENGTH ) {
			cIndex++;
		}
	} else if (prevKey == key)
	{
		cIndex = (cIndex > 0) ? cIndex - 1 : 0;
		if ( keyboardType == NUMERIC ) {
			cMessage[cIndex] = T9TableNum[key - 1][(++prevLetter) % numberOfNumsAssignedToKey[key - 1]];
		} else if ( keyboardType == LOWERCASE ) {
			cMessage[cIndex] = T9TableLow[key - 1][(++prevLetter) % numberOfLettersAssignedToKey[key - 1]];
		} else {
			cMessage[cIndex] = T9TableUp[key - 1][(++prevLetter) % numberOfLettersAssignedToKey[key - 1]];
		}
		if ( cIndex < MAX_MSG_LENGTH ) {
			cIndex++;
		}
	}
	else
	{
		prevLetter = 0;
		if ( cIndex >= MAX_MSG_LENGTH ) {
			cIndex = (cIndex > 0) ? cIndex - 1 : 0;
		}
		if ( keyboardType == NUMERIC ) {
			cMessage[cIndex] = T9TableNum[key - 1][prevLetter];
		} else if ( keyboardType == LOWERCASE ) {
			cMessage[cIndex] = T9TableLow[key - 1][prevLetter];
		} else {
			cMessage[cIndex] = T9TableUp[key - 1][prevLetter];
		}
		if ( cIndex < MAX_MSG_LENGTH ) {
			cIndex++;
		}

	}
	cMessage[cIndex] = '\0';
	if ( keyboardType == NUMERIC ) {
		prevKey = 0;
		prevLetter = 0;
	} else {
		prevKey = key;
	}
}

void processBackspace() {
	cIndex = (cIndex > 0) ? cIndex - 1 : 0;
	cMessage[cIndex] = '\0';
	prevKey = 0;
    prevLetter = 0;
}

void  MSG_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) {
	uint8_t state = bKeyPressed + 2 * bKeyHeld;

	if (state == MSG_BUTTON_EVENT_SHORT) {

		switch (Key)
		{
			case KEY_0:
			case KEY_1:
			case KEY_2:
			case KEY_3:
			case KEY_4:
			case KEY_5:
			case KEY_6:
			case KEY_7:
			case KEY_8:
			case KEY_9:
				if ( keyTickCounter > NEXT_CHAR_DELAY) {
					prevKey = 0;
    				prevLetter = 0;
				}
				insertCharInMessage(Key);
				keyTickCounter = 0;
				break;
			case KEY_STAR:
				keyboardType = (KeyboardType)((keyboardType + 1) % END_TYPE_KBRD);
				break;
			case KEY_F:
				processBackspace();
				break;
			case KEY_UP:
				// While scrolled back, UP scrolls forward again; only at the
				// newest page does it keep its original "recall last message"
				// behaviour on the compose line.
				if (msgLogScroll > 0) {
					msgLogScroll--;
					gUpdateDisplay = true;
					break;
				}
				memset(cMessage, 0, sizeof(cMessage));
				memcpy(cMessage, lastcMessage, PAYLOAD_LENGTH);
				// BOUNDS: lastcMessage[] is copied at full width with no guaranteed NUL, so
				// strlen() could run past cMessage[] and leave cIndex > MAX_MSG_LENGTH, which
				// would then index out of bounds on the next keypress.
				cMessage[PAYLOAD_LENGTH - 1] = '\0';
				cIndex = strlen(cMessage);
				break;
			case KEY_DOWN:
				// Scroll back through history. The view shows MSG_LOG_LINES
				// entries, so the furthest useful scroll is count - lines.
				if (msgLogCount > MSG_LOG_LINES && msgLogScroll < (uint8_t)(msgLogCount - MSG_LOG_LINES))
					msgLogScroll++;
				gUpdateDisplay = true;
				break;
			case KEY_MENU:
				// Send message
				MSG_Send(cMessage);
				break;
			case KEY_EXIT:
				gRequestDisplayScreen = DISPLAY_MAIN;
				break;

			default:
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
				break;
		}

	} else if (state == MSG_BUTTON_EVENT_LONG) {

		switch (Key)
		{
			case KEY_F:
				MSG_Init();
				break;
			default:
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
				break;
		}
	}

}

void MSG_ClearPacketBuffer()
{
	memset(gMsgFrame, 0, sizeof(gMsgFrame));
}

void MSG_Send(const char *cMessage){
	MSG_ClearPacketBuffer();
	memset(txPayload, 0, sizeof(txPayload));
	memcpy(txPayload, cMessage, V2_PAYLOAD_LEN);
	// v2 always encrypts and always authenticates. There is no unprotected mode
	// to select, so the old MESSENGER_CONFIG.encrypt branch is gone - a setting
	// that could turn authentication off would defeat the point of v2.
	txType = V2_TYPE_ENC_MSG;
	MSG_SendPacket();
}

void MSG_ConfigureFSK(bool rx)
{
	// REG_70
	//
	// <15>   0 Enable TONE1
	//        1 = Enable
	//        0 = Disable
	//
	// <14:8> 0 TONE1 tuning gain
	//        0 ~ 127
	//
	// <7>    0 Enable TONE2
	//        1 = Enable
	//        0 = Disable
	//
	// <6:0>  0 TONE2/FSK tuning gain
	//        0 ~ 127
	//
	BK4819_WriteRegister(BK4819_REG_70,
		( 0u << 15) |    // 0
		( 0u <<  8) |    // 0
		( 1u <<  7) |    // 1
		(96u <<  0));    // 96

	// Tone2 = FSK baudrate                       // kamilsss655 2024
	switch(gEeprom.MESSENGER_CONFIG.data.modulation)
	{
		case MOD_AFSK_1200:
			TONE2_FREQ = 12389u;
			break;
		case MOD_FSK_700:
			TONE2_FREQ = 7227u;
			break;
		case MOD_FSK_450:
			TONE2_FREQ = 4646u;
			break;
	}

	BK4819_WriteRegister(BK4819_REG_72, TONE2_FREQ);
	
	switch(gEeprom.MESSENGER_CONFIG.data.modulation)
	{
		case MOD_FSK_700:
		case MOD_FSK_450:
			BK4819_WriteRegister(BK4819_REG_58,
				(0u << 13) |		// 1 FSK TX mode selection
									//   0 = FSK 1.2K and FSK 2.4K TX .. no tones, direct FM
									//   1 = FFSK 1200 / 1800 TX
									//   2 = ???
									//   3 = FFSK 1200 / 2400 TX
									//   4 = ???
									//   5 = NOAA SAME TX
									//   6 = ???
									//   7 = ???
									//
				(0u << 10) |		// 0 FSK RX mode selection
									//   0 = FSK 1.2K, FSK 2.4K RX and NOAA SAME RX .. no tones, direct FM
									//   1 = ???
									//   2 = ???
									//   3 = ???
									//   4 = FFSK 1200 / 2400 RX
									//   5 = ???
									//   6 = ???
									//   7 = FFSK 1200 / 1800 RX
									//
				(3u << 8) |			// 0 FSK RX gain
									//   0 ~ 3
									//
				(0u << 6) |			// 0 ???
									//   0 ~ 3
									//
				(0u << 4) |			// 0 FSK preamble type selection
									//   0 = 0xAA or 0x55 due to the MSB of FSK sync byte 0
									//   1 = ???
									//   2 = 0x55
									//   3 = 0xAA
									//
				(0u << 1) |			// 1 FSK RX bandwidth setting
									//   0 = FSK 1.2K .. no tones, direct FM
									//   1 = FFSK 1200 / 1800
									//   2 = NOAA SAME RX
									//   3 = ???
									//   4 = FSK 2.4K and FFSK 1200 / 2400
									//   5 = ???
									//   6 = ???
									//   7 = ???
									//
				(1u << 0));			// 1 FSK enable
									//   0 = disable
									//   1 = enable
		break;
		case MOD_AFSK_1200:
			BK4819_WriteRegister(BK4819_REG_58,
				(1u << 13) |		// 1 FSK TX mode selection
									//   0 = FSK 1.2K and FSK 2.4K TX .. no tones, direct FM
									//   1 = FFSK 1200 / 1800 TX
									//   2 = ???
									//   3 = FFSK 1200 / 2400 TX
									//   4 = ???
									//   5 = NOAA SAME TX
									//   6 = ???
									//   7 = ???
									//
				(7u << 10) |		// 0 FSK RX mode selection
									//   0 = FSK 1.2K, FSK 2.4K RX and NOAA SAME RX .. no tones, direct FM
									//   1 = ???
									//   2 = ???
									//   3 = ???
									//   4 = FFSK 1200 / 2400 RX
									//   5 = ???
									//   6 = ???
									//   7 = FFSK 1200 / 1800 RX
									//
				(3u << 8) |			// 0 FSK RX gain
									//   0 ~ 3
									//
				(0u << 6) |			// 0 ???
									//   0 ~ 3
									//
				(0u << 4) |			// 0 FSK preamble type selection
									//   0 = 0xAA or 0x55 due to the MSB of FSK sync byte 0
									//   1 = ???
									//   2 = 0x55
									//   3 = 0xAA
									//
				(1u << 1) |			// 1 FSK RX bandwidth setting
									//   0 = FSK 1.2K .. no tones, direct FM
									//   1 = FFSK 1200 / 1800
									//   2 = NOAA SAME RX
									//   3 = ???
									//   4 = FSK 2.4K and FFSK 1200 / 2400
									//   5 = ???
									//   6 = ???
									//   7 = ???
									//
				(1u << 0));			// 1 FSK enable
									//   0 = disable
									//   1 = enable
		break;
	}

	// v2 uses a DIFFERENT 4-byte sync word from v1's 30 72 57 6C.
	//
	// This is the version boundary, and it costs zero payload bytes: a v1 radio
	// never raises an interrupt for a v2 frame and vice versa, so the two
	// protocols are invisible to each other at the hardware layer. No version
	// confusion to handle, no downgrade path, no v1 garbage reaching a parser
	// that now has a MAC to check (the protocol spec section 6). The `ver`
	// byte inside the frame is kept for evolution WITHIN v2.
	//
	// 4B 35 9C 2E - 16 of 32 bits set, no run longer than three, chosen so the
	// correlator has an easy time of it.

	// REG_5A .. bytes 0 & 1 sync pattern
	//
	// <15:8> sync byte 0
	// < 7:0> sync byte 1
	BK4819_WriteRegister(BK4819_REG_5A, 0x4B35);

	// REG_5B .. bytes 2 & 3 sync pattern
	//
	// <15:8> sync byte 2
	// < 7:0> sync byte 3
	BK4819_WriteRegister(BK4819_REG_5B, 0x9C2E);

	// CRC LEFT DISABLED - deliberately, against the protocol spec section 6.
	//
	// The spec proposed 0x5625 -> 0x5665 (bit 6), copying AirCopy, as "free
	// rejection of corrupted frames before they reach the MAC check". It is
	// free only if the hardware CRC is transparent to the declared frame
	// length, and the source does not settle that: AirCopy declares 72 bytes
	// (REG_5D = 0x4700) for a 72-byte buffer whose CRC16 it computes ITSELF in
	// software (app/aircopy.c:51, :90), yet it also reads a hardware CRC status
	// bit (app/aircopy.c:80-82). Either the hardware appends and strips two
	// bytes around the declared length, or it does not - and which one is true
	// decides whether a 56-byte v2 frame still arrives as 56 bytes.
	//
	// Getting that wrong breaks FSK receive outright, and it cannot be told
	// apart from any other RX failure without a second radio. The Poly1305 tag
	// already rejects every corrupted frame with certainty, so the CRC buys
	// only earliness. Not worth an untestable risk to the one thing v2 exists
	// to make work. Flip this to 0x5665 as a one-line bench experiment once
	// radio #2 arrives.
	BK4819_WriteRegister(BK4819_REG_5C, 0x5625);

	// set the almost full threshold
	if(rx)
		BK4819_WriteRegister(BK4819_REG_5E, (64u << 3) | (1u << 0));  // 0 ~ 127, 0 ~ 7

	// packet size .. sync + packet - size of a single packet

	uint16_t size = sizeof(gMsgFrame);
	// size -= (fsk_reg59 & (1u << 3)) ? 4 : 2;
	if(rx)
		size = (((size + 1) / 2) * 2) + 2;             // round up to even, else FSK RX doesn't work

	BK4819_WriteRegister(BK4819_REG_5D, (size << 8));

	// clear FIFO's
	BK4819_FskClearFifo();

	// configure main FSK params
	BK4819_WriteRegister(BK4819_REG_59,
				(0u        <<       15) |   // 0/1     1 = clear TX FIFO
				(0u        <<       14) |   // 0/1     1 = clear RX FIFO
				(0u        <<       13) |   // 0/1     1 = scramble
				(0u        <<       12) |   // 0/1     1 = enable RX
				(0u        <<       11) |   // 0/1     1 = enable TX
				(0u        <<       10) |   // 0/1     1 = invert data when RX
				(0u        <<        9) |   // 0/1     1 = invert data when TX
				(0u        <<        8) |   // 0/1     ???
				((rx ? 0u : 15u) <<  4) |   // 0 ~ 15  preamble length .. bit toggling
				(1u        <<        3) |   // 0/1     sync length
				(0u        <<        0)     // 0 ~ 7   ???
				
	);

	// clear interupts
	BK4819_WriteRegister(BK4819_REG_02, 0);
}

#endif
