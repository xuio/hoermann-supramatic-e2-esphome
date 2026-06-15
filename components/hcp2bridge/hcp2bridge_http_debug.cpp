#include "hcp2bridge.h"
#include "hcp2bridge_internal.h"
#include "hcp2_entity_mapping.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#ifdef USE_ESP32
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#endif

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

#ifdef USE_ESP32
std::string HCP2Bridge::http_debug_door_json_() {
  hcp2_drive_status_t status = this->drive_status_snapshot_();
  std::string json = "{\"target_position_raw\":";
  json += std::to_string(status.target_position);
  json += ",\"current_position_raw\":";
  json += std::to_string(status.current_position);
  json += ",\"state_raw\":";
  json += std::to_string(status.state);
  json += ",\"state\":\"";
  json += hcp2_state_name(static_cast<hcp2_drive_state_code_t>(status.state));
  json += "\",\"light\":";
  json += status.light_on ? "true" : "false";
  json += ",\"obstruction\":";
  json += this->is_obstructed() ? "true" : "false";
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_lp_json_() {
  std::string json = "{\"health_flags\":";
  json += std::to_string(this->get_lp_health_flags());
  json += ",\"max_rx_fifo\":";
  json += std::to_string(this->get_lp_max_rx_fifo_count());
  json += ",\"max_loop_us\":";
  json += std::to_string(this->get_lp_max_loop_us());
  json += ",\"max_poll_rx_to_schedule_us\":";
  json += std::to_string(this->get_lp_max_poll_rx_to_schedule_us());
  json += ",\"max_response_schedule_to_tx_start_us\":";
  json += std::to_string(this->get_lp_max_response_schedule_to_tx_start_us());
  json += ",\"max_response_tx_us\":";
  json += std::to_string(this->get_lp_max_response_tx_us());
  json += ",\"max_de_hold_us\":";
  json += std::to_string(this->get_lp_max_de_hold_us());
  json += ",\"last_poll_age_ms\":";
  json += std::to_string(this->get_lp_last_poll_age_ms());
  json += ",\"loop_overruns\":";
  json += std::to_string(this->get_lp_loop_overrun_count());
  json += ",\"rx_starvations\":";
  json += std::to_string(this->get_lp_rx_starvation_count());
  json += ",\"stuck_de_recoveries\":";
  json += std::to_string(this->get_lp_stuck_de_count());
  json += ",\"mailbox_repairs\":";
  json += std::to_string(this->get_lp_mailbox_repair_count());
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_hp_json_() {
  std::string json = "{\"resets\":";
  json += std::to_string(this->get_hp_reset_count());
  json += ",\"panic_resets\":";
  json += std::to_string(this->get_hp_panic_reset_count());
  json += ",\"wdt_resets\":";
  json += std::to_string(this->get_hp_wdt_reset_count());
  json += ",\"brownout_resets\":";
  json += std::to_string(this->get_hp_brownout_reset_count());
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_health_json_() {
  const bool lp_mode = !this->hp_fallback_;
  const uint32_t polls_seen = this->get_lp_poll_count();
  const uint32_t polls_answered = this->get_lp_response_count();
  const uint32_t missed_polls = this->get_lp_missed_poll_count();
  const uint32_t raw_missed_polls = this->get_lp_raw_missed_poll_count();
  const bool pending_response = this->has_lp_pending_response();
  const uint32_t health_flags = this->get_lp_health_flags();
  const uint32_t tx_aborts = this->get_lp_tx_abort_count();
  const uint32_t collisions = this->get_lp_collision_count();
  const uint32_t loop_overruns = this->get_lp_loop_overrun_count();
  const uint32_t rx_starvations = this->get_lp_rx_starvation_count();
  const uint32_t stuck_de = this->get_lp_stuck_de_count();
  const uint32_t last_poll_age_ms = this->get_lp_last_poll_age_ms();
  const uint32_t max_de_hold_us = this->get_lp_max_de_hold_us();
  const bool bus_online = this->is_bus_online();
  const bool valid_broadcast = this->has_valid_broadcast();
  const bool lp_seen = polls_seen > 0u && this->get_lp_heartbeat() > 0u;

  std::string reasons = "[";
  bool first_reason = true;
  const auto add_reason = [&](const char *reason) {
    if (!first_reason) {
      reasons += ",";
    }
    first_reason = false;
    reasons += "\"";
    reasons += reason;
    reasons += "\"";
  };

  if (!lp_mode) {
    add_reason("hp_fallback_enabled");
  }
  if (!lp_seen) {
    add_reason("lp_not_seen");
  }
  if (!bus_online) {
    add_reason("bus_offline");
  }
  if (!valid_broadcast) {
    add_reason("no_broadcast_state");
  }
  if (last_poll_age_ms > (HCP2BRIDGE_BUS_ONLINE_TIMEOUT_US / 1000u)) {
    add_reason("last_poll_stale");
  }
  if (missed_polls != 0u) {
    add_reason("missed_polls");
  }
  if (health_flags != 0u) {
    add_reason("lp_health_flags");
  }
  if (tx_aborts != 0u) {
    add_reason("tx_aborts");
  }
  if (collisions != 0u) {
    add_reason("collisions");
  }
  if (loop_overruns != 0u) {
    add_reason("loop_overruns");
  }
  if (rx_starvations != 0u) {
    add_reason("rx_starvations");
  }
  if (stuck_de != 0u) {
    add_reason("stuck_de_recoveries");
  }
  if (max_de_hold_us > 0u && max_de_hold_us > HCP2BRIDGE_MAX_DE_HIGH_US) {
    add_reason("de_hold_too_long");
  }
  reasons += "]";

  const bool safe_for_ota_restart = first_reason;
  std::string json = "{\"verdict\":\"";
  json += safe_for_ota_restart ? "ok" : "fail";
  json += "\",\"safe_for_ota_restart\":";
  json += safe_for_ota_restart ? "true" : "false";
  json += ",\"reasons\":";
  json += reasons;
  json += ",\"checks\":{\"lp_mode\":";
  json += lp_mode ? "true" : "false";
  json += ",\"lp_seen\":";
  json += lp_seen ? "true" : "false";
  json += ",\"bus_online\":";
  json += bus_online ? "true" : "false";
  json += ",\"valid_broadcast\":";
  json += valid_broadcast ? "true" : "false";
  json += ",\"last_poll_age_ms\":";
  json += std::to_string(last_poll_age_ms);
  json += ",\"polls_seen\":";
  json += std::to_string(polls_seen);
  json += ",\"polls_answered\":";
  json += std::to_string(polls_answered);
  json += ",\"missed_polls\":";
  json += std::to_string(missed_polls);
  json += ",\"raw_missed_polls\":";
  json += std::to_string(raw_missed_polls);
  json += ",\"pending_response\":";
  json += pending_response ? "true" : "false";
  json += ",\"health_flags\":";
  json += std::to_string(health_flags);
  json += ",\"tx_aborts\":";
  json += std::to_string(tx_aborts);
  json += ",\"collisions\":";
  json += std::to_string(collisions);
  json += ",\"loop_overruns\":";
  json += std::to_string(loop_overruns);
  json += ",\"rx_starvations\":";
  json += std::to_string(rx_starvations);
  json += ",\"stuck_de_recoveries\":";
  json += std::to_string(stuck_de);
  json += ",\"max_de_hold_us\":";
  json += std::to_string(max_de_hold_us);
  json += "},\"stats\":";
  json += this->http_debug_stats_json_();
  json += ",\"door\":";
  json += this->http_debug_door_json_();
  json += ",\"lp\":";
  json += this->http_debug_lp_json_();
  json += ",\"hp\":";
  json += this->http_debug_hp_json_();
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_stats_json_() {
  std::string json = "{\"protocol\":\"hcp2\",\"mode\":\"";
  json += this->hp_fallback_ ? "hp_fallback" : "lp";
  json += "\",\"uptime_ms\":";
  json += std::to_string(millis());
  json += ",\"bus_online\":";
  json += this->is_bus_online() ? "true" : "false";
  json += ",\"valid_broadcast\":";
  json += this->has_valid_broadcast() ? "true" : "false";
  json += ",\"state\":\"";
  json += this->get_state_string();
  json += "\",\"position\":";
  json += std::to_string(this->get_position());
  json += ",\"polls_seen\":";
  json += std::to_string(this->get_lp_poll_count());
  json += ",\"polls_answered\":";
  json += std::to_string(this->get_lp_response_count());
  json += ",\"missed_polls\":";
  json += std::to_string(this->get_lp_missed_poll_count());
  json += ",\"raw_missed_polls\":";
  json += std::to_string(this->get_lp_raw_missed_poll_count());
  json += ",\"pending_response\":";
  json += this->has_lp_pending_response() ? "true" : "false";
  json += ",\"crc_errors\":";
  json += std::to_string(this->get_lp_crc_error_count());
  json += ",\"rx_errors\":";
  json += std::to_string(this->get_lp_rx_error_count());
  json += ",\"tx_aborts\":";
  json += std::to_string(this->get_lp_tx_abort_count());
  json += ",\"collisions\":";
  json += std::to_string(this->get_lp_collision_count());
  json += ",\"lp_heartbeat\":";
  json += std::to_string(this->get_lp_heartbeat());
  json += ",\"lp_resets\":";
  json += std::to_string(this->get_lp_reset_count());
  json += ",\"hp_resets\":";
  json += std::to_string(this->get_hp_reset_count());
  json += ",\"protocol_log\":";
  json += this->protocol_log_summary_json_();
  json += ",\"websocket\":{\"connected\":";
  json += (this->http_debug_log_ws_client_ != nullptr ? "true" : "false");
  json += ",\"connects\":";
  json += std::to_string(this->http_debug_log_ws_connect_count_);
  json += ",\"disconnects\":";
  json += std::to_string(this->http_debug_log_ws_disconnect_count_);
  json += ",\"rejects\":";
  json += std::to_string(this->http_debug_log_ws_reject_count_);
  json += ",\"peer_closes\":";
  json += std::to_string(this->http_debug_log_ws_peer_close_count_);
  json += ",\"read_failures\":";
  json += std::to_string(this->http_debug_log_ws_read_fail_count_);
  json += ",\"write_failures\":";
  json += std::to_string(this->http_debug_log_ws_write_fail_count_);
  json += ",\"last_errno\":";
  json += std::to_string(this->http_debug_log_ws_last_errno_);
  json += ",\"last_close_reason\":\"";
  json += this->http_debug_log_ws_last_close_reason_;
  json += "\"}";
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_support_json_() {
  std::string json = "{\"device\":\"hcp2bridge\",\"target\":\"supramatic_series_4\",\"stats\":";
  json += this->http_debug_stats_json_();
  json += ",\"health\":";
  json += this->http_debug_health_json_();
  json += ",\"door\":";
  json += this->http_debug_door_json_();
  json += ",\"lp\":";
  json += this->http_debug_lp_json_();
  json += ",\"hp\":";
  json += this->http_debug_hp_json_();
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_index_html_() {
  return R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HCP2 Bridge Debug</title>
<link rel="icon" href="data:,">
<style>
:root{color-scheme:light dark;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#111827;color:#e5e7eb}
body{margin:0;padding:18px;line-height:1.35}
h1{font-size:22px;margin:0 0 12px}
h2{font-size:15px;margin:0 0 10px;color:#cbd5e1}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
.panel{border:1px solid #334155;border-radius:8px;background:#0f172a;padding:12px}
.status{display:inline-flex;align-items:center;gap:8px;border-radius:999px;padding:7px 10px;font-weight:700}
.ok{background:#064e3b;color:#d1fae5}.fail{background:#7f1d1d;color:#fee2e2}.unknown{background:#374151;color:#f3f4f6}
.row{display:flex;justify-content:space-between;gap:12px;border-bottom:1px solid #1f2937;padding:5px 0;font-size:13px}
.row:last-child{border-bottom:0}
.key{color:#94a3b8}.value{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;text-align:right}
button,a.button{display:inline-block;margin:0 6px 8px 0;border:1px solid #475569;border-radius:6px;background:#1e293b;color:#e5e7eb;padding:7px 10px;text-decoration:none;cursor:pointer}
button:hover,a.button:hover{background:#334155}
label{font-size:13px;color:#cbd5e1}
	pre{white-space:pre-wrap;overflow:auto;max-height:48vh;background:#020617;border:1px solid #1f2937;border-radius:6px;padding:10px;font-size:12px}
	#log{height:60vh;max-height:none;overflow-anchor:none}
.muted{color:#94a3b8;font-size:12px}
.reasons{margin-top:8px;color:#fecaca;font-size:13px}
</style>
</head>
<body>
<h1>HCP2 Bridge Debug</h1>
<div class="panel">
  <span id="verdict" class="status unknown">loading</span>
  <span id="updated" class="muted"></span>
  <div id="reasons" class="reasons"></div>
</div>
<div class="grid" style="margin-top:12px">
  <section class="panel"><h2>Health</h2><div id="continuity"></div></section>
  <section class="panel"><h2>Door</h2><div id="door"></div></section>
  <section class="panel"><h2>Checks</h2><div id="counters"></div></section>
  <section class="panel"><h2>LP Core</h2><div id="timing"></div></section>
  <section class="panel"><h2>Stats</h2><div id="statsPanel"></div></section>
  <section class="panel"><h2>Protocol Log</h2><div id="protocolPanel"></div></section>
  <section class="panel"><h2>HP</h2><div id="hpPanel"></div></section>
</div>
<section class="panel" style="margin-top:12px">
  <h2>RAM Protocol Log</h2>
  <button onclick="controlLog('start')">Start</button>
  <button onclick="controlLog('stop')">Stop</button>
  <button onclick="controlLog('clear')">Clear</button>
  <button onclick="refreshLog()">Refresh Log</button>
	  <button onclick="connectLogStream(true)">Reconnect Stream</button>
  <button onclick="downloadCachedLog()">Download JSON</button>
  <a class="button" href="/hcp2_log" download="hcp2-log.ndjson">Device NDJSON</a>
  <a class="button" href="/hcp2_log.bin" download="hcp2-log.bin">Device Raw</a>
  <div id="logSummary" class="muted"></div>
  <div id="logStream" class="muted">stream disconnected</div>
  <div id="logCache" class="muted">cache empty</div>
  <pre id="log"></pre>
</section>
<section class="panel" style="margin-top:12px">
  <h2>Raw JSON</h2>
  <button onclick="loadRaw('/health')">Health</button>
  <button onclick="loadRaw('/stats')">Stats</button>
  <button onclick="loadRaw('/support')">Support</button>
  <pre id="raw"></pre>
</section>
<script>
const $=id=>document.getElementById(id);
let logSocket=null;
let logReconnectTimer=null;
let logLines=[];
let logCache=[];
let logCacheBytes=0;
let refreshBusy=false;
let logLoadBusy=false;
let rawPath=null;
let rawBusy=false;
let lastHealthStreamMs=0;
let lastHealthOkMs=0;
	let logReconnectDelayMs=1000;
	let logRenderQueued=false;
	let logSeenSeqs=new Set();
	let logPendingAppend=[];
	let logPendingReplace=null;
	let logRenderStickToBottom=true;
	const logStickThresholdPx=24;
	const maxCacheAgeMs=30*60*1000;
	const maxCacheBytes=100*1024*1024;
function row(k,v){return `<div class="row"><span class="key">${k}</span><span class="value">${v??''}</span></div>`}
function setRows(id,items){$(id).innerHTML=items.map(([k,v])=>row(k,v)).join('')}
function label(k){return String(k).replaceAll('_',' ')}
function fmt(v){
  if(v===undefined||v===null)return '';
  if(Array.isArray(v))return v.join(', ');
  if(typeof v==='number')return Number.isInteger(v)?String(v):v.toFixed(3);
  if(typeof v==='object')return JSON.stringify(v);
  return String(v);
}
function objectRows(obj,priority=[]){
  const source=obj||{};
  const seen=new Set();
  const rows=[];
  function add(k){
    if(seen.has(k)||!Object.prototype.hasOwnProperty.call(source,k))return;
    const v=source[k];
    if(v&&typeof v==='object'&&!Array.isArray(v))return;
    seen.add(k);
    rows.push([label(k),fmt(v)]);
  }
  priority.forEach(add);
  Object.keys(source).sort().forEach(add);
  return rows;
}
function setObjectRows(id,obj,priority=[]){setRows(id,objectRows(obj,priority))}
function sleep(ms){return new Promise(resolve=>setTimeout(resolve,ms))}
async function getJson(path,attempts=3){
  let lastError=null;
  for(let attempt=0;attempt<attempts;attempt++){
    try{
      const r=await fetch(path,{cache:'no-store'});
      const t=await r.text();
      try{return JSON.parse(t)}catch(e){throw new Error(`${path} returned ${r.status}: ${t.slice(0,48)||'non-JSON'}`)}
    }catch(e){
      lastError=e;
      if(attempt+1<attempts)await sleep(150*(attempt+1));
    }
  }
  throw lastError||new Error(path+' unavailable');
}
function applyHealth(health,source='http'){
  const stats=health.stats||{};
  const ok=health.verdict==='ok';
  if(source==='stream')lastHealthStreamMs=Date.now();
  lastHealthOkMs=Date.now();
  $('verdict').className='status '+(ok?'ok':'fail');
  $('verdict').textContent=ok?'continuity ok':'continuity problem';
  $('updated').textContent=' updated '+new Date().toLocaleTimeString()+` via ${source}`;
  $('reasons').textContent=(health.reasons&&health.reasons.length)?'Reasons: '+health.reasons.join(', '):'';
  const c=health.checks||{};
  const door=Object.assign({position:stats.position,state:stats.state},health.door||{});
  const lp=health.lp||{};
  const hp=health.hp||{};
  const p=stats.protocol_log||{};
  setObjectRows('continuity',{verdict:health.verdict,safe_for_ota_restart:health.safe_for_ota_restart,reasons:health.reasons||[]},['verdict','safe_for_ota_restart','reasons']);
  setObjectRows('door',door,['state','position','state_raw','target_position_raw','current_position_raw','light','obstruction']);
  setObjectRows('counters',c,['lp_mode','lp_seen','bus_online','valid_broadcast','polls_seen','polls_answered','pending_response','raw_missed_polls','missed_polls']);
  setObjectRows('timing',lp,['health_flags','max_loop_us','max_rx_fifo','max_poll_rx_to_schedule_us','max_response_schedule_to_tx_start_us','max_response_tx_us','max_de_hold_us','last_poll_age_ms']);
  setObjectRows('statsPanel',stats,['protocol','mode','uptime_ms','bus_online','valid_broadcast','state','position','polls_seen','polls_answered','missed_polls','raw_missed_polls','pending_response']);
  setObjectRows('protocolPanel',p,['enabled','used','capacity','overwritten_records','overwritten_bytes','dropped_records','dropped_bytes','next_seq','storage','mode','flash_writes','ready']);
  setObjectRows('hpPanel',hp,['resets','panic_resets','wdt_resets','brownout_resets']);
  $('logSummary').textContent=`log ${p.enabled?'enabled':'disabled'}, ${p.used||0}/${p.capacity||0} bytes, overwritten ${p.overwritten_records||0} records`;
  updateLogCacheSummary();
}
function requestRefresh(){refresh().catch(()=>{})}
async function refresh(){
  if(refreshBusy)return;
  refreshBusy=true;
  try{
    const health=await getJson('/health');
    applyHealth(health,'http');
  }catch(e){
    if(!lastHealthOkMs){
      $('verdict').className='status unknown';
      $('verdict').textContent='debug fetch failed';
    }
    $('updated').textContent=' last fetch failed '+new Date().toLocaleTimeString();
    $('reasons').textContent=e.message;
  }finally{
    refreshBusy=false;
  }
}
function resetLogCache(){logCache=[];logCacheBytes=0;logSeenSeqs=new Set();updateLogCacheSummary()}
function pruneLogCache(now=Date.now()){
  const minMs=now-maxCacheAgeMs;
  while(logCache.length&&(logCache[0].received_ms<minMs||logCacheBytes>maxCacheBytes)){
    const removed=logCache.shift();
    logCacheBytes-=removed.bytes||0;
  }
  if(logCacheBytes<0)logCacheBytes=0;
}
function updateLogCacheSummary(){
  pruneLogCache();
  const kb=(logCacheBytes/1024).toFixed(1);
  const oldest=logCache[0]?.received_at||'none';
  const newest=logCache[logCache.length-1]?.received_at||'none';
	  $('logCache').textContent=`visible ${logLines.length} lines; browser cache ${logCache.length} records, ${kb} KiB, newest 30 min / 100 MiB, ${oldest} to ${newest}`;
	}
function cacheLogLine(line,receivedMs=Date.now()){
  if(!line)return false;
  let entry=null;
  try{entry=JSON.parse(line)}catch(e){}
  if(entry&&typeof entry.seq==='number'){
    if(logSeenSeqs.has(entry.seq))return false;
    logSeenSeqs.add(entry.seq);
  }
  const record={received_at:new Date(receivedMs).toISOString(),received_ms:receivedMs,bytes:line.length,raw:line};
  if(entry&&typeof entry==='object')record.entry=entry;else record.parse_error=true;
  logCache.push(record);
  logCacheBytes+=record.bytes;
  pruneLogCache(receivedMs);
  return true;
}
	function logNearBottom(el=$('log')){return el.scrollHeight-el.scrollTop-el.clientHeight<=logStickThresholdPx}
	function logDisplayHasContent(){
	  return $('log').textContent.length>0||(logPendingReplace!==null&&logPendingReplace.length>0)||logPendingAppend.length>0;
	}
	function renderLogNow(){
	  const el=$('log');
	  const stick=logRenderStickToBottom&&logNearBottom(el);
	  const scrollTop=el.scrollTop;
	  if(logPendingReplace!==null){
	    el.textContent=logPendingReplace;
	    logPendingReplace=null;
	  }
	  if(logPendingAppend.length){
	    el.appendChild(document.createTextNode(logPendingAppend.join('')));
	    logPendingAppend=[];
	  }
	  if(stick)el.scrollTop=el.scrollHeight;else el.scrollTop=scrollTop;
	}
	function renderLog(stickToBottom=true){
	  logRenderStickToBottom=logRenderStickToBottom&&stickToBottom;
	  if(logRenderQueued)return;
	  logRenderStickToBottom=stickToBottom;
	  logRenderQueued=true;
	  requestAnimationFrame(()=>{
	    logRenderQueued=false;
	    renderLogNow();
  });
}
	function appendLogText(text,replace=false){
	  const now=Date.now();
	  const stickToBottom=replace||logNearBottom();
	  if(replace){logLines=[];resetLogCache();logPendingAppend=[];logPendingReplace=''}
	  let changed=replace;
	  const newLines=[];
	  for(const line of text.split('\n')){
	    if(!line)continue;
	    if(cacheLogLine(line,now)){
	      logLines.push(line);
	      newLines.push(line);
	      changed=true;
	    }
	  }
	  if(changed){
	    if(replace){
	      logPendingReplace=logLines.join('\n');
	    }else if(newLines.length){
	      const prefix=logDisplayHasContent()?'\n':'';
	      logPendingAppend.push(prefix+newLines.join('\n'));
	    }
	    renderLog(stickToBottom);
	  }
	  updateLogCacheSummary();
	}
async function controlLog(action){
  await fetch('/hcp2_log/'+action,{cache:'no-store'});
  requestRefresh();
	  if(action==='clear'){logLines=[];logPendingAppend=[];logPendingReplace='';resetLogCache();renderLog(true);await refreshLog(false)}else await refreshLog();
	}
	async function refreshLog(replace=true){
	  if(logLoadBusy)return;
	  logLoadBusy=true;
	  try{
    const r=await fetch('/hcp2_log',{cache:'no-store'});
    const text=await r.text();
    appendLogText(text,replace);
  }catch(e){
    $('logStream').textContent='log refresh failed: '+e.message;
  }finally{
	    logLoadBusy=false;
	  }
	}
	function closeLogStream(){
	  if(logReconnectTimer){clearTimeout(logReconnectTimer);logReconnectTimer=null}
	  if(logSocket){logSocket.onclose=null;try{logSocket.close()}catch(e){};logSocket=null}
	}
	function connectLogStream(replace=false){
	  closeLogStream();
	  lastHealthStreamMs=Date.now();
	  const scheme=location.protocol==='https:'?'wss':'ws';
	  const path=replace?'/hcp2_log/ws?replace=1':'/hcp2_log/ws';
	  const ws=new WebSocket(`${scheme}://${location.host}${path}`);
  logSocket=ws;
  $('logStream').textContent='stream connecting';
  ws.onopen=()=>{$('logStream').textContent='stream connected';logReconnectDelayMs=1000;refreshLog(false)};
  ws.onmessage=event=>{
    let message=null;
    try{message=JSON.parse(event.data)}catch(e){}
    if(message&&message.type==='health'&&message.health){
      applyHealth(message.health,'stream');
      return;
    }
    if(message&&message.type==='log'&&typeof message.text==='string'){
      appendLogText(message.text);
      return;
    }
    appendLogText(event.data);
  };
  ws.onerror=()=>{try{ws.close()}catch(e){}};
  ws.onclose=()=>{
    if(logSocket===ws)logSocket=null;
    const delay=logReconnectDelayMs;
    logReconnectDelayMs=Math.min(logReconnectDelayMs*2,10000);
    $('logStream').textContent=`stream disconnected; reconnecting in ${Math.round(delay/1000)}s`;
	    logReconnectTimer=setTimeout(()=>connectLogStream(false),delay);
	  };
	}
	window.addEventListener('pagehide',closeLogStream);
	function cachedLogExport(){
  pruneLogCache();
  return {
    format:'hcp2-debug-log-cache-v1',
    exported_at:new Date().toISOString(),
    source:location.host,
    cache:{record_count:logCache.length,bytes:logCacheBytes,max_age_ms:maxCacheAgeMs,max_bytes:maxCacheBytes},
    records:logCache.map(({received_ms,bytes,...record})=>record)
  };
}
function downloadCachedLog(){
  const payload=JSON.stringify(cachedLogExport(),null,2);
  const blob=new Blob([payload],{type:'application/json'});
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  const stamp=new Date().toISOString().replace(/[:.]/g,'-');
  a.href=url;
  a.download=`hcp2-debug-log-${stamp}.json`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}
async function refreshRaw(){
  if(!rawPath||rawBusy)return;
  rawBusy=true;
  try{
    const data=await getJson(rawPath,2);
    $('raw').textContent=JSON.stringify(data,null,2);
  }catch(e){
    $('raw').textContent='refresh failed: '+e.message;
  }finally{
    rawBusy=false;
  }
}
async function loadRaw(path){rawPath=path;await refreshRaw()}
	setInterval(()=>{if(logSocket&&logSocket.readyState===WebSocket.CONNECTING)return;if(Date.now()-lastHealthStreamMs>2000)requestRefresh()},1000);
	setInterval(()=>{if(!logSocket||logSocket.readyState!==WebSocket.OPEN)requestRefresh()},5000);
setInterval(()=>{refreshRaw().catch(()=>{})},3000);
	async function init(){
	  connectLogStream();
	  setTimeout(()=>{if((!logSocket||logSocket.readyState!==WebSocket.CONNECTING)&&Date.now()-lastHealthStreamMs>2000)requestRefresh()},2500);
	}
init();
</script>
</body>
</html>)HTML";
}

void HCP2Bridge::setup_http_debug_server_() {
  this->http_debug_server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->http_debug_server_ == nullptr) {
    ESP_LOGW(TAG, "Failed to create HCP2 HTTP debug socket, retrying");
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }

  int enable = 1;
  int err = this->http_debug_server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (err != 0) {
    ESP_LOGW(TAG, "SO_REUSEADDR failed for HCP2 HTTP debug socket: errno=%d", errno);
  }
  err = this->http_debug_server_->setblocking(false);
  if (err != 0) {
    ESP_LOGW(TAG, "Failed to set HCP2 HTTP debug socket nonblocking: errno=%d, retrying", errno);
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }

  struct sockaddr_storage server_addr;
  socklen_t server_addr_len =
      socket::set_sockaddr_any((struct sockaddr *) &server_addr, sizeof(server_addr), this->http_debug_port_);
  if (server_addr_len == 0) {
    ESP_LOGW(TAG, "Failed to build HCP2 HTTP debug bind address, retrying");
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }
  err = this->http_debug_server_->bind((struct sockaddr *) &server_addr, server_addr_len);
  if (err != 0) {
    ESP_LOGW(TAG, "Failed to bind HCP2 HTTP debug port %u: errno=%d, retrying", (unsigned int) this->http_debug_port_,
             errno);
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }
  err = this->http_debug_server_->listen(4);
  if (err != 0) {
    ESP_LOGW(TAG, "Failed to listen on HCP2 HTTP debug port %u: errno=%d, retrying", (unsigned int) this->http_debug_port_,
             errno);
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }
  this->http_debug_next_setup_ms_ = 0;
  ESP_LOGI(TAG, "HCP2 HTTP debug listening on port %u", (unsigned int) this->http_debug_port_);
}

void HCP2Bridge::start_http_debug_task_() {
  if (!this->http_debug_enabled_() || this->http_debug_task_handle_ != nullptr) {
    return;
  }

  const BaseType_t ok = xTaskCreatePinnedToCore(HCP2Bridge::http_debug_task_trampoline_, "hcp2_http_dbg",
                                                HCP2BRIDGE_HTTP_TASK_STACK_BYTES, this, 1,
                                                &this->http_debug_task_handle_, tskNO_AFFINITY);
  if (ok != pdPASS) {
    this->http_debug_task_handle_ = nullptr;
    ESP_LOGW(TAG, "Failed to start HCP2 HTTP debug task; core HCP2 responder continues");
  }
}

void HCP2Bridge::http_debug_task_trampoline_(void *arg) {
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->http_debug_task_loop_();
}

void HCP2Bridge::http_debug_task_loop_() {
  for (;;) {
    if (this->http_debug_enabled_()) {
      this->maybe_setup_http_debug_server_();
      this->http_debug_accept_client_();
      this->http_debug_service_pending_client_();
      this->http_debug_service_log_ws_();
    }
    vTaskDelay(pdMS_TO_TICKS(HCP2BRIDGE_HTTP_TASK_IDLE_MS));
  }
}

void HCP2Bridge::maybe_setup_http_debug_server_() {
  if (this->http_debug_server_ != nullptr) {
    return;
  }
  const uint32_t now_ms = millis();
  if (this->http_debug_next_setup_ms_ != 0 &&
      static_cast<int32_t>(now_ms - this->http_debug_next_setup_ms_) < 0) {
    return;
  }
  this->setup_http_debug_server_();
}

void HCP2Bridge::http_debug_accept_client_() {
  if (this->http_debug_server_ == nullptr || !this->http_debug_server_->ready()) {
    return;
  }
  while (true) {
    struct sockaddr_storage source_addr;
    socklen_t source_addr_len = sizeof(source_addr);
    auto client = this->http_debug_server_->accept_loop_monitored((struct sockaddr *) &source_addr, &source_addr_len);
    if (!client) {
      break;
    }
    int enable = 1;
    client->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    int err = client->setblocking(false);
    if (err != 0) {
      client.reset();
      continue;
    }
    if (this->http_debug_pending_client_ != nullptr) {
      if (millis() - this->http_debug_pending_client_started_ms_ > HCP2BRIDGE_HTTP_PENDING_TIMEOUT_MS) {
        this->http_debug_pending_client_.reset();
        this->http_debug_request_buffer_len_ = 0;
      } else {
        this->http_debug_send_response_(std::move(client), "503 Service Unavailable", "text/plain; charset=utf-8",
                                        "busy\n");
        continue;
      }
    }
    this->http_debug_pending_client_ = std::move(client);
    this->http_debug_request_buffer_len_ = 0;
    this->http_debug_pending_client_started_ms_ = millis();
    break;
  }
}

void HCP2Bridge::http_debug_service_pending_client_() {
  if (this->http_debug_pending_client_ == nullptr) {
    return;
  }
  if (millis() - this->http_debug_pending_client_started_ms_ > HCP2BRIDGE_HTTP_PENDING_TIMEOUT_MS) {
    this->http_debug_pending_client_.reset();
    this->http_debug_request_buffer_len_ = 0;
    return;
  }
  size_t budget = HCP2BRIDGE_HTTP_REQUEST_READ_BUDGET_BYTES;
  while (budget > 0u) {
    char buffer[96];
    const size_t read_budget = std::min(sizeof(buffer), budget);
    ssize_t read_len = this->http_debug_pending_client_->read(buffer, read_budget);
    if (read_len > 0) {
      budget -= (size_t) read_len;
      for (ssize_t i = 0; i < read_len; i++) {
        const char c = buffer[i];
        if (this->http_debug_request_buffer_len_ < sizeof(this->http_debug_request_buffer_) - 1u) {
          this->http_debug_request_buffer_[this->http_debug_request_buffer_len_++] = c;
          if (this->http_debug_request_complete_()) {
            this->http_debug_request_buffer_[this->http_debug_request_buffer_len_] = '\0';
            std::string request(this->http_debug_request_buffer_, this->http_debug_request_buffer_len_);
            auto client = std::move(this->http_debug_pending_client_);
            this->http_debug_request_buffer_len_ = 0;
            this->http_debug_handle_request_(std::move(client), request);
            return;
          }
        } else {
          auto client = std::move(this->http_debug_pending_client_);
          this->http_debug_request_buffer_len_ = 0;
          this->http_debug_send_response_(std::move(client), "414 URI Too Long",
                                          "text/plain; charset=utf-8", "request headers too long\n");
          return;
        }
      }
      continue;
    }
    if (read_len == 0) {
      this->http_debug_pending_client_.reset();
      this->http_debug_request_buffer_len_ = 0;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    this->http_debug_pending_client_.reset();
    this->http_debug_request_buffer_len_ = 0;
    return;
  }
}

bool HCP2Bridge::http_debug_request_complete_() const {
  if (this->http_debug_request_buffer_len_ >= 4u) {
    for (size_t i = 3; i < this->http_debug_request_buffer_len_; i++) {
      if (this->http_debug_request_buffer_[i - 3u] == '\r' && this->http_debug_request_buffer_[i - 2u] == '\n' &&
          this->http_debug_request_buffer_[i - 1u] == '\r' && this->http_debug_request_buffer_[i] == '\n') {
        return true;
      }
    }
  }
  if (this->http_debug_request_buffer_len_ >= 2u) {
    for (size_t i = 1; i < this->http_debug_request_buffer_len_; i++) {
      if (this->http_debug_request_buffer_[i - 1u] == '\n' && this->http_debug_request_buffer_[i] == '\n') {
        return true;
      }
    }
  }
  return false;
}

void HCP2Bridge::http_debug_service_log_ws_() {
  if (this->http_debug_log_ws_client_ == nullptr) {
    return;
  }

  if (this->http_debug_log_ws_client_->ready()) {
    size_t budget = HCP2BRIDGE_HTTP_WS_DRAIN_BUDGET_BYTES;
    while (budget > 0u) {
      char buffer[96];
      const size_t read_budget = std::min(sizeof(buffer), budget);
      ssize_t read_len = this->http_debug_log_ws_client_->read(buffer, read_budget);
      if (read_len > 0) {
        budget -= (size_t) read_len;
        continue;
      }
      if (read_len == 0) {
        this->http_debug_log_ws_peer_close_count_++;
        this->http_debug_log_ws_last_errno_ = 0;
        this->http_debug_close_log_ws_("peer_close");
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      this->http_debug_log_ws_read_fail_count_++;
      this->http_debug_log_ws_last_errno_ = errno;
      this->http_debug_close_log_ws_("read_error");
      return;
    }
  }

  const uint32_t now_ms = millis();
  if (this->http_debug_log_ws_last_status_ms_ == 0 ||
      now_ms - this->http_debug_log_ws_last_status_ms_ >= HCP2BRIDGE_HTTP_WS_HEALTH_INTERVAL_MS) {
    this->http_debug_log_ws_last_status_ms_ = now_ms;
    if (!this->http_debug_send_ws_text_(this->http_debug_log_ws_client_.get(), this->http_debug_ws_health_json_(),
                                        HCP2BRIDGE_HTTP_WS_WRITE_TIMEOUT_MS)) {
      this->http_debug_log_ws_write_fail_count_++;
      this->http_debug_log_ws_last_errno_ = errno;
      this->http_debug_close_log_ws_("health_write_error");
      return;
    }
  }

  if (this->http_debug_log_ws_last_send_ms_ != 0 &&
      now_ms - this->http_debug_log_ws_last_send_ms_ < HCP2BRIDGE_HTTP_WS_SEND_INTERVAL_MS) {
    return;
  }
  uint32_t next_cursor = this->http_debug_log_ws_next_seq_;
  const std::string body =
      this->protocol_log_body_since_(this->http_debug_log_ws_next_seq_, &next_cursor, HCP2BRIDGE_HTTP_WS_MAX_CHUNK_BYTES);
  this->http_debug_log_ws_next_seq_ = next_cursor;
  this->http_debug_log_ws_last_send_ms_ = now_ms;
  if (body.empty()) {
    return;
  }
  if (!this->http_debug_send_ws_text_(this->http_debug_log_ws_client_.get(), this->http_debug_ws_log_json_(body),
                                      HCP2BRIDGE_HTTP_WS_WRITE_TIMEOUT_MS)) {
    this->http_debug_log_ws_write_fail_count_++;
    this->http_debug_log_ws_last_errno_ = errno;
    this->http_debug_close_log_ws_("log_write_error");
  }
}

void HCP2Bridge::http_debug_close_log_ws_(const char *reason) {
  if (this->http_debug_log_ws_client_ == nullptr) {
    return;
  }
  this->http_debug_log_ws_client_.reset();
  this->http_debug_log_ws_disconnect_count_++;
  this->http_debug_log_ws_last_close_reason_ = reason != nullptr ? reason : "unknown";
  ESP_LOGI(TAG, "HCP2 HTTP debug log WebSocket disconnected: %s", this->http_debug_log_ws_last_close_reason_);
}

void HCP2Bridge::http_debug_handle_request_(std::unique_ptr<socket::Socket> client, const std::string &request) {
  const std::string path = this->http_debug_path_from_request_(request);
  if (path.empty()) {
    this->http_debug_send_response_(std::move(client), "400 Bad Request", "text/plain; charset=utf-8",
                                    "bad request\n");
    return;
  }
  if (path == "/hcp2_log/ws") {
    this->http_debug_upgrade_log_ws_(std::move(client), request);
    return;
  }
  if (path == "/" || path == "/index.html") {
    this->http_debug_send_response_(std::move(client), "200 OK", "text/html; charset=utf-8",
                                    this->http_debug_index_html_());
    return;
  }
  if (path == "/favicon.ico") {
    this->http_debug_send_response_(std::move(client), "204 No Content", "image/x-icon", "");
    return;
  }
  if (path == "/stats") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json", this->http_debug_stats_json_());
    return;
  }
  if (path == "/health" || path == "/preflight") {
    const std::string body = this->http_debug_health_json_();
    const char *status = body.find("\"verdict\":\"ok\"") != std::string::npos ? "200 OK" : "503 Service Unavailable";
    this->http_debug_send_response_(std::move(client), status, "application/json", body);
    return;
  }
  if (path == "/support") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_support_json_());
    return;
  }
  if (path == "/hcp2_log/start") {
    this->protocol_log_enabled_ = true;
    this->protocol_log_append_control_("start");
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->protocol_log_summary_json_());
    return;
  }
  if (path == "/hcp2_log/stop") {
    this->protocol_log_append_control_("stop");
    this->protocol_log_enabled_ = false;
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->protocol_log_summary_json_());
    return;
  }
  if (path == "/hcp2_log/clear") {
    this->protocol_log_clear_();
    this->protocol_log_append_control_("clear");
    if (this->http_debug_log_ws_client_ != nullptr) {
      this->http_debug_log_ws_next_seq_ = 1;
      this->http_debug_log_ws_last_send_ms_ = 0;
      this->http_debug_log_ws_last_status_ms_ = 0;
    }
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->protocol_log_summary_json_());
    return;
  }
  if (path == "/hcp2_log") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/x-ndjson; charset=utf-8",
                                    this->protocol_log_body_());
    return;
  }
  if (path == "/hcp2_log.bin") {
    this->http_debug_send_log_binary_response_(std::move(client));
    return;
  }
  this->http_debug_send_response_(std::move(client), "404 Not Found", "text/plain; charset=utf-8", "not found\n");
}

void HCP2Bridge::http_debug_upgrade_log_ws_(std::unique_ptr<socket::Socket> client, const std::string &request) {
  const std::string upgrade = this->http_debug_header_value_(request, "Upgrade");
  const std::string key = this->http_debug_header_value_(request, "Sec-WebSocket-Key");
  if (!hcp2_ascii_iequals(upgrade, "websocket") || key.empty()) {
    this->http_debug_send_response_(std::move(client), "400 Bad Request", "text/plain; charset=utf-8",
                                    "bad websocket upgrade\n");
    return;
  }
  if (this->http_debug_log_ws_client_ != nullptr) {
    const bool replace_existing = request.rfind("GET /hcp2_log/ws?replace=1 ", 0) == 0 ||
                                  request.rfind("GET /hcp2_log/ws?replace=true ", 0) == 0;
    if (replace_existing) {
      this->http_debug_close_log_ws_("replaced");
    } else {
      this->http_debug_log_ws_reject_count_++;
      this->http_debug_send_response_(std::move(client), "409 Conflict", "text/plain; charset=utf-8",
                                      "log websocket already connected\n");
      ESP_LOGW(TAG, "Rejected duplicate HCP2 HTTP debug log WebSocket");
      return;
    }
  }

  std::string accept_source = key;
  accept_source += HCP2BRIDGE_WEBSOCKET_GUID;
  unsigned char digest[20];
  unsigned char encoded[32];
  size_t encoded_len = 0;
  if (mbedtls_sha1((const unsigned char *) accept_source.data(), accept_source.size(), digest) != 0 ||
      mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len, digest, sizeof(digest)) != 0) {
    this->http_debug_send_response_(std::move(client), "500 Internal Server Error", "text/plain; charset=utf-8",
                                    "websocket handshake failed\n");
    return;
  }

  std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
  response += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
  response.append((const char *) encoded, encoded_len);
  response += "\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  if (!this->http_debug_write_all_(client.get(), response, HCP2BRIDGE_HTTP_WS_HANDSHAKE_TIMEOUT_MS)) {
    return;
  }

  this->http_debug_log_ws_client_ = std::move(client);
  this->http_debug_log_ws_next_seq_ = this->protocol_log_next_seq_snapshot_();
  this->http_debug_log_ws_last_send_ms_ = 0;
  this->http_debug_log_ws_last_status_ms_ = 0;
  this->http_debug_log_ws_connect_count_++;
  this->http_debug_log_ws_last_errno_ = 0;
  this->http_debug_log_ws_last_close_reason_ = "none";
  ESP_LOGI(TAG, "HCP2 HTTP debug log WebSocket connected");
}

bool HCP2Bridge::http_debug_send_ws_text_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms) {
  if (client == nullptr || payload.empty() || payload.size() > 65535u) {
    return false;
  }
  std::string frame;
  frame.reserve(payload.size() + 4u);
  frame.push_back((char) 0x81);
  if (payload.size() <= 125u) {
    frame.push_back((char) payload.size());
  } else {
    frame.push_back((char) 126);
    frame.push_back((char) ((payload.size() >> 8u) & 0xFFu));
    frame.push_back((char) (payload.size() & 0xFFu));
  }
  frame += payload;
  return this->http_debug_write_all_(client, frame, timeout_ms);
}

std::string HCP2Bridge::http_debug_ws_log_json_(const std::string &body) {
  std::string json = "{\"type\":\"log\",\"text\":";
  json += http_debug_json_string_(body);
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_ws_health_json_() {
  std::string json = "{\"type\":\"health\",\"health\":";
  json += this->http_debug_health_json_();
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_json_string_(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2u);
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20u) {
          char escaped[7];
          std::snprintf(escaped, sizeof(escaped), "\\u%04X", (unsigned int) c);
          out += escaped;
        } else {
          out.push_back((char) c);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string HCP2Bridge::http_debug_header_value_(const std::string &request, const char *name) {
  size_t line_start = 0;
  while (line_start < request.size()) {
    size_t line_end = request.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = request.size();
    }
    size_t trimmed_end = line_end;
    if (trimmed_end > line_start && request[trimmed_end - 1u] == '\r') {
      trimmed_end--;
    }
    const size_t colon = request.find(':', line_start);
    if (colon != std::string::npos && colon < trimmed_end) {
      const std::string header_name = request.substr(line_start, colon - line_start);
      if (hcp2_ascii_iequals(header_name, name)) {
        size_t value_start = colon + 1u;
        while (value_start < trimmed_end && (request[value_start] == ' ' || request[value_start] == '\t')) {
          value_start++;
        }
        while (trimmed_end > value_start &&
               (request[trimmed_end - 1u] == ' ' || request[trimmed_end - 1u] == '\t')) {
          trimmed_end--;
        }
        return request.substr(value_start, trimmed_end - value_start);
      }
    }
    line_start = line_end + 1u;
  }
  return "";
}

void HCP2Bridge::http_debug_send_response_(std::unique_ptr<socket::Socket> client, const char *status,
                                           const char *content_type, const std::string &body) {
  std::string response = "HTTP/1.1 ";
  response += status;
  response += "\r\nContent-Type: ";
  response += content_type;
  response += "\r\nContent-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  response += body;
  const uint32_t timeout_ms =
      response.size() > HCP2BRIDGE_HTTP_LARGE_RESPONSE_BYTES ? HCP2BRIDGE_HTTP_LARGE_RESPONSE_TIMEOUT_MS
                                                             : HCP2BRIDGE_HTTP_RESPONSE_TIMEOUT_MS;
  if (!this->http_debug_write_all_(client.get(), response, timeout_ms)) {
    ESP_LOGW(TAG, "Failed to write HCP2 HTTP response: errno=%d", errno);
  }
}

void HCP2Bridge::http_debug_send_log_binary_response_(std::unique_ptr<socket::Socket> client) {
  const std::string body = this->protocol_log_body_();
  this->http_debug_send_response_(std::move(client), "200 OK", "application/octet-stream", body);
}

bool HCP2Bridge::http_debug_write_all_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms) {
  size_t offset = 0;
  const uint32_t started = millis();
  while (offset < payload.size()) {
    if (timeout_ms != 0u && millis() - started > timeout_ms) {
      return false;
    }
    ssize_t written = client->write(payload.data() + offset, payload.size() - offset);
    if (written > 0) {
      offset += (size_t) written;
      continue;
    }
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      return false;
    }
    if (timeout_ms != 0u && millis() - started > timeout_ms) {
      return false;
    }
    delay(1);
  }
  return true;
}

std::string HCP2Bridge::http_debug_path_from_request_(const std::string &request) {
  const size_t first_space = request.find(' ');
  if (first_space == std::string::npos) {
    return "";
  }
  const size_t second_space = request.find(' ', first_space + 1);
  if (second_space == std::string::npos || second_space <= first_space + 1) {
    return "";
  }
  std::string path = request.substr(first_space + 1, second_space - first_space - 1);
  const size_t query = path.find('?');
  if (query != std::string::npos) {
    path.resize(query);
  }
  return path;
}

#endif

}  // namespace hcp2bridge
}  // namespace esphome
