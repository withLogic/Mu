#include <string.h>

#include <boolean.h>

#include "emulator.h"
#include "hardwareRegisterNames.h"
#include "hardwareRegisters.h"
#include "memoryAccess.h"
#include "68328Functions.h"
#include "portability.h"
#include "m68k/m68k.h"


chip_t   chips[CHIP_END];
int32_t  pllWakeWait;
uint32_t clk32Counter;
double   timer1CycleCounter;
double   timer2CycleCounter;


static inline uint8_t registerArrayRead8(uint32_t address){return BUFFER_READ_8(palmReg, address, 0, 0xFFF);}
static inline uint16_t registerArrayRead16(uint32_t address){return BUFFER_READ_16(palmReg, address, 0, 0xFFF);}
static inline uint32_t registerArrayRead32(uint32_t address){return BUFFER_READ_32(palmReg, address, 0, 0xFFF);}
static inline void registerArrayWrite8(uint32_t address, uint8_t value){BUFFER_WRITE_8(palmReg, address, 0, 0xFFF, value);}
static inline void registerArrayWrite16(uint32_t address, uint16_t value){BUFFER_WRITE_16(palmReg, address, 0, 0xFFF, value);}
static inline void registerArrayWrite32(uint32_t address, uint32_t value){BUFFER_WRITE_32(palmReg, address, 0, 0xFFF, value);}

static inline void setIprIsrBit(uint32_t interruptBit){
   //allows for setting an interrupt with masking by IMR and logging in IPR
   registerArrayWrite32(IPR, registerArrayRead32(IPR) | interruptBit);
   registerArrayWrite32(ISR, registerArrayRead32(ISR) | (interruptBit & ~registerArrayRead32(IMR)));
}

static inline void clearIprIsrBit(uint32_t interruptBit){
   registerArrayWrite32(IPR, registerArrayRead32(IPR) & ~interruptBit);
   registerArrayWrite32(ISR, registerArrayRead32(ISR) & ~interruptBit);
}

static inline void pllWakeCpuIfOff(){
   uint16_t pllcr = registerArrayRead16(PLLCR);
   if(pllcr & 0x0008 && pllWakeWait == -1){
      //CPU is off and not already in the process of waking up
      switch(pllcr & 0x0003){

         case 0x0000:
            pllWakeWait = 32;
            break;

         case 0x0001:
            pllWakeWait = 48;
            break;

         case 0x0002:
            pllWakeWait = 64;
            break;

         case 0x0003:
            pllWakeWait = 96;
            break;
      }
   }
}

static inline bool pllOn(){
   return !CAST_TO_BOOL(registerArrayRead16(PLLCR) & 0x0008);
}

static inline void setCsa(uint16_t value){
   chips[CHIP_A_ROM].enable = CAST_TO_BOOL(value & 0x0001);
   chips[CHIP_A_ROM].readOnly = CAST_TO_BOOL(value & 0x8000);
   chips[CHIP_A_ROM].size = 0x20000/*128kb*/ << ((value >> 1) & 0x0007);

   //CSA is now just a normal chipselect
   if(chips[CHIP_A_ROM].enable && chips[CHIP_A_ROM].inBootMode)
      chips[CHIP_A_ROM].inBootMode = false;

   registerArrayWrite16(CSA, value & 0x81FF);
}

static inline void setCsb(uint16_t value){
   uint16_t csControl1 = registerArrayRead16(CSCTRL1);

   chips[CHIP_B_SED].enable = CAST_TO_BOOL(value & 0x0001);
   chips[CHIP_B_SED].readOnly = CAST_TO_BOOL(value & 0x8000);
   chips[CHIP_B_SED].size = 0x20000/*128kb*/ << ((value >> 1) & 0x0007);

   //attributes
   chips[CHIP_B_SED].supervisorOnlyProtectedMemory = CAST_TO_BOOL(value & 0x4000);
   chips[CHIP_B_SED].readOnlyForProtectedMemory = CAST_TO_BOOL(value & 0x2000);
   if(csControl1 & 0x4000 && csControl1 & 0x0001)
      chips[CHIP_B_SED].unprotectedSize = 0x8000/*32kb*/ << (((value >> 11) & 0x0003) | 0x0004);
   else
      chips[CHIP_B_SED].unprotectedSize = 0x8000/*32kb*/ << ((value >> 11) & 0x0003);

   registerArrayWrite16(CSB, value & 0xF9FF);
}

static inline void setCsc(uint16_t value){
   uint16_t csControl1 = registerArrayRead16(CSCTRL1);

   chips[CHIP_C_USB].enable = CAST_TO_BOOL(value & 0x0001);
   chips[CHIP_C_USB].readOnly = CAST_TO_BOOL(value & 0x8000);
   chips[CHIP_C_USB].size = 0x8000/*32kb*/ << ((value >> 1) & 0x0007);

   //attributes
   chips[CHIP_C_USB].supervisorOnlyProtectedMemory = CAST_TO_BOOL(value & 0x4000);
   chips[CHIP_C_USB].readOnlyForProtectedMemory = CAST_TO_BOOL(value & 0x2000);
   if(csControl1 & 0x4000 && csControl1 & 0x0004)
      chips[CHIP_C_USB].unprotectedSize = 0x8000/*32kb*/ << (((value >> 11) & 0x0003) | 0x0004);
   else
      chips[CHIP_C_USB].unprotectedSize = 0x8000/*32kb*/ << ((value >> 11) & 0x0003);

   registerArrayWrite16(CSC, value & 0xF9FF);
}

static inline void setCsd(uint16_t value){
   uint16_t csControl1 = registerArrayRead16(CSCTRL1);

   chips[CHIP_D_RAM].enable = CAST_TO_BOOL(value & 0x0001);
   chips[CHIP_D_RAM].readOnly = CAST_TO_BOOL(value & 0x8000);
   if(csControl1 & 0x0040 && value & 0x0200)
      chips[CHIP_D_RAM].size = 0x800000/*8mb*/ << ((value >> 1) & 0x0001);
   else
      chips[CHIP_D_RAM].size = 0x8000/*32kb*/ << ((value >> 1) & 0x0007);

   //attributes
   chips[CHIP_D_RAM].supervisorOnlyProtectedMemory = CAST_TO_BOOL(value & 0x4000);
   chips[CHIP_D_RAM].readOnlyForProtectedMemory = CAST_TO_BOOL(value & 0x2000);
   if(csControl1 & 0x4000 && csControl1 & 0x0010)
      chips[CHIP_D_RAM].unprotectedSize = 0x8000/*32kb*/ << (((value >> 11) & 0x0003) | 0x0004);
   else
      chips[CHIP_D_RAM].unprotectedSize = 0x8000/*32kb*/ << ((value >> 11) & 0x0003);

   registerArrayWrite16(CSD, value);
}

static inline void setCsgba(uint16_t value){
   uint16_t csugba = registerArrayRead16(CSUGBA);

   //add extra address bits if enabled
   if(csugba & 0x8000)
      chips[CHIP_A_ROM].start = ((csugba >> 12) & 0x0007) << 29 | (value >> 1) << 14;
   else
      chips[CHIP_A_ROM].start = (value >> 1) << 14;

   registerArrayWrite16(CSGBA, value & 0xFFFE);
}

static inline void setCsgbb(uint16_t value){
   uint16_t csugba = registerArrayRead16(CSUGBA);

   //add extra address bits if enabled
   if(csugba & 0x8000)
      chips[CHIP_B_SED].start = ((csugba >> 8) & 0x0007) << 29 | (value >> 1) << 14;
   else
      chips[CHIP_B_SED].start = (value >> 1) << 14;

   registerArrayWrite16(CSGBB, value & 0xFFFE);
}

static inline void setCsgbc(uint16_t value){
   uint16_t csugba = registerArrayRead16(CSUGBA);

   //add extra address bits if enabled
   if(csugba & 0x8000)
      chips[CHIP_C_USB].start = ((csugba >> 4) & 0x0007) << 29 | (value >> 1) << 14;
   else
      chips[CHIP_C_USB].start = (value >> 1) << 14;

   registerArrayWrite16(CSGBC, value & 0xFFFE);
}

static inline void setCsgbd(uint16_t value){
   uint16_t csugba = registerArrayRead16(CSUGBA);

   //add extra address bits if enabled
   if(csugba & 0x8000)
      chips[CHIP_D_RAM].start = (csugba & 0x0007) << 29 | (value >> 1) << 14;
   else
      chips[CHIP_D_RAM].start = (value >> 1) << 14;

   registerArrayWrite16(CSGBD, value & 0xFFFE);
}

static inline void setCsctrl1(uint16_t value){
   uint16_t oldCsctrl1 = registerArrayRead16(CSCTRL1);

   registerArrayWrite16(CSCTRL1, value & 0x7F55);
   if((oldCsctrl1 & 0x4055) != (value & 0x4055)){
      //something important changed, update all chipselects
      //CSA is not dependant on CSCTRL1
      setCsb(registerArrayRead16(CSB));
      setCsc(registerArrayRead16(CSC));
      setCsd(registerArrayRead16(CSD));
   }
}
//csctrl 2 and 3 only deal with timing and bus transfer size


void printUnknownHwAccess(unsigned int address, unsigned int value, unsigned int size, bool isWrite){
   if(isWrite){
      debugLog("CPU wrote %d bits of 0x%08X to register 0x%04X, PC 0x%08X.\n", size, value, address, m68k_get_reg(NULL, M68K_REG_PC));
   }
   else{
      debugLog("CPU read %d bits from register 0x%04X, PC 0x%08X.\n", size, address, m68k_get_reg(NULL, M68K_REG_PC));
   }
}


void checkInterrupts(){
   uint32_t activeInterrupts = registerArrayRead32(ISR);
   uint16_t interruptLevelControlRegister = registerArrayRead16(ILCR);
   uint16_t portDEdgeSelect = registerArrayRead16(PDIRQEG);
   uint32_t intLevel = 0;
   bool reenablePllIfOff = false;

   if(activeInterrupts & INT_EMIQ){
      //EMIQ - Emulator Irq, has nothing to do with emulation, used for debugging on a dev board
      intLevel = 7;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_SPI1){
      uint32_t spi1IrqLevel = interruptLevelControlRegister >> 12;
      if(intLevel < spi1IrqLevel)
         intLevel = spi1IrqLevel;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_IRQ5){
      if(intLevel < 5)
         intLevel = 5;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_IRQ3){
      if(intLevel < 3)
         intLevel = 3;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_IRQ2){
      if(intLevel < 2)
         intLevel = 2;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_IRQ1){
      if(intLevel < 1)
         intLevel = 1;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_PWM2){
      uint32_t pwm2IrqLevel = (interruptLevelControlRegister >> 4) & 0x0007;
      if(intLevel < pwm2IrqLevel)
         intLevel = pwm2IrqLevel;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_UART2){
      uint32_t uart2IrqLevel = (interruptLevelControlRegister >> 8) & 0x0007;
      if(intLevel < uart2IrqLevel)
         intLevel = uart2IrqLevel;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_TMR2){
      //TMR2 - Timer 2
      uint32_t timer2IrqLevel = interruptLevelControlRegister & 0x0007;
      if(intLevel < timer2IrqLevel)
         intLevel = timer2IrqLevel;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & (INT_TMR1 | INT_PWM1 | INT_IRQ6)){
      //All Fixed Level 6 Interrupts
      if(intLevel < 6)
         intLevel = 6;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & (INT_SPI2 | INT_UART1 | INT_WDT | INT_RTC | INT_KB | INT_RTI)){
      //All Fixed Level 4 Interrupts
      if(intLevel < 4)
         intLevel = 4;
      reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_INT0){
      //INTx, only reenable the PLL if interrupt is set to level sensitive
      if(intLevel < 4)
         intLevel = 4;
      if(!(portDEdgeSelect & 0x01))
         reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_INT1){
      //INTx, only reenable the PLL if interrupt is set to level sensitive
      if(intLevel < 4)
         intLevel = 4;
      if(!(portDEdgeSelect & 0x02))
         reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_INT2){
      //INTx, only reenable the PLL if interrupt is set to level sensitive
      if(intLevel < 4)
         intLevel = 4;
      if(!(portDEdgeSelect & 0x04))
         reenablePllIfOff = true;
   }

   if(activeInterrupts & INT_INT3){
      //INTx, only reenable the PLL if interrupt is set to level sensitive
      if(intLevel < 4)
         intLevel = 4;
      if(!(portDEdgeSelect & 0x08))
         reenablePllIfOff = true;
   }

   if(reenablePllIfOff)
      pllWakeCpuIfOff();

   m68k_set_irq(intLevel);//should be called even if intLevel is 0, that is how the interrupt state gets cleared
}

static inline uint8_t getPortDValue(){
   uint8_t requestedRow = registerArrayRead8(PKDIR) & registerArrayRead8(PKDATA);//keys are requested on port k and read on port d
   uint8_t portDValue = 0x00;//ports always read the chip pins even if they are set to output
   uint8_t portDData = registerArrayRead8(PDDATA);
   uint8_t portDDir = registerArrayRead8(PDDIR);
   uint8_t portDPolarity = registerArrayRead8(PDPOL);
   
   portDValue |= 0x80/*battery not dead bit*/;
   
   if(!palmSdCard.inserted){
      portDValue |= 0x20;
   }
   
   if((requestedRow & 0x20) == 0){
      //kbd row 0, pins are 0 when button pressed and 1 when released, Palm OS then uses PDPOL to swap back to pressed == 1
      portDValue |= !palmInput.buttonCalender | !palmInput.buttonAddress << 1 | !palmInput.buttonTodo << 2 | !palmInput.buttonNotes << 3;
   }
   
   if((requestedRow & 0x40) == 0){
      //kbd row 1, pins are 0 when button pressed and 1 when released, Palm OS then uses PDPOL to swap back to pressed == 1
      portDValue |= !palmInput.buttonUp | !palmInput.buttonDown << 1;
   }
   
   if((requestedRow & 0x80) == 0){
      //kbd row 2, pins are 0 when button pressed and 1 when released, Palm OS then uses PDPOL to swap back to pressed == 1
      portDValue |= !palmInput.buttonPower | !palmInput.buttonContrast << 1 | !palmInput.buttonAddress << 3;
   }
   
   portDValue ^= portDPolarity;//only input polarity is affected by PDPOL
   portDValue &= ~portDDir;//only use above pin values for inputs
   portDValue |= portDData & portDDir;//if a pin is an output and has its data bit set return that too
   
   return portDValue;
}

static inline uint8_t getPortKValue(){
   uint8_t portKValue = 0x00;//ports always read the chip pins even if they are set to output
   uint8_t portKData = registerArrayRead8(PKDATA);
   uint8_t portKDir = registerArrayRead8(PKDIR);
   uint8_t portKSel = registerArrayRead8(PKSEL);

   portKValue |= !palmMisc.inDock << 2 & ~portKDir & portKSel;
   portKValue |= portKData & portKDir & portKSel;

   return portKValue;
}

static inline void checkPortDInts(){
   uint8_t portDValue = getPortDValue();
   uint8_t portDDir = registerArrayRead8(PDDIR);
   uint8_t portDIntEnable = registerArrayRead8(PDIRQEN);
   uint8_t portDKeyboardEnable = registerArrayRead8(PDKBEN);
   uint8_t portDIrqPins = ~registerArrayRead8(PDSEL);
   uint16_t interruptControlRegister = registerArrayRead16(ICR);

   if(portDIntEnable & portDValue & ~portDDir & 0x01){
      //int 0, polarity set with PDPOL
      setIprIsrBit(INT_INT0);
   }
   
   if(portDIntEnable & portDValue & ~portDDir & 0x02){
      //int 1, polarity set with PDPOL
      setIprIsrBit(INT_INT1);
   }
   
   if(portDIntEnable & portDValue & ~portDDir & 0x04){
      //int 2, polarity set with PDPOL
      setIprIsrBit(INT_INT2);
   }
   
   if(portDIntEnable & portDValue & ~portDDir & 0x08){
      //int 3, polarity set with PDPOL
      setIprIsrBit(INT_INT3);
   }
   
   if(portDIrqPins & ~portDDir & 0x10 && ((portDValue & 0x10) != 0) == ((interruptControlRegister & 0x8000) != 0)){
      //irq 1, polarity set in ICR
      setIprIsrBit(INT_IRQ1);
   }
   
   if(portDIrqPins & ~portDDir & 0x20 && ((portDValue & 0x20) != 0) == ((interruptControlRegister & 0x4000) != 0)){
      //irq 2, polarity set in ICR
      setIprIsrBit(INT_IRQ2);
   }
   
   if(portDIrqPins & ~portDDir & 0x40 && ((portDValue & 0x40) != 0) == ((interruptControlRegister & 0x2000) != 0)){
      //irq 3, polarity set in ICR
      setIprIsrBit(INT_IRQ3);
   }
   
   if(portDIrqPins & ~portDDir & 0x80 && ((portDValue & 0x80) != 0) == ((interruptControlRegister & 0x1000) != 0)){
      //irq 6, polarity set in ICR
      setIprIsrBit(INT_IRQ6);
   }
   
   if(portDKeyboardEnable & ~portDValue & ~portDDir){
      //active low/off level triggered interrupt
      setIprIsrBit(INT_KB);
   }
   
   checkInterrupts();
}

static inline void updateAlarmLedStatus(){
   if(registerArrayRead8(PBDATA) & registerArrayRead8(PBSEL) & registerArrayRead8(PBDIR) & 0x40)
      palmMisc.alarmLed = true;
   else
      palmMisc.alarmLed = false;
}

static inline void updateLcdStatus(){
   if(registerArrayRead8(PKDATA) & registerArrayRead8(PKSEL) & registerArrayRead8(PKDIR) & 0x02)
      palmMisc.lcdOn = true;
   else
      palmMisc.lcdOn = false;
}

static inline void updateBacklightStatus(){
   if(registerArrayRead8(PGDATA) & registerArrayRead8(PGSEL) & registerArrayRead8(PGDIR) & 0x02)
      palmMisc.backlightOn = true;
   else
      palmMisc.backlightOn = false;
}

static inline void updateVibratorStatus(){
   if(registerArrayRead8(PKDATA) & registerArrayRead8(PKSEL) & registerArrayRead8(PKDIR) & 0x10)
      palmMisc.vibratorOn = true;
   else
      palmMisc.vibratorOn = false;
}

static inline void setPllfsr16(uint16_t value){
   uint16_t oldPllfsr = registerArrayRead16(PLLFSR);
   if(!(oldPllfsr & 0x4000)){
      //frequency protect bit not set
      registerArrayWrite16(PLLFSR, (value & 0x4FFF) | (oldPllfsr & 0x8000));//preserve CLK32 bit
      double prescaler1 = (registerArrayRead16(PLLCR) & 0x0080) ? 2.0 : 1.0;
      double p = value & 0x00FF;
      double q = (value & 0x0F00) >> 8;
      palmCrystalCycles = 2.0 * (14.0 * (p + 1.0) + q + 1.0) / prescaler1;
      debugLog("New CPU frequency of:%f cycles per second.\n", CPU_FREQUENCY);
      debugLog("New CLK32 cycle count of:%f.\n", palmCrystalCycles);
   }
}

static inline void setPllcr(uint16_t value){
   //values that matter are disable PLL, prescaler 1 and possibly wakeselect
   registerArrayWrite16(PLLCR, value & 0x3FBB);
   uint16_t pllfsr = registerArrayRead16(PLLFSR);
   double prescaler1 = (value & 0x0080) ? 2.0 : 1.0;
   double p = pllfsr & 0x00FF;
   double q = (pllfsr & 0x0F00) >> 8;
   palmCrystalCycles = 2.0 * (14.0 * (p + 1.0) + q + 1.0) / prescaler1;
   debugLog("New CPU frequency of:%f cycles per second.\n", CPU_FREQUENCY);
   debugLog("New CLK32 cycle count of:%f.\n", palmCrystalCycles);
   
   if(value & 0x0008){
      //The PLL shuts down 30 clock cycles of SYSCLK after the DISPLL bit is set in the PLLCR
      m68k_modify_timeslice(-m68k_cycles_remaining() + 30);
      debugLog("Disable PLL set, CPU off in 30 cycles!\n");
   }
}

static inline void setScr(uint8_t value){
   uint8_t oldScr = registerArrayRead8(SCR);
   uint8_t newScr = value;

   //preserve privilege violation, write protect violation and bus error timeout
   newScr |= oldScr & 0xE0;

   //clear violations on writing 1 to them
   newScr &= ~(oldScr & value & 0xE0);

   chips[CHIP_REGISTERS].supervisorOnlyProtectedMemory = CAST_TO_BOOL(value & 0x08);

   registerArrayWrite8(SCR, newScr);//must be written before calling setRegisterFFFFAccessMode
   if((newScr & 0x04) != (oldScr & 0x04)){
      if(newScr & 0x04)
         setRegisterXXFFAccessMode();
      else
         setRegisterFFFFAccessMode();
   }
}

static inline double dmaclksPerClk32(){
   uint16_t pllcr = registerArrayRead16(PLLCR);
   double   dmaclks = palmCrystalCycles;
   
   if(pllcr & 0x0080){
      //prescaler 1 enabled, divide by 2
      dmaclks /= 2.0;
   }
   
   if(pllcr & 0x0020){
      //prescaler 2 enabled, divides value from prescaler 1 by 2
      dmaclks /= 2.0;
   }
   
   return dmaclks;
}

static inline double sysclksPerClk32(){
   uint16_t pllcr = registerArrayRead16(PLLCR);
   double   sysclks = dmaclksPerClk32();
   uint16_t sysclkSelect = (pllcr >> 8) & 0x0003;
   
   switch(sysclkSelect){
         
      case 0x0000:
         sysclks /= 2.0;
         break;
         
      case 0x0001:
         sysclks /= 4.0;
         break;
         
      case 0x0002:
         sysclks /= 8.0;
         break;
         
      case 0x0003:
         sysclks /= 16.0;
         break;
         
      default:
         //no divide for 0x0004, 0x0005, 0x0006 or 0x0007
         break;
   }
   
   return sysclks;
}

static inline void rtiInterruptClk32(){
   //this function is part of clk32();
   uint16_t triggeredRtiInterrupts = 0;
   
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 512) == 0){
      //RIS7 - 512HZ
      triggeredRtiInterrupts |= 0x8000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 256) == 0){
      //RIS6 - 256HZ
      triggeredRtiInterrupts |= 0x4000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 128) == 0){
      //RIS5 - 128HZ
      triggeredRtiInterrupts |= 0x2000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 64) == 0){
      //RIS4 - 64HZ
      triggeredRtiInterrupts |= 0x1000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 32) == 0){
      //RIS3 - 32HZ
      triggeredRtiInterrupts |= 0x0800;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 16) == 0){
      //RIS2 - 16HZ
      triggeredRtiInterrupts |= 0x0400;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 8) == 0){
      //RIS1 - 8HZ
      triggeredRtiInterrupts |= 0x0200;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 4) == 0){
      //RIS0 - 4HZ
      triggeredRtiInterrupts |= 0x0100;
   }
   
   triggeredRtiInterrupts &= registerArrayRead16(RTCIENR);
   if(triggeredRtiInterrupts){
      registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) | triggeredRtiInterrupts);
      setIprIsrBit(INT_RTI);
   }
}

static inline void timer12Clk32(){
   //this function is part of clk32();
   uint16_t timer1Control = registerArrayRead16(TCTL1);
   uint16_t timer1Prescaler = registerArrayRead16(TPRER1) & 0x00FF;
   uint16_t timer1Compare = registerArrayRead16(TCMP1);
   uint16_t timer1OldCount = registerArrayRead16(TCN1);
   uint16_t timer1Count = timer1OldCount;
   
   uint16_t timer2Control = registerArrayRead16(TCTL2);
   uint16_t timer2Prescaler = registerArrayRead16(TPRER2) & 0x00FF;
   uint16_t timer2Compare = registerArrayRead16(TCMP2);
   uint16_t timer2OldCount = registerArrayRead16(TCN2);
   uint16_t timer2Count = timer2OldCount;
   
   //timer 1
   if(timer1Control & 0x0001){
      //enabled
      switch((timer1Control & 0x000E) >> 1){
            
         case 0x0000://stop counter
         case 0x0003://TIN pin / timer prescaler, nothing is attached to TIN
            //do nothing
            break;
            
         case 0x0001://SYSCLK / timer prescaler
            if(pllOn())
               timer1CycleCounter += sysclksPerClk32() / (double)timer1Prescaler;
            break;
            
         case 0x0002://SYSCLK / 16 / timer prescaler
            if(pllOn())
               timer1CycleCounter += sysclksPerClk32() / 16.0 / (double)timer1Prescaler;
            break;
            
         default://CLK32 / timer prescaler
            timer1CycleCounter += 1.0 / (double)timer1Prescaler;
            break;
      }
      
      if(timer1CycleCounter >= 1.0){
         timer1Count += (uint16_t)timer1CycleCounter;
         timer1CycleCounter -= (uint16_t)timer1CycleCounter;
      }
      
      if(timer1OldCount < timer1Compare && timer1Count >= timer1Compare){
         //the timer is not cycle accurate and may not hit the value in the compare register perfectly so check if it would have during in the emulated time
         if(timer1Control & 0x0010){
            //interrupt enabled
            setIprIsrBit(INT_TMR1);
         }
         
         if(!(timer1Control & 0x0100)){
            //not free running, reset to 0, to prevent loss of ticks after compare event just subtract timerXCompare
            timer1Count -= timer1Compare;
         }
      }
      
      registerArrayWrite16(TCN1, timer1Count);
   }
   
   //timer 2
   if(timer2Control & 0x0001){
      //enabled
      switch((timer2Control & 0x000E) >> 1){
            
         case 0x0000://stop counter
         case 0x0003://TIN pin / timer prescaler, nothing is attached to TIN
            //do nothing
            break;
            
         case 0x0001://SYSCLK / timer prescaler
            if(pllOn())
               timer2CycleCounter += sysclksPerClk32() / (double)timer2Prescaler;
            break;
            
         case 0x0002://SYSCLK / 16 / timer prescaler
            if(pllOn())
               timer2CycleCounter += sysclksPerClk32() / 16.0 / (double)timer2Prescaler;
            break;
            
         default://CLK32 / timer prescaler
            timer2CycleCounter += 1.0 / (double)timer2Prescaler;
            break;
      }
      
      if(timer2CycleCounter >= 1.0){
         timer2Count += (uint16_t)timer2CycleCounter;
         timer2CycleCounter -= (uint16_t)timer2CycleCounter;
      }
      
      if(timer2OldCount < timer2Compare && timer2Count >= timer2Compare){
         //the timer is not cycle accurate and may not hit the value in the compare register perfectly so check if it would have during in the emulated time
         if(timer2Control & 0x0010){
            //interrupt enabled
            setIprIsrBit(INT_TMR2);
         }
         
         if(!(timer2Control & 0x0100)){
            //not free running, reset to 0, to prevent loss of ticks after compare event just subtract timerXCompare
            timer2Count -= timer2Compare;
         }
      }
      
      registerArrayWrite16(TCN2, timer2Count);
   }
}

static inline void rtcAddSecondClk32(){
   //this function is part of clk32();
   
   //rtc
   if(registerArrayRead16(RTCCTL) & 0x0080){
      //rtc enable bit set
      uint16_t rtcInterruptEvents;
      uint32_t newRtcTime;
      uint32_t oldRtcTime = registerArrayRead32(RTCTIME);
      uint32_t hours = oldRtcTime >> 24;
      uint32_t minutes = (oldRtcTime >> 16) & 0x0000003F;
      uint32_t seconds = oldRtcTime & 0x0000003F;
      
      seconds++;
      rtcInterruptEvents = 0x0010;//1 second interrupt
      if(seconds >= 60){
         minutes++;
         seconds = 0;
         rtcInterruptEvents |= 0x0002;//1 minute interrupt
         if(minutes >= 60){
            hours++;
            minutes = 0;
            rtcInterruptEvents |= 0x0020;//1 hour interrupt
            if(hours >= 24){
               hours = 0;
               uint16_t days = registerArrayRead16(DAYR);
               days++;
               registerArrayWrite16(DAYR, days & 0x01FF);
               rtcInterruptEvents |= 0x0008;//1 day interrupt
            }
         }
      }
      
      rtcInterruptEvents &= registerArrayRead16(RTCIENR);
      if(rtcInterruptEvents){
         registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) | rtcInterruptEvents);
         setIprIsrBit(INT_RTC);
      }
      
      newRtcTime = seconds & 0x0000003F;
      newRtcTime |= minutes << 16;
      newRtcTime |= hours << 24;
      registerArrayWrite32(RTCTIME, newRtcTime);
   }
   
   //watchdog
   uint16_t watchdogState = registerArrayRead16(WATCHDOG);
   if(watchdogState & 0x0001){
      //watchdog enabled
      watchdogState += 0x0100;//add second to watchdog timer
      watchdogState &= 0x0383;//cap overflow
      if((watchdogState & 0x0200) == 0x0200){
         //time expired
         if(watchdogState & 0x0002){
            //interrupt
            setIprIsrBit(INT_WDT);
         }
         else{
            //reset
            emulatorReset();
            return;
         }
      }
      registerArrayWrite16(WATCHDOG, watchdogState);
   }
}

void clk32(){
   registerArrayWrite16(PLLFSR, registerArrayRead16(PLLFSR) ^ 0x8000);

   //second position counter
   if(clk32Counter >= CRYSTAL_FREQUENCY - 1){
      clk32Counter = 0;
      rtcAddSecondClk32();
   }
   else{
      clk32Counter++;
   }

   //PLLCR wake select wait
   if(pllWakeWait != -1){
      if(pllWakeWait == 0){
         //reenable PLL and CPU
         registerArrayWrite16(PLLCR, registerArrayRead16(PLLCR) & 0xFFF7);
         debugLog("PLL reenabled, CPU is on!\n");
      }
      pllWakeWait--;
   }
   
   rtiInterruptClk32();
   timer12Clk32();
   
   checkInterrupts();
}

bool cpuIsOn(){
   return pllOn() && !lowPowerStopActive;
}

bool registersAreXXFFMapped(){
   return CAST_TO_BOOL(registerArrayRead8(SCR) & 0x04);
}

bool sed1376ClockConnected(){
   //this is the clock output pin for the SED1376, if its disabled so is the LCD controller
   return !CAST_TO_BOOL(registerArrayRead8(PFSEL) & 0x04);
}

void refreshButtonState(){
   checkPortDInts();
}

void setBusErrorTimeOut(){
   uint8_t scr = registerArrayRead8(SCR);
   debugLog("Bus error timeout, PC:0x%08X\n", m68k_get_reg(NULL, M68K_REG_PC));
   if(scr & 0x10){
      //trigger bus error interrupt
   }
   registerArrayWrite8(SCR, scr | 0x80);
}

void setWriteProtectViolation(){
   uint8_t scr = registerArrayRead8(SCR);
   debugLog("Write protect violation, PC:0x%08X\n", m68k_get_reg(NULL, M68K_REG_PC));
   if(scr & 0x10){
      //trigger bus error interrupt
   }
   registerArrayWrite8(SCR, scr | 0x40);
}

void setPrivilegeViolation(){
   uint8_t scr = registerArrayRead8(SCR);
   debugLog("Privilege violation, PC:0x%08X\n", m68k_get_reg(NULL, M68K_REG_PC));
   if(scr & 0x10){
      //trigger bus error interrupt
   }
   registerArrayWrite8(SCR, scr | 0x20);
}

int interruptAcknowledge(int intLevel){
   int vectorOffset = registerArrayRead8(IVR);
   int vector;
   
   //If an interrupt occurs before the IVR has been programmed, the interrupt vector number 0x0F is returned to the CPU as an uninitialized interrupt.
   if(!vectorOffset)
      vector = 15/*EXCEPTION_UNINITIALIZED_INTERRUPT*/;
   else
      vector = vectorOffset | intLevel;

   lowPowerStopActive = false;
   
   //the interrupt should only be cleared after its been handled
   
   return vector;
}


unsigned int getHwRegister8(unsigned int address){
   if((address & 0x0000F000) != 0x0000F000){
      //not emu or hardware register, invalid access
      return 0x00;
   }
   
   address &= 0x00000FFF;
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_ALL)
   printUnknownHwAccess(address, 0, 8, false);
#endif
   switch(address){
         
      case PDDATA:
         return getPortDValue();

      case PKDATA:
         return getPortKValue();

      case LCKCON:
         
      //I/O direction
      case PDDIR:
      case PKDIR:

      //select between GPIO or special function
      case PBSEL:
      case PCSEL:
      case PDSEL:
      case PESEL:
      case PFSEL:
      case PGSEL:
      case PJSEL:
      case PKSEL:
      case PMSEL:
         
      //pull up/down enable
      case PAPUEN:
      case PBPUEN:
      case PCPDEN:
      case PDPUEN:
      case PEPUEN:
      case PFPUEN:
      case PGPUEN:
      case PJPUEN:
      case PKPUEN:
      case PMPUEN:
         //simple read, no actions needed
         //PGPUEN, PGSEL PMSEL and PMPUEN lack the top 2 bits but that is handled on write
         //PDSEL lacks the bottom 4 bits but that is handled on write
         return registerArrayRead8(address);
         
      default:
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_UNKNOWN) && !defined(EMU_LOG_REGISTER_ACCESS_ALL)
         printUnknownHwAccess(address, 0, 8, false);
#endif
         return 0x00;
   }
   
   return 0x00;//silence warnings
}

unsigned int getHwRegister16(unsigned int address){
   if((address & 0x0000F000) != 0x0000F000){
      //not emu or hardware register, invalid access
      return 0x0000;
   }
   
   address &= 0x00000FFF;
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_ALL)
   printUnknownHwAccess(address, 0, 16, false);
#endif
   switch(address){
         
      //32 bit registers accessed as 16 bit
      case IMR:
      case IMR + 2:
      case IPR:
      case IPR + 2:
         
      case CSA:
      case CSB:
      case CSC:
      case CSD:
      case CSGBA:
      case CSGBB:
      case CSGBC:
      case CSGBD:
      case CSUGBA:
      case PLLCR:
      case PLLFSR:
      case DRAMC:
      case SDCTRL:
      case RTCISR:
      case RTCCTL:
      case RTCIENR:
         //simple read, no actions needed
         return registerArrayRead16(address);
         
      default:
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_UNKNOWN) && !defined(EMU_LOG_REGISTER_ACCESS_ALL)
         printUnknownHwAccess(address, 0, 16, false);
#endif
         return 0x0000;
   }
   
   return 0x0000;//silence warnings
}

unsigned int getHwRegister32(unsigned int address){
   if((address & 0x0000F000) == 0x0000E000){
      //32 bit emu register read, valid
      return 0x00000000;
   }
   else if((address & 0x0000F000) != 0x0000F000){
      //not emu or hardware register, invalid access
      return 0x00000000;
   }
   
   address &= 0x00000FFF;
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_ALL)
   printUnknownHwAccess(address, 0, 32, false);
#endif
   switch(address){

      //16 bit registers being read as 32 bit
      case PLLFSR:

      case ISR:
      case IPR:
      case IMR:
      case RTCTIME:
      case IDR:
         //simple read, no actions needed
         return registerArrayRead32(address);
         
      default:
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_UNKNOWN) && !defined(EMU_LOG_REGISTER_ACCESS_ALL)
         printUnknownHwAccess(address, 0, 32, false);
#endif
         return 0x00000000;
   }
   
   return 0x00000000;//silence warnings
}


void setHwRegister8(unsigned int address, unsigned int value){
   if((address & 0x0000F000) != 0x0000F000){
      //not emu or hardware register, invalid access
      return;
   }
   
   address &= 0x00000FFF;
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_ALL)
   printUnknownHwAccess(address, value, 8, true);
#endif
   switch(address){
         
      case SCR:
         setScr(value);
         break;
         
      case IVR:
         //write without the bottom 3 bits
         registerArrayWrite8(address, value & 0xF8);
         break;
         
      case PBSEL:
      case PBDIR:
      case PBDATA:
         registerArrayWrite8(address, value);
         updateAlarmLedStatus();
         break;
         
      case PDSEL:
         //write without the bottom 4 bits
         registerArrayWrite8(address, value & 0xF0);
         checkPortDInts();
         break;

      case PDPOL:
      case PDIRQEN:
      case PDIRQEG:
         //write without the top 4 bits
         registerArrayWrite8(address, value & 0x0F);
         checkPortDInts();
         break;
         
      case PFSEL:
         //this is the clock output pin for the SED1376, if its disabled so is the LCD controller
         setSed1376Attached(!CAST_TO_BOOL(value & 0x04));
         registerArrayWrite8(PFSEL, value);
         break;
         
      case PGSEL:
      case PGDIR:
      case PGDATA:
         //port g also does spi stuff, unemulated so far
         //write without the top 2 bits
         registerArrayWrite8(address, value & 0x3F);
         updateBacklightStatus();
         break;
         
      case PKSEL:
      case PKDIR:
      case PKDATA:
         registerArrayWrite8(address, value);
         checkPortDInts();
         updateLcdStatus();
         updateVibratorStatus();
         break;
         
      
      case PMSEL:
      case PMDIR:
      case PMDATA:
         //unemulated
         //infrared shutdown
         registerArrayWrite8(address, value & 0x3F);
         break;
         
      case PMPUEN:
      case PGPUEN:
         //write without the top 2 bits
         registerArrayWrite8(address, value & 0x3F);
         break;
         
      //select between GPIO or special function
      case PCSEL:
      case PESEL:
      case PJSEL:
      
      //direction select
      case PADIR:
      case PCDIR:
      case PDDIR:
      case PEDIR:
      case PFDIR:
      case PJDIR:
      
      //pull up/down enable
      case PAPUEN:
      case PBPUEN:
      case PCPDEN:
      case PDPUEN:
      case PEPUEN:
      case PFPUEN:
      case PJPUEN:
      case PKPUEN:
         
      //port data value, nothing attached to port
      case PCDATA:
      case PDDATA:
      case PEDATA:
      case PFDATA:
      case PJDATA:
         
      //misc port config
      case PDKBEN:
         
      //dragonball LCD controller, not attached to anything in Palm m515
      case LCKCON:
         
         //simple write, no actions needed
         registerArrayWrite8(address, value);
         break;
         
      default:
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_UNKNOWN) && !defined(EMU_LOG_REGISTER_ACCESS_ALL)
         printUnknownHwAccess(address, value, 8, true);
#endif
         break;
   }
}

void setHwRegister16(unsigned int address, unsigned int value){
   if((address & 0x0000F000) != 0x0000F000){
      //not emu or hardware register, invalid access
      return;
   }
   
   address &= 0x00000FFF;
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_ALL)
   printUnknownHwAccess(address, value, 16, true);
#endif
   switch(address){
         
      case RTCIENR:
         //missing bits 6 and 7
         registerArrayWrite16(address, value & 0xFF3F);
         break;
         
      case IMR:
         //this is a 32 bit register but Palm OS writes it as 16 bit chunks
         registerArrayWrite16(address, value & 0x00FF);
         break;
      case IMR + 2:
         //this is a 32 bit register but Palm OS writes it as 16 bit chunks
         registerArrayWrite16(address, value & 0x03FF);
         break;
         
      case ISR + 2:
         //this is a 32 bit register but Palm OS writes it as 16 bit chunks
         registerArrayWrite16(ISR + 2, registerArrayRead16(ISR + 2) & ~(value & 0x0F00));
         break;
         
      case TCTL1:
      case TCTL2:
         registerArrayWrite16(address, value & 0x01FF);
         break;
         
      case WATCHDOG:
         //writing to the watchdog resets the counter bits(8 and 9) to 0
         registerArrayWrite16(address, value & 0x0083);
         break;
         
      case RTCISR:
         registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) & ~value);
         if(!(registerArrayRead16(RTCISR) & 0xFF00))
            clearIprIsrBit(INT_RTI);
         if(!(registerArrayRead16(RTCISR) & 0x003F))
            clearIprIsrBit(INT_RTC);
         break;
         
      case PLLFSR:
         setPllfsr16(value);
         break;
         
      case PLLCR:
         setPllcr(value);
         break;
         
      case ICR:
         //missing bottom 7 bits
         registerArrayWrite16(address, value & 0xFF80);
         break;
         
      case DRAMC:
         //unemulated
         //missing bit 7 and 6
         registerArrayWrite16(address, value & 0xFF3F);
         break;
         
      case DRAMMC:
         //unemulated
         registerArrayWrite16(address, value);
         break;
         
      case SDCTRL:
         //unemulated
         //missing bits 13, 9, 8 and 7
         registerArrayWrite16(address, value & 0xDC7F);
         break;

      case CSA:
         setCsa(value);
         resetAddressSpace();
         break;

      case CSB:
         setCsb(value);
         resetAddressSpace();
         break;

      case CSC:
         setCsc(value);
         resetAddressSpace();
         break;

      case CSD:
         setCsd(value);
         resetAddressSpace();
         break;

      case CSGBA:
         //sets the starting location of ROM(0x10000000)
         setCsgba(value);
         resetAddressSpace();
         break;

      case CSGBB:
         //sets the starting location of the SED1376(0x1FF80000)
         setCsgbb(value);
         resetAddressSpace();
         break;

      case CSGBC:
         //sets the starting location of USBPhilipsPDIUSBD12(address 0x10400000)
         //since I dont plan on adding hotsync should be fine to leave unemulated, its unemulated in pose
         setCsgbc(value);
         resetAddressSpace();
         break;

      case CSGBD:
         //sets the starting location of RAM(0x00000000)
         setCsgbd(value);
         resetAddressSpace();
         break;

      case CSUGBA:
         registerArrayWrite16(CSUGBA, value);
         //refresh all chipselect address lines
         setCsgba(registerArrayRead16(CSGBA));
         setCsgbb(registerArrayRead16(CSGBB));
         setCsgbc(registerArrayRead16(CSGBC));
         setCsgbd(registerArrayRead16(CSGBD));
         resetAddressSpace();
         break;

      case CSCTRL1:
         setCsctrl1(value);
         resetAddressSpace();
         break;
         
      default:
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_UNKNOWN) && !defined(EMU_LOG_REGISTER_ACCESS_ALL)
         printUnknownHwAccess(address, value, 16, true);
#endif
         break;
   }
}

void setHwRegister32(unsigned int address, unsigned int value){
   if((address & 0x0000F000) == 0x0000E000){
      //32 bit emu register write, valid
      return;
   }
   else if((address & 0x0000F000) != 0x0000F000){
      //not emu or hardware register, invalid access
      return;
   }
   
   address &= 0x00000FFF;
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_ALL)
   printUnknownHwAccess(address, value, 32, true);
#endif
   switch(address){
         
      case RTCTIME:
         registerArrayWrite32(address, value & 0x1F3F003F);
         break;
         
      case IDR:
      case IPR:
         //write to read only register, do nothing
         break;
         
      case ISR:
         //clear ISR and IPR for external hardware whereever there is a 1 bit in value
         registerArrayWrite32(IPR, registerArrayRead32(IPR) & ~(value & 0x000F0F00/*external hardware int mask*/));
         registerArrayWrite32(ISR, registerArrayRead32(ISR) & ~(value & 0x000F0F00/*external hardware int mask*/));
         break;
         
      case IMR:
         registerArrayWrite32(address, value & 0x00FF3FFF);
         break;
         
      case LSSA:
         //simple write, no actions needed
         registerArrayWrite32(address, value);
         break;
      
      default:
#if defined(EMU_DEBUG) && defined(EMU_LOG_REGISTER_ACCESS_UNKNOWN) && !defined(EMU_LOG_REGISTER_ACCESS_ALL)
         printUnknownHwAccess(address, value, 32, true);
#endif
         break;
   }
}


void resetHwRegisters(){
   memset(palmReg, 0x00, REG_SIZE - BOOTLOADER_SIZE);
   clk32Counter = 0;
   pllWakeWait = -1;
   timer1CycleCounter = 0.0;
   timer2CycleCounter = 0.0;
   for(uint32_t chip = CHIP_BEGIN; chip < CHIP_END; chip++){
      chips[chip].enable = false;
      chips[chip].start = 0x00000000;
      chips[chip].size = 0x00000000;
      chips[chip].mask = 0x00000000;

      chips[chip].inBootMode = false;
      chips[chip].readOnly = false;
      chips[chip].readOnlyForProtectedMemory = false;
      chips[chip].supervisorOnlyProtectedMemory = false;
      chips[chip].unprotectedSize = 0x00000000;
   }
   //all chipselects are disabled at boot and CSA is mapped to 0x00000000 and covers the entire address range until CSGBA set otherwise
   chips[CHIP_A_ROM].inBootMode = true;

   //masks for reading and writing
   chips[CHIP_A_ROM].mask = 0x003FFFFF;
   chips[CHIP_B_SED].mask = 0x0003FFFF;
   chips[CHIP_C_USB].mask = 0x00000000;
   chips[CHIP_D_RAM].mask = palmSpecialFeatures & FEATURE_RAM_HUGE ? 0x07FFFFFF/*128mb*/ : 0x00FFFFFF/*16mb*/;
   
   //system control
   registerArrayWrite8(SCR, 0x1C);
   
   //CPU id
   registerArrayWrite32(IDR, 0x56000000);
   
   //I/O drive control //probably unused
   registerArrayWrite16(IODCR, 0x1FFF);
   
   //chip selects
   registerArrayWrite16(CSA, 0x00B0);
   registerArrayWrite16(CSD, 0x0200);
   registerArrayWrite16(EMUCS, 0x0060);
   registerArrayWrite16(CSCTRL2, 0x1000);
   registerArrayWrite16(CSCTRL3, 0x9C00);
   
   //phase lock loop
   registerArrayWrite16(PLLCR, 0x24B3);
   registerArrayWrite16(PLLFSR, 0x0347);
   
   //power control
   registerArrayWrite8(PCTLR, 0x1F);
   
   //interrupts
   registerArrayWrite32(IMR, 0x00FFFFFF);
   registerArrayWrite16(ILCR, 0x6533);
   
   //GPIO ports
   registerArrayWrite8(PADATA, 0xFF);
   registerArrayWrite8(PAPUEN, 0xFF);
   
   registerArrayWrite8(PBDATA, 0xFF);
   registerArrayWrite8(PBPUEN, 0xFF);
   registerArrayWrite8(PBSEL, 0xFF);
   
   registerArrayWrite8(PCPDEN, 0xFF);
   registerArrayWrite8(PCSEL, 0xFF);
   
   registerArrayWrite8(PDDATA, 0xFF);
   registerArrayWrite8(PDPUEN, 0xFF);
   registerArrayWrite8(PDSEL, 0xF0);
   
   registerArrayWrite8(PEDATA, 0xFF);
   registerArrayWrite8(PEPUEN, 0xFF);
   registerArrayWrite8(PESEL, 0xFF);
   
   registerArrayWrite8(PFDATA, 0xFF);
   registerArrayWrite8(PFPUEN, 0xFF);
   registerArrayWrite8(PFSEL, 0x87);
   
   registerArrayWrite8(PGDATA, 0x3F);
   registerArrayWrite8(PGPUEN, 0x3D);
   registerArrayWrite8(PGSEL, 0x08);
   
   registerArrayWrite8(PJDATA, 0xFF);
   registerArrayWrite8(PJPUEN, 0xFF);
   registerArrayWrite8(PJSEL, 0xEF);
   
   registerArrayWrite8(PKDATA, 0x0F);
   registerArrayWrite8(PKPUEN, 0xFF);
   registerArrayWrite8(PKSEL, 0xFF);
   
   registerArrayWrite8(PMDATA, 0x20);
   registerArrayWrite8(PMPUEN, 0x3F);
   registerArrayWrite8(PMSEL, 0x3F);
   
   //pulse width modulation control
   registerArrayWrite16(PWMC1, 0x0020);
   registerArrayWrite8(PWMP1, 0xFE);
   
   //timers
   registerArrayWrite16(TCMP1, 0xFFFF);
   registerArrayWrite16(TCMP2, 0xFFFF);
   
   //serial I/O
   registerArrayWrite16(UBAUD1, 0x0002);
   registerArrayWrite16(UBAUD2, 0x0002);
   registerArrayWrite16(HMARK, 0x0102);
   
   //LCD control registers, unused since the SED1376 is present
   registerArrayWrite8(LVPW, 0xFF);
   registerArrayWrite16(LXMAX, 0x03F0);
   registerArrayWrite16(LYMAX, 0x01FF);
   registerArrayWrite16(LCWCH, 0x0101);
   registerArrayWrite8(LBLKC, 0x7F);
   registerArrayWrite16(LRRA, 0x00FF);
   registerArrayWrite8(LGPMR, 0x84);
   registerArrayWrite8(DMACR, 0x62);
   
   //realtime clock
   //RTCTIME is not changed on reset
   registerArrayWrite16(WATCHDOG, 0x0001);
   registerArrayWrite16(RTCCTL, 0x0080);//conflicting size in datasheet, it says its 8 bit but provides 16 bit values
   registerArrayWrite16(STPWCH, 0x003F);//conflicting size in datasheet, it says its 8 bit but provides 16 bit values
   //DAYR is not changed on reset
   
   //SDRAM control, unused since RAM refresh is unemulated
   registerArrayWrite16(SDCTRL, 0x003C);
   
   //add register settings to misc I/O
   updateAlarmLedStatus();
   updateLcdStatus();
   updateBacklightStatus();
   updateVibratorStatus();
}

void setRtc(uint32_t days, uint32_t hours, uint32_t minutes, uint32_t seconds){
   uint32_t rtcTime;
   rtcTime = seconds & 0x0000003F;
   rtcTime |= (minutes << 16) & 0x003F0000;
   rtcTime |= (hours << 24) & 0x1F000000;
   registerArrayWrite32(RTCTIME, rtcTime);
   registerArrayWrite16(DAYR, days & 0x01FF);
}
