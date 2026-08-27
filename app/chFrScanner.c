
#include "app/app.h"
#include "app/activity.h"
#include "app/chFrScanner.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"

int8_t            gScanStateDir;
bool              gScanKeepResult;
bool              gScanPauseMode;

#ifdef ENABLE_SCAN_RANGES
uint32_t          gScanRangeStart;
uint32_t          gScanRangeStop;
#endif

// Priority scanning state. The previous scan_next_chan_t rotation visited
// PRI1, PRI2, normal, normal - so HALF of all scan time went to two channels,
// and one of its four slots (SCAN_NEXT_CHAN_DUAL_WATCH) was entirely dead code.
// Replaced with an interval counter: N normal channels, then one priority check.
static uint8_t      priNormalCount;   // normal channels visited since the last check
static uint8_t      priAlternate;     // alternates PRI1 / PRI2 between checks
uint32_t            initialFrqOrChan;
uint8_t           	initialCROSS_BAND_RX_TX;
uint32_t            lastFoundFrqOrChan;

static void NextFreqChannel(void);
static void NextMemChannel(void);

// Spec 5.7: an unset priority channel reads 0xFF from erased EEPROM, and
// 255 >= 0 passes a bare signed test - the sentinel was only rejected further
// down by RADIO_CheckValidChannel(). Correct behaviour, reached by accident.
// Test it explicitly so both the "disabled" (-1) and "unset" (0xFF) cases are
// rejected on purpose rather than by luck.
static bool IsUsablePriorityChannel(const int ch)
{
	if (ch < 0 || ch > MR_CHANNEL_LAST)     // -1 = disabled, 0xFF = unset
		return false;
	return RADIO_CheckValidChannel((uint16_t)ch, false, 0);
}

void CHFRSCANNER_Start(const bool storeBackupSettings, const int8_t scan_direction)
{
	if (storeBackupSettings) {
		initialCROSS_BAND_RX_TX = gEeprom.CROSS_BAND_RX_TX;
		gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
		gScanKeepResult = false;
	}
	
	RADIO_SelectVfos();

	gNextMrChannel   = gRxVfo->CHANNEL_SAVE;
	priNormalCount   = 0;
	priAlternate     = 0;
	gScanStateDir    = scan_direction;

	if (IS_MR_CHANNEL(gNextMrChannel))
	{	// channel mode
		if (storeBackupSettings) {
			initialFrqOrChan = gRxVfo->CHANNEL_SAVE;
			lastFoundFrqOrChan = initialFrqOrChan;
		}
		NextMemChannel();
	}
	else
	{	// frequency mode
		if (storeBackupSettings) {
			initialFrqOrChan = gRxVfo->freq_config_RX.Frequency;
			lastFoundFrqOrChan = initialFrqOrChan;
		}
		NextFreqChannel();
	}

	gScanPauseDelayIn_10ms = scan_pause_delay_in_2_10ms;
	gScheduleScanListen    = false;
	gRxReceptionMode       = RX_MODE_NONE;
	gScanPauseMode         = false;
}

void CHFRSCANNER_ContinueScanning(void)
{
	if (IS_FREQ_CHANNEL(gNextMrChannel))
	{
		if (gCurrentFunction == FUNCTION_INCOMING)
			APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
		else
			NextFreqChannel();  // switch to next frequency
	}
	else
	{
		if (gCurrentCodeType == CODE_TYPE_OFF && gCurrentFunction == FUNCTION_INCOMING)
			APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
		else
			NextMemChannel();    // switch to next channel
	}
	
	gScanPauseMode      = false;
	gRxReceptionMode    = RX_MODE_NONE;
	gScheduleScanListen = false;
}

void CHFRSCANNER_Found(void)
{
	switch (gEeprom.SCAN_RESUME_MODE)
	{
		case SCAN_RESUME_TO:
			if (!gScanPauseMode)
			{
				gScanPauseDelayIn_10ms = scan_pause_delay_in_1_10ms;
				gScheduleScanListen    = false;
				gScanPauseMode         = true;
			}
			break;

		case SCAN_RESUME_CO:
		case SCAN_RESUME_SE:
			gScanPauseDelayIn_10ms = 0;
			gScheduleScanListen    = false;
			break;
	}

	if (IS_MR_CHANNEL(gRxVfo->CHANNEL_SAVE)) { //memory scan
		lastFoundFrqOrChan = gRxVfo->CHANNEL_SAVE;
	}
	else { // frequency scan
		lastFoundFrqOrChan = gRxVfo->freq_config_RX.Frequency;
	}

	// Feature #4. This is the single call site for scan hits, and it has already
	// worked out what we want to record. Note the coverage limit documented in
	// activity.h: the spectrum analyser never reaches here.
	ACTIVITY_Record(lastFoundFrqOrChan, IS_MR_CHANNEL(gRxVfo->CHANNEL_SAVE));

	gScanKeepResult = true;
}

void CHFRSCANNER_Stop(void)
{
	if(initialCROSS_BAND_RX_TX != CROSS_BAND_OFF) {
		gEeprom.CROSS_BAND_RX_TX = initialCROSS_BAND_RX_TX;
		initialCROSS_BAND_RX_TX = CROSS_BAND_OFF;
	}
	
	gScanStateDir = SCAN_OFF;

	const uint32_t chFr = gScanKeepResult ? lastFoundFrqOrChan : initialFrqOrChan;
	const bool channelChanged = chFr != initialFrqOrChan;
	if (IS_MR_CHANNEL(gNextMrChannel)) {
		gEeprom.MrChannel[gEeprom.RX_VFO]     = chFr;
		gEeprom.ScreenChannel[gEeprom.RX_VFO] = chFr;
		RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);

		if(channelChanged) {
			SETTINGS_SaveVfoIndices();
			gUpdateStatus = true;
		}
	}
	else {
		gRxVfo->freq_config_RX.Frequency = chFr;
		RADIO_ApplyTxOffset(gRxVfo);
		RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
		if(channelChanged) {
			SETTINGS_SaveChannel(gRxVfo->CHANNEL_SAVE, gEeprom.RX_VFO, gRxVfo, 1);
		}
	}

	RADIO_SetupRegisters(true);
	gUpdateDisplay = true;
}

static void NextFreqChannel(void)
{
#ifdef ENABLE_SCAN_RANGES
	if(gScanRangeStart) {
		gRxVfo->freq_config_RX.Frequency = APP_SetFreqByStepAndLimits(gRxVfo, gScanStateDir, gScanRangeStart, gScanRangeStop);
	}
	else
#endif
		gRxVfo->freq_config_RX.Frequency = APP_SetFrequencyByStep(gRxVfo, gScanStateDir);

	RADIO_ApplyTxOffset(gRxVfo);
	RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
	RADIO_SetupRegisters(true);

#ifdef ENABLE_FASTER_CHANNEL_SCAN
	gScanPauseDelayIn_10ms = 9;   // 90ms
#else
	gScanPauseDelayIn_10ms = scan_pause_delay_in_6_10ms;
#endif

	gUpdateDisplay     = true;
}

static void NextMemChannel(void)
{
	const bool         enabled   = (gEeprom.SCAN_LIST_DEFAULT < 2) ? gEeprom.SCAN_LIST_ENABLED[gEeprom.SCAN_LIST_DEFAULT] : true;
	const int          chan1     = (gEeprom.SCAN_LIST_DEFAULT < 2) ? gEeprom.SCANLIST_PRIORITY_CH1[gEeprom.SCAN_LIST_DEFAULT] : -1;
	const int          chan2     = (gEeprom.SCAN_LIST_DEFAULT < 2) ? gEeprom.SCANLIST_PRIORITY_CH2[gEeprom.SCAN_LIST_DEFAULT] : -1;
	const unsigned int prev_chan = gNextMrChannel;
	unsigned int       chan      = 0xFF;

	// Priority checking is off when the scan list is disabled, when
	// SCAN_LIST_DEFAULT == 2 ("all channels", where chan1/chan2 are -1 by
	// design), or when the user has set the interval to 0.
	if (enabled && gEeprom.PRIORITY_INTERVAL > 0)
	{
		if (priNormalCount >= gEeprom.PRIORITY_INTERVAL)
		{
			// Alternate between the two priority channels so that having a
			// second one does not halve the first one's revisit rate.
			const int first  = priAlternate ? chan2 : chan1;
			const int second = priAlternate ? chan1 : chan2;

			if (IsUsablePriorityChannel(first))
				chan = (unsigned int)first;
			else if (IsUsablePriorityChannel(second))
				chan = (unsigned int)second;

			if (chan != 0xFF)
			{
				priNormalCount = 0;
				priAlternate  ^= 1;
				gNextMrChannel = chan;
			}
		}
		else
		{
			priNormalCount++;
		}
	}

	if (chan == 0xFF)
	{	// normal channel step
		chan = RADIO_FindNextChannel(gNextMrChannel + gScanStateDir, gScanStateDir, (gEeprom.SCAN_LIST_DEFAULT < 2) ? true : false, gEeprom.SCAN_LIST_DEFAULT);
		if (chan == 0xFF)
		{	// no valid channel found
			chan = MR_CHANNEL_FIRST;
		}

		gNextMrChannel = chan;
	}

	if (gNextMrChannel != prev_chan)
	{
		gEeprom.MrChannel[    gEeprom.RX_VFO] = gNextMrChannel;
		gEeprom.ScreenChannel[gEeprom.RX_VFO] = gNextMrChannel;

		RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);
		RADIO_SetupRegisters(true);

		gUpdateDisplay = true;
	}

#ifdef ENABLE_FASTER_CHANNEL_SCAN
	gScanPauseDelayIn_10ms = 9;  // 90ms .. <= ~60ms it misses signals (squelch response and/or PLL lock time) ?
#else
	gScanPauseDelayIn_10ms = scan_pause_delay_in_3_10ms;
#endif
}
