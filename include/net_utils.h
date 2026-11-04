/**
 * HTTP/HTTPS 通信ユーティリティ
 *
 * secrets.h より後に include すること (ROOT_CA_PEM の有無で TLS 検証を切り替えるため)。
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <memory>

inline bool isHttpsUrl(const String& url) {
  return url.startsWith("https://");
}

inline bool isHttpUrl(const String& url) {
  return url.startsWith("http://") || isHttpsUrl(url);
}

/**
 * URL のスキームに合ったクライアントを生成する。
 * HTTPClient は渡したクライアントを参照し続けるので、
 * 戻り値は http.end() まで生存させること。
 *
 * https の場合:
 *   - secrets.h で ROOT_CA_PEM が定義されていれば、サーバー証明書を検証する
 *   - 未定義なら検証しない (中間者攻撃で内容を改ざんされうる)。その旨をシリアルに警告する
 */
inline std::unique_ptr<WiFiClient> createClientFor(const String& url) {
  if (!isHttpsUrl(url)) {
    return std::unique_ptr<WiFiClient>(new WiFiClient());
  }

  auto* secure = new WiFiClientSecure();
#ifdef ROOT_CA_PEM
  secure->setCACert(ROOT_CA_PEM);
#else
  static bool warned = false;
  if (!warned) {
    Serial.println("WARNING: ROOT_CA_PEM is not defined. TLS certificate is NOT verified.");
    warned = true;
  }
  secure->setInsecure();
#endif
  return std::unique_ptr<WiFiClient>(secure);
}

/**
 * WiFi が切れていたら再接続を試み、接続できたかを返す。
 */
inline bool ensureWiFiConnected(unsigned long timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("WiFi disconnected. Reconnecting...");
  WiFi.reconnect();
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}
