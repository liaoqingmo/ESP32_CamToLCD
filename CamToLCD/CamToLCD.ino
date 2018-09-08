
// Pete Wentworth cspwcspw@gmail.com.  Sept 2018

// Get my OV7670 camera to deliver frames to my ILI9341 TFT LCD.
// Big chunks of the camera-side code were lifted from or inspired by
// the ESP_I2S_Camera project by Ivan Grokhotkov (igrr)
// The LCD side of things started from some driver code on the Banggood site.

// Uncomment this define to get some debugging, but perhaps slow down XCLK a bit first ...
//#define DebuggingPort

#include "Log.h"

#ifdef DebuggingPort  // if we are going to use debugging, create a debug sink.
DebugPort debug(&Serial, false);
#endif

#include "LCD.h"
LCD *theLCD;

#include "CAM_OV7670.h"
pw_OV7670 *theCam;

int missedBlocks = 0, blockCount = 0, framesGrabbed = 0;

// The control logic is a state machine:
enum State {
  Waiting, // We don't know where we are, and need a VSYNC to synchronize ourselves.
  Opening, // As soon as we hit this state we'll prepare the sink: e.g. send a frame header, open a file, set up LCD etc.
  Running, // Queueing blocks as they arrive in the interrupt handlier, and sinking the data in the main loop.
  Closing, // We got a VSYNC. We can close the frame / close a file, restart the I2S engine, print stats, etc.
  Overrun  // If either VSYNC or a scanline interrupts before we're finalized, we've lost the beat.
};
char *stateNames[] = {"Waiting", "Opening", "Running", "Closing", "Overrun"};
State theState;

//******* The camera. SIOD and SIOC pins need pull-ups to Vcc(3.3v) (4.7k ohms seems OK) **********

// The "System" is a combination of the interrupt
// callbacks that produces blocks, and the sink mechanism that consumes the blocks.
// Here a block is always a full scanline. But the DMA buffers have limited capacity,
// and the base i2sCamera code anticipates future cameras where a scanline might need
// more than one DMA block.

DMABuffer *queuedBlock = 0;
// This listener gets a callback after every scan line.
void IRAM_ATTR sinkOneScanline(DMABuffer *buf)
{

#ifdef DebuggingPort
  blockCount++;
#endif

  switch (theState) {

    case Waiting:  // We just stay Waiting.  Only a VSYNC can rescue us.
      break;

    case Opening:
#ifdef DebuggingPort
      if (queuedBlock != 0) { // problem, we've not fully dealt with the last buffer yet.
        debug.print('f');
      }
#endif
      queuedBlock = buf; // queue the first scanline for handling
      break;

    case Running:
#ifdef DebuggingPort
      if (queuedBlock != 0) { // problem, cos we've not dealt with the last buffer yet.
        debug.print('F');
      }
#endif
      queuedBlock = buf;
      break;

    case Closing:
      missedBlocks++;
      theState = Overrun;
      break;

    case Overrun:
      missedBlocks++;
      break;
  }
}

// If registered, this listener gets a callback after every VSYNC.
void IRAM_ATTR handleVSYNC()
{
#ifdef DebuggingPort
  if (blockCount == theCam->yres) {
    // debug_print("f");
  }
  else {
    debug_printf("{%d} ", blockCount);  // We did not get the expected numer of scanlines.
  }

  blockCount = 0;
#endif


  switch (theState) {
    case Waiting:
      theState = Opening;  // The main loop can start preparing to send the next frame.
      break;

    case Opening:
      debug_print(".");
      // Serial.println("Unexpected VSYNC when we are in state Opening");
      theState = Waiting;
      break;

    case Running:
      theState = Closing;  // Tell the main loop we're at the end of a frame.
      break;

    case Closing:
      debug_print("M");
      theState = Overrun;
      break;

    case Overrun:
      debug_print("m");
      break;
  }
}

void reclaim_JTAG_pins()
{ // https://www.esp32.com/viewtopic.php?t=2687
  // At reset, these pins (12,13,14,15) are configured for JTAG function.
  // You need to change function back to GPIO in the IO MUX to make the pins work as GPIOs.
  // If you use GPIO driver (include "driver/gpio.h", not "rom/gpio.h"),
  // it will configure the pin as GPIO for you, once you call gpio_config to configure the pin.
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[12], PIN_FUNC_GPIO);
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[13], PIN_FUNC_GPIO);
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[14], PIN_FUNC_GPIO);
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[15], PIN_FUNC_GPIO);
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\nPete's CamToLCD, V1.0 (%s %s)\n%s\n", __DATE__, __TIME__, __FILE__);

  reclaim_JTAG_pins();
  debug_enableOutput(false);

  theLCD = new LCD();
  theLCD->Setup();
  theLCD->TestSuite();

  //   Camera pin names  D9  D8  D7  D6  D5  D4  D3  D2
  int cam_databus[] = {  13, 35, 12, 32, 14, 33, 27, 25}; // lsb on the right
  int VP = 36;
  int VN = 39;
  //                   SIOD, SIOC, VSYNC, HREF, XCLK, PCLK ...
  theCam = new pw_OV7670(05,   00,    VN,   34,   26,   VP, cam_databus, sinkOneScanline, handleVSYNC);
  theCam->TestSuite("Setup");
  theCam->setMode(2, true);
}

// This is for interactively tweaking camera register settings,
// providing a kind of camera playground.
// None of this is central to the main ideas of the project.
int saturation = 1;
int frameStartX = 164;
int frameStartY =   8;
bool autoDeNoise = false;
bool softSleep = false;
int driveStrength = 1;
int testPattern = 0;

void handleUserInput()
{

  int ch = Serial.read();
  if (ch < 0) return;

  switch (ch) {

    case 'm' :
      { theCam->softSleep(false);  // wake up camera before changing modes
        int nextMode = (theCam->mode + 1) % 3;
        theCam->setMode(nextMode, true);
        theLCD->ClearScreen(YELLOW);
      }
      break;

    case 'f':
      frameStartX--;
      theCam->frameControl(frameStartX, frameStartY);
      Serial.printf("frameStart  %d, %d \n", frameStartX, frameStartY);
      break;

    case 'F':
      frameStartX++;
      theCam->frameControl(frameStartX, frameStartY);
      Serial.printf("frameStart  %d, %d \n", frameStartX, frameStartY);
      break;

    case 'v':
      frameStartY--;
      theCam->frameControl(frameStartX, frameStartY);
      Serial.printf("frameStart  %d, %d \n", frameStartX, frameStartY);
      break;

    case 'V':
      frameStartY++;
      theCam->frameControl(frameStartX, frameStartY);
      Serial.printf("frameStart  %d, %d\n", frameStartX, frameStartY);
      break;

    case 's':
      saturation--;
      theCam->saturation(saturation);
      Serial.printf("Saturation %d\n", saturation);
      break;

    case 'S':
      saturation++;
      theCam->saturation(saturation);
      Serial.printf("Saturation  %d\n", saturation);
      break;

    case 'd':
      autoDeNoise = !autoDeNoise;
      theCam->autoDeNoise(autoDeNoise);
      Serial.printf("Auto de-noise %d\n", autoDeNoise);
      break;

    case 'z':   // This is nice to freeze a frame.
      softSleep = !softSleep;
      theCam->softSleep(softSleep);
      Serial.printf("Soft sleep %d\n", softSleep);
      break;

    case 'q':
      driveStrength = (driveStrength + 1) % 4;
      theCam->setDriveStrength(driveStrength);
      Serial.printf("Drive strength %d\n", driveStrength);
      break;

    case 't':
      testPattern = (testPattern + 1) % 4;
      theCam->testPattern(testPattern);
      Serial.printf("Test pattern %d\n", testPattern);
      break;
  }
}


long lastFpsTime = 0;
int fpsReportAfterFrames = 100;
int etHotspot;  // Elapsed microsecs spent in the last call to the hotspot code

int regTry = 0;
int testKind = 0;  // what kind of testimage do we want?


void loop(void)
{

  // I normally only poll for user input at the end of a frame, but that fails if the camera is asleep...
  if (softSleep)
  {
    handleUserInput();
  }

  switch (theState)
  {
    case Waiting: break; // Just be patient and wait to synchronize with the start of the next frame.

    case Opening:        // Set up to sink the next frame.
      {
        theCam->i2sRestart();
        // Tell the LCD where to put the image on the screen.  Center it.
        int x = (320 - theCam->xres) / 2; // this will need fixing when I buy a bigger screen.
        int y = (240 - theCam->yres) / 2;
        theLCD->Address_set(x, y, x + theCam->xres - 1 - 0, y + theCam->yres - 1);

        theState = Running;
      }
      break;

    case Running:         // If there is a queued block, send it to the LCD
      {
        DMABuffer *buf = queuedBlock;
        if (buf) {
          long t0 = micros();    // collect some diagnostic timing information
          theLCD->SinkDMABuf(theCam->xres, buf);
          etHotspot = micros() - t0;
          queuedBlock = 0;      // Indicate that we're done with this buffer
        }
      }
      break;

    case Closing:  // Clear any queued block and finalize things before allowing the next frame to begin
    case Overrun:  // Time after a VSYNC and before the next scanline is our biggest chunk of idle time,
      { // so we do a bit of other housekeeping here too, like polling for user input.

        DMABuffer *buf = queuedBlock;
        if (buf) {
          // send the last scan row to the display if there is one
          theLCD->SinkDMABuf(theCam->xres, buf);
          queuedBlock = 0;        // Indicate that we're done with this buffer
        }

        // Housekeeping
        if (++framesGrabbed % fpsReportAfterFrames == 0) {
          long timeNow = millis();
          double fps = (fpsReportAfterFrames * 1000.0) / (timeNow - lastFpsTime);
          Serial.printf("Mode:%dx%d  Last %d frames = %.1f FPS.  Hotspot = %d microsecs\n",
                        theCam->xres, theCam->yres, fpsReportAfterFrames, fps, etHotspot);

          lastFpsTime = timeNow;
        }
        handleUserInput();
        // If we got this all done before the next scanline arrived ...
        if (theState != Overrun) {  // Yay!  We got finalized before the deadline.
          theState = Opening;       // Just go back and expect scanlines to appear for the next frame.
        }
        else {                      // Oops, we'll have to miss a frame and wait for the next vsync
          theState = Waiting;
        }
      }
      break;
  }
}
