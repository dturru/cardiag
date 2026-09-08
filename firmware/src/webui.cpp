#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#include "webui.h"
#include "sniffer.h"
#include "config.h"

static WebServer g_server(80);
static bool      g_running = false;

// Built once per request rather than held as live state. At a few hundred
// milliseconds per poll the cost is irrelevant, and a snapshot cannot tear.
static char g_json[WEB_JSON_CAP];

// ---------------------------------------------------------------------------
// Page
//
// Served from flash as one string: no filesystem image, so flashing stays a
// single step and there is no way for the binary and the UI to drift apart.
// Vanilla JS on purpose -- a CDN is not reachable from an access point with no
// uplink, which is exactly the situation this page exists for.
// ---------------------------------------------------------------------------

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>cardiag</title>
<style>
:root{color-scheme:dark}
body{margin:0;background:#111;color:#ddd;font:13px/1.4 ui-monospace,Menlo,Consolas,monospace}
header{position:sticky;top:0;background:#181818;border-bottom:1px solid #333;padding:10px 12px}
h1{margin:0 0 6px;font-size:15px;color:#fff;letter-spacing:.5px}
#stat{color:#888;font-size:12px}
.btns{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}
button{flex:1;min-width:80px;padding:12px 10px;font:600 13px ui-monospace,monospace;
 background:#222;color:#ddd;border:1px solid #444;border-radius:6px}
button:active{background:#333}
button.on{border-color:#4a9;color:#6db}
#clear{background:#243;border-color:#4a9;color:#8ec}
table{width:100%;border-collapse:collapse}
td{padding:3px 4px;white-space:nowrap;border-bottom:1px solid #1e1e1e}
td.id{color:#8ab;font-weight:600}
td.meta{color:#666;font-size:11px;text-align:right}
.b{color:#bbb}
.hb{color:#3a3a3a}
.chg{color:#111;background:#e5c04a;border-radius:3px;padding:0 2px;font-weight:700}
#wrap{overflow-x:auto}
#err{color:#e66;padding:10px 12px}
</style></head><body>
<header>
<h1>cardiag <span id="mode" style="color:#6db"></span></h1>
<div id="stat">connecting...</div>
<div class="btns">
<button id="clear">CLEAR MARKS</button>
<button id="m2">SNIFF</button>
<button id="m1">LISTEN</button>
</div>
</header>
<div id="err"></div>
<div id="wrap"><table id="t"></table></div>
<script>
const $=s=>document.querySelector(s);
let stop=false;

function cell(row){
  let out='';
  for(let i=0;i<row.b.length;i++){
    const v=row.b[i].toString(16).toUpperCase().padStart(2,'0');
    if(row.h>>i&1)      out+='<span class="hb">··</span> ';
    else if(row.c>>i&1) out+='<span class="chg">'+v+'</span> ';
    else                out+='<span class="b">'+v+'</span> ';
  }
  return out;
}

async function tick(){
  if(stop)return;
  try{
    const j=await(await fetch('/api/table',{cache:'no-store'})).json();
    $('#err').textContent='';
    $('#mode').textContent=j.mode;
    $('#stat').textContent=j.ids+' ids · '+j.frames+' frames · missed '+j.missed
      +' · bus_err '+j.busErr+' · marked '+j.markedAgo+'s ago'
      +(j.overflow?' · OVERFLOW '+j.overflow:'');
    $('#t').innerHTML=j.rows.map(r=>
      '<tr><td class="id">0x'+r.id.toString(16).toUpperCase().padStart(3,'0')+'</td>'
      +'<td class="meta">'+r.per+'ms</td><td class="meta">'+r.n+'</td>'
      +'<td>'+cell(r)+'</td></tr>').join('');
    $('#m1').className=j.mode=='LISTEN'?'on':'';
    $('#m2').className=j.mode=='SNIFF'?'on':'';
  }catch(e){ $('#err').textContent='lost the board — '+e; }
}

const post=u=>fetch(u,{method:'POST'}).then(tick);
$('#clear').onclick=()=>post('/api/clear');
$('#m1').onclick=()=>post('/api/mode?m=1');
$('#m2').onclick=()=>post('/api/mode?m=2');

tick(); setInterval(tick, POLLMS);
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handleRoot() {
  // The poll interval lives in config.h, so patch it in rather than keeping a
  // second copy of the number inside the page.
  String page = FPSTR(PAGE);
  page.replace("POLLMS", String(WEB_POLL_HINT_MS));
  g_server.send(200, "text/html", page);
}

static void handleTable() {
  const size_t n = snifferSnapshotJson(g_json, sizeof(g_json),
                                       cardiagFrames(), cardiagMissed(),
                                       cardiagBusErr());
  (void)n;

  // The snapshot is a bare object; splice the mode in without a JSON library.
  String body = g_json;
  body.replace("{\"frames\"",
               String("{\"mode\":\"") + cardiagModeName() + "\",\"frames\"");
  g_server.send(200, "application/json", body);
}

static void handleClear() {
  snifferClearMarks();
  g_server.send(200, "text/plain", "ok");
}

static void handleMode() {
  const uint8_t m = (uint8_t)g_server.arg("m").toInt();
  if (!cardiagSetPassiveMode(m)) {
    // Reachable only by hand-crafting a request; refuse loudly rather than
    // silently ignoring, so the rule is visible from the outside.
    g_server.send(403, "text/plain", "passive modes only");
    return;
  }
  g_server.send(200, "text/plain", "ok");
}

// ---------------------------------------------------------------------------

void webuiStart() {
  if (g_running) return;

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL)) {
    Serial.println("AP failed to start.");
    WiFi.mode(WIFI_OFF);
    return;
  }

  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/api/table", HTTP_GET, handleTable);
  g_server.on("/api/clear", HTTP_POST, handleClear);
  g_server.on("/api/mode", HTTP_POST, handleMode);
  g_server.begin();

  g_running = true;
  Serial.printf("AP up: SSID \"%s\"  pass \"%s\"  ->  http://%s/\n",
                WIFI_AP_SSID, WIFI_AP_PASS, WiFi.softAPIP().toString().c_str());
}

void webuiStop() {
  if (!g_running) return;
  g_server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  g_running = false;
  Serial.println("AP down, radio off.");
}

void webuiLoop() {
  if (g_running) g_server.handleClient();
}

bool webuiRunning() { return g_running; }
