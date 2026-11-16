// 办公室墨水屏温度牌 —— 微雪 ESP32-S3-ePaper-3.97
//
// 工作方式：醒来 → 读 SHTC3 → 判断三档 → 必要时刷屏 → 深度睡眠 N 分钟。
// 墨水屏断电保持画面，所以大部分时间整机只有深度睡眠电流。
//
// 文案、阈值、字体都在 config/messages.json，改完跑 src/tools/gen_assets.py 再编译。
// 屏幕驱动、I2C、SHTC3 读数沿用微雪官方示例 04_I2C_SHTC3（同目录其余文件）。

#include <math.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "EPD_3in97.h"
#include "GUI_Paint.h"
#include "user_config.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"
#include "assets_gen.h"
#include "clock_status.h"
#include "display_power.h"
#include "weekday_theme.h"
#include "weekday_assets_gen.h"
#include "network_setup.h"

// 微雪示例在 SHTC3 换算里固定减了 4℃（为抵消常亮演示的自发热）。
// 本固件大部分时间在深度睡眠、刚醒就读数，自发热小得多，先把这 4℃ 加回来，
// 再用 messages.json 的 temp_offset_c 按参考温度计校准。
static const float VENDOR_SELF_HEAT_OFFSET = 4.0f;

static const float REFRESH_DELTA_C = 0.3f;   // 温度变化超过它才刷屏
static const float HYSTERESIS_C = 0.2f;      // 档位回滞，避免在边界来回跳
static const int FULL_CLEAN_EVERY = 24;      // 每 N 次刷新做一次完整刷新，清残影
static const int SAMPLES = 3;

enum State : int8_t { ST_COLD = 0, ST_OK = 1, ST_WARM = 2, ST_UNKNOWN = -1 };

// RTC 内存在深度睡眠中保留，用来记住上次显示了什么
RTC_DATA_ATTR int16_t g_lastShownTenths = INT16_MIN;
RTC_DATA_ATTR int8_t g_lastState = ST_UNKNOWN;
RTC_DATA_ATTR uint16_t g_refreshCount = 0;
RTC_DATA_ATTR uint32_t g_wakeCount = 0;

static UBYTE *g_frame = nullptr;
static char g_updated[32] = "TIME UNSET";
static unsigned g_coverIndex = 0;
static unsigned g_weekday = 0;
static bool g_clockValid = false;

static void readClockStatus() {
  g_clockValid=false;
  g_coverIndex=0;
  snprintf(g_updated,sizeof(g_updated),"TIME UNSET");
  uint8_t control=0, raw[7]={0};
  if (i2c_read_buff(rtc_dev_handle, 0x00, &control, 1) != ESP_OK ||
      i2c_read_buff(rtc_dev_handle, 0x04, raw, 7) != ESP_OK) {
    Serial.println("rtc=I2C_ERROR");return;
  }
  const ClockStamp stamp = decodeClock(control, raw);
  g_coverIndex = coverForDate(stamp);
  g_weekday = weekdayForDate(stamp);
  g_clockValid = stamp.valid;
  if (stamp.valid) {
    snprintf(g_updated, sizeof(g_updated), "%02u-%02u %02u:%02u", stamp.month, stamp.day, stamp.hour, stamp.minute);
  }
  Serial.printf("rtc=%s\n", g_updated);
  if(!stamp.valid) Serial.printf("rtc_invalid control=%02x seconds=%02x date=%02x/%02x/%02x\n",control,raw[0],raw[6],raw[5],raw[3]);
}

static State classify(float t, State prev) {
  float low = CFG_OK_LOW_C, high = CFG_OK_HIGH_C;
  // 回滞：已在某档时，要越过边界再多 HYSTERESIS_C 才切走
  if (prev == ST_OK) { low -= HYSTERESIS_C; high += HYSTERESIS_C; }
  else if (prev == ST_COLD) { low += HYSTERESIS_C; }
  else if (prev == ST_WARM) { high -= HYSTERESIS_C; }
  if (t < low) return ST_COLD;
  if (t > high) return ST_WARM;
  return ST_OK;
}

static bool readTemperature(i2c_equipment_shtc3 &sensor, float &tempC, float &rh) {
  float sumT = 0, sumH = 0;
  int ok = 0;
  for (int i = 0; i < SAMPLES; i++) {
    shtc3_data_t d = sensor.readTempHumi();
    // 读失败时示例返回的值不可信；用合理范围过滤
    if (d.Temp > -30 && d.Temp < 80 && d.RH >= 0 && d.RH <= 100) {
      sumT += d.Temp; sumH += d.RH; ok++;
    }
    delay(20);
  }
  if (ok < 2) return false;
  tempC = sumT / ok + VENDOR_SELF_HEAT_OFFSET + CFG_TEMP_OFFSET_C;
  rh = sumH / ok;
  return true;
}

static void blit(const Bitmap &b, int x0, int y0) {
  const int rowBytes = (b.w + 7) / 8;
  for (int y = 0; y < b.h; y++) {
    for (int x = 0; x < b.w; x++) {
      if (b.data[y * rowBytes + x / 8] & (0x80 >> (x % 8))) {
        Paint_SetPixel(x0 + x, y0 + y, BLACK);
      }
    }
  }
}

static const Bitmap *glyphFor(char c) {
  for (int i = 0; DIGIT_CHARS[i]; i++) {
    if (DIGIT_CHARS[i] == c) return &DIGIT_GLYPHS[i];
  }
  return nullptr;
}

// 与 gen_assets.py 的 compose_preview() 同一套版面
static void drawLegacyScreen(float tempC, float rh, State st) {
  Paint_NewImage(g_frame, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, CFG_ROTATION, WHITE);
  Paint_SelectImage(g_frame);
  Paint_Clear(WHITE);
  const int W = Paint.Width, H = Paint.Height;
  Paint_DrawString_EN(LAYOUT_MARGIN, 4, g_updated, &Font16, WHITE, BLACK);

  char buf[12];
  snprintf(buf, sizeof(buf), "%.1f", tempC);
  int numW = LAYOUT_UNIT_GAP + UNIT_GLYPH.w;
  for (char *p = buf; *p; p++) { const Bitmap *g = glyphFor(*p); if (g) numW += g->w; }
  int x = (W - numW) / 2;
  for (char *p = buf; *p; p++) {
    const Bitmap *g = glyphFor(*p);
    if (!g) continue;
    blit(*g, x, LAYOUT_NUM_TOP);
    x += g->w;
  }
  blit(UNIT_GLYPH, x + LAYOUT_UNIT_GAP, LAYOUT_NUM_TOP);

  const int divY = LAYOUT_NUM_TOP + DIGIT_GLYPHS[0].h + LAYOUT_DIVIDER_GAP;
  Paint_DrawRectangle(LAYOUT_MARGIN, divY, W - LAYOUT_MARGIN, divY + 2, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

  const Bitmap *lines = MSG_COLD; int n = MSG_COLD_LINES;
  if (st == ST_OK) { lines = MSG_OK; n = MSG_OK_LINES; }
  else if (st == ST_WARM) { lines = MSG_WARM; n = MSG_WARM_LINES; }
  int y = divY + LAYOUT_DIVIDER_GAP + 4;
  for (int i = 0; i < n; i++) {
    blit(lines[i], (W - lines[i].w) / 2, y);
    y += lines[i].h + LAYOUT_LINE_GAP;
  }

  blit(FOOTER, (W - FOOTER.w) / 2, H - LAYOUT_FOOTER_BOTTOM - FOOTER.h);

  // 湿度用板载小号英文字体，放右下角
  snprintf(buf, sizeof(buf), "RH%d%%", (int)lroundf(rh));
  Paint_DrawString_EN(W - LAYOUT_MARGIN - 12 * 7, 4, buf, &Font16, WHITE, BLACK);
}

static void drawScreen(float tempC, float rh, State st) {
  Paint_NewImage(g_frame, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, CFG_ROTATION, WHITE);
  Paint_SelectImage(g_frame);
  Paint_Clear(WHITE);
  blit(WEEKDAY_BACKGROUNDS[g_coverIndex], 0, 0);
  // Clear every baked example value before drawing live readings.
  Paint_DrawRectangle(0,0,799,53,WHITE,DOT_PIXEL_1X1,DRAW_FILL_FULL);
  Paint_DrawRectangle(290,100,799,270,WHITE,DOT_PIXEL_1X1,DRAW_FILL_FULL);
  Paint_DrawString_EN(24,16,g_updated,&Font16,WHITE,BLACK);
  if (g_clockValid) blit(WEEKDAY_LABELS[g_weekday],220,14);
  char buf[20];
  snprintf(buf,sizeof(buf),"RH%d%%",(int)lroundf(rh));
  Paint_DrawString_EN(690,16,buf,&Font16,WHITE,BLACK);
  snprintf(buf,sizeof(buf),"%.1f",tempC);
  int width=8+LIVE_UNIT.w;
  for(char *p=buf;*p;++p) for(int i=0;DIGIT_CHARS[i];++i) if(*p==DIGIT_CHARS[i]) width+=LIVE_DIGITS[i].w;
  int x=290+(500-width)/2;
  if(width>500) {
    Paint_DrawString_EN(320,155,buf,&Font24,WHITE,BLACK);
  } else {
    for(char *p=buf;*p;++p) for(int i=0;DIGIT_CHARS[i];++i) if(*p==DIGIT_CHARS[i]) { blit(LIVE_DIGITS[i],x,110);x+=LIVE_DIGITS[i].w; }
    blit(LIVE_UNIT,x+8,110);
  }
  // Compare measured value directly: no comfort band or hysteresis for this request.
  if (tempC >= CFG_REQUEST_MIN_C) {
    Paint_DrawRectangle(290,365,799,425,WHITE,DOT_PIXEL_1X1,DRAW_FILL_FULL);
    blit(REQUEST_LINE,290+(500-REQUEST_LINE.w)/2,375);
  } else {
    Paint_DrawRectangle(290,290,799,425,WHITE,DOT_PIXEL_1X1,DRAW_FILL_FULL);
  }
  Paint_DrawRectangle(0,430,799,479,WHITE,DOT_PIXEL_1X1,DRAW_FILL_FULL);
  blit(REQUEST_FOOTER,(800-REQUEST_FOOTER.w)/2,446);
  Serial.printf("cover=%u request=%d\n",g_coverIndex,tempC >= CFG_REQUEST_MIN_C);
}

static void drawSensorError() {
  Paint_NewImage(g_frame, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT, CFG_ROTATION, WHITE);
  Paint_SelectImage(g_frame);
  Paint_Clear(WHITE);
  Paint_DrawString_EN(40, 4, g_updated, &Font16, WHITE, BLACK);
  Paint_DrawString_EN(40, 40, "SENSOR ERROR", &Font24, WHITE, BLACK);
}

static bool pushFrame(bool fullClean) {
  auto pmuRead = [](uint8_t reg, uint8_t &value) { return i2c_read_buff(pmu_dev_handle, reg, &value, 1) == ESP_OK; };
  auto pmuWrite = [](uint8_t reg, uint8_t value) { return i2c_write_buff(pmu_dev_handle, reg, &value, 1) == ESP_OK; };
  uint8_t id=0, rails=0, voltage=0;
  pmuRead(0x03, id); pmuRead(0x90, rails); pmuRead(0x94, voltage);
  Serial.printf("pmu id=%02x rails=%02x aldo3=%u mV\n", id, rails, (voltage & 31)*100+500);
  if (!setDisplayRail(true, pmuRead, pmuWrite)) {
    Serial.println("display=ERROR PMU enable/readback failed");
    return false;
  }
  delay(500);
  DEV_Module_Init();
  EPD_3IN97_ClearError();
  if (fullClean) {
    EPD_3IN97_Init();
    if (!EPD_3IN97_HasError()) EPD_3IN97_Display(g_frame);
  } else {
    EPD_3IN97_Init_Fast();
    if (!EPD_3IN97_HasError()) EPD_3IN97_Display_Fast(g_frame);
  }
  const bool success = !EPD_3IN97_HasError();
  EPD_3IN97_Sleep();
  if (!setDisplayRail(false, pmuRead, pmuWrite)) Serial.println("WARNING PMU display power-off failed");
  Serial.printf("display=%s full=%d\n", success ? "OK" : "ERROR", fullClean);
  return success;
}

static void goToSleep() {
  esp_sleep_enable_timer_wakeup((uint64_t)CFG_UPDATE_MINUTES * 60ULL * 1000000ULL);
  // 按 BOOT 键立即唤醒并强制刷新
  pinMode((int)ext_wakeup_pin_1, INPUT_PULLUP);
  const uint32_t releaseStart = millis();
  while (digitalRead((int)ext_wakeup_pin_1) == LOW && millis() - releaseStart < 2000) delay(10);
  rtc_gpio_pullup_en(ext_wakeup_pin_1);
  rtc_gpio_pulldown_dis(ext_wakeup_pin_1);
  // A stuck/held button must not create an infinite wake-refresh loop.
  if (digitalRead((int)ext_wakeup_pin_1) == HIGH) esp_sleep_enable_ext0_wakeup(ext_wakeup_pin_1, 0);
  Serial.printf("sleep=%d seconds wake_count=%lu\n", CFG_UPDATE_MINUTES * 60, (unsigned long)g_wakeCount);
  Serial.flush();
  esp_deep_sleep_start();
}

static void showNetworkPortal(const char *ssid,const char *password) {
  Paint_NewImage(g_frame,EPD_3IN97_WIDTH,EPD_3IN97_HEIGHT,CFG_ROTATION,WHITE);
  Paint_SelectImage(g_frame); Paint_Clear(WHITE);
  Paint_DrawString_EN(35,35,"Wi-Fi Setup",&Font24,WHITE,BLACK);
  Paint_DrawString_EN(35,100,"1. Connect your phone to:",&Font20,WHITE,BLACK);
  Paint_DrawString_EN(35,140,ssid,&Font24,WHITE,BLACK);
  Paint_DrawString_EN(35,195,"Password:",&Font20,WHITE,BLACK);
  Paint_DrawString_EN(35,230,password,&Font24,WHITE,BLACK);
  Paint_DrawString_EN(35,295,"2. Open http://192.168.4.1",&Font20,WHITE,BLACK);
  Paint_DrawString_EN(35,365,"Exit: press and release BOOT, or wait 5 min.",&Font16,WHITE,BLACK);
  Paint_DrawString_EN(35,405,"Later: hold BOOT until this page appears.",&Font16,WHITE,BLACK);
  pushFrame(true);
}

void setup() {
  Serial.begin(115200);
  const uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 1500) delay(10);
  rtc_gpio_deinit(ext_wakeup_pin_1);
  ++g_wakeCount;
  Serial.printf("desk-thermometer v1.5.2 wake_count=%lu\n", (unsigned long)g_wakeCount);
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const bool coldBoot = (cause == ESP_SLEEP_WAKEUP_UNDEFINED);
  const bool buttonWake = (cause == ESP_SLEEP_WAKEUP_EXT0);

  i2c_master_Init();
  readClockStatus();
  const UWORD frameBytes = ((EPD_3IN97_WIDTH + 7) / 8) * EPD_3IN97_HEIGHT;
  g_frame = (UBYTE *)malloc(frameBytes);
  if (!g_frame) { Serial.println("frame alloc failed"); goToSleep(); }
  // Long press while running/sleeping enters setup; never require holding BOOT at power-on.
  pinMode((int)ext_wakeup_pin_1, INPUT_PULLUP);
  uint32_t held=millis();
  while(buttonWake && digitalRead((int)ext_wakeup_pin_1)==LOW && millis()-held<2500) delay(10);
  bool forcePortal=buttonWake && digitalRead((int)ext_wakeup_pin_1)==LOW;
  serviceNetwork(forcePortal,coldBoot,g_clockValid,showNetworkPortal);
  readClockStatus();
  i2c_equipment_shtc3 sensor;

  float tempC = NAN, rh = NAN;
  const bool ok = readTemperature(sensor, tempC, rh);
  sensor.shtc3_Sleep();


  if (!ok) {
    Serial.println("SHTC3 read failed");
    drawSensorError();
    pushFrame(true);
    g_lastState = ST_UNKNOWN;
    g_lastShownTenths = INT16_MIN;
    goToSleep();
  }

  const State st = classify(tempC, (State)g_lastState);
  const int16_t tenths = (int16_t)lroundf(tempC * 10);
  const bool changed = st != g_lastState ||
                       g_lastShownTenths == INT16_MIN ||
                       abs(tenths - g_lastShownTenths) >= (int)lroundf(REFRESH_DELTA_C * 10);

  Serial.printf("wake=%d T=%.2f RH=%.1f state=%d changed=%d\n", (int)cause, tempC, rh, st, changed);

  // Refresh the timestamp on every scheduled sample, including steady temperature.
  if (coldBoot || buttonWake || cause == ESP_SLEEP_WAKEUP_TIMER || changed) {
    drawScreen(tempC, rh, st);
    const bool fullClean = coldBoot || (g_refreshCount % FULL_CLEAN_EVERY == 0);
    if (pushFrame(fullClean)) {
      g_refreshCount++;
      g_lastState = st;
      g_lastShownTenths = tenths;
    } else {
      g_lastState = ST_UNKNOWN;
      g_lastShownTenths = INT16_MIN;
    }
  }
  goToSleep();
}

void loop() {}
