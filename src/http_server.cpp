#include "globals.h"
#include "http_server.h"
#include "schedule.h"
#include "alarm.h"
#include "config.h"
#include <WiFi.h>

void setupHttp() {

  // Root — web UI
  httpServer.on("/", HTTP_GET, []() {
    String html = R"(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Alarm Clock</title>
<style>
  body{font-family:monospace;max-width:480px;margin:2em auto;padding:1em}
  h1{font-size:1.2em}
  table{width:100%;border-collapse:collapse}
  td,th{padding:0.4em;border:1px solid #ccc;text-align:center}
  input[type=time]{font-size:1em}
  input[type=checkbox]{transform:scale(1.3)}
  button{padding:0.5em 1.5em;font-size:1em;margin-top:1em;cursor:pointer}
  .dismiss{background:#c00;color:#fff;border:none;border-radius:4px}
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
<br>
<form method='POST' action='/dismiss'>
<button class='dismiss' type='submit'>Dismiss / Cancel Today</button>
</form>
<hr>
<div id='diag' style='font-size:0.85em;color:#555'>Loading status...</div>
<script>
function refresh(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('diag').innerHTML=
      'Time: <b>'+d.time+'</b> &nbsp; State: <b>'+d.state+'</b><br>'+
      'Cancelled today: <b>'+d.cancelled+'</b> &nbsp; RTC ok: <b>'+d.rtc+'</b><br>'+
      'Next: <b>'+d.next_day+' '+d.next_alarm+'</b><br>'+
      'NTP sync: <b>'+d.ntp_sync+'</b> &nbsp; WiFi: '+d.wifi_rssi+' dBm &nbsp; <small>(refreshes every 1s)</small>';
  }).catch(()=>{ document.getElementById('diag').innerHTML='(status unavailable)'; });
}
refresh(); setInterval(refresh,1000);
</script>
</body></html>)";
    httpServer.send(200, "text/html", html);
  });

  // Web UI form POST — saves all days at once
  httpServer.on("/alarm/ui", HTTP_POST, []() {
    for (int d = 0; d < 7; d++) {
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

  // Dismiss / pre-empt
  auto handleDismiss = []() {
    httpServer.send(200, "text/plain", "ok");
    dismiss();
  };
  httpServer.on("/dismiss", HTTP_POST, handleDismiss);

  // Status JSON
  httpServer.on("/status", HTTP_GET, []() {
    char timeStr[9]  = "--:--:--";
    char nextDay[4]  = "---";
    char nextTime[6] = "--:--";

    if (rtcAvailable) {
      DateTime now = rtc.now();
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
        now.hour(), now.minute(), now.second());
      int outDay; uint8_t outH, outM;
      if (nextAlarm(now.dayOfTheWeek(), now.hour(), now.minute(), todayCancelled,
                    outDay, outH, outM)) {
        strncpy(nextDay, DAY_KEYS[outDay], sizeof(nextDay) - 1);
        snprintf(nextTime, sizeof(nextTime), "%02d:%02d", outH, outM);
      }
    }

    char ntpSyncStr[32] = "never";
    if (lastNtpSync > 0) {
      unsigned long elapsed = millis() - lastNtpSync;
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

    char buf[384];
    snprintf(buf, sizeof(buf),
      "{\"time\":\"%s\",\"state\":\"%s\",\"cancelled\":%s,"
      "\"rtc\":%s,\"wifi_rssi\":%d,"
      "\"next_day\":\"%s\",\"next_alarm\":\"%s\","
      "\"ntp_sync\":\"%s\"}",
      timeStr,
      alarmState == IDLE ? "idle" : "alarm",
      todayCancelled ? "true" : "false",
      rtcAvailable   ? "true" : "false",
      WiFi.RSSI(),
      nextDay, nextTime,
      ntpSyncStr);
    httpServer.send(200, "application/json", buf);
  });

  httpServer.begin();
  Serial.printf("HTTP server: http://%s/\n", WiFi.localIP().toString().c_str());
}
