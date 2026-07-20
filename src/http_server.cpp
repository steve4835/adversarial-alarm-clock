#include "globals.h"
#include "http_server.h"
#include "schedule.h"
#include "alarm.h"
#include "keep_awake.h"
#include "config.h"
#include <WiFi.h>

void setupHttp() {

  // Root — web UI
  httpServer.on("/", HTTP_GET, []() {
    String html = R"(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>alarm clock</title>
<style>
  body{font-family:monospace;max-width:480px;margin:2em auto;padding:1em}
  h1{font-size:1.2em}
  table{width:100%;border-collapse:collapse}
  td,th{padding:0.4em;border:1px solid #ccc;text-align:center}
  input[type=time]{font-size:1em}
  input[type=checkbox]{transform:scale(1.3)}
  button{padding:0.5em 1.5em;font-size:1em;margin-top:1em;cursor:pointer}
  .dismiss{background:#c00;color:#fff;}
</style></head><body>
<h1>Adversarial Alarm Clock</h1>
<form method='POST' action='/alarm/ui'>
<table>
<tr><th>Day</th><th>Time</th><th>On</th></tr>)";

    const char* dayNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    for (int d = 0; d < 7; d++) {
      html += "<tr><td>" + String(dayNames[d]) + "</td><td>";
      html += "<input type='time' name='t" + String(d) + "' value='";
      char t[6];
      snprintf(t, sizeof(t), "%02d:%02d", schedule[d].hour, schedule[d].minute);
      html += String(t) + "'></td><td>";
      html += "<input type='checkbox' name='e" + String(d) + "'";
      if (schedule[d].enabled) html += " checked";
      html += "></td></tr>";
    }
    html += R"(</table>
<button type='submit'>Save Schedule</button>
</form>
<div id='lock-msg' style='display:none;color:#c00;margin:0.5em 0'>Next alarm fires within 1 hour &mdash; that row is locked.</div>
<br>)";
    const char* dismissBtn;
    if (SHOW_DISMISS_ON_WEB) {
      dismissBtn = "<form method='POST' action='/dismiss'><button class='dismiss' type='submit'>Dismiss / Cancel Today</button></form>";
    } else {
      dismissBtn = "";
    }
    char dismissSection[256];
    snprintf(dismissSection, sizeof(dismissSection), "%s<hr>", dismissBtn);
    html += dismissSection;
    html += R"(
<button id='ka-dismiss' class='dismiss' style='display:none' onclick='dismissKeepAwake()'>Dismiss Keep-Awake Chirp</button>
<div id='diag' style='font-size:0.85em;color:#555'>Loading status...</div>
<script>
function dismissKeepAwake(){
  fetch('/dismissKeepAwake',{method:'POST'}).then(refresh);
}
function refresh(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('diag').innerHTML=
      'Time: <b>'+d.time+'</b> &nbsp; State: <b>'+d.state+'</b><br>'+
      'Cancelled today: <b>'+d.cancelled+'</b> &nbsp; RTC ok: <b>'+d.rtc+'</b><br>'+
      (d.rtc_power_lost ? '<b style="color:orange">⚠ RTC lost power — time may be drifted (pending NTP sync)</b><br>' : '')+
      'Next: <b>'+d.next_day+' '+d.next_alarm+'</b><br>'+
      'NTP sync: <b>'+d.ntp_sync+'</b> &nbsp; WiFi: '+d.wifi_rssi+' dBm'+
      (d.keep_awake ? '<br><b style="color:#c00">Keep-awake active</b>' : '');
    var ld=d.locked_day;
    document.getElementById('lock-msg').style.display=(ld>=0)?'':'none';
    document.getElementById('ka-dismiss').style.display=d.keep_awake_dismissable?'':'none';
    for(var i=0;i<7;i++){
      var lock=(i===ld);
      var t=document.querySelector('input[name="t'+i+'"]');
      var e=document.querySelector('input[name="e'+i+'"]');
      if(t)t.disabled=lock;
      if(e)e.disabled=lock;
    }
  }).catch(()=>{ document.getElementById('diag').innerHTML='(status unavailable)'; });
}
refresh(); setInterval(refresh,1000);
</script>
</body></html>)";
    httpServer.send(200, "text/html", html);
  });

  // Web UI form POST — saves all days at once
  httpServer.on("/alarm/ui", HTTP_POST, []() {
    struct tm tm;
    int lockedDay = getLocalTime(&tm) ? alarmImminent(tm) : -1;
    for (int d = 0; d < 7; d++) {
      if (d == lockedDay) continue;
      String tKey = "t" + String(d);
      String eKey = "e" + String(d);
      if (httpServer.hasArg(tKey)) {
        String t = httpServer.arg(tKey); // "HH:MM"
        if (t.length() >= 5 && t[2] == ':') {
          schedule[d].hour   = t.substring(0, 2).toInt();
          schedule[d].minute = t.substring(3, 5).toInt();
        }
        schedule[d].enabled = httpServer.hasArg(eKey);
        saveDay(d);
      }
    }
    resetTodayCancelledIfSafe();
    logNextAlarm();
    httpServer.sendHeader("Location", "/");
    httpServer.send(303);
  });

  // API: POST /alarm?day=mon&h=6&m=30[&enabled=0]
  httpServer.on("/alarm", HTTP_POST, []() {
    if (!httpServer.hasArg("day")) {
      httpServer.send(400, "text/plain", "missing ?day=");
      return;
    }
    int d = dayIndex(httpServer.arg("day").c_str());
    if (d < 0) {
      httpServer.send(400, "text/plain", "unknown day — use sun/mon/tue/wed/thu/fri/sat");
      return;
    }
    {
      struct tm tm;
      if (getLocalTime(&tm) && d == alarmImminent(tm)) {
        httpServer.send(409, "text/plain", "alarm fires within 60 minutes — changes locked");
        return;
      }
    }
    if (httpServer.hasArg("h")) schedule[d].hour   = httpServer.arg("h").toInt();
    if (httpServer.hasArg("m")) schedule[d].minute = httpServer.arg("m").toInt();
    schedule[d].enabled = httpServer.hasArg("enabled")
                          ? (httpServer.arg("enabled").toInt() != 0)
                          : true;
    saveDay(d);
    resetTodayCancelledIfSafe();
    logNextAlarm();
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %02d:%02d enabled=%d",
      DAY_KEYS[d], schedule[d].hour, schedule[d].minute, schedule[d].enabled);
    httpServer.send(200, "text/plain", buf);
  });

  // Dismiss / pre-empt — body must contain DISMISS_TOKEN somewhere
  auto handleDismiss = []() {
    if (httpServer.arg("plain").indexOf(DISMISS_TOKEN) < 0) {
      httpServer.send(403, "text/plain", "forbidden");
    } else {
      httpServer.send(200, "text/plain", "ok");
      dismiss();
    }
  };
  httpServer.on("/dismiss", HTTP_POST, handleDismiss);

  // Dismiss the current/next keep-awake chirp — no body required.
  httpServer.on("/dismissKeepAwake", HTTP_POST, []() {
    if (dismissKeepAwake()) {
      httpServer.send(200, "text/plain", "ok");
    } else {
      httpServer.send(409, "text/plain", "not within a keep-awake dismiss window");
    }
  });

  // Status JSON
  httpServer.on("/status", HTTP_GET, []() {
    char timeStr[9]  = "--:--:--";
    char nextDay[4]  = "---";
    char nextTime[6] = "--:--";
    int  lockedDay   = -1;

    {
      struct tm tm;
      if (getLocalTime(&tm)) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
          tm.tm_hour, tm.tm_min, tm.tm_sec);
        int outDay; uint8_t outH, outM;
        if (nextAlarm(tm.tm_wday, tm.tm_hour, tm.tm_min, todayCancelled,
                      outDay, outH, outM)) {
          strncpy(nextDay, DAY_KEYS[outDay], sizeof(nextDay) - 1);
          snprintf(nextTime, sizeof(nextTime), "%02d:%02d", outH, outM);
        }
        lockedDay = alarmImminent(tm);
      }
    }

    char ntpSyncStr[32] = "never";
    if (lastNtpSync > 0) {
      unsigned long elapsed = millis() - lastNtpSync;
      if (elapsed > 7UL * 24 * 60 * 60 * 1000) {
        strncpy(ntpSyncStr, "over 7 days ago", sizeof(ntpSyncStr) - 1);
      } else {
        unsigned long secs = elapsed / 1000;
        unsigned long mins = secs / 60;
        unsigned long hrs  = mins / 60;
        if (hrs > 0)
          snprintf(ntpSyncStr, sizeof(ntpSyncStr), "%luh %02lum ago", hrs, mins % 60);
        else if (mins > 0)
          snprintf(ntpSyncStr, sizeof(ntpSyncStr), "%lum ago", mins);
        else
          snprintf(ntpSyncStr, sizeof(ntpSyncStr), "%lus ago", secs);
      }
    }

    char buf[760];
    snprintf(buf, sizeof(buf),
      "{\"time\":\"%s\",\"state\":\"%s\",\"cancelled\":%s,"
      "\"locked_day\":%d,"
      "\"rtc\":%s,\"rtc_power_lost\":%s,\"wifi_rssi\":%d,"
      "\"next_day\":\"%s\",\"next_alarm\":\"%s\","
      "\"ntp_sync\":\"%s\","
      "\"first_synced_at\":\"%s\","
      "\"keep_awake\":%s,\"keep_awake_dismissable\":%s,"
      "\"hw_info\":"
      "{\"cpu_mhz\":%u,\"chip_model\":\"%s\",\"chip_rev\":%d,\"cores\":%d,"
      "\"flash_size\":%u,\"flash_speed\":%u,"
      "\"heap_free\":%u,\"heap_min_free\":%u,\"heap_max_alloc\":%u}"
      "}",
      timeStr,
      alarmState == IDLE ? "idle" : "alarm",
      todayCancelled ? "true" : "false",
      lockedDay,
      rtcAvailable   ? "true" : "false",
      rtcPowerLost   ? "true" : "false",
      WiFi.RSSI(),
      nextDay, nextTime,
      ntpSyncStr,
      firstSyncTime,
      isKeepAwakeActive()          ? "true" : "false",
      keepAwakeDismissWindowOpen() ? "true" : "false",
      ESP.getCpuFreqMHz(), ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
      ESP.getFlashChipSize(), ESP.getFlashChipSpeed(),
      ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    httpServer.send(200, "application/json", buf);
  });

  httpServer.begin();
  Serial.printf("HTTP server: http://%s/\n", WiFi.localIP().toString().c_str());
}
