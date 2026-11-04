/**
 * ESP32-C3 + 2.2インチLCD BLE センサーモニター
 *
 * 使用LCD: ILI9341 2.2インチ (240x320 縦向き)
 * 使用ボード: Seeed XIAO ESP32C3
 * 使用ライブラリ: LovyanGFX, NimBLE-Arduino
 *
 * 機能:
 * - BLE のアドバタイズをパッシブスキャンし、BTHome v2 形式のセンサーデータを受信
 *   (接続はしないので、台数の上限は MAX_DEVICES の表のサイズだけ)
 * - 温度 / ホールセンサー(開閉・オンオフ) / 加速度 をカード形式で表示 (1画面 2x3 = 6台)
 * - 加速度の変化 (または motion フラグ) を検知するとカードの枠をオレンジに、
 *   オンオフが変化するとシアンに光らせる。一定時間受信が無いとグレー表示
 * - BOOTボタン: 短押しで次のページ、3秒長押しで一覧をクリア
 *
 * 送信側のサンプルは src/ble_sensor_node.cpp (pio run -e ble_node)。
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <math.h>
#include <string.h>

#include "bthome.h"
#include "lgfx_config.h"
#include "long_press_button.h"
#include "text_utils.h"

// ================== 設定 ==================
#define BUTTON_PIN 9
const unsigned long LONG_PRESS_TIME = 3000;  // 長押しで一覧クリア

const int MAX_DEVICES = 12;                  // 記憶するデバイス数 (超えたら最も古いものを置き換え)
const unsigned long STALE_MS = 30000;        // これ以上受信が無ければオフライン表示
const unsigned long HIGHLIGHT_MS = 3000;     // 変化を検知してから枠を光らせる時間
const unsigned long DUPLICATE_WINDOW_MS = 4000;  // 同じ packet id をこの時間内に受けたら重複とみなす
const float ACCEL_CHANGE_THRESHOLD = 0.5f;   // 前回からの加速度変化 (m/s²) がこれを超えたら「動いた」

const unsigned long REDRAW_INTERVAL = 500;   // 経過時間表示などの定期再描画
const unsigned long MIN_REDRAW_INTERVAL = 100;  // 受信時の再描画の最短間隔
const unsigned long PAGE_INTERVAL = 8000;    // 複数ページあるときの自動切り替え

// 画面レイアウト (240x320 縦)
const int SCREEN_W = 240;
const int SCREEN_H = 320;
const int HEADER_H = 20;
const int CARD_COLS = 2;
const int CARD_ROWS = 3;
const int CARDS_PER_PAGE = CARD_COLS * CARD_ROWS;
const int CARD_W = 116;
const int CARD_H = 96;
const int CARD_GAP_X = 2;
const int CARD_GAP_Y = 4;
const int CARD_LEFT = (SCREEN_W - (CARD_W * CARD_COLS + CARD_GAP_X * (CARD_COLS - 1))) / 2;
const int CARD_TOP = HEADER_H + 3;

const int HISTORY_LEN = 36;  // 加速度グラフの点数

// 色
const uint16_t COLOR_HEADER_BG = lgfx::color565(20, 40, 90);
const uint16_t COLOR_CARD_BG = lgfx::color565(24, 28, 40);
const uint16_t COLOR_BORDER = lgfx::color565(70, 75, 90);
const uint16_t COLOR_STALE = lgfx::color565(90, 90, 90);
const uint16_t COLOR_GRAPH = lgfx::color565(80, 200, 255);

// ================== デバイス管理 ==================
struct SensorDevice {
  bool used = false;
  uint8_t addr[6] = {};  // NimBLE のネイティブ順 (addr[0] が表示上の末尾バイト)
  char name[24] = {};
  int rssi = -127;
  bthome::Reading reading;  // 最新値 (項目ごとにマージ)
  unsigned long firstSeenMs = 0;
  unsigned long lastSeenMs = 0;    // 最後に広告を受けた時刻 (重複含む)
  unsigned long lastUpdateMs = 0;  // 最後に新しいパケットを受けた時刻
  bool havePacketId = false;
  uint8_t lastPacketId = 0;
  float history[HISTORY_LEN] = {};
  uint8_t historyLen = 0;
  uint8_t historyHead = 0;  // 次に書き込む位置
  unsigned long lastMotionMs = 0;
  unsigned long lastBinaryChangeMs = 0;
  uint32_t packets = 0;
};

SensorDevice devices[MAX_DEVICES];
SemaphoreHandle_t devicesMutex = nullptr;
volatile bool devicesChanged = false;

volatile uint32_t statEncrypted = 0;
volatile uint32_t statMalformed = 0;

struct DevicesLock {
  DevicesLock() { xSemaphoreTake(devicesMutex, portMAX_DELAY); }
  ~DevicesLock() { xSemaphoreGive(devicesMutex); }
};

// 受信値を項目ごとにマージ (送られてこなかった項目は前回値を残す)
void mergeReading(bthome::Reading& dst, const bthome::Reading& src) {
  dst.version = src.version;
  dst.triggerBased = src.triggerBased;
  if (src.hasPacketId) { dst.hasPacketId = true; dst.packetId = src.packetId; }
  if (src.hasBattery) { dst.hasBattery = true; dst.battery = src.battery; }
  if (src.hasTemperature) { dst.hasTemperature = true; dst.temperature = src.temperature; }
  if (src.hasHumidity) { dst.hasHumidity = true; dst.humidity = src.humidity; }
  if (src.hasVoltage) { dst.hasVoltage = true; dst.voltage = src.voltage; }
  if (src.hasBinary) { dst.hasBinary = true; dst.binary = src.binary; dst.binaryId = src.binaryId; }
  if (src.hasMotion) { dst.hasMotion = true; dst.motion = src.motion; }
  if (src.hasAcceleration) { dst.hasAcceleration = true; dst.acceleration = src.acceleration; }
}

// UTF-8 の文字の途中で切らないように、dst (終端含め size バイト) へコピーする
void copyUtf8Truncated(char* dst, size_t size, const char* src) {
  size_t n = 0;
  while (src[n] != '\0') {
    const size_t charLen = utf8CharLen(static_cast<uint8_t>(src[n]));
    if (n + charLen > size - 1) break;
    size_t k = 1;
    while (k < charLen && src[n + k] != '\0') k++;
    if (k < charLen) break;  // 末尾が壊れた文字は捨てる
    n += charLen;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// devicesMutex を取得した状態で呼ぶこと
SensorDevice* findOrAllocateDevice(const uint8_t* addr, unsigned long now) {
  SensorDevice* freeSlot = nullptr;
  SensorDevice* oldest = nullptr;
  for (auto& d : devices) {
    if (d.used) {
      if (memcmp(d.addr, addr, 6) == 0) return &d;
      if (!oldest || d.lastSeenMs < oldest->lastSeenMs) oldest = &d;
    } else if (!freeSlot) {
      freeSlot = &d;
    }
  }
  SensorDevice* slot = freeSlot ? freeSlot : oldest;
  *slot = SensorDevice();
  slot->used = true;
  memcpy(slot->addr, addr, 6);
  slot->firstSeenMs = now;
  return slot;
}

// devicesMutex を取得した状態で呼ぶこと
void applyAdvertisement(const uint8_t* addr, const std::string& advName, int rssi,
                        const bthome::Reading& r, unsigned long now) {
  SensorDevice* d = findOrAllocateDevice(addr, now);
  const bool isNew = d->packets == 0;

  d->rssi = rssi;
  d->lastSeenMs = now;
  if (!advName.empty()) {
    copyUtf8Truncated(d->name, sizeof(d->name), advName.c_str());
  } else if (d->name[0] == '\0') {
    // 名前が無ければアドレス末尾 2 バイトで識別
    snprintf(d->name, sizeof(d->name), "BLE-%02X%02X", addr[1], addr[0]);
  }

  // 同じ packet id の再送 (BTHome は同じ内容を何度も広告する) は値の更新をしない
  if (r.hasPacketId && d->havePacketId && r.packetId == d->lastPacketId &&
      now - d->lastUpdateMs < DUPLICATE_WINDOW_MS) {
    return;
  }
  if (r.hasPacketId) {
    d->havePacketId = true;
    d->lastPacketId = r.packetId;
  }

  // 変化の検知
  if (r.hasBinary && d->reading.hasBinary && r.binary != d->reading.binary) {
    d->lastBinaryChangeMs = now;
  }
  if (r.hasMotion && r.motion) {
    d->lastMotionMs = now;
  }
  if (r.hasAcceleration) {
    if (d->reading.hasAcceleration &&
        fabsf(r.acceleration - d->reading.acceleration) > ACCEL_CHANGE_THRESHOLD) {
      d->lastMotionMs = now;
    }
    d->history[d->historyHead] = r.acceleration;
    d->historyHead = (d->historyHead + 1) % HISTORY_LEN;
    if (d->historyLen < HISTORY_LEN) d->historyLen++;
  }

  mergeReading(d->reading, r);
  d->lastUpdateMs = now;
  d->packets++;
  devicesChanged = true;

  if (isNew) {
    Serial.printf("New device: %s (%02X:%02X:%02X:%02X:%02X:%02X) RSSI %d\n", d->name,
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], rssi);
  }
}

// ================== BLE スキャン ==================
class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  // NimBLE のホストタスクから呼ばれる。重い処理 (描画) はここでしない
  void onResult(NimBLEAdvertisedDevice* adv) override {
    if (!adv->haveServiceData()) return;

    const std::string sd = adv->getServiceData(NimBLEUUID((uint16_t)bthome::SERVICE_UUID));
    if (sd.empty()) return;

    bthome::Reading r;
    const auto result = bthome::parse(reinterpret_cast<const uint8_t*>(sd.data()), sd.size(), r);
    if (result == bthome::ParseResult::Encrypted) {
      statEncrypted = statEncrypted + 1;
      return;
    }
    if (result != bthome::ParseResult::Ok) {
      statMalformed = statMalformed + 1;
      return;
    }

    const std::string name = adv->haveName() ? adv->getName() : std::string();
    const int rssi = adv->getRSSI();
    const uint8_t* addr = adv->getAddress().getNative();
    const unsigned long now = millis();

    DevicesLock lock;
    applyAdvertisement(addr, name, rssi, r, now);
  }
};

ScanCallbacks scanCallbacks;
NimBLEScan* scan = nullptr;

void startScan() {
  NimBLEDevice::init("");
  scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);  // 同じデバイスの広告も毎回受け取る
  scan->setDuplicateFilter(false);
  scan->setMaxResults(0);       // 結果を内部に溜めない (連続スキャンでメモリが増え続けるのを防ぐ)
  scan->setActiveScan(false);   // パッシブスキャン (スキャン応答を要求しない)
  scan->setInterval(100);
  scan->setWindow(90);
  scan->start(0, nullptr, false);  // 0 = 無期限
  Serial.println("BLE scan started");
}

// ================== 表示 ==================
LGFX tft;
LGFX_Sprite card(&tft);  // カード 1 枚分を描いてから転送する (ちらつき防止)
bool cardSpriteReady = false;

LongPressButton button(BUTTON_PIN, LONG_PRESS_TIME);

int currentPage = 0;
unsigned long lastPageChange = 0;
unsigned long lastRedraw = 0;
int lastPageCount = 1;
int lastRenderedPage = -1;  // 直前にカードを描いたページ (-1 = 未描画/空表示)

String formatAge(unsigned long ms) {
  if (ms < 1000) return "now";
  if (ms < 60000) return String(ms / 1000) + "s";
  if (ms < 3600000) return String(ms / 60000) + "m";
  return String(ms / 3600000) + "h";
}

template <typename GFX>
void drawRssiBars(GFX& g, int x, int y, int rssi, uint16_t onColor) {
  const int level = rssi > -60 ? 4 : rssi > -70 ? 3 : rssi > -80 ? 2 : rssi > -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    const int h = 3 * (i + 1);
    const uint16_t c = i < level ? onColor : COLOR_BORDER;
    g.fillRect(x + i * 5, y + 12 - h, 4, h, c);
  }
}

// 加速度の推移を折れ線で描く (縦軸は自動スケール、最小幅 1 m/s²)
template <typename GFX>
void drawSparkline(GFX& g, const SensorDevice& d, int x, int y, int w, int h, uint16_t color) {
  g.drawRect(x, y, w, h, COLOR_BORDER);
  if (d.historyLen < 2) return;

  const int start = (d.historyHead + HISTORY_LEN - d.historyLen) % HISTORY_LEN;
  float lo = d.history[start];
  float hi = lo;
  for (int i = 1; i < d.historyLen; i++) {
    const float v = d.history[(start + i) % HISTORY_LEN];
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  float span = hi - lo;
  if (span < 1.0f) {
    const float mid = (hi + lo) / 2;
    lo = mid - 0.5f;
    span = 1.0f;
  }

  const int innerH = h - 3;
  int prevX = 0;
  int prevY = 0;
  for (int i = 0; i < d.historyLen; i++) {
    const float v = d.history[(start + i) % HISTORY_LEN];
    const int px = x + 1 + (w - 3) * i / (HISTORY_LEN - 1);
    const int py = y + 1 + innerH - (int)((v - lo) / span * innerH + 0.5f);
    if (i > 0) g.drawLine(prevX, prevY, px, py, color);
    prevX = px;
    prevY = py;
  }
}

const char* binaryLabel(uint8_t binaryId) {
  switch (binaryId) {
    case bthome::id::DOOR: return "ドア";
    case bthome::id::WINDOW: return "窓";
    default: return "磁気";
  }
}

const char* binaryValueText(uint8_t binaryId, bool on) {
  if (binaryId == bthome::id::DOOR || binaryId == bthome::id::WINDOW) return on ? "開" : "閉";
  return on ? "ON" : "OFF";
}

// カード 1 枚を描画 (g はカードサイズのスプライト、または LCD に原点をずらして描く)
template <typename GFX>
void drawCard(GFX& g, int ox, int oy, const SensorDevice& d, unsigned long now) {
  const bool stale = now - d.lastSeenMs > STALE_MS;
  const bool moving = !stale && d.lastMotionMs != 0 && now - d.lastMotionMs < HIGHLIGHT_MS;
  const bool switched = !stale && d.lastBinaryChangeMs != 0 && now - d.lastBinaryChangeMs < HIGHLIGHT_MS;
  const uint16_t fg = stale ? COLOR_STALE : TFT_WHITE;
  const uint16_t border = stale ? COLOR_STALE : moving ? TFT_ORANGE : switched ? TFT_CYAN : COLOR_BORDER;
  const bthome::Reading& r = d.reading;
  char buf[24];

  g.fillRect(ox, oy, CARD_W, CARD_H, TFT_BLACK);
  g.fillRoundRect(ox, oy, CARD_W, CARD_H, 6, COLOR_CARD_BG);
  g.drawRoundRect(ox, oy, CARD_W, CARD_H, 6, border);
  if (moving || switched) g.drawRoundRect(ox + 1, oy + 1, CARD_W - 2, CARD_H - 2, 5, border);

  // --- 1行目: 名前 + 電波強度 ---
  g.setTextDatum(TL_DATUM);
  g.setFont(&fonts::efontJA_12);
  g.setTextColor(fg, COLOR_CARD_BG);
  g.drawString(fitText(g, String(d.name), CARD_W - 32), ox + 5, oy + 4);
  drawRssiBars(g, ox + CARD_W - 25, oy + 3, d.rssi, stale ? COLOR_STALE : TFT_GREEN);

  // --- 2行目: 温度 (大) + 湿度・電池 ---
  g.setFont(&fonts::Font4);
  g.setTextColor(stale ? COLOR_STALE : TFT_YELLOW, COLOR_CARD_BG);
  if (r.hasTemperature) {
    snprintf(buf, sizeof(buf), "%.1f", r.temperature);
  } else {
    strcpy(buf, "--.-");
  }
  g.drawString(buf, ox + 5, oy + 19);
  const int unitX = ox + 6 + g.textWidth(buf);
  g.setFont(&fonts::efontJA_12);
  g.drawString("℃", unitX, oy + 22);

  g.setTextDatum(TR_DATUM);
  g.setTextColor(fg, COLOR_CARD_BG);
  if (r.hasHumidity) {
    snprintf(buf, sizeof(buf), "%.0f%%", r.humidity);
    g.drawString(buf, ox + CARD_W - 5, oy + 20);
  }
  g.setFont(&fonts::Font0);
  if (r.hasBattery) {
    snprintf(buf, sizeof(buf), "BAT%u%%", r.battery);
    g.setTextColor(r.battery <= 20 && !stale ? TFT_RED : fg, COLOR_CARD_BG);
    g.drawString(buf, ox + CARD_W - 5, oy + 36);
  } else if (r.hasVoltage) {
    snprintf(buf, sizeof(buf), "%.2fV", r.voltage);
    g.drawString(buf, ox + CARD_W - 5, oy + 36);
  }

  // --- 3行目: ホールセンサー (オンオフ) + 経過時間 ---
  g.setTextDatum(TL_DATUM);
  g.setFont(&fonts::efontJA_12);
  g.setTextColor(fg, COLOR_CARD_BG);
  g.drawString(binaryLabel(r.binaryId), ox + 5, oy + 50);
  const int pillX = ox + 33;
  const int pillY = oy + 48;
  if (r.hasBinary) {
    const uint16_t pillBg = stale ? COLOR_BORDER : r.binary ? TFT_GREEN : COLOR_BORDER;
    const uint16_t pillFg = r.binary && !stale ? TFT_BLACK : fg;
    g.fillRoundRect(pillX, pillY, 36, 16, 8, pillBg);
    g.setTextDatum(MC_DATUM);
    g.setTextColor(pillFg, pillBg);
    g.drawString(binaryValueText(r.binaryId, r.binary), pillX + 18, pillY + 8);
  } else {
    g.drawString("--", pillX + 4, oy + 50);
  }

  g.setFont(&fonts::Font0);
  g.setTextDatum(TR_DATUM);
  // 経過時間 (オフライン時は赤)
  g.setTextColor(stale ? TFT_RED : TFT_LIGHTGREY, COLOR_CARD_BG);
  g.drawString(formatAge(now - d.lastUpdateMs).c_str(), ox + CARD_W - 5, oy + 53);

  // --- 4行目: 加速度 + グラフ ---
  g.setTextDatum(TL_DATUM);
  g.setTextColor(fg, COLOR_CARD_BG);
  if (r.hasAcceleration) {
    snprintf(buf, sizeof(buf), "ACC %.2f m/s2", r.acceleration);
  } else {
    strcpy(buf, "ACC --");
  }
  g.drawString(buf, ox + 5, oy + 68);
  if (moving) {
    g.setTextDatum(TR_DATUM);
    g.setTextColor(TFT_ORANGE, COLOR_CARD_BG);
    g.drawString("MOVE", ox + CARD_W - 5, oy + 68);
  }
  if (r.hasAcceleration) {
    drawSparkline(g, d, ox + 5, oy + 77, CARD_W - 10, 15, stale ? COLOR_STALE : COLOR_GRAPH);
  }
}

void drawHeader(int deviceCount, int pageCount) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COLOR_HEADER_BG);
  tft.setTextColor(TFT_WHITE, COLOR_HEADER_BG);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("BLE センサー", 5, HEADER_H / 2);

  String right = String(deviceCount) + "台";
  if (pageCount > 1) right += "  " + String(currentPage + 1) + "/" + String(pageCount);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(right, SCREEN_W - 5, HEADER_H / 2);
  tft.setTextDatum(TL_DATUM);
}

void drawEmptyState(unsigned long now) {
  tft.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setFont(&fonts::efontJA_16);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  const int dots = (now / 500) % 4;
  String msg = "センサーを探しています";
  for (int i = 0; i < dots; i++) msg += ".";
  tft.drawString(msg, SCREEN_W / 2, 130);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("BTHome v2 (非暗号化) の", SCREEN_W / 2, 175);
  tft.drawString("アドバタイズを受信します", SCREEN_W / 2, 195);
  if (statEncrypted > 0) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("暗号化パケットは未対応です", SCREEN_W / 2, 230);
  }
  tft.setTextDatum(TL_DATUM);
}

// 共有テーブルのコピーを取り、名前順に並べて描画する
void render() {
  static SensorDevice snapshot[MAX_DEVICES];
  int count = 0;
  {
    DevicesLock lock;
    for (const auto& d : devices) {
      if (d.used) snapshot[count++] = d;
    }
    devicesChanged = false;
  }
  // 時刻はコピーの後で取る (先に取ると、直後に受信した lastSeenMs の方が新しくなり
  // unsigned の引き算がアンダーフローしてオフライン (グレー) と誤表示される)
  const unsigned long now = millis();
  std::sort(snapshot, snapshot + count, [](const SensorDevice& a, const SensorDevice& b) {
    const int c = strcmp(a.name, b.name);
    return c != 0 ? c < 0 : memcmp(a.addr, b.addr, 6) < 0;
  });

  const int pageCount = std::max(1, (count + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE);
  if (currentPage >= pageCount) currentPage = 0;
  lastPageCount = pageCount;
  drawHeader(count, pageCount);

  if (count == 0) {
    drawEmptyState(now);
    lastRenderedPage = -1;
    return;
  }

  // 「探しています」画面からの切り替え時やページ切り替え時は、カードの隙間も含めて消す
  if (lastRenderedPage != currentPage) {
    tft.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, TFT_BLACK);
    lastRenderedPage = currentPage;
  }

  for (int slot = 0; slot < CARDS_PER_PAGE; slot++) {
    const int x = CARD_LEFT + (slot % CARD_COLS) * (CARD_W + CARD_GAP_X);
    const int y = CARD_TOP + (slot / CARD_COLS) * (CARD_H + CARD_GAP_Y);
    const int index = currentPage * CARDS_PER_PAGE + slot;

    if (index >= count) {
      tft.fillRect(x, y, CARD_W, CARD_H, TFT_BLACK);
      continue;
    }
    if (cardSpriteReady) {
      drawCard(card, 0, 0, snapshot[index], now);
      card.pushSprite(x, y);
    } else {
      drawCard(tft, x, y, snapshot[index], now);  // スプライト確保失敗時 (ちらつく)
    }
  }
}

void clearDevices() {
  DevicesLock lock;
  for (auto& d : devices) d = SensorDevice();
  devicesChanged = true;
}

void showToast(const char* message) {
  tft.fillRoundRect(30, 140, 180, 40, 8, TFT_ORANGE);
  tft.setTextColor(TFT_BLACK, TFT_ORANGE);
  tft.setFont(&fonts::efontJA_16);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(message, SCREEN_W / 2, 160);
  tft.setTextDatum(TL_DATUM);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== BLE Sensor Monitor ===");

  devicesMutex = xSemaphoreCreateMutex();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  button.begin();

  // スプラッシュ
  tft.setTextDatum(MC_DATUM);
  tft.setFont(&fonts::efontJA_16);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("BLE", 120, 100);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Sensor Monitor", 120, 145);
  tft.setFont(&fonts::efontJA_12);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("短押し:ページ  長押し:クリア", 120, 190);
  tft.setTextDatum(TL_DATUM);

  card.setColorDepth(16);
  cardSpriteReady = card.createSprite(CARD_W, CARD_H) != nullptr;  // 約 22KB
  if (!cardSpriteReady) Serial.println("WARNING: card sprite allocation failed");

  startScan();
  delay(1500);

  tft.fillScreen(TFT_BLACK);
  lastPageChange = millis();
  render();
  lastRedraw = millis();
}

void loop() {
  const unsigned long now = millis();

  button.update();
  if (button.consumeLongPress()) {
    clearDevices();
    currentPage = 0;
    showToast("一覧をクリア");
    delay(800);
    tft.fillScreen(TFT_BLACK);
    lastRenderedPage = -1;
    render();
    lastRedraw = millis();
    return;
  }
  if (button.wasShortPress() && lastPageCount > 1) {
    currentPage = (currentPage + 1) % lastPageCount;
    lastPageChange = now;
    render();
    lastRedraw = now;
  }

  // 複数ページあるときは自動で切り替え
  if (lastPageCount > 1 && now - lastPageChange >= PAGE_INTERVAL) {
    currentPage = (currentPage + 1) % lastPageCount;
    lastPageChange = now;
    lastRedraw = 0;  // すぐ描き直す
  }

  const bool due = now - lastRedraw >= REDRAW_INTERVAL;
  const bool changed = devicesChanged && now - lastRedraw >= MIN_REDRAW_INTERVAL;
  if (due || changed) {
    render();
    lastRedraw = now;
  }

  // 何らかの理由でスキャンが止まっていたら再開
  static unsigned long lastScanCheck = 0;
  if (now - lastScanCheck >= 5000) {
    lastScanCheck = now;
    if (scan && !scan->isScanning()) {
      Serial.println("Scan stopped. Restarting...");
      scan->start(0, nullptr, false);
    }
  }

  delay(10);
}
