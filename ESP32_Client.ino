#include "esp_camera.h"                      // Camera library
#include "esp_vfs_fat.h"                     // SD card library
#include <EEPROM.h>                          // Used to store the file number.
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "SD_MMC.h"
#include <SPI.h>
#include "FS.h"
#include <WiFi.h>                            // Used to connect to WiFi to get the time via NTP.
#include <ESP32Time.h>                       // Used to set the internal RTC, so the timestamp of AVI files is correct.

#define PWDN_GPIO_NUM     32                 // Pins for CAMERA_MODEL_AI_THINKER
#define RESET_GPIO_NUM    -1                 //
#define XCLK_GPIO_NUM      0                 //
#define SIOD_GPIO_NUM     26                 //
#define SIOC_GPIO_NUM     27                 //
#define Y9_GPIO_NUM       35                 //
#define Y8_GPIO_NUM       34                 //
#define Y7_GPIO_NUM       39                 //
#define Y6_GPIO_NUM       36                 //
#define Y5_GPIO_NUM       21                 //
#define Y4_GPIO_NUM       19                 //
#define Y3_GPIO_NUM       18                 //
#define Y2_GPIO_NUM        5                 //
#define VSYNC_GPIO_NUM    25                 //
#define HREF_GPIO_NUM     23                 //
#define PCLK_GPIO_NUM     22                 //

#define EEPROM_SIZE        3                 // Size of EEPROM used (holds last used file number, max 65,535).

const uint16_t      AVI_HEADER_SIZE = 252;   // Size of the AVI file header.
const int           SENSOR_PIN      = 13;     // GPIO3 connected to the PIR sensor.  This is also used as U0RXD but isn't required when the program is running. 
const long unsigned SENSOR_INTERVAL = 500;   // Time (ms) between motion sensor checks 
const long unsigned MOTION_DELAY    = 15000; // Time (ms) after motion last detected that we keep recording

const long unsigned FRAME_INTERVAL  = 100;   // Time (ms) between frame captures 
const uint8_t       JPEG_QUALITY    = 10;    // JPEG quality (0-63).
const uint8_t       MAX_FRAMES      = 15;    // Maximum number of frames we hold at any time

const long unsigned WIFI_TIMEOUT    = 20000; // Try to connect to WiFi for this long, then give up.

const long unsigned DETECTION_WAIT = 180000; // Time after last video, where it will be connected to last detection
const long unsigned COOLDOWN = 420000; // Time after last detection where no movement will be reported
uint16_t detection_id = 0;
int camera_id = -1;
long unsigned last_detection = -1;
int last_detection_id = -1;
String last_filename = "";

const byte buffer00dc   [4]  = {0x30, 0x30, 0x64, 0x63}; // "00dc"
const byte buffer0000   [4]  = {0x00, 0x00, 0x00, 0x00}; // 0x00000000
const byte bufferAVI1   [4]  = {0x41, 0x56, 0x49, 0x31}; // "AVI1"            
const byte bufferidx1   [4]  = {0x69, 0x64, 0x78, 0x31}; // "idx1" 

                               
const byte aviHeader[AVI_HEADER_SIZE] =      // This is the AVI file header.  Some of these values get overwritten.
{
  0x52, 0x49, 0x46, 0x46,  // 0x00 "RIFF"
  0x00, 0x00, 0x00, 0x00,  // 0x04           Total file size less 8 bytes [gets updated later] 
  0x41, 0x56, 0x49, 0x20,  // 0x08 "AVI "

  0x4C, 0x49, 0x53, 0x54,  // 0x0C "LIST"
  0x44, 0x00, 0x00, 0x00,  // 0x10 68        Structure length
  0x68, 0x64, 0x72, 0x6C,  // 0x04 "hdrl"

  0x61, 0x76, 0x69, 0x68,  // 0x08 "avih"    fcc
  0x38, 0x00, 0x00, 0x00,  // 0x0C 56        Structure length
  0x90, 0xD0, 0x03, 0x00,  // 0x20 250000    dwMicroSecPerFrame     [based on FRAME_INTERVAL] 
  0x00, 0x00, 0x00, 0x00,  // 0x24           dwMaxBytesPerSec       [gets updated later] 
  0x00, 0x00, 0x00, 0x00,  // 0x28 0         dwPaddingGranularity
  0x10, 0x00, 0x00, 0x00,  // 0x2C 0x10      dwFlags - AVIF_HASINDEX set.
  0x00, 0x00, 0x00, 0x00,  // 0x30           dwTotalFrames          [gets updated later]
  0x00, 0x00, 0x00, 0x00,  // 0x34 0         dwInitialFrames (used for interleaved files only)
  0x01, 0x00, 0x00, 0x00,  // 0x38 1         dwStreams (just video)
  0x00, 0x00, 0x00, 0x00,  // 0x3C 0         dwSuggestedBufferSize
  0x20, 0x03, 0x00, 0x00,  // 0x40 800       dwWidth - 800 (S-VGA)  [based on FRAMESIZE] 
  0x58, 0x02, 0x00, 0x00,  // 0x44 600       dwHeight - 600 (S-VGA) [based on FRAMESIZE]      
  0x00, 0x00, 0x00, 0x00,  // 0x48           dwReserved
  0x00, 0x00, 0x00, 0x00,  // 0x4C           dwReserved
  0x00, 0x00, 0x00, 0x00,  // 0x50           dwReserved
  0x00, 0x00, 0x00, 0x00,  // 0x54           dwReserved

  0x4C, 0x49, 0x53, 0x54,  // 0x58 "LIST"
  0x84, 0x00, 0x00, 0x00,  // 0x5C 144
  0x73, 0x74, 0x72, 0x6C,  // 0x60 "strl"

  0x73, 0x74, 0x72, 0x68,  // 0x64 "strh"    Stream header
  0x30, 0x00, 0x00, 0x00,  // 0x68  48       Structure length
  0x76, 0x69, 0x64, 0x73,  // 0x6C "vids"    fccType - video stream
  0x4D, 0x4A, 0x50, 0x47,  // 0x70 "MJPG"    fccHandler - Codec
  0x00, 0x00, 0x00, 0x00,  // 0x74           dwFlags - not set
  0x00, 0x00,              // 0x78           wPriority - not set
  0x00, 0x00,              // 0x7A           wLanguage - not set
  0x00, 0x00, 0x00, 0x00,  // 0x7C           dwInitialFrames
  0x01, 0x00, 0x00, 0x00,  // 0x80 1         dwScale
  0x04, 0x00, 0x00, 0x00,  // 0x84 4         dwRate (frames per second)         [based on FRAME_INTERVAL]         
  0x00, 0x00, 0x00, 0x00,  // 0x88           dwStart               
  0x00, 0x00, 0x00, 0x00,  // 0x8C           dwLength (frame count)             [gets updated later]
  0x00, 0x00, 0x00, 0x00,  // 0x90           dwSuggestedBufferSize
  0x00, 0x00, 0x00, 0x00,  // 0x94           dwQuality
  0x00, 0x00, 0x00, 0x00,  // 0x98           dwSampleSize

  0x73, 0x74, 0x72, 0x66,  // 0x9C "strf"    Stream format header
  0x28, 0x00, 0x00, 0x00,  // 0xA0 40        Structure length
  0x28, 0x00, 0x00, 0x00,  // 0xA4 40        BITMAPINFOHEADER length (same as above)
  0x20, 0x03, 0x00, 0x00,  // 0xA8 800       Width                  [based on FRAMESIZE] 
  0x58, 0x02, 0x00, 0x00,  // 0xAC 600       Height                 [based on FRAMESIZE] 
  0x01, 0x00,              // 0xB0 1         Planes  
  0x18, 0x00,              // 0xB2 24        Bit count (bit depth once uncompressed)                   
  0x4D, 0x4A, 0x50, 0x47,  // 0xB4 "MJPG"    Compression 
  0x00, 0x00, 0x04, 0x00,  // 0xB8 262144    Size image (approx?)                             
  0x00, 0x00, 0x00, 0x00,  // 0xBC           X pixels per metre 
  0x00, 0x00, 0x00, 0x00,  // 0xC0           Y pixels per metre
  0x00, 0x00, 0x00, 0x00,  // 0xC4           Colour indices used  
  0x00, 0x00, 0x00, 0x00,  // 0xC8           Colours considered important (0 all important).


  0x49, 0x4E, 0x46, 0x4F, // 0xCB "INFO"
  0x1C, 0x00, 0x00, 0x00, // 0xD0 28         Structure length
  0x70, 0x61, 0x75, 0x6c, // 0xD4 
  0x2e, 0x77, 0x2e, 0x69, // 0xD8 
  0x62, 0x62, 0x6f, 0x74, // 0xDC 
  0x73, 0x6f, 0x6e, 0x40, // 0xE0 
  0x67, 0x6d, 0x61, 0x69, // 0xE4 
  0x6c, 0x2e, 0x63, 0x6f, // 0xE8 
  0x6d, 0x00, 0x00, 0x00, // 0xEC 

  0x4C, 0x49, 0x53, 0x54, // 0xF0 "LIST"
  0x00, 0x00, 0x00, 0x00, // 0xF4           Total size of frames        [gets updated later]
  0x6D, 0x6F, 0x76, 0x69  // 0xF8 "movi"
};

// Following the header above are each of the frames.  Each one consists of
//  "00dc"      Stream 0, Uncompressed DIB format.
//   0x00000000 Length of frame
//   Frame      then the rest of the frame data received from the camera (note JFIF in the frame data gets overwritten with AVI1) 
//   0x00       we also potentially add a padding byte to ensure the frame chunk is an even number of bytes.
//
// At the end of the file we add an idx1 index section.  Details in closeFile() routine.
 


camera_fb_t *frameBuffer[MAX_FRAMES];      // This is where we hold references to the captured frames in a circular buffer.
                                           // typedef struct 
                                           // {
                                           //   uint8_t *buf;         Pointer to the pixel data 
                                           //   size_t len;           Length of the buffer in bytes 
                                           //   size_t width;         Width of the buffer in pixels 
                                           //   size_t height;        Height of the buffer in pixels 
                                           //   pixformat_t format;   Format of the pixel data 
                                           // } camera_fb_t;
                                           
uint8_t  frameInPos  = 0;                  // Position within buffer where we write to.
uint8_t  frameOutPos = 0;                  // Position within buffer where we read from.
                                       
                                           // The following relate to the AVI file that gets created.
uint16_t fileFramesCaptured  = 0;          // Number of frames captured by camera.
uint16_t fileFramesWritten   = 0;          // Number of frames written to the AVI file.
uint32_t fileFramesTotalSize = 0;          // Total size of frames in file.
uint32_t fileStartTime       = 0;          // Used to calculate FPS. 
uint32_t filePadding         = 0;          // Total padding in the file.  

                                           // These 2 variable conrtol the camera, and the actions required each processing loop. 
boolean motionDetected  = false;           // This is set when motion is detected.  It stays set for MOTION_DELAY ms after motion stops.
boolean fileOpen        = false;           // This is set when we have an open AVI file.

FILE *aviFile;                             // AVI file
FILE *idx1File;                            // Temporary file used to hold the index information   
 
TaskHandle_t Core0Task;                    // freeRTOS task handler for core 0
TaskHandle_t Core1Task;                    // freeRTOS task handler for core 1

enum relative                              // Used when setting position within a file stream.
{
  FROM_START,
  FROM_CURRENT,
  FROM_END
};

ESP32Time rtc;


const char *ssid               = "Mi 11";            // WiFi network to connect to.
const char *password           = "22@bars34"; // Password.
const char *detectionServer    = "http://192.168.217.11:5000/"; // IP of the system's REST API server
const char *email              = "email@email.com";
const char *password_user      = "password";

String refresh = "";
String access_token = "";

void setup() {
  // put your setup code here, to run once:
  // Initialise serial monitor.
  Serial.begin(115200);
  while(!Serial);

  initialiseWiFi();
  connectServer();

  // Initialise EEPROM - used to hold file number used in the avi file names.
  EEPROM.begin(EEPROM_SIZE);
  RegisterCam();
  Serial.println("camera_id " + String(camera_id));

  int code = reportDetection();
  Serial.println("code " + String(code));

  // Initialise the SD card.
  //initialiseSDCard();


  // Initialise the camera.
  //initialiseCamera(); 


  // Get the current time from the Internet and initialise the ESP32's RTC. 
  // This is not essential, and this can be commented out if not required.  The only impact is
  // the files created will not have the correct timestamp.
  //initialiseTime();
  

  // Set up the task handlers on each core.
  //xTaskCreatePinnedToCore(codeCore0Task, "Core0Task", 8192, NULL, 5, &Core0Task, 0);
  //xTaskCreatePinnedToCore(codeCore1Task, "Core1Task", 8192, NULL, 5, &Core1Task, 1);
}

void loop() {
  // put your main code here, to run repeatedly:

}


int reportDetection() {
  if (last_detection != -1) {
    return 1;
  }
  last_detection = millis();
  if (camera_id == -1) {
    return -1;
  }
  if(WiFi.status()!= WL_CONNECTED){
    Serial.println("WiFi Disconnected");
    return -1;
  }
  WiFiClient client;
  HTTPClient http;
  
  http.begin(client, String(detectionServer) + "detect/report");
  
  http.addHeader("Authorization", "Bearer " + access_token);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.POST("{\"camera_id\":\"" + String(camera_id) + "\"}");
  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);      
  
  if(httpResponseCode > 0) {
    String payload = http.getString();
    Serial.println(payload);
    DynamicJsonDocument doc(512);
    deserializeJson(doc, payload);

    if (httpResponseCode == 401) {
      String msg = doc["msg"].as<String>();
      if(msg == "Token has expired") {
        RefreshLogin();
        return reportDetection();
      }
      Serial.println("Error!!! Wrong data from server");
      return -1;
    }
    
    try {
      int detec_id = doc["detection_id"].as<int>();
      last_detection = millis();
      last_detection_id = detec_id;
      return 1;
    }
    catch(...) {
      Serial.println("Error!!! Wrong data from server");
      return -1;
    }
  }

  // Free resources
  http.end();
}


// ------------------------------------------------------------------------------------------
// Core 0 is used to capture frames, and check the motion sensor.
// ------------------------------------------------------------------------------------------

void codeCore0Task(void *parameter)
{
  unsigned long currentMillis    = 0; // Current time
  unsigned long lastSensorCheck  = 0; // Last time we checked the motion sensor
  unsigned long lastPictureTaken = 0; // Last time we captured a frame
  unsigned long lastMotion       = 0; // Last time we detected movement

    
  for (;;)
  {
    currentMillis = millis();

    if (last_detection > -1) {
      if (currentMillis - last_detection > 180000) {
        last_detection = -1;
      }
    }
    // Check for movement every SENSOR_INTERVAL ms.
    if (currentMillis - lastSensorCheck > SENSOR_INTERVAL)
    {
      lastSensorCheck = currentMillis;
      
   
      // Check the sensor.
      if (digitalRead(SENSOR_PIN) == HIGH)                  // Current movement
      {
        if (!motionDetected) {
          Serial.println("Movement detected.");
          int stat = reportDetection();
          if(stat == -1)
            continue;
        }
        lastMotion = currentMillis;
        motionDetected = true;
      }
      else if (lastMotion == 0)                             // Never any movement (i.e. at startup)
        motionDetected = false;   
      else if (currentMillis - lastMotion < MOTION_DELAY)   // Recent movement
        motionDetected = true;         
      else
        motionDetected = false;                             // No recent movement
    }


    // If we need to, capture a frame every FRAME_INTERVAL ms.
    if (motionDetected && currentMillis - lastPictureTaken > FRAME_INTERVAL)
    {
      lastPictureTaken = currentMillis;

      captureFrame();
    }

    delay(1);
  }
}



// ------------------------------------------------------------------------------------------
// Core 1 is used to write the frames captured to the AVI file.
// ------------------------------------------------------------------------------------------

void codeCore1Task(void *parameter)
{
  for (;;)
  { 
        
    // Once motion is detected we open a new file.
    if (motionDetected && !fileOpen)
      startFile();


    // If there are frames waiting to be processed add these to the file.
    if (motionDetected && fileOpen && framesInBuffer() > 0)
      addToFile();


    // Once motion stops, add any remaining frames to the file, and close the file.
    if (!motionDetected && fileOpen)
      closeFile();
  
    
    delay(1);
  }
}



// ------------------------------------------------------------------------------------------
// Setup the camera.
// ------------------------------------------------------------------------------------------

void initialiseCamera()
{
  Serial.println("Initialising camera");
  
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.pixel_format = PIXFORMAT_JPEG;  // Image format PIXFORMAT_JPEG - possibly only format supported for OV2640.  PIXFORMAT_GRAYSCALE
  
  config.xclk_freq_hz = 20000000;

                                         // The follow fields determine the size of the buffer required.
  config.frame_size   = FRAMESIZE_SVGA;  //  800 x  600. 40ms capture.
  config.jpeg_quality = JPEG_QUALITY;    // 0-63.  Only relevent if PIXFORMAT_JPEG format.  Very low may cause camera to crash at higher frame sizes.
  config.fb_count     = MAX_FRAMES;      // Maximum frames (depends on frame_size, jpeg_quality, frameDelay)

  esp_err_t cam_err = esp_camera_init(&config);

  if (cam_err == ESP_OK)
  {
    Serial.println("Camera ready");    
  }
  else
  {
    Serial.print("Camera initialisation error ");
    Serial.println(esp_err_to_name(cam_err));
    fatalError();
  }


  // Some camera settings can be set using the sensor values below.
  sensor_t *s = esp_camera_sensor_get();

  s->set_brightness(s, 1);                  // -2 to 2
  s->set_contrast(s, 0);                    // -2 to 2
  s->set_saturation(s, 0);                  // -2 to 2
  s->set_special_effect(s, 0);              // Special effect.  0 - None, 1 - Negative, 2 - Grayscale, 3 - Red Tint, 4 - Green Tint, 5 - Blue Tint, 6 - Sepia
  s->set_whitebal(s, 1);                    // 0 = disable , 1 = enable
  s->set_awb_gain(s, 1);                    // 0 = disable , 1 = enable
  s->set_wb_mode(s, 0);                     // 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
  s->set_exposure_ctrl(s, 1);               // 0 = disable , 1 = enable
  s->set_aec2(s, 1);                        // 0 = disable , 1 = enable
  s->set_ae_level(s, 0);                    // -2 to 2
  s->set_aec_value(s, 300);                 // 0 to 1200
  s->set_gain_ctrl(s, 1);                   // 0 = disable , 1 = enable
  s->set_agc_gain(s, 0);                    // 0 to 30
  s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
  s->set_bpc(s, 0);                         // 0 = disable , 1 = enable
  s->set_wpc(s, 1);                         // 0 = disable , 1 = enable
  s->set_raw_gma(s, 1);                     // 0 = disable , 1 = enable
  s->set_lenc(s, 1);                        // 0 = disable , 1 = enable
  s->set_hmirror(s, 0);                     // Horizontal mirror.  0 = disable , 1 = enable
  s->set_vflip(s, 0);                       // Vertical flip.  0 = disable , 1 = enable
  s->set_dcw(s, 1);                         // 0 = disable , 1 = enable
}



// ------------------------------------------------------------------------------------------
// Routine to capture a single frame and write to memory.
// ------------------------------------------------------------------------------------------

void captureFrame()
{

  // Only start capturing frames when there is an open AVI file.
  if (!fileOpen)
  {
    Serial.println("Waiting for AVI file, frame skipped.");
    return;
  }

  
  // If the buffer is already full, skip this frame.
  if (framesInBuffer() == MAX_FRAMES)
  {
    Serial.println("Frame buffer full, frame skipped.");
    return;
  }


  // Determine where to write the frame pointer in the buffer.
  frameInPos = fileFramesCaptured % MAX_FRAMES;


  // Take a picture and store pointer to the frame in the buffer.
  frameBuffer[frameInPos] = esp_camera_fb_get();
  if (frameBuffer[frameInPos]->buf == NULL)
  {
    Serial.print("Frame capture failed.");
    return;    
  }
  

  // Keep track of the total frames captured and total size of frames (needed to update file header later).
  fileFramesCaptured++;
  fileFramesTotalSize += frameBuffer[frameInPos]->len;           

}



// ---------------------------------------------------------------------------
// Set up the SD card. 
// ---------------------------------------------------------------------------

void initialiseSDCard()
{
  Serial.println("Initialising SD card");
 
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  
  esp_vfs_fat_sdmmc_mount_config_t mount_config = 
  {
    .format_if_mount_failed = false,
    .max_files = 2,
  };
  
  sdmmc_card_t *card;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);

  if (ret == ESP_OK) 
  {
    Serial.println("SD card ready");
  }
  else  
  { 
    Serial.print("SD card initialisation error ");
    Serial.println(esp_err_to_name(ret));
    fatalError();
  }
}



// ------------------------------------------------------------------------------------------
// Routine to create a new AVI file each time motion is detected.
// ------------------------------------------------------------------------------------------

void startFile()
{ 
  char AVIFilename[30] = "/sdcard/videos/VID";
  uint16_t fileNumber = 0;
  char padded[6] = "";

    
  // Reset file statistics.
  fileFramesCaptured  = 0;        
  fileFramesTotalSize = 0;  
  fileFramesWritten   = 0; 
  filePadding         = 0;
  fileStartTime       = millis();


  // Get the last file number used from the EEPROM.
  EEPROM.get(0, fileNumber);

  
  // Increment the file number, and format the new file name.
  fileNumber++;
  sprintf(padded, "%05d", fileNumber);
  strcat(AVIFilename, padded);
  strcat(AVIFilename, ".avi");

  last_filename = String(AVIFilename);
    
  
  // Open the AVI file.
  aviFile = fopen(AVIFilename, "w");
  if (aviFile == NULL)  
  {
    Serial.print ("Unable to open AVI file ");
    Serial.println(AVIFilename);
    return;  
  }  
  else
  {
    Serial.print(AVIFilename);
    Serial.println(" opened.");
  }
  
  
  // Write the AVI header to the file.
  size_t written = fwrite(aviHeader, 1, AVI_HEADER_SIZE, aviFile);
  if (written != AVI_HEADER_SIZE)
  {
    Serial.println("Unable to write header to AVI file");
    return;
   }


  // Update the EEPROM with the new file number.
  EEPROM.put(0, fileNumber);
  EEPROM.commit();


  // Open the idx1 temporary file.  This is read/write because we read back in after writing.
  idx1File = fopen("/sdcard/videos/idx1.tmp", "w+");
  if (idx1File == NULL)  
  {
    Serial.println ("Unable to open idx1 file for read/write");
    return;  
  }  


  // Set the flag to indicate we are ready to start recording.  
  fileOpen = true;
}



// ------------------------------------------------------------------------------------------
// Routine to add a frame to the AVI file.  Should only be called when framesInBuffer() > 0, 
// and there is already a file open.
// ------------------------------------------------------------------------------------------

void addToFile()
{
  // For each frame we write a chunk to the AVI file made up of:
  //  "00dc" - chunk header.  Stream ID (00) & type (dc = compressed video)
  //  The size of the chunk (frame size + padding)
  //  The frame from camera frame buffer
  //  Padding (0x00) to ensure an even number of bytes in the chunk.  
  // 
  // We then update the FOURCC in the frame from JFIF to AVI1  
  //
  // We also write to the temporary idx file.  This keeps track of the offset & size of each frame.
  // This is read back later (before we close the AVI file) to update the idx1 chunk.
  
  size_t bytesWritten;
  

  // Determine the position to read from in the buffer.
  frameOutPos = fileFramesWritten % MAX_FRAMES;


  // Calculate if a padding byte is required (frame chunks need to be an even number of bytes).
  uint8_t paddingByte = frameBuffer[frameOutPos]->len & 0x00000001;
  

  // Keep track of the current position in the file relative to the start of the movi section.  This is used to update the idx1 file.
  uint32_t frameOffset = ftell(aviFile) - AVI_HEADER_SIZE;

  
  // Add the chunk header "00dc" to the file.
  bytesWritten = fwrite(buffer00dc, 1, 4, aviFile); 
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write 00dc header to AVI file");
    return;
  }


  // Add the frame size to the file (including padding).
  uint32_t frameSize = frameBuffer[frameOutPos]->len + paddingByte;  
  bytesWritten = writeLittleEndian(frameSize, aviFile, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write frame size to AVI file");
    return;
  }
  

  // Write the frame from the camera.
  bytesWritten = fwrite(frameBuffer[frameOutPos]->buf, 1, frameBuffer[frameOutPos]->len, aviFile);
  if (bytesWritten != frameBuffer[frameOutPos]->len)
  {
    Serial.println("Unable to write frame to AVI file");
    return;
  }

    
  // Release this frame from memory.
  esp_camera_fb_return(frameBuffer[frameOutPos]);   


  // The frame from the camera contains a chunk header of JFIF (bytes 7-10) that we want to replace with AVI1.
  // So we move the write head back to where the frame was just written + 6 bytes. 
  fseek(aviFile, (bytesWritten - 6) * -1, SEEK_END);
  

  // Then overwrite with the new chunk header value of AVI1.
  bytesWritten = fwrite(bufferAVI1, 1, 4, aviFile);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write AVI1 to AVI file");
    return;
  }

 
  // Move the write head back to the end of the file.
  fseek(aviFile, 0, SEEK_END);

    
  // If required, add the padding to the file.
  if(paddingByte > 0)
  {
    bytesWritten = fwrite(buffer0000, 1, paddingByte, aviFile); 
    if (bytesWritten != paddingByte)
    {
      Serial.println("Unable to write padding to AVI file");
      return;
    }
  }


  // Write the frame offset to the idx1 file for this frame (used later).
  bytesWritten = writeLittleEndian(frameOffset, idx1File, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write frame offset to idx1 file");
    return;
  } 


  // Write the frame size to the idx1 file for this frame (used later).
  bytesWritten = writeLittleEndian(frameSize - paddingByte, idx1File, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write frame size to idx1 file");
    return;
  } 

  
  // Increment the frames written count, and keep track of total padding.
  fileFramesWritten++;
  filePadding = filePadding + paddingByte;
}



// ------------------------------------------------------------------------------------------
// Once motion stops we update the file totals, write the idx1 chunk, and close the file.
// ------------------------------------------------------------------------------------------

void closeFile()
{
  // Update the flag immediately to prevent any further frames getting written to the buffer.
  fileOpen = false;


  // Flush any remaining frames from the buffer.  
  while(framesInBuffer() > 0)
  {
    addToFile();
  }

  
  // Calculate how long the AVI file runs for.
  unsigned long fileDuration = (millis() - fileStartTime) / 1000UL;

 
  // Update AVI header with total file size. This is the sum of:
  //   AVI header (252 bytes less the first 8 bytes)
  //   fileFramesWritten * 8 (extra chunk bytes for each frame)
  //   fileFramesTotalSize (frames from the camera)
  //   filePadding
  //   idx1 section (8 + 16 * fileFramesWritten)
  writeLittleEndian((AVI_HEADER_SIZE - 8) + fileFramesWritten * 8 + fileFramesTotalSize + filePadding + (8 + 16 * fileFramesWritten), aviFile, 0x04, FROM_START);


  // Update the AVI header with maximum bytes per second.
  uint32_t maxBytes = fileFramesTotalSize / fileDuration;  
  writeLittleEndian(maxBytes, aviFile, 0x24, FROM_START);
  

  // Update AVI header with total number of frames.
  writeLittleEndian(fileFramesWritten, aviFile, 0x30, FROM_START);
  
  
  // Update stream header with total number of frames.
  writeLittleEndian(fileFramesWritten, aviFile, 0x8C, FROM_START);


  // Update movi section with total size of frames.  This is the sum of:
  //   fileFramesWritten * 8 (extra chunk bytes for each frame)
  //   fileFramesTotalSize (frames from the camera)
  //   filePadding
  writeLittleEndian(fileFramesWritten * 8 + fileFramesTotalSize + filePadding, aviFile, 0xF4, FROM_START);


  // Move the write head back to the end of the AVI file.
  fseek(aviFile, 0, SEEK_END);

   
  // Add the idx1 section to the end of the AVI file
  writeIdx1Chunk();
  
  
  fclose(aviFile);
  
  Serial.print("File closed, size: ");
  Serial.println(AVI_HEADER_SIZE + fileFramesWritten * 8 + fileFramesTotalSize + filePadding + (8 + 16 * fileFramesWritten));

}



// ----------------------------------------------------------------------------------
// Routine to add the idx1 (frame index) chunk to the end of the file.  
// ----------------------------------------------------------------------------------

void writeIdx1Chunk()
{
  // The idx1 chunk consists of:
  // +--- 1 per file ----------------------------------------------------------------+ 
  // | fcc         FOURCC 'idx1'                                                     |
  // | cb          DWORD  length not including first 8 bytes                         |
  // | +--- 1 per frame -----------------------------------------------------------+ |
  // | | dwChunkId DWORD  '00dc' StreamID = 00, Type = dc (compressed video frame) | |
  // | | dwFlags   DWORD  '0000'  dwFlags - none set                               | | 
  // | | dwOffset  DWORD   Offset from movi for this frame                         | |
  // | | dwSize    DWORD   Size of this frame                                      | |
  // | +---------------------------------------------------------------------------+ | 
  // +-------------------------------------------------------------------------------+  
  // The offset & size of each frame are read from the idx1.tmp file that we created
  // earlier when adding each frame to the main file.
  // 
  size_t bytesWritten = 0;


  // Write the idx1 header to the file
  bytesWritten = fwrite(bufferidx1, 1, 4, aviFile);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write idx1 chunk header to AVI file");
    return;
  }


  // Write the chunk size to the file.
  bytesWritten = writeLittleEndian((uint32_t)fileFramesWritten * 16, aviFile, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write idx1 size to AVI file");
    return;
  }


  // We need to read the idx1 file back in, so move the read head to the start of the idx1 file.
  fseek(idx1File, 0, SEEK_SET);
  
  
  // For each frame, write a sub chunk to the AVI file (offset & size are read from the idx file)
  char readBuffer[8];
  for (uint32_t x = 0; x < fileFramesWritten; x++)
  {
    // Read the offset & size from the idx file.
    bytesWritten = fread(readBuffer, 1, 8, idx1File);
    if (bytesWritten != 8)
    {
      Serial.println("Unable to read from idx file");
      return;
    }
    
    // Write the subchunk header 00dc
    bytesWritten = fwrite(buffer00dc, 1, 4, aviFile);
    if (bytesWritten != 4)
    {
      Serial.println("Unable to write 00dc to AVI file idx");
      return;
    }

    // Write the subchunk flags
    bytesWritten = fwrite(buffer0000, 1, 4, aviFile);
    if (bytesWritten != 4)
    {
      Serial.println("Unable to write flags to AVI file idx");
      return;
    }

    // Write the offset & size
    bytesWritten = fwrite(readBuffer, 1, 8, aviFile);
    if (bytesWritten != 8)
    {
      Serial.println("Unable to write offset & size to AVI file idx");
      return;
    }
  }


  // Close the idx1 file.
  fclose(idx1File);
  
}


void initialiseWiFi() {
  unsigned long wifiStart        = millis();


  // Connect to WiFi.
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT) 
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) 
  {
    Serial.println(" connected.");
  }
  else
  {
    Serial.println(" connection failed.");
    WiFi.mode(WIFI_OFF);
    return;
  }
}

void connectServer() {
  if(WiFi.status()== WL_CONNECTED){
    WiFiClient client;
    HTTPClient http;
    
    http.begin(client, String(detectionServer) + "auth/login");
    
    http.addHeader("Content-Type", "application/json");
    int httpResponseCode = http.POST("{\"email\":\"" + String(email) + "\",\"password\":\"" + String(password_user) + "\"}");
    
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);

    if(httpResponseCode > 0) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);

      try {
        refresh = doc["user"]["refresh"].as<String>();
        access_token = doc["user"]["access"].as<String>();
      }
      catch(...) {
        Serial.println("Error!!! Wrong data from server");
        return;
      }
    }
    
    // Free resources
    http.end();
    }
    else {
    Serial.println("WiFi Disconnected");
    }
}

void RefreshLogin() { // Call login funciton if refresh token doesn't work.
  if(WiFi.status()== WL_CONNECTED){
    WiFiClient client;
    HTTPClient http;
    
    http.begin(client, String(detectionServer) + "auth/token/refresh");
    
    http.addHeader("Authorization", "Bearer " + refresh);
    int httpResponseCode = http.GET();
    
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);

    if(httpResponseCode > 0) {
      String payload = http.getString();
      DynamicJsonDocument doc(512);
      deserializeJson(doc, payload);

      try {
        access_token = doc["access"].as<String>();
      }
      catch(...) {
        Serial.println("Error!!! Wrong data from server");
        return;
      }
    }
    
    // Free resources
    http.end();
    }
    else {
    Serial.println("WiFi Disconnected");
    }
}

void RegisterCam() {
  if(EEPROM.read(2) < 255) {
    camera_id = EEPROM.read(2);
  }
  else {
    if(WiFi.status()== WL_CONNECTED){
      WiFiClient client;
      HTTPClient http;
      
      http.begin(client, String(detectionServer) + "detect/camera/add");
      
      http.addHeader("Authorization", "Bearer " + access_token);
      int httpResponseCode = http.POST("");
      
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
  
      if(httpResponseCode == 200) {
        String payload = http.getString();
        Serial.println(payload);
        DynamicJsonDocument doc(512);
        deserializeJson(doc, payload);
  
        try {
          camera_id = doc[0]["camera_id"].as<int>();
          EEPROM.write(2, camera_id);
          EEPROM.commit();
        }
        catch(...) {
          Serial.println("Error!!! Wrong data from server");
          return;
        }
      }
      else if(httpResponseCode == 401) {
        RefreshLogin();
        RegisterCam();
      }
      
      // Free resources
      http.end();
      }
      else {
      Serial.println("WiFi Disconnected");
      }
  }
}

// ----------------------------------------------------------------------------------
// Routine to connect to WiFi, get the time from NTP server, and set the ESP's RTC.
// This is only done so the timestamps on the AVI files created are correct. It's not 
// critical for the functioning of the video camera.
// ----------------------------------------------------------------------------------

void initialiseTime()
{  
  const char *ntpServer          = "pool.ntp.org";   // NTP server
  const long  gmtOffset_sec      = 3 * 60 * 60;     // Jerusalem GMT (+3 hours)
  const int   daylightOffset_sec = 3600;             // 1 hour daylight savings
    
  
  //Get the time and set the RTC.
  Serial.println("Getting time.");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  
  // Display the time
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)) 
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  else
    Serial.println("Failed to obtain time");
}



// ------------------------------------------------------------------------------------------
// Write a 32 bit value in little endian format (LSB first) to file at specific location.
// ------------------------------------------------------------------------------------------

uint8_t writeLittleEndian(uint32_t value, FILE *file, int32_t offset, relative position)
{
  uint8_t digit[1];
  uint8_t writeCount = 0;

  
  // Set position within file.  Either relative to: SOF, current position, or EOF.
  if (position == FROM_START)          
    fseek(file, offset, SEEK_SET);    // offset >= 0
  else if (position == FROM_CURRENT)
    fseek(file, offset, SEEK_CUR);    // Offset > 0, < 0, or 0
  else if (position == FROM_END)
    fseek(file, offset, SEEK_END);    // offset <= 0 ??
  else
    return 0;  


  // Write the value to the file a byte at a time (LSB first).
  for (uint8_t x = 0; x < 4; x++)
  {
    digit[0] = value % 0x100;
    writeCount = writeCount + fwrite(digit, 1, 1, file);
    value = value >> 8;
  }


  // Return the number of bytes written to the file.
  return writeCount;
}



// ------------------------------------------------------------------------------------------
// Calculate how many frames are waiting to be processed.
// ------------------------------------------------------------------------------------------

uint8_t framesInBuffer()
{
  return fileFramesCaptured - fileFramesWritten;
}



// ------------------------------------------------------------------------------------------
// If we get here, then something bad has happened so easiest thing is just to restart.
// ------------------------------------------------------------------------------------------

void fatalError()
{
  Serial.println("Fatal error - restarting.");
  delay(1000);
  
  ESP.restart();
}
