#include "Arduino.h"
#include "display_bsp.h"
#include "src/app_bsp/lvgl_bsp.h"
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "adc_bsp.h"
#include "ui/ui.h"
#include "WiFi.h"
#include "Wire.h"
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "Audio.h"
#include "es8311.h"
#include <WebServer.h>
#include <Preferences.h>
#include "arduinoFFT.h"

#define I2S_DOUT 8
#define I2S_BCLK 9
#define I2S_LRC 45
#define I2S_MCLK 16
#define I2C_SDA 13
#define I2C_SCL 14
#define FFT_SAMPLES 256

const int BUTTON_PIN = 0;
const unsigned long DEBOUNCE_DELAY = 50;
static constexpr char FIRMWARE_VERSION[] = "0.2.4";
const int DISPLAY_WIDTH = 400;
const int DISPLAY_HEIGHT = 300;
int curr_url = 0;
int SPECTRUM_Y_START = DISPLAY_HEIGHT;
const int SPECTRUM_HEIGHT = 100;
const int BARS_COUNT = 32;
const int BAR_WIDTH = 10;
const int BAR_PADDING = 2;
const int SPECTRUM_X_START = 8;
uint32_t spectrum_last_report = 0;
uint32_t spectrum_frame_count = 0;

double vReal[FFT_SAMPLES];
double vImag[FFT_SAMPLES];
volatile int16_t shared_pcm_buffer[FFT_SAMPLES];
volatile bool shared_data_ready = false;
volatile uint32_t callback_trigger_count = 0;

int16_t fft_output_heights[BARS_COUNT] = {0};
int16_t previous_heights[BARS_COUNT] = {0};
arduinoFFT FFT = arduinoFFT();
Audio audio;
ES8311 es;
WiFiUDP ntpUDP;
Preferences preferences;
WebServer server(80);
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 3600 * 8, 60000);
DisplayPort RlcdPort(12, 11, 5, 40, 41, DISPLAY_WIDTH, DISPLAY_HEIGHT);

String ssid;
String pass;

String ntpServer = "ntp.aliyun.com";
const int MAX_STATIONS = 20;
String stations[MAX_STATIONS];
String stationNames[MAX_STATIONS];
int stationsCount = 0;
int defaultVolume = 70;

struct StationPreset
{
  const char *name;
  const char *url;
};

const StationPreset defaultStations[] = {
    {"中国之声", "https://lhttp.qtfm.cn/live/15318317/64k.mp3"},
    {"国际新闻广播", "https://lhttp.qtfm.cn/live/20500172/64k.mp3"},
    {"两广之声音乐台", "https://lhttp.qtfm.cn/live/20500149/64k.mp3"},
    {"经典华语音乐", "https://lhttp.qtfm.cn/live/5022308/64k.mp3"},
    {"怀集音乐之声", "https://lhttp.qingting.fm/live/4804/64k.mp3"}};
const int DEFAULT_STATIONS_COUNT = sizeof(defaultStations) / sizeof(defaultStations[0]);

volatile uint32_t lastDataTime = 0;
const uint32_t RECONNECT_TIMEOUT = 15000;
bool isPlaying = false;

bool currentStationIsValid()
{
  return curr_url >= 0 && curr_url < stationsCount && !stations[curr_url].isEmpty();
}

void showCurrentStation()
{
  if (!currentStationIsValid() || !Lvgl_lock(500))
    return;

  lv_label_set_text(ui_Label1, stationNames[curr_url].c_str());
  lv_label_set_text(ui_Label2, "");
  Lvgl_unlock();
}

void startAudio(const char *url)
{
  if (url == nullptr || url[0] == '\0')
  {
    isPlaying = false;
    return;
  }

  Serial.print("开始播放: ");
  Serial.println(url);
  lastDataTime = millis();
  isPlaying = audio.connecttohost(url);
}
void audio_process_i2s(int32_t *outBuff, int16_t validSamples, bool *continueI2S)
{
  (void)outBuff;
  (void)validSamples;
  if (continueI2S)
    *continueI2S = true;
  lastDataTime = millis();
}
void audio_eof_stream(const char *info)
{
  Serial.print("音频流结束: ");
  Serial.println(info);
  isPlaying = false;
  vTaskDelay(pdMS_TO_TICKS(500));
  if (currentStationIsValid())
    startAudio(stations[curr_url].c_str());
}
void audio_showstation(const char *info)
{
  Serial.print("电台状态: ");
  Serial.println(info);
}
static char audio_codec[8] = "---";
static char audio_rate[8] = "---k";
static char audio_channels[8] = "---";
static char audio_bitrate[12] = "--- kbps";

void my_audio_info(Audio::msg_t m)
{
  Serial.printf("Audio Event -> %s: %s (code: %d)\n", m.s, m.msg, m.e);
  bool update_label3 = false;
  if (m.e == Audio::evt_id3data || strncmp(m.s, "streamtitle", 11) == 0)
  {
    if (Lvgl_lock(500))
    {
      const char *dynamic_text = m.msg;
      if (strstr(m.msg, "Title: ") != NULL)
        dynamic_text = m.msg + 7;
      lv_label_set_text(ui_Label2, dynamic_text);
      Lvgl_unlock();
    }
  }
  if (strncmp(m.s, "station_name", 12) == 0)
  {
    Serial.print("流媒体电台名称: ");
    Serial.println(m.msg);
  }
  if (strncmp(m.s, "info", 4) == 0 && strstr(m.msg, "Decoder has been initialized") != NULL)
  {
    if (strstr(m.msg, "MP3") != NULL)
      strcpy(audio_codec, "MP3");
    else if (strstr(m.msg, "VORBIS") != NULL)
      strcpy(audio_codec, "OGG");
    else if (strstr(m.msg, "AAC") != NULL)
      strcpy(audio_codec, "AAC");
    else if (strstr(m.msg, "FLAC") != NULL)
      strcpy(audio_codec, "FLAC");
    update_label3 = true;
  }
  if (strncmp(m.s, "info", 4) == 0 && strncmp(m.msg, "SampleRate (Hz): ", 17) == 0)
  {
    long rate = atol(m.msg + 17);
    if (rate % 1000 == 0)
    {
      snprintf(audio_rate, sizeof(audio_rate), "%ldk", rate / 1000);
    }
    else
    {
      snprintf(audio_rate, sizeof(audio_rate), "%.1fk", (float)rate / 1000.0);
    }
    update_label3 = true;
  }
  if (strncmp(m.s, "info", 4) == 0 && strncmp(m.msg, "Channels: ", 10) == 0)
  {
    int ch = atoi(m.msg + 10);
    if (ch == 2)
      strcpy(audio_channels, "STEREO");
    else if (ch == 1)
      strcpy(audio_channels, "MONO");
    else
      snprintf(audio_channels, sizeof(audio_channels), "%d CH", ch);
    update_label3 = true;
  }
  if (strncmp(m.s, "bitrate", 7) == 0)
  {
    long bitr = atol(m.msg);
    snprintf(audio_bitrate, sizeof(audio_bitrate), "%ld kbps", bitr / 1000);
    update_label3 = true;
  }
  if (strncmp(m.s, "info", 4) == 0 && strstr(m.msg, "Closing web stream") != NULL)
  {
    strcpy(audio_codec, "---");
    strcpy(audio_rate, "---k");
    strcpy(audio_channels, "---");
    strcpy(audio_bitrate, "--- kbps");
    update_label3 = true;
  }
  if (update_label3)
  {
    if (Lvgl_lock(500))
    {
      char buf[64];
      // "44.1k   STEREO   MP3    128 kbps"
      snprintf(buf, sizeof(buf), "%s   %s   %s    %s", audio_rate, audio_channels, audio_codec, audio_bitrate);
      lv_label_set_text(ui_Label3, buf);
      Lvgl_unlock();
    }
  }
}

void updateWiFiStatus()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    long rssi = WiFi.RSSI();
    if (Lvgl_lock(-1))
    {
      lv_label_set_text_fmt(ui_Label4, "%ld dBm", rssi);
      Lvgl_unlock();
    }
  }
  else
  {
    if (Lvgl_lock(-1))
    {
      lv_label_set_text(ui_Label4, "Wi-Fi 未连接");
      Lvgl_unlock();
    }
  }
}
void Time_UpdateTask(void *pvParameters)
{
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(1000);
  for (;;)
  {
    static int ntp_sync_counter = 0;
    uint32_t a_br = audio.getBitRate() / 1024;
    if (ntp_sync_counter++ > 60)
    {
      timeClient.update();
      ntp_sync_counter = 0;
    }
    String currentTime = timeClient.getFormattedTime();
    if (Lvgl_lock(500))
    {
      lv_label_set_text(ui_Label5, currentTime.c_str());
      lv_label_set_text_fmt(ui_Label8, "%ld kbps", a_br);
      Lvgl_unlock();
    }
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  uint16_t *buffer = (uint16_t *)color_map;
  for (int y = area->y1; y <= area->y2; y++)
  {
    if (y >= SPECTRUM_Y_START)
    {
      buffer += (area->x2 - area->x1 + 1);
      continue;
    }
    for (int x = area->x1; x <= area->x2; x++)
    {
      uint8_t color = (*buffer < 0x0001) ? ColorBlack : ColorWhite;
      RlcdPort.RLCD_SetPixel(x, y, color);
      buffer++;
    }
  }
  if (lv_disp_flush_is_last(drv))
    RlcdPort.RLCD_Display();
  lv_disp_flush_ready(drv);
}

void Adc_LoopTask(void *arg)
{
  for (;;)
  {
    updateWiFiStatus();
    int data;
    float vol = Adc_GetBatteryVoltage(&data);
    if (Lvgl_lock(500))
    {
      int v_int = (int)vol;
      int v_frac = (int)((vol - v_int) * 100);
      lv_label_set_text_fmt(ui_Label6, "%d.%02d V", v_int, v_frac);
      Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
void Spectrum_Analyzer_Task(void *pvParameters)
{
  uint32_t last_render_time = 0;
  const uint32_t RENDER_DELAY_MS = 20;
  for (;;)
  {
    if (audio.isRunning() && shared_data_ready)
    {
      uint32_t current_time = millis();
      if (current_time - last_render_time < RENDER_DELAY_MS)
      {
        shared_data_ready = false;
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      last_render_time = current_time;
      for (int i = 0; i < FFT_SAMPLES; i++)
      {
        vReal[i] = (double)shared_pcm_buffer[i];
        vImag[i] = 0.0;
      }
      shared_data_ready = false;
      FFT.Windowing(vReal, FFT_SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
      FFT.Compute(vReal, vImag, FFT_SAMPLES, FFT_FORWARD);
      FFT.ComplexToMagnitude(vReal, vImag, FFT_SAMPLES);
      const double fft_norm_factor = 2.0 / FFT_SAMPLES;
      const double FFT_MAX_POSSIBLE = 32768.0;
      const double bin_width = 44100.0 / FFT_SAMPLES; // ~172 Hz na koszyk

      for (int b = 0; b < BARS_COUNT; b++)
      {
        int start_idx, end_idx;
        int max_usable_bins = 93; // granica ~16 kHz
        if (b < 6)
        {
          start_idx = b + 1;
          end_idx = b + 1;
        }
        else
        {
          int log_bars = BARS_COUNT - 6;
          int log_b = b - 6;
          start_idx = 7 + (int)(pow(2.0, (double)log_b / (log_bars - 1) * log2(max_usable_bins - 7)));
          end_idx = 7 + (int)(pow(2.0, (double)(log_b + 1) / (log_bars - 1) * log2(max_usable_bins - 7)));
        }
        if (start_idx < 1)
          start_idx = 1;
        if (end_idx > max_usable_bins)
          end_idx = max_usable_bins;
        if (end_idx < start_idx)
          end_idx = start_idx;
        double rms_magnitude = 0.0;
        int count = 0;
        double center_freq = 0.0;
        for (int i = start_idx; i <= end_idx; i++)
        {
          double normalized_bin = vReal[i] * fft_norm_factor;
          rms_magnitude += (normalized_bin * normalized_bin);
          center_freq += (i * bin_width);
          count++;
        }
        rms_magnitude = sqrt(rms_magnitude / count);
        center_freq /= count;
        // skalowanie
        double db = -75.0;
        if (rms_magnitude > 0.0)
        {
          db = 20.0 * log10(rms_magnitude / FFT_MAX_POSSIBLE);
        }
        double f2 = center_freq * center_freq;
        double f4 = f2 * f2;
        double num = 12194.0 * 12194.0 * f4;
        double den = (f2 + 20.6 * 20.6) * sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0);
        double a_weight = 20.0 * log10(num / den) + 2.0;
        db += a_weight;
        int16_t target_h = 0;
        double db_min = -75.0;
        if (db > db_min)
        {
          target_h = (int16_t)((db - db_min) * (SPECTRUM_HEIGHT / (-db_min)));
        }

        if (target_h > SPECTRUM_HEIGHT)
          target_h = SPECTRUM_HEIGHT;
        if (target_h < 0)
          target_h = 0;
        if (target_h > fft_output_heights[b])
        {
          fft_output_heights[b] = target_h;
        }
        else
        {
          fft_output_heights[b] -= 4;
          if (fft_output_heights[b] < 0)
            fft_output_heights[b] = 0;
        }
      }
      if (Lvgl_lock(15))
      {
        int base_y = SPECTRUM_Y_START + SPECTRUM_HEIGHT - 1;
        for (int b = 0; b < BARS_COUNT; b++)
        {
          int x_bar = SPECTRUM_X_START + b * (BAR_WIDTH + BAR_PADDING);
          int new_h = fft_output_heights[b];
          int old_h = previous_heights[b];
          if (new_h > old_h)
          {
            for (int y = base_y - old_h; y >= base_y - new_h; y--)
            {
              for (int x = x_bar; x < x_bar + BAR_WIDTH; x++)
                RlcdPort.RLCD_SetPixel(x, y, ColorBlack);
            }
          }
          else if (new_h < old_h)
          {
            for (int y = base_y - old_h; y < base_y - new_h; y++)
            {
              for (int x = x_bar; x < x_bar + BAR_WIDTH; x++)
                RlcdPort.RLCD_SetPixel(x, y, ColorWhite);
            }
          }
          previous_heights[b] = new_h;
        }
        RlcdPort.RLCD_Display();
        Lvgl_unlock();
        spectrum_frame_count++;
      }
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(15));
    }
    uint32_t t_now = millis();
    if (t_now - spectrum_last_report >= 1000)
    {
      Serial.printf("FFT: %d FPS | RAW: %d/sek\n",
                    spectrum_frame_count, callback_trigger_count);
      callback_trigger_count = 0;
      spectrum_frame_count = 0;
      spectrum_last_report = t_now;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void loadConfiguration()
{
  preferences.begin("radio_cfg", true); // RO (true)
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("pass", "");
  ntpServer = preferences.getString("ntp_srv", "ntp.aliyun.com");
  curr_url = preferences.getInt("curr_url", 0);
  defaultVolume = preferences.getInt("default_vol", 70);
  defaultVolume = constrain(defaultVolume, 0, 100);
  stationsCount = preferences.getInt("st_count", 0);
  if (stationsCount < 0 || stationsCount > MAX_STATIONS)
    stationsCount = 0;

  if (stationsCount == 0)
  {
    Serial.println("未找到电台配置，使用中国电台预设");
    stationsCount = DEFAULT_STATIONS_COUNT;
    for (int i = 0; i < stationsCount; i++)
    {
      stationNames[i] = defaultStations[i].name;
      stations[i] = defaultStations[i].url;
    }
  }
  else
  {
    for (int i = 0; i < stationsCount; i++)
    {
      String key = "st_" + String(i);
      String nameKey = "st_name_" + String(i);
      stations[i] = preferences.getString(key.c_str(), "");
      stationNames[i] = preferences.getString(nameKey.c_str(), "电台 " + String(i + 1));
    }
    Serial.println("已加载保存的电台列表");
  }

  if (curr_url < 0 || curr_url >= stationsCount)
    curr_url = 0;
  preferences.end();
}

void saveConfiguration(String newSsid, String newPass, String rawStations, String newNtp, int newVolume)
{
  newVolume = constrain(newVolume, 0, 100);
  preferences.begin("radio_cfg", false); // RW
  preferences.putString("ssid", newSsid);
  preferences.putString("pass", newPass);
  preferences.putString("ntp_srv", newNtp);
  preferences.putInt("default_vol", newVolume);
  int idx = 0;
  int fromPos = 0;
  while (idx < MAX_STATIONS && fromPos <= rawStations.length())
  {
    int toPos = rawStations.indexOf('\n', fromPos);
    String entry = toPos == -1 ? rawStations.substring(fromPos) : rawStations.substring(fromPos, toPos);
    entry.trim();
    if (entry.length() > 0)
    {
      int separator = entry.indexOf('|');
      String stationName = separator >= 0 ? entry.substring(0, separator) : "电台 " + String(idx + 1);
      String stationUrl = separator >= 0 ? entry.substring(separator + 1) : entry;
      stationName.trim();
      stationUrl.trim();

      if (stationName.length() == 0)
        stationName = "电台 " + String(idx + 1);

      if (stationUrl.length() > 0)
      {
        String key = "st_" + String(idx);
        String nameKey = "st_name_" + String(idx);
        preferences.putString(key.c_str(), stationUrl);
        preferences.putString(nameKey.c_str(), stationName);
        idx++;
      }
    }

    if (toPos == -1)
      break;
    fromPos = toPos + 1;
  }
  preferences.putInt("st_count", idx);
  preferences.putInt("curr_url", 0);
  preferences.end();
  Serial.println("配置已保存");
}

String htmlEscape(String value)
{
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

void handleRoot()
{
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif;background:#222;color:#fff;padding:20px;} input,textarea{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;background:#333;color:#fff;border:1px solid #555;} button{background:#00b4d8;color:#fff;border:0;padding:12px;width:100%;cursor:pointer;font-size:16px;} .vol-container{display:flex;justify-content:space-between;align-items:center;}</style>";
  html += "<title>RLCD 网络电台 v" + String(FIRMWARE_VERSION) + "</title></head><body>";
  html += "<h2>网络电台配置</h2>";
  html += "<p style='color:#aaa;margin-top:-8px;'>固件版本 v" + String(FIRMWARE_VERSION) + "</p>";
  html += "<form action='/save' method='POST'>";
  html += "<label>Wi-Fi 名称:</label><input type='text' name='ssid' value='" + htmlEscape(ssid) + "'>";
  html += "<label>Wi-Fi 密码:</label><input type='password' name='pass' value='" + htmlEscape(pass) + "'>";
  html += "<label>NTP 服务器:</label><input type='text' name='ntp' value='" + htmlEscape(ntpServer) + "'>";
  html += "<div class='vol-container'><label>音量:</label><span id='volVal'>" + String(defaultVolume) + "</span></div>";
  html += "<input type='range' name='volume' min='0' max='100' value='" + String(defaultVolume) + "' oninput='vol(this.value);'>";
  html += "<label>电台列表（名称|地址，每行一个）:</label>";
  html += "<textarea name='stations' rows='10'>";
  for (int i = 0; i < stationsCount; i++)
  {
    html += htmlEscape(stationNames[i]) + "|" + htmlEscape(stations[i]) + "\n";
  }
  html += "</textarea>";
  html += "<button type='submit'>保存并重启</button>";
  html += "</form></body><script>function vol(a){fetch('/vol/?a='+a)}</script></html>";
  server.send(200, "text/html", html);
}
void handleVol()
{
  if (server.args() > 0)
  {
    String vol = server.arg(0);
    int volume = constrain(vol.toInt(), 0, 100);
    Serial.print("vol: ");
    Serial.println(volume);
    server.send(200, "text/plain", "OK");
    es.setVolume(volume);
    preferences.putInt("default_vol", volume);
  }
  else
  {
    server.send(400, "text/plain", "Err");
  }
}

void handleSave()
{
  if (server.hasArg("ssid") && server.hasArg("pass") && server.hasArg("stations") && server.hasArg("ntp") && server.hasArg("volume"))
  {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");
    String newStations = server.arg("stations");
    String newNtp = server.arg("ntp");
    int newVolume = server.arg("volume").toInt();
    saveConfiguration(newSsid, newPass, newStations, newNtp, newVolume);
    server.send(200, "text/html", "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body><h3>保存完成，设备即将重启。</h3></body></html>");
    delay(2000);
    ESP.restart();
  }
  else
  {
    server.send(400, "text/plain; charset=UTF-8", "配置不完整");
  }
}
void setup()
{
  Serial.begin(115200);
  delay(500);
  WiFi.setHostname("ESP-RLCD-Radio");
  RlcdPort.RLCD_Init();
  Lvgl_PortInit(DISPLAY_WIDTH, DISPLAY_HEIGHT, Lvgl_FlushCallback);
  loadConfiguration();
  timeClient.setPoolServerName(ntpServer.c_str());
  ui_init();
  lv_scr_load(ui_connect);
  if (Lvgl_lock(500))
  {
    String x = "正在连接 Wi-Fi...\n";
    x += ssid;
    lv_label_set_text(ui_Label7, x.c_str());
    Lvgl_unlock();
  }
  if (ssid.length() > 0)
  {
    Serial.print("正在连接 Wi-Fi: ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < 40)
    {
      delay(500);
      Serial.print(".");
      counter++;
    }
    Serial.println("");
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("网络地址: ");
    if (Lvgl_lock(500))
    {
      String y = "网络连接成功\n";
      y += WiFi.localIP().toString();
      lv_label_set_text(ui_Label7, y.c_str());
      Lvgl_unlock();
    }
    Serial.println(WiFi.localIP());
    server.on("/", HTTP_GET, handleRoot);
    server.on("/vol/", HTTP_GET, handleVol);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    timeClient.begin();
    delay(2000);
    SPECTRUM_Y_START = 200;
    lv_scr_load(ui_Monitor);
    Serial.println("\n电台列表:");
    for (int i = 0; i < stationsCount; i++)
    {
      Serial.printf("电台 %d: %s | %s\n", i, stationNames[i].c_str(), stations[i].c_str());
    }
    bool ntpUpdated = false;
    for (int attempt = 0; attempt < 10 && !ntpUpdated; attempt++)
    {
      ntpUpdated = timeClient.forceUpdate();
      if (ntpUpdated)
        break;
      delay(500);
    }
    Serial.println(ntpUpdated ? "NTP 同步成功" : "NTP 同步失败，稍后重试");
  }
  else
  {
    Serial.println("Wi-Fi 连接失败，启动配置热点");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("RLCD-Radio-Setup", "11111111");
    Serial.println(WiFi.softAPIP());
    if (Lvgl_lock(500))
    {
      lv_label_set_text(ui_Label7, "请配置 Wi-Fi\n连接到 RLCD-Radio-Setup\n密码: 11111111\n访问: 192.168.4.1");
      Lvgl_unlock();
    }
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    for (;;)
    {
      server.handleClient();
      delay(10);
    }
  }
  pinMode(46, OUTPUT); // wzmacniacz audio
  pinMode(0, INPUT);   // guzik
  digitalWrite(46, LOW);
  Adc_PortInit();
  Wire.begin(I2C_SDA, I2C_SCL);
  Audio::audio_info_callback = my_audio_info;
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(audio.getVolumeSteps());
  es.begin(I2C_SDA, I2C_SCL, 400000);
  es.setVolume(defaultVolume);
  es.setBitsPerSample(16);
  preferences.begin("radio_cfg", false);
  if (currentStationIsValid())
  {
    showCurrentStation();
    startAudio(stations[curr_url].c_str());
    digitalWrite(46, HIGH);
  }
  xTaskCreatePinnedToCore(Adc_LoopTask, "ADC_Task", 3000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Time_UpdateTask, "Time_Task", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(Spectrum_Analyzer_Task, "Spectrum_Task", 4096, NULL, 1, NULL, 0);
}

void handleButton()
{
  static int lastButtonState = HIGH;
  static int currentButtonState = HIGH;
  static unsigned long lastDebounceTime = 0;
  int reading = digitalRead(BUTTON_PIN);
  if (reading != currentButtonState)
  {
    lastDebounceTime = millis();
    currentButtonState = reading;
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY)
  {
    if (reading != lastButtonState)
    {
      lastButtonState = reading;
      if (lastButtonState == LOW)
      {
        curr_url++;
        if (curr_url >= stationsCount)
          curr_url = 0;
        if (currentStationIsValid())
        {
          showCurrentStation();
          startAudio(stations[curr_url].c_str());
          digitalWrite(46, HIGH);
          preferences.putInt("curr_url", curr_url);
        }
      }
    }
  }
}

void loop()
{
  server.handleClient();
  ArduinoOTA.handle();
  audio.loop();
  handleButton();
  if (isPlaying && currentStationIsValid() && (millis() - lastDataTime > RECONNECT_TIMEOUT))
  {
    Serial.println("音频无数据，正在重新连接...");
    startAudio(stations[curr_url].c_str());
  }
  vTaskDelay(pdMS_TO_TICKS(1));
}
