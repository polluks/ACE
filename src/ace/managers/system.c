/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <ace/managers/system.h>
#include <stdlib.h>
#include <clib/graphics_protos.h>
#include <clib/dos_protos.h>
#include <hardware/intbits.h>
#include <hardware/dmabits.h>
#include <ace/utils/custom.h>
#include <ace/managers/log.h>
#include <ace/managers/timer.h>
#include <exec/execbase.h>
#include <proto/exec.h> // Bartman's compiler needs this
#include <proto/dos.h> // Bartman's compiler needs this
#include <proto/graphics.h> // Bartman's compiler needs this

// There are hardware interrupt vectors
// Some may be triggered by more than one event - there are 15 events
// http://eab.abime.net/showthread.php?p=1081007
// http://ada.untergrund.net/?p=boardthread&id=31
// http://palbo.dk/dataskolen/maskinsprog/english/letter_09.pdf
// Clearing bit is done twice for some high end configs
// http://eab.abime.net/showthread.php?t=95263

//---------------------------------------------------------------------- DEFINES

// First OS interrupt is at offset 0x64 - each is 4 bytes wide
#define SYSTEM_INT_VECTOR_FIRST (0x64/4)
#define SYSTEM_INT_VECTOR_COUNT 7
#define SYSTEM_INT_HANDLER_COUNT 15

//------------------------------------------------------------------------ TYPES

typedef void (*tHwIntVector)(void);

typedef struct _tAceInterrupt {
	volatile tAceIntHandler pHandler;
	volatile void *pData;
} tAceInterrupt;

//---------------------------------------------------------------------- GLOBALS

// Saved regs
static UWORD s_uwOsIntEna;
static UWORD s_uwOsDmaCon;
static UWORD s_uwAceDmaCon = 0;
static UWORD s_uwOsInitialDma;

static UWORD s_pOsCiaTimerA[CIA_COUNT];
static UWORD s_pOsCiaTimerB[CIA_COUNT];
static UWORD s_pAceCiaTimerA[CIA_COUNT] = {0xFFFF, 0xFFFF}; // as long as possible
static UWORD s_pAceCiaTimerB[CIA_COUNT] = {0xFFFF, 0xFFFF};

// Interrupts
static void HWINTERRUPT int1Handler(void);
static void HWINTERRUPT int2Handler(void);
static void HWINTERRUPT int3Handler(void);
static void HWINTERRUPT int4Handler(void);
static void HWINTERRUPT int5Handler(void);
static void HWINTERRUPT int6Handler(void);
static void HWINTERRUPT int7Handler(void);
static const tHwIntVector s_pAceHwInterrupts[SYSTEM_INT_VECTOR_COUNT] = {
	int1Handler, int2Handler, int3Handler, int4Handler,
	int5Handler, int6Handler, int7Handler
};
static volatile tHwIntVector s_pOsHwInterrupts[SYSTEM_INT_VECTOR_COUNT] = {0};
static volatile tHwIntVector * s_pHwVectors = 0;
static volatile tAceInterrupt s_pAceInterrupts[SYSTEM_INT_HANDLER_COUNT] = {{0}};
static volatile tAceInterrupt s_pAceCiaInterrupts[CIA_COUNT][5] = {{{0}}};
static volatile UWORD s_uwAceInts = 0;

// Manager logic vars
static WORD s_wSystemUses;
struct GfxBase *GfxBase = 0;
struct View *s_pOsView;
static const UWORD s_uwOsMinDma = DMAF_DISK | DMAF_BLITTER;

#if defined(BARTMAN_GCC)
struct DosLibrary *DOSBase = 0;
struct ExecBase *SysBase = 0;
#endif

//----------------------------------------------------------- INTERRUPT HANDLERS

// All interrupt handlers should clear their flags twice
// http://eab.abime.net/showthread.php?p=834185#post834185

FN_HOTSPOT
void HWINTERRUPT int1Handler(void) {
	logPushInt();
	// Soft / diskBlk / TBE (RS232 TX end)
	logPopInt();
}

FN_HOTSPOT
void HWINTERRUPT int2Handler(void) {
	logPushInt();
	UWORD uwIntReq = g_pCustom->intreqr;
	if(uwIntReq & INTF_PORTS) {
		// CIA A interrupt - Parallel, keyboard
		UBYTE ubIcr = g_pCia[CIA_A]->icr; // Read clears interrupt flags
		if(ubIcr & CIAICRF_SERIAL) {
			// Keyboard
			if(s_pAceCiaInterrupts[CIA_A][CIAICRB_SERIAL].pHandler) {
				s_pAceCiaInterrupts[CIA_A][CIAICRB_SERIAL].pHandler(
					g_pCustom, s_pAceCiaInterrupts[CIA_A][CIAICRB_SERIAL].pData
				);
			}
			if(ubIcr & CIAICRF_TIMER_A) {
				if(s_pAceCiaInterrupts[CIA_A][CIAICRB_TIMER_A].pHandler) {
					s_pAceCiaInterrupts[CIA_A][CIAICRB_TIMER_A].pHandler(
						g_pCustom, s_pAceCiaInterrupts[CIA_A][CIAICRB_TIMER_A].pData
					);
				}
			}
			// CIA-A timer A is used for keyboard, timer B for exec task switch
			// For some reason after save/restore A.ta is used without problems,
			// but A.tb makes OS unstable - we don't use it until it gets fixed
			// if(ubIcr & CIAICRF_TIMER_B) {
			// 	if(s_pAceCiaInterrupts[CIA_A][CIAICRB_TIMER_B].pHandler) {
			// 		s_pAceCiaInterrupts[CIA_A][CIAICRB_TIMER_B].pHandler(
			// 			g_pCustom, s_pAceCiaInterrupts[CIA_A][CIAICRB_TIMER_B].pData
			// 		);
			// 	}
			// }
			// TODO: this could be re-enabled in vblank since we don't need it
			// to retrigger during same frame. Or perhaps it's needed so that kbd
			// controller won't overflow its queue
		}
		g_pCustom->intreq = INTF_PORTS;
		g_pCustom->intreq = INTF_PORTS;
	}
	// TODO: handle "mouse, some of disk functions"
	logPopInt();
}

FN_HOTSPOT
void HWINTERRUPT int3Handler(void) {
	logPushInt();
	// VBL / Copper / Blitter
	UWORD uwIntReq = g_pCustom->intreqr;
	UWORD uwReqClr = 0;

	// Vertical blanking
	if(uwIntReq & INTF_VERTB) {
		// Do ACE-specific stuff
		// TODO when ACE gets ported to C++ this could be constexpr if'ed
		timerOnInterrupt();

		// Process handlers
		if(s_pAceInterrupts[INTB_VERTB].pHandler) {
			s_pAceInterrupts[INTB_VERTB].pHandler(
				g_pCustom, s_pAceInterrupts[INTB_VERTB].pData
			);
		}
		uwReqClr = INTF_VERTB;
	}

	// Copper
	if(uwIntReq & INTF_COPER) {
		if(s_pAceInterrupts[INTB_COPER].pHandler) {
			s_pAceInterrupts[INTB_COPER].pHandler(
				g_pCustom, s_pAceInterrupts[INTB_VERTB].pData
			);
		}
		uwReqClr |= INTF_COPER;
	}

	// Blitter
	if(uwIntReq & INTF_BLIT) {
		if(s_pAceInterrupts[INTB_BLIT].pHandler) {
			s_pAceInterrupts[INTB_BLIT].pHandler(
				g_pCustom, s_pAceInterrupts[INTB_VERTB].pData
			);
		}
		uwReqClr |= INTF_BLIT;
	}
	logPopInt();
	g_pCustom->intreq = uwReqClr;
	g_pCustom->intreq = uwReqClr;
}

FN_HOTSPOT
void HWINTERRUPT int4Handler(void) {
	logPushInt();
	UWORD uwIntReq = g_pCustom->intreqr;
	UWORD uwReqClr = 0;

	// Audio channel 0
	if(uwIntReq & INTF_AUD0) {
		if(s_pAceInterrupts[INTB_AUD0].pHandler) {
			s_pAceInterrupts[INTB_AUD0].pHandler(
				g_pCustom, s_pAceInterrupts[INTB_AUD0].pData
			);
		}
		uwReqClr |= INTF_AUD0;
	}

	// Audio channel 1
	if(uwIntReq & INTF_AUD1) {
		if(s_pAceInterrupts[INTB_AUD1].pHandler) {
			s_pAceInterrupts[INTB_AUD1].pHandler(
				g_pCustom, s_pAceInterrupts[INTB_AUD1].pData
			);
		}
		uwReqClr |= INTF_AUD1;
	}

	// Audio channel 2
	if(uwIntReq & INTF_AUD2) {
		if(s_pAceInterrupts[INTB_AUD2].pHandler) {
			s_pAceInterrupts[INTB_AUD2].pHandler(
				g_pCustom, s_pAceInterrupts[INTB_AUD2].pData
			);
		}
		uwReqClr |= INTF_AUD2;
	}

	// Audio channel 3
	if(uwIntReq & INTF_AUD3) {
		if(s_pAceInterrupts[INTB_AUD3].pHandler) {
			s_pAceInterrupts[INTB_AUD3].pHandler(
				g_pCustom, s_pAceInterrupts[INTB_AUD3].pData
			);
		}
		uwReqClr |= INTF_AUD3;
	}
	logPopInt();
	g_pCustom->intreq = uwReqClr;
	g_pCustom->intreq = uwReqClr;
}

FN_HOTSPOT
void HWINTERRUPT int5Handler(void) {
	logPushInt();
	// DskSyn / RBF
	logPopInt();
}

FN_HOTSPOT
void HWINTERRUPT int6Handler(void) {
	logPushInt();
	// TODO: perhaps I should disable CIA interrupts while I process them?
	UWORD uwIntReq = g_pCustom->intreqr;
	UWORD uwReqClr = 0;

	if(uwIntReq & INTF_EXTER) {
		// INTF_EXTER - CIA B
		// Check and clear CIA interrupt flags
		UBYTE ubIcr = g_pCia[CIA_B]->icr;
		if(ubIcr & CIAICRF_SERIAL) {
			if(s_pAceCiaInterrupts[CIA_B][CIAICRB_SERIAL].pHandler) {
				s_pAceCiaInterrupts[CIA_B][CIAICRB_SERIAL].pHandler(
					g_pCustom, s_pAceCiaInterrupts[CIA_B][CIAICRB_SERIAL].pData
				);
			}
		}
		if(ubIcr & CIAICRF_TIMER_A) {
			if(s_pAceCiaInterrupts[CIA_B][CIAICRB_TIMER_A].pHandler) {
				s_pAceCiaInterrupts[CIA_B][CIAICRB_TIMER_A].pHandler(
					g_pCustom, s_pAceCiaInterrupts[CIA_B][CIAICRB_TIMER_A].pData
				);
			}
		}
		if(ubIcr & CIAICRF_TIMER_B) {
			if(s_pAceCiaInterrupts[CIA_B][CIAICRB_TIMER_B].pHandler) {
				s_pAceCiaInterrupts[CIA_B][CIAICRB_TIMER_B].pHandler(
					g_pCustom, s_pAceCiaInterrupts[CIA_B][CIAICRB_TIMER_B].pData
				);
			}
		}
		uwReqClr |= INTF_EXTER;
		// TODO: I heard that Zorro and Clockport can trigger this interrupt
		// and it sets the same bit. So there should be a handler for it in case of
		// ICR intbits not being set.
	}
	// TOOD: is there any other interrupt source on level 6?
	// If yes, then detect it. If not, remove check for EXTER flag

	logPopInt();
	// Clear int flags
	g_pCustom->intreq = uwReqClr;
	g_pCustom->intreq = uwReqClr;
}

FN_HOTSPOT
void HWINTERRUPT int7Handler(void) {
	logPushInt();
	// EXTERNAL
	logPopInt();
}

//-------------------------------------------------------------------- FUNCTIONS

/**
 * @brief Flushes FDD disk activity.
 * Originally written by Asman in asm, known as osflush
 * Rewritten by Asman for issue #76, reformatted for ACE codestyle
 * More explanation about how it works:
 * http://eab.abime.net/showthread.php?t=87202&page=2
 */
static void systemFlushIo() {
	struct StandardPacket *pPacket = (struct StandardPacket*)AllocMem(
		sizeof(struct StandardPacket), MEMF_CLEAR
	);

	if(!pPacket) {
		return;
	}

	// Get filesystem message port
	struct MsgPort *pMsgPort = DeviceProc((CONST_STRPTR)"sys");
	if (pMsgPort) {
		// Get our message port
		struct Process *pProcess = (struct Process *)FindTask(0);
		struct MsgPort *pProcessMsgPort = &pProcess->pr_MsgPort;

		// Fill in packet
		struct DosPacket *pDosPacket = &pPacket->sp_Pkt;
		struct Message *pMsg = &pPacket->sp_Msg;

		// It is how Tripos packet system was hacked to exec messaging system
		pMsg->mn_Node.ln_Name = (char*)pDosPacket;
		pMsg->mn_ReplyPort = pProcessMsgPort;

		pDosPacket->dp_Link = pMsg;
		pDosPacket->dp_Port = pProcessMsgPort;
		pDosPacket->dp_Type = ACTION_FLUSH;

		// Send packet
		PutMsg(pMsgPort, pMsg);

		// Wait for reply
		WaitPort(pProcessMsgPort);
		GetMsg(pProcessMsgPort);
	}

	FreeMem(pPacket, sizeof(struct StandardPacket));
}

void systemKill(const char *szMsg) {
	printf("ERR: SYSKILL: '%s'", szMsg);
	logWrite("ERR: SYSKILL: '%s'", szMsg);

	if(GfxBase) {
		CloseLibrary((struct Library *) GfxBase);
	}
	if(DOSBase) {
		CloseLibrary((struct Library *) DOSBase);
	}
	exit(EXIT_FAILURE);
}

/**
 * @brief The startup code to give ACE somewhat initial state.
 */
void systemCreate(void) {
#if defined(BARTMAN_GCC)
	// Bartman's startup code doesn't initialize anything
	SysBase = *((struct ExecBase**)4UL);
#endif

	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 0L);
	if (!GfxBase) {
		systemKill("Can't open Gfx Library!\n");
		return;
	}

	DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
	if (!DOSBase) {
		systemKill("Can't open DOS Library!\n");
		return;
	}

  // Determine original stack size
  struct Process *pProcess = (struct Process *)FindTask(NULL);
  ULONG ulStackSize = (char *)pProcess->pr_Task.tc_SPUpper - (char *)pProcess->pr_Task.tc_SPLower;
  if (pProcess->pr_CLI) {
    ulStackSize = *(ULONG *)pProcess->pr_ReturnAddr;
  }
	logWrite("Stack size: %lu\n", ulStackSize);

	// Disable as much of OS stuff as possible so that it won't trash stuff when
	// re-enabled periodically.
	// Save the system copperlists and flush the view

	// This prevents "copjmp nasty bug"
	// http://eab.abime.net/showthread.php?t=71190
	// Asman told me that Ross from EAB found out that LoadView may cause such behavior
	OwnBlitter();
	WaitBlit();

	s_pOsView = GfxBase->ActiView;
	WaitTOF();
	LoadView(0);
	WaitTOF();
	WaitTOF(); // Wait for interlaced screen to finish

	// get VBR location on 68010+ machine
	// http://eab.abime.net/showthread.php?t=65430&page=3
	if (SysBase->AttnFlags & AFF_68010) {
		UWORD pGetVbrCode[] = {0x4e7a, 0x0801, 0x4e73};
		s_pHwVectors = (tHwIntVector *)Supervisor((void *)pGetVbrCode);
	}

	// Finish disk activity
	systemFlushIo();

	// save the state of the hardware registers (INTENA, DMA, ADKCON etc.)
	s_uwOsIntEna = g_pCustom->intenar;
	s_uwOsDmaCon = g_pCustom->dmaconr;
	s_uwOsInitialDma = s_uwOsDmaCon;

	// Disable interrupts (this is the actual "kill system/OS" part)
	g_pCustom->intena = 0x7FFF;
	g_pCustom->intreq = 0x7FFF;

	// Disable all DMA - only once
	// Wait for vbl before disabling sprite DMA
	while (!(g_pCustom->intreqr & INTF_VERTB)) {}
	g_pCustom->dmacon = 0x07FF;

	// Unuse system so that it gets backed up once and then re-enable
	// as little as needed
	s_wSystemUses = 1;
	systemUnuse();
	systemUse();
}

/**
 * @brief Cleanup after app, restore anything that systemCreate took over.
 */
void systemDestroy(void) {
	// Disable all interrupts
	g_pCustom->intena = 0x7FFF;
	g_pCustom->intreq = 0x7FFF;

	// Wait for vbl before disabling sprite DMA
	while (!(g_pCustom->intreqr & INTF_VERTB)) {}
	g_pCustom->dmacon = 0x07FF;
	g_pCustom->intreq = 0x7FFF;

	// Start waking up OS
	if(s_wSystemUses != 1) {
		logWrite("ERR: unclosed/overclosed system usage count: %hd\n", s_wSystemUses);
		// Ensure that OS gets restured - along with
	}

	// Restore every single OS DMA & interrupt
	s_wSystemUses = 0;
	systemUse();
	g_pCustom->dmacon = DMAF_SETCLR | DMAF_MASTER | s_uwOsInitialDma;

	// restore old view
	WaitTOF();
	LoadView(s_pOsView);
	WaitTOF();

	WaitBlit();
	DisownBlitter();

	logWrite("Closing graphics.library...\n");
	CloseLibrary((struct Library *) GfxBase);
	logWrite("Closing dos.library...\n");
	CloseLibrary((struct Library *) DOSBase);
}

void systemUnuse(void) {
	--s_wSystemUses;
	if(!s_wSystemUses) {
		if(g_pCustom->dmaconr & DMAF_DISK) {
			// Flush disk activity if it was used
			// This 'if' is here because otherwise systemUnuse() called
			// by systemCreate() would indefinitely wait for OS when it's killed.
			// systemUse() restores disk DMA, so it's an easy check if OS was
			// actually restored.
			systemFlushIo();
		}

		// Disable interrupts (this is the actual "kill system/OS" part)
		g_pCustom->intena = 0x7FFF;
		g_pCustom->intreq = 0x7FFF;

		// Disable CIA interrupts
		g_pCia[CIA_A]->icr = 0x7F;
		g_pCia[CIA_B]->icr = 0x7F;

		// save OS CIA timer values
		s_pOsCiaTimerA[CIA_A] = ciaGetTimerA(g_pCia[CIA_A]);
		// s_pOsCiaTimerB[CIA_A] = ciaGetTimerB(g_pCia[CIA_A]);
		s_pOsCiaTimerA[CIA_B] = ciaGetTimerA(g_pCia[CIA_B]);
		s_pOsCiaTimerB[CIA_B] = ciaGetTimerB(g_pCia[CIA_B]);

		// set ACE CIA timers
		ciaSetTimerA(g_pCia[CIA_A], s_pAceCiaTimerA[CIA_A]);
		// ciaSetTimerB(g_pCia[CIA_A], s_pAceCiaTimerB[CIA_A]);
		ciaSetTimerA(g_pCia[CIA_B], s_pAceCiaTimerA[CIA_B]);
		ciaSetTimerB(g_pCia[CIA_B], s_pAceCiaTimerB[CIA_B]);

		// Enable ACE CIA interrupts
		g_pCia[CIA_A]->icr = (
			CIAICRF_SETCLR | CIAICRF_SERIAL | CIAICRF_TIMER_A | CIAICRF_TIMER_B
		);
		g_pCia[CIA_B]->icr = (
			CIAICRF_SETCLR | CIAICRF_SERIAL | CIAICRF_TIMER_A | CIAICRF_TIMER_B
		);

		// Game's bitplanes & copperlists are still used so don't disable them
		// Wait for vbl before disabling sprite DMA
		while (!(g_pCustom->intreqr & INTF_VERTB)) {}
		g_pCustom->dmacon = s_uwOsMinDma;

		// Save OS interrupt vectors and enable ACE's
		g_pCustom->intreq = 0x7FFF;
		for(UWORD i = 0; i < SYSTEM_INT_VECTOR_COUNT; ++i) {
			s_pOsHwInterrupts[i] = s_pHwVectors[SYSTEM_INT_VECTOR_FIRST + i];
			s_pHwVectors[SYSTEM_INT_VECTOR_FIRST + i] = s_pAceHwInterrupts[i];
		}

		// Enable needed DMA (and interrupt) channels
		g_pCustom->dmacon = DMAF_SETCLR | DMAF_MASTER | s_uwAceDmaCon;
		// Everything that's supported by ACE to simplify things for now
		g_pCustom->intena = INTF_SETCLR | INTF_INTEN | (
			INTF_BLIT | INTF_COPER | INTF_VERTB | INTF_EXTER |
			INTF_PORTS | INTF_AUD0 | INTF_AUD1 | INTF_AUD2 | INTF_AUD3
		);
	}
#if defined(ACE_DEBUG)
	if(s_wSystemUses < 0) {
		logWrite("ERR: System uses less than 0!\n");
		s_wSystemUses = 0;
	}
#endif
}

void systemUse(void) {
	if(!s_wSystemUses) {
		// Disable app interrupts/dma, keep display-related DMA
		g_pCustom->intena = 0x7FFF;
		g_pCustom->intreq = 0x7FFF;
		g_pCustom->dmacon = s_uwOsMinDma;
		while (!(g_pCustom->intreqr & INTF_VERTB)) {}

		// Disable CIA interrupts
		g_pCia[CIA_A]->icr = 0x7F;
		g_pCia[CIA_B]->icr = 0x7F;

		// Restore interrupt vectors - also CIA
		for(UWORD i = 0; i < SYSTEM_INT_VECTOR_COUNT; ++i) {
			s_pHwVectors[SYSTEM_INT_VECTOR_FIRST + i] = s_pOsHwInterrupts[i];
		}

		// Restore old CIA timer values
		ciaSetTimerA(g_pCia[CIA_A], s_pOsCiaTimerA[CIA_A]);
		// ciaSetTimerB(g_pCia[CIA_A], s_pOsCiaTimerB[CIA_A]);
		ciaSetTimerA(g_pCia[CIA_B], s_pOsCiaTimerA[CIA_B]);
		ciaSetTimerB(g_pCia[CIA_B], s_pOsCiaTimerB[CIA_B]);

		// re-enable CIA-B ALRM interrupt which was set by AmigaOS
		// According to UAE debugger there's nothing in CIA_A
		g_pCia[CIA_B]->icr = CIAICRF_SETCLR | CIAICRF_TOD;

		// restore old DMA/INTENA/ADKCON etc. settings
		// All interrupts but only needed DMA
		g_pCustom->dmacon = DMAF_SETCLR | DMAF_MASTER | (s_uwOsDmaCon & s_uwOsMinDma);
		g_pCustom->intena = INTF_SETCLR | INTF_INTEN  | s_uwOsIntEna;
	}
	++s_wSystemUses;
}

UBYTE systemIsUsed(void) {
	return s_wSystemUses > 0;
}

void systemSetInt(
	UBYTE ubIntNumber, tAceIntHandler pHandler, volatile void *pIntData
) {
	// Disable ACE handler during data swap to ensure atomic op
	s_pAceInterrupts[ubIntNumber].pHandler = 0;
	s_pAceInterrupts[ubIntNumber].pData = pIntData;

	// Re-enable handler or disable it if 0 was passed
	s_pAceInterrupts[ubIntNumber].pHandler = pHandler;
}

void systemSetCiaInt(
	UBYTE ubCia, UBYTE ubIntBit, tAceIntHandler pHandler, volatile void *pIntData
) {
	// Disable ACE handler during data swap to ensure atomic op
	s_pAceCiaInterrupts[ubCia][ubIntBit].pHandler = 0;
	s_pAceCiaInterrupts[ubCia][ubIntBit].pData = pIntData;

	// Re-enable handler or disable it if 0 was passed
	s_pAceCiaInterrupts[ubCia][ubIntBit].pHandler = pHandler;
}

void systemSetDmaBit(UBYTE ubDmaBit, UBYTE isEnabled) {
	UWORD uwDmaFlag = BV(ubDmaBit);
	systemSetDmaMask(uwDmaFlag, isEnabled);
}

void systemSetDmaMask(UWORD uwDmaMask, UBYTE isEnabled) {
	if(isEnabled) {
		s_uwAceDmaCon |= uwDmaMask;
		s_uwOsDmaCon |= uwDmaMask;
		if(!s_wSystemUses || !(uwDmaMask & s_uwOsMinDma)) {
			// Enable right now if OS is asleep or it's not critical for it to live
			g_pCustom->dmacon = DMAF_SETCLR | uwDmaMask;
		}
	}
	else {
		s_uwAceDmaCon &= ~uwDmaMask;
		if(!(uwDmaMask & s_uwOsMinDma)) {
			s_uwOsDmaCon &= ~uwDmaMask;
		}
		if(!s_wSystemUses || !(uwDmaMask & s_uwOsMinDma)) {
			// Disable right now if OS is asleep or it's not critical for it to live
			g_pCustom->dmacon = uwDmaMask;
		}
	}
}

void systemSetTimer(UBYTE ubCia, UBYTE ubTimer, UWORD uwTicks) {
	// Save timer values since backing them up on the fly in continuous mode
	// is impossible
	UWORD *pTimers = ((ubTimer == 0) ? s_pAceCiaTimerA : s_pAceCiaTimerB);
	pTimers[ubCia] = uwTicks;

	if(!s_wSystemUses) {
		// OS is already dead - set CIA timer right now
		if(!ubTimer) {
			ciaSetTimerA(g_pCia[ubCia], uwTicks);
		}
		else {
			ciaSetTimerB(g_pCia[ubCia], uwTicks);
		}
	}
}

void systemDump(void) {
	// logBlockBegin("systemDump()");

	logWrite("OS Usage counter: %hd\n", s_wSystemUses);

	// Print handlers
	// logWrite("ACE handlers:\n");
	// for(UWORD i = 0; i < SYSTEM_INT_HANDLER_COUNT; ++i) {
	// 	logWrite(
	// 		"Int %hu: code %p, data %p\n",
	// 		i, s_pAceInterrupts[i].pHandler, s_pAceInterrupts[i].pData
	// 	);
	// }

	// Print vectors
	// logWrite("Vectors:\n");
	// for(UWORD i = 0; i < SYSTEM_INT_VECTOR_COUNT; ++i) {
	// 	logWrite(
	// 		"vec %hu: ACE %p, OS %p, curr %p\n",
	// 		i, s_pAceHwInterrupts[i], s_pOsHwInterrupts[i],
	// 		s_pHwVectors[SYSTEM_INT_VECTOR_FIRST+ i]
	// 	);
	// }

	// logBlockEnd("systemDump()");
}
