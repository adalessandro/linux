#include "headers.h"

#define STATUS_IMAGE_CHECKSUM_MISMATCH -199
#define EVENT_SIGNALED 1

static B_UINT16 CFG_CalculateChecksum(B_UINT8 *pu8Buffer, B_UINT32 u32Size)
{
	B_UINT16 u16CheckSum = 0;

	while (u32Size--) {
		u16CheckSum += (B_UINT8)~(*pu8Buffer);
		pu8Buffer++;
	}
	return u16CheckSum;
}

bool IsReqGpioIsLedInNVM(struct bcm_mini_adapter *Adapter, UINT gpios)
{
	INT Status;

	Status = (Adapter->gpioBitMap & gpios) ^ gpios;
	if (Status)
		return false;
	else
		return TRUE;
}

static INT LED_Blink(struct bcm_mini_adapter *Adapter,
		     UINT GPIO_Num,
		     UCHAR uiLedIndex,
		     ULONG timeout,
		     INT num_of_time,
		     enum bcm_led_events currdriverstate)
{
	int Status = STATUS_SUCCESS;
	bool bInfinite = false;

	/* Check if num_of_time is -ve. If yes, blink led in infinite loop */
	if (num_of_time < 0) {
		bInfinite = TRUE;
		num_of_time = 1;
	}
	while (num_of_time) {
		if (currdriverstate == Adapter->DriverState)
			TURN_ON_LED(Adapter, GPIO_Num, uiLedIndex);

		/* Wait for timeout after setting on the LED */
		Status = wait_event_interruptible_timeout(
				Adapter->LEDInfo.notify_led_event,
				currdriverstate != Adapter->DriverState ||
					kthread_should_stop(),
				msecs_to_jiffies(timeout));

		if (kthread_should_stop()) {
			BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL,
				"Led thread got signal to exit..hence exiting");
			Adapter->LEDInfo.led_thread_running =
					BCM_LED_THREAD_DISABLED;
			TURN_OFF_LED(Adapter, GPIO_Num, uiLedIndex);
			Status = EVENT_SIGNALED;
			break;
		}
		if (Status) {
			TURN_OFF_LED(Adapter, GPIO_Num, uiLedIndex);
			Status = EVENT_SIGNALED;
			break;
		}

		TURN_OFF_LED(Adapter, GPIO_Num, uiLedIndex);
		Status = wait_event_interruptible_timeout(
				Adapter->LEDInfo.notify_led_event,
				currdriverstate != Adapter->DriverState ||
					kthread_should_stop(),
				msecs_to_jiffies(timeout));
		if (bInfinite == false)
			num_of_time--;
	}
	return Status;
}

static INT ScaleRateofTransfer(ULONG rate)
{
	if (rate <= 3)
		return rate;
	else if ((rate > 3) && (rate <= 100))
		return 5;
	else if ((rate > 100) && (rate <= 200))
		return 6;
	else if ((rate > 200) && (rate <= 300))
		return 7;
	else if ((rate > 300) && (rate <= 400))
		return 8;
	else if ((rate > 400) && (rate <= 500))
		return 9;
	else if ((rate > 500) && (rate <= 600))
		return 10;
	else
		return MAX_NUM_OF_BLINKS;
}

static INT blink_in_normal_bandwidth(struct bcm_mini_adapter *ad,
				     INT *time,
				     INT *time_tx,
				     INT *time_rx,
				     UCHAR GPIO_Num_tx,
				     UCHAR uiTxLedIndex,
				     UCHAR GPIO_Num_rx,
				     UCHAR uiRxLedIndex,
				     enum bcm_led_events currdriverstate,
				     ulong *timeout)
{
	/*
	 * Assign minimum number of blinks of
	 * either Tx or Rx.
	 */
	*time = (*time_tx > *time_rx ? *time_rx : *time_tx);

	if (*time > 0) {
		/* Blink both Tx and Rx LEDs */
		if ((LED_Blink(ad, 1 << GPIO_Num_tx, uiTxLedIndex, *timeout,
			      *time, currdriverstate) == EVENT_SIGNALED) ||
		    (LED_Blink(ad, 1 << GPIO_Num_rx, uiRxLedIndex, *timeout,
			      *time, currdriverstate) == EVENT_SIGNALED))
			return EVENT_SIGNALED;
	}

	if (*time == *time_tx) {
		/* Blink pending rate of Rx */
		if (LED_Blink(ad, (1 << GPIO_Num_rx), uiRxLedIndex, *timeout,
			      *time_rx - *time,
			      currdriverstate) == EVENT_SIGNALED)
			return EVENT_SIGNALED;

		*time = *time_rx;
	} else {
		/* Blink pending rate of Tx */
		if (LED_Blink(ad, 1 << GPIO_Num_tx, uiTxLedIndex, *timeout,
			      *time_tx - *time,
			      currdriverstate) == EVENT_SIGNALED)
			return EVENT_SIGNALED;

		*time = *time_tx;
	}

	return 0;
}

static INT LED_Proportional_Blink(struct bcm_mini_adapter *Adapter,
				  UCHAR GPIO_Num_tx,
				  UCHAR uiTxLedIndex,
				  UCHAR GPIO_Num_rx,
				  UCHAR uiRxLedIndex,
				  enum bcm_led_events currdriverstate)
{
	/* Initial values of TX and RX packets */
	ULONG64 Initial_num_of_packts_tx = 0, Initial_num_of_packts_rx = 0;
	/* values of TX and RX packets after 1 sec */
	ULONG64 Final_num_of_packts_tx = 0, Final_num_of_packts_rx = 0;
	/* Rate of transfer of Tx and Rx in 1 sec */
	ULONG64 rate_of_transfer_tx = 0, rate_of_transfer_rx = 0;
	int Status = STATUS_SUCCESS;
	INT num_of_time = 0, num_of_time_tx = 0, num_of_time_rx = 0;
	UINT remDelay = 0;
	/* UINT GPIO_num = DISABLE_GPIO_NUM; */
	ulong timeout = 0;

	/* Read initial value of packets sent/received */
	Initial_num_of_packts_tx = Adapter->dev->stats.tx_packets;
	Initial_num_of_packts_rx = Adapter->dev->stats.rx_packets;

	/* Scale the rate of transfer to no of blinks. */
	num_of_time_tx = ScaleRateofTransfer((ULONG)rate_of_transfer_tx);
	num_of_time_rx = ScaleRateofTransfer((ULONG)rate_of_transfer_rx);

	while ((Adapter->device_removed == false)) {
		timeout = 50;

		if (EVENT_SIGNALED == blink_in_normal_bandwidth(Adapter,
								&num_of_time,
								&num_of_time_tx,
								&num_of_time_rx,
								GPIO_Num_tx,
								uiTxLedIndex,
								GPIO_Num_rx,
								uiRxLedIndex,
								currdriverstate,
								&timeout))
			return EVENT_SIGNALED;


		/*
		 * If Tx/Rx rate is less than maximum blinks per second,
		 * wait till delay completes to 1 second
		 */
		remDelay = MAX_NUM_OF_BLINKS - num_of_time;
		if (remDelay > 0) {
			timeout = 100 * remDelay;
			Status = wait_event_interruptible_timeout(
					Adapter->LEDInfo.notify_led_event,
					currdriverstate != Adapter->DriverState
						|| kthread_should_stop(),
					msecs_to_jiffies(timeout));

			if (kthread_should_stop()) {
				BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS,
					LED_DUMP_INFO, DBG_LVL_ALL,
					"Led thread got signal to exit..hence exiting");
				Adapter->LEDInfo.led_thread_running =
						BCM_LED_THREAD_DISABLED;
				return EVENT_SIGNALED;
			}
			if (Status)
				return EVENT_SIGNALED;
		}

		/* Turn off both Tx and Rx LEDs before next second */
		TURN_OFF_LED(Adapter, 1 << GPIO_Num_tx, uiTxLedIndex);
		TURN_OFF_LED(Adapter, 1 << GPIO_Num_rx, uiTxLedIndex);

		/*
		 * Read the Tx & Rx packets transmission after 1 second and
		 * calculate rate of transfer
		 */
		Final_num_of_packts_tx = Adapter->dev->stats.tx_packets;
		Final_num_of_packts_rx = Adapter->dev->stats.rx_packets;

		rate_of_transfer_tx = Final_num_of_packts_tx -
						Initial_num_of_packts_tx;
		rate_of_transfer_rx = Final_num_of_packts_rx -
						Initial_num_of_packts_rx;

		/* Read initial value of packets sent/received */
		Initial_num_of_packts_tx = Final_num_of_packts_tx;
		Initial_num_of_packts_rx = Final_num_of_packts_rx;

		/* Scale the rate of transfer to no of blinks. */
		num_of_time_tx =
			ScaleRateofTransfer((ULONG)rate_of_transfer_tx);
		num_of_time_rx =
			ScaleRateofTransfer((ULONG)rate_of_transfer_rx);

	}
	return Status;
}

/*
 * -----------------------------------------------------------------------------
 * Procedure:   ValidateDSDParamsChecksum
 *
 * Description: Reads DSD Params and validates checkusm.
 *
 * Arguments:
 *      Adapter - Pointer to Adapter structure.
 *      ulParamOffset - Start offset of the DSD parameter to be read and
 *			validated.
 *      usParamLen - Length of the DSD Parameter.
 *
 * Returns:
 *  <OSAL_STATUS_CODE>
 * -----------------------------------------------------------------------------
 */
static INT ValidateDSDParamsChecksum(struct bcm_mini_adapter *Adapter,
				     ULONG ulParamOffset,
				     USHORT usParamLen)
{
	INT Status = STATUS_SUCCESS;
	PUCHAR puBuffer = NULL;
	USHORT usChksmOrg = 0;
	USHORT usChecksumCalculated = 0;

	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"LED Thread:ValidateDSDParamsChecksum: 0x%lx 0x%X",
			ulParamOffset, usParamLen);

	puBuffer = kmalloc(usParamLen, GFP_KERNEL);
	if (!puBuffer) {
		BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL,
				"LED Thread: ValidateDSDParamsChecksum Allocation failed");
		return -ENOMEM;

	}

	/* Read the DSD data from the parameter offset. */
	if (STATUS_SUCCESS != BeceemNVMRead(Adapter, (PUINT)puBuffer,
					    ulParamOffset, usParamLen)) {
		BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL,
				"LED Thread: ValidateDSDParamsChecksum BeceemNVMRead failed");
		Status = STATUS_IMAGE_CHECKSUM_MISMATCH;
		goto exit;
	}

	/* Calculate the checksum of the data read from the DSD parameter. */
	usChecksumCalculated = CFG_CalculateChecksum(puBuffer, usParamLen);
	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"LED Thread: usCheckSumCalculated = 0x%x\n",
			usChecksumCalculated);

	/*
	 * End of the DSD parameter will have a TWO bytes checksum stored in it.
	 * Read it and compare with the calculated Checksum.
	 */
	if (STATUS_SUCCESS != BeceemNVMRead(Adapter, (PUINT)&usChksmOrg,
					    ulParamOffset+usParamLen, 2)) {
		BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL,
				"LED Thread: ValidateDSDParamsChecksum BeceemNVMRead failed");
		Status = STATUS_IMAGE_CHECKSUM_MISMATCH;
		goto exit;
	}
	usChksmOrg = ntohs(usChksmOrg);
	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"LED Thread: usChksmOrg = 0x%x", usChksmOrg);

	/*
	 * Compare the checksum calculated with the checksum read
	 * from DSD section
	 */
	if (usChecksumCalculated ^ usChksmOrg) {
		BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL,
				"LED Thread: ValidateDSDParamsChecksum: Checksums don't match");
		Status = STATUS_IMAGE_CHECKSUM_MISMATCH;
		goto exit;
	}

exit:
	kfree(puBuffer);
	return Status;
}


/*
 * -----------------------------------------------------------------------------
 * Procedure:   ValidateHWParmStructure
 *
 * Description: Validates HW Parameters.
 *
 * Arguments:
 *      Adapter - Pointer to Adapter structure.
 *      ulHwParamOffset - Start offset of the HW parameter Section to be read
 *				and validated.
 *
 * Returns:
 *  <OSAL_STATUS_CODE>
 * -----------------------------------------------------------------------------
 */
static INT ValidateHWParmStructure(struct bcm_mini_adapter *Adapter,
				   ULONG ulHwParamOffset)
{

	INT Status = STATUS_SUCCESS;
	USHORT HwParamLen = 0;
	/*
	 * Add DSD start offset to the hwParamOffset to get
	 * the actual address.
	 */
	ulHwParamOffset += DSD_START_OFFSET;

	/* Read the Length of HW_PARAM structure */
	BeceemNVMRead(Adapter, (PUINT)&HwParamLen, ulHwParamOffset, 2);
	HwParamLen = ntohs(HwParamLen);
	if (0 == HwParamLen || HwParamLen > Adapter->uiNVMDSDSize)
		return STATUS_IMAGE_CHECKSUM_MISMATCH;

	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"LED Thread:HwParamLen = 0x%x", HwParamLen);
	Status = ValidateDSDParamsChecksum(Adapter, ulHwParamOffset,
					   HwParamLen);
	return Status;
} /* ValidateHWParmStructure() */

static int ReadLEDInformationFromEEPROM(struct bcm_mini_adapter *Adapter,
					UCHAR GPIO_Array[])
{
	int Status = STATUS_SUCCESS;

	ULONG  dwReadValue	= 0;
	USHORT usHwParamData	= 0;
	USHORT usEEPROMVersion	= 0;
	UCHAR  ucIndex		= 0;
	UCHAR  ucGPIOInfo[32]	= {0};

	BeceemNVMRead(Adapter, (PUINT)&usEEPROMVersion,
		      EEPROM_VERSION_OFFSET, 2);

	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"usEEPROMVersion: Minor:0x%X Major:0x%x",
			usEEPROMVersion & 0xFF,
			((usEEPROMVersion >> 8) & 0xFF));


	if (((usEEPROMVersion>>8)&0xFF) < EEPROM_MAP5_MAJORVERSION) {
		BeceemNVMRead(Adapter, (PUINT)&usHwParamData,
			      EEPROM_HW_PARAM_POINTER_ADDRESS, 2);
		usHwParamData = ntohs(usHwParamData);
		dwReadValue   = usHwParamData;
	} else {
		/*
		 * Validate Compatibility section and then read HW param
		 * if compatibility section is valid.
		 */
		Status = ValidateDSDParamsChecksum(Adapter,
						   DSD_START_OFFSET,
						   COMPATIBILITY_SECTION_LENGTH_MAP5);

		if (Status != STATUS_SUCCESS)
			return Status;

		BeceemNVMRead(Adapter, (PUINT)&dwReadValue,
			      EEPROM_HW_PARAM_POINTER_ADDRRES_MAP5, 4);
		dwReadValue = ntohl(dwReadValue);
	}


	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"LED Thread: Start address of HW_PARAM structure = 0x%lx",
			dwReadValue);

	/*
	 * Validate if the address read out is within the DSD.
	 * Adapter->uiNVMDSDSize gives whole DSD size inclusive of Autoinit.
	 * lower limit should be above DSD_START_OFFSET and
	 * upper limit should be below (Adapter->uiNVMDSDSize-DSD_START_OFFSET)
	 */
	if (dwReadValue < DSD_START_OFFSET ||
			dwReadValue > (Adapter->uiNVMDSDSize-DSD_START_OFFSET))
		return STATUS_IMAGE_CHECKSUM_MISMATCH;

	Status = ValidateHWParmStructure(Adapter, dwReadValue);
	if (Status)
		return Status;

	/*
	 * Add DSD_START_OFFSET to the offset read from the EEPROM.
	 * This will give the actual start HW Parameters start address.
	 * To read GPIO section, add GPIO offset further.
	 */

	dwReadValue += DSD_START_OFFSET;
			/* = start address of hw param section. */
	dwReadValue += GPIO_SECTION_START_OFFSET;
			/* = GPIO start offset within HW Param section. */

	/*
	 * Read the GPIO values for 32 GPIOs from EEPROM and map the function
	 * number to GPIO pin number to GPIO_Array
	 */
	BeceemNVMRead(Adapter, (UINT *)ucGPIOInfo, dwReadValue, 32);
	for (ucIndex = 0; ucIndex < 32; ucIndex++) {

		switch (ucGPIOInfo[ucIndex]) {
		case RED_LED:
			GPIO_Array[RED_LED] = ucIndex;
			Adapter->gpioBitMap |= (1 << ucIndex);
			break;
		case BLUE_LED:
			GPIO_Array[BLUE_LED] = ucIndex;
			Adapter->gpioBitMap |= (1 << ucIndex);
			break;
		case YELLOW_LED:
			GPIO_Array[YELLOW_LED] = ucIndex;
			Adapter->gpioBitMap |= (1 << ucIndex);
			break;
		case GREEN_LED:
			GPIO_Array[GREEN_LED] = ucIndex;
			Adapter->gpioBitMap |= (1 << ucIndex);
			break;
		default:
			break;
		}

	}
	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"GPIO's bit map correspond to LED :0x%X",
			Adapter->gpioBitMap);
	return Status;
}


static int ReadConfigFileStructure(struct bcm_mini_adapter *Adapter,
				   bool *bEnableThread)
{
	int Status = STATUS_SUCCESS;
	/* Array to store GPIO numbers from EEPROM */
	UCHAR GPIO_Array[NUM_OF_LEDS+1];
	UINT uiIndex = 0;
	UINT uiNum_of_LED_Type = 0;
	PUCHAR puCFGData	= NULL;
	UCHAR bData = 0;
	struct bcm_led_state_info *curr_led_state;

	memset(GPIO_Array, DISABLE_GPIO_NUM, NUM_OF_LEDS+1);

	if (!Adapter->pstargetparams || IS_ERR(Adapter->pstargetparams)) {
		BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL, "Target Params not Avail.\n");
		return -ENOENT;
	}

	/* Populate GPIO_Array with GPIO numbers for LED functions */
	/* Read the GPIO numbers from EEPROM */
	Status = ReadLEDInformationFromEEPROM(Adapter, GPIO_Array);
	if (Status == STATUS_IMAGE_CHECKSUM_MISMATCH) {
		*bEnableThread = false;
		return STATUS_SUCCESS;
	} else if (Status) {
		*bEnableThread = false;
		return Status;
	}

	/*
	 * CONFIG file read successfully. Deallocate the memory of
	 * uiFileNameBufferSize
	 */
	BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO, DBG_LVL_ALL,
			"LED Thread: Config file read successfully\n");
	puCFGData = (PUCHAR) &Adapter->pstargetparams->HostDrvrConfig1;

	/*
	 * Offset for HostDrvConfig1, HostDrvConfig2, HostDrvConfig3 which
	 * will have the information of LED type, LED on state for different
	 * driver state and LED blink state.
	 */

	for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++) {
		bData = *puCFGData;
		curr_led_state = &Adapter->LEDInfo.LEDState[uiIndex];

		/*
		 * Check Bit 8 for polarity. If it is set,
		 * polarity is reverse polarity
		 */
		if (bData & 0x80) {
			curr_led_state->BitPolarity = 0;
			/* unset the bit 8 */
			bData = bData & 0x7f;
		}

		curr_led_state->LED_Type = bData;
		if (bData <= NUM_OF_LEDS)
			curr_led_state->GPIO_Num = GPIO_Array[bData];
		else
			curr_led_state->GPIO_Num = DISABLE_GPIO_NUM;

		puCFGData++;
		bData = *puCFGData;
		curr_led_state->LED_On_State = bData;
		puCFGData++;
		bData = *puCFGData;
		curr_led_state->LED_Blink_State = bData;
		puCFGData++;
	}

	/*
	 * Check if all the LED settings are disabled. If it is disabled,
	 * dont launch the LED control thread.
	 */
	for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++) {
		curr_led_state = &Adapter->LEDInfo.LEDState[uiIndex];

		if ((curr_led_state->LED_Type == DISABLE_GPIO_NUM) ||
			(curr_led_state->LED_Type == 0x7f) ||
			(curr_led_state->LED_Type == 0))
			uiNum_of_LED_Type++;
	}
	if (uiNum_of_LED_Type >= NUM_OF_LEDS)
		*bEnableThread = false;

	return Status;
}

/*
 * -----------------------------------------------------------------------------
 * Procedure:   LedGpioInit
 *
 * Description: Initializes LED GPIOs. Makes the LED GPIOs to OUTPUT mode
 *			  and make the initial state to be OFF.
 *
 * Arguments:
 *      Adapter - Pointer to MINI_ADAPTER structure.
 *
 * Returns: VOID
 *
 * -----------------------------------------------------------------------------
 */
static VOID LedGpioInit(struct bcm_mini_adapter *Adapter)
{
	UINT uiResetValue = 0;
	UINT uiIndex      = 0;
	struct bcm_led_state_info *curr_led_state;

	/* Set all LED GPIO Mode to output mode */
	if (rdmalt(Adapter, GPIO_MODE_REGISTER, &uiResetValue,
		   sizeof(uiResetValue)) < 0)
		BCM_DEBUG_PRINT (Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
			DBG_LVL_ALL, "LED Thread: RDM Failed\n");
	for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++) {
		curr_led_state = &Adapter->LEDInfo.LEDState[uiIndex];

		if (curr_led_state->GPIO_Num != DISABLE_GPIO_NUM)
			uiResetValue |= (1 << curr_led_state->GPIO_Num);

		TURN_OFF_LED(Adapter, 1 << curr_led_state->GPIO_Num, uiIndex);

	}
	if (wrmalt(Adapter, GPIO_MODE_REGISTER, &uiResetValue,
		   sizeof(uiResetValue)) < 0)
		BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL, "LED Thread: WRM Failed\n");

	Adapter->LEDInfo.bIdle_led_off = false;
}

static INT BcmGetGPIOPinInfo(struct bcm_mini_adapter *Adapter,
			     UCHAR *GPIO_num_tx,
			     UCHAR *GPIO_num_rx,
			     UCHAR *uiLedTxIndex,
			     UCHAR *uiLedRxIndex,
			     enum bcm_led_events currdriverstate)
{
	UINT uiIndex = 0;
	struct bcm_led_state_info *led_state_info;

	*GPIO_num_tx = DISABLE_GPIO_NUM;
	*GPIO_num_rx = DISABLE_GPIO_NUM;

	for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++) {
		led_state_info = &Adapter->LEDInfo.LEDState[uiIndex];

		if (((currdriverstate == NORMAL_OPERATION) ||
			(currdriverstate == IDLEMODE_EXIT) ||
			(currdriverstate == FW_DOWNLOAD)) &&
		    (led_state_info->LED_Blink_State & currdriverstate) &&
		    (led_state_info->GPIO_Num != DISABLE_GPIO_NUM)) {
			if (*GPIO_num_tx == DISABLE_GPIO_NUM) {
				*GPIO_num_tx = led_state_info->GPIO_Num;
				*uiLedTxIndex = uiIndex;
			} else {
				*GPIO_num_rx = led_state_info->GPIO_Num;
				*uiLedRxIndex = uiIndex;
			}
		} else {
			if ((led_state_info->LED_On_State & currdriverstate) &&
			    (led_state_info->GPIO_Num != DISABLE_GPIO_NUM)) {
				*GPIO_num_tx = led_state_info->GPIO_Num;
				*uiLedTxIndex = uiIndex;
			}
		}
	}
	return STATUS_SUCCESS;
}

static void handle_adapter_driver_state(struct bcm_mini_adapter *ad,
					enum bcm_led_events currdriverstate,
					UCHAR GPIO_num,
					UCHAR dummyGPIONum,
					UCHAR uiLedIndex,
					UCHAR dummyIndex,
					ulong timeout,
					UINT uiResetValue,
					UINT uiIndex)
{
	switch (ad->DriverState) {
	case DRIVER_INIT:
		currdriverstate = DRIVER_INIT;
				/* ad->DriverState; */
		BcmGetGPIOPinInfo(ad, &GPIO_num, &dummyGPIONum,
				  &uiLedIndex, &dummyIndex,
				  currdriverstate);

		if (GPIO_num != DISABLE_GPIO_NUM)
			TURN_ON_LED(ad, 1 << GPIO_num, uiLedIndex);

		break;
	case FW_DOWNLOAD:
		/*
		 * BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS,
		 *	LED_DUMP_INFO, DBG_LVL_ALL,
		 *	"LED Thread: FW_DN_DONE called\n");
		 */
		currdriverstate = FW_DOWNLOAD;
		BcmGetGPIOPinInfo(ad, &GPIO_num, &dummyGPIONum,
				  &uiLedIndex, &dummyIndex,
				  currdriverstate);

		if (GPIO_num != DISABLE_GPIO_NUM) {
			timeout = 50;
			LED_Blink(ad, 1 << GPIO_num, uiLedIndex, timeout,
				  -1, currdriverstate);
		}
		break;
	case FW_DOWNLOAD_DONE:
		currdriverstate = FW_DOWNLOAD_DONE;
		BcmGetGPIOPinInfo(ad, &GPIO_num, &dummyGPIONum,
				  &uiLedIndex, &dummyIndex, currdriverstate);
		if (GPIO_num != DISABLE_GPIO_NUM)
			TURN_ON_LED(ad, 1 << GPIO_num, uiLedIndex);
		break;

	case SHUTDOWN_EXIT:
		/*
		 * no break, continue to NO_NETWORK_ENTRY
		 * state as well.
		 */
	case NO_NETWORK_ENTRY:
		currdriverstate = NO_NETWORK_ENTRY;
		BcmGetGPIOPinInfo(ad, &GPIO_num, &dummyGPIONum,
				  &uiLedIndex, &dummyGPIONum, currdriverstate);
		if (GPIO_num != DISABLE_GPIO_NUM)
			TURN_ON_LED(ad, 1 << GPIO_num, uiLedIndex);
		break;
	case NORMAL_OPERATION:
		{
			UCHAR GPIO_num_tx = DISABLE_GPIO_NUM;
			UCHAR GPIO_num_rx = DISABLE_GPIO_NUM;
			UCHAR uiLEDTx = 0;
			UCHAR uiLEDRx = 0;

			currdriverstate = NORMAL_OPERATION;
			ad->LEDInfo.bIdle_led_off = false;

			BcmGetGPIOPinInfo(ad, &GPIO_num_tx, &GPIO_num_rx,
					  &uiLEDTx, &uiLEDRx, currdriverstate);
			if ((GPIO_num_tx == DISABLE_GPIO_NUM) &&
					(GPIO_num_rx == DISABLE_GPIO_NUM)) {
				GPIO_num = DISABLE_GPIO_NUM;
			} else {
				/*
				 * If single LED is selected, use same
				 * for both Tx and Rx
				 */
				if (GPIO_num_tx == DISABLE_GPIO_NUM) {
					GPIO_num_tx = GPIO_num_rx;
					uiLEDTx = uiLEDRx;
				} else if (GPIO_num_rx == DISABLE_GPIO_NUM) {
					GPIO_num_rx = GPIO_num_tx;
					uiLEDRx = uiLEDTx;
				}
				/*
				 * Blink the LED in proportionate
				 * to Tx and Rx transmissions.
				 */
				LED_Proportional_Blink(ad,
						       GPIO_num_tx, uiLEDTx,
						       GPIO_num_rx, uiLEDRx,
						       currdriverstate);
			}
		}
		break;
	case LOWPOWER_MODE_ENTER:
		currdriverstate = LOWPOWER_MODE_ENTER;
		if (DEVICE_POWERSAVE_MODE_AS_MANUAL_CLOCK_GATING ==
				ad->ulPowerSaveMode) {
			/* Turn OFF all the LED */
			uiResetValue = 0;
			for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++) {
				if (ad->LEDInfo.LEDState[uiIndex].GPIO_Num != DISABLE_GPIO_NUM)
					TURN_OFF_LED(ad,
						     (1 << ad->LEDInfo.LEDState[uiIndex].GPIO_Num),
						     uiIndex);
			}

		}
		/* Turn off LED And WAKE-UP for Sendinf IDLE mode ACK */
		ad->LEDInfo.bLedInitDone = false;
		ad->LEDInfo.bIdle_led_off = TRUE;
		wake_up(&ad->LEDInfo.idleModeSyncEvent);
		GPIO_num = DISABLE_GPIO_NUM;
		break;
	case IDLEMODE_CONTINUE:
		currdriverstate = IDLEMODE_CONTINUE;
		GPIO_num = DISABLE_GPIO_NUM;
		break;
	case IDLEMODE_EXIT:
		break;
	case DRIVER_HALT:
		currdriverstate = DRIVER_HALT;
		GPIO_num = DISABLE_GPIO_NUM;
		for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++) {
			if (ad->LEDInfo.LEDState[uiIndex].GPIO_Num !=
					DISABLE_GPIO_NUM)
				TURN_OFF_LED(ad,
					     (1 << ad->LEDInfo.LEDState[uiIndex].GPIO_Num),
					     uiIndex);
		}
		/* ad->DriverState = DRIVER_INIT; */
		break;
	case LED_THREAD_INACTIVE:
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL, "InActivating LED thread...");
		currdriverstate = LED_THREAD_INACTIVE;
		ad->LEDInfo.led_thread_running =
				BCM_LED_THREAD_RUNNING_INACTIVELY;
		ad->LEDInfo.bLedInitDone = false;
		/* disable ALL LED */
		for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++) {
			if (ad->LEDInfo.LEDState[uiIndex].GPIO_Num !=
					DISABLE_GPIO_NUM)
				TURN_OFF_LED(ad,
					     (1 << ad->LEDInfo.LEDState[uiIndex].GPIO_Num),
					     uiIndex);
		}
		break;
	case LED_THREAD_ACTIVE:
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL, "Activating LED thread again...");
		if (ad->LinkUpStatus == false)
			ad->DriverState = NO_NETWORK_ENTRY;
		else
			ad->DriverState = NORMAL_OPERATION;

		ad->LEDInfo.led_thread_running =
				BCM_LED_THREAD_RUNNING_ACTIVELY;
		break;
		/* return; */
	default:
		break;
	}
}

static VOID LEDControlThread(struct bcm_mini_adapter *Adapter)
{
	UINT uiIndex = 0;
	UCHAR GPIO_num = 0;
	UCHAR uiLedIndex = 0;
	UINT uiResetValue = 0;
	enum bcm_led_events currdriverstate = 0;
	ulong timeout = 0;

	INT Status = 0;

	UCHAR dummyGPIONum = 0;
	UCHAR dummyIndex = 0;

	/* currdriverstate = Adapter->DriverState; */
	Adapter->LEDInfo.bIdleMode_tx_from_host = false;

	/*
	 * Wait till event is triggered
	 *
	 * wait_event(Adapter->LEDInfo.notify_led_event,
	 *	currdriverstate!= Adapter->DriverState);
	 */

	GPIO_num = DISABLE_GPIO_NUM;

	while (TRUE) {
		/* Wait till event is triggered */
		if ((GPIO_num == DISABLE_GPIO_NUM)
						||
				((currdriverstate != FW_DOWNLOAD) &&
				 (currdriverstate != NORMAL_OPERATION) &&
				 (currdriverstate != LOWPOWER_MODE_ENTER))
						||
				(currdriverstate == LED_THREAD_INACTIVE))
			Status = wait_event_interruptible(
					Adapter->LEDInfo.notify_led_event,
					currdriverstate != Adapter->DriverState
						|| kthread_should_stop());

		if (kthread_should_stop() || Adapter->device_removed) {
			BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL,
				"Led thread got signal to exit..hence exiting");
			Adapter->LEDInfo.led_thread_running =
						BCM_LED_THREAD_DISABLED;
			TURN_OFF_LED(Adapter, 1 << GPIO_num, uiLedIndex);
			return; /* STATUS_FAILURE; */
		}

		if (GPIO_num != DISABLE_GPIO_NUM)
			TURN_OFF_LED(Adapter, 1 << GPIO_num, uiLedIndex);

		if (Adapter->LEDInfo.bLedInitDone == false) {
			LedGpioInit(Adapter);
			Adapter->LEDInfo.bLedInitDone = TRUE;
		}

		handle_adapter_driver_state(Adapter,
					    currdriverstate,
					    GPIO_num,
					    dummyGPIONum,
					    uiLedIndex,
					    dummyIndex,
					    timeout,
					    uiResetValue,
					    uiIndex
					    );
	}
	Adapter->LEDInfo.led_thread_running = BCM_LED_THREAD_DISABLED;
}

int InitLedSettings(struct bcm_mini_adapter *Adapter)
{
	int Status = STATUS_SUCCESS;
	bool bEnableThread = TRUE;
	UCHAR uiIndex = 0;

	/*
	 * Initially set BitPolarity to normal polarity. The bit 8 of LED type
	 * is used to change the polarity of the LED.
	 */

	for (uiIndex = 0; uiIndex < NUM_OF_LEDS; uiIndex++)
		Adapter->LEDInfo.LEDState[uiIndex].BitPolarity = 1;

	/*
	 * Read the LED settings of CONFIG file and map it
	 * to GPIO numbers in EEPROM
	 */
	Status = ReadConfigFileStructure(Adapter, &bEnableThread);
	if (STATUS_SUCCESS != Status) {
		BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
				DBG_LVL_ALL,
				"LED Thread: FAILED in ReadConfigFileStructure\n");
		return Status;
	}

	if (Adapter->LEDInfo.led_thread_running) {
		if (bEnableThread) {
			;
		} else {
			Adapter->DriverState = DRIVER_HALT;
			wake_up(&Adapter->LEDInfo.notify_led_event);
			Adapter->LEDInfo.led_thread_running =
						BCM_LED_THREAD_DISABLED;
		}

	} else if (bEnableThread) {
		/* Create secondary thread to handle the LEDs */
		init_waitqueue_head(&Adapter->LEDInfo.notify_led_event);
		init_waitqueue_head(&Adapter->LEDInfo.idleModeSyncEvent);
		Adapter->LEDInfo.led_thread_running =
					BCM_LED_THREAD_RUNNING_ACTIVELY;
		Adapter->LEDInfo.bIdle_led_off = false;
		Adapter->LEDInfo.led_cntrl_threadid =
			kthread_run((int (*)(void *)) LEDControlThread,
				    Adapter, "led_control_thread");
		if (IS_ERR(Adapter->LEDInfo.led_cntrl_threadid)) {
			BCM_DEBUG_PRINT(Adapter, DBG_TYPE_OTHERS, LED_DUMP_INFO,
					DBG_LVL_ALL,
					"Not able to spawn Kernel Thread\n");
			Adapter->LEDInfo.led_thread_running =
				BCM_LED_THREAD_DISABLED;
			return PTR_ERR(Adapter->LEDInfo.led_cntrl_threadid);
		}
	}
	return Status;
}
