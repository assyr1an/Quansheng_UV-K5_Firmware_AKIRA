/* Copyright 2026 assyr1an
 * https://github.com/assyr1an
 *
 * Part of AKIRA, a fork of kamilsss655/uv-k5-firmware-custom.
 * This file is original to AKIRA and is not derived from upstream work.
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
