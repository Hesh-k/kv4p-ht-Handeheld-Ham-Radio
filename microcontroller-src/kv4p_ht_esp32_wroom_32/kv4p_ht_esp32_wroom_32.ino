#include <DRA818.h>
#include <driver/adc.h>
#include <algorithm>
#include <esp_task_wdt.h>

// Commands defined here must match the Android app
const uint8_t COMMAND_PTT_DOWN = 1; // start transmitting audio that Android app will send
const uint8_t COMMAND_PTT_UP = 2; // stop transmitting audio, go into RX mode
const uint8_t COMMAND_TUNE_TO = 3; // change the frequency
const uint8_t COMMAND_FILTERS = 4; // toggle filters on/off

// Delimeter must also match Android app
#define DELIMITER_LENGTH 8
const uint8_t delimiter[DELIMITER_LENGTH] = {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00};
int matchedDelimiterTokens = 0;

// Mode of the app, which is essentially a state machine
#define MODE_TX 0
#define MODE_RX 1
int mode = MODE_RX;

// Audio sampling rate, must match what Android app expects (and sends).
#define AUDIO_SAMPLE_RATE 8000

// Buffer for sample audio bytes from the radio module
#define RX_AUDIO_BUFFER_SIZE 4000
uint8_t rxAudioBuffer[RX_AUDIO_BUFFER_SIZE]; // Circular buffer
uint8_t* rxBufferHead = &rxAudioBuffer[0];
uint8_t* rxBufferTail = &rxAudioBuffer[0];
#define AUDIO_SEND_THRESHOLD 64 // minimum bytes in buffer before they'll be sent

// Buffer for outgoing audio bytes to send to radio module
#define TX_AUDIO_BUFFER_SIZE 1000 // Holds data we already got off of USB serial from Android app
// TODO change this to a circular buffer too
uint8_t txAudioBuffer1[TX_AUDIO_BUFFER_SIZE]; // Processed tx audio bytes that will be sent to radio module
uint8_t txAudioBuffer2[TX_AUDIO_BUFFER_SIZE]; // 2nd tx audio buffer so we can alternate to avoid any gaps due to processing
bool usingTxBuffer1 = true;
int txAudioBufferIdx1 = 0;
int txAudioBufferLen1 = 0;
int txAudioBufferIdx2 = 0;
int txAudioBufferLen2 = 0;

#define USB_BUFFER_SIZE 1024

// The number of bytes from the end of any buffer when rx or tx audio is processed (to avoid overrun during async processing)
#define BYTES_TO_TRIGGER_FROM_BUFFER_END 20

// Connections to radio module
#define RXD2_PIN 16
#define TXD2_PIN 17
#define DAC_PIN 25
#define ADC_PIN 34 // If this is changed, you may need to manually edit adc1_config_channel_atten() below too.
#define PTT_PIN 18
#define PD_PIN 19
#define SQ_PIN 32

// Built in LED
#define LED_PIN 2

// Object used for radio module serial comms
DRA818* dra = new DRA818(&Serial2, DRA818_VHF);

// Tx runaway detection stuff
long txStartTime = -1;
#define RUNAWAY_TX_SEC 200

void setup() {
  // Communication with Android via USB cable
  Serial.begin(921600);
  Serial.setRxBufferSize(USB_BUFFER_SIZE);
  Serial.setTxBufferSize(USB_BUFFER_SIZE);

  // Configure watch dog timer (WDT), which will reset the system if it gets stuck somehow.
  esp_task_wdt_init(10, true); // Reboot if locked up for a bit
  esp_task_wdt_add(NULL); // Add the current task to WDT watch

  // Debug LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Set up radio module defaults
  pinMode(PD_PIN, OUTPUT);
  digitalWrite(PD_PIN, HIGH); // Power on
  pinMode(SQ_PIN, INPUT);
  pinMode(PTT_PIN, OUTPUT);
  digitalWrite(PTT_PIN, HIGH); // Rx
  pinMode(DAC_PIN, OUTPUT);

  // Communication with DRA818V radio module via GPIO pins
  Serial2.begin(9600, SERIAL_8N1, RXD2_PIN, TXD2_PIN);

  int result = -1;
  while (result != 1) {
    result = dra->handshake(); // Wait for module to start up
  }
  // Serial.println("handshake: " + String(result));
  // tuneTo(146.520, 146.520, 0, 0);
  result = dra->volume(8);
  // Serial.println("volume: " + String(result));
  result = dra->filters(false, false, false);
  // Serial.println("filters: " + String(result));

  // Configure the ADC and DAC resolution to 8 bits
  analogReadResolution(8);
  analogWriteResolution(8);

  // Configure the ADC attenuation (off)
  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_0);

  // Start in RX mode
  setMode(MODE_RX);

  // Start async rx/tx timer
  setupTimer();
}

// Transmit audio is very sensitive to the microcontroller not being very precise with
// when it updates the DAC, so we regulate it based on how much time actually passed.
unsigned long targetTxBufferEndMicros = micros();
unsigned long startedTxMicros = micros();

unsigned long ulmap(unsigned long x, unsigned long in_min, unsigned long in_max, unsigned long out_min, unsigned long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void IRAM_ATTR readWriteAnalog() {
  try {
    // This needs to do as LITTLE processing as possible, or we won't capture
    // enough audio for it to play back properly.
    if (mode) { // MODE_RX
      // TODO consider replacing this with an interpolating approach like is used for MODE_TX below
      *rxBufferTail = (uint8_t) analogRead(ADC_PIN);
      rxBufferTail++;
      if ((rxBufferTail - rxAudioBuffer - RX_AUDIO_BUFFER_SIZE) == 0) {
        rxBufferTail = &rxAudioBuffer[0]; // loop circular buffer
      }
    } else { // MODE_TX
      unsigned long now = micros();
      if (now < targetTxBufferEndMicros) {
        if (usingTxBuffer1) {
          txAudioBufferIdx1 = ulmap(now, startedTxMicros, targetTxBufferEndMicros, 0, txAudioBufferLen1 - 1); // Advance a number of audio bytes relative to time since last DAC update
          dacWrite(DAC_PIN, txAudioBuffer1[txAudioBufferIdx1]);          
        } else {
          txAudioBufferIdx2 = ulmap(now, startedTxMicros, targetTxBufferEndMicros, 0, txAudioBufferLen2 - 1); // Advance a number of audio bytes relative to time since last DAC update
          dacWrite(DAC_PIN, txAudioBuffer2[txAudioBufferIdx2]);   
        }
      }
    }
  } catch (int e) {
    // Disregard, we don't want to crash. Just pick up at next loop of readWriteAnalog().
    // Serial.println("Exception in readWriteAnalog(), skipping cycle.");
  }
}

void setupTimer() {
  // Configure a hardware timer
  hw_timer_t * timer = NULL;
  timer = timerBegin(0, ESP.getCpuFreqMHz(), true); // Use the first timer, prescale by 240 (for 1MHz counting on 240MHz ESP32), count up
  // timer = timerBegin(0, 80, true); // 80Mhz instead of 240Mhz because the GPIO bus (called "APB") is locked to 80Mhz despite CPU speed.

  // Set the timer to call the readWriteAnalog function
  timerAttachInterrupt(timer, &readWriteAnalog, true);
  // int microseconds = (1 / AUDIO_SAMPLE_RATE) * 1000000;
  int microseconds = 41; // Should be 125 (at 8kHz audio) but is divided by 3 due to 80Mhz GPIO bus. Unsure why I can't use 80Mhz divider to do this programatically.
  timerAlarmWrite(timer, microseconds, true);
  timerAlarmEnable(timer);
}

void loop() {
  try {
    if (mode == MODE_RX) {
      if (Serial.available()) {
        // Read a command from Android app
        uint8_t tempBuffer[100]; // Big enough for a command and params, won't hold audio data
        int bytesRead = 0;

        while (bytesRead < (DELIMITER_LENGTH + 1)) { // Read the delimiter and the command byte only (no params yet)
          tempBuffer[bytesRead++] = Serial.read();
          // while (bytesRead < (DELIMITER_LENGTH + 1) && !Serial.available()) { } // If we need more bytes, wait for a byte.
        }
        switch (tempBuffer[DELIMITER_LENGTH]) {
          case COMMAND_PTT_DOWN:
          {
            setMode(MODE_TX);
          }
            break;
          case COMMAND_TUNE_TO:
          {
            // Example:
            // 145.450144.850061
            // 7 chars for tx, 7 chars for rx, 2 chars for tone, 1 char for squelch (17 bytes total for params)
            setMode(MODE_RX);

            // If we haven't received all the parameters needed for COMMAND_TUNE_TO, wait for them before continuing.
            // This can happen if ESP32 has pulled part of the command+params from the buffer before Android has completed
            // putting them in there. If so, we take byte-by-byte until we get the full params.
            int paramBytesMissing = 17;
            String paramsStr = "";
            if (paramBytesMissing > 0) {
              uint8_t paramPartsBuffer[paramBytesMissing];
              for (int j = 0; j < paramBytesMissing; j++) {
                unsigned long waitStart = micros();
                while (!Serial.available()) { 
                  // Wait for a byte.
                  if ((micros() - waitStart) > 500000) { // Give the Android app 0.5 second max before giving up on the command
                    esp_task_wdt_reset();
                    return;
                  }
                }
                paramPartsBuffer[j] = Serial.read();
              }
              paramsStr += String((char *)paramPartsBuffer);
              paramBytesMissing--;
            }
            float freqTxFloat = paramsStr.substring(0, 8).toFloat();
            float freqRxFloat = paramsStr.substring(7, 15).toFloat();
            int toneInt = paramsStr.substring(14, 16).toInt();
            int squelchInt = paramsStr.substring(16, 17).toInt();

            // Serial.println("PARAMS: " + paramsStr.substring(0, 16) + " freqTxFloat: " + String(freqTxFloat) + " freqRxFloat: " + String(freqRxFloat) + " toneInt: " + String(toneInt));

            tuneTo(freqTxFloat, freqRxFloat, toneInt, squelchInt);
          }
            break;
          case COMMAND_FILTERS:
          {
            int paramBytesMissing = 3; // e.g. 000, in order of emphasis, highpass, lowpass
            String paramsStr = "";
            if (paramBytesMissing > 0) {
              uint8_t paramPartsBuffer[paramBytesMissing];
              for (int j = 0; j < paramBytesMissing; j++) {
                unsigned long waitStart = micros();
                while (!Serial.available()) { 
                  // Wait for a byte.
                  if ((micros() - waitStart) > 500000) { // Give the Android app 0.5 second max before giving up on the command
                    esp_task_wdt_reset();
                    return;
                  }
                }
                paramPartsBuffer[j] = Serial.read();
              }
              paramsStr += String((char *)paramPartsBuffer);
              paramBytesMissing--;
            }
            bool emphasis = (paramsStr.charAt(0) == '1');
            bool highpass = (paramsStr.charAt(1) == '1');
            bool lowpass = (paramsStr.charAt(2) == '1');

            dra->filters(emphasis, highpass, lowpass);
          }
            break;
          default:
          {
            // Unexpected.
          }
            break;
        }
      }
      // Don't send every byte as it comes in or it will bog down the processor.
      // Wait for a critical mass of bytes to be ready.
      int bytesToSend = (rxBufferTail < rxBufferHead) ? // has the buffer looped?
          (rxAudioBuffer + RX_AUDIO_BUFFER_SIZE - rxBufferHead) + (rxBufferTail - rxAudioBuffer) : // buffer has looped
          (rxBufferTail - rxBufferHead); // buffer has not looped
      if (bytesToSend < AUDIO_SEND_THRESHOLD) {
        esp_task_wdt_reset();
        return;
      }

      // Make a copy of the audio to send, so the ADC reading doesn't conflict with us reading from the circular
      // buffer. This copy is very short-lived, just what we need to send right now.
      uint8_t rxAudioBufferCopy[bytesToSend];
      if (rxBufferTail < rxBufferHead) { // Tail pointer has looped to start of circular buffer
        memcpy(rxAudioBufferCopy, rxBufferHead, rxAudioBuffer + RX_AUDIO_BUFFER_SIZE - rxBufferHead); // Unsent data up to end of buffer
        memcpy(rxAudioBufferCopy + (rxAudioBuffer + RX_AUDIO_BUFFER_SIZE - rxBufferHead), rxAudioBuffer, rxBufferTail - rxAudioBuffer); // Unsent data at start of looped buffer
      } else { // Buffer has not looped
        memcpy(rxAudioBufferCopy, rxBufferHead, bytesToSend);
      }
      rxBufferHead += bytesToSend;
      if ((rxBufferHead - rxAudioBuffer - RX_AUDIO_BUFFER_SIZE) > 0) {
        rxBufferHead -= RX_AUDIO_BUFFER_SIZE; // Loop head pointer back into the circular buffer (it was out of bounds)
      }

      // If the radio module is indicating that squelch is active, force the audio to quiet (remove background hiss).
      if (digitalRead(SQ_PIN) == HIGH) {
        for (int i = 0; i < bytesToSend; i++) {
          rxAudioBufferCopy[i] = 128; // No sound (half of byte, or 0v)
        }
      }
      Serial.write(rxAudioBufferCopy, bytesToSend);
    } else if (mode == MODE_TX) {
      // Check for runaway tx
      int txSeconds = (micros() - txStartTime) / 1000000;
      if (txSeconds > RUNAWAY_TX_SEC) {
        setMode(MODE_RX);
        esp_task_wdt_reset();
        return;
      }

      // Check for incoming commands or audio from Android
      int bytesRead = 0;
      uint8_t tempBuffer[TX_AUDIO_BUFFER_SIZE];
      int bytesAvailable = Serial.available();
      if (bytesAvailable > 0) {
        bytesRead = Serial.readBytes(tempBuffer, bytesAvailable);

        for (int i = 0; i < bytesRead; i++) {
          // If we've seen the entire delimiter...
          if (matchedDelimiterTokens == DELIMITER_LENGTH) {
            // Process next byte as a command.
            uint8_t command = tempBuffer[i];
            matchedDelimiterTokens = 0;
            switch (command) {
              case COMMAND_PTT_UP: // Only command we can receive in TX mode is PTT_UP
                setMode(MODE_RX);
                break;
            }
          } else {
            if (tempBuffer[i] == delimiter[matchedDelimiterTokens]) { // This byte may be part of the delimiter
              matchedDelimiterTokens++;
            } else { // This byte is not consistent with the command delimiter, reset counter
              matchedDelimiterTokens = 0;
            }
          }
        }
      }
      processTxAudio(tempBuffer, bytesRead);
    }

    // Regularly reset the WDT timer to prevent the device from rebooting (prove we're not locked up).
    esp_task_wdt_reset();
  } catch (int e) {
    // Disregard, we don't want to crash. Just pick up at next loop().)
    // Serial.println("Exception in loop(), skipping cycle.");
  }
}

void tuneTo(float freqTx, float freqRx, int tone, int squelch) {
  int result = dra->group(DRA818_25K, freqTx, freqRx, tone, squelch, 0);
  // Serial.println("tuneTo: " + String(result));
}

void setMode(int newMode) {
  mode = newMode;
  switch (mode) {
    case MODE_RX:
      digitalWrite(LED_PIN, LOW);
      digitalWrite(PTT_PIN, HIGH);
      break;
    case MODE_TX:
      txStartTime = micros();
      digitalWrite(LED_PIN, HIGH);
      digitalWrite(PTT_PIN, LOW);
      usingTxBuffer1 = true;
      txAudioBufferIdx1 = 0;
      txAudioBufferLen1 = 0;
      txAudioBufferIdx2 = 0;
      txAudioBufferLen2 = 0;
      break;
  }
}

void processTxAudio(uint8_t tempBuffer[], int bytesRead) {
  // Add this next chunk of audio to the audio buffer
  if (bytesRead > 0) {
    if (usingTxBuffer1) { // Read into tx buffer 2 while buffer 1 is playing
      memcpy(txAudioBuffer2 + txAudioBufferLen2, tempBuffer, std::min(bytesRead, TX_AUDIO_BUFFER_SIZE - txAudioBufferLen2));
      txAudioBufferLen2 += std::min(bytesRead, TX_AUDIO_BUFFER_SIZE - txAudioBufferLen2);
      if (txAudioBufferLen2 >= TX_AUDIO_BUFFER_SIZE - BYTES_TO_TRIGGER_FROM_BUFFER_END) {
        usingTxBuffer1 = false; // Start playing from buffer 2, it's full.
        startedTxMicros = micros();
        targetTxBufferEndMicros = startedTxMicros + (1000000 * txAudioBufferLen2 / TX_AUDIO_BUFFER_SIZE * TX_AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE); // Transmit should be done by this system time, used to regulate DAC playback rate
        txAudioBufferLen1 = 0;
      }
    } else { // Read into tx buffer 1 while buffer 2 is playing
      memcpy(txAudioBuffer1 + txAudioBufferLen1, tempBuffer, std::min(bytesRead, TX_AUDIO_BUFFER_SIZE - txAudioBufferLen1));
      txAudioBufferLen1 += std::min(bytesRead, TX_AUDIO_BUFFER_SIZE - txAudioBufferLen1);
      if (txAudioBufferLen1 >= TX_AUDIO_BUFFER_SIZE - BYTES_TO_TRIGGER_FROM_BUFFER_END) {
        usingTxBuffer1 = true; // Start playing from buffer 1, it's full.
        startedTxMicros = micros();
        targetTxBufferEndMicros = startedTxMicros + (1000000 * txAudioBufferLen1 / TX_AUDIO_BUFFER_SIZE * TX_AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE); // Transmit should be done by this system time, used to regulate DAC playback rate
        txAudioBufferLen2 = 0;
      }
    }
  }
}
