// Run module
#include "burner.h"

int bRunPause = 0;
int bAltPause = 0;

int bAlwaysDrawFrames = 0;

static bool bShowFPS = false;
static unsigned int nDoFPS = 0;

static bool bMute = false;
static int nOldAudVolume;

int kNetGame = 0;						// Non-zero if network is being used
int kNetSpectator = 0;			// Non-zero if network replay is active

#ifdef FBNEO_DEBUG
int counter;								// General purpose variable used when debugging
#endif

static double nFrameLast = 0;		// Last value of getTime()

static bool bAppDoStep = 0;
static bool bAppDoFast = 0;
static bool bAppDoFasttoggled = 0;
static int nFastSpeed = 10;

int bRestartVideo = 0;
int bDrvExit = 0;
int bMediaExit = 0;

// For System Macros (below)
static int prevPause = 0, prevFFWD = 0, prevSState = 0, prevLState = 0, prevUState = 0;
UINT32 prevPause_debounce = 0;

static void CheckSystemMacros() // These are the Pause / FFWD macros added to the input dialog
{
	// Pause
	if (macroSystemPause && macroSystemPause != prevPause && timeGetTime() > prevPause_debounce+90) {
		if (bHasFocus) {
			PostMessage(hScrnWnd, WM_KEYDOWN, VK_PAUSE, 0);
			prevPause_debounce = timeGetTime();
		}
	}
	prevPause = macroSystemPause;
	// FFWD
	if (!kNetGame) {
		if (macroSystemFFWD) {
			bAppDoFast = 1; prevFFWD = 1;
		} else if (prevFFWD) {
			bAppDoFast = 0; prevFFWD = 0;
		}
	}
	// Load State
	if (macroSystemLoadState && macroSystemLoadState != prevLState) {
		PostMessage(hScrnWnd, WM_KEYDOWN, VK_F9, 0);
	}
	prevLState = macroSystemLoadState;
	// Save State
	if (macroSystemSaveState && macroSystemSaveState != prevSState) {
		PostMessage(hScrnWnd, WM_KEYDOWN, VK_F10, 0);
	}
	prevSState = macroSystemSaveState;
	// UNDO State
	if (macroSystemUNDOState && macroSystemUNDOState != prevUState) {
		scrnSSUndo();
	}
	prevUState = macroSystemUNDOState;
}

static int GetInput(bool bCopy)
{
	static unsigned int i = 0;
	InputMake(bCopy); 						// get input

	if (!kNetGame) {
		CheckSystemMacros();
	}

	// Update Input dialog every 3rd frame
	if ((i%3) == 0) {
		InpdUpdate();
	}

	// Update Input Set dialog
	InpsUpdate();
	return 0;
}

static time_t fpstimer;
static unsigned int nPreviousFrames;

static void DisplayFPSInit()
{
	nDoFPS = 0;
	fpstimer = 0;
	nPreviousFrames = nFramesRendered;
}

static void DisplayFPS()
{
	time_t temptime = clock();
	double fps = (double)(nFramesRendered - nPreviousFrames) * CLOCKS_PER_SEC / (temptime - fpstimer);
	if (bAppDoFast) {
		fps *= nFastSpeed+1;
	}

	if (kNetGame) {
		QuarkUpdateStats(fps);
	} else {
		if (fpstimer && (temptime - fpstimer) > 0) { // avoid strange fps values
			VidSSetStats(fps, 0, 0);
			VidOverlaySetStats(fps, 0, 0);
		}
	}

	fpstimer = temptime;
	nPreviousFrames = nFramesRendered;
}

// define this function somewhere above RunMessageLoop()
void ToggleLayer(unsigned char thisLayer)
{
	nBurnLayer ^= thisLayer;				// xor with thisLayer
	VidRedraw();
	VidPaint(0);
}

int bRunaheadFrame = 0;
int gAcbGameBufferSize = 0;
char gAcbGameBuffer[16 * 1024 * 1024];
char *gAcbGameScanPointer;

int RunaheadReadAcb(struct BurnArea* pba)
{
	memcpy(gAcbGameScanPointer, pba->Data, pba->nLen);
	gAcbGameScanPointer += pba->nLen;
	return 0;
}

int RunaheadWriteAcb(struct BurnArea* pba)
{
	memcpy(pba->Data, gAcbGameScanPointer, pba->nLen);
	gAcbGameScanPointer += pba->nLen;
	return 0;
}

void RunaheadSaveState()
{
	bRunaheadFrame = 1;
	gAcbGameScanPointer = gAcbGameBuffer;
	BurnAcb = RunaheadReadAcb;
	BurnAreaScan(ACB_FULLSCANL | ACB_READ, NULL);
	gAcbGameBufferSize = gAcbGameScanPointer - gAcbGameBuffer;
	bRunaheadFrame = 0;
}

void RunaheadLoadState()
{
	bRunaheadFrame = 1;
	gAcbGameScanPointer = gAcbGameBuffer;
	BurnAcb = RunaheadWriteAcb;
	BurnAreaScan(ACB_FULLSCANL | ACB_WRITE, NULL);
	bRunaheadFrame = 0;
}

LARGE_INTEGER qpFrequency;

double getTime()
{
	static int qpInit = 1;
	if (qpInit) {
		QueryPerformanceFrequency(&qpFrequency);
		qpInit = 0;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return ((1e9 * now.QuadPart) / qpFrequency.QuadPart) / (double)1e6;
}

// With or without sound, run one frame.
// If bDraw is true, it's the last frame before we are up to date, and so we should draw the screen
int RunFrame(int bDraw, int bPause)
{
	if (!bDrvOkay) {
		return 1;
	}

	if (bAudPlaying) {
		AudSoundCheck();
	}

	if (bPause) {
		GetInput(false);						// Update burner inputs, but not game inputs
	} else {
		nFramesEmulated++;
		nCurrentFrame++;

		if (kNetGame) {
			GetInput(true);						// Update inputs
			if (NetworkGetInput()) {	// Synchronize input with Network
				return 1;
			}
			if (kNetSpectator) {
				VidOverlaySaveInfo(true);
			}
		} else {
			if (nReplayStatus == 2) {
				GetInput(false);				  // Update burner inputs, but not game inputs
				if (ReplayInput()) {		  // Read input from file
					SetPauseMode(1);        // Replay has finished
					bAppDoFast = 0;
					bAppDoFasttoggled = 0;  // Disable FFWD
					MenuEnableItems();
					InputSetCooperativeLevel(false, false);
					return 0;
				}
			} else {
				GetInput(true);				  	// Update inputs
			}
		}

		if (nReplayStatus == 1) {
			RecordInput();					  	// Write input to file
		}

		// Render frame with video or audio
		if (bDraw) {
			int runahead = nVidRunahead;
			if (strstr(BurnDrvGetTextA(DRV_NAME), "tgm2")) {
				runahead = 0;
			}
			if (runahead > 0 && runahead <= 2) {
				pBurnSoundOut = nAudNextSound; 
				BurnDrvFrame();
				RunaheadSaveState();
				for (int i = 0; i < runahead; i++) {
					BurnDrvFrame();
				}
				VidFrame();
				RunaheadLoadState();
			} else {
				// Render video and audio
				pBurnSoundOut = nAudNextSound;
				VidFrame();
			}
			// add audio from last frame
			AudSoundFrame();
		} else {
			BurnDrvFrame();
		}

		if (kNetGame) {
			QuarkIncrementFrame();
		}

		DetectorUpdate();
		
#ifdef INCLUDE_AVI_RECORDING
		if (nAviStatus) {
			if (AviRecordFrame(bDraw)) {
				AviStop();
			}
		}
#endif
	}

	return 0;
}

int RunIdle()
{
	if (bAudPlaying) {
		AudSoundCheck();
	}

	// Render loop with sound
	double nTime = getTime();
	double nAccTime = nTime - nFrameLast;
	double nFps = 1000.0 * 100.0 / nAppVirtualFps;
	double nFpsIdle = nFps - 1.0;

	if (nAccTime < nFps) {
		if (kNetGame) {
			if (nAccTime < nFpsIdle)  {
				QuarkRunIdle(1);
			}
		} else {

			if (nAccTime < nFpsIdle) {
				Sleep(1);
			}
		}
		return 0;
	}

	// frame accumulator
	int nCount = -1;
	do {
		nAccTime -= nFps;
		nFrameLast += nFps;
		nCount++;
	} while (nAccTime > nFps);

	if (nCount > 10) {						// Limit frame skipping
		nCount = 10;
	}

	if (bRunPause) {
		if (bAppDoStep) {					  // Step one frame
			nCount = 1;
		} else {
			RunFrame(1, 1);						// Paused
			return 0;
		}
	}
	bAppDoStep = 0;

	if (kNetGame)
	{
		RunFrame(1, 0);					    // End-frame
	} else {
		if (bAppDoFast) {				    // do more frames
			for (int i = 0; i < nFastSpeed; i++) {
#ifdef INCLUDE_AVI_RECORDING
				if (nAviStatus) {
					RunFrame(1, 0);
				} else {
					RunFrame(0, 0);
				}
#else
				RunFrame(0, 0);
#endif
			}
		}
		if (!bAlwaysDrawFrames) {
			for (int i = nCount; i > 0; i--) {	// Mid-frames
				RunFrame(0, 0);
			}
		}
		RunFrame(1, 0);					// End-frame
	}

	// render
	nFramesRendered++;
	VidPaint(3);

	// fps
	if (nDoFPS < nFramesRendered) {
		DisplayFPS();
		nDoFPS = nFramesRendered + 30;
	}

	return 0;
}

int RunReset()
{
	// Reset FPS display
	DisplayFPSInit();

	// Reset the speed throttling code
	nFrameLast = getTime();

	return 0;
}

static int RunInit()
{
	AudSoundPlay();

	RunReset();

	return 0;
}

static int RunExit()
{
	// Stop sound if it was playing
	AudSoundStop();

	nFrameLast = 0;

	bAppDoFast = 0;
	bAppDoFasttoggled = 0;

	return 0;
}

// Keyboard callback function, a way to provide a driver with shifted/unshifted ASCII data from the keyboard.  see drv/msx/d_msx.cpp for usage
void (*cBurnerKeyCallback)(UINT8 code, UINT8 KeyType, UINT8 down) = NULL;

static void BurnerHandlerKeyCallback(MSG *Msg, INT32 KeyDown, INT32 KeyType)
{
	INT32 shifted = (GetAsyncKeyState(VK_SHIFT) & 0x80000000) ? 0xf0 : 0;
	INT32 scancode = (Msg->lParam >> 16) & 0xFF;
	UINT8 keyboardState[256];
	GetKeyboardState(keyboardState);
	char charvalue[2];
	if (ToAsciiEx(Msg->wParam, scancode, keyboardState, (LPWORD)&charvalue[0], 0, GetKeyboardLayout(0)) == 1) {
		cBurnerKeyCallback(charvalue[0], shifted|KeyType, KeyDown);
	}
}

// The main message loop
int RunMessageLoop()
{
	MSG Msg;

	do {
		bRestartVideo = 0;
		bDrvExit = 0;

		// Remove pending initialisation messages from the queue
		while (PeekMessage(&Msg, NULL, WM_APP + 0, WM_APP + 0, PM_NOREMOVE)) {
			if (Msg.message != WM_QUIT)	{
				PeekMessage(&Msg, NULL, WM_APP + 0, WM_APP + 0, PM_REMOVE);
			}
		}

		RunInit();

		ShowWindow(hScrnWnd, nAppShowCmd);								// Show the screen window
		nAppShowCmd = SW_NORMAL;
		SetForegroundWindow(hScrnWnd);

		GameInpCheckLeftAlt();
		GameInpCheckMouse();															// Hide the cursor

		if (bVidDWMSync) {
			bprintf(0, _T("[Win7+] Sync to DWM is enabled (if available).\n"));
			SuperWaitVBlankInit();
		}

		if (bAutoLoadGameList && !nCmdOptUsed) {
			static INT32 bLoaded = 0; // only do this once (at startup!)

			if (bLoaded == 0)
				PostMessage(hScrnWnd, WM_KEYDOWN, VK_F6, 0);
			bLoaded = 1;
		}

		while (1) {
			if (PeekMessage(&Msg, NULL, 0, 0, PM_REMOVE)) {
				// A message is waiting to be processed
				if (Msg.message == WM_QUIT)	{											// Quit program
					break;
				}
				if (Msg.message == (WM_APP + 0)) {										// Restart video
					bRestartVideo = 1;
					break;
				}

				if (bDrvExit)
				{
					DrvExit();
					bDrvExit = 0;
					continue;
				}

				if (bMediaExit)
				{
					MediaExit(true);
					break;
				}

				if (bMenuEnabled && nVidFullscreen == 0) {								// Handle keyboard messages for the menu
					if (MenuHandleKeyboard(&Msg)) {
						continue;
					}
				}

				if (Msg.message == WM_SYSKEYDOWN || Msg.message == WM_KEYDOWN) {
					if (Msg.lParam & 0x20000000) {
						// An Alt/AltGr-key was pressed
						switch (Msg.wParam) {

#if defined (FBNEO_DEBUG)
							case 'C': {
								static int count = 0;
								if (count == 0) {
									count++;
									{ char* p = NULL; if (*p) { printf("crash...\n"); } }
								}
								break;
							}
#endif
							case 'W':
								if (kNetGame) {
									QuarkTogglePerfMon();
								}
								break;

							// 'Silence' & 'Sound Restored' Code (added by CaptainCPS-X)
							case 'S': {
								TCHAR buffer[60];
								bMute = !bMute;

								if (bMute) {
									nOldAudVolume = nAudVolume;
									nAudVolume = 0;// mute sound
									_stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_MUTE, true), nAudVolume / 100);
								} else {
									nAudVolume = nOldAudVolume;// restore volume
									_stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_MUTE_OFF, true), nAudVolume / 100);
								}
								if (AudSoundSetVolume() == 0) {
									VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
								} else {
									VidSNewShortMsg(buffer);
								}
								break;
							}

							case VK_OEM_PLUS: {
								if (bMute) break; // if mute, not do this
								nOldAudVolume = nAudVolume;
								nAudVolume += 500;
								if (nAudVolume > 10000) {
									nAudVolume = 10000;
								}

								if (AudSoundSetVolume() == 0) {
									VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
								} else {
									TCHAR buffer[60];
									_stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_VOLUMESET, true), nAudVolume / 100);
									VidSNewShortMsg(buffer);
								}
								VidOverlayShowVolume(nAudVolume);
								break;
							}
							case VK_OEM_MINUS: {
								if (bMute) break; // if mute, not do this
								nOldAudVolume = nAudVolume;
								nAudVolume -= 500;
								if (nAudVolume < 0) {
									nAudVolume = 0;
								}

								if (AudSoundSetVolume() == 0) {
									VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
								} else {
									TCHAR buffer[60];
									_stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_VOLUMESET, true), nAudVolume / 100);
									VidSNewShortMsg(buffer);
								}
								VidOverlayShowVolume(nAudVolume);
								break;
							}
							case VK_MENU: {
								continue;
							}
						}
					} else {

						if (cBurnerKeyCallback)
							BurnerHandlerKeyCallback(&Msg, (Msg.message == WM_KEYDOWN) ? 1 : 0, 0);

						switch (Msg.wParam) {

#if 0	//defined (FBNEO_DEBUG)
							case 'N':
								counter--;
								if (counter < 0) {
									bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X (%d).\n"), counter, counter);
								} else {
									bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X.\n"), counter);
								}
								break;
							case 'M':
								counter++;
								if (counter < 0) {
									bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X (%d).\n"), counter, counter);
								} else {
									bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X.\n"), counter);
								}
								break;
#endif
							case VK_ESCAPE:
								if (bEditActive) {
									if (!nVidFullscreen) {
										DeActivateChat();
									}
								} else {
									if (nCmdOptUsed & 1) {
										PostQuitMessage(0);
									} else {
										if (nVidFullscreen) {
											nVidFullscreen = 0;
											POST_INITIALISE_MESSAGE;
										}
									}
								}
								break;

							case VK_RETURN:
								if (bEditActive) {
									int i = 0;
									while (EditText[i]) {
										if (EditText[i++] != 0x20) {
											break;
										}
									}
									if (i) {
										char text[MAX_CHAT_SIZE + 1];
										TCHARToANSI(EditText, text, MAX_CHAT_SIZE + 1);
										if (kNetGame) {
											QuarkSendChatText(text);
										}
									}
									DeActivateChat();
									break;
								}

								if (GetAsyncKeyState(VK_CONTROL) & 0x80000000) {
									bMenuEnabled = !bMenuEnabled;
									POST_INITIALISE_MESSAGE;
									break;
								}

								break;

							case VK_F1:
								if (!kNetGame) {
									if (Msg.lParam & 0x20000000) {
										bool bOldAppDoFast = bAppDoFast;

										if (((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT)) & 0x80000000) == 0) {
											if (bRunPause) {
												bAppDoStep = 1;
											} else {
												bAppDoFast = 1;
											}
										}

										if ((GetAsyncKeyState(VK_SHIFT) & 0x80000000) && !GetAsyncKeyState(VK_CONTROL)) { // Shift-F1: toggles FFWD state
											bAppDoFast = !bAppDoFast;
											bAppDoFasttoggled = bAppDoFast;
										}

										if (bOldAppDoFast != bAppDoFast) {
											DisplayFPSInit(); // resync fps display
										}
									}
								}
								break;

							case VK_BACK:
								if ((GetAsyncKeyState(VK_SHIFT) & 0x80000000) && !GetAsyncKeyState(VK_CONTROL))
								{ // Shift-Backspace: toggles recording/replay frame counter
									bReplayFrameCounterDisplay = !bReplayFrameCounterDisplay;
									if (!bReplayFrameCounterDisplay) {
										VidSKillTinyMsg();
									}
								} else if (!bEditActive) { // Backspace: toggles FPS counter
									bShowFPS = !bShowFPS;
									VidSShowStats(bShowFPS);
									VidOverlayShowStats(bShowFPS);
									DisplayFPS();
								}
								break;

							case 'T':
								if (kNetGame && !bEditActive) {
									if (AppMessage(&Msg)) {
										ActivateChat();
									}
								}
								break;

							case VK_TAB:
								if (GetAsyncKeyState(VK_SHIFT)) {
									nVidRunahead = (nVidRunahead + 1) % 3;
									MenuUpdate();
								}
								else if (kNetGame && !bEditActive) {
									if (!bVidOverlay) {
										bVidOverlay = !bVidOverlay;
										bVidBigOverlay = false;
									}
									else if (!bVidBigOverlay) {
										bVidBigOverlay = true;
									}
									else {
										bVidOverlay = false;
										bVidBigOverlay = false;
									}
									MenuUpdate();
								}
								break;
						}
					}
				} else {
					if (Msg.message == WM_SYSKEYUP || Msg.message == WM_KEYUP) {

						if (cBurnerKeyCallback)
							BurnerHandlerKeyCallback(&Msg, (Msg.message == WM_KEYDOWN) ? 1 : 0, 0);

						switch (Msg.wParam) {
							case VK_MENU:
								continue;
							case VK_F1:
								if (!kNetGame) {
									bool bOldAppDoFast = bAppDoFast;

									if (!bAppDoFasttoggled)
										bAppDoFast = 0;
									bAppDoFasttoggled = 0;
									if (bOldAppDoFast != bAppDoFast) {
										DisplayFPSInit(); // resync fps display
									}
								}
								break;
						}
					}
				}

				// Check for messages for dialogs etc.
				if (AppMessage(&Msg)) {
					if (TranslateAccelerator(hScrnWnd, hAccel, &Msg) == 0) {
						if (bEditActive) {
							TranslateMessage(&Msg);
						}
						DispatchMessage(&Msg);
					}
				}
			} else {
				// No messages are waiting
				SplashDestroy(0);
				RunIdle();
			}
		}

		RunExit();
		MediaExit(true);
		if (bRestartVideo) {
			MediaInit();
			PausedRedraw();
		}
	} while (bRestartVideo);

	return 0;
}
