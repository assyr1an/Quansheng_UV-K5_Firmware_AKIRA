/* Feature #4 — activity logging.
 *
 * A record of which frequencies went active during a scan. Nothing else in the
 * UV-K5 ecosystem does this.
 *
 * RAM ONLY, and deliberately so — two independent reasons:
 *
 *  1. EEPROM wear and blocking cost. Every EEPROM_WriteBuffer() call costs a
 *     hard SYSTEM_DelayMs(8) (driver/eeprom.c). A scan at 90ms/channel writing
 *     one entry per hit would stall the scanner ~9% of the time AND burn the
 *     part, because a log is by definition always-changing data.
 *  2. Threat model. A record of what we monitored and when must not survive a
 *     power cycle, for the same reasons the message log must not
 *     (the security design).
 *
 * COVERAGE LIMIT — CONFIRMED ON HARDWARE 2026-08-27, not merely predicted.
 * The hook is CHFRSCANNER_Found(), which sees only the channel/frequency
 * scanner. The spectrum analyser runs a separate modal loop that never calls it
 * — and that loop was measured to block the UART entirely for as long as its
 * screen is open (the codebase notes section 13 #20). So this logs "what the
 * scanner heard", never "everything the radio heard". Do not describe it
 * otherwise.
 */

#ifndef APP_ACTIVITY_H
#define APP_ACTIVITY_H

#include <stdbool.h>
#include <stdint.h>

// 24 entries x 8 bytes = 192, plus 2 bytes of state.
#define ACTIVITY_LOG_SIZE 24u

// Two hits on the same frequency inside this window update the existing entry
// instead of appending. Without it a repeater held open for 30 seconds fills
// the whole ring with one transmission. ~1.28s per tick unit, so 8 ~= 10s.
#define ACTIVITY_COALESCE_TICKS 8u

typedef struct {
	uint32_t freqOrChan;   // frequency in 10Hz units, or MR channel number
	uint16_t timeTicks;    // uptime >> 7  (~1.28s resolution, ~23h range)
	uint8_t  rssi;         // BK4819_GetRSSI() >> 1
	uint8_t  flags;        // bit0: entry is a channel number, not a frequency
} ActivityEntry_t;         // 8 bytes, naturally aligned - no padding

#define ACTIVITY_FLAG_IS_CHANNEL (1u << 0)

extern ActivityEntry_t gActivityLog[ACTIVITY_LOG_SIZE];
extern uint8_t         gActivityLogHead;    // newest entry
extern uint8_t         gActivityLogCount;   // used entries, saturating

void ACTIVITY_Record(uint32_t freqOrChan, bool isChannel);
void ACTIVITY_Clear(void);                  // called by the panic wipe

#endif
