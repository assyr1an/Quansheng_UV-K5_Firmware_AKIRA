/* Feature #4 — activity logging. See activity.h for the design rationale. */

#include <string.h>

#include "app/activity.h"
#include "driver/bk4819.h"
#include "misc.h"

ActivityEntry_t gActivityLog[ACTIVITY_LOG_SIZE];
uint8_t         gActivityLogHead;
uint8_t         gActivityLogCount;

void ACTIVITY_Clear(void)
{
	memset(gActivityLog, 0, sizeof(gActivityLog));
	gActivityLogHead  = 0;
	gActivityLogCount = 0;
}

void ACTIVITY_Record(const uint32_t freqOrChan, const bool isChannel)
{
	// >> 7 gives ~1.28s per unit: ~23 hours of range in a uint16_t. The MCU has
	// no RTC, so this is uptime-relative by necessity - rendered as "-1h23m" by
	// the host tool rather than as a wall-clock time.
	const uint16_t now  = (uint16_t)(SCHEDULER_UptimeTicks() >> 7);
	const uint8_t  rssi = (uint8_t)(BK4819_GetRSSI() >> 1);

	// Coalesce: a repeater held open must not fill the ring with one
	// transmission. Same frequency, recent enough -> refresh in place.
	if (gActivityLogCount > 0)
	{
		ActivityEntry_t *head = &gActivityLog[gActivityLogHead];
		if (head->freqOrChan == freqOrChan &&
		    (uint16_t)(now - head->timeTicks) <= ACTIVITY_COALESCE_TICKS)
		{
			head->timeTicks = now;
			if (rssi > head->rssi)      // keep the strongest of the burst
				head->rssi = rssi;
			return;
		}
	}

	gActivityLogHead = (uint8_t)((gActivityLogHead + 1u) % ACTIVITY_LOG_SIZE);
	if (gActivityLogCount < ACTIVITY_LOG_SIZE)
		gActivityLogCount++;

	ActivityEntry_t *e = &gActivityLog[gActivityLogHead];
	e->freqOrChan = freqOrChan;
	e->timeTicks  = now;
	e->rssi       = rssi;
	e->flags      = isChannel ? ACTIVITY_FLAG_IS_CHANNEL : 0u;
}
