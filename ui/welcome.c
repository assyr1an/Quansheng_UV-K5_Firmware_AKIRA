/* Original work Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
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

#include <string.h>

#include "driver/eeprom.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "helper/battery.h"
#include "settings.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/welcome.h"
#include "ui/status.h"
#include "version.h"

void UI_DisplayReleaseKeys(void)
{
	memset(gStatusLine,  0, sizeof(gStatusLine));
	memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

	UI_PrintString("RELEASE", 0, 127, 1, 10);
	UI_PrintString("ALL KEYS", 0, 127, 3, 10);

	ST7565_BlitStatusLine();  // blank status line
	ST7565_BlitFullScreen();
}

void UI_DisplayWelcome(void)
{
	char line[16];
	uint8_t i;

	memset(gStatusLine,  0, sizeof(gStatusLine));
	memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

	if (gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_NONE)
	{
		ST7565_BlitFullScreen();
		return;
	}

	// The secondary line still carries whatever the display mode asked for, so
	// the branding costs no information. Battery volts at power-on is worth
	// keeping in a preparedness build - it is the one moment you always see it.
	memset(line, 0, sizeof(line));
	if (gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_VOLTAGE)
	{
		sprintf(line, "%u.%02uV %u%%",
			gBatteryVoltageAverage / 100,
			gBatteryVoltageAverage % 100,
			BATTERY_VoltsToPercent(gBatteryVoltageAverage));
	}
	else
	{
		EEPROM_ReadBuffer(0x0EC0, line, 16);
	}

	// The mark. Drawn rather than stored: two rules straight into the frame
	// buffer cost a loop and no bitmap data, which matters with ~1 KB of flash
	// left. gFrameBuffer is [7][128], one byte per column, 8 vertical pixels.
	for (i = 0; i < 128; i++)
	{
		gFrameBuffer[0][i] = 0b00011000;   // heavy rule above
		gFrameBuffer[4][i] = 0b00000110;   // lighter rule below
	}

	UI_PrintString("AKIRA", 0, 127, 1, 12);
	UI_PrintStringSmall(line, 0, 128, 5);
	UI_PrintStringSmall(Version, 0, 128, 6);

	ST7565_BlitStatusLine();  // blank status line
	ST7565_BlitFullScreen();
}
