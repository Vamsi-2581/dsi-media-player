#include <calico.h>
#include <calico/nds/arm7/mcu.h>
#include <calico/nds/arm7/i2c.h>
#include <calico/nds/arm7/gpio.h>
#include <calico/nds/arm7/pmic.h>
#include <nds.h>
#include <maxmod7.h>

static volatile bool volBtnPressed;
static u8 lastVol = 0xFF;
static bool lastHinge = false;

static void onVolButton(unsigned irq_mask) {
	(void)irq_mask;
	volBtnPressed = true;
}

static bool isHingeClosed(void) {
	return (REG_RCNT_EXT & RCNT_EXT_HINGE) != 0;
}

int main() {
	envReadNvramSettings();
	keypadStartExtServer();
	lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
	irqEnable(IRQ_VBLANK);
	rtcInit();
	rtcSyncTime();
	pmInit();
	blkInit();
	touchInit();
	touchStartServer(80, MAIN_THREAD_PRIO);
	soundStartServer(MAIN_THREAD_PRIO - 0x10);
	micStartServer(MAIN_THREAD_PRIO - 0x18);
	wlmgrStartServer(MAIN_THREAD_PRIO - 8);
	mmInstall(MAIN_THREAD_PRIO + 1);

	bool isDsi = systemIsTwlMode();

	if (isDsi) {
		/* Disable automatic sleep so audio keeps playing with lid closed */
		pmSetSleepAllowed(false);

		/* Enable MCU volume button IRQ — without this, the MCU doesn't
		   process button presses or update the volume register */
		mcuIrqSet(MCU_IRQ_VOLBTN, onVolButton);

		/* Send initial volume to ARM9 */
		lastVol = i2cReadRegister8(I2cDev_MCU, McuReg_VolumeLevel);
		lastHinge = isHingeClosed();
		pxiSend(PxiChannel_User0, (u32)lastVol);
	}

	while (pmMainLoop()) {
		threadWaitForVBlank();

		if (!isDsi) continue;

		/* Read volume when button was pressed (flag set by MCU IRQ) */
		if (volBtnPressed) {
			volBtnPressed = false;
			u8 vol = i2cReadRegister8(I2cDev_MCU, McuReg_VolumeLevel);
			if (vol != lastVol) {
				lastVol = vol;
				pxiSend(PxiChannel_User0, (u32)vol);
			}
		}

		/* Detect hinge state change and toggle backlights via PMIC */
		bool hinge = isHingeClosed();
		if (hinge != lastHinge) {
			lastHinge = hinge;
			u8 ctrl = pmicReadRegister(PmicReg_Control);
			if (hinge) {
				ctrl &= ~(PMIC_CTRL_LCD_BL_TOP | PMIC_CTRL_LCD_BL_BOTTOM);
			} else {
				ctrl |= PMIC_CTRL_LCD_BL_TOP | PMIC_CTRL_LCD_BL_BOTTOM;
			}
			pmicWriteRegister(PmicReg_Control, ctrl);
		}
	}

	return 0;
}
