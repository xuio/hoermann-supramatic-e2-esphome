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

#if __has_include("esphome/core/version.h")
#include "esphome/core/version.h"
#endif
#ifndef ESPHOME_VERSION
#define ESPHOME_VERSION "unknown"
#endif

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

// Compile-time firmware build stamp (additive /stats field; cheap literal).
static const char *const HCP2BRIDGE_BUILD_INFO = ESPHOME_VERSION " " __DATE__ " " __TIME__;

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
  json += ",\"build\":\"";
  json += HCP2BRIDGE_BUILD_INFO;
  json += "\",\"command_sequence\":";
  json += std::to_string(this->get_command_sequence());
  json += ",\"last_command\":\"";
  json += button_name_(this->last_commanded_button_);
  json += "\",\"last_command_age_ms\":";
  if (this->last_commanded_ms_ != 0u) {
    json += std::to_string(static_cast<uint32_t>(millis() - this->last_commanded_ms_));
  } else {
    json += "null";
  }
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
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>HCP2 Bridge Debug</title>
<link rel="icon" href="data:,">
<style>
:root{
  color-scheme:dark;
  --bg:#0b1220;--panel:#0f172a;--panel2:#020617;--line:#1f2937;--line2:#334155;
  --txt:#e5e7eb;--muted:#94a3b8;--key:#94a3b8;
  --ok:#10b981;--warn:#f59e0b;--fail:#ef4444;--rx:#22d3ee;--tx:#f59e0b;
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  --mono:ui-monospace,SFMono-Regular,Menlo,monospace;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);line-height:1.35;padding-bottom:24px}
h1{font-size:18px;margin:0}
h2{font-size:13px;margin:0 0 8px;color:#cbd5e1;text-transform:uppercase;letter-spacing:.04em}
a{color:#7dd3fc}
.wrap{padding:0 14px}
/* ---- sticky status header (region 0) ---- */
#hdr{position:sticky;top:0;z-index:10;background:var(--bg);border-bottom:1px solid var(--line);
  padding:calc(env(safe-area-inset-top) + 10px) 14px 10px}
.hdrtop{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.status{display:inline-flex;align-items:center;gap:8px;border-radius:999px;padding:7px 12px;font-weight:700;font-size:14px}
.ok{background:#064e3b;color:#d1fae5}.fail{background:#7f1d1d;color:#fee2e2}.unknown{background:#374151;color:#f3f4f6}
#updated{color:var(--muted);font-size:12px}
#reasons{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
#reasons .rchip{background:#7f1d1d;color:#fee2e2;border-radius:6px;padding:2px 8px;font-size:12px;font-family:var(--mono)}
/* vital chips */
#vitals{display:flex;gap:8px;margin-top:10px;overflow-x:auto;-webkit-overflow-scrolling:touch;padding-bottom:2px}
.chip{flex:0 0 auto;min-width:84px;border:1px solid var(--line2);border-radius:8px;background:var(--panel);padding:6px 9px}
.chip .cl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.03em;white-space:nowrap}
.chip .cv{font-family:var(--mono);font-size:16px;font-weight:600;white-space:nowrap}
.chip .cu{font-size:10px;color:var(--muted)}
.chip.ok .cv{color:var(--ok)}.chip.warn .cv{color:var(--warn)}.chip.fail .cv{color:var(--fail)}.chip.off .cv{color:var(--muted)}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--muted);vertical-align:middle;margin-left:4px}
.dot.beat{background:var(--ok);animation:beat .5s ease-out}
@keyframes beat{from{transform:scale(1.9);opacity:.4}to{transform:scale(1);opacity:1}}
/* stale / disconnected overlays on the header */
#hdr.stale{filter:saturate(.35);border-bottom:1px dashed var(--warn)}
#hdr.stale::after,#hdr.disconnected::after{content:attr(data-banner);display:block;margin-top:8px;font-size:12px;
  font-family:var(--mono);color:#fde68a}
#hdr.disconnected{filter:grayscale(.5)}
#hdr.disconnected::after{color:#fca5a5}
/* ---- panels / grid ---- */
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px;margin-top:12px}
.panel{border:1px solid var(--line2);border-radius:8px;background:var(--panel);padding:12px}
.row{display:flex;justify-content:space-between;gap:12px;border-bottom:1px solid var(--line);padding:5px 0;font-size:13px}
.row:last-child{border-bottom:0}
.key{color:var(--key)}.value{font-family:var(--mono);text-align:right;white-space:nowrap}
.value.ok{color:var(--ok)}.value.warn{color:var(--warn)}.value.fail{color:var(--fail)}
.muted{color:var(--muted);font-size:12px}
/* door bar */
#posbar{position:relative;height:14px;border-radius:7px;background:#0b1220;border:1px solid var(--line2);margin:8px 0;overflow:hidden}
#posfill{position:absolute;left:0;top:0;bottom:0;background:linear-gradient(90deg,#0e7490,#22d3ee);width:0}
#postick{position:absolute;top:-2px;bottom:-2px;width:2px;background:#fbbf24}
.badges{display:flex;gap:6px;flex-wrap:wrap;margin-top:6px}
.badge{border:1px solid var(--line2);border-radius:6px;padding:2px 8px;font-size:12px;font-family:var(--mono);color:var(--muted)}
.badge.on{color:#fde68a;border-color:#a16207}.badge.alert{color:#fee2e2;border-color:#b91c1c;background:#450a0a}
/* timing */
#jitterSpark{width:100%;height:34px;display:block;margin-top:4px}
/* buttons */
button,a.button,select,input[type=text]{font:inherit;border:1px solid var(--line2);border-radius:6px;background:#1e293b;color:var(--txt);
  padding:8px 11px;text-decoration:none;cursor:pointer;min-height:40px}
button:hover,a.button:hover{background:#334155}
button.pri{border-color:#0e7490}
button.on{background:#0e7490;border-color:#22d3ee}
input[type=text]{cursor:text;min-width:120px}
label.flt{display:inline-flex;align-items:center;gap:5px;font-size:12px;color:#cbd5e1}
.toolbar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:8px}
.toolbar .sp{flex:1 1 auto}
/* log */
#logwrap{position:relative}
pre{white-space:pre-wrap;overflow:auto;background:var(--panel2);border:1px solid var(--line);border-radius:6px;padding:10px;font-size:12px;margin:0}
#log{height:60vh;overflow-anchor:none}
#raw{max-height:48vh}
#logNewPill{position:absolute;right:14px;bottom:12px;display:none;background:#0e7490;color:#e0f2fe;border:1px solid #22d3ee;
  border-radius:999px;padding:6px 12px;font-size:12px;cursor:pointer}
#logNewPill.show{display:block}
/* table mode */
#logtbl{display:none;height:60vh;overflow:auto;background:var(--panel2);border:1px solid var(--line);border-radius:6px;font-size:12px}
#logtbl.show{display:block}#log.hide{display:none}
.lgrid{display:grid;grid-template-columns:62px 78px 56px 38px 130px 110px 52px 40px 1fr;gap:0;font-family:var(--mono)}
.lgrid>div{padding:2px 6px;border-bottom:1px solid #0b1324;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.lh>div{position:sticky;top:0;background:#0b1324;color:var(--muted);border-bottom:1px solid var(--line2)}
.lr.rx .c-dir{color:var(--rx)}.lr.tx .c-dir{color:var(--tx)}
.lr.err{border-left:3px solid var(--fail)}.lr.err .c-crc{color:var(--fail)}
.lr.cmderr .c-fr{color:var(--fail)}
details{margin-top:12px}summary{cursor:pointer;color:#cbd5e1;font-size:13px;padding:6px 0;user-select:none}
.linkrow{display:flex;justify-content:space-between;gap:10px;font-size:12px;padding:3px 0;border-bottom:1px solid var(--line)}
.linkrow .value{font-family:var(--mono)}
@media (max-width:560px){#log,#logtbl{height:52vh}.chip{min-width:74px}}
</style>
</head>
<body>
<header id="hdr">
  <div class="hdrtop">
    <span id="verdict" class="status unknown">loading</span>
    <h1>HCP2 Bridge Debug</h1>
    <span class="sp" style="flex:1"></span>
    <span id="updated"></span>
  </div>
  <div id="reasons"></div>
  <div id="vitals"></div>
</header>
<div class="wrap">
<div class="grid">
  <section class="panel"><h2>Door</h2>
    <div id="posbar"><div id="posfill"></div><div id="postick"></div></div>
    <div id="door"></div>
    <div class="badges" id="doorBadges"></div>
    <div class="muted" id="lastCmd" style="margin-top:6px"></div>
  </section>
  <section class="panel"><h2>Bus timing <span class="muted" id="timingMode"></span></h2>
    <div id="timing"></div>
    <canvas id="jitterSpark"></canvas>
    <div class="muted" id="timingNote"></div>
  </section>
  <section class="panel"><h2>Counters</h2><div id="counters"></div></section>
</div>

<section class="panel" style="margin-top:12px">
  <h2>Live packet log</h2>
  <div class="toolbar">
    <button id="pauseBtn" onclick="togglePause()">Pause</button>
    <button id="modeBtn" onclick="toggleMode()">Table view</button>
    <label class="flt"><input type="checkbox" id="fErr" onchange="onFilter()">errors only</label>
    <select id="fDir" onchange="onFilter()"><option value="">dir: all</option><option value="rx">rx</option><option value="tx">tx</option></select>
    <select id="fFrame" onchange="onFilter()"><option value="">frame: all</option><option>status_poll</option><option>broadcast_status</option><option>command_arg</option><option>bus_scan</option><option>other_valid</option></select>
    <select id="fType" onchange="onFilter()"><option value="">type: all</option><option>protocol</option><option>state</option><option>command</option><option>lp_trace</option><option>control</option></select>
    <input type="text" id="fText" placeholder="filter text / hex" oninput="onFilter()">
    <span class="sp"></span>
    <button onclick="copyRaw()">Copy raw</button>
    <button onclick="copyDecoded()">Copy decoded</button>
    <button onclick="clearLocalView()">Clear view</button>
    <button class="pri" onclick="downloadCachedLog()">Download JSON</button>
    <button onclick="connectLogStream(true)">Reconnect Stream</button>
  </div>
  <div id="logStream" class="muted">stream disconnected</div>
  <div id="logCache" class="muted">cache empty</div>
  <div id="logwrap">
    <pre id="log"></pre>
    <div id="logtbl"></div>
    <div id="logNewPill" onclick="jumpToBottom()">&#8595; new records</div>
  </div>
</section>

<details id="drawer">
  <summary>Diagnostics &amp; device controls</summary>
  <div class="grid" style="margin-top:10px">
    <section class="panel"><h2>Link diagnostics</h2><div id="linkDiag"></div></section>
    <section class="panel"><h2>LP core</h2><div id="lpPanel"></div></section>
    <section class="panel"><h2>HP resets</h2><div id="hpPanel"></div></section>
    <section class="panel"><h2>Device log storage</h2><div id="protocolPanel"></div></section>
    <section class="panel"><h2>Stats</h2><div id="statsPanel"></div></section>
  </div>
  <section class="panel" style="margin-top:12px">
    <h2>Device protocol log</h2>
    <div class="toolbar">
      <button onclick="controlLog('start')">Start capture</button>
      <button onclick="controlLog('stop')">Stop capture</button>
      <button onclick="controlLog('clear')">Clear device buffer</button>
      <button onclick="refreshLog()">Refresh from device</button>
      <a class="button" href="/hcp2_log" download="hcp2-log.ndjson">Device NDJSON</a>
      <a class="button" href="/hcp2_log.bin" download="hcp2-log.bin">Device Raw</a>
    </div>
    <div id="logSummary" class="muted"></div>
  </section>
  <section class="panel" style="margin-top:12px">
    <h2>Raw JSON</h2>
    <div class="toolbar">
      <button onclick="loadRaw('/health')">Health</button>
      <button onclick="loadRaw('/stats')">Stats</button>
      <button onclick="loadRaw('/support')">Support</button>
    </div>
    <pre id="raw"></pre>
  </section>
</details>
</div>
<script>
const $=id=>document.getElementById(id);
const RESPONSE_DELAY_US=4200;        // HCP2_DEFAULT_RESPONSE_DELAY_US
const STALE_MS=2000;                 // matches the firmware health-push fallback window
const LOG_DISPLAY_MAX=5000;          // rendered lines; full history stays in logCache for export
const JITTER_RING=2048, POLL_RING=600;
let logSocket=null,logReconnectTimer=null;
let logLines=[],logCache=[],logCacheBytes=0;
let refreshBusy=false,logLoadBusy=false,rawPath=null,rawBusy=false;
let lastHealthStreamMs=0,lastHealthOkMs=0;
let logReconnectDelayMs=1000,logRenderQueued=false;
let logSeenSeqs=new Set(),logPendingAppend=[],logPendingReplace=null;
let logRenderStickToBottom=true;
const logStickThresholdPx=24;
const maxCacheAgeMs=30*60*1000;
const maxCacheBytes=100*1024*1024;
let displayPaused=false,tableMode=false;
let logEnabled=true;
let lastHeartbeat=null,lastHealth=null;
// derived-metric state (best-effort, client-side; reset on reconnect / source switch)
let dv=newDerive();
function newDerive(){return{src:null,pendUs:null,jit:[],polls:[],miss:0,maxMiss:0,lastJit:null}}
function resetDerived(){dv=newDerive()}

function row(k,v,cls){return `<div class="row"><span class="key">${k}</span><span class="value${cls?(' '+cls):''}">${v??''}</span></div>`}
function setRows(id,items){$(id).innerHTML=items.map(r=>row(r[0],r[1],r[2])).join('')}
function label(k){return String(k).replaceAll('_',' ')}
function fmt(v){
  if(v===undefined||v===null)return '';
  if(Array.isArray(v))return v.join(', ');
  if(typeof v==='number')return Number.isInteger(v)?String(v):v.toFixed(3);
  if(typeof v==='object')return JSON.stringify(v);
  return String(v);
}
function objectRows(obj,priority){
  const source=obj||{},seen=new Set(),rows=[];
  function add(k){
    if(seen.has(k)||!Object.prototype.hasOwnProperty.call(source,k))return;
    const v=source[k];
    if(v&&typeof v==='object'&&!Array.isArray(v))return;
    seen.add(k);rows.push([label(k),fmt(v)]);
  }
  (priority||[]).forEach(add);
  Object.keys(source).sort().forEach(add);
  return rows;
}
function setObjectRows(id,obj,priority){setRows(id,objectRows(obj,priority))}
function sleep(ms){return new Promise(r=>setTimeout(r,ms))}
async function getJson(path,attempts=3){
  let lastError=null;
  for(let attempt=0;attempt<attempts;attempt++){
    try{
      const r=await fetch(path,{cache:'no-store'});
      const t=await r.text();
      try{return JSON.parse(t)}catch(e){throw new Error(`${path} returned ${r.status}`)}
    }catch(e){lastError=e;if(attempt+1<attempts)await sleep(150*(attempt+1))}
  }
  throw lastError||new Error(path+' unavailable');
}

/* ---------------- health + vitals ---------------- */
function chip(id,label,val,unit,cls){
  return `<div class="chip ${cls||''}"><div class="cl">${label}</div><div class="cv">${val}${id==='vHeart'?'<span id="heartDot" class="dot"></span>':''}</div><div class="cu">${unit||''}</div></div>`;
}
function humanUptime(ms){
  if(ms==null)return '—';
  let s=Math.floor(ms/1000);const d=Math.floor(s/86400);s-=d*86400;const h=Math.floor(s/3600);s-=h*3600;const m=Math.floor(s/60);s-=m*60;
  if(d)return d+'d'+h+'h';if(h)return h+'h'+m+'m';if(m)return m+'m'+s+'s';return s+'s';
}
function pct(arr,p){if(!arr.length)return null;const a=arr.slice().sort((x,y)=>x-y);const i=Math.min(a.length-1,Math.floor(p/100*a.length));return a[i]}
function applyHealth(health,source='http'){
  lastHealth=health;
  const stats=health.stats||{};
  const ok=health.verdict==='ok';
  if(source==='stream')lastHealthStreamMs=Date.now();
  lastHealthOkMs=Date.now();
  $('verdict').className='status '+(ok?'ok':'fail');
  $('verdict').textContent=ok?'continuity ok':'continuity problem';
  $('updated').textContent='updated '+new Date().toLocaleTimeString()+' via '+source+(stats.build?(' · fw '+stats.build):'');
  $('reasons').innerHTML=(health.reasons||[]).map(r=>`<span class="rchip">${r}</span>`).join('');
  const c=health.checks||{},lp=health.lp||{},hp=health.hp||{},p=stats.protocol_log||{};
  logEnabled=p.enabled!==false;
  // counters: keep bound to health.checks exactly (test contract) -- no extra nodes
  setObjectRows('counters',c,['polls_seen','polls_answered','pending_response','raw_missed_polls','missed_polls','tx_aborts','collisions','loop_overruns','rx_starvations','stuck_de_recoveries','health_flags','last_poll_age_ms','bus_online','valid_broadcast','lp_mode','lp_seen']);
  // statsPanel: render the WHOLE stats object (priority only reorders; all keys survive for the E2E fallback)
  setObjectRows('statsPanel',stats,['protocol','mode','uptime_ms','bus_online','valid_broadcast','state','position','polls_seen','polls_answered','missed_polls','raw_missed_polls','pending_response','crc_errors','rx_errors','tx_aborts','collisions','lp_heartbeat','lp_resets','hp_resets']);
  setObjectRows('lpPanel',lp,['health_flags','max_loop_us','max_rx_fifo','max_poll_rx_to_schedule_us','max_response_schedule_to_tx_start_us','max_response_tx_us','max_de_hold_us','last_poll_age_ms','loop_overruns','rx_starvations','stuck_de_recoveries','mailbox_repairs']);
  setObjectRows('hpPanel',hp,['resets','panic_resets','wdt_resets','brownout_resets']);
  setObjectRows('protocolPanel',p,['enabled','used','capacity','overwritten_records','overwritten_bytes','dropped_records','dropped_bytes','next_seq','storage','mode','flash_writes','ready']);
  renderDoor(health,stats);
  renderTiming(stats,lp);
  renderLinkDiag(stats);
  renderVitals(health,stats,c,lp);
  $('logSummary').textContent=`device log ${p.enabled?'enabled':'disabled'}, ${p.used||0}/${p.capacity||0} bytes, overwritten ${p.overwritten_records||0}`;
  updateLogCacheSummary();
  applyStale();
}
function renderVitals(health,stats,c,lp){
  const off=!logEnabled;
  const beat=(lastHeartbeat!=null&&stats.lp_heartbeat>lastHeartbeat);
  lastHeartbeat=stats.lp_heartbeat;
  const age=c.last_poll_age_ms;
  const ageCls=age==null?'':(age>1000?'fail':age>250?'warn':'ok');
  const jp95=dv.jit.length?pct(dv.jit,95):null;
  const rate=pollRate();
  const items=[
    chip('vLink','WS link',linkState(),'',linkClass()),
    chip('vBus','bus',stats.bus_online?'online':'offline','',stats.bus_online?'ok':'fail'),
    chip('vHeart','LP beat',stats.lp_heartbeat??'—','',off?'off':'ok'),
    chip('vAge','poll age',age??'—','ms',ageCls),
    chip('vMiss','missed/consec',off?'—':((c.missed_polls??0)+'/'+dv.maxMiss),off?'(log off)':'',(!off&&(c.missed_polls||dv.maxMiss)?'fail':'ok')),
    chip('vRate','poll rate',off?'—':(rate==null?'—':rate.toFixed(1)),off?'(log off)':'Hz',off?'off':'ok'),
    chip('vJit','jitter p95',off?'—':(jp95==null?'—':jp95),off?'(log off)':'µs',off?'off':(jp95!=null&&Math.abs(jp95)>800?'warn':'ok')),
    chip('vUp','uptime',humanUptime(stats.uptime_ms),'','')
  ];
  $('vitals').innerHTML=items.join('');
  if(beat){const d=$('heartDot');if(d){d.classList.add('beat');setTimeout(()=>d.classList.remove('beat'),480)}}
}
function linkState(){
  if(logSocket&&logSocket.readyState===WebSocket.OPEN){
    return (Date.now()-lastHealthStreamMs>STALE_MS)?'stale':'connected';
  }
  return 'down';
}
function linkClass(){const s=linkState();return s==='connected'?'ok':s==='stale'?'warn':'fail'}
function renderDoor(health,stats){
  const d=Object.assign({position:stats.position,state:stats.state},health.door||{});
  const pos=(typeof stats.position==='number')?stats.position:(d.current_position_raw!=null?d.current_position_raw/200:0);
  const tgt=(d.target_position_raw!=null)?d.target_position_raw/200:pos;
  $('posfill').style.width=Math.max(0,Math.min(1,pos))*100+'%';
  $('postick').style.left=Math.max(0,Math.min(1,tgt))*100+'%';
  setObjectRows('door',d,['state','position','state_raw','target_position_raw','current_position_raw']);
  const b=[];
  b.push(`<span class="badge ${d.light?'on':''}">light ${d.light?'on':'off'}</span>`);
  b.push(`<span class="badge ${d.obstruction?'alert':''}">obstruction ${d.obstruction?'YES':'no'}</span>`);
  $('doorBadges').innerHTML=b.join('');
  if(dv.lastCmd){
    const a=dv.lastCmd;
    $('lastCmd').textContent=`last command: ${a.button} (${a.ok?'ok':'rejected'}${a.reason?', '+a.reason:''}) — ack ${a.ack||'pending'}`;
  }else if(stats.last_command&&stats.last_command!=='none'){
    const age=stats.last_command_age_ms;
    $('lastCmd').textContent=`last command: ${stats.last_command}${age!=null?(' ('+Math.round(age/1000)+'s ago)'):''} — from device`;
  }
}
function renderTiming(stats,lp){
  $('timingMode').textContent='('+(stats.mode||'lp')+' core)';
  const off=!logEnabled;
  const j=dv.jit;
  const rows=[
    ['response-delay jitter',off?'(log off)':'',''],
    ['  last',off?'—':(dv.lastJit==null?'—':dv.lastJit+' µs')],
    ['  p50',off?'—':(j.length?pct(j,50)+' µs':'—')],
    ['  p95',off?'—':(j.length?pct(j,95)+' µs':'—')],
    ['  p99',off?'—':(j.length?pct(j,99)+' µs':'—')],
    ['  max (derived)',off?'—':(j.length?Math.max.apply(null,j)+' µs':'—')],
    ['fw max sched→tx',lp.max_response_schedule_to_tx_start_us!=null?lp.max_response_schedule_to_tx_start_us+' µs':'—'],
    ['fw max wire tx (max only)',lp.max_response_tx_us!=null?lp.max_response_tx_us+' µs':'—'],
    ['fw max DE hold (max only)',lp.max_de_hold_us!=null?lp.max_de_hold_us+' µs':'—'],
    ['poll rate',off?'(log off)':(pollRate()==null?'—':pollRate().toFixed(1)+' Hz')],
    ['samples',off?'—':String(j.length)]
  ];
  setRows('timing',rows.map(r=>[r[0],r[1]]));
  $('timingNote').innerHTML=off
    ?'Derived timing needs the device protocol log. <a href="javascript:controlLog(\'start\')">Start capture</a>.'
    :'Jitter = (TX start − poll RX) − 4200&micro;s configured delay; not on-wire latency.';
  drawSpark();
}
function pollRate(){
  if(dv.polls.length<2)return null;
  const now=dv.polls[dv.polls.length-1],first=dv.polls[0];
  const span=now-first;if(span<=0)return null;
  return (dv.polls.length-1)/span*1000;
}
function drawSpark(){
  const cv=$('jitterSpark');if(!cv)return;
  const w=cv.clientWidth||300,h=34;cv.width=w;cv.height=h;
  const ctx=cv.getContext('2d');ctx.clearRect(0,0,w,h);
  const j=dv.jit;if(j.length<2)return;
  const show=j.slice(-Math.min(j.length,w));
  let mn=Math.min.apply(null,show),mx=Math.max.apply(null,show);if(mn===mx){mn-=1;mx+=1}
  ctx.strokeStyle='#22d3ee';ctx.lineWidth=1;ctx.beginPath();
  show.forEach((v,i)=>{const x=i/(show.length-1)*w,y=h-((v-mn)/(mx-mn))*(h-4)-2;i?ctx.lineTo(x,y):ctx.moveTo(x,y)});
  ctx.stroke();
}
function renderLinkDiag(stats){
  const w=stats.websocket||{},p=stats.protocol_log||{};
  const rows=[
    ['ws connected',String(!!w.connected),w.connected?'ok':'fail'],
    ['ws reconnects (client)',String(clientReconnects)],
    ['ws rejects (server)',String(w.rejects??0),(w.rejects?'warn':'')],
    ['ws peer closes',String(w.peer_closes??0)],
    ['ws read failures',String(w.read_failures??0),(w.read_failures?'warn':'')],
    ['ws write failures',String(w.write_failures??0),(w.write_failures?'fail':'')],
    ['ws last close',String(w.last_close_reason??'none')],
    ['log dropped records',String(p.dropped_records??0),(p.dropped_records?'warn':'')],
    ['log overwritten',String(p.overwritten_records??0)],
    ['ui records cached',String(logCache.length)],
    ['ui records deduped',String(dedupeDrops)],
    ['ui cache bytes',((logCacheBytes/1024).toFixed(1))+' KiB']
  ];
  $('linkDiag').innerHTML=rows.map(r=>`<div class="linkrow"><span class="key">${r[0]}</span><span class="value ${r[2]||''}">${r[1]}</span></div>`).join('');
}
let clientReconnects=0,dedupeDrops=0;
function applyStale(){
  const hdr=$('hdr');
  hdr.classList.remove('stale','disconnected');
  const open=logSocket&&logSocket.readyState===WebSocket.OPEN;
  if(open){
    if(Date.now()-lastHealthStreamMs>STALE_MS){
      hdr.classList.add('stale');
      hdr.dataset.banner='STALE — no live update for '+Math.round((Date.now()-lastHealthStreamMs)/1000)+'s';
    }
  }else if(!lastHealthOkMs||Date.now()-lastHealthOkMs>STALE_MS){
    hdr.classList.add('disconnected');
    hdr.dataset.banner='LINK DOWN — stream disconnected';
    if($('verdict').className.indexOf('fail')<0){$('verdict').className='status unknown';$('verdict').textContent='link down'}
  }
}

/* ---------------- derived metrics from packet records ---------------- */
function deriveRecord(e){
  if(!e||typeof e!=='object')return;
  if(e.type==='command'){dv.lastCmd={button:e.button,ok:e.ok,reason:e.reason,ms:e.ms,ack:null};return}
  if(e.type==='state'&&dv.lastCmd&&!dv.lastCmd.ack){
    if(e.ms-dv.lastCmd.ms<4000){
      const b=dv.lastCmd.button;
      if((b==='open'&&/open/i.test(e.state))||(b==='close'&&/clos/i.test(e.state))||(b==='stop'&&/stop|stand/i.test(e.state)))
        dv.lastCmd.ack='ok';
    }
    return;
  }
  if(e.type!=='protocol'||typeof e.event_us!=='number')return;
  if(dv.src!==null&&e.source!==dv.src){dv.pendUs=null;dv.polls=[];dv.miss=0} // source switch: segment
  dv.src=e.source;
  if(e.event==='rx'&&e.frame==='status_poll'){
    if(dv.pendUs!==null){dv.miss++;if(dv.miss>dv.maxMiss)dv.maxMiss=dv.miss}
    dv.pendUs=e.event_us;
    dv.polls.push(e.ms);if(dv.polls.length>POLL_RING)dv.polls.shift();
    // drop poll timestamps older than 10s for the rate window
    while(dv.polls.length>2&&dv.polls[dv.polls.length-1]-dv.polls[0]>10000)dv.polls.shift();
  }else if(e.event==='tx'&&dv.pendUs!==null){
    let raw=(e.event_us-dv.pendUs)>>>0;
    dv.pendUs=null;dv.miss=0;
    if(raw>0&&raw<1000000){
      const jit=raw-RESPONSE_DELAY_US;
      dv.lastJit=jit;dv.jit.push(jit);if(dv.jit.length>JITTER_RING)dv.jit.shift();
    }
  }
}

/* ---------------- rolling browser cache + export ---------------- */
function resetLogCache(){logCache=[];logCacheBytes=0;logSeenSeqs=new Set();updateLogCacheSummary()}
function pruneLogCache(now=Date.now()){
  const minMs=now-maxCacheAgeMs;
  while(logCache.length&&(logCache[0].received_ms<minMs||logCacheBytes>maxCacheBytes)){
    const removed=logCache.shift();logCacheBytes-=removed.bytes||0;
  }
  if(logCacheBytes<0)logCacheBytes=0;
}
function updateLogCacheSummary(){
  pruneLogCache();
  const kb=(logCacheBytes/1024).toFixed(1);
  const oldest=logCache[0]?.received_at||'none',newest=logCache[logCache.length-1]?.received_at||'none';
  $('logCache').textContent=`visible ${logLines.length} lines; browser cache ${logCache.length} records, ${kb} KiB (newest 30 min / 100 MiB), ${oldest} to ${newest}`;
}
function cacheLogLine(line,receivedMs=Date.now()){
  if(!line)return false;
  let entry=null;try{entry=JSON.parse(line)}catch(e){}
  if(entry&&typeof entry.seq==='number'){
    if(logSeenSeqs.has(entry.seq)){dedupeDrops++;return false}
    logSeenSeqs.add(entry.seq);
  }
  const record={received_at:new Date(receivedMs).toISOString(),received_ms:receivedMs,bytes:line.length,raw:line};
  if(entry&&typeof entry==='object'){record.entry=entry;deriveRecord(entry)}else record.parse_error=true;
  logCache.push(record);logCacheBytes+=record.bytes;pruneLogCache(receivedMs);
  return true;
}
function cachedLogExport(){
  pruneLogCache();
  return{
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
  const stamp=new Date().toISOString().replace(/[:.]/g,'-');
  const fname=`hcp2-debug-log-${stamp}.json`;
  if(navigator.share&&navigator.canShare){
    try{const f=new File([blob],fname,{type:'application/json'});if(navigator.canShare({files:[f]})){navigator.share({files:[f],title:'HCP2 debug log'}).catch(()=>{});return}}catch(e){}
  }
  const url=URL.createObjectURL(blob),a=document.createElement('a');
  a.href=url;a.download=fname;document.body.appendChild(a);a.click();a.remove();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}

/* ---------------- live log display ---------------- */
function matchFilter(line){
  const err=$('fErr').checked,dir=$('fDir').value,fr=$('fFrame').value,ty=$('fType').value,txt=$('fText').value.toLowerCase();
  if(!err&&!dir&&!fr&&!ty&&!txt)return true;
  let e=null;try{e=JSON.parse(line)}catch(_){return !err&&!dir&&!fr&&!ty&&(!txt||line.toLowerCase().includes(txt))}
  if(err&&!(e.event==='bad_crc'||e.event==='rx_error'||e.ok===false))return false;
  if(dir&&e.event!==dir)return false;
  if(fr&&e.frame!==fr)return false;
  if(ty&&e.type!==ty)return false;
  if(txt&&!line.toLowerCase().includes(txt))return false;
  return true;
}
function logNearBottom(el=$('log')){return el.scrollHeight-el.scrollTop-el.clientHeight<=logStickThresholdPx}
function logDisplayHasContent(){return $('log').textContent.length>0||(logPendingReplace!==null&&logPendingReplace.length>0)||logPendingAppend.length>0}
function trimDisplay(){
  if(logLines.length<=LOG_DISPLAY_MAX)return false;
  if(!logNearBottom())return false;            // defer trim while user scrolled up
  logLines=logLines.slice(-LOG_DISPLAY_MAX);
  logPendingReplace=logLines.filter(matchFilter).join('\n');logPendingAppend=[];
  return true;
}
function renderLogNow(){
  if(displayPaused)return;
  const el=$('log');
  const stick=logRenderStickToBottom&&logNearBottom(el);
  const scrollTop=el.scrollTop;
  if(logPendingReplace!==null){el.textContent=logPendingReplace;logPendingReplace=null}
  if(logPendingAppend.length){el.appendChild(document.createTextNode(logPendingAppend.join('')));logPendingAppend=[]}
  if(stick){el.scrollTop=el.scrollHeight;$('logNewPill').classList.remove('show')}
  else{el.scrollTop=scrollTop;if(logDisplayHasContent())$('logNewPill').classList.add('show')}
  if(tableMode)renderTable();
}
function renderLog(stickToBottom=true){
  logRenderStickToBottom=logRenderStickToBottom&&stickToBottom;
  if(logRenderQueued)return;
  logRenderStickToBottom=stickToBottom;logRenderQueued=true;
  requestAnimationFrame(()=>{logRenderQueued=false;renderLogNow()});
}
function appendLogText(text,replace=false){
  const now=Date.now();
  const stickToBottom=replace||logNearBottom();
  if(replace){logLines=[];resetLogCache();resetDerived();logPendingAppend=[];logPendingReplace=''}
  let changed=replace;const newLines=[];
  for(const line of text.split('\n')){
    if(!line)continue;
    if(cacheLogLine(line,now)){logLines.push(line);if(matchFilter(line))newLines.push(line);changed=true}
  }
  if(changed){
    if(replace){logPendingReplace=logLines.filter(matchFilter).join('\n')}
    else if(newLines.length){const prefix=logDisplayHasContent()?'\n':'';logPendingAppend.push(prefix+newLines.join('\n'))}
    trimDisplay();
    renderLog(stickToBottom);
  }
  updateLogCacheSummary();
}
function rerenderDisplay(){
  logPendingReplace=logLines.filter(matchFilter).join('\n');logPendingAppend=[];
  logRenderQueued=false;renderLog(true);
}
function onFilter(){rerenderDisplay()}
function togglePause(){
  displayPaused=!displayPaused;
  $('pauseBtn').textContent=displayPaused?'Resume':'Pause';
  $('pauseBtn').classList.toggle('on',displayPaused);
  if(!displayPaused){rerenderDisplay()}
}
function toggleMode(){
  tableMode=!tableMode;
  $('modeBtn').textContent=tableMode?'Text view':'Table view';
  $('modeBtn').classList.toggle('on',tableMode);
  $('log').classList.toggle('hide',tableMode);
  $('logtbl').classList.toggle('show',tableMode);
  if(tableMode)renderTable();
}
function decodeOne(e){
  if(e.type==='protocol')return{dir:e.event,fr:e.frame,bt:'',crc:(e.event==='bad_crc'?'BAD':e.event==='rx_error'?'ERR':'ok'),len:e.len,hex:e.hex||''};
  if(e.type==='command')return{dir:'cmd',fr:e.phase,bt:e.button,crc:e.ok===false?'NAK':'ok',len:'',hex:e.reason||''};
  if(e.type==='state')return{dir:'st',fr:e.state,bt:'',crc:'',len:'',hex:'pos '+(e.current_position)+'/'+(e.target_position)};
  if(e.type==='lp_trace')return{dir:'lp',fr:e.event,bt:'',crc:'',len:'',hex:'v='+e.value};
  return{dir:e.type||'?',fr:'',bt:'',crc:'',len:'',hex:''};
}
function renderTable(){
  const tbl=$('logtbl');
  const stick=logNearBottom(tbl)||tbl.scrollTop===0;
  const lines=logLines.filter(matchFilter).slice(-LOG_DISPLAY_MAX);
  let prev={};const cells=['<div class="lgrid lh"><div>seq</div><div>ms</div><div>&Delta;ms</div><div>dir</div><div>frame</div><div>btn/evt</div><div>crc</div><div>len</div><div>hex</div></div>'];
  for(const line of lines){
    let e;try{e=JSON.parse(line)}catch(_){continue}
    const d=decodeOne(e);const dm=(prev[e.frame||e.type]!=null)?(e.ms-prev[e.frame||e.type]):'';prev[e.frame||e.type]=e.ms;
    let cls='lr '+(d.dir==='rx'?'rx':d.dir==='tx'?'tx':'');
    if(d.crc==='BAD'||d.crc==='ERR')cls+=' err';if(d.crc==='NAK')cls+=' cmderr';
    cells.push(`<div class="${cls}"><div>${e.seq??''}</div><div>${e.ms??''}</div><div>${dm}</div><div class="c-dir">${d.dir}</div><div class="c-fr">${d.fr||''}</div><div>${d.bt||''}</div><div class="c-crc">${d.crc||''}</div><div>${d.len}</div><div>${d.hex}</div></div>`);
  }
  tbl.innerHTML=cells.join('');
  if(stick)tbl.scrollTop=tbl.scrollHeight;
}
function jumpToBottom(){const el=tableMode?$('logtbl'):$('log');el.scrollTop=el.scrollHeight;$('logNewPill').classList.remove('show')}
function copyRaw(){const t=logLines.filter(matchFilter).map(l=>{const r=logCache.find(x=>x.raw===l);return r?r.raw:l}).join('\n');navigator.clipboard&&navigator.clipboard.writeText(t)}
function copyDecoded(){
  const t=logLines.filter(matchFilter).map(l=>{let e;try{e=JSON.parse(l)}catch(_){return l}const d=decodeOne(e);return [e.seq,e.ms,d.dir,d.fr,d.bt,d.crc,d.hex].filter(x=>x!=='').join(' ')}).join('\n');
  navigator.clipboard&&navigator.clipboard.writeText(t);
}
function clearLocalView(){logLines=[];logPendingAppend=[];logPendingReplace='';resetLogCache();resetDerived();renderLog(true);if(tableMode)renderTable()}

/* ---------------- device control + stream ---------------- */
function requestRefresh(){refresh().catch(()=>{})}
async function refresh(){
  if(refreshBusy)return;refreshBusy=true;
  try{applyHealth(await getJson('/health'),'http')}
  catch(e){
    if(!lastHealthOkMs){$('verdict').className='status unknown';$('verdict').textContent='debug fetch failed'}
    $('updated').textContent='last fetch failed '+new Date().toLocaleTimeString();
    applyStale();
  }finally{refreshBusy=false}
}
async function controlLog(action){
  await fetch('/hcp2_log/'+action,{cache:'no-store'});
  requestRefresh();
  if(action==='clear'){logLines=[];logPendingAppend=[];logPendingReplace='';resetLogCache();resetDerived();renderLog(true);await refreshLog(false)}
  else await refreshLog();
}
async function refreshLog(replace=true){
  if(logLoadBusy)return;logLoadBusy=true;
  try{const r=await fetch('/hcp2_log',{cache:'no-store'});appendLogText(await r.text(),replace)}
  catch(e){$('logStream').textContent='log refresh failed: '+e.message}
  finally{logLoadBusy=false}
}
function closeLogStream(){
  if(logReconnectTimer){clearTimeout(logReconnectTimer);logReconnectTimer=null}
  if(logSocket){logSocket.onclose=null;try{logSocket.close()}catch(e){};logSocket=null}
}
function connectLogStream(replace=false){
  closeLogStream();resetDerived();
  lastHealthStreamMs=Date.now();
  const scheme=location.protocol==='https:'?'wss':'ws';
  const path=replace?'/hcp2_log/ws?replace=1':'/hcp2_log/ws';
  const ws=new WebSocket(`${scheme}://${location.host}${path}`);
  logSocket=ws;$('logStream').textContent='stream connecting';
  ws.onopen=()=>{$('logStream').textContent='stream connected';logReconnectDelayMs=1000;refreshLog(false)};
  ws.onmessage=event=>{
    let message=null;try{message=JSON.parse(event.data)}catch(e){}
    if(message&&message.type==='health'&&message.health){applyHealth(message.health,'stream');return}
    if(message&&message.type==='log'&&typeof message.text==='string'){appendLogText(message.text);return}
    appendLogText(event.data);
  };
  ws.onerror=()=>{try{ws.close()}catch(e){}};
  ws.onclose=()=>{
    if(logSocket===ws)logSocket=null;
    const delay=logReconnectDelayMs;logReconnectDelayMs=Math.min(logReconnectDelayMs*2,10000);
    clientReconnects++;
    $('logStream').textContent=`stream disconnected; reconnecting in ${Math.round(delay/1000)}s`;
    applyStale();
    logReconnectTimer=setTimeout(()=>connectLogStream(false),delay);
  };
}
window.addEventListener('pagehide',closeLogStream);
async function refreshRaw(){
  if(!rawPath||rawBusy)return;rawBusy=true;
  try{$('raw').textContent=JSON.stringify(await getJson(rawPath,2),null,2)}
  catch(e){$('raw').textContent='refresh failed: '+e.message}
  finally{rawBusy=false}
}
async function loadRaw(path){rawPath=path;await refreshRaw()}
// drawer state persistence (outside the test-critical counters/stats path)
try{if(localStorage.getItem('hcp2drawer')==='1')$('drawer').open=true;
  $('drawer').addEventListener('toggle',()=>{try{localStorage.setItem('hcp2drawer',$('drawer').open?'1':'0')}catch(e){}});}catch(e){}
// gentle fallbacks (unchanged cadence): /health only when stream stale, /stats only when a Raw path is selected
setInterval(()=>{if(logSocket&&logSocket.readyState===WebSocket.CONNECTING)return;if(Date.now()-lastHealthStreamMs>STALE_MS)requestRefresh();applyStale()},1000);
setInterval(()=>{if(!logSocket||logSocket.readyState!==WebSocket.OPEN)requestRefresh()},5000);
setInterval(()=>{refreshRaw().catch(()=>{})},3000);
async function init(){
  connectLogStream();
  setTimeout(()=>{if((!logSocket||logSocket.readyState!==WebSocket.CONNECTING)&&Date.now()-lastHealthStreamMs>STALE_MS)requestRefresh()},2500);
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
