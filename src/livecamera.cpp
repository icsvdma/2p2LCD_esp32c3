/**
 * ESP32-C3 + 2.2インチLCD ライブカメラ表示
 * 
 * 使用LCD: ILI9341 2.2インチ (240x320 縦向き)
 * 使用ボード: Seeed XIAO ESP32C3
 * 使用ライブラリ: LovyanGFX, JPEGDEC
 * 
 * 機能:
 * - WiFi接続して国道ライブカメラ画像を表示
 * - 3秒毎に画像を切り替え
 * - 複数のカメラ地点をローテーション表示
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

#include <LovyanGFX.hpp>
#include <JPEGDEC.h>

#include "secrets.h"

// LovyanGFX LCD設定クラス
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 8;
      cfg.pin_mosi = 10;
      cfg.pin_miso = 9;
      cfg.pin_dc   = 5;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = 7;
      cfg.pin_rst  = 6;
      cfg.pin_busy = -1;
      cfg.memory_width  = 240;
      cfg.memory_height = 320;
      cfg.panel_width   = 240;
      cfg.panel_height  = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable = true;
      cfg.invert   = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

// カメラ地点の定義
struct CameraLocation {
  const char* url;  // 完全なURL
  const char* name;
};

// ライブカメラ地点
const CameraLocation cameras[] = {
  // 山形県 国道13号
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/24379-3.jpg", "山形大橋"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/05049-3.jpg", "鳥上げ坂"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/24388-3.jpg", "十日町"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/13110-3.jpg", "蟹沢"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/13146-3.jpg", "東根宮崎"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/07035-3.jpg", "村山大橋(南)"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/07036-3.jpg", "村山大橋(北)"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/07206-3.jpg", "尾花沢北IC"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/07037-3.jpg", "毒沢"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/07038-3.jpg", "舟形TN尾花沢"},
  {"http://www2.thr.mlit.go.jp/yamagata/ycam/data/07047-3.jpg", "舟形TN新庄側"},
  // 福島県 米沢
  {"https://www.thr.mlit.go.jp/fukushima/chuo/chuophoto/yonehati.jpg", "米沢八幡原"},
  {"https://www.thr.mlit.go.jp/fukushima/chuo/chuophoto/yonechu.jpg", "米沢中央"},
  {"https://www.thr.mlit.go.jp/fukushima/chuo/chuophoto/yonekita.jpg", "米沢北"}
};

const int NUM_CAMERAS = sizeof(cameras) / sizeof(cameras[0]);
int currentCamera1 = 0;  // 上側のカメラ
int currentCamera2 = 1;  // 下側のカメラ

// 更新間隔 (10秒)
const unsigned long UPDATE_INTERVAL = 10000;
unsigned long lastUpdate = 0;

// LCDオブジェクト
LGFX tft;

// JPEG デコーダー
JPEGDEC jpeg;

// 画像データバッファ（2つ分）
uint8_t* jpegBuffer1 = nullptr;
size_t jpegBufferSize1 = 0;
uint8_t* jpegBuffer2 = nullptr;
size_t jpegBufferSize2 = 0;

// 現在処理中の画像位置
int currentDrawY = 0;

// JPEG描画コールバック
int jpegDrawCallback(JPEGDRAW *pDraw) {
  // LCDに直接描画（Y座標をオフセット）
  tft.pushImage(pDraw->x, pDraw->y + currentDrawY, pDraw->iWidth, pDraw->iHeight, (uint16_t*)pDraw->pPixels);
  return 1;
}

// ================== WiFi接続 ==================
void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::efontJA_12);
  tft.setCursor(10, 10);
  tft.println("WiFi接続中...");
  tft.print("SSID: ");
  tft.println(SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    tft.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    tft.println();
    tft.println("接続成功!");
    tft.print("IP: ");
    tft.println(WiFi.localIP());
  } else {
    tft.println();
    tft.println("接続失敗");
  }
  delay(1500);
}

// カメラ画像のURLを取得
String getCameraImageUrl(const char* cameraUrl) {
  return String(cameraUrl);
}

// 画像をダウンロード
bool downloadImage(const String& url, uint8_t** buffer, size_t* bufferSize) {
  // 既存のバッファをクリア
  if (*buffer) {
    free(*buffer);
    *buffer = nullptr;
    *bufferSize = 0;
  }
  
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    *bufferSize = http.getSize();
    if (*bufferSize > 0) {
      *buffer = (uint8_t*)malloc(*bufferSize);
      if (*buffer) {
        WiFiClient* stream = http.getStreamPtr();
        size_t bytesRead = 0;
        
        while (http.connected() && bytesRead < *bufferSize) {
          size_t available = stream->available();
          if (available) {
            size_t toRead = min(available, *bufferSize - bytesRead);
            size_t read = stream->readBytes(*buffer + bytesRead, toRead);
            bytesRead += read;
          }
          delay(1);
        }
        
        http.end();
        Serial.printf("Downloaded %d bytes\n", bytesRead);
        return (bytesRead == *bufferSize);
      }
    }
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
  }
  
  http.end();
  return false;
}

// JPEG画像をデコードして表示 (240x160にリサイズして上下2分割)
bool displayImage(uint8_t* buffer, size_t bufferSize, int yOffset, const char* cameraName) {
  if (!buffer || bufferSize == 0) {
    return false;
  }
  
  currentDrawY = yOffset;
  int result = jpeg.openRAM(buffer, bufferSize, jpegDrawCallback);
  
  if (result) {
    int originalWidth = jpeg.getWidth();
    int originalHeight = jpeg.getHeight();
    
    Serial.printf("Camera: %s, Original: %dx%d\n", cameraName, originalWidth, originalHeight);
    
    // 画面の半分の高さ = 160px
    // 横幅いっぱい = 240px
    const int targetWidth = 240;
    const int targetHeight = 160;
    
    // JPEGをデコードして表示（横幅いっぱいに拡大）
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    
    // スケールを計算（JPEG_SCALE_HALF = 1/2, QUARTER = 1/4, EIGHTH = 1/8）
    float scaleX = (float)targetWidth / originalWidth;
    float scaleY = (float)targetHeight / originalHeight;
    
    // より大きいスケールを選択して画面を埋める
    float scale = max(scaleX, scaleY);
    
    int decodeOption = 0;  // フルサイズ
    if (scale <= 0.5) {
      decodeOption = JPEG_SCALE_HALF;
    } else if (scale <= 0.25) {
      decodeOption = JPEG_SCALE_QUARTER;
    } else if (scale <= 0.125) {
      decodeOption = JPEG_SCALE_EIGHTH;
    }
    
    jpeg.decode(0, 0, decodeOption);
    jpeg.close();
    
    // カメラ名を表示
    int textY = yOffset + targetHeight - 18;
    tft.fillRect(0, textY, 240, 18, tft.color565(0, 0, 80));
    tft.setTextColor(TFT_WHITE, tft.color565(0, 0, 80));
    tft.setFont(&fonts::efontJA_12);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(cameraName, 120, textY + 9);
    tft.setTextDatum(TL_DATUM);
    
    return true;
  }
  
  return false;
}

// 2つの画像を同時に表示
void displayBothImages() {
  tft.fillScreen(TFT_BLACK);
  
  // 上側の画像を表示
  if (displayImage(jpegBuffer1, jpegBufferSize1, 0, cameras[currentCamera1].name)) {
    Serial.printf("Displayed camera 1: %s\n", cameras[currentCamera1].name);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("取得失敗(上)", 120, 80);
    tft.setTextDatum(TL_DATUM);
  }
  
  // 区切り線
  tft.drawFastHLine(0, 160, 240, TFT_YELLOW);
  
  // 下側の画像を表示
  if (displayImage(jpegBuffer2, jpegBufferSize2, 160, cameras[currentCamera2].name)) {
    Serial.printf("Displayed camera 2: %s\n", cameras[currentCamera2].name);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("取得失敗(下)", 120, 240);
    tft.setTextDatum(TL_DATUM);
  }
}

// 次のカメラへ
void nextCameras() {
  currentCamera1 = (currentCamera1 + 2) % NUM_CAMERAS;
  currentCamera2 = (currentCamera2 + 2) % NUM_CAMERAS;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== Live Camera Display ===");
  
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  
  // スプラッシュ画面
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setFont(&fonts::efontJA_16);
  tft.setTextSize(2);
  tft.drawString("Live", 120, 80);
  tft.drawString("Camera", 120, 120);
  tft.setTextSize(1);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("国道13号北部", 120, 170);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("ESP32-C3 + LovyanGFX", 120, 220);
  tft.setTextDatum(TL_DATUM);
  delay(2000);
  
  connectWiFi();
  
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setFont(&fonts::efontJA_16);
    tft.drawString("WiFiエラー!", 120, 160);
    tft.setTextDatum(TL_DATUM);
    delay(5000);
    ESP.restart();
  }
  
  // 初回画像表示
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("画像取得中...", 120, 80);
  tft.drawString(cameras[currentCamera1].name, 120, 100);
  tft.setTextDatum(TL_DATUM);
  
  String url1 = getCameraImageUrl(cameras[currentCamera1].url);
  bool success1 = downloadImage(url1, &jpegBuffer1, &jpegBufferSize1);
  
  tft.setTextDatum(MC_DATUM);
  tft.drawString(cameras[currentCamera2].name, 120, 220);
  tft.setTextDatum(TL_DATUM);
  
  String url2 = getCameraImageUrl(cameras[currentCamera2].url);
  bool success2 = downloadImage(url2, &jpegBuffer2, &jpegBufferSize2);
  
  if (success1 || success2) {
    displayBothImages();
  }
  
  lastUpdate = millis();
}

void loop() {
  // 3秒毎に次のカメラ画像を表示
  if (millis() - lastUpdate >= UPDATE_INTERVAL) {
    nextCameras();
    
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setFont(&fonts::efontJA_12);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("画像取得中...", 120, 80);
    tft.setTextDatum(TL_DATUM);
    
    String url1 = getCameraImageUrl(cameras[currentCamera1].url);
    String url2 = getCameraImageUrl(cameras[currentCamera2].url);
    
    Serial.printf("Fetching: %s and %s\n", cameras[currentCamera1].name, cameras[currentCamera2].name);
    
    bool success1 = downloadImage(url1, &jpegBuffer1, &jpegBufferSize1);
    bool success2 = downloadImage(url2, &jpegBuffer2, &jpegBufferSize2);
    
    if (success1 || success2) {
      displayBothImages();
    } else {
      // エラー表示
      tft.fillScreen(TFT_RED);
      tft.setTextColor(TFT_WHITE, TFT_RED);
      tft.setTextDatum(MC_DATUM);
      tft.setFont(&fonts::efontJA_16);
      tft.drawString("取得失敗", 120, 160);
      tft.setTextDatum(TL_DATUM);
    }
    
    lastUpdate = millis();
  }
  
  delay(100);
}
