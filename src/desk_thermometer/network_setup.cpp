#include "network_setup.h"
#include "network_policy.h"
#include "clock_status.h"
#include "i2c_bsp.h"
#include "portal_page.h"
#include "json_string.h"
#include "portal_button.h"
#include "user_config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <esp_random.h>
#include <time.h>

static RTC_DATA_ATTR uint32_t syncCycles=0;
static volatile bool ntpReceived=false;
static void onNtp(struct timeval*) { ntpReceived=true; }
static uint8_t bcd(int n) { return (n/10)*16+n%10; }

static bool writeClock(int64_t epoch) {
  if (!validEpoch(epoch)) return false;
  // The external clock stores local China time; system/NTP timestamps are UTC.
  time_t local=epoch+8*3600;
  struct tm t={}; gmtime_r(&local,&t);
  uint8_t control=0;
  if(i2c_read_buff(rtc_dev_handle,0,&control,1)!=ESP_OK) return false;
  control &= ~0x22; // run, 24-hour mode; preserve other control flags
  if(i2c_write_buff(rtc_dev_handle,0,&control,1)!=ESP_OK) return false;
  uint8_t raw[]={bcd(t.tm_sec),bcd(t.tm_min),bcd(t.tm_hour),bcd(t.tm_mday),
                 bcd(t.tm_wday),bcd(t.tm_mon+1),bcd(t.tm_year-100)};
  if(i2c_write_buff(rtc_dev_handle,4,raw,7)!=ESP_OK) return false;
  uint8_t readback[7]={};
  if(i2c_read_buff(rtc_dev_handle,4,readback,7)!=ESP_OK) return false;
  ClockStamp stamp=decodeClock(control,readback);
  bool ok=stamp.valid && stamp.year==unsigned(t.tm_year+1900) &&
    stamp.month==unsigned(t.tm_mon+1) && stamp.day==unsigned(t.tm_mday) &&
    stamp.hour==unsigned(t.tm_hour) && stamp.minute==unsigned(t.tm_min);
  Serial.printf("clock_write=%s\n",ok?"OK":"ERROR");
  return ok;
}

static bool connectNetwork(const String &ssid,const String &password) {
  WiFi.begin(ssid.c_str(),password.c_str());
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<15000) delay(50);
  bool ok=WiFi.status()==WL_CONNECTED;
  Serial.printf("wifi_connect=%s\n",ok?"OK":"ERROR");
  return ok;
}

static bool syncNetworkTime() {
  ntpReceived=false;
  sntp_set_time_sync_notification_cb(onNtp);
  configTime(0,0,"ntp.aliyun.com","time.cloudflare.com","pool.ntp.org");
  uint32_t start=millis();
  while(!ntpReceived && millis()-start<12000) delay(50);
  bool ok=ntpReceived && writeClock(time(nullptr));
  esp_sntp_stop();
  Serial.printf("ntp_sync=%s\n",ok?"OK":"ERROR");
  return ok;
}

void serviceNetwork(bool forcePortal,bool coldBoot,bool clockValid,
                    void (*showPortal)(const char*,const char*)) {
  Preferences prefs;
  if(!prefs.begin("desk-net",false)) { Serial.println("network=NVS_ERROR"); return; }
  struct Credentials { char ssid[33]; char password[64]; } saved={};
  if(prefs.getBytesLength("credentials")==sizeof(saved)) prefs.getBytes("credentials",&saved,sizeof(saved));
  saved.ssid[32]=0; saved.password[63]=0;
  String ssid=saved.ssid, password=saved.password;
  bool configured=validCredentials(ssid.c_str(),password.c_str());
  // A portal is never opened just because the saved office network is absent.
  bool portal=forcePortal || (coldBoot && !configured);
  bool sync=shouldSync(configured,clockValid,syncCycles);
  ++syncCycles;
  if(!portal && !sync) { prefs.end(); return; }
  WiFi.persistent(false);
  WiFi.mode(portal?WIFI_AP_STA:WIFI_STA);
  if(!portal) {
    if(connectNetwork(ssid,password)) syncNetworkTime();
    syncCycles=1;
  } else {
    char apName[32], apPassword[9];
    snprintf(apName,sizeof(apName),"TempSign-%04X",unsigned(ESP.getEfuseMac()&0xffff));
    snprintf(apPassword,sizeof(apPassword),"%08lu",(unsigned long)(esp_random()%100000000u));
    if(!WiFi.softAP(apName,apPassword)) {
      Serial.println("portal=AP_ERROR"); WiFi.mode(WIFI_OFF); prefs.end(); return;
    }
    DNSServer dns; dns.start(53,"*",WiFi.softAPIP());
    // Only listen on the device hotspot, not on the company LAN.
    WebServer server(WiFi.softAPIP(),80);
    String token=String(esp_random(),HEX)+String(esp_random(),HEX);
    bool done=false;
    auto reply=[&](int code,const String &message) {
      server.sendHeader("Cache-Control","no-store");
      server.send(code,"text/plain; charset=utf-8",message);
    };
    auto authorized=[&]() { return server.arg("token")==token; };
    server.on("/",HTTP_GET,[&](){
      String page=FPSTR(PORTAL_PAGE); page.replace("@@TOKEN@@",token);
      server.sendHeader("Cache-Control","no-store");
      server.send(200,"text/html; charset=utf-8",page);
    });
    server.on("/scan",HTTP_POST,[&](){
      if(!authorized()) { reply(403,"页面已过期，请重新打开。"); return; }
      int count=WiFi.scanComplete();
      if(server.arg("start")=="1" && count!=WIFI_SCAN_RUNNING) {
        WiFi.scanDelete();
        count=WiFi.scanNetworks(true,false);
      }
      if(count==WIFI_SCAN_RUNNING) {reply(202,"正在扫描附近的2.4GHz网络……");return;}
      if(count<0) {reply(500,"扫描失败，请重试，或手动输入名称。");return;}
      String body="[";
      String seen[32]; int used=0;
      for(int i=0;i<count && used<32;++i) {
        String name=WiFi.SSID(i); if(name.isEmpty()) continue;
        bool duplicate=false;
        for(int j=0;j<used;++j) if(seen[j]==name) duplicate=true;
        if(duplicate) continue;
        if(used) body+=",";
        seen[used++]=name;
        body+="{\"ssid\":";body+=jsonString(name.c_str()).c_str();
        body+=",\"rssi\":";body+=String(WiFi.RSSI(i));body+="}";
      }
      body+="]";
      server.sendHeader("Cache-Control","no-store");
      server.send(200,"application/json; charset=utf-8",body);
      Serial.printf("wifi_scan=OK count=%d\n",used);
    });
    server.on("/save",HTTP_POST,[&](){
      if(!authorized()) { reply(403,"页面已过期，请重新打开。"); return; }
      if(WiFi.scanComplete()==WIFI_SCAN_RUNNING) {reply(409,"正在扫描，请稍后再连接。");return;}
      String newSsid=server.arg("ssid"),newPassword=server.arg("password");
      if(!validCredentials(newSsid.c_str(),newPassword.c_str())) {
        reply(400,"Wi-Fi名称需为1～32字节，密码需为8～63字节。"); return;
      }
      if(!connectNetwork(newSsid,newPassword)) {
        reply(400,"连接失败。请检查名称、密码和2.4GHz网络；原配置未替换。"); return;
      }
      // Save as one blob so power loss cannot mix an old SSID with a new password.
      Credentials next={};
      newSsid.toCharArray(next.ssid,sizeof(next.ssid));
      newPassword.toCharArray(next.password,sizeof(next.password));
      if(prefs.putBytes("credentials",&next,sizeof(next))!=sizeof(next)) {
        reply(500,"保存失败，请重试。"); return;
      }
      bool ok=syncNetworkTime();
      if(ok) { syncCycles=1; reply(200,"Wi-Fi已保存，时间已校准。可以返回温度牌。"); }
      else reply(200,"Wi-Fi已保存，但网络校时失败。请点“使用手机时间”，再返回温度牌。");
    });
    server.on("/time",HTTP_POST,[&](){
      if(!authorized()) { reply(403,"页面已过期，请重新打开。"); return; }
      String input=server.arg("epoch");
      char *end=nullptr; int64_t epoch=strtoll(input.c_str(),&end,10);
      if(input.isEmpty() || *end || !validEpoch(epoch)) { reply(400,"手机时间无效，请检查手机日期。"); return; }
      if(writeClock(epoch)) { syncCycles=1; reply(200,"已使用手机时间校准。可以返回温度牌。"); }
      else reply(500,"板载时钟写入失败，需要进一步检查。");
    });
    server.on("/done",HTTP_POST,[&](){
      if(!authorized()) { reply(403,"页面已过期，请重新打开。"); return; }
      reply(200,"正在返回温度牌，热点即将关闭。"); done=true;
    });
    server.onNotFound([&](){server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");});
    server.begin(); showPortal(apName,apPassword);
    Serial.println("portal=READY timeout=300s");
    uint32_t start=millis();
    PortalExitButton exitButton;
    while(!done && millis()-start<300000) {
      dns.processNextRequest();server.handleClient();
      if(exitButton.update(digitalRead((int)ext_wakeup_pin_1)==LOW,millis())) {
        Serial.println("portal=BUTTON_EXIT");done=true;
      }
      delay(5);
    }
    delay(200);server.stop();dns.stop();WiFi.scanDelete();WiFi.softAPdisconnect(true);
    Serial.println("portal=CLOSED");
  }
  WiFi.disconnect(true,false); WiFi.mode(WIFI_OFF); prefs.end();
}
