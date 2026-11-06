/*
  TALLY LIGHT OBS - ESP32 + WS2812
  - Conecta no Wi-Fi configurado e no OBS WebSocket (v5, OBS 28+)
  - Acende VERMELHO quando a cena configurada esta NO AR (Program)
  - Pagina de configuracao pelo Wi-Fi do proprio ESP32

  Bibliotecas (Gerenciador de Bibliotecas):
    - WebSockets   (Markus Sattler / links2004)
    - ArduinoJson  (Benoit Blanchon) versao 7.x
    - Adafruit NeoPixel
  Placa: "ESP32 Dev Module" (pacote esp32 da Espressif)

  Configuracao: conecte no Wi-Fi  "Tally-Config"  senha  "tally1234"
                e abra  http://192.168.4.1
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

// ---------------- Hardware ----------------
#define LED_PIN   5      // GPIO do DIN da fita WS2812
#define AP_SSID   "Tally-Config"
#define AP_PASS   "tally1234"   // minimo 8 caracteres

// ---------------- Configuracao ----------------
struct Config {
  String ssid, pass, obsIp, obsPass, scenes;
  int obsPort = 4455;
  int numLeds = 8;
  int brightness = 120;     // 1..255
} cfg;

Preferences prefs;
WebServer server(80);
WebSocketsClient ws;
Adafruit_NeoPixel strip(8, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------------- Estado ----------------
bool obsConnected = false;
bool isLive = false;       // cena em PROGRAM (no ar)
bool isPreview = false;    // cena em PREVIEW (proxima, Studio Mode)
bool wsStarted = false;
String currentScene = "";
String previewScene = "";

void loadConfig() {
  prefs.begin("tally", true);
  cfg.ssid       = prefs.getString("ssid", "");
  cfg.pass       = prefs.getString("pass", "");
  cfg.obsIp      = prefs.getString("obsIp", "");
  cfg.obsPass    = prefs.getString("obsPass", "");
  cfg.scenes     = prefs.getString("scenes", "Camera 1");
  cfg.obsPort    = prefs.getInt("obsPort", 4455);
  cfg.numLeds    = prefs.getInt("numLeds", 8);
  cfg.brightness = prefs.getInt("bright", 120);
  prefs.end();
}

void saveConfig() {
  prefs.begin("tally", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("obsIp", cfg.obsIp);
  prefs.putString("obsPass", cfg.obsPass);
  prefs.putString("scenes", cfg.scenes);
  prefs.putInt("obsPort", cfg.obsPort);
  prefs.putInt("numLeds", cfg.numLeds);
  prefs.putInt("bright", cfg.brightness);
  prefs.end();
}

// ---------------- Cena ativa? ----------------
// Aceita varias cenas separadas por ponto e virgula: "Camera 1;Camera 1 + Slides"
bool matchScene(const String &name) {
  String list = cfg.scenes;
  int start = 0;
  while (start <= (int)list.length()) {
    int end = list.indexOf(';', start);
    if (end < 0) end = list.length();
    String item = list.substring(start, end);
    item.trim();
    if (item.length() && item.equalsIgnoreCase(name)) return true;
    start = end + 1;
  }
  return false;
}

void setScene(const String &name) {
  currentScene = name;
  isLive = matchScene(name);
  Serial.printf("Cena no ar: %s -> %s\n", name.c_str(), isLive ? "TALLY ON" : "off");
}

void setPreview(const String &name) {
  previewScene = name;
  isPreview = name.length() > 0 && matchScene(name);
  Serial.printf("Cena em preview: %s -> %s\n", name.c_str(), isPreview ? "VERDE" : "off");
}

// ---------------- Autenticacao OBS (SHA256 + Base64) ----------------
String sha256b64(const String &in) {
  unsigned char hash[32];
  mbedtls_sha256((const unsigned char *)in.c_str(), in.length(), hash, 0);
  unsigned char out[64];
  size_t olen = 0;
  mbedtls_base64_encode(out, sizeof(out), &olen, hash, 32);
  return String((char *)out).substring(0, olen);
}

String makeAuth(const String &password, const String &salt, const String &challenge) {
  String secret = sha256b64(password + salt);
  return sha256b64(secret + challenge);
}

// ---------------- WebSocket OBS ----------------
void sendJson(JsonDocument &doc) {
  String s;
  serializeJson(doc, s);
  ws.sendTXT(s);
}

void sendIdentify(JsonObject hello) {
  JsonDocument doc;
  doc["op"] = 1;
  JsonObject d = doc["d"].to<JsonObject>();
  d["rpcVersion"] = 1;
  d["eventSubscriptions"] = 4 | 1024;  // 4 = eventos de Cenas, 1024 = eventos de UI (Studio Mode)
  if (hello["authentication"].is<JsonObject>()) {
    String salt = hello["authentication"]["salt"].as<String>();
    String chal = hello["authentication"]["challenge"].as<String>();
    d["authentication"] = makeAuth(cfg.obsPass, salt, chal);
  }
  sendJson(doc);
}

void requestCurrentScene() {
  JsonDocument doc;
  doc["op"] = 6;
  JsonObject d = doc["d"].to<JsonObject>();
  d["requestType"] = "GetCurrentProgramScene";
  d["requestId"] = "init";
  sendJson(doc);
}

// Falha (sem Studio Mode) e tratada na resposta: preview fica vazio
void requestPreviewScene() {
  JsonDocument doc;
  doc["op"] = 6;
  JsonObject d = doc["d"].to<JsonObject>();
  d["requestType"] = "GetCurrentPreviewScene";
  d["requestId"] = "prev";
  sendJson(doc);
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("OBS desconectado");
      obsConnected = false;
      isLive = false;
      isPreview = false;
      break;
    case WStype_CONNECTED:
      Serial.println("WebSocket aberto, aguardando Hello...");
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) return;
      int op = doc["op"] | -1;
      JsonObject d = doc["d"].as<JsonObject>();

      if (op == 0) {                       // Hello
        sendIdentify(d);
      } else if (op == 2) {                // Identified
        Serial.println("OBS autenticado");
        obsConnected = true;
        requestCurrentScene();
        requestPreviewScene();
      } else if (op == 7) {                // Resposta de requisicao
        if (d["requestType"] == "GetCurrentProgramScene" && d["requestStatus"]["result"] == true) {
          String n = d["responseData"]["currentProgramSceneName"] | "";
          if (n == "") n = d["responseData"]["sceneName"] | "";
          setScene(n);
        } else if (d["requestType"] == "GetCurrentPreviewScene") {
          if (d["requestStatus"]["result"] == true) {
            String n = d["responseData"]["currentPreviewSceneName"] | "";
            if (n == "") n = d["responseData"]["sceneName"] | "";
            setPreview(n);
          } else {
            setPreview("");              // Studio Mode desligado
          }
        }
      } else if (op == 5) {                // Evento
        String ev = d["eventType"] | "";
        if (ev == "CurrentProgramSceneChanged") {
          setScene(d["eventData"]["sceneName"].as<String>());
        } else if (ev == "CurrentPreviewSceneChanged") {
          setPreview(d["eventData"]["sceneName"].as<String>());
        } else if (ev == "StudioModeStateChanged") {
          if (d["eventData"]["studioModeEnabled"] == true) requestPreviewScene();
          else setPreview("");
        }
      }
      break;
    }
    default: break;
  }
}

void startObs() {
  if (wsStarted || cfg.obsIp.length() == 0) return;
  ws.begin(cfg.obsIp, cfg.obsPort, "/");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 3000, 2);
  wsStarted = true;
  Serial.printf("Conectando ao OBS %s:%d\n", cfg.obsIp.c_str(), cfg.obsPort);
}

// ---------------- LEDs ----------------
void fillStrip(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = strip.Color(r, g, b);
  for (int i = 0; i < strip.numPixels(); i++) strip.setPixelColor(i, c);
  strip.show();
}

// Acende somente o primeiro LED (os demais apagados)
void showOne(uint8_t r, uint8_t g, uint8_t b) {
  strip.clear();
  strip.setPixelColor(0, strip.Color(r, g, b));
  strip.show();
}

// Pisca a fita inteira 3 vezes
void blink3(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < 3; i++) {
    fillStrip(r, g, b);
    delay(250);
    fillStrip(0, 0, 0);
    delay(250);
  }
}

void updateLeds() {
  static uint32_t last = 0;
  if (millis() - last < 100) return;
  last = millis();
  bool blink = (millis() / 500) % 2;

  if (WiFi.status() != WL_CONNECTED)  showOne(blink ? 80 : 0, blink ? 40 : 0, 0);  // 1 LED amarelo piscando: sem Wi-Fi
  else if (!obsConnected)             showOne(40, 0, 0);                            // 1 LED vermelho fraco: ligado, OBS nao encontrado
  else if (isLive)                    fillStrip(255, 0, 0);                         // TODOS vermelhos: camera NO AR
  else if (isPreview)                 fillStrip(0, 255, 0);                         // TODOS verdes: PROXIMA camera (preview)
  else                                showOne(80, 80, 80);                        // 1 LED branco: OBS encontrado, camera fora do ar
}

// ---------------- Pagina web ----------------
String esc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

void handleStatus() {
  JsonDocument doc;
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  doc["ip"] = WiFi.localIP().toString();
  doc["obs"] = obsConnected;
  doc["scene"] = currentScene;
  doc["live"] = isLive;
  doc["preview"] = previewScene;
  doc["isPreview"] = isPreview;
  String s;
  serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleRoot() {
  String h = F("<!DOCTYPE html><html lang='pt-br'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Tally Light</title><style>"
               "body{font-family:sans-serif;background:#111;color:#eee;max-width:480px;margin:0 auto;padding:16px}"
               "h1{font-size:22px}label{display:block;margin-top:14px;font-size:14px;color:#aaa}"
               "input[type=text],input[type=password],input[type=number]{width:100%;padding:10px;margin-top:4px;"
               "border-radius:6px;border:1px solid #444;background:#222;color:#fff;box-sizing:border-box;font-size:16px}"
               "button{margin-top:22px;width:100%;padding:14px;font-size:17px;border:0;border-radius:8px;background:#c62828;color:#fff}"
               ".st{background:#222;border-radius:8px;padding:10px;margin:12px 0;font-size:14px}"
               "small{color:#888}</style></head><body><h1>&#128308; Tally Light OBS</h1>"
               "<div class='st' id='st'>Carregando status...</div><form method='POST' action='/save'>");

  h += "<label>Wi-Fi (SSID)<input type='text' name='ssid' value='" + esc(cfg.ssid) + "'></label>";
  h += "<label>Senha do Wi-Fi<input type='password' name='pass' value='" + esc(cfg.pass) + "'></label>";
  h += "<label>IP do computador com OBS<input type='text' name='obsIp' placeholder='192.168.0.10' value='" + esc(cfg.obsIp) + "'></label>";
  h += "<label>Porta do OBS WebSocket<input type='number' name='obsPort' value='" + String(cfg.obsPort) + "'></label>";
  h += "<label>Senha do OBS WebSocket (vazio se nao houver)<input type='password' name='obsPass' value='" + esc(cfg.obsPass) + "'></label>";
  h += "<label>Nome da cena (identico ao OBS)<input type='text' name='scenes' value='" + esc(cfg.scenes) + "'>"
       "<small>Varias cenas: separe com ponto e virgula. Ex.: Camera 1;Camera 1 + Slide</small></label>";
  h += "<label>Quantidade de LEDs<input type='number' name='numLeds' min='1' max='300' value='" + String(cfg.numLeds) + "'></label>";
  h += "<label>Brilho (1-255)<input type='number' name='bright' min='1' max='255' value='" + String(cfg.brightness) + "'></label>";
  h += F("<button type='submit'>Salvar e reiniciar</button></form>"
         "<script>function up(){fetch('/status').then(r=>r.json()).then(s=>{"
         "document.getElementById('st').innerHTML='Wi-Fi: '+(s.wifi?'conectado ('+s.ip+')':'desconectado')+"
         "'<br>OBS: '+(s.obs?'conectado':'desconectado')+'<br>Cena no ar: '+(s.scene||'-')+"
         "'<br>Cena em preview: '+(s.preview||'-')+"
         "'<br>Tally: <b>'+(s.live?'NO AR (vermelho)':(s.isPreview?'PROXIMA (verde)':'desligado'))+'</b>'}).catch(()=>{})}up();setInterval(up,2000)</script>"
         "</body></html>");
  server.send(200, "text/html", h);
}

void handleSave() {
  cfg.ssid    = server.arg("ssid");
  cfg.pass    = server.arg("pass");
  cfg.obsIp   = server.arg("obsIp");  cfg.obsIp.trim();
  cfg.obsPass = server.arg("obsPass");
  cfg.scenes  = server.arg("scenes");
  cfg.obsPort = constrain(server.arg("obsPort").toInt(), 1, 65535);
  cfg.numLeds = constrain(server.arg("numLeds").toInt(), 1, 300);
  cfg.brightness = constrain(server.arg("bright").toInt(), 1, 255);
  saveConfig();
  server.send(200, "text/html; charset=utf-8",
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<body style='font-family:sans-serif;background:#111;color:#eee;padding:20px'>"
              "<h2>Salvo!</h2><p>Reiniciando... reconecte no Wi-Fi <b>" AP_SSID "</b> e abra 192.168.4.1 se precisar ajustar de novo.</p>");
  delay(1500);
  ESP.restart();
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  loadConfig();

  strip.updateLength(cfg.numLeds);
  strip.setPin(LED_PIN);
  strip.begin();
  strip.setBrightness(cfg.brightness);
  blink3(255, 255, 255);  // 3 piscadas BRANCAS: ESP32 ligado

  // AP sempre ativo (para configurar na igreja) + cliente Wi-Fi
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setAutoReconnect(true);
  if (cfg.ssid.length()) {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    Serial.printf("Conectando ao Wi-Fi %s\n", cfg.ssid.c_str());
  }

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("Config: Wi-Fi " AP_SSID " -> http://192.168.4.1");
}

void loop() {
  server.handleClient();

  static bool wasConnected = false;
  bool conn = (WiFi.status() == WL_CONNECTED);
  if (conn && !wasConnected) blink3(0, 0, 255);   // 3 piscadas AZUIS: Wi-Fi conectado
  wasConnected = conn;

  if (conn) {
    startObs();
    if (wsStarted) ws.loop();
  } else {
    obsConnected = false;
    isLive = false;
    isPreview = false;
  }
  updateLeds();
}
