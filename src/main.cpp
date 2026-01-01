/**
 * ESP32-C3 + 2.2インチLCD NHKニュースRSSリーダー
 * 
 * 使用LCD: ILI9341 2.2インチ (240x320 縦向き)
 * 使用ボード: Seeed XIAO ESP32C3
 * 使用ライブラリ: LovyanGFX (日本語フォント対応)
 * 
 * 機能:
 * - APモードで設定変更可能 (GPIO9ボタン長押しで起動)
 * - WiFi設定、RSS URL設定
 * - NHK NEWS WEB RSS から取得して表示
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>

#include <LovyanGFX.hpp>

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

// ================== 設定関連 ==================
Preferences preferences;

// デフォルト設定
#define DEFAULT_RSS_URL "https://www.nhk.or.jp/rss/news/cat0.xml"

// 設定保存用
String cfg_ssid;
String cfg_password;
String cfg_rssUrl;

// APモード設定
const char* AP_SSID = "ESP32-RSSReader";
const char* AP_PASSWORD = "12345678";
WebServer server(80);

// ボタンピン (XIAO ESP32C3のブートボタン)
#define BUTTON_PIN 9
#define LONG_PRESS_TIME 3000  // 3秒長押しでAPモード

// 動作モード
bool apMode = false;

// 最大記事数
#define MAX_ARTICLES 30

// 記事データ構造 (タイトルと説明を保持)
struct Article {
  String title;
  String description;
};

Article articles[MAX_ARTICLES];
int articleCount = 0;
int currentArticle = 0;

// 表示設定
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define HEADER_HEIGHT 20
#define TITLE_AREA_HEIGHT 50
#define FOOTER_HEIGHT 18
#define DESC_AREA_TOP (HEADER_HEIGHT + TITLE_AREA_HEIGHT)
#define DESC_AREA_HEIGHT (SCREEN_HEIGHT - DESC_AREA_TOP - FOOTER_HEIGHT)

// スクロール設定
int scrollOffset = 0;
int maxScrollOffset = 0;
const int SCROLL_STEP = 2;  // スクロール速度 (ピクセル/フレーム)
const unsigned long SCROLL_INTERVAL = 50;  // スクロール更新間隔 (ms)
unsigned long lastScrollTime = 0;
bool scrollingDown = true;
const unsigned long SCROLL_PAUSE = 2000;  // 上下端で一時停止する時間 (ms)
unsigned long scrollPauseStart = 0;
bool scrollPaused = false;

// 更新間隔 (10分)
const unsigned long UPDATE_INTERVAL = 600000;
unsigned long lastUpdate = 0;

// 記事切り替え間隔 (15秒) - スクロールが終わったら次へ
const unsigned long ARTICLE_INTERVAL = 15000;
unsigned long lastArticleChange = 0;

// LCDオブジェクト
LGFX tft;

// スプライト (説明文スクロール用)
LGFX_Sprite descSprite(&tft);

// ================== 設定の読み込み・保存 ==================
void loadSettings() {
  preferences.begin("rssreader", true);  // 読み取り専用
  cfg_ssid = preferences.getString("ssid", SSID);
  cfg_password = preferences.getString("password", PASSWORD);
  cfg_rssUrl = preferences.getString("rssUrl", DEFAULT_RSS_URL);
  preferences.end();
  
  Serial.println("Settings loaded:");
  Serial.println("  SSID: " + cfg_ssid);
  Serial.println("  RSS: " + cfg_rssUrl);
}

void saveSettings() {
  preferences.begin("rssreader", false);  // 書き込みモード
  preferences.putString("ssid", cfg_ssid);
  preferences.putString("password", cfg_password);
  preferences.putString("rssUrl", cfg_rssUrl);
  preferences.end();
  
  Serial.println("Settings saved!");
}

// ================== APモード Webサーバー ==================
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>RSS Reader Settings</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #1a1a2e; color: #eee; }
    h1 { color: #e94560; text-align: center; }
    .container { max-width: 400px; margin: 0 auto; }
    label { display: block; margin-top: 15px; font-weight: bold; }
    input, select { width: 100%; padding: 10px; margin-top: 5px; border: none; border-radius: 5px; box-sizing: border-box; }
    input[type="text"], input[type="password"] { background: #16213e; color: #fff; }
    button { width: 100%; padding: 12px; margin-top: 20px; background: #e94560; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; }
    button:hover { background: #ff6b6b; }
    .preset { margin-top: 10px; }
    select { background: #16213e; color: #fff; }
    .info { background: #16213e; padding: 10px; border-radius: 5px; margin-top: 20px; font-size: 12px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>RSS Reader</h1>
    <form action="/save" method="POST">
      <label>WiFi SSID:</label>
      <input type="text" name="ssid" value="%SSID%" required>
      
      <label>WiFi Password:</label>
      <input type="password" name="password" value="%PASSWORD%">
      
      <label>RSS URL:</label>
      <input type="text" name="rssUrl" value="%RSSURL%" required>
      
      <label class="preset">Preset RSS:</label>
      <select onchange="document.querySelector('input[name=rssUrl]').value=this.value">
        <option value="">-- Select Preset --</option>
        <option value="https://www.nhk.or.jp/rss/news/cat0.xml">NHK - 主要ニュース</option>
        <option value="https://www.nhk.or.jp/rss/news/cat1.xml">NHK - 社会</option>
        <option value="https://www.nhk.or.jp/rss/news/cat3.xml">NHK - 科学・医療</option>
        <option value="https://www.nhk.or.jp/rss/news/cat4.xml">NHK - 政治</option>
        <option value="https://www.nhk.or.jp/rss/news/cat5.xml">NHK - 経済</option>
        <option value="https://www.nhk.or.jp/rss/news/cat6.xml">NHK - 国際</option>
        <option value="https://www.nhk.or.jp/rss/news/cat7.xml">NHK - スポーツ</option>
        <option value="https://news.yahoo.co.jp/rss/topics/top-picks.xml">Yahoo - 主要</option>
        <option value="https://news.yahoo.co.jp/rss/topics/it.xml">Yahoo - IT</option>
      </select>
      
      <button type="submit">Save & Restart</button>
    </form>
    <div class="info">
      <strong>Access Point:</strong> %APSSID%<br>
      <strong>IP:</strong> 192.168.4.1<br>
      <strong>Hint:</strong> Save settings to restart in Station mode.
    </div>
  </div>
</body>
</html>
)rawliteral";

void handleRoot() {
  String html = htmlPage;
  html.replace("%SSID%", cfg_ssid);
  html.replace("%PASSWORD%", cfg_password);
  html.replace("%RSSURL%", cfg_rssUrl);
  html.replace("%APSSID%", AP_SSID);
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("rssUrl")) {
    cfg_ssid = server.arg("ssid");
    cfg_password = server.arg("password");
    cfg_rssUrl = server.arg("rssUrl");
    
    saveSettings();
    
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <style>
    body { font-family: Arial; text-align: center; padding: 50px; background: #1a1a2e; color: #eee; }
    h1 { color: #4ecca3; }
  </style>
</head>
<body>
  <h1>Settings Saved!</h1>
  <p>Restarting in Station mode...</p>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void startAPMode() {
  apMode = true;
  
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setFont(&fonts::efontJA_16);
  tft.drawString("設定モード", 120, 40);
  
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("WiFi APに接続:", 120, 80);
  
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(AP_SSID, 120, 110);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Password:", 120, 140);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(AP_PASSWORD, 120, 165);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("ブラウザで開く:", 120, 205);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("http://192.168.4.1", 120, 235);
  
  tft.setFont(&fonts::Font0);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Save settings to exit AP mode", 120, 290);
  tft.setTextDatum(TL_DATUM);
  
  // APモード開始
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  Serial.println("AP Mode started");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  
  // Webサーバー設定
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  
  Serial.println("Web server started");
}

// ================== WiFi接続 (ステーションモード) ==================
void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::efontJA_12);
  tft.setCursor(10, 10);
  tft.println("WiFi接続中...");
  tft.print("SSID: ");
  tft.println(cfg_ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg_ssid.c_str(), cfg_password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    tft.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    tft.println();
    tft.println("接続成功!");
  } else {
    tft.println();
    tft.println("接続失敗");
  }
  delay(1000);
}

// XMLタグから値を抽出
String extractXmlValue(const String& xml, const String& tag) {
  String startTag = "<" + tag;
  String endTag = "</" + tag + ">";
  
  int startPos = xml.indexOf(startTag);
  if (startPos < 0) return "";
  
  int tagEnd = xml.indexOf(">", startPos);
  if (tagEnd < 0) return "";
  
  int valueStart = tagEnd + 1;
  int endPos = xml.indexOf(endTag, valueStart);
  
  if (endPos < 0) return "";
  
  String value = xml.substring(valueStart, endPos);
  value.replace("<![CDATA[", "");
  value.replace("]]>", "");
  value.trim();
  
  return value;
}

// HTMLエンティティをデコード
void decodeHtmlEntities(String& text) {
  text.replace("&amp;", "&");
  text.replace("&lt;", "<");
  text.replace("&gt;", ">");
  text.replace("&quot;", "\"");
  text.replace("&#39;", "'");
  text.replace("&nbsp;", " ");
}

// NHK RSSフィードをパース
int parseRSS(const String& payload) {
  int added = 0;
  int searchStart = 0;
  
  while (articleCount < MAX_ARTICLES) {
    int itemStart = payload.indexOf("<item>", searchStart);
    if (itemStart < 0) break;
    
    int itemEnd = payload.indexOf("</item>", itemStart);
    if (itemEnd < 0) break;
    
    String item = payload.substring(itemStart, itemEnd + 7);
    String title = extractXmlValue(item, "title");
    String description = extractXmlValue(item, "description");
    
    // HTMLエンティティをデコード
    decodeHtmlEntities(title);
    decodeHtmlEntities(description);
    
    if (title.length() > 0) {
      articles[articleCount].title = title;
      articles[articleCount].description = description.length() > 0 ? description : "説明なし";
      articleCount++;
      added++;
    }
    
    searchStart = itemEnd + 7;
  }
  return added;
}

// RSSフィード取得
bool fetchRSS() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::efontJA_12);
  tft.setCursor(10, 100);
  tft.println("ニュース取得中...");
  tft.setCursor(10, 120);
  tft.println(cfg_rssUrl.substring(0, 35) + "...");
  
  articleCount = 0;
  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient http;
  http.begin(client, cfg_rssUrl);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // リダイレクト追跡を有効化
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int added = parseRSS(payload);
    tft.setCursor(10, 130);
    tft.printf("取得完了: %d件", added);
    Serial.printf("NHK News: +%d articles\n", added);
  } else {
    tft.setCursor(10, 130);
    tft.printf("エラー: %d", httpCode);
    Serial.printf("HTTP error: %d\n", httpCode);
  }
  http.end();
  
  delay(500);
  Serial.printf("Total articles: %d\n", articleCount);
  return articleCount > 0;
}

// 説明文の高さを計算（折り返し込み）
int calculateTextHeight(const String& text, int maxWidth) {
  tft.setFont(&fonts::efontJA_12);
  int lineHeight = 16;
  int lines = 1;
  int currentWidth = 0;
  
  for (int i = 0; i < text.length(); ) {
    uint8_t c = text.charAt(i);
    int charBytes = 1;
    
    // UTF-8マルチバイト文字のバイト数を判定
    if ((c & 0xF0) == 0xF0) charBytes = 4;
    else if ((c & 0xE0) == 0xE0) charBytes = 3;
    else if ((c & 0xC0) == 0xC0) charBytes = 2;
    
    String ch = text.substring(i, i + charBytes);
    int charWidth = tft.textWidth(ch);
    
    if (currentWidth + charWidth > maxWidth) {
      lines++;
      currentWidth = charWidth;
    } else {
      currentWidth += charWidth;
    }
    
    i += charBytes;
  }
  
  return lines * lineHeight;
}

// 説明文をスプライトに描画（折り返し対応）
void drawDescriptionToSprite(LGFX_Sprite& sprite, const String& text, int maxWidth) {
  sprite.setFont(&fonts::efontJA_12);
  sprite.setTextColor(TFT_WHITE);
  
  int lineHeight = 16;
  int x = 4;
  int y = 4;
  int currentWidth = 0;
  
  for (int i = 0; i < text.length(); ) {
    uint8_t c = text.charAt(i);
    int charBytes = 1;
    
    // UTF-8マルチバイト文字のバイト数を判定
    if ((c & 0xF0) == 0xF0) charBytes = 4;
    else if ((c & 0xE0) == 0xE0) charBytes = 3;
    else if ((c & 0xC0) == 0xC0) charBytes = 2;
    
    String ch = text.substring(i, i + charBytes);
    int charWidth = tft.textWidth(ch);
    
    if (currentWidth + charWidth > maxWidth - 8) {
      y += lineHeight;
      currentWidth = 0;
      x = 4;
    }
    
    sprite.setCursor(x + currentWidth, y);
    sprite.print(ch);
    currentWidth += charWidth;
    
    i += charBytes;
  }
}

// 現在の記事を表示
void displayArticle() {
  if (articleCount == 0) return;
  
  tft.fillScreen(TFT_BLACK);
  
  // ヘッダー (NHKニュース + 記事番号)
  tft.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, tft.color565(180, 0, 0));  // NHK赤
  tft.setTextColor(TFT_WHITE, tft.color565(180, 0, 0));
  tft.setFont(&fonts::Font0);
  tft.setCursor(5, 5);
  tft.print("NHK NEWS ");
  tft.print(currentArticle + 1);
  tft.print("/");
  tft.print(articleCount);
  
  // タイトルエリア背景
  tft.fillRect(0, HEADER_HEIGHT, SCREEN_WIDTH, TITLE_AREA_HEIGHT, tft.color565(30, 30, 50));
  
  // タイトル表示（折り返し対応）
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_YELLOW, tft.color565(30, 30, 50));
  
  String title = articles[currentArticle].title;
  int titleMaxWidth = SCREEN_WIDTH - 10;
  int tx = 5;
  int ty = HEADER_HEIGHT + 5;
  int titleLineHeight = 16;
  int currentWidth = 0;
  int lineCount = 0;
  int maxLines = 3;  // タイトルは最大3行
  
  for (int i = 0; i < title.length() && lineCount < maxLines; ) {
    uint8_t c = title.charAt(i);
    int charBytes = 1;
    
    if ((c & 0xF0) == 0xF0) charBytes = 4;
    else if ((c & 0xE0) == 0xE0) charBytes = 3;
    else if ((c & 0xC0) == 0xC0) charBytes = 2;
    
    String ch = title.substring(i, i + charBytes);
    int charWidth = tft.textWidth(ch);
    
    if (currentWidth + charWidth > titleMaxWidth) {
      ty += titleLineHeight;
      currentWidth = 0;
      lineCount++;
      if (lineCount >= maxLines) break;
    }
    
    tft.setCursor(tx + currentWidth, ty);
    tft.print(ch);
    currentWidth += charWidth;
    
    i += charBytes;
  }
  
  // 説明エリアの区切り線
  tft.drawFastHLine(0, DESC_AREA_TOP, SCREEN_WIDTH, TFT_DARKGREY);
  
  // 説明文の高さを計算
  String desc = articles[currentArticle].description;
  int descHeight = calculateTextHeight(desc, SCREEN_WIDTH - 8) + 16;
  
  // スプライト作成（説明文全体を描画）
  descSprite.deleteSprite();
  descSprite.createSprite(SCREEN_WIDTH, descHeight);
  descSprite.fillSprite(TFT_BLACK);
  drawDescriptionToSprite(descSprite, desc, SCREEN_WIDTH);
  
  // スクロール範囲を計算
  maxScrollOffset = max(0, descHeight - DESC_AREA_HEIGHT);
  scrollOffset = 0;
  scrollingDown = true;
  scrollPaused = false;
  
  // 初期表示 (クリッピングを使用)
  tft.setClipRect(0, DESC_AREA_TOP, SCREEN_WIDTH, DESC_AREA_HEIGHT);
  descSprite.pushSprite(0, DESC_AREA_TOP - scrollOffset);
  tft.clearClipRect();
  
  // フッター
  tft.fillRect(0, SCREEN_HEIGHT - FOOTER_HEIGHT, SCREEN_WIDTH, FOOTER_HEIGHT, TFT_NAVY);
  tft.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
  tft.setFont(&fonts::Font0);
  tft.setCursor(5, SCREEN_HEIGHT - FOOTER_HEIGHT + 4);
  if (maxScrollOffset > 0) {
    tft.print("Scroll | ");
  }
  tft.print("Update:");
  tft.print(UPDATE_INTERVAL / 60000);
  tft.print("min");
}

// スクロール更新
void updateScroll() {
  if (maxScrollOffset <= 0) return;  // スクロール不要
  
  unsigned long now = millis();
  
  // 一時停止中
  if (scrollPaused) {
    if (now - scrollPauseStart >= SCROLL_PAUSE) {
      scrollPaused = false;
    }
    return;
  }
  
  // スクロール更新
  if (now - lastScrollTime >= SCROLL_INTERVAL) {
    if (scrollingDown) {
      scrollOffset += SCROLL_STEP;
      if (scrollOffset >= maxScrollOffset) {
        scrollOffset = maxScrollOffset;
        scrollingDown = false;
        scrollPaused = true;
        scrollPauseStart = now;
      }
    } else {
      scrollOffset -= SCROLL_STEP;
      if (scrollOffset <= 0) {
        scrollOffset = 0;
        scrollingDown = true;
        scrollPaused = true;
        scrollPauseStart = now;
      }
    }
    
    // 説明エリアを再描画 (クリッピングを使用)
    tft.setClipRect(0, DESC_AREA_TOP, SCREEN_WIDTH, DESC_AREA_HEIGHT);
    descSprite.pushSprite(0, DESC_AREA_TOP - scrollOffset);
    tft.clearClipRect();
    
    lastScrollTime = now;
  }
}

// 次の記事へ
void nextArticle() {
  if (articleCount > 0) {
    currentArticle = (currentArticle + 1) % articleCount;
    displayArticle();
  }
}

// ================== ボタンチェック ==================
bool checkButtonForAPMode() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // 起動時にボタンが押されているかチェック
  if (digitalRead(BUTTON_PIN) == LOW) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setFont(&fonts::efontJA_12);
    tft.drawString("ボタンを押し続けて", 120, 140);
    tft.drawString("設定モードに入る...", 120, 165);
    tft.setTextDatum(TL_DATUM);
    
    unsigned long pressStart = millis();
    int progress = 0;
    
    // プログレスバー描画
    tft.drawRect(40, 200, 160, 20, TFT_WHITE);
    
    while (digitalRead(BUTTON_PIN) == LOW) {
      unsigned long elapsed = millis() - pressStart;
      int newProgress = (elapsed * 150) / LONG_PRESS_TIME;
      
      if (newProgress > progress && newProgress <= 150) {
        tft.fillRect(45, 205, newProgress, 10, TFT_GREEN);
        progress = newProgress;
      }
      
      if (elapsed >= LONG_PRESS_TIME) {
        return true;  // APモードに入る
      }
      delay(10);
    }
    
    // ボタンが離された場合は通常起動
    tft.fillScreen(TFT_BLACK);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== RSS Reader ===");
  
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  
  // 設定を読み込み
  loadSettings();
  
  // ボタン長押しでAPモードに入る
  if (checkButtonForAPMode()) {
    Serial.println("Entering AP Mode...");
    startAPMode();
    return;  // setup終了、APモードのloop処理へ
  }
  
  // スプラッシュ画面
  tft.setTextColor(tft.color565(180, 0, 0), TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setFont(&fonts::efontJA_16);
  tft.setTextSize(2);
  tft.drawString("RSS", 120, 80);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Reader", 120, 130);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Boot長押し=設定", 120, 180);
  tft.setFont(&fonts::Font0);
  tft.drawString("ESP32-C3 + LovyanGFX", 120, 220);
  tft.setTextDatum(TL_DATUM);
  delay(2000);
  
  connectWiFi();
  
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setFont(&fonts::efontJA_16);
    tft.drawString("WiFiエラー!", 120, 140);
    tft.setFont(&fonts::efontJA_12);
    tft.drawString("Bootボタン長押しで", 120, 180);
    tft.drawString("設定モードに入れます", 120, 205);
    tft.setTextDatum(TL_DATUM);
    delay(5000);
    ESP.restart();
  }
  
  if (fetchRSS()) {
    displayArticle();
  } else {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setFont(&fonts::efontJA_16);
    tft.drawString("RSS取得失敗!", 120, 160);
    tft.setTextDatum(TL_DATUM);
  }
  
  lastUpdate = millis();
  lastArticleChange = millis();
  lastScrollTime = millis();
}

void loop() {
  // APモードの場合はWebサーバー処理
  if (apMode) {
    server.handleClient();
    return;
  }
  
  // スクロール更新
  updateScroll();
  
  // 記事切り替え (15秒ごと)
  if (articleCount > 0 && millis() - lastArticleChange >= ARTICLE_INTERVAL) {
    nextArticle();
    lastArticleChange = millis();
  }
  
  // 定期更新 (10分毎)
  if (millis() - lastUpdate >= UPDATE_INTERVAL) {
    if (fetchRSS()) {
      currentArticle = 0;
      displayArticle();
    }
    lastUpdate = millis();
    lastArticleChange = millis();
  }
  
  delay(10);  // スムーズなスクロールのため短い間隔
}
