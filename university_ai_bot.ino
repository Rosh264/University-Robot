/*
 * ============================================================
 *  Garden City University — AI Welcome Bot
 *  ESP32 Arduino Firmware
 * ============================================================
 *
 *  Hardware:
 *    - INMP441 MEMS Microphone (I2S Input)
 *    - MAX98357A I2S Amplifier + Speaker (I2S Output)
 *    - 1.3" SH1106 OLED Display (I2C)
 *    - PIR Motion Sensor
 *    - SG90 Servo Motor
 *
 *  Pin Mapping:
 *    INMP441:    WS=33, SCK=32, SD=35
 *    MAX98357A:  LRC=25, BCLK=26, DIN=22
 *    OLED:       SDA=21, SCL=19
 *    PIR:        GPIO27
 *    Servo:      GPIO18
 *
 *  Flow:
 *    PIR detect → OLED welcome → Servo wave → Speaker greet →
 *    Mic record → WiFi POST → AI process → OLED + Speaker response
 *
 *  Board: ESP32 Dev Module
 *  Partition Scheme: Default 4MB with spiffs
 *
 *  Required Libraries (install via Arduino Library Manager):
 *    - U8g2 by oliver
 *    - ESP32Servo by Kevin Harrington
 *    - ArduinoJson by Benoit Blanchon
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include <base64.h>
#include "SPIFFS.h"

// ============================================================
//  CONFIGURATION — EDIT THESE VALUES
// ============================================================

// WiFi credentials
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Backend server URL (your computer's IP running server.py)
const char* SERVER_URL = "http://192.168.1.100:5000/api/query";
const char* STATUS_URL = "http://192.168.1.100:5000/api/status";

// Recording settings
#define RECORD_DURATION_SEC   5       // seconds to record
#define SAMPLE_RATE           16000   // 16kHz for speech
#define BITS_PER_SAMPLE       16
#define CHANNELS              1       // mono

// ============================================================
//  PIN DEFINITIONS (matching your schematic)
// ============================================================

// INMP441 Microphone (I2S Port 0 — Input)
#define I2S_MIC_PORT          I2S_NUM_0
#define I2S_MIC_WS            33
#define I2S_MIC_SCK           32
#define I2S_MIC_SD            35

// MAX98357A Amplifier (I2S Port 1 — Output)
#define I2S_SPK_PORT          I2S_NUM_1
#define I2S_SPK_LRC           25
#define I2S_SPK_BCLK          26
#define I2S_SPK_DIN           22

// OLED Display (I2C with custom pins)
#define OLED_SDA              21
#define OLED_SCL              19

// PIR Motion Sensor
#define PIR_PIN               27

// Servo Motor
#define SERVO_PIN             18

// ============================================================
//  GLOBAL OBJECTS
// ============================================================

// OLED: 1.3" SH1106 128x64, Software I2C with custom pins
U8G2_SH1106_128X64_NONAME_F_SW_I2C u8g2(
  U8G2_R0,          // rotation
  OLED_SCL,          // clock
  OLED_SDA,          // data
  U8X8_PIN_NONE      // reset (none)
);

// Servo motor
Servo greetingServo;

// Audio buffer for recording
// 16kHz × 16-bit × 5 sec = 160,000 bytes
#define AUDIO_BUFFER_SIZE (SAMPLE_RATE * (BITS_PER_SAMPLE / 8) * RECORD_DURATION_SEC)
uint8_t* audioBuffer = NULL;
size_t   audioBufferLen = 0;

// WAV header for the recorded audio
#define WAV_HEADER_SIZE 44

// Bot state machine
enum BotState {
  STATE_IDLE,
  STATE_WELCOME,
  STATE_GREETING,
  STATE_LISTENING,
  STATE_PROCESSING,
  STATE_RESPONDING,
  STATE_FOLLOWUP,
  STATE_GOODBYE
};

BotState currentState = STATE_IDLE;

// Timing
unsigned long lastMotionTime = 0;
unsigned long stateStartTime = 0;
const unsigned long COOLDOWN_MS = 10000;  // 10 sec cooldown between visitors

// Response storage
String responseText = "";
String responseAudioB64 = "";

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println(" Garden City University — AI Welcome Bot");
  Serial.println("========================================\n");

  // --- Initialize SPIFFS ---
  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS mount failed!");
  } else {
    Serial.println("[OK] SPIFFS mounted");
  }

  // --- Initialize OLED ---
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);
  displayStartup();
  Serial.println("[OK] OLED initialized");

  // --- Initialize PIR ---
  pinMode(PIR_PIN, INPUT);
  Serial.println("[OK] PIR sensor on GPIO" + String(PIR_PIN));

  // --- Initialize Servo ---
  ESP32PWM::allocateTimer(0);
  greetingServo.setPeriodHertz(50);
  greetingServo.attach(SERVO_PIN, 500, 2400);
  greetingServo.write(90);  // center position
  Serial.println("[OK] Servo on GPIO" + String(SERVO_PIN));

  // --- Connect WiFi ---
  displayMessage("Connecting WiFi...", WIFI_SSID);
  connectWiFi();

  // --- Initialize I2S Microphone ---
  initI2SMic();
  Serial.println("[OK] I2S Microphone initialized");

  // --- Initialize I2S Speaker ---
  initI2SSpeaker();
  Serial.println("[OK] I2S Speaker initialized");

  // --- Allocate Audio Buffer ---
  audioBuffer = (uint8_t*)ps_malloc(AUDIO_BUFFER_SIZE + WAV_HEADER_SIZE);
  if (audioBuffer == NULL) {
    // Fallback to regular malloc if no PSRAM
    audioBuffer = (uint8_t*)malloc(AUDIO_BUFFER_SIZE + WAV_HEADER_SIZE);
  }
  if (audioBuffer == NULL) {
    Serial.println("[ERROR] Failed to allocate audio buffer!");
    displayMessage("ERROR:", "No memory for audio!");
    while (1) delay(1000);
  }
  Serial.printf("[OK] Audio buffer: %d bytes\n", AUDIO_BUFFER_SIZE + WAV_HEADER_SIZE);

  // --- Ready ---
  displayIdle();
  Serial.println("\n[READY] Bot is waiting for visitors...\n");

  // Send startup status to server
  sendStatusPing();
}

// ============================================================
//  MAIN LOOP — State Machine
// ============================================================

void loop() {
  switch (currentState) {

    case STATE_IDLE:
      // Poll PIR sensor
      if (digitalRead(PIR_PIN) == HIGH) {
        unsigned long now = millis();
        if (now - lastMotionTime > COOLDOWN_MS) {
          lastMotionTime = now;
          Serial.println("\n[PIR] Motion detected!");
          currentState = STATE_WELCOME;
          stateStartTime = millis();
        }
      }
      break;

    case STATE_WELCOME:
      Serial.println("[STATE] Welcome");
      displayWelcome();
      delay(2000);
      currentState = STATE_GREETING;
      break;

    case STATE_GREETING:
      Serial.println("[STATE] Greeting — Servo wave + Speaker");
      servoWave();
      playGreeting();
      currentState = STATE_LISTENING;
      break;

    case STATE_LISTENING:
      Serial.println("[STATE] Listening — Recording audio...");
      displayListening();
      recordAudio();
      currentState = STATE_PROCESSING;
      break;

    case STATE_PROCESSING:
      Serial.println("[STATE] Processing — Sending to AI...");
      displayProcessing();
      if (sendAudioToServer()) {
        currentState = STATE_RESPONDING;
      } else {
        displayMessage("Connection Error", "Please try again");
        delay(3000);
        currentState = STATE_GOODBYE;
      }
      break;

    case STATE_RESPONDING:
      Serial.println("[STATE] Responding");
      displayResponse(responseText);
      playResponseAudio();
      currentState = STATE_FOLLOWUP;
      stateStartTime = millis();
      break;

    case STATE_FOLLOWUP:
      // Listen briefly for follow-up question
      Serial.println("[STATE] Follow-up check...");
      displayMessage("Any other", "questions?");
      delay(2000);

      // Record a short clip to check for speech
      if (detectSpeech()) {
        Serial.println("[FOLLOWUP] Speech detected, recording...");
        displayListening();
        recordAudio();
        currentState = STATE_PROCESSING;
      } else {
        Serial.println("[FOLLOWUP] No speech, saying goodbye");
        currentState = STATE_GOODBYE;
      }
      break;

    case STATE_GOODBYE:
      Serial.println("[STATE] Goodbye");
      displayGoodbye();
      servoWave();
      delay(3000);
      displayIdle();
      currentState = STATE_IDLE;
      break;
  }

  delay(10);
}

// ============================================================
//  WiFi CONNECTION
// ============================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WiFi] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    displayMessage("WiFi Connected!", WiFi.localIP().toString().c_str());
    delay(1500);
  } else {
    Serial.println("\n[WiFi] FAILED to connect!");
    displayMessage("WiFi FAILED!", "Check credentials");
    delay(3000);
  }
}

// ============================================================
//  I2S MICROPHONE (INMP441) — Input on Port 0
// ============================================================

void initI2SMic() {
  i2s_config_t i2s_mic_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 1024,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  i2s_pin_config_t i2s_mic_pins = {
    .bck_io_num   = I2S_MIC_SCK,
    .ws_io_num    = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_MIC_SD
  };

  i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins);
  i2s_zero_dma_buffer(I2S_MIC_PORT);
}

// ============================================================
//  I2S SPEAKER (MAX98357A) — Output on Port 1
// ============================================================

void initI2SSpeaker() {
  i2s_config_t i2s_spk_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 1024,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };

  i2s_pin_config_t i2s_spk_pins = {
    .bck_io_num   = I2S_SPK_BCLK,
    .ws_io_num    = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPK_PORT, &i2s_spk_config, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &i2s_spk_pins);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

// ============================================================
//  AUDIO RECORDING
// ============================================================

void recordAudio() {
  Serial.printf("[MIC] Recording %d seconds...\n", RECORD_DURATION_SEC);

  // Leave space for WAV header at the beginning
  uint8_t* audioData = audioBuffer + WAV_HEADER_SIZE;
  size_t totalBytesRead = 0;
  size_t bytesRead = 0;

  // Flush any old data in the I2S buffer
  uint8_t flushBuf[1024];
  for (int i = 0; i < 10; i++) {
    i2s_read(I2S_MIC_PORT, flushBuf, sizeof(flushBuf), &bytesRead, 10);
  }

  // Record audio samples
  unsigned long startTime = millis();
  while (totalBytesRead < AUDIO_BUFFER_SIZE) {
    size_t remaining = AUDIO_BUFFER_SIZE - totalBytesRead;
    size_t toRead = (remaining > 1024) ? 1024 : remaining;

    i2s_read(I2S_MIC_PORT, audioData + totalBytesRead, toRead, &bytesRead, portMAX_DELAY);
    totalBytesRead += bytesRead;

    // Show recording progress on OLED
    if (millis() - startTime > 500) {
      int elapsed = (millis() - startTime) / 1000;
      displayRecordingProgress(elapsed, RECORD_DURATION_SEC);
    }
  }

  audioBufferLen = totalBytesRead;
  Serial.printf("[MIC] Recorded %d bytes\n", totalBytesRead);

  // Write WAV header at the start of the buffer
  writeWavHeader(audioBuffer, totalBytesRead);
}

void writeWavHeader(uint8_t* buffer, size_t dataSize) {
  uint32_t fileSize = dataSize + WAV_HEADER_SIZE - 8;
  uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);

  // RIFF chunk
  buffer[0]  = 'R'; buffer[1]  = 'I'; buffer[2]  = 'F'; buffer[3]  = 'F';
  buffer[4]  = fileSize & 0xFF;
  buffer[5]  = (fileSize >> 8) & 0xFF;
  buffer[6]  = (fileSize >> 16) & 0xFF;
  buffer[7]  = (fileSize >> 24) & 0xFF;
  buffer[8]  = 'W'; buffer[9]  = 'A'; buffer[10] = 'V'; buffer[11] = 'E';

  // fmt sub-chunk
  buffer[12] = 'f'; buffer[13] = 'm'; buffer[14] = 't'; buffer[15] = ' ';
  buffer[16] = 16;  buffer[17] = 0;   buffer[18] = 0;   buffer[19] = 0;   // Sub-chunk size (16 for PCM)
  buffer[20] = 1;   buffer[21] = 0;                                         // Audio format (1 = PCM)
  buffer[22] = CHANNELS; buffer[23] = 0;                                    // Num channels
  buffer[24] = SAMPLE_RATE & 0xFF;
  buffer[25] = (SAMPLE_RATE >> 8) & 0xFF;
  buffer[26] = (SAMPLE_RATE >> 16) & 0xFF;
  buffer[27] = (SAMPLE_RATE >> 24) & 0xFF;
  buffer[28] = byteRate & 0xFF;
  buffer[29] = (byteRate >> 8) & 0xFF;
  buffer[30] = (byteRate >> 16) & 0xFF;
  buffer[31] = (byteRate >> 24) & 0xFF;
  buffer[32] = blockAlign; buffer[33] = 0;
  buffer[34] = BITS_PER_SAMPLE; buffer[35] = 0;

  // data sub-chunk
  buffer[36] = 'd'; buffer[37] = 'a'; buffer[38] = 't'; buffer[39] = 'a';
  buffer[40] = dataSize & 0xFF;
  buffer[41] = (dataSize >> 8) & 0xFF;
  buffer[42] = (dataSize >> 16) & 0xFF;
  buffer[43] = (dataSize >> 24) & 0xFF;
}

// ============================================================
//  SPEECH DETECTION (simple energy-based)
// ============================================================

bool detectSpeech() {
  Serial.println("[MIC] Checking for speech...");
  uint8_t tempBuf[2048];
  size_t bytesRead = 0;
  int32_t energy = 0;

  // Read 1 second of audio and calculate energy
  for (int i = 0; i < 16; i++) {
    i2s_read(I2S_MIC_PORT, tempBuf, sizeof(tempBuf), &bytesRead, 100);

    // Calculate RMS energy
    int16_t* samples = (int16_t*)tempBuf;
    int numSamples = bytesRead / 2;
    for (int s = 0; s < numSamples; s++) {
      energy += abs(samples[s]);
    }
  }

  int avgEnergy = energy / (16 * 1024);
  Serial.printf("[MIC] Average energy: %d\n", avgEnergy);

  // Threshold — adjust based on your environment
  return avgEnergy > 500;
}

// ============================================================
//  AUDIO PLAYBACK
// ============================================================

void playGreeting() {
  Serial.println("[SPK] Playing greeting...");
  displayMessage("Speaking:", "Hello! How can");
  delay(200);
  displayMessage("Speaking:", "I help you?");

  // Check if pre-recorded greeting exists in SPIFFS
  if (SPIFFS.exists("/greeting.wav")) {
    File greetFile = SPIFFS.open("/greeting.wav", "r");
    if (greetFile) {
      // Skip WAV header
      greetFile.seek(WAV_HEADER_SIZE);

      uint8_t playBuf[1024];
      size_t bytesWritten;
      while (greetFile.available()) {
        size_t bytesRead = greetFile.read(playBuf, sizeof(playBuf));
        i2s_write(I2S_SPK_PORT, playBuf, bytesRead, &bytesWritten, portMAX_DELAY);
      }
      greetFile.close();
      Serial.println("[SPK] Greeting played from SPIFFS");
      return;
    }
  }

  // If no file, generate a simple beep tone as placeholder
  Serial.println("[SPK] No greeting file — playing tone");
  playTone(800, 300);
  delay(100);
  playTone(1000, 300);
  delay(100);
  playTone(1200, 500);
  delay(500);
}

void playResponseAudio() {
  if (responseAudioB64.length() == 0) {
    Serial.println("[SPK] No audio response to play");
    return;
  }

  Serial.println("[SPK] Decoding and playing response audio...");

  // Decode base64 audio
  int decodedLen = base64_decode_expected_len(responseAudioB64.length());
  uint8_t* decodedAudio = (uint8_t*)malloc(decodedLen);

  if (decodedAudio == NULL) {
    Serial.println("[SPK] Failed to allocate decode buffer");
    return;
  }

  int actualLen = base64_decode_chars(
    responseAudioB64.c_str(),
    responseAudioB64.length(),
    (char*)decodedAudio
  );

  Serial.printf("[SPK] Decoded %d bytes of audio\n", actualLen);

  // Skip WAV header if present, then play PCM data
  int offset = 0;
  if (actualLen > WAV_HEADER_SIZE &&
      decodedAudio[0] == 'R' && decodedAudio[1] == 'I' &&
      decodedAudio[2] == 'F' && decodedAudio[3] == 'F') {
    offset = WAV_HEADER_SIZE;
  }

  // Play audio through I2S speaker
  size_t bytesWritten;
  int remaining = actualLen - offset;
  int pos = offset;

  while (remaining > 0) {
    int chunk = (remaining > 1024) ? 1024 : remaining;
    i2s_write(I2S_SPK_PORT, decodedAudio + pos, chunk, &bytesWritten, portMAX_DELAY);
    pos += chunk;
    remaining -= chunk;
  }

  free(decodedAudio);
  Serial.println("[SPK] Response audio playback complete");
}

void playTone(int frequency, int durationMs) {
  int numSamples = (SAMPLE_RATE * durationMs) / 1000;
  int16_t* toneBuf = (int16_t*)malloc(numSamples * sizeof(int16_t));

  if (toneBuf == NULL) return;

  for (int i = 0; i < numSamples; i++) {
    float t = (float)i / SAMPLE_RATE;
    toneBuf[i] = (int16_t)(8000 * sin(2.0 * PI * frequency * t));
  }

  size_t bytesWritten;
  i2s_write(I2S_SPK_PORT, toneBuf, numSamples * sizeof(int16_t), &bytesWritten, portMAX_DELAY);

  free(toneBuf);
}

// ============================================================
//  HTTP COMMUNICATION
// ============================================================

bool sendAudioToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected!");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return false;
  }

  Serial.println("[HTTP] Sending audio to server...");

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/octet-stream");
  http.setTimeout(30000);  // 30 second timeout for AI processing

  size_t totalSize = audioBufferLen + WAV_HEADER_SIZE;
  int httpCode = http.POST(audioBuffer, totalSize);

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("[HTTP] Response received");

    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      responseText = doc["text"].as<String>();
      responseAudioB64 = doc["audio"].as<String>();
      Serial.printf("[HTTP] Text: %s\n", responseText.c_str());
      Serial.printf("[HTTP] Audio size: %d chars (base64)\n", responseAudioB64.length());
      http.end();
      return true;
    } else {
      Serial.printf("[HTTP] JSON parse error: %s\n", error.c_str());
    }
  } else {
    Serial.printf("[HTTP] POST failed, code: %d\n", httpCode);
    Serial.println("[HTTP] Error: " + http.errorToString(httpCode));
  }

  http.end();
  return false;
}

void sendStatusPing() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(STATUS_URL);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["device"] = "esp32_welcome_bot";
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["free_heap"] = ESP.getFreeHeap();

  String json;
  serializeJson(doc, json);
  http.POST(json);
  http.end();
}

// ============================================================
//  SERVO GREETING ANIMATION
// ============================================================

void servoWave() {
  Serial.println("[SERVO] Waving...");

  // Wave 3 times
  for (int wave = 0; wave < 3; wave++) {
    for (int pos = 90; pos >= 30; pos -= 3) {
      greetingServo.write(pos);
      delay(15);
    }
    for (int pos = 30; pos <= 150; pos += 3) {
      greetingServo.write(pos);
      delay(15);
    }
    for (int pos = 150; pos >= 90; pos -= 3) {
      greetingServo.write(pos);
      delay(15);
    }
  }

  greetingServo.write(90);  // return to center
  Serial.println("[SERVO] Wave complete");
}

// ============================================================
//  OLED DISPLAY FUNCTIONS
// ============================================================

void displayStartup() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(10, 15, "Garden City");
  u8g2.drawStr(10, 28, "University");
  u8g2.drawHLine(10, 32, 108);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(10, 46, "AI Welcome Bot");
  u8g2.drawStr(10, 58, "Initializing...");
  u8g2.sendBuffer();
}

void displayIdle() {
  u8g2.clearBuffer();

  // University name
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(8, 12, "Garden City");
  u8g2.drawStr(8, 24, "University");

  // Divider
  u8g2.drawHLine(8, 28, 112);

  // Status
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(8, 42, "Status: Ready");

  // WiFi indicator
  if (WiFi.status() == WL_CONNECTED) {
    u8g2.drawStr(8, 56, "WiFi: Connected");
  } else {
    u8g2.drawStr(8, 56, "WiFi: Disconnected");
  }

  // Border
  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.sendBuffer();
}

void displayWelcome() {
  u8g2.clearBuffer();

  // Decorative border
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawFrame(2, 2, 124, 60);

  // Welcome text
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(14, 18, "Welcome to");

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(14, 33, "Garden City");
  u8g2.drawStr(14, 46, "University");

  // Star decorations
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(40, 60, "* * * * *");

  u8g2.sendBuffer();
}

void displayListening() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(16, 20, "Listening...");

  // Microphone icon (simple)
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(28, 40, "[ Recording ]");
  u8g2.drawStr(22, 55, "Speak your query");

  u8g2.sendBuffer();
}

void displayRecordingProgress(int elapsed, int total) {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(20, 16, "Recording...");

  // Progress bar
  int barWidth = 100;
  int barHeight = 12;
  int barX = 14;
  int barY = 26;
  int fillWidth = (elapsed * barWidth) / total;

  u8g2.drawFrame(barX, barY, barWidth, barHeight);
  u8g2.drawBox(barX + 1, barY + 1, fillWidth - 2, barHeight - 2);

  // Time display
  char timeStr[20];
  snprintf(timeStr, sizeof(timeStr), "%d / %d sec", elapsed, total);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(35, 52, timeStr);

  u8g2.sendBuffer();
}

void displayProcessing() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(16, 20, "Processing...");

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(10, 38, "Analyzing your");
  u8g2.drawStr(10, 50, "question with AI");

  // Animated dots effect (static here, but shown)
  u8g2.drawStr(48, 62, ". . .");

  u8g2.sendBuffer();
}

void displayResponse(String text) {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(8, 12, "Response:");
  u8g2.drawHLine(8, 15, 112);

  // Word-wrap the response text
  u8g2.setFont(u8g2_font_6x10_tf);

  int maxCharsPerLine = 20;
  int y = 28;
  int lineCount = 0;
  int maxLines = 4;

  String remaining = text;
  while (remaining.length() > 0 && lineCount < maxLines) {
    String line;
    if ((int)remaining.length() <= maxCharsPerLine) {
      line = remaining;
      remaining = "";
    } else {
      // Find last space before limit
      int breakPoint = maxCharsPerLine;
      while (breakPoint > 0 && remaining.charAt(breakPoint) != ' ') {
        breakPoint--;
      }
      if (breakPoint == 0) breakPoint = maxCharsPerLine;
      line = remaining.substring(0, breakPoint);
      remaining = remaining.substring(breakPoint);
      remaining.trim();
    }

    u8g2.drawStr(6, y, line.c_str());
    y += 11;
    lineCount++;
  }

  if (remaining.length() > 0) {
    u8g2.drawStr(90, y - 11, "...");
  }

  u8g2.sendBuffer();
  delay(3000);

  // If text is long, show remaining in a second screen
  if (remaining.length() > 0) {
    u8g2.clearBuffer();
    u8g2.drawFrame(0, 0, 128, 64);
    u8g2.setFont(u8g2_font_6x10_tf);

    y = 14;
    lineCount = 0;
    while (remaining.length() > 0 && lineCount < 5) {
      String line;
      if ((int)remaining.length() <= maxCharsPerLine) {
        line = remaining;
        remaining = "";
      } else {
        int breakPoint = maxCharsPerLine;
        while (breakPoint > 0 && remaining.charAt(breakPoint) != ' ') {
          breakPoint--;
        }
        if (breakPoint == 0) breakPoint = maxCharsPerLine;
        line = remaining.substring(0, breakPoint);
        remaining = remaining.substring(breakPoint);
        remaining.trim();
      }
      u8g2.drawStr(6, y, line.c_str());
      y += 11;
      lineCount++;
    }
    u8g2.sendBuffer();
    delay(3000);
  }
}

void displayGoodbye() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawFrame(2, 2, 124, 60);

  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(20, 22, "Thank You!");

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(18, 40, "Visit us again");

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(14, 56, "Garden City Univ.");

  u8g2.sendBuffer();
}

void displayMessage(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(8, 24, line1);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(8, 44, line2);

  u8g2.sendBuffer();
}
