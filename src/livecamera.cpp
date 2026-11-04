/**
 * ESP32-C3 + 2.2インチLCD ライブカメラ表示
 *
 * 使用LCD: ILI9341 2.2インチ (240x320 縦向き)
 * 使用ボード: Seeed XIAO ESP32C3
 * 使用ライブラリ: LovyanGFX, JPEGDEC
 *
 * 機能:
 * - WiFi接続して国道ライブカメラ画像を表示
 * - 画面を上下に分割し、2地点を同時に表示 (各 240x160)
 * - 10秒毎に次の2地点へ切り替え、全地点をローテーション表示
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include <JPEGDEC.h>

#include "secrets.h"
#include "lgfx_config.h"
#include "net_utils.h"

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

// 画面レイアウト
const int SCREEN_WIDTH = 240;
const int PANE_HEIGHT = 160;  // 上下それぞれの表示領域の高さ
const int CAPTION_HEIGHT = 18;

// 更新間隔 (10秒)
const unsigned long UPDATE_INTERVAL = 10000;
unsigned long lastUpdate = 0;

// ダウンロード設定
const size_t MAX_JPEG_SIZE = 100 * 1024;          // 1枚あたりの上限 (ヒープ枯渇防止)
const unsigned long HTTP_TIMEOUT_MS = 10000;      // 接続・応答待ち
const unsigned long DOWNLOAD_TIMEOUT_MS = 15000;  // 本文受信全体の締め切り

// LCDオブジェクト
LGFX tft;

// JPEG デコーダー
JPEGDEC jpeg;

// 画像データバッファ（2つ分）
uint8_t* jpegBuffer1 = nullptr;
size_t jpegBufferSize1 = 0;
uint8_t* jpegBuffer2 = nullptr;
size_t jpegBufferSize2 = 0;

// デコード結果を描画するときのオフセット (中央寄せ・上下の表示位置)
int drawOffsetX = 0;
int drawOffsetY = 0;

// JPEG描画コールバック
int jpegDrawCallback(JPEGDRAW* pDraw) {
  // RGB565_BIG_ENDIAN でデコードしている。LovyanGFX は setSwapBytes(false) (デフォルト) のとき
  // uint16_t* を swap565 (ビッグエンディアン) として扱うので、そのまま渡せば正しい色になる。
  // はみ出し分は displayImage() で設定したクリップ領域で切り取られる。
  tft.pushImage(pDraw->x + drawOffsetX, pDraw->y + drawOffsetY,
                pDraw->iWidth, pDraw->iHeight, (uint16_t*)pDraw->pPixels);
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
  WiFi.setAutoReconnect(true);
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

// 画像をダウンロード
// Content-Length が無い (チャンク転送等) 場合も、MAX_JPEG_SIZE までは受信する
bool downloadImage(const char* url, uint8_t** buffer, size_t* bufferSize) {
  // 既存のバッファをクリア
  free(*buffer);
  *buffer = nullptr;
  *bufferSize = 0;

  if (!ensureWiFiConnected()) {
    Serial.println("WiFi not connected");
    return false;
  }

  auto client = createClientFor(url);  // http / https に応じたクライアント
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(*client, url)) {
    Serial.printf("HTTP begin failed: %s\n", url);
    return false;
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d (%s)\n", httpCode, url);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();  // 不明な場合は -1
  if (contentLength == 0 || contentLength > (int)MAX_JPEG_SIZE) {
    Serial.printf("Invalid content length: %d\n", contentLength);
    http.end();
    return false;
  }
  const bool knownLength = contentLength > 0;
  const size_t capacity = knownLength ? (size_t)contentLength : MAX_JPEG_SIZE;

  uint8_t* data = (uint8_t*)malloc(capacity);
  if (!data) {
    Serial.printf("malloc failed: %u bytes (free heap: %u)\n", capacity, ESP.getFreeHeap());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t bytesRead = 0;
  const unsigned long deadline = millis() + DOWNLOAD_TIMEOUT_MS;

  // 全部読むか、切断されて残りデータも無くなるか、締め切りまで読む
  while (bytesRead < capacity && (long)(deadline - millis()) > 0) {
    const size_t available = stream->available();
    if (available > 0) {
      const size_t toRead = min(available, capacity - bytesRead);
      bytesRead += stream->readBytes(data + bytesRead, toRead);
    } else if (!http.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();

  const bool complete = knownLength ? (bytesRead == capacity) : (bytesRead > 0 && bytesRead < capacity);
  Serial.printf("Downloaded %u bytes%s\n", bytesRead, complete ? "" : " (incomplete)");

  if (!complete) {
    free(data);
    return false;
  }

  if (!knownLength) {
    // 余った領域を返す (失敗しても元のポインタは有効なのでそのまま使う)
    uint8_t* shrunk = (uint8_t*)realloc(data, bytesRead);
    if (shrunk) data = shrunk;
  }

  *buffer = data;
  *bufferSize = bytesRead;
  return true;
}

// 縮小率を選ぶ: 表示領域 (targetW x targetH) を埋められる範囲で最も小さくする。
// JPEGDEC は 1, 1/2, 1/4, 1/8 の縮小のみ対応 (拡大は不可)。
int chooseDecodeScale(int width, int height, int targetW, int targetH, int* outW, int* outH) {
  static const struct { int option; int divisor; } SCALES[] = {
    {JPEG_SCALE_EIGHTH, 8},
    {JPEG_SCALE_QUARTER, 4},
    {JPEG_SCALE_HALF, 2},
  };
  for (const auto& s : SCALES) {
    const int w = width / s.divisor;
    const int h = height / s.divisor;
    if (w >= targetW && h >= targetH) {
      *outW = w;
      *outH = h;
      return s.option;
    }
  }
  *outW = width;
  *outH = height;
  return 0;  // 等倍 (元画像が表示領域より小さい場合は余白が残る)
}

// JPEG画像をデコードして表示 (240x160 の領域に中央寄せ、はみ出しは切り取り)
bool displayImage(uint8_t* buffer, size_t bufferSize, int yOffset, const char* cameraName) {
  if (!buffer || bufferSize == 0) {
    return false;
  }

  if (!jpeg.openRAM(buffer, bufferSize, jpegDrawCallback)) {
    Serial.printf("JPEG open failed: %s (error %d)\n", cameraName, jpeg.getLastError());
    return false;
  }

  const int originalWidth = jpeg.getWidth();
  const int originalHeight = jpeg.getHeight();

  int scaledW = 0;
  int scaledH = 0;
  const int decodeOption =
      chooseDecodeScale(originalWidth, originalHeight, SCREEN_WIDTH, PANE_HEIGHT, &scaledW, &scaledH);

  Serial.printf("Camera: %s, Original: %dx%d -> %dx%d\n",
                cameraName, originalWidth, originalHeight, scaledW, scaledH);

  // 中央寄せ (大きい場合は左右/上下を均等に切り取る)
  drawOffsetX = (SCREEN_WIDTH - scaledW) / 2;
  drawOffsetY = yOffset + (PANE_HEIGHT - scaledH) / 2;

  jpeg.setPixelType(RGB565_BIG_ENDIAN);

  // 自分の表示領域の外 (もう一方の画像の領域) に描かないようにクリップ
  tft.setClipRect(0, yOffset, SCREEN_WIDTH, PANE_HEIGHT);
  const int decoded = jpeg.decode(0, 0, decodeOption);
  tft.clearClipRect();
  jpeg.close();

  if (!decoded) {
    Serial.printf("JPEG decode failed: %s (error %d)\n", cameraName, jpeg.getLastError());
    return false;
  }

  // カメラ名を表示
  const int textY = yOffset + PANE_HEIGHT - CAPTION_HEIGHT;
  const uint16_t captionBg = tft.color565(0, 0, 80);
  tft.fillRect(0, textY, SCREEN_WIDTH, CAPTION_HEIGHT, captionBg);
  tft.setTextColor(TFT_WHITE, captionBg);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(cameraName, SCREEN_WIDTH / 2, textY + CAPTION_HEIGHT / 2);
  tft.setTextDatum(TL_DATUM);

  return true;
}

void drawPaneError(const char* message, int yOffset) {
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(message, SCREEN_WIDTH / 2, yOffset + PANE_HEIGHT / 2);
  tft.setTextDatum(TL_DATUM);
}

// 2つの画像を同時に表示
void displayBothImages() {
  tft.fillScreen(TFT_BLACK);

  // 上側の画像を表示
  if (displayImage(jpegBuffer1, jpegBufferSize1, 0, cameras[currentCamera1].name)) {
    Serial.printf("Displayed camera 1: %s\n", cameras[currentCamera1].name);
  } else {
    drawPaneError("取得失敗(上)", 0);
  }

  // 下側の画像を表示
  if (displayImage(jpegBuffer2, jpegBufferSize2, PANE_HEIGHT, cameras[currentCamera2].name)) {
    Serial.printf("Displayed camera 2: %s\n", cameras[currentCamera2].name);
  } else {
    drawPaneError("取得失敗(下)", PANE_HEIGHT);
  }

  // 区切り線 (画像の後に描いて上書きされないようにする)
  tft.drawFastHLine(0, PANE_HEIGHT, SCREEN_WIDTH, TFT_YELLOW);
}

// 次のカメラへ
void nextCameras() {
  currentCamera1 = (currentCamera1 + 2) % NUM_CAMERAS;
  currentCamera2 = (currentCamera2 + 2) % NUM_CAMERAS;
}

void showLoading(bool showNames) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("画像取得中...", SCREEN_WIDTH / 2, 80);
  if (showNames) {
    tft.drawString(cameras[currentCamera1].name, SCREEN_WIDTH / 2, 100);
    tft.drawString(cameras[currentCamera2].name, SCREEN_WIDTH / 2, 220);
  }
  tft.setTextDatum(TL_DATUM);
}

// 現在の2地点を取得して表示
void updateCameras() {
  Serial.printf("Fetching: %s and %s\n", cameras[currentCamera1].name, cameras[currentCamera2].name);

  const bool success1 = downloadImage(cameras[currentCamera1].url, &jpegBuffer1, &jpegBufferSize1);
  const bool success2 = downloadImage(cameras[currentCamera2].url, &jpegBuffer2, &jpegBufferSize2);

  if (success1 || success2) {
    displayBothImages();
  } else {
    // エラー表示
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setFont(&fonts::efontJA_16);
    tft.drawString("取得失敗", SCREEN_WIDTH / 2, 160);
    tft.setTextDatum(TL_DATUM);
  }
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
  showLoading(true);
  updateCameras();

  lastUpdate = millis();
}

void loop() {
  // UPDATE_INTERVAL (10秒) 毎に次のカメラ画像を表示
  if (millis() - lastUpdate >= UPDATE_INTERVAL) {
    nextCameras();
    showLoading(false);
    updateCameras();
    lastUpdate = millis();
  }

  delay(100);
}
