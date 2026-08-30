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

// AKIRA boot logo - style F, scanlines fading downward.
// 82 x 16, column-major, 8 rows per page: the ST7565 frame-buffer
// layout, so it blits with two memcpy's and no bit shuffling.
// Cropped to the columns the word occupies (x offset 23); storing the
// full 128 would waste 92 bytes for nothing.
#define AKIRA_LOGO_W   82
#define AKIRA_LOGO_X   23
static const uint8_t AKIRA_LOGO[2][AKIRA_LOGO_W] = {
	{
		0x40,0x70,0x7C,0x3E,0x0F,0x03,0x03,0x03,0x03,0x0F,0x3E,0x7C,0x70,0x40,0x00,0x00,
		0x00,0x7F,0x7F,0x7F,0x00,0x40,0x60,0x70,0x38,0x1C,0x0E,0x07,0x03,0x01,0x00,0x00,
		0x00,0x00,0x00,0x00,0x03,0x03,0x03,0x7F,0x7F,0x7F,0x7F,0x03,0x03,0x03,0x00,0x00,
		0x00,0x00,0x00,0x7F,0x7F,0x7F,0x43,0x43,0x43,0x43,0x43,0x67,0x7F,0x7E,0x3C,0x00,
		0x00,0x00,0x00,0x00,0x40,0x70,0x7C,0x3E,0x0F,0x03,0x03,0x03,0x03,0x0F,0x3E,0x7C,
		0x70,0x40
	},
	{
		0x57,0x57,0x57,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x57,0x57,0x57,0x00,0x00,
		0x00,0x57,0x57,0x57,0x01,0x03,0x07,0x06,0x14,0x10,0x50,0x40,0x40,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x40,0x40,0x40,0x57,0x57,0x57,0x57,0x40,0x40,0x40,0x00,0x00,
		0x00,0x00,0x00,0x57,0x57,0x57,0x00,0x00,0x01,0x03,0x07,0x06,0x14,0x10,0x50,0x40,
		0x40,0x00,0x00,0x00,0x57,0x57,0x57,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x57,
		0x57,0x57
	}
};

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

	// The logo drops straight into pages 0 and 1 - it is stored in the frame
	// buffer's own column-major layout, so there is no bit shuffling to do.
	memcpy(&gFrameBuffer[0][AKIRA_LOGO_X], AKIRA_LOGO[0], AKIRA_LOGO_W);
	memcpy(&gFrameBuffer[1][AKIRA_LOGO_X], AKIRA_LOGO[1], AKIRA_LOGO_W);

	// A rule under the mark, drawn rather than stored.
	for (i = 8; i < 120; i++)
		gFrameBuffer[3][i] = 0b00000110;

	UI_PrintStringSmall(line, 0, 128, 4);
	UI_PrintStringSmall(Version, 0, 128, 6);

	ST7565_BlitStatusLine();  // blank status line
	ST7565_BlitFullScreen();
}
