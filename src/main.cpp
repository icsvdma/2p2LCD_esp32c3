/**
 * ESP32-C3 + 2.2インチLCD RSSニュースリーダー
 *
 * 使用LCD: ILI9341 2.2インチ (240x320 縦向き)
 * 使用ボード: Seeed XIAO ESP32C3
 * 使用ライブラリ: LovyanGFX (日本語フォント対応)
 *
 * 機能:
 * - RSS フィード (デフォルト: NHK NEWS WEB) を取得して1記事ずつ表示
 * - 説明文が長い場合は自動スクロールし、最後まで表示してから次の記事へ
 * - BOOTボタン(GPIO9)を3秒長押しで設定モード (APモード) に入る
 *   ※ GPIO9 はストラッピングピン。押したまま電源投入/リセットすると
 *     ダウンロードモードで起動してしまうので、起動後に押すこと
 * - 設定モードでは WiFi 設定と RSS URL を Web ブラウザから変更可能
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_random.h>
#include <vector>

#include "secrets.h"
#include "lgfx_config.h"
#include "net_utils.h"
#include "text_utils.h"
#include "long_press_button.h"

// ================== 設定関連 ==================
Preferences preferences;

// デフォルト設定
#define DEFAULT_RSS_URL "https://www.nhk.or.jp/rss/news/cat0.xml"

// 設定保存用
String cfg_ssid;
String cfg_password;
String cfg_rssUrl;

// APモード設定 (パスワードは APモード開始時にランダム生成して画面に表示する)
const char* AP_SSID = "ESP32-RSSReader";
const int AP_PASSWORD_LENGTH = 10;
String apPassword;
WebServer server(80);

// ボタンピン (XIAO ESP32C3のBOOTボタン)
#define BUTTON_PIN 9
#define LONG_PRESS_TIME 3000      // 3秒長押しでAPモード
#define LONG_PRESS_FEEDBACK 300   // この時間以上押されたら進捗を表示

// 動作モード
bool apMode = false;

// 最大記事数
#define MAX_ARTICLES 30
// 受け付ける RSS の最大サイズ (ヒープ枯渇防止)
#define MAX_RSS_BYTES (64 * 1024)

// 記事データ構造 (タイトルと説明を保持)
struct Article {
  String title;
  String description;
};

std::vector<Article> articles;
String feedTitle = "RSS";
int currentArticle = 0;

// 表示設定
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define HEADER_HEIGHT 20
#define TITLE_AREA_HEIGHT 50
#define FOOTER_HEIGHT 18
#define DESC_AREA_TOP (HEADER_HEIGHT + TITLE_AREA_HEIGHT)
#define DESC_AREA_HEIGHT (SCREEN_HEIGHT - DESC_AREA_TOP - FOOTER_HEIGHT)
// 区切り線 (DESC_AREA_TOP の1px) の下が説明文の表示領域
#define DESC_VIEW_TOP (DESC_AREA_TOP + 1)
#define DESC_VIEW_HEIGHT (DESC_AREA_HEIGHT - 1)

#define LINE_HEIGHT 16
#define TEXT_PADDING 4
#define TITLE_MAX_LINES 3

// 色
const uint16_t COLOR_HEADER_BG = lgfx::color565(180, 0, 0);
const uint16_t COLOR_TITLE_BG  = lgfx::color565(30, 30, 50);

// スクロール設定
int scrollOffset = 0;
int maxScrollOffset = 0;
const int SCROLL_STEP = 2;                  // スクロール速度 (ピクセル/フレーム)
const unsigned long SCROLL_INTERVAL = 50;   // スクロール更新間隔 (ms)
const unsigned long SCROLL_PAUSE = 2000;    // 上下端で一時停止する時間 (ms)
unsigned long lastScrollTime = 0;
unsigned long scrollPauseStart = 0;
bool scrollPaused = false;
bool scrollDone = true;  // 説明文を最後まで表示し終えたか

// 更新間隔 (10分)。取得に1件も成功していないときは短い間隔で再試行
const unsigned long UPDATE_INTERVAL = 600000;
const unsigned long RETRY_INTERVAL = 60000;
unsigned long lastUpdate = 0;

// 記事の最短表示時間 (15秒)。スクロールが必要な記事は最後まで表示してから次へ
const unsigned long ARTICLE_INTERVAL = 15000;
unsigned long lastArticleChange = 0;

// LCDオブジェクト
LGFX tft;

// 説明文表示用スプライト (表示領域と同じサイズを1つだけ確保して使い回す)
// 8bit カラーで 240 x 231 ≒ 55KB。確保できなければ LCD に直接描画する
LGFX_Sprite descSprite(&tft);
bool descSpriteReady = false;
std::vector<TextLine> descLines;

// ================== ボタン (非ブロッキング長押し検出) ==================
LongPressButton button(BUTTON_PIN, LONG_PRESS_TIME);
bool apRequested = false;  // 起動処理中に長押しされた

// 指定時間待つ。その間に長押しされたら apRequested を立てて true を返す
bool waitWithButton(unsigned long ms) {
  const unsigned long start = millis();
  while (millis() - start < ms) {
    if (button.update()) {
      apRequested = true;
      return true;
    }
    delay(10);
  }
  return false;
}

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

// HTML に埋め込む値をエスケープ (属性値・本文どちらにも使える)
String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

const char HTML_HEAD[] = R"rawliteral(<!DOCTYPE html>
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
    label.inline { display: inline; font-weight: normal; }
    input, select { width: 100%; padding: 10px; margin-top: 5px; border: none; border-radius: 5px; box-sizing: border-box; }
    input[type="checkbox"] { width: auto; }
    input[type="text"], input[type="password"] { background: #16213e; color: #fff; }
    button { width: 100%; padding: 12px; margin-top: 20px; background: #e94560; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; }
    button:hover { background: #ff6b6b; }
    .preset { margin-top: 10px; }
    select { background: #16213e; color: #fff; }
    .info { background: #16213e; padding: 10px; border-radius: 5px; margin-top: 20px; font-size: 12px; }
    .error { background: #5c1a1a; padding: 10px; border-radius: 5px; }
  </style>
</head>
<body>
  <div class="container">
)rawliteral";

const char HTML_PRESETS[] = R"rawliteral(
      <label class="preset">Preset RSS:</label>
      <select onchange="if(this.value)document.querySelector('input[name=rssUrl]').value=this.value">
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
)rawliteral";

const char HTML_TAIL[] = R"rawliteral(
  </div>
</body>
</html>
)rawliteral";

void sendSettingsPage(int status, const String& errorMessage) {
  // 値は replace() ではなく連結で埋め込む (値に %XXX% が含まれても壊れないように)
  String html;
  html.reserve(4096);
  html += HTML_HEAD;
  html += "    <h1>RSS Reader</h1>\n";
  if (errorMessage.length() > 0) {
    html += "    <p class=\"error\">" + htmlEscape(errorMessage) + "</p>\n";
  }
  html += "    <form action=\"/save\" method=\"POST\">\n";
  html += "      <label>WiFi SSID:</label>\n";
  html += "      <input type=\"text\" name=\"ssid\" maxlength=\"32\" value=\"" + htmlEscape(cfg_ssid) + "\" required>\n";
  // 保存済みパスワードはページに埋め込まない (AP 接続者に読まれないように)
  html += "      <label>WiFi Password:</label>\n";
  html += "      <input type=\"password\" name=\"password\" maxlength=\"63\" placeholder=\"変更しない場合は空欄\" autocomplete=\"new-password\">\n";
  html += "      <input type=\"checkbox\" name=\"open\" id=\"open\" value=\"1\"> <label class=\"inline\" for=\"open\">パスワードなし (オープンネットワーク)</label>\n";
  html += "      <label>RSS URL:</label>\n";
  html += "      <input type=\"text\" name=\"rssUrl\" maxlength=\"256\" value=\"" + htmlEscape(cfg_rssUrl) + "\" required>\n";
  html += HTML_PRESETS;
  html += "      <button type=\"submit\">Save &amp; Restart</button>\n";
  html += "    </form>\n";
  html += "    <div class=\"info\">\n";
  html += "      <strong>Access Point:</strong> " + htmlEscape(AP_SSID) + "<br>\n";
  html += "      <strong>IP:</strong> " + WiFi.softAPIP().toString() + "<br>\n";
  html += "      <strong>Hint:</strong> Save settings to restart in Station mode.\n";
  html += "    </div>\n";
  html += HTML_TAIL;
  server.send(status, "text/html; charset=utf-8", html);
}

void handleRoot() {
  sendSettingsPage(200, "");
}

void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("rssUrl")) {
    sendSettingsPage(400, "Missing parameters");
    return;
  }

  String ssid = server.arg("ssid");
  String rssUrl = server.arg("rssUrl");
  String password = server.arg("password");
  const bool openNetwork = server.hasArg("open");
  rssUrl.trim();

  // 入力チェック
  if (ssid.length() < 1 || ssid.length() > 32) {
    sendSettingsPage(400, "SSID は 1〜32 バイトで入力してください");
    return;
  }
  if (!isHttpUrl(rssUrl) || rssUrl.length() > 256) {
    sendSettingsPage(400, "RSS URL は http:// または https:// で始まる 256 文字以内の URL にしてください");
    return;
  }
  if (!openNetwork && password.length() > 0 &&
      (password.length() < 8 || password.length() > 63)) {
    sendSettingsPage(400, "WiFi パスワードは 8〜63 文字で入力してください");
    return;
  }

  cfg_ssid = ssid;
  cfg_rssUrl = rssUrl;
  if (openNetwork) {
    cfg_password = "";
  } else if (password.length() > 0) {
    cfg_password = password;
  }  // 空欄なら保存済みのパスワードを維持

  saveSettings();

  const char* html = R"rawliteral(<!DOCTYPE html>
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
  server.send(200, "text/html; charset=utf-8", html);
  delay(1000);  // レスポンスを送り切る猶予
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// 紛らわしい文字 (0/O, 1/l/I) を除いたランダムなパスワードを生成
// esp_random() は RF (WiFi) 有効時にハードウェア乱数になる
String generateApPassword() {
  static const char CHARSET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  String pw;
  for (int i = 0; i < AP_PASSWORD_LENGTH; i++) {
    pw += CHARSET[esp_random() % (sizeof(CHARSET) - 1)];
  }
  return pw;
}

void startAPMode() {
  apMode = true;

  // 通常表示用のリソースを解放
  descSprite.deleteSprite();
  descSpriteReady = false;
  articles.clear();
  articles.shrink_to_fit();

  // APモード開始 (RF 有効化後にパスワードを生成)
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  apPassword = generateApPassword();
  WiFi.softAP(AP_SSID, apPassword.c_str());
  const String apIp = WiFi.softAPIP().toString();

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
  tft.setFont(&fonts::efontJA_16);
  tft.drawString(apPassword, 120, 165);

  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("ブラウザで開く:", 120, 205);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("http://" + apIp, 120, 235);

  tft.setFont(&fonts::Font0);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Save settings to exit AP mode", 120, 290);
  tft.setTextDatum(TL_DATUM);

  Serial.println("AP Mode started");
  Serial.print("IP: ");
  Serial.println(apIp);

  // Webサーバー設定
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Web server started");
}

// ================== WiFi接続 (ステーションモード) ==================
// 接続できたら true。待機中に長押しされたら apRequested が立つ
bool connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::efontJA_12);
  tft.setCursor(10, 10);
  tft.println("WiFi接続中...");
  tft.print("SSID: ");
  tft.println(cfg_ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg_ssid.c_str(), cfg_password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    if (waitWithButton(500)) return false;
    tft.print(".");
    attempts++;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  tft.println();
  tft.println(connected ? "接続成功!" : "接続失敗");
  waitWithButton(1000);
  return connected;
}

// ================== RSS 取得・解析 ==================

// XMLタグから値を抽出 (例: tag="title" は <title> / <title attr=..> にマッチし、<titleX> にはマッチしない)
String extractXmlValue(const String& xml, const String& tag) {
  const String startTag = "<" + tag;
  const String endTag = "</" + tag + ">";

  int startPos = -1;
  int from = 0;
  while (true) {
    const int pos = xml.indexOf(startTag, from);
    if (pos < 0) return "";
    const int next = pos + startTag.length();
    if (next >= (int)xml.length()) return "";
    const char c = xml[next];
    if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      startPos = pos;
      break;
    }
    from = next;
  }

  const int tagEnd = xml.indexOf(">", startPos);
  if (tagEnd < 0) return "";
  if (xml[tagEnd - 1] == '/') return "";  // <tag/> (空要素)

  const int valueStart = tagEnd + 1;
  const int endPos = xml.indexOf(endTag, valueStart);
  if (endPos < 0) return "";

  String value = xml.substring(valueStart, endPos);
  value.replace("<![CDATA[", "");
  value.replace("]]>", "");
  value.trim();

  return value;
}

// HTMLエンティティをデコード
// &amp; は最後に置換する (先に置換すると "&amp;lt;" が "<" まで二重デコードされる)
void decodeHtmlEntities(String& text) {
  text.replace("&lt;", "<");
  text.replace("&gt;", ">");
  text.replace("&quot;", "\"");
  text.replace("&#39;", "'");
  text.replace("&#039;", "'");
  text.replace("&apos;", "'");
  text.replace("&nbsp;", " ");
  text.replace("&amp;", "&");
}

// RSS フィードを解析して out に格納 (最大 MAX_ARTICLES 件)。チャンネル名も返す
int parseRSS(const String& payload, std::vector<Article>& out, String& channelTitle) {
  out.clear();

  const int firstItem = payload.indexOf("<item");
  channelTitle = extractXmlValue(firstItem >= 0 ? payload.substring(0, firstItem) : payload, "title");
  decodeHtmlEntities(channelTitle);

  int searchStart = 0;
  while ((int)out.size() < MAX_ARTICLES) {
    int itemStart = payload.indexOf("<item>", searchStart);
    if (itemStart < 0) itemStart = payload.indexOf("<item ", searchStart);
    if (itemStart < 0) break;

    const int itemEnd = payload.indexOf("</item>", itemStart);
    if (itemEnd < 0) break;

    const String item = payload.substring(itemStart, itemEnd + 7);
    String title = extractXmlValue(item, "title");
    String description = extractXmlValue(item, "description");

    decodeHtmlEntities(title);
    decodeHtmlEntities(description);

    if (title.length() > 0) {
      out.push_back({title, description.length() > 0 ? description : String("説明なし")});
    }

    searchStart = itemEnd + 7;
  }
  return out.size();
}

void showStatusScreen(const String& line1, const String& line2) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(line1, 10, 100);
  tft.drawString(fitText(tft, line2, SCREEN_WIDTH - 20), 10, 120);
}

// RSSフィード取得
// 成功したときだけ記事一覧を置き換える (失敗時は前回の記事を保持する)
bool fetchRSS() {
  showStatusScreen("ニュース取得中...", cfg_rssUrl);

  if (!ensureWiFiConnected()) {
    tft.drawString("WiFi未接続", 10, 140);
    Serial.println("WiFi not connected");
    delay(1500);
    return false;
  }

  bool ok = false;
  {
    auto client = createClientFor(cfg_rssUrl);
    HTTPClient http;
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // リダイレクト追跡を有効化

    if (!http.begin(*client, cfg_rssUrl)) {
      tft.drawString("URLが不正です", 10, 140);
      Serial.println("HTTP begin failed");
    } else {
      const int httpCode = http.GET();
      const int size = http.getSize();  // チャンク転送などで不明な場合は -1

      if (httpCode != HTTP_CODE_OK) {
        tft.drawString("エラー: " + String(httpCode), 10, 140);
        Serial.printf("HTTP error: %d\n", httpCode);
      } else if (size > MAX_RSS_BYTES) {
        tft.drawString("サイズ超過: " + String(size), 10, 140);
        Serial.printf("RSS too large: %d bytes\n", size);
      } else {
        const String payload = http.getString();
        std::vector<Article> fetched;
        String channelTitle;
        const int added = parseRSS(payload, fetched, channelTitle);

        if (added > 0) {
          articles.swap(fetched);
          if (channelTitle.length() > 0) feedTitle = channelTitle;
          ok = true;
          tft.drawString("取得完了: " + String(added) + "件", 10, 140);
        } else {
          tft.drawString("記事が見つかりません", 10, 140);
        }
        Serial.printf("RSS: %d bytes, %d articles\n", payload.length(), added);
      }
      http.end();
    }
  }

  delay(ok ? 500 : 1500);
  Serial.printf("Total articles: %d, free heap: %u\n", articles.size(), ESP.getFreeHeap());
  return ok;
}

// ================== 表示 ==================

void drawHeader() {
  tft.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, COLOR_HEADER_BG);

  // 右側: 記事番号
  const String counter = String(currentArticle + 1) + "/" + String(articles.size());
  tft.setFont(&fonts::Font0);
  tft.setTextColor(TFT_WHITE, COLOR_HEADER_BG);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(counter, SCREEN_WIDTH - 5, HEADER_HEIGHT / 2);
  const int counterWidth = tft.textWidth(counter);

  // 左側: フィード名 (RSS の channel/title。収まらなければ省略)
  tft.setFont(&fonts::efontJA_12);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(fitText(tft, feedTitle, SCREEN_WIDTH - 15 - counterWidth), 5, HEADER_HEIGHT / 2);
  tft.setTextDatum(TL_DATUM);
}

void drawFooter() {
  tft.fillRect(0, SCREEN_HEIGHT - FOOTER_HEIGHT, SCREEN_WIDTH, FOOTER_HEIGHT, TFT_NAVY);
  tft.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
  tft.setFont(&fonts::Font0);
  tft.setTextDatum(TL_DATUM);
  tft.setCursor(5, SCREEN_HEIGHT - FOOTER_HEIGHT + 5);
  if (maxScrollOffset > 0) {
    tft.print("Scroll | ");
  }
  tft.print("Update:");
  tft.print(UPDATE_INTERVAL / 60000);
  tft.print("min");
}

// 長押し中の進捗をフッターに表示
int lastProgressWidth = -1;
void drawLongPressProgress(unsigned long heldMs) {
  const int barMax = SCREEN_WIDTH - 10;
  const int w = min<unsigned long>(heldMs, LONG_PRESS_TIME) * barMax / LONG_PRESS_TIME;
  if (w == lastProgressWidth) return;

  if (lastProgressWidth < 0) {
    tft.fillRect(0, SCREEN_HEIGHT - FOOTER_HEIGHT, SCREEN_WIDTH, FOOTER_HEIGHT, TFT_NAVY);
    tft.drawRect(4, SCREEN_HEIGHT - FOOTER_HEIGHT + 3, barMax + 2, FOOTER_HEIGHT - 6, TFT_WHITE);
  }
  tft.fillRect(5, SCREEN_HEIGHT - FOOTER_HEIGHT + 4, w, FOOTER_HEIGHT - 8, TFT_GREEN);
  lastProgressWidth = w;
}

void drawTitle(const String& title) {
  tft.fillRect(0, HEADER_HEIGHT, SCREEN_WIDTH, TITLE_AREA_HEIGHT, COLOR_TITLE_BG);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_YELLOW, COLOR_TITLE_BG);
  tft.setTextDatum(TL_DATUM);

  const int maxWidth = SCREEN_WIDTH - 10;
  std::vector<TextLine> lines;
  const bool truncated = wrapText(tft, title, maxWidth, lines, TITLE_MAX_LINES);

  for (size_t i = 0; i < lines.size(); i++) {
    String line;
    if (truncated && i == lines.size() - 1) {
      // 最終行は残り全文を「…」付きで収める
      line = fitText(tft, title.substring(lines[i].start), maxWidth);
    } else {
      line = title.substring(lines[i].start, lines[i].end);
    }
    tft.drawString(line, 5, HEADER_HEIGHT + 5 + i * LINE_HEIGHT);
  }
}

// 説明文のうち、表示領域に入る行だけを描画する
// originY: 表示領域の上端の y 座標 (スプライトなら 0、LCD 直接なら DESC_VIEW_TOP)
template <typename GFX>
void drawDescriptionLines(GFX& gfx, const String& text, int originY) {
  gfx.setFont(&fonts::efontJA_12);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setTextDatum(TL_DATUM);

  const int first = max(0, (scrollOffset - TEXT_PADDING) / LINE_HEIGHT);
  for (int i = first; i < (int)descLines.size(); i++) {
    const int y = TEXT_PADDING + i * LINE_HEIGHT - scrollOffset;
    if (y >= DESC_VIEW_HEIGHT) break;
    gfx.drawString(text.substring(descLines[i].start, descLines[i].end), TEXT_PADDING, originY + y);
  }
}

void renderDescription() {
  if (articles.empty()) return;
  const String& desc = articles[currentArticle].description;

  if (descSpriteReady) {
    descSprite.fillSprite(TFT_BLACK);
    drawDescriptionLines(descSprite, desc, 0);
    descSprite.pushSprite(0, DESC_VIEW_TOP);
  } else {
    // スプライトを確保できなかった場合のフォールバック (多少ちらつく)
    tft.setClipRect(0, DESC_VIEW_TOP, SCREEN_WIDTH, DESC_VIEW_HEIGHT);
    tft.fillRect(0, DESC_VIEW_TOP, SCREEN_WIDTH, DESC_VIEW_HEIGHT, TFT_BLACK);
    drawDescriptionLines(tft, desc, DESC_VIEW_TOP);
    tft.clearClipRect();
  }
}

// 現在の記事を表示
void displayArticle() {
  if (articles.empty()) return;
  if (currentArticle >= (int)articles.size()) currentArticle = 0;

  const Article& article = articles[currentArticle];

  tft.fillScreen(TFT_BLACK);
  drawHeader();
  drawTitle(article.title);

  // 説明エリアの区切り線
  tft.drawFastHLine(0, DESC_AREA_TOP, SCREEN_WIDTH, TFT_DARKGREY);

  // 説明文を折り返し、スクロール範囲を計算
  // (幅の計測は描画と同じフォントで行う)
  tft.setFont(&fonts::efontJA_12);
  wrapText(tft, article.description, SCREEN_WIDTH - TEXT_PADDING * 2, descLines);
  const int contentHeight = descLines.size() * LINE_HEIGHT + TEXT_PADDING * 2;
  maxScrollOffset = max(0, contentHeight - DESC_VIEW_HEIGHT);

  // スクロール状態をリセット (先頭で一時停止してから下へ)
  scrollOffset = 0;
  scrollPaused = true;
  scrollPauseStart = millis();
  scrollDone = (maxScrollOffset == 0);

  renderDescription();
  drawFooter();
  lastProgressWidth = -1;
}

// スクロール更新 (下端まで行って一時停止したら scrollDone)
void updateScroll() {
  if (scrollDone || maxScrollOffset <= 0) return;

  const unsigned long now = millis();

  if (scrollPaused) {
    if (now - scrollPauseStart < SCROLL_PAUSE) return;
    scrollPaused = false;
    if (scrollOffset >= maxScrollOffset) {
      scrollDone = true;  // 下端での一時停止が終わった
      return;
    }
    lastScrollTime = now;
  }

  if (now - lastScrollTime >= SCROLL_INTERVAL) {
    scrollOffset += SCROLL_STEP;
    if (scrollOffset >= maxScrollOffset) {
      scrollOffset = maxScrollOffset;
      scrollPaused = true;
      scrollPauseStart = now;
    }
    renderDescription();
    lastScrollTime = now;
  }
}

// 次の記事へ
void nextArticle() {
  if (!articles.empty()) {
    currentArticle = (currentArticle + 1) % articles.size();
    displayArticle();
  }
}

void showFetchError() {
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setFont(&fonts::efontJA_16);
  tft.drawString("RSS取得失敗!", 120, 140);
  tft.setFont(&fonts::efontJA_12);
  tft.drawString(String(RETRY_INTERVAL / 1000) + "秒後に再試行します", 120, 175);
  tft.drawString("Boot長押しで設定モード", 120, 200);
  tft.setTextDatum(TL_DATUM);
}

// ================== ボタンチェック (起動直後) ==================
// 起動直後にボタンが押されていれば進捗バーを出し、3秒押し続けたら true
bool checkButtonAtBoot() {
  if (digitalRead(BUTTON_PIN) != LOW) return false;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setFont(&fonts::efontJA_12);
  tft.drawString("ボタンを押し続けて", 120, 140);
  tft.drawString("設定モードに入る...", 120, 165);
  tft.setTextDatum(TL_DATUM);

  const unsigned long pressStart = millis();
  int progress = 0;

  // プログレスバー描画
  tft.drawRect(40, 200, 160, 20, TFT_WHITE);

  while (digitalRead(BUTTON_PIN) == LOW) {
    const unsigned long elapsed = millis() - pressStart;
    const int newProgress = min<unsigned long>(elapsed, LONG_PRESS_TIME) * 150 / LONG_PRESS_TIME;

    if (newProgress > progress) {
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
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== RSS Reader ===");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  button.begin();

  // 設定を読み込み
  loadSettings();

  // ボタン長押しでAPモードに入る
  if (checkButtonAtBoot()) {
    Serial.println("Entering AP Mode...");
    startAPMode();
    return;  // setup終了、APモードのloop処理へ
  }

  // スプラッシュ画面
  tft.setTextColor(COLOR_HEADER_BG, TFT_BLACK);
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

  const bool connected = !waitWithButton(2000) && connectWiFi();

  if (apRequested) {
    Serial.println("Entering AP Mode...");
    startAPMode();
    return;
  }

  if (!connected) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setFont(&fonts::efontJA_16);
    tft.drawString("WiFiエラー!", 120, 140);
    tft.setFont(&fonts::efontJA_12);
    tft.drawString("Bootボタン長押しで", 120, 180);
    tft.drawString("設定モードに入れます", 120, 205);
    tft.drawString("(10秒後に再起動)", 120, 230);
    tft.setTextDatum(TL_DATUM);
    if (waitWithButton(10000)) {
      startAPMode();
      return;
    }
    ESP.restart();
  }

  // 説明文スプライトを確保 (ヒープに余裕がある起動直後に1回だけ)
  descSprite.setColorDepth(8);
  descSpriteReady = descSprite.createSprite(SCREEN_WIDTH, DESC_VIEW_HEIGHT) != nullptr;
  if (!descSpriteReady) {
    Serial.println("WARNING: descSprite allocation failed. Falling back to direct drawing.");
  }

  if (fetchRSS()) {
    displayArticle();
  } else {
    showFetchError();
  }

  lastUpdate = millis();
  lastArticleChange = millis();
  lastScrollTime = millis();
}

void loop() {
  // APモードの場合はWebサーバー処理
  if (apMode) {
    server.handleClient();
    delay(2);
    return;
  }

  // BOOT ボタン長押しで、動作中でも設定モードへ
  if (button.update()) {
    Serial.println("Entering AP Mode...");
    startAPMode();
    return;
  }
  if (!articles.empty()) {
    if (button.isPressed() && button.heldMs() >= LONG_PRESS_FEEDBACK) {
      drawLongPressProgress(button.heldMs());
    } else if (button.justReleased() && lastProgressWidth >= 0) {
      drawFooter();  // 途中で離したら元に戻す
      lastProgressWidth = -1;
    }
  }

  // スクロール更新
  updateScroll();

  // 記事切り替え (最短15秒。スクロールが必要な記事は最後まで表示してから)
  if (!articles.empty() && scrollDone && millis() - lastArticleChange >= ARTICLE_INTERVAL) {
    nextArticle();
    lastArticleChange = millis();
  }

  // 定期更新 (10分毎。記事が無いときは1分毎に再試行)
  const unsigned long interval = articles.empty() ? RETRY_INTERVAL : UPDATE_INTERVAL;
  if (millis() - lastUpdate >= interval) {
    if (fetchRSS()) {
      currentArticle = 0;
    }
    if (!articles.empty()) {
      displayArticle();  // 失敗時も前回の記事を表示し直す
    } else {
      showFetchError();
    }
    lastUpdate = millis();
    lastArticleChange = millis();
  }

  delay(10);  // スムーズなスクロールのため短い間隔
}
