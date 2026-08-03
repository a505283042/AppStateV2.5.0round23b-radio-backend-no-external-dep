#pragma once

#include <pgmspace.h>
#include "web/web_config.h"

static const char WEBCTRL_FEEDBACK_JS[] PROGMEM = R"JS(
(() => {
  'use strict';

  const STORAGE_KEY = 'webctrl_haptic_feedback';
  const TARGET_SELECTOR = 'button,a[href],.chip';
  const pressedByPointer = new Map();
  let hapticEnabled = true;
  let webNoticeSequence = 0;
  let webNoticeInitialized = false;
  let webNoticePollTimer = null;
  let webNoticeHideTimer = null;

  function installStyle(){
    if(document.getElementById('webButtonFeedbackStyle')) return;
    const style = document.createElement('style');
    style.id = 'webButtonFeedbackStyle';
    style.textContent = `
      button,a[href],.chip{
        -webkit-tap-highlight-color:transparent;
        touch-action:manipulation;
        transition:transform .08s ease,filter .12s ease,box-shadow .12s ease,opacity .12s ease;
      }
      .webfb-pressed{
        transform:scale(.965)!important;
        filter:brightness(1.18);
        box-shadow:0 0 0 2px rgba(121,192,255,.38),inset 0 2px 7px rgba(0,0,0,.28)!important;
      }
      .webfb-ack{animation:webfbAck .22s ease-out;}
      .webfb-success{animation:webfbSuccess .34s ease-out;}
      .webfb-error{animation:webfbError .34s ease-out;}
      .web-global-notice{
        position:fixed;left:50%;top:calc(14px + env(safe-area-inset-top));z-index:10020;
        width:min(430px,calc(100vw - 28px));display:grid;grid-template-columns:1fr auto;gap:12px;
        padding:14px 14px 14px 16px;border:1px solid rgba(121,192,255,.7);border-radius:14px;
        background:rgba(22,31,43,.97);color:#f0f6fc;box-shadow:0 14px 40px rgba(0,0,0,.48);
        opacity:0;transform:translate(-50%,-18px);pointer-events:none;
        transition:opacity .18s ease,transform .18s ease;
      }
      .web-global-notice.show{opacity:1;transform:translate(-50%,0);pointer-events:auto}
      .web-global-notice.warning{border-color:#d29922;background:rgba(72,49,8,.97)}
      .web-global-notice.error{border-color:#f85149;background:rgba(82,22,22,.97)}
      .web-global-notice-title{display:block;font-size:16px;font-weight:800;line-height:1.3}
      .web-global-notice-detail{display:block;margin-top:4px;font-size:14px;line-height:1.45;color:#c9d1d9}
      .web-global-notice.warning .web-global-notice-detail{color:#f2cc60}
      .web-global-notice.error .web-global-notice-detail{color:#ffa198}
      .web-global-notice-close{align-self:start;min-width:34px;height:34px;padding:0;border:0;border-radius:10px;background:rgba(255,255,255,.1);color:#fff;font-size:22px;line-height:1;cursor:pointer}
      @keyframes webfbAck{
        0%{box-shadow:0 0 0 0 rgba(121,192,255,.62)}
        100%{box-shadow:0 0 0 8px rgba(121,192,255,0)}
      }
      @keyframes webfbSuccess{
        0%{box-shadow:0 0 0 0 rgba(63,185,80,.72)}
        100%{box-shadow:0 0 0 9px rgba(63,185,80,0)}
      }
      @keyframes webfbError{
        0%{box-shadow:0 0 0 0 rgba(248,81,73,.75)}
        100%{box-shadow:0 0 0 9px rgba(248,81,73,0)}
      }
      button:disabled,.webfb-disabled,[aria-disabled="true"]{
        transform:none!important;
        filter:none!important;
        box-shadow:none!important;
      }
      @media (prefers-reduced-motion:reduce){
        button,a[href],.chip{transition:none!important}
        .webfb-ack,.webfb-success,.webfb-error{animation:none!important}
      }
    `;
    document.head.appendChild(style);
  }

  function readHapticEnabled(){
    try{
      const stored = localStorage.getItem(STORAGE_KEY);
      return stored === null ? true : stored !== '0';
    }catch(e){
      return true;
    }
  }

  function writeHapticEnabled(enabled){
    hapticEnabled = !!enabled;
    try{
      localStorage.setItem(STORAGE_KEY, hapticEnabled ? '1' : '0');
    }catch(e){}
    syncHapticToggle();
  }

  function hapticSupported(){
    return typeof navigator.vibrate === 'function';
  }

  function vibrate(durationMs=10){
    if(!hapticEnabled || !hapticSupported()) return false;
    try{
      return navigator.vibrate(Math.max(1, Math.min(30, Number(durationMs) || 10))) === true;
    }catch(e){
      return false;
    }
  }

  function targetFromEvent(event){
    const target = event && event.target;
    if(!(target instanceof Element)) return null;
    const el = target.closest(TARGET_SELECTOR);
    if(!el) return null;
    if(el.matches('button:disabled,[aria-disabled="true"],.webfb-disabled')) return null;
    return el;
  }

  function restartAnimation(el, className, durationMs){
    if(!el) return;
    el.classList.remove(className);
    void el.offsetWidth;
    el.classList.add(className);
    window.setTimeout(() => el.classList.remove(className), durationMs);
  }

  function releasePointer(pointerId, acknowledge){
    const el = pressedByPointer.get(pointerId);
    if(!el) return;
    pressedByPointer.delete(pointerId);
    el.classList.remove('webfb-pressed');
    if(acknowledge) restartAnimation(el, 'webfb-ack', 240);
  }

  function syncHapticToggle(){
    const toggle = document.querySelector('[data-web-feedback-haptic-toggle]');
    const supported = hapticSupported();
    if(toggle){
      toggle.checked = hapticEnabled && supported;
      toggle.disabled = !supported;
    }
  }

  function ensureWebNoticePopup(){
    let popup = document.getElementById('webGlobalNotice');
    if(popup) return popup;

    popup = document.createElement('div');
    popup.id = 'webGlobalNotice';
    popup.className = 'web-global-notice';
    popup.setAttribute('role', 'alert');
    popup.setAttribute('aria-live', 'assertive');
    popup.innerHTML = `
      <div>
        <strong class="web-global-notice-title"></strong>
        <span class="web-global-notice-detail"></span>
      </div>
      <button type="button" class="web-global-notice-close" aria-label="关闭提示">×</button>`;
    document.body.appendChild(popup);
    popup.querySelector('.web-global-notice-close').addEventListener('click', () => {
      popup.classList.remove('show');
      if(webNoticeHideTimer){ clearTimeout(webNoticeHideTimer); webNoticeHideTimer = null; }
    });
    return popup;
  }

  function showWebNotice(title, detail, level='info'){
    const popup = ensureWebNoticePopup();
    popup.querySelector('.web-global-notice-title').textContent = title || '设备提示';
    popup.querySelector('.web-global-notice-detail').textContent = detail || '';
    popup.classList.remove('info','warning','error','show');
    popup.classList.add(level === 'error' ? 'error' : (level === 'warning' ? 'warning' : 'info'));
    void popup.offsetWidth;
    popup.classList.add('show');

    if(webNoticeHideTimer) clearTimeout(webNoticeHideTimer);
    const duration = level === 'error' ? 7000 : (level === 'warning' ? 5500 : 3500);
    webNoticeHideTimer = setTimeout(() => popup.classList.remove('show'), duration);
    if(level === 'error') vibrate(24);
    else if(level === 'warning') vibrate(16);
  }

  function scheduleWebNoticePoll(delayMs){
    if(webNoticePollTimer) clearTimeout(webNoticePollTimer);
    webNoticePollTimer = setTimeout(pollWebNotice, Math.max(500, Number(delayMs) || 1200));
  }

  async function pollWebNotice(){
    try{
      const r = await fetch(`/api/notice?after=${encodeURIComponent(webNoticeSequence)}`, {cache:'no-store'});
      const j = await r.json();
      if(j && j.ok){
        const sequence = Number(j.sequence) || 0;
        if(!webNoticeInitialized){
          webNoticeInitialized = true;
          // 页面刚打开时只显示最近1.5秒内的新提示，避免弹出历史机械故障。
          if(j.changed && j.active && Number(j.age_ms) <= 1500){
            showWebNotice(j.title, j.detail, j.level);
          }
        }else if(j.changed && j.active && sequence !== webNoticeSequence){
          showWebNotice(j.title, j.detail, j.level);
        }
        webNoticeSequence = sequence;
      }
    }catch(e){}
    scheduleWebNoticePoll(document.hidden ? 3500 : 1000);
  }

  function init(){
    installStyle();
    hapticEnabled = readHapticEnabled();
    syncHapticToggle();
    ensureWebNoticePopup();
    scheduleWebNoticePoll(120);

    const toggle = document.querySelector('[data-web-feedback-haptic-toggle]');
    if(toggle){
      toggle.addEventListener('change', () => writeHapticEnabled(toggle.checked));
    }
  }

  document.addEventListener('pointerdown', event => {
    if(event.button !== undefined && event.button !== 0) return;
    const el = targetFromEvent(event);
    if(!el) return;
    pressedByPointer.set(event.pointerId, el);
    el.classList.add('webfb-pressed');
    if(event.pointerType === 'touch' || event.pointerType === 'pen') vibrate(10);
  }, true);

  document.addEventListener('pointerup', event => releasePointer(event.pointerId, true), true);
  document.addEventListener('pointercancel', event => releasePointer(event.pointerId, false), true);

  document.addEventListener('keydown', event => {
    if(event.repeat || (event.key !== 'Enter' && event.key !== ' ')) return;
    const el = targetFromEvent(event);
    if(el) el.classList.add('webfb-pressed');
  }, true);

  document.addEventListener('keyup', event => {
    if(event.key !== 'Enter' && event.key !== ' ') return;
    const el = targetFromEvent(event);
    if(!el) return;
    el.classList.remove('webfb-pressed');
    restartAnimation(el, 'webfb-ack', 240);
  }, true);

  document.addEventListener('visibilitychange', () => {
    if(!document.hidden) return;
    pressedByPointer.forEach(el => el.classList.remove('webfb-pressed'));
    pressedByPointer.clear();
  });

  window.webButtonFeedback = {
    isHapticEnabled: () => hapticEnabled,
    setHapticEnabled: writeHapticEnabled,
    vibrate,
    acknowledge: el => restartAnimation(el, 'webfb-ack', 240),
    success: el => restartAnimation(el, 'webfb-success', 360),
    error: el => restartAnimation(el, 'webfb-error', 360)
  };
  window.webNoticePopup = showWebNotice;

  if(document.readyState === 'loading'){
    document.addEventListener('DOMContentLoaded', init, {once:true});
  }else{
    init();
  }
})();
)JS";

static const char WEBCTRL_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <script src="/web-feedback.js" defer></script>
  <title>ESP32S3 播放器控制</title>
  <style>
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    h1{font-size:22px;margin:0 0 8px}
    .muted{color:#aaa;font-size:14px}
    .title{font-size:22px;font-weight:700;margin:6px 0 4px}
    .sub{font-size:16px;color:#cfcfcf;margin:2px 0}
    .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
    .k{font-size:13px;color:#aaa}
    .v{font-size:16px;font-weight:600;margin-top:2px}
    .bar{height:10px;background:#333;border-radius:999px;overflow:hidden;margin-top:10px}
    .fill{height:100%;width:0;background:#79c0ff}
    .control-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}
    .control-grid + .control-grid{margin-top:10px}
    .control-grid.two{grid-template-columns:repeat(2,minmax(0,1fr))}
    .control-grid.one{grid-template-columns:minmax(0,1fr)}
    .control-grid button{width:100%;min-width:0;min-height:48px;padding:12px 8px;line-height:1.25;white-space:normal}
    button,.linkbtn{border:none;border-radius:12px;padding:14px 10px;background:#2f6feb;color:#fff;font-size:16px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    button.secondary,.linkbtn.secondary{background:#444}
    button.warn{background:#a04040}
    .status{display:flex;justify-content:space-between;gap:8px;align-items:center;flex-wrap:wrap}
    .small{font-size:12px;color:#aaa}
    .media{display:grid;grid-template-columns:112px 1fr;gap:14px;align-items:start}
    .media.noCover{grid-template-columns:112px 1fr}
    .cover{width:112px;height:112px;border-radius:14px;background:#2a2a2a;overflow:hidden;display:flex;align-items:center;justify-content:center;color:#8e8e8e;font-size:13px;cursor:pointer;user-select:none}
    .cover img{width:100%;height:100%;object-fit:cover;display:block}
    .cover-hint{width:112px;margin-top:7px;color:#888;font-size:12px;text-align:center;line-height:1.3}
    .cover.rotate{border-radius:50%;padding:4px;background:#202020}
    .cover.rotate img{border-radius:50%}
    .cover.spin img{animation:webCoverSpin 12s linear infinite}
    .cover.coverPanel{
    position:relative;
    border-radius:16px;
    background:#181818;
    box-shadow:inset 0 0 0 1px rgba(255,255,255,.06),0 8px 22px rgba(0,0,0,.28);
  }

  .cover.coverPanel img{
    position:absolute;
    left:4px;
    top:4px;
    width:104px;
    height:104px;
    border-radius:50%;
    object-fit:cover;
  }

  .cover.coverPanel.coverReady::after{
    content:"";
    position:absolute;
    left:0;
    right:0;
    bottom:0;
    height:48px;
    background:linear-gradient(
      180deg,
      rgba(24,24,24,0) 0%,
      rgba(24,24,24,.35) 16%,
      #181818 34%,
      #181818 100%
    );
    box-shadow:0 -10px 18px rgba(0,0,0,.22) inset;
    z-index:2;
    pointer-events:none;
  }

  .cover.coverPanel.coverReady::before{
    content:"";
    position:absolute;
    left:50%;
    top:56px;
    width:13px;
    height:13px;
    transform:translate(-50%,-50%);
    border-radius:50%;
    background:#151515;
    box-shadow:0 0 0 2px rgba(255,255,255,.08);
    z-index:3;
    pointer-events:none;
  }

  .cover.coverPanel span{
    position:relative;
    z-index:4;
  }
    @keyframes webCoverSpin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
    .lyrics{line-height:1.5}
    .lyrics .line{font-size:18px;font-weight:700;margin:0 0 8px}
    .lyrics .next{font-size:14px;color:#bdbdbd}
    .volrow{display:flex;align-items:center;gap:12px;margin-top:8px}
    .volrow input[type=range]{flex:1}
    input[type=range]{accent-color:#79c0ff}
    .seekbar{width:100%;margin-top:14px;cursor:pointer}
    .seekbar:disabled{cursor:not-allowed;opacity:.45}
    .web-lock-disabled,[data-page-lock-target="1"][aria-disabled="true"]{opacity:.45!important;cursor:not-allowed!important}
    .web-lock-toast{position:fixed;left:50%;bottom:calc(24px + env(safe-area-inset-bottom));z-index:9999;max-width:calc(100vw - 32px);padding:11px 16px;border-radius:999px;background:rgba(180,35,24,.96);color:#fff;font-size:14px;font-weight:700;box-shadow:0 8px 28px rgba(0,0,0,.38);opacity:0;transform:translate(-50%,12px);pointer-events:none;transition:opacity .16s ease,transform .16s ease}
    .web-lock-toast.show{opacity:1;transform:translate(-50%,0)}
    .nettrack-only{display:none}
    body.nettrack-mode .nettrack-only{display:block}
    .battery-summary-main{font-size:22px;font-weight:800;font-variant-numeric:tabular-nums}
    .battery-summary-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin-top:12px}
    .battery-summary-grid .v{font-size:14px;word-break:break-word}
    @media(max-width:560px){.battery-summary-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
    body.nettrack-mode .hide-when-nettrack{display:none!important}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <div class="status">
        <div>
          <h1 style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">
            <span>ESP32S3 播放器</span>
            <button id="lockBtn" class="secondary" type="button" style="padding:8px 12px;font-size:13px">锁定</button>
          </h1>
        </div>
      </div>
      <div class="nav" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px">
        <a class="linkbtn secondary" href="/artists" style="padding:10px 12px;font-size:14px">歌手页</a>
        <a class="linkbtn secondary" href="/albums" style="padding:10px 12px;font-size:14px">专辑页</a>
        <a class="linkbtn secondary" href="/nfc" style="padding:10px 12px;font-size:14px">NFC管理</a>
        <a class="linkbtn secondary" href="/radios" style="padding:10px 12px;font-size:14px">电台页</a>
        <a class="linkbtn secondary" href="/netmusic" style="padding:10px 12px;font-size:14px">NAS页</a>
        <a class="linkbtn secondary" href="/settings" style="padding:10px 12px;font-size:14px">网页设置</a>
      </div>
    </div>

    <div class="card">
      <div class="media" id="mediaBox">
        <div>
          <div class="cover" id="coverBox"><span id="coverFallback">无封面</span><img id="coverImg" alt="封面" decoding="async" loading="eager" style="display:none"></div>
          <div class="cover-hint">点击封面切换视图</div>
        </div>
        <div>
          <div class="title" id="title">-</div>
          <div class="sub" id="artist">-</div>
          <div class="sub" id="album">-</div>
          <input id="seekSlider" class="seekbar" type="range" min="0" max="0" value="0" step="250" disabled aria-label="播放进度">
          <div class="status" style="margin-top:8px">
            <div class="small" id="time">0:00 / 0:00</div>
            <div class="small" id="playState">-</div>
          </div>
        </div>
      </div>
    </div>

    <div class="card grid">
      <div><div class="k">播放模式</div><div class="v" id="mode">-</div></div>
      <div><div class="k">音量</div><div class="v" id="volume">-</div></div>
      <div><div class="k">列表位置</div><div class="v" id="displayPos">-</div></div>
      <div><div class="k">应用状态</div><div class="v" id="appState">-</div></div>
    </div>

    <div class="card">
      <div class="k" style="display:flex;align-items:center;justify-content:space-between;gap:10px">
        <span>网页音量调节</span>
        <button id="volumeLockBtn" class="secondary" type="button" style="padding:8px 12px;font-size:13px">音量锁</button>
      </div>
      <div class="volrow">
        <span class="small">0</span>
        <input id="volumeSlider" type="range" min="0" max="100" value="0" step="1">
        <span class="small">100</span>
      </div>
    </div>

    <div class="card lyrics">
      <div class="k">歌词摘要</div>
      <div class="line" id="lyricCurrent">-</div>
      <div class="next" id="lyricNext">-</div>
    </div>

    <div class="card hide-when-nettrack" id="mainControlCard">
      <div class="control-grid">
        <button class="secondary" id="prevBtn" onclick="handlePrev()">上一首</button>
        <button id="playPauseBtn" onclick="sendCmd('/api/playpause')">播放/暂停</button>
        <button class="secondary" id="nextBtn" onclick="handleNext()">下一首</button>
      </div>
      <div class="control-grid" id="modeRow">
        <button class="secondary" id="modeToggleBtn" onclick="sendCmd('/api/mode/toggle')">顺序/随机</button>
        <button class="secondary" id="modeCategoryBtn" onclick="sendCmd('/api/mode/category')">全部/歌手/专辑</button>
        <button class="secondary" id="saveStateBtn" onclick="savePlayerState()">保存当前状态</button>
      </div>
      <div class="control-grid one" id="radioBackRow" style="display:none">
        <button class="secondary" id="radioBackBtn" onclick="returnFromRadio()">返回音乐播放</button>
      </div>
      <div class="small" style="margin-top:8px">保存到设备内部 NVS：音量、当前歌曲、播放模式、当前分组与视图</div>
    </div>

    <div class="card nettrack-only" id="netTrackControlCard">
      <div class="control-grid">
        <button class="secondary" onclick="nasControl('/api/netmusic/prev')">上一首</button>
        <button onclick="nasControl('/api/netmusic/toggle')">播放/暂停</button>
        <button class="secondary" onclick="nasControl('/api/netmusic/next')">下一首</button>
      </div>
      <div class="control-grid two">
        <button class="secondary" onclick="nasControl('/api/netmusic/mode')" id="netTrackModeBtn">顺序/随机</button>
        <button class="secondary" onclick="nasControl('/api/netmusic/return-local')">返回本地播放</button>
      </div>
    </div>

    <div class="card" id="batterySummaryCard">
      <div class="status">
        <div>
          <div class="k">电池</div>
          <div class="battery-summary-main" id="batterySummaryMain">正在读取...</div>
        </div>
        <div class="small" id="batterySummaryState">-</div>
      </div>
      <div class="bar"><div class="fill" id="batterySummaryFill"></div></div>
      <div class="battery-summary-grid">
        <div><div class="k">电压</div><div class="v" id="batterySummaryVoltage">-</div></div>
        <div><div class="k">电流</div><div class="v" id="batterySummaryCurrent">-</div></div>
        <div><div class="k">预计续航</div><div class="v" id="batterySummaryRuntime">-</div></div>
        <div><div class="k">剩余容量</div><div class="v" id="batterySummaryCapacity">-</div></div>
      </div>
    </div>
  </div>

<script>
let POLL_MS = 1000;
const MIN_STATUS_CHECK_MS = 350;
const STATUS_POLL_JITTER_MS = 180;
const LOCAL_CLOCK_TICK_MS = 250;
const FORCE_FULL_STATUS_MS = 10000;
const BATTERY_SUMMARY_POLL_MS = 5000;
let LYRIC_WAIT_POLL_THRESHOLD_MS = 150;
let lastCoverTrack = '';
let pollTimer = null;
let lyricTimer = null;
let volumeTimer = null;
let inFlight = false;
let statusController = null;
let batterySummaryInFlight = false;
let statusFetchStartedAt = 0;
let currentPollMs = POLL_MS;
let nextPollAt = Date.now() + POLL_MS;
let lastStatus = null;
let lastStatusAt = 0;
let lastFullStatusAt = 0;
let lastStateToken = 0;
let localClockTimer = null;
let playClockBaseMs = 0;
let playClockBaseAt = performance.now();
let playClockTotalMs = 0;
let playClockPlaying = false;
let playClockPaused = false;
let playClockRescanning = false;
let playClockSeeking = false;
let playClockRevision = 0;
let coverToggleBusy = false;
let pageActive = !document.hidden;
let pagePausedByVisibility = false;
const LOCK_STORAGE_KEY = 'webctrl_page_locked';
let pageLocked = false;
let volumeLocked = true;
let seekDragging = false;
let seekOptimisticMs = null;
let seekOptimisticUntil = 0;

function getLockTargets(){
  return [
    ...document.querySelectorAll('.nav a'),
    ...document.querySelectorAll('#mainControlCard button, #netTrackControlCard button'),
    document.getElementById('coverBox'),
    document.getElementById('volumeLockBtn'),
    document.getElementById('seekSlider')
  ].filter(Boolean);
}

let pageLockNoticeTimer = null;
let pageLockNoticeLastAt = 0;

function showPageLockedNotice(){
  const now = performance.now();
  if(now - pageLockNoticeLastAt < 350) return;
  pageLockNoticeLastAt = now;

  let toast = document.getElementById('pageLockToast');
  if(!toast){
    toast = document.createElement('div');
    toast.id = 'pageLockToast';
    toast.className = 'web-lock-toast';
    toast.setAttribute('role', 'status');
    toast.setAttribute('aria-live', 'polite');
    document.body.appendChild(toast);
  }

  toast.textContent = '网页已锁定，请先点击右上角“解锁”';
  toast.classList.remove('show');
  void toast.offsetWidth;
  toast.classList.add('show');
  if(pageLockNoticeTimer) clearTimeout(pageLockNoticeTimer);
  pageLockNoticeTimer = setTimeout(() => toast.classList.remove('show'), 1500);
}

function lockedTargetFromEvent(event){
  const target = event && event.target;
  if(!(target instanceof Element)) return null;
  return target.closest('[data-page-lock-target="1"]');
}

function interceptLockedControl(event){
  if(!pageLocked) return;
  if(event.type === 'keydown' && event.key !== 'Enter' && event.key !== ' ') return;

  const target = lockedTargetFromEvent(event);
  if(!target) return;

  event.preventDefault();
  event.stopImmediatePropagation();
  showPageLockedNotice();
}

function applyLockState(){
  const btn = document.getElementById('lockBtn');
  if(btn){
    btn.textContent = pageLocked ? '解锁' : '锁定';
    btn.className = pageLocked ? 'warn' : 'secondary';
  }

  const targets = getLockTargets();
  targets.forEach(el => {
    if(!el) return;

    el.dataset.pageLockTarget = '1';
    el.classList.toggle('web-lock-disabled', pageLocked);
    el.classList.toggle('webfb-disabled', pageLocked);

    if(pageLocked){
      el.setAttribute('aria-disabled', 'true');
    }else{
      el.removeAttribute('aria-disabled');
    }

    // 滑块仍使用原生 disabled 阻止拖动；按钮和链接保留事件入口，
    // 由捕获阶段统一拦截并提示“网页已锁定”。
    if(el.tagName === 'INPUT'){
      if(pageLocked){
        if(!Object.prototype.hasOwnProperty.call(el.dataset, 'lockWasDisabled')){
          el.dataset.lockWasDisabled = el.disabled ? '1' : '0';
        }
        el.disabled = true;
      }else if(Object.prototype.hasOwnProperty.call(el.dataset, 'lockWasDisabled')){
        el.disabled = el.dataset.lockWasDisabled === '1';
        delete el.dataset.lockWasDisabled;
      }
    }
  });

  applyVolumeLockState();
}

function saveLockState(){
  try{
    localStorage.setItem(LOCK_STORAGE_KEY, pageLocked ? '1' : '0');
  }catch(e){}
}

function loadLockState(){
  try{
    pageLocked = localStorage.getItem(LOCK_STORAGE_KEY) === '1';
  }catch(e){
    pageLocked = false;
  }
}

function togglePageLock(){
  pageLocked = !pageLocked;
  applyLockState();
  saveLockState();
  if(lastStatus) render(lastStatus);
}

function applyVolumeLockState(){
  const btn = document.getElementById('volumeLockBtn');
  const slider = document.getElementById('volumeSlider');

  if(btn){
    btn.textContent = volumeLocked ? '音量解锁' : '音量锁';
    btn.className = volumeLocked ? 'warn' : 'secondary';
  }

  if(slider){
    slider.disabled = pageLocked || volumeLocked;
    slider.style.opacity = (pageLocked || volumeLocked) ? '0.45' : '';
  }
}

function webBoolValue(v, fallback){
  if(typeof v === 'boolean') return v;
  if(typeof v === 'number') return v !== 0;
  if(typeof v === 'string'){
    const s = v.toLowerCase();
    if(s === '1' || s === 'true' || s === 'on' || s === 'yes') return true;
    if(s === '0' || s === 'false' || s === 'off' || s === 'no') return false;
  }
  return fallback;
}

function syncVolumeLockFromStatus(j){
  if(!j || !Object.prototype.hasOwnProperty.call(j, 'volume_locked')) return;
  const nextLocked = webBoolValue(j.volume_locked, volumeLocked);
  volumeLocked = nextLocked;
  applyVolumeLockState();
}

async function toggleVolumeLock(){
  try{
    const r = await fetch('/api/ui/volume_lock', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:`value=${volumeLocked ? 0 : 1}`
    });
    const j = await r.json();
    if(!j || !j.ok){
      throw new Error((j && j.message) ? j.message : 'volume lock update failed');
    }
    syncVolumeLockFromStatus(j);
    scheduleNext(500);
  }catch(e){
    alert('音量锁状态同步失败');
  }
}

function scheduleNext(ms){
  if(!pageActive){
    if(pollTimer){
      clearTimeout(pollTimer);
      pollTimer = null;
    }
    return;
  }

  const baseDelay = Math.max(MIN_STATUS_CHECK_MS, Number(ms) || POLL_MS);
  const delay = baseDelay + Math.floor(Math.random() * STATUS_POLL_JITTER_MS);
  if(pollTimer) clearTimeout(pollTimer);
  nextPollAt = Date.now() + delay;
  pollTimer = setTimeout(pollStatus, delay);
}
function fmt(ms){ const s=Math.max(0,Math.floor((ms||0)/1000)); const m=Math.floor(s/60); const r=s%60; return `${m}:${String(r).padStart(2,'0')}`; }
function currentPlayClockMs(now = performance.now()){
  let play = Number(playClockBaseMs) || 0;
  if(playClockPlaying && !playClockPaused && !playClockRescanning && !playClockSeeking){
    play += Math.max(0, now - playClockBaseAt);
  }
  if(playClockTotalMs > 0) play = Math.min(play, playClockTotalMs);
  return Math.max(0, play);
}
function syncPlayClock(snap, force = false){
  if(!snap) return;
  const now = performance.now();
  const serverMs = Math.max(0, Number(snap.seeking ? (snap.seek_target_ms || snap.play_ms) : snap.play_ms) || 0);
  const previousMs = currentPlayClockMs(now);
  const revision = Number(snap.playback_revision) || 0;
  const changedRunState = revision !== playClockRevision ||
    !!snap.is_playing !== playClockPlaying ||
    !!snap.is_paused !== playClockPaused ||
    !!snap.rescanning !== playClockRescanning ||
    !!snap.seeking !== playClockSeeking;
  const drift = serverMs - previousMs;
  let correctedMs = serverMs;
  if(!force && !changedRunState && Math.abs(drift) < 500){
    // 小误差只吸收四分之一，避免网络校验让网页时间来回跳动。
    correctedMs = previousMs + drift * 0.25;
  }
  playClockBaseMs = Math.max(0, correctedMs);
  playClockBaseAt = now;
  if(typeof snap.total_ms !== 'undefined'){
    playClockTotalMs = Math.max(0, Number(snap.total_ms) || 0);
  }
  playClockPlaying = !!snap.is_playing;
  playClockPaused = !!snap.is_paused;
  playClockRescanning = !!snap.rescanning;
  playClockSeeking = !!snap.seeking;
  playClockRevision = revision;
}
function estimatePlayMs(){ return currentPlayClockMs(); }
function updateLocalClockUi(){
  if(!lastStatus) return;
  const totalMs = Math.max(0, Number(lastStatus.total_ms) || playClockTotalMs || 0);
  const optimisticActive = seekOptimisticMs !== null && Date.now() < seekOptimisticUntil;
  const displayMs = seekDragging || optimisticActive
    ? Number(seekOptimisticMs || 0)
    : currentPlayClockMs();
  const timeNode = document.getElementById('time');
  if(timeNode) timeNode.textContent = `${fmt(displayMs)} / ${fmt(totalMs)}`;
  const seekSlider = document.getElementById('seekSlider');
  if(seekSlider && !seekDragging){
    seekSlider.value = String(Math.max(0, Math.min(totalMs, displayMs)));
  }
}
function formatBatteryVoltage(mv){
  const value = Number(mv) || 0;
  return value > 0 ? `${(value / 1000).toFixed(2)}V` : '-';
}
function formatBatteryCurrent(ma){
  const value = Number(ma);
  if(!Number.isFinite(value)) return '-';
  return `${value > 0 ? '+' : ''}${Math.round(value)}mA`;
}
function formatBatteryRuntime(b){
  if(!b) return '-';
  const minutes = Math.max(0, Number(b.runtime_minutes) || 0);
  if(b.runtime_ready && minutes > 0){
    if(minutes >= 24 * 60) return '超过24小时';
    if(minutes < 60) return `约${minutes}分钟`;
    return `约${Math.floor(minutes / 60)}时${minutes % 60}分`;
  }
  return b.runtime_label || '-';
}
function batteryPowerStateLabel(b){
  if(!b || !b.valid) return '电量计未就绪';
  if(b.power_state === 'charging') return '充电中';
  if(b.power_state === 'external_power') return '外接电源';
  return '电池供电';
}
function renderBatterySummary(payload){
  const b = payload && payload.battery ? payload.battery : null;
  const main = document.getElementById('batterySummaryMain');
  const state = document.getElementById('batterySummaryState');
  const fill = document.getElementById('batterySummaryFill');
  if(!b || !b.valid){
    main.textContent = '电量未知';
    state.textContent = b && b.runtime_label ? b.runtime_label : '读取失败';
    fill.style.width = '0%';
    document.getElementById('batterySummaryVoltage').textContent = '-';
    document.getElementById('batterySummaryCurrent').textContent = '-';
    document.getElementById('batterySummaryRuntime').textContent = '-';
    document.getElementById('batterySummaryCapacity').textContent = '-';
    return;
  }
  const percent = Math.max(0, Math.min(100, Number(b.percent) || 0));
  main.textContent = `${percent}%`;
  state.textContent = batteryPowerStateLabel(b);
  fill.style.width = `${percent}%`;
  document.getElementById('batterySummaryVoltage').textContent = formatBatteryVoltage(b.voltage_mv);
  document.getElementById('batterySummaryCurrent').textContent = formatBatteryCurrent(b.current_ma);
  document.getElementById('batterySummaryRuntime').textContent = formatBatteryRuntime(b);
  const remaining = Number(b.remaining_capacity_mah) || 0;
  const total = Number(b.full_charge_capacity_mah) || Number(b.design_capacity_mah) || 0;
  document.getElementById('batterySummaryCapacity').textContent = total > 0
    ? `${remaining}/${total}mAh`
    : (remaining > 0 ? `${remaining}mAh` : '-');
}
async function fetchBatterySummary(){
  if(batterySummaryInFlight || document.hidden) return;
  batterySummaryInFlight = true;
  try{
    const r = await fetch('/api/system/diagnostics?scope=battery', {cache:'no-store'});
    const j = await r.json();
    if(j && j.ok) renderBatterySummary(j);
  }catch(e){}
  finally{ batterySummaryInFlight = false; }
}

function startLocalClock(){
  if(localClockTimer) clearInterval(localClockTimer);
  localClockTimer = setInterval(updateLocalClockUi, LOCAL_CLOCK_TICK_MS);
  updateLocalClockUi();
}
function stopLocalClock(){
  if(localClockTimer){ clearInterval(localClockTimer); localClockTimer = null; }
}
function abortStatusFetch(){
  if(statusController){
    try{ statusController.abort(); }catch(e){}
    statusController = null;
  }
  inFlight = false;
  statusFetchStartedAt = 0;
}
function clearLyricTimer(){ if(lyricTimer){ clearTimeout(lyricTimer); lyricTimer=null; } }
function updateLyricsFromState(j){
  const currentNode = document.getElementById('lyricCurrent');
  const nextNode = document.getElementById('lyricNext');

  if(j.show_next_lyric===false){
    nextNode.style.display='none';
  }else{
    nextNode.style.display='block';
  }

  if(j.lyrics_loading){
    currentNode.textContent = '歌词加载中...';
    if(j.show_next_lyric!==false){
      nextNode.textContent = '请稍候';
    }
    return;
  }

  currentNode.textContent = j.has_lyrics ? (j.current_lyric || '...') : '当前曲目暂无歌词';

  if(j.show_next_lyric===false) return;
  nextNode.textContent = j.has_lyrics
    ? ((j.next_lyric && j.next_lyric.length) ? `下一句：${j.next_lyric}` : '下一句：-')
    : '-';
}
function scheduleLyricTransition(j){
  clearLyricTimer();
  if(!j || !j.has_lyrics || !j.is_playing || j.is_paused || j.rescanning) return;
  const nextStart = Number(j.next_lyric_start_ms) || 0;
  if(nextStart <= 0 || !j.next_lyric || !j.next_lyric.length) return;
  const msToLyric = nextStart - estimatePlayMs();
  if(msToLyric <= 0) return;
  const msToPoll = Math.max(0, nextPollAt - Date.now());
  if(msToPoll >= msToLyric && (msToPoll - msToLyric) <= LYRIC_WAIT_POLL_THRESHOLD_MS){ return; }
  lyricTimer = setTimeout(() => {
    if(!lastStatus || !lastStatus.has_lyrics) return;
    lastStatus.current_lyric = lastStatus.next_lyric || lastStatus.current_lyric;
    lastStatus.current_lyric_start_ms = lastStatus.next_lyric_start_ms || lastStatus.current_lyric_start_ms;
    lastStatus.next_lyric = lastStatus.following_lyric || '';
    lastStatus.next_lyric_start_ms = lastStatus.following_lyric_start_ms || 0;
    lastStatus.following_lyric = '';
    lastStatus.following_lyric_start_ms = 0;
    updateLyricsFromState(lastStatus);
    scheduleLyricTransition(lastStatus);
  }, Math.max(1, msToLyric));
}
async function fetchStatus(){
  if(!pageActive) return;
  if(inFlight){
    if(statusFetchStartedAt && Date.now() - statusFetchStartedAt > 10000){
      abortStatusFetch();
    }else{
      scheduleNext(500);
      return;
    }
  }

  inFlight = true;
  statusFetchStartedAt = Date.now();
  const controller = new AbortController();
  statusController = controller;
  const timeoutId = setTimeout(() => controller.abort(), 8000);

  try{
    const r = await fetch(`/api/status?t=${Date.now()}`, {cache:'no-store', signal:controller.signal});
    const j = await r.json();
    lastStatusAt = Date.now();
    lastFullStatusAt = lastStatusAt;
    lastStateToken = Number(j.state_token) || 0;
    syncPlayClock(j, false);
    lastStatus = j;
    syncVolumeLockFromStatus(j);
    applyNetTrackMode(j);
    render(j);
    currentPollMs = Math.max(MIN_STATUS_CHECK_MS, Number(j.next_poll_ms) || POLL_MS);
    if(Number(j.refresh_poll_ms) > 0) POLL_MS = Number(j.refresh_poll_ms);
    if(Number(j.lyric_wait_poll_threshold_ms) > 0) LYRIC_WAIT_POLL_THRESHOLD_MS = Number(j.lyric_wait_poll_threshold_ms);
    const lyricThreshold = (typeof j.lyric_wait_poll_threshold_ms !== 'undefined' && Number(j.lyric_wait_poll_threshold_ms) > 0) ? Number(j.lyric_wait_poll_threshold_ms) : LYRIC_WAIT_POLL_THRESHOLD_MS;
    scheduleLyricTransition(j);
    scheduleNext(currentPollMs);
  }catch(e){
    document.getElementById('net').textContent = e.name === 'AbortError' ? '网页请求超时，正在重试' : '网页状态获取失败';
    scheduleNext(Math.max(POLL_MS, 3000));
  }finally{
    clearTimeout(timeoutId);
    if(statusController === controller) statusController = null;
    inFlight = false;
    statusFetchStartedAt = 0;
  }
}

async function fetchStatusCheck(){
  if(!pageActive || !lastStatus){
    await fetchStatus();
    return;
  }
  if(inFlight){
    scheduleNext(500);
    return;
  }

  inFlight = true;
  statusFetchStartedAt = Date.now();
  const controller = new AbortController();
  statusController = controller;
  const timeoutId = setTimeout(() => controller.abort(), 5000);
  let needFull = false;

  try{
    const r = await fetch(`/api/status/check?token=${encodeURIComponent(lastStateToken)}&t=${Date.now()}`, {cache:'no-store', signal:controller.signal});
    const j = await r.json();
    lastStatusAt = Date.now();
    syncPlayClock(j, false);
    if(lastStatus){
      lastStatus.play_ms = Number(j.play_ms) || 0;
      lastStatus.total_ms = Math.max(0, Number(j.total_ms) || 0);
      lastStatus.is_playing = !!j.is_playing;
      lastStatus.is_paused = !!j.is_paused;
      lastStatus.rescanning = !!j.rescanning;
      lastStatus.seeking = !!j.seeking;
      lastStatus.seek_target_ms = Number(j.seek_target_ms) || 0;
      lastStatus.playback_revision = Number(j.playback_revision) || 0;
    }
    updateLocalClockUi();
    currentPollMs = Math.max(MIN_STATUS_CHECK_MS, Number(j.next_check_ms) || currentPollMs || POLL_MS);
    needFull = !!j.changed || (Date.now() - lastFullStatusAt >= FORCE_FULL_STATUS_MS);
    if(!needFull){
      lastStateToken = Number(j.state_token) || lastStateToken;
      scheduleNext(currentPollMs);
    }
  }catch(e){
    scheduleNext(Math.max(POLL_MS, 2500));
  }finally{
    clearTimeout(timeoutId);
    if(statusController === controller) statusController = null;
    inFlight = false;
    statusFetchStartedAt = 0;
  }

  if(needFull) await fetchStatus();
}

async function pollStatus(){
  if(!lastStatus || Date.now() - lastFullStatusAt >= FORCE_FULL_STATUS_MS){
    await fetchStatus();
  }else{
    await fetchStatusCheck();
  }
}

function pausePagePolling(){
  pageActive = false;
  pagePausedByVisibility = true;

  if(pollTimer){
    clearTimeout(pollTimer);
    pollTimer = null;
  }

  clearLyricTimer();
  stopLocalClock();
  abortStatusFetch();
}

function resumePagePolling(){
  pageActive = true;
  startLocalClock();
  fetchBatterySummary();

  if(pagePausedByVisibility){
    pagePausedByVisibility = false;
    scheduleNext(80);
  }
}

function handleVisibilityChange(){
  if(document.hidden){
    pausePagePolling();
  }else{
    resumePagePolling();
  }
}
function updateCover(j){
  const media=document.getElementById('mediaBox');
  const box=document.getElementById('coverBox');
  const img=document.getElementById('coverImg');
  const fallback=document.getElementById('coverFallback');

  const track=Number.isInteger(j.track_idx)?j.track_idx:-1;
  const rotateView=(j.view==='rotate');
  const coverPanelView=(j.view==='cover_panel');
  const allowCover = j.show_cover !== false;
  // 网页封面旋转由独立设置控制，不再依赖设备当前显示视图。
  const allowSpin = j.web_cover_spin !== false;
  const spinActive = allowSpin && !!j.is_playing && !j.is_paused && !j.rescanning;
  const base = j.cover_url && j.cover_url.length ? j.cover_url : '';

  // 封面区域保持原有布局，只是控制图片显示
  media.classList.toggle('noCover', !allowCover);
  if(!allowCover){ 
    // 不显示封面图片，但保持封面区域。
    box.classList.remove('rotate','spin','coverPanel','coverReady');
    img.style.display='none';
    img.removeAttribute('src');
    fallback.style.display='block';
    fallback.innerHTML='网页端封面<br>显示已关闭';
    return; 
  }

  // 普通视图开启旋转时也使用圆形唱片样式；封面面板自身已有圆形图片。
  box.classList.toggle('rotate', rotateView || (allowSpin && !coverPanelView));
  box.classList.toggle('coverPanel', coverPanelView);
  box.classList.toggle('spin', spinActive);

  const coverLoading = !!j.cover_loading;

  if(!j.has_cover || !base){ 
    lastCoverTrack = '';
    box.classList.remove('coverReady');
    img.style.display='none';
    img.removeAttribute('src');
    fallback.style.display='block';
    fallback.textContent = coverLoading ? '封面加载中...' : '无封面';
    return;
  }

  const coverRev = j.cover_rev && j.cover_rev.length ? j.cover_rev : '';
  const coverKey = (j.source_type==='radio')
    ? `radio:${j.radio_idx||-1}:${coverRev}:${base}`
    : `track:${track}:${coverRev}:${base}`;

  if(coverKey !== lastCoverTrack){ 
    lastCoverTrack = coverKey;

    box.classList.remove('coverReady');
    img.style.display = 'none';
    fallback.style.display = 'block';
    fallback.textContent = '封面加载中...';

    img.onerror = () => { 
      box.classList.remove('coverReady');
      img.style.display = 'none';
      fallback.style.display = 'block';
      fallback.textContent = '封面读取失败';
    };

    img.onload = () => {
      box.classList.add('coverReady');
      fallback.style.display = 'none';
      img.style.display = 'block';
    };

    img.src = base;
    return;
  }

  if(img.complete && img.naturalWidth > 0){
    box.classList.add('coverReady');
    fallback.style.display = 'none';
    img.style.display = 'block';
  }else{
    box.classList.remove('coverReady');
    fallback.style.display = 'block';
    fallback.textContent = '封面加载中...';
    img.style.display = 'none';
  }
}
async function toggleViewFromCover(){
  if(pageLocked || coverToggleBusy) return;
  coverToggleBusy=true;
  try{ await fetch('/api/view/toggle',{method:'POST'});}catch(e){}
  scheduleNext(500);
  setTimeout(()=>{coverToggleBusy=false;},250);
}

function applyNetTrackMode(j){
  const isNetTrack = j && j.source_type === 'net_track';

  document.body.classList.toggle('nettrack-mode', isNetTrack);

  const mainCard = document.getElementById('mainControlCard');
  if(mainCard){
    mainCard.style.display = isNetTrack ? 'none' : '';
  }

  const netCard = document.getElementById('netTrackControlCard');
  if(netCard){
    netCard.style.display = isNetTrack ? 'block' : 'none';
  }

  const modeBtn = document.getElementById('netTrackModeBtn');
  if(modeBtn){
    const mode = j && j.mode ? j.mode : '';
    modeBtn.textContent = mode.indexOf('rnd') >= 0 ? '随机播放中' : '顺序播放中';
  }
}

async function nasControl(path){
  if(pageLocked) return;

  try{
    const r = await fetch(path, {method:'POST'});
    const j = await r.json();

    if(!j || !j.ok){
      alert((j && (j.message || j.error)) || '操作失败');
    }
  }catch(e){
    alert('NAS 控制失败');
  }

  scheduleNext(300);
}

function render(j){
  document.getElementById('title').textContent=j.title||'(无曲目)';
  document.getElementById('artist').textContent=j.artist||'-';
  document.getElementById('album').textContent=j.album||'-';
  document.getElementById('mode').textContent=j.mode_label||j.mode||'-';
  document.getElementById('volume').textContent=`${j.volume ?? 0}%`;
  document.getElementById('displayPos').textContent=(j.display_pos >=0 && j.display_total>0)?`${j.display_pos+1} / ${j.display_total}`:'-';
  document.getElementById('appState').textContent=`${j.app_state_label||j.app_state||'-'} · ${j.view_label||j.view||'-'}`;
  const seekSlider=document.getElementById('seekSlider');
  const totalMs=Math.max(0,Number(j.total_ms)||0);
  if(j.seek_result==='ok' && seekOptimisticMs!==null &&
     Math.abs((Number(j.play_ms)||0)-seekOptimisticMs)<3000){
    seekOptimisticMs=null;
    seekOptimisticUntil=0;
  }
  const optimisticActive=seekOptimisticMs!==null && Date.now()<seekOptimisticUntil;
  const displayPlayMs=seekDragging || optimisticActive
    ? Number(seekOptimisticMs||0)
    : currentPlayClockMs();
  document.getElementById('time').textContent=`${fmt(displayPlayMs)} / ${fmt(totalMs)}`;
  document.getElementById('playState').textContent=j.rescanning
    ? (j.can_cancel_scan ? '扫描中（可取消）' : '扫描中（取消中）')
    : (j.seeking ? '跳转中...' : (j.is_paused ? '已暂停' : (j.is_playing ? '播放中' : '已停止')));
  if(seekSlider){
    seekSlider.max=String(totalMs);
    if(!seekDragging){ seekSlider.value=String(Math.max(0,Math.min(totalMs,displayPlayMs))); }
    const isRadio=(j.source_type==='radio');
    seekSlider.disabled=pageLocked || j.rescanning || j.seeking || isRadio || !j.seekable || totalMs<=0;
    seekSlider.title=seekSlider.disabled ? '当前音源不可拖动进度' : '拖动后松开以跳转';
  }
  updateLyricsFromState(j);
  const slider=document.getElementById('volumeSlider'); if(document.activeElement !== slider){ slider.value=Number(j.volume ?? 0); }

  const isRadio = (j.source_type === 'radio');

  const prevBtn = document.getElementById('prevBtn');
  const nextBtn = document.getElementById('nextBtn');
  const modeRow = document.getElementById('modeRow');

  prevBtn.textContent = isRadio ? '上一电台' : '上一首';
  nextBtn.textContent = isRadio ? '下一电台' : '下一首';

  modeRow.style.display = isRadio ? 'none' : 'grid';

  const radioBackRow = document.getElementById('radioBackRow');
  if (radioBackRow) {
    radioBackRow.style.display = isRadio ? 'grid' : 'none';
  }

  updateCover(j);
}
async function handlePrev(){
  if(pageLocked) return;
  await sendCmd('/api/prev');
}

async function handleNext(){
  if(pageLocked) return;
  await sendCmd('/api/next');
}

async function sendCmd(path){
  if(pageLocked) return;
  try{ await fetch(path,{method:'POST'});}catch(e){}
  scheduleNext(500);
}
async function returnFromRadio(){
  if(pageLocked) return;
  try{
    const r = await fetch('/api/radio/stop', {method:'POST'});
    const j = await r.json();
    alert(j && j.ok ? (j.message || '已返回本地播放') : ((j && j.message) ? j.message : '操作失败'));
  }catch(e){
    alert('操作失败');
  }
  scheduleNext(500);
}
async function savePlayerState(){
  if(pageLocked) return;
  try{
    const r = await fetch('/api/state/save', {method:'POST'});
    const j = await r.json();
    alert(j && j.ok ? '当前状态已保存到 NVS' : ((j && j.message) ? j.message : '保存失败'));
  }catch(e){
    alert('保存失败');
  }
  scheduleNext(500);
}
async function submitSeek(targetMs){
  if(pageLocked || !lastStatus || !lastStatus.seekable || lastStatus.source_type==='radio') return;
  const total=Math.max(0,Number(lastStatus.total_ms)||0);
  if(total<=0) return;
  const target=Math.max(0,Math.min(total,Math.round(Number(targetMs)||0)));
  seekOptimisticMs=target;
  seekOptimisticUntil=Date.now()+4000;
  try{
    const r=await fetch('/api/seek',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:`ms=${encodeURIComponent(target)}`
    });
    const j=await r.json();
    if(!j || !j.ok){
      seekOptimisticMs=null;
      seekOptimisticUntil=0;
      alert((j && (j.message || j.error)) || '进度跳转失败');
    }
  }catch(e){
    seekOptimisticMs=null;
    seekOptimisticUntil=0;
    alert('进度跳转请求失败');
  }
  scheduleNext(250);
}

function sendVolumeDebounced(v){
  if(pageLocked || volumeLocked) return;
  if(volumeTimer) clearTimeout(volumeTimer);
  volumeTimer=setTimeout(async()=>{
    try{
      await fetch('/api/volume',{
        method:'POST',
        headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
        body:`value=${encodeURIComponent(v)}`
      });
    }catch(e){}
    scheduleNext(500);
  },80);
}
const seekSlider=document.getElementById('seekSlider');
seekSlider.addEventListener('pointerdown',()=>{ if(!seekSlider.disabled) seekDragging=true; });
seekSlider.addEventListener('input',(e)=>{
  if(e.target.disabled) return;
  seekDragging=true;
  seekOptimisticMs=Number(e.target.value||0);
  seekOptimisticUntil=Date.now()+4000;
  const total=lastStatus ? Number(lastStatus.total_ms||0) : Number(e.target.max||0);
  document.getElementById('time').textContent=`${fmt(seekOptimisticMs)} / ${fmt(total)}`;
});
seekSlider.addEventListener('change',async(e)=>{
  if(e.target.disabled) return;
  const target=Number(e.target.value||0);
  seekDragging=false;
  await submitSeek(target);
});
seekSlider.addEventListener('pointercancel',()=>{ seekDragging=false; });

const slider=document.getElementById('volumeSlider');
slider.addEventListener('input',(e)=>{
  if(pageLocked || volumeLocked){
    if(lastStatus && lastStatus.volume !== undefined){
      e.target.value = Number(lastStatus.volume || 0);
      document.getElementById('volume').textContent = `${lastStatus.volume ?? 0}%`;
    }
    return;
  }

  const v = Number(e.target.value || 0);
  document.getElementById('volume').textContent = `${v}%`;
  sendVolumeDebounced(v);
});

slider.addEventListener('change',(e)=>{
  if(pageLocked || volumeLocked){
    if(lastStatus && lastStatus.volume !== undefined){
      e.target.value = Number(lastStatus.volume || 0);
    }
    return;
  }

  const v = Number(e.target.value || 0);
  sendVolumeDebounced(v);
});
// 捕获阶段先于按钮反馈脚本和 onclick 执行。锁定时不缩放、不振动、不发请求，只显示提示。
document.addEventListener('pointerdown', interceptLockedControl, true);
document.addEventListener('click', interceptLockedControl, true);
document.addEventListener('keydown', interceptLockedControl, true);

document.getElementById('coverBox').addEventListener('click',toggleViewFromCover);
document.getElementById('lockBtn').addEventListener('click', togglePageLock);
document.getElementById('volumeLockBtn').addEventListener('click', toggleVolumeLock);

document.addEventListener('visibilitychange', handleVisibilityChange);
window.addEventListener('pageshow', ()=>{ if(!document.hidden) resumePagePolling(); });
window.addEventListener('pagehide', pausePagePolling);

loadLockState();
applyLockState();
applyVolumeLockState();

const netCardInit = document.getElementById('netTrackControlCard');
if(netCardInit){
  netCardInit.style.display = 'none';
}

startLocalClock();
setTimeout(fetchStatus, 200 + Math.floor(Math.random() * 900));
setTimeout(fetchBatterySummary, 700);
setInterval(fetchBatterySummary, BATTERY_SUMMARY_POLL_MS);
</script>
</body>
</html>
)HTML";

static const char WEBCTRL_SETTINGS_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <script src="/web-feedback.js" defer></script>
  <title>网页设置</title>
  <style>
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    details.card{padding:0;overflow:hidden;border:1px solid transparent;transition:border-color .16s ease,box-shadow .16s ease}
    details.card>summary{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px;cursor:pointer;font-size:20px;font-weight:800;list-style:none;user-select:none}
    details.card>summary::-webkit-details-marker{display:none}
    details.card>summary::after{content:'›';font-size:28px;line-height:1;color:#aaa;transform:rotate(0deg);transition:transform .16s ease}
    details.card>summary:hover{background:rgba(47,111,235,.08)}
    details.card>summary:focus-visible{outline:2px solid #58a6ff;outline-offset:-3px}
    details.card[open]{border-color:#315f97;box-shadow:0 0 0 1px rgba(47,111,235,.24),0 4px 18px rgba(0,0,0,.25)}
    details.card[open]>summary{border-bottom:1px solid #315f97;background:linear-gradient(90deg,rgba(47,111,235,.22),rgba(47,111,235,.07));color:#cfe6ff}
    details.card[open]>summary::after{transform:rotate(90deg);color:#79c0ff}
    .setting-body{padding:16px}
    .row{display:grid;grid-template-columns:1fr auto;gap:12px;align-items:center;margin-bottom:12px}
    .row.toggle-row{grid-template-columns:minmax(0,1fr) auto}
    .row.toggle-row>label{min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
    .row.toggle-row>input[type=checkbox],.row.toggle-row>input[type=radio]{justify-self:end;margin:0 4px 0 0;flex:0 0 auto}
    label{font-size:15px}
    input[type=number],input[type=text],input[type=password],select{width:180px;padding:10px;border-radius:10px;border:1px solid #444;background:#111;color:#eee;box-sizing:border-box}
    input[type=checkbox],input[type=radio]{accent-color:#2f6feb}
    input[type=checkbox]{transform:scale(1.2)}
    input[type=range]{width:180px;accent-color:#2f6feb}
    details.card[hidden]{display:none}
    .status-value{font-weight:700;text-align:right;word-break:break-word}
    .range-box{display:flex;align-items:center;gap:10px}
    .range-value{min-width:42px;text-align:right;font-variant-numeric:tabular-nums}
    button,a{border:none;border-radius:12px;padding:12px 14px;background:#2f6feb;color:#fff;font-size:15px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    a.secondary,button.secondary{background:#444}
    button.warn{background:#9a6700}
    button.danger{background:#b42318}
    .actions{display:flex;gap:10px;flex-wrap:wrap}
    .time-fields{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:12px 0}
    .time-fields label{font-size:14px;color:#ccc}
    .time-fields input{width:72px;padding:10px;border-radius:10px;border:1px solid #444;background:#111;color:#eee}
    .weekday-fields{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin:8px 0 12px}
    .weekday-fields label{font-size:14px;color:#ddd;display:inline-flex;gap:6px;align-items:center}
    .muted{color:#aaa;font-size:14px}
    .diag-group{margin-top:16px;padding-top:4px;border-top:1px solid #303030}
    .diag-group:first-child{margin-top:0;border-top:none}
    .diag-title{font-size:13px;font-weight:800;color:#79c0ff;letter-spacing:.04em;margin:0 0 10px}
    .diag-value{font-weight:700;text-align:right;word-break:break-word;font-variant-numeric:tabular-nums}
    .config-list-heading{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:14px 2px 8px;font-weight:800}
    .config-list-count{font-size:13px;font-weight:700;color:#9aa4b2}
    .config-item-main{display:flex;align-items:center;gap:8px;min-width:0}
    .config-item-title{font-weight:800;white-space:nowrap}
    .config-item-name{color:#ddd;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
    .config-item-meta{margin-left:auto;font-size:12px;font-weight:700;color:#9aa4b2;white-space:nowrap}
    .config-item-actions-row{display:flex;justify-content:flex-end;gap:6px;flex-wrap:wrap;margin-bottom:12px}
    .config-item-actions-row button{padding:7px 10px;font-size:13px}
    .wifi-list{display:flex;flex-direction:column;gap:10px;margin:0 0 14px}
    .wifi-item{border:1px solid #343434;border-radius:14px;background:#151515;overflow:hidden;transition:border-color .16s ease,box-shadow .16s ease,background .16s ease}
    .wifi-item>summary,.radio-item>summary,.nas-item>summary,.asset-item>summary{display:flex;align-items:center;gap:10px;padding:12px;cursor:pointer;list-style:none;user-select:none;transition:background .16s ease,color .16s ease}
    .wifi-item>summary::-webkit-details-marker,.radio-item>summary::-webkit-details-marker,.nas-item>summary::-webkit-details-marker,.asset-item>summary::-webkit-details-marker{display:none}
    .wifi-item>summary::after,.radio-item>summary::after,.nas-item>summary::after,.asset-item>summary::after{content:'›';font-size:24px;line-height:1;color:#aaa;transform:rotate(0deg);transition:transform .16s ease,color .16s ease}
    .wifi-item>summary:hover,.radio-item>summary:hover,.nas-item>summary:hover,.asset-item>summary:hover{background:rgba(47,111,235,.08)}
    .wifi-item>summary:focus-visible,.radio-item>summary:focus-visible,.nas-item>summary:focus-visible,.asset-item>summary:focus-visible{outline:2px solid #58a6ff;outline-offset:-3px}
    .wifi-item[open],.radio-item[open],.nas-item[open],.asset-item[open]{border-color:#2f6feb;box-shadow:0 0 0 1px rgba(47,111,235,.22)}
    .wifi-item[open]>summary,.radio-item[open]>summary,.nas-item[open]>summary,.asset-item[open]>summary{border-bottom:1px solid #315f97;background:rgba(47,111,235,.17);color:#cfe6ff}
    .wifi-item[open]>summary .config-item-name,.radio-item[open]>summary .config-item-name,.nas-item[open]>summary .config-item-name,.asset-item[open]>summary .config-item-name{color:#cfe6ff}
    .wifi-item[open]>summary::after,.radio-item[open]>summary::after,.nas-item[open]>summary::after,.asset-item[open]>summary::after{transform:rotate(90deg);color:#79c0ff}
    .wifi-item.is-current:not([open]),.nas-item.is-current:not([open]){border-color:#2f7d50;background:#14221a}
    .wifi-item-body,.radio-item-body,.nas-item-body{padding:12px}
    .wifi-item-title{display:flex;align-items:center;gap:8px;font-weight:800}
    .wifi-item-actions{display:flex;gap:6px;flex-wrap:wrap}
    .wifi-item-actions button{padding:7px 10px;font-size:13px}
    .wifi-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px 12px}
    .wifi-field{display:flex;flex-direction:column;gap:6px}
    .wifi-field label{font-size:13px;color:#bbb}
    .wifi-field input{width:100%}
    .wifi-checks{display:flex;gap:16px;flex-wrap:wrap;align-items:center;margin-top:12px}
    .wifi-checks label{display:inline-flex;gap:7px;align-items:center;color:#ddd}
    .wifi-badge{display:inline-flex;padding:3px 8px;border-radius:999px;background:#175f35;color:#b7f7ce;font-size:12px}
    .wifi-empty{border:1px dashed #444;border-radius:12px;padding:18px;text-align:center;color:#999}
    .radio-list{display:flex;flex-direction:column;gap:10px;margin:0 0 14px}
    .radio-item{border:1px solid #343434;border-radius:14px;background:#151515;overflow:hidden;transition:border-color .16s ease,box-shadow .16s ease,background .16s ease}
    .radio-item-title{font-weight:800}
    .radio-item-actions{display:flex;gap:6px;flex-wrap:wrap}
    .radio-item-actions button{padding:7px 10px;font-size:13px}
    .radio-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px 12px}
    .radio-field{display:flex;flex-direction:column;gap:6px}
    .radio-field label{font-size:13px;color:#bbb}
    .radio-field input{width:100%}
    .radio-url-field{grid-column:1 / -1}
    .radio-upload{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:10px}
    .radio-upload input[type=file]{max-width:100%;color:#ccc}
    .radio-empty{border:1px dashed #444;border-radius:12px;padding:18px;text-align:center;color:#999}
    .nas-list{display:flex;flex-direction:column;gap:10px;margin:0 0 14px}
    .nas-item{border:1px solid #343434;border-radius:14px;background:#151515;overflow:hidden;transition:border-color .16s ease,box-shadow .16s ease,background .16s ease}
    .nas-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px 12px}
    .nas-field{display:flex;flex-direction:column;gap:6px}
    .nas-field label{font-size:13px;color:#bbb}
    .nas-field input{width:100%}
    .nas-path-field{grid-column:1 / -1}
    .nas-active-row{display:flex;align-items:center;gap:8px;margin-top:12px;color:#ddd}
    .nas-active-badge{display:inline-flex;padding:3px 8px;border-radius:999px;background:#1f4e78;color:#c7e5ff;font-size:12px}
    .nas-empty{border:1px dashed #444;border-radius:12px;padding:18px;text-align:center;color:#999}
    .asset-list{display:flex;flex-direction:column;gap:10px}
    .asset-item{border:1px solid #343434;border-radius:14px;background:#151515;overflow:hidden;transition:border-color .16s ease,box-shadow .16s ease,background .16s ease}
    .asset-item-body{padding:14px}
    .asset-status{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:12px;color:#bbb;font-size:13px}
    .asset-upload{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
    .asset-upload input[type=file]{max-width:100%;color:#ccc}
    .config-center-list{display:flex;flex-direction:column;gap:10px}
    details.config-section{border:1px solid #343434;border-radius:14px;background:#151515;overflow:hidden;transition:border-color .16s ease,box-shadow .16s ease}
    details.config-section>summary{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px;cursor:pointer;font-size:17px;font-weight:800;list-style:none;user-select:none}
    details.config-section>summary::-webkit-details-marker{display:none}
    details.config-section>summary::after{content:'›';font-size:24px;line-height:1;color:#aaa;transform:rotate(0deg);transition:transform .16s ease}
    details.config-section>summary:hover{background:rgba(47,111,235,.08)}
    details.config-section>summary:focus-visible{outline:2px solid #58a6ff;outline-offset:-3px}
    details.config-section[open]{border-color:#2f6feb;box-shadow:0 0 0 1px rgba(47,111,235,.2)}
    details.config-section[open]>summary{border-bottom:1px solid #315f97;background:rgba(47,111,235,.14);color:#b9dcff}
    details.config-section[open]>summary::after{transform:rotate(90deg);color:#79c0ff}
    details.config-section>.setting-body{padding:14px}
    @media(max-width:560px){.config-item-meta{display:none}.wifi-grid,.radio-grid,.nas-grid{grid-template-columns:1fr}.radio-url-field,.nas-path-field{grid-column:auto}.row{grid-template-columns:1fr}.row.toggle-row{grid-template-columns:minmax(0,1fr) auto}.row>.status-value,.row>.diag-value,.row>div{text-align:left}.row input[type=number],.row input[type=text],.row input[type=password],.row select{width:100%}}
    @media(max-width:560px){
      #systemDiagnosticsCard .diag-group .row{grid-template-columns:minmax(108px,42%) minmax(0,1fr);gap:10px;align-items:start}
      #systemDiagnosticsCard .diag-group .row>label{color:#79c0ff;font-weight:700;line-height:1.45}
      #systemDiagnosticsCard .diag-group .row>.diag-value{text-align:right;min-width:0;overflow-wrap:anywhere;line-height:1.45}
    }
    @media(max-width:340px){
      #systemDiagnosticsCard .diag-group .row{grid-template-columns:1fr;gap:4px}
      #systemDiagnosticsCard .diag-group .row>.diag-value{text-align:left}
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <div class="actions">
        <a class="secondary" href="/">返回控制页</a>
      </div>
      <h2>网页设置</h2>
    </div>

    <details class="card" id="tfConfigCenterCard">
      <summary>TF卡配置中心</summary>
      <div class="setting-body">
        <div class="config-center-list">
        <details class="config-section" id="wifiConfigCard">
          <summary>Wi-Fi 网络</summary>
          <div class="setting-body">
            <div class="row"><label>当前连接</label><div class="status-value" id="wifiConnectionText">正在读取...</div></div>
            <div class="row"><label>配置文件</label><div class="status-value" id="wifiConfigPathText">/System/config/wifi.conf</div></div>
            <div class="row"><label for="wifiHostname">设备主机名</label><input id="wifiHostname" type="text" maxlength="63" autocomplete="off" placeholder="esp32s3-player"></div>
            <div class="config-list-heading"><span>已有网络</span><span class="config-list-count" id="wifiNetworkCount">0 个</span></div>
            <div id="wifiNetworkList" class="wifi-list"></div>
            <div class="actions">
              <button class="secondary" id="wifiAddBtn" onclick="addWifiNetwork()">添加网络</button>
              <button class="secondary" id="wifiReloadBtn" onclick="loadWifiConfig(false)">重新读取</button>
            </div>
            <div class="actions" style="margin-top:10px">
              <button id="wifiSaveBtn" onclick="saveWifiConfig(false)">保存配置</button>
              <button class="warn" id="wifiApplyBtn" onclick="saveWifiConfig(true)">保存并重连</button>
            </div>
            <div class="muted" id="wifiConfigMessage" style="margin-top:10px">网页不会读取明文密码；本地播放期间保存会先暂存PSRAM，在下次切歌安全窗口写入TF卡。</div>
          </div>
        </details>


        <details class="config-section" id="radioConfigCard">
          <summary>网络电台配置</summary>
          <div class="setting-body">
            <div class="row"><label>配置文件</label><div class="status-value" id="radioConfigPathText">/System/config/radio_list.txt</div></div>
            <div class="row"><label>台标目录</label><div class="status-value" id="radioAssetDirText">/System/assets/radio</div></div>
            <div class="config-list-heading"><span>已有电台</span><span class="config-list-count" id="radioConfigCount">0 个</span></div>
            <div id="radioConfigList" class="radio-list"></div>
            <div class="actions">
              <button class="secondary" id="radioAddBtn" onclick="addRadioConfigItem()">添加电台</button>
              <button class="secondary" id="radioReloadBtn" onclick="loadRadioConfig(false)">重新读取</button>
              <a class="secondary" href="/radios">打开电台页</a>
            </div>
            <div class="actions" style="margin-top:10px">
              <button id="radioSaveBtn" onclick="saveRadioConfig()">保存电台配置</button>
            </div>
            <div class="muted" id="radioConfigMessage" style="margin-top:10px">地址、顺序和台标都先暂存PSRAM；本地播放时在下次切歌安全窗口写入TF卡。</div>
          </div>
        </details>

        <details class="config-section" id="nasConfigCard">
          <summary>NAS 配置</summary>
          <div class="setting-body">
            <div class="row"><label>根地址文件</label><div class="status-value" id="nasBasePathText">/System/config/net_music_base.txt</div></div>
            <div class="row"><label>曲库源文件</label><div class="status-value" id="nasSourcesPathText">/System/config/net_music_sources.txt</div></div>
            <div class="row"><label for="nasBaseUrl">NAS HTTP 根地址</label><input id="nasBaseUrl" type="text" maxlength="768" autocomplete="off" placeholder="http://192.168.1.105:8080/music/"></div>
            <div class="config-list-heading"><span>NAS 曲库源</span><span class="config-list-count" id="nasSourceCount">0 个</span></div>
            <div id="nasSourceList" class="nas-list"></div>
            <div class="actions">
              <button class="secondary" id="nasAddBtn" onclick="addNasSource()">添加曲库源</button>
              <button class="secondary" id="nasReloadBtn" onclick="loadNasConfig(false)">重新读取</button>
              <a class="secondary" href="/netmusic">打开NAS页</a>
            </div>
            <div class="actions" style="margin-top:10px">
              <button id="nasSaveBtn" onclick="saveNasConfig(false)">保存配置</button>
              <button class="warn" id="nasApplyBtn" onclick="saveNasConfig(true)">保存并应用</button>
            </div>
            <div class="muted" id="nasConfigMessage" style="margin-top:10px">根地址和曲库源先暂存PSRAM；本地播放时在下次切歌安全窗口写入TF卡。保存并应用会停止正在播放的NAS歌曲，但不会自动下载完整列表。</div>
          </div>
        </details>

        <details class="config-section" id="systemAssetCard">
          <summary>系统图片</summary>
          <div class="setting-body">
            <div class="asset-list">
              <details class="asset-item" id="defaultCoverAssetItem">
                <summary><div class="config-item-main"><span class="config-item-title">默认封面</span><span class="config-item-name" id="defaultCoverAssetSummary">正在读取...</span></div><span class="config-item-meta">240×240 方图优先</span></summary>
                <div class="asset-item-body">
                  <div class="asset-status"><span>路径：</span><span id="defaultCoverAssetPath">/System/assets/default_cover.jpg</span><span id="defaultCoverAssetStatus">未读取</span></div>
                  <div class="asset-upload">
                    <input id="defaultCoverAssetFile" type="file" accept="image/jpeg,.jpg,.jpeg">
                    <button id="defaultCoverAssetUploadBtn" onclick="uploadSystemAsset('default_cover')">上传默认封面</button>
                  </div>
                  <div class="muted" style="margin-top:10px">仅支持完整JPEG，最大400KB。上传后在下一次加载默认封面时生效。</div>
                </div>
              </details>

              <details class="asset-item" id="netCoverLoadingAssetItem">
                <summary><div class="config-item-main"><span class="config-item-title">NAS 加载图</span><span class="config-item-name" id="netCoverLoadingAssetSummary">正在读取...</span></div><span class="config-item-meta">NAS MP3 起播占位图</span></summary>
                <div class="asset-item-body">
                  <div class="asset-status"><span>路径：</span><span id="netCoverLoadingAssetPath">/System/assets/net_cover_loading.jpg</span><span id="netCoverLoadingAssetStatus">未读取</span></div>
                  <div class="asset-upload">
                    <input id="netCoverLoadingAssetFile" type="file" accept="image/jpeg,.jpg,.jpeg">
                    <button id="netCoverLoadingAssetUploadBtn" onclick="uploadSystemAsset('net_cover_loading')">上传NAS加载图</button>
                  </div>
                  <div class="muted" style="margin-top:10px">仅支持完整JPEG，最大400KB。上传后在下一次播放NAS MP3时生效；NAS FLAC仍按现有逻辑先显示默认封面。</div>
                </div>
              </details>
            </div>
            <div class="muted" id="systemAssetMessage" style="margin-top:10px">图片先暂存PSRAM；本地播放时在下次切歌安全窗口写入TF卡。</div>
          </div>
        </details>

        </div>
      </div>
    </details>

    <details class="card">
      <summary>时间与闹钟</summary>
      <div class="setting-body">
      <div class="row"><label>当前时间</label><div id="rtcTimeText">-</div></div>
      <div class="row"><label>闹钟状态</label><div id="alarmEnabledText">-</div></div>
      <div class="row"><label>下次触发</label><div id="alarmNextText">-</div></div>
      <div class="actions">
        <button onclick="syncRtcFromBrowser()">校准时间</button>
        <button class="secondary" onclick="refreshClockAlarmStatus()">刷新</button>
      </div>
      <div class="muted" id="rtcStatusText">未读取</div>

      <div class="time-fields">
        <label>闹钟时间</label>
        <input id="alarmHour" type="number" min="0" max="23" step="1" placeholder="时">
        <input id="alarmMinute" type="number" min="0" max="59" step="1" placeholder="分">
        <input id="alarmSecond" type="number" min="0" max="59" step="1" placeholder="秒">
      </div>
      <div class="row"><label>重复</label>
        <select id="alarmRepeat" onchange="updateAlarmRepeatUi()">
          <option value="once">单次</option>
          <option value="daily">每天</option>
          <option value="weekdays">工作日</option>
          <option value="weekends">周末</option>
          <option value="weekly">每周指定</option>
        </select>
      </div>
      <div id="alarmWeekdayBox">
        <div class="weekday-fields">
          <label><input type="checkbox" class="alarmWeekday" value="1">周一</label>
          <label><input type="checkbox" class="alarmWeekday" value="2">周二</label>
          <label><input type="checkbox" class="alarmWeekday" value="3">周三</label>
          <label><input type="checkbox" class="alarmWeekday" value="4">周四</label>
          <label><input type="checkbox" class="alarmWeekday" value="5">周五</label>
          <label><input type="checkbox" class="alarmWeekday" value="6">周六</label>
          <label><input type="checkbox" class="alarmWeekday" value="0">周日</label>
        </div>
      </div>
      <div class="row"><label>动作</label>
        <select id="alarmAction">
          <option value="resume_last">恢复上次播放</option>
          <option value="wake_only">只开机</option>
        </select>
      </div>
      <div class="row"><label>音量</label><input id="alarmVolume" type="number" min="0" max="100" step="1"></div>
      <div class="actions">
        <button onclick="saveAlarm()">保存并启用</button>
        <button class="secondary" onclick="disableAlarm()">关闭</button>
        <button class="danger" onclick="deleteAlarm()">删除</button>
      </div>
      </div>
    </details>

    <details class="card">
      <summary>网页显示</summary>
      <div class="setting-body">
      <div class="row"><label>页面刷新</label>
        <select id="refresh_preset">
          <option value="power">省电</option>
          <option value="balanced">平衡</option>
          <option value="smooth">流畅</option>
        </select>
      </div>
      <div class="row"><label>歌词同步</label>
        <select id="lyric_sync_mode">
          <option value="precise">精准</option>
          <option value="balanced">平衡</option>
          <option value="follow_poll">省流量</option>
        </select>
      </div>
      <div class="row toggle-row"><label>下一句歌词</label><input id="show_next_lyric" type="checkbox"></div>
      <div class="row toggle-row"><label>网页封面</label><input id="show_cover" type="checkbox"></div>
      <div class="row toggle-row"><label>封面旋转</label><input id="web_cover_spin" type="checkbox"></div>
      <div class="row toggle-row"><label>按键振动</label><input id="web_haptic_feedback" data-web-feedback-haptic-toggle type="checkbox"></div>
      <div class="actions">
        <button onclick="saveWebDisplaySettings()">保存显示设置</button>
        <button class="secondary" onclick="loadSettings()">重新读取</button>
      </div>
      </div>
    </details>

    <details class="card">
      <summary>设备设置</summary>
      <div class="setting-body">
      <div class="row"><label>显示类型</label>
        <select id="device_view">
          <option value="info">歌词信息</option>
          <option value="rotate">旋转封面</option>
          <option value="cover_panel">封面面板</option>
        </select>
      </div>
      <div class="row toggle-row"><label>屏幕</label><input id="screen_enabled" type="checkbox"></div>
      <div class="row"><label>睡眠关机</label>
        <select id="sleep_timer_minutes">
          <option value="0">关闭</option>
          <option value="15">15分钟</option>
          <option value="30">30分钟</option>
          <option value="60">60分钟</option>
          <option value="90">90分钟</option>
        </select>
      </div>
      <div class="row toggle-row"><label>霍尔控制</label><input id="hall_control_enabled" type="checkbox" onchange="syncHallSolenoidUi()"></div>
      <div class="row toggle-row"><label>电磁铁</label><input id="solenoid_enabled" type="checkbox" onchange="syncHallSolenoidUi()"></div>
      <div class="row toggle-row"><label>电磁铁方向反转</label><input id="solenoid_direction_inverted" type="checkbox" onchange="syncHallSolenoidUi()"></div>
      <div id="hall_solenoid_hint" class="muted small">电磁铁开启时，霍尔自动联动并负责到位后的播放/暂停。</div>
      <div id="solenoid_direction_hint" class="muted small">反转开启时交换A/B对应的靠近与离开方向。</div>
      <div class="row toggle-row"><label>状态灯</label><input id="status_led_enabled" type="checkbox" onchange="syncStatusLedUi()"></div>
      <div class="row"><label>状态灯亮度</label>
        <select id="status_led_brightness">
          <option value="low">低</option>
          <option value="medium">中</option>
          <option value="high">高</option>
        </select>
      </div>
      <div class="actions">
        <button onclick="saveDeviceSettings()">保存设备设置</button>
        <button class="secondary" onclick="loadSettings()">重新读取</button>
      </div>
      </div>
    </details>

    <details class="card" id="audioOutputCard">
      <summary>音频输出</summary>
      <div class="setting-body">
        <div class="row"><label>输出路径</label>
          <select id="audio_output_route">
            <option value="headphone">耳机</option>
            <option value="speaker">功放</option>
            <option value="bluetooth">蓝牙</option>
          </select>
        </div>
        <div class="row"><label>当前状态</label><div class="status-value" id="audioOutputStatusText">正在读取...</div></div>
        <div class="actions">
          <button id="audioOutputApplyBtn" onclick="applyAudioOutputRoute()">应用路径</button>
          <button class="secondary" onclick="loadAudioOutputStatus(true)">刷新</button>
        </div>
      </div>
    </details>

    <details class="card" id="speakerSettingsCard" hidden>
      <summary>功放设置</summary>
      <div class="setting-body">
        <div class="row"><label>功放状态</label><div class="status-value" id="ampPowerText">-</div></div>
        <div class="row toggle-row"><label>功放静音</label><input id="amp_muted" type="checkbox"></div>
        <div class="actions">
          <button id="ampMuteApplyBtn" onclick="applyAmpMute()">应用静音</button>
        </div>
      </div>
    </details>

    <details class="card" id="bluetoothSettingsCard" hidden>
      <summary>蓝牙设置</summary>
      <div class="setting-body">
        <div class="row"><label>蓝牙电源</label><div class="status-value" id="btPowerText">-</div></div>
        <div class="row"><label>连接状态</label><div class="status-value" id="btLinkText">-</div></div>
        <div class="row"><label>设备名称</label><div class="status-value" id="btDeviceText">-</div></div>
        <div class="row"><label>蓝牙音量</label>
          <div class="range-box">
            <input id="bt_volume" type="range" min="0" max="100" step="1" oninput="syncBtVolumeText()" onchange="applyBtVolume()">
            <span class="range-value" id="btVolumeText">0%</span>
          </div>
        </div>
        <div class="actions">
          <button class="secondary" id="btQueryBtn" onclick="queryBtDevice()">刷新设备</button>
          <button id="btPairBtn" onclick="pairBluetooth()">蓝牙配对</button>
          <button class="danger" id="btRestartBtn" onclick="restartBluetooth()">重启模块</button>
        </div>
      </div>
    </details>

    <details class="card" id="systemDiagnosticsCard" ontoggle="handleSystemDiagnosticsToggle()">
      <summary>系统诊断</summary>
      <div class="setting-body">
        <div class="diag-group">
          <div class="diag-title">电池</div>
          <div class="row"><label>电量</label><div class="diag-value" id="diagBatteryPercent">-</div></div>
          <div class="row"><label>电压 / 电流</label><div class="diag-value" id="diagBatteryElectrical">-</div></div>
          <div class="row"><label>供电状态</label><div class="diag-value" id="diagBatteryPower">-</div></div>
          <div class="row"><label>预计续航</label><div class="diag-value" id="diagBatteryRuntime">-</div></div>
          <div class="row"><label>容量 / 健康度</label><div class="diag-value" id="diagBatteryCapacity">-</div></div>
        </div>
        <div class="diag-group">
          <div class="diag-title">内存</div>
          <div class="row"><label>内部 RAM</label><div class="diag-value" id="diagInternalRam">-</div></div>
          <div class="row"><label>DMA RAM</label><div class="diag-value" id="diagDmaRam">-</div></div>
          <div class="row"><label>PSRAM</label><div class="diag-value" id="diagPsram">-</div></div>
          <div class="row"><label>历史最低堆</label><div class="diag-value" id="diagMinHeap">-</div></div>
        </div>
        <div class="diag-group">
          <div class="diag-title">任务栈余量</div>
          <div class="row"><label>音频 / UI</label><div class="diag-value" id="diagTaskAudioUi">-</div></div>
          <div class="row"><label>资源 / 网络封面</label><div class="diag-value" id="diagTaskAssetCover">-</div></div>
          <div class="row"><label>FLAC预取 / 曲库重扫</label><div class="diag-value" id="diagTaskFlacRescan">-</div></div>
          <div class="row"><label>主循环（含监控）</label><div class="diag-value" id="diagTaskLoop">-</div></div>
        </div>
        <div class="diag-group">
          <div class="diag-title">硬件与系统</div>
          <div class="row"><label>固件 / 运行时间</label><div class="diag-value" id="diagFirmwareUptime">-</div></div>
          <div class="row"><label>I²C / MCP23017</label><div class="diag-value" id="diagI2cMcp">-</div></div>
          <div class="row"><label>BQ27441 / RTC</label><div class="diag-value" id="diagBqRtc">-</div></div>
          <div class="row"><label>网络</label><div class="diag-value" id="diagNetwork">-</div></div>
          <div class="row"><label>音频</label><div class="diag-value" id="diagAudio">-</div></div>
        </div>
        <div class="actions">
          <button class="secondary" onclick="loadSystemDiagnostics(false)">立即刷新</button>
        </div>
        <div class="muted" id="diagUpdatedText">未读取</div>
      </div>
    </details>

    <details class="card">
      <summary>本地曲库维护</summary>
      <div class="setting-body">
        <div class="row"><label>扫描状态</label><div id="scanStatusText">正在读取...</div></div>
        <div class="row"><label>重扫模式</label>
          <select id="scanMode">
            <option value="ultra">超快速目录</option>
            <option value="fast" selected>快速增量</option>
            <option value="strict">严格增量</option>
            <option value="full">强制全量</option>
          </select>
        </div>
        <div class="actions">
          <button class="warn" id="settingsScanBtn" onclick="toggleMusicScan()">开始重扫</button>
        </div>
      </div>
    </details>
  </div>

<script>
async function fetchWithTimeout(url, options={}, timeoutMs=3500){
  const controller = new AbortController();
  const timer = setTimeout(()=>controller.abort(), timeoutMs);
  try{
    return await fetch(url, {...options, signal: controller.signal});
  }finally{
    clearTimeout(timer);
  }
}

let settingsRescanning = false;
let scanStatusBusy = false;
let audioOutputBusy = false;
let btDeviceAutoQueryPending = false;
let lastAudioOutputStatus = null;
let systemDiagnosticsBusy = false;
let lastSystemDiagnosticsAt = 0;
let wifiConfigBusy = false;
let wifiConfigMaxNetworks = 16;
let wifiConfigNetworks = [];
let radioConfigBusy = false;
let radioConfigMaxItems = 64;
let radioConfigItems = [];
let nasConfigBusy = false;
let nasConfigMaxSources = 8;
let nasConfigSources = [];
let nasConfigActiveIndex = 0;

function setWifiConfigBusy(busy){
  wifiConfigBusy = !!busy;
  ['wifiAddBtn','wifiReloadBtn','wifiSaveBtn','wifiApplyBtn'].forEach(id=>{
    const el = document.getElementById(id);
    if(el) el.disabled = wifiConfigBusy;
  });
  document.querySelectorAll('#wifiNetworkList input,#wifiHostname')
    .forEach(el=>{ el.disabled = wifiConfigBusy; });
  document.querySelectorAll('#wifiNetworkList button').forEach(el=>{
    el.disabled = wifiConfigBusy || el.dataset.edgeDisabled === '1';
  });
}

function wifiMakeField(labelText, input){
  const box = document.createElement('div');
  box.className = 'wifi-field';
  const label = document.createElement('label');
  label.textContent = labelText;
  box.appendChild(label);
  box.appendChild(input);
  return box;
}

function wifiMakeInput(type, value, placeholder=''){
  const input = document.createElement('input');
  input.type = type;
  input.value = value == null ? '' : String(value);
  input.placeholder = placeholder;
  input.autocomplete = type === 'password' ? 'new-password' : 'off';
  return input;
}

function syncWifiModelFromDom(){
  document.querySelectorAll('#wifiNetworkList .wifi-item').forEach((item, index)=>{
    const network = wifiConfigNetworks[index];
    if(!network) return;
    network.expanded = item.open;
    network.ssid = item.querySelector('[data-field="ssid"]').value;
    network.password = item.querySelector('[data-field="password"]').value;
    network.channel = item.querySelector('[data-field="channel"]').value;
    network.bssid = item.querySelector('[data-field="bssid"]').value;
    network.hidden = item.querySelector('[data-field="hidden"]').checked;
    const clear = item.querySelector('[data-field="clear_password"]');
    network.clearPassword = !!(clear && clear.checked);
  });
}

function moveWifiNetwork(index, delta){
  if(wifiConfigBusy) return;
  syncWifiModelFromDom();
  const target = index + delta;
  if(target < 0 || target >= wifiConfigNetworks.length) return;
  const temp = wifiConfigNetworks[index];
  wifiConfigNetworks[index] = wifiConfigNetworks[target];
  wifiConfigNetworks[target] = temp;
  renderWifiNetworks();
}

function removeWifiNetwork(index){
  if(wifiConfigBusy) return;
  syncWifiModelFromDom();
  const network = wifiConfigNetworks[index];
  if(network && network.ssid && !confirm(`确认删除网络“${network.ssid}”？`)) return;
  wifiConfigNetworks.splice(index, 1);
  renderWifiNetworks();
}

function addWifiNetwork(){
  if(wifiConfigBusy) return;
  syncWifiModelFromDom();
  if(wifiConfigNetworks.length >= wifiConfigMaxNetworks){
    alert(`最多配置 ${wifiConfigMaxNetworks} 个网络`);
    return;
  }
  wifiConfigNetworks.push({
    sourceIndex:-1,
    ssid:'',
    password:'',
    hasPassword:false,
    clearPassword:false,
    hidden:false,
    channel:0,
    bssid:'',
    active:false,
    expanded:true
  });
  renderWifiNetworks();
  const items = document.querySelectorAll('#wifiNetworkList .wifi-item');
  const last = items[items.length - 1];
  if(last){
    const input = last.querySelector('[data-field="ssid"]');
    if(input) input.focus();
  }
}

function renderWifiNetworks(){
  const list = document.getElementById('wifiNetworkList');
  if(!list) return;
  list.textContent = '';
  const count = document.getElementById('wifiNetworkCount');
  if(count) count.textContent = `${wifiConfigNetworks.length} 个`;

  if(!wifiConfigNetworks.length){
    const empty = document.createElement('div');
    empty.className = 'wifi-empty';
    empty.textContent = '尚未配置网络，请先添加至少一个网络。';
    list.appendChild(empty);
    setWifiConfigBusy(wifiConfigBusy);
    return;
  }

  wifiConfigNetworks.forEach((network, index)=>{
    const item = document.createElement('details');
    item.className = 'wifi-item';
    if(network.active) item.classList.add('is-current');
    item.open = !!network.expanded;
    item.addEventListener('toggle', ()=>{ network.expanded = item.open; });

    const summary = document.createElement('summary');
    const main = document.createElement('div');
    main.className = 'config-item-main';
    const order = document.createElement('span');
    order.className = 'config-item-title';
    order.textContent = `网络 ${index + 1}`;
    const summaryName = document.createElement('span');
    summaryName.className = 'config-item-name';
    summaryName.textContent = network.ssid || '未命名网络';
    main.append(order, summaryName);
    if(network.active){
      const badge = document.createElement('span');
      badge.className = 'wifi-badge';
      badge.textContent = '当前连接';
      main.appendChild(badge);
    }
    const meta = document.createElement('span');
    meta.className = 'config-item-meta';
    meta.textContent = network.hidden
      ? '隐藏网络'
      : ((Number(network.channel) || 0) > 0 ? `信道 ${network.channel}` : '自动信道');
    summary.append(main, meta);
    item.appendChild(summary);

    const body = document.createElement('div');
    body.className = 'wifi-item-body';
    const actions = document.createElement('div');
    actions.className = 'config-item-actions-row';
    const up = document.createElement('button');
    up.type = 'button';
    up.className = 'secondary';
    up.textContent = '上移';
    up.dataset.edgeDisabled = index === 0 ? '1' : '0';
    up.disabled = index === 0;
    up.onclick = ()=>moveWifiNetwork(index, -1);
    const down = document.createElement('button');
    down.type = 'button';
    down.className = 'secondary';
    down.textContent = '下移';
    down.dataset.edgeDisabled = index === wifiConfigNetworks.length - 1 ? '1' : '0';
    down.disabled = index === wifiConfigNetworks.length - 1;
    down.onclick = ()=>moveWifiNetwork(index, 1);
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'danger';
    remove.textContent = '删除';
    remove.onclick = ()=>removeWifiNetwork(index);
    actions.append(up, down, remove);
    body.appendChild(actions);

    const grid = document.createElement('div');
    grid.className = 'wifi-grid';

    const ssid = wifiMakeInput('text', network.ssid, 'Wi-Fi 名称');
    ssid.maxLength = 32;
    ssid.dataset.field = 'ssid';
    ssid.addEventListener('input', ()=>{
      summaryName.textContent = ssid.value.trim() || '未命名网络';
    });
    grid.appendChild(wifiMakeField('SSID', ssid));

    const password = wifiMakeInput(
      'password',
      network.password || '',
      network.hasPassword ? '留空保持原密码' : '开放网络可留空'
    );
    password.maxLength = 64;
    password.dataset.field = 'password';
    grid.appendChild(wifiMakeField('密码', password));

    const channel = wifiMakeInput('number', network.channel || 0, '0=自动');
    channel.min = '0';
    channel.max = '14';
    channel.step = '1';
    channel.dataset.field = 'channel';
    grid.appendChild(wifiMakeField('信道（0为自动）', channel));

    const bssid = wifiMakeInput('text', network.bssid || '', 'AA:BB:CC:DD:EE:FF');
    bssid.maxLength = 17;
    bssid.dataset.field = 'bssid';
    grid.appendChild(wifiMakeField('BSSID（可选）', bssid));
    body.appendChild(grid);

    const checks = document.createElement('div');
    checks.className = 'wifi-checks';
    const hiddenLabel = document.createElement('label');
    const hidden = document.createElement('input');
    hidden.type = 'checkbox';
    hidden.checked = !!network.hidden;
    hidden.dataset.field = 'hidden';
    hidden.addEventListener('change', ()=>{
      meta.textContent = hidden.checked
        ? '隐藏网络'
        : ((Number(channel.value) || 0) > 0 ? `信道 ${channel.value}` : '自动信道');
    });
    channel.addEventListener('input', ()=>{
      if(!hidden.checked){
        meta.textContent = (Number(channel.value) || 0) > 0
          ? `信道 ${channel.value}`
          : '自动信道';
      }
    });
    hiddenLabel.append(hidden, document.createTextNode('隐藏网络'));
    checks.appendChild(hiddenLabel);

    if(network.hasPassword){
      const clearLabel = document.createElement('label');
      const clear = document.createElement('input');
      clear.type = 'checkbox';
      clear.checked = !!network.clearPassword;
      clear.dataset.field = 'clear_password';
      clearLabel.append(clear, document.createTextNode('清除已保存密码'));
      checks.appendChild(clearLabel);
      password.addEventListener('input', ()=>{
        if(password.value) clear.checked = false;
      });
    }

    body.appendChild(checks);
    item.appendChild(body);
    list.appendChild(item);
  });

  setWifiConfigBusy(wifiConfigBusy);
}

async function loadWifiConfig(silent=true){
  if(wifiConfigBusy) return;
  setWifiConfigBusy(true);
  const message = document.getElementById('wifiConfigMessage');
  try{
    const r = await fetchWithTimeout('/api/config/wifi', {cache:'no-store'}, 4500);
    const j = await r.json();
    if(!j || !j.ok) throw new Error((j && j.message) || '读取失败');

    wifiConfigMaxNetworks = Number(j.max_networks) || 16;
    wifiConfigNetworks = (j.networks || []).map((network, index)=>({
      sourceIndex:Number.isInteger(network.index) ? network.index : index,
      ssid:network.ssid || '',
      password:'',
      hasPassword:!!network.has_password,
      clearPassword:false,
      hidden:!!network.hidden,
      channel:Number(network.channel) || 0,
      bssid:network.bssid || '',
      active:!!network.active,
      expanded:false
    }));

    const hostname = document.getElementById('wifiHostname');
    if(hostname) hostname.value = j.hostname || 'esp32s3-player';
    const path = document.getElementById('wifiConfigPathText');
    if(path) path.textContent = j.path || '/System/config/wifi.conf';
    const status = document.getElementById('wifiConnectionText');
    if(status){
      const name = j.active_ssid || (j.mode === 'AP' ? '设备热点' : '未连接');
      status.textContent = `${j.mode || '-'} · ${name} · ${j.ip || '-'}`;
    }
    if(message){
      message.textContent = j.configured
        ? (j.write_pending
          ? '当前显示的是PSRAM待写配置；下次切歌后写入TF卡。'
          : '网页不会读取明文密码；已有网络密码留空即保持不变。')
        : `尚无有效配置：${j.error || '请添加网络后保存'}`;
    }
    renderWifiNetworks();
  }catch(e){
    if(message) message.textContent = `WiFi配置读取失败：${e.message || '网络错误'}`;
    if(!silent) alert('WiFi配置读取失败');
  }finally{
    setWifiConfigBusy(false);
  }
}

async function saveWifiConfig(applyAfterSave){
  if(wifiConfigBusy) return;
  syncWifiModelFromDom();
  if(!wifiConfigNetworks.length){
    alert('至少保留一个WiFi网络');
    return;
  }
  if(applyAfterSave &&
     !confirm('配置写入TF卡后将断开当前网络，并按新顺序重新连接。确认继续？')){
    return;
  }

  const params = new URLSearchParams();
  params.set('hostname', document.getElementById('wifiHostname').value.trim());
  params.set('count', String(wifiConfigNetworks.length));

  wifiConfigNetworks.forEach((network, index)=>{
    const password = network.password || '';
    const keepPassword = network.sourceIndex >= 0 &&
      network.hasPassword &&
      !network.clearPassword &&
      !password.length;
    params.set(`ssid_${index}`, (network.ssid || '').trim());
    params.set(`password_${index}`, password);
    params.set(`keep_password_${index}`, keepPassword ? '1' : '0');
    params.set(`source_index_${index}`, String(network.sourceIndex));
    params.set(`hidden_${index}`, network.hidden ? '1' : '0');
    params.set(`channel_${index}`, String(Number(network.channel) || 0));
    params.set(`bssid_${index}`, (network.bssid || '').trim());
  });

  setWifiConfigBusy(true);
  const message = document.getElementById('wifiConfigMessage');
  try{
    const url = applyAfterSave
      ? '/api/config/wifi/apply'
      : '/api/config/wifi/save';
    const r = await fetchWithTimeout(url, {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:params.toString()
    }, 6000);
    const j = await r.json();
    if(!j || !j.ok){
      alert((j && j.message) || 'WiFi配置保存失败');
      return;
    }
    if(message) message.textContent = j.message || 'WiFi配置已保存';
    alert(j.message || 'WiFi配置已保存');
    if(!applyAfterSave || j.state === 'pending'){
      setWifiConfigBusy(false);
      await loadWifiConfig(true);
    }
  }catch(e){
    alert('WiFi配置保存失败');
  }finally{
    setWifiConfigBusy(false);
  }
}

function setRadioConfigBusy(busy){
  radioConfigBusy = !!busy;
  ['radioAddBtn','radioReloadBtn','radioSaveBtn'].forEach(id=>{
    const el = document.getElementById(id);
    if(el) el.disabled = radioConfigBusy;
  });
  document.querySelectorAll('#radioConfigList input,#radioConfigList button')
    .forEach(el=>{
      el.disabled = radioConfigBusy || el.dataset.edgeDisabled === '1';
    });
}

function radioMakeInput(value, placeholder='', maxLength=0){
  const input = document.createElement('input');
  input.type = 'text';
  input.value = value == null ? '' : String(value);
  input.placeholder = placeholder;
  input.autocomplete = 'off';
  if(maxLength > 0) input.maxLength = maxLength;
  return input;
}

function radioMakeField(labelText, input, extraClass=''){
  const box = document.createElement('div');
  box.className = `radio-field${extraClass ? ` ${extraClass}` : ''}`;
  const label = document.createElement('label');
  label.textContent = labelText;
  box.append(label, input);
  return box;
}

function syncRadioConfigFromDom(){
  document.querySelectorAll('#radioConfigList .radio-item').forEach((item, index)=>{
    const model = radioConfigItems[index];
    if(!model) return;
    model.expanded = item.open;
    model.name = item.querySelector('[data-field="name"]').value;
    model.url = item.querySelector('[data-field="url"]').value;
    model.format = item.querySelector('[data-field="format"]').value;
    model.region = item.querySelector('[data-field="region"]').value;
    model.logo = item.querySelector('[data-field="logo"]').value;
  });
}

function moveRadioConfigItem(index, delta){
  if(radioConfigBusy) return;
  syncRadioConfigFromDom();
  const target = index + delta;
  if(target < 0 || target >= radioConfigItems.length) return;
  const temp = radioConfigItems[index];
  radioConfigItems[index] = radioConfigItems[target];
  radioConfigItems[target] = temp;
  renderRadioConfigItems();
}

function removeRadioConfigItem(index){
  if(radioConfigBusy) return;
  syncRadioConfigFromDom();
  const item = radioConfigItems[index];
  if(item && item.name && !confirm(`确认删除电台“${item.name}”？`)) return;
  radioConfigItems.splice(index, 1);
  renderRadioConfigItems();
}

function addRadioConfigItem(){
  if(radioConfigBusy) return;
  syncRadioConfigFromDom();
  if(radioConfigItems.length >= radioConfigMaxItems){
    alert(`最多配置 ${radioConfigMaxItems} 个电台`);
    return;
  }
  radioConfigItems.push({name:'',url:'',format:'mp3',region:'',logo:'',expanded:true});
  renderRadioConfigItems();
  const rows = document.querySelectorAll('#radioConfigList .radio-item');
  const last = rows[rows.length - 1];
  if(last){
    const input = last.querySelector('[data-field="name"]');
    if(input) input.focus();
  }
}

async function uploadRadioLogo(index, fileInput){
  if(radioConfigBusy) return;
  const file = fileInput && fileInput.files ? fileInput.files[0] : null;
  if(!file){
    alert('请先选择JPEG或PNG图片');
    return;
  }
  if(file.size <= 0 || file.size > 1024 * 1024){
    alert('台标大小必须在1MB以内');
    return;
  }
  if(!/^image\/(jpeg|png)$/i.test(file.type || '')){
    alert('只支持JPEG或PNG图片');
    return;
  }

  setRadioConfigBusy(true);
  const message = document.getElementById('radioConfigMessage');
  try{
    const form = new FormData();
    form.append('file', file, file.name);
    const r = await fetchWithTimeout(`/api/config/radios/logo?slot=${index}`, {
      method:'POST',
      body:form
    }, 30000);
    const j = await r.json();
    if(!j || !j.ok){
      alert((j && j.message) || '台标上传失败');
      return;
    }
    syncRadioConfigFromDom();
    if(radioConfigItems[index]) radioConfigItems[index].logo = j.path || '';
    renderRadioConfigItems();
    if(message) message.textContent = j.message || '台标已上传';
    alert(`${j.message || '台标已上传'}\n路径：${j.path || '-'}`);
  }catch(e){
    alert(`台标上传失败：${e.message || '网络错误'}`);
  }finally{
    setRadioConfigBusy(false);
  }
}

function renderRadioConfigItems(){
  const list = document.getElementById('radioConfigList');
  if(!list) return;
  list.textContent = '';
  const count = document.getElementById('radioConfigCount');
  if(count) count.textContent = `${radioConfigItems.length} 个`;

  if(!radioConfigItems.length){
    const empty = document.createElement('div');
    empty.className = 'radio-empty';
    empty.textContent = '当前没有电台，可添加后保存；保存空列表会清空电台目录。';
    list.appendChild(empty);
    setRadioConfigBusy(radioConfigBusy);
    return;
  }

  radioConfigItems.forEach((model, index)=>{
    const item = document.createElement('details');
    item.className = 'radio-item';
    item.open = !!model.expanded;
    item.addEventListener('toggle', ()=>{ model.expanded = item.open; });

    const summary = document.createElement('summary');
    const main = document.createElement('div');
    main.className = 'config-item-main';
    const order = document.createElement('span');
    order.className = 'config-item-title';
    order.textContent = `电台 ${index + 1}`;
    const summaryName = document.createElement('span');
    summaryName.className = 'config-item-name';
    summaryName.textContent = model.name || '未命名电台';
    main.append(order, summaryName);
    const meta = document.createElement('span');
    meta.className = 'config-item-meta';
    meta.textContent = [model.format || '自动', model.region || '未分类'].join(' · ');
    summary.append(main, meta);
    item.appendChild(summary);

    const body = document.createElement('div');
    body.className = 'radio-item-body';
    const actions = document.createElement('div');
    actions.className = 'config-item-actions-row';
    const up = document.createElement('button');
    up.type = 'button'; up.className = 'secondary'; up.textContent = '上移';
    up.dataset.edgeDisabled = index === 0 ? '1' : '0';
    up.disabled = index === 0;
    up.onclick = ()=>moveRadioConfigItem(index, -1);
    const down = document.createElement('button');
    down.type = 'button'; down.className = 'secondary'; down.textContent = '下移';
    down.dataset.edgeDisabled = index === radioConfigItems.length - 1 ? '1' : '0';
    down.disabled = index === radioConfigItems.length - 1;
    down.onclick = ()=>moveRadioConfigItem(index, 1);
    const remove = document.createElement('button');
    remove.type = 'button'; remove.className = 'danger'; remove.textContent = '删除';
    remove.onclick = ()=>removeRadioConfigItem(index);
    actions.append(up, down, remove);
    body.appendChild(actions);

    const grid = document.createElement('div');
    grid.className = 'radio-grid';
    const name = radioMakeInput(model.name, '电台名称', 96);
    name.dataset.field = 'name';
    name.addEventListener('input', ()=>{
      summaryName.textContent = name.value.trim() || '未命名电台';
    });
    grid.appendChild(radioMakeField('名称', name));
    const format = radioMakeInput(model.format, 'mp3 / aac / auto', 24);
    format.dataset.field = 'format';
    grid.appendChild(radioMakeField('格式（可选）', format));
    const url = radioMakeInput(model.url, 'http://或https://流地址', 512);
    url.dataset.field = 'url';
    grid.appendChild(radioMakeField('电台地址', url, 'radio-url-field'));
    const region = radioMakeInput(model.region, '地区或分类', 64);
    region.dataset.field = 'region';
    grid.appendChild(radioMakeField('地区（可选）', region));
    const refreshMeta = ()=>{
      meta.textContent = [format.value.trim() || '自动', region.value.trim() || '未分类'].join(' · ');
    };
    format.addEventListener('input', refreshMeta);
    region.addEventListener('input', refreshMeta);
    const logo = radioMakeInput(model.logo, '远程URL或上传后的本地路径', 192);
    logo.dataset.field = 'logo';
    grid.appendChild(radioMakeField('台标路径（可选）', logo));
    body.appendChild(grid);

    const upload = document.createElement('div');
    upload.className = 'radio-upload';
    const file = document.createElement('input');
    file.type = 'file';
    file.accept = 'image/jpeg,image/png,.jpg,.jpeg,.png';
    const button = document.createElement('button');
    button.type = 'button'; button.className = 'secondary';
    button.textContent = '上传台标';
    button.onclick = ()=>uploadRadioLogo(index, file);
    const hint = document.createElement('span');
    hint.className = 'muted';
    hint.textContent = 'JPEG/PNG，最大1MB；上传后仍需保存电台配置。';
    upload.append(file, button, hint);
    body.appendChild(upload);
    item.appendChild(body);
    list.appendChild(item);
  });

  setRadioConfigBusy(radioConfigBusy);
}

async function loadRadioConfig(silent=true){
  if(radioConfigBusy) return;
  setRadioConfigBusy(true);
  const message = document.getElementById('radioConfigMessage');
  try{
    const r = await fetchWithTimeout('/api/config/radios', {cache:'no-store'}, 6000);
    const j = await r.json();
    if(!j || !j.ok) throw new Error((j && (j.error || j.message)) || '读取失败');
    radioConfigMaxItems = Number(j.max_items) || 64;
    radioConfigItems = (j.items || []).map(item=>({
      name:item.name || '',
      url:item.url || '',
      format:item.format || '',
      region:item.region || '',
      logo:item.logo || '',
      expanded:false
    }));
    const path = document.getElementById('radioConfigPathText');
    if(path) path.textContent = j.path || '/System/config/radio_list.txt';
    const dir = document.getElementById('radioAssetDirText');
    if(dir) dir.textContent = j.asset_dir || '/System/assets/radio';
    if(message){
      const base = j.write_pending
        ? '当前显示的是PSRAM待写电台配置；下次切歌后写入TF卡。'
        : '电台地址、顺序和台标可在此管理；本地播放期间保存不会直接写卡。';
      message.textContent = j.warning ? `${base} 注意：${j.warning}` : base;
    }
    renderRadioConfigItems();
  }catch(e){
    radioConfigItems = [];
    renderRadioConfigItems();
    if(message) message.textContent = `电台配置读取失败：${e.message || '网络错误'}`;
    if(!silent) alert('电台配置读取失败');
  }finally{
    setRadioConfigBusy(false);
  }
}

async function saveRadioConfig(){
  if(radioConfigBusy) return;
  syncRadioConfigFromDom();

  const params = new URLSearchParams();
  params.set('count', String(radioConfigItems.length));
  radioConfigItems.forEach((item, index)=>{
    params.set(`name_${index}`, (item.name || '').trim());
    params.set(`url_${index}`, (item.url || '').trim());
    params.set(`format_${index}`, (item.format || '').trim());
    params.set(`region_${index}`, (item.region || '').trim());
    params.set(`logo_${index}`, (item.logo || '').trim());
  });

  setRadioConfigBusy(true);
  const message = document.getElementById('radioConfigMessage');
  try{
    const r = await fetchWithTimeout('/api/config/radios/save', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:params.toString()
    }, 10000);
    const j = await r.json();
    if(!j || !j.ok){
      alert((j && j.message) || '电台配置保存失败');
      return;
    }
    if(message) message.textContent = j.message || '电台配置已保存';
    alert(j.message || '电台配置已保存');
    setRadioConfigBusy(false);
    await loadRadioConfig(true);
  }catch(e){
    alert(`电台配置保存失败：${e.message || '网络错误'}`);
  }finally{
    setRadioConfigBusy(false);
  }
}

function setNasConfigBusy(busy){
  nasConfigBusy = !!busy;
  ['nasAddBtn','nasReloadBtn','nasSaveBtn','nasApplyBtn'].forEach(id=>{
    const el = document.getElementById(id);
    if(el) el.disabled = nasConfigBusy;
  });
  const base = document.getElementById('nasBaseUrl');
  if(base) base.disabled = nasConfigBusy;
  document.querySelectorAll('#nasSourceList input').forEach(el=>{
    el.disabled = nasConfigBusy;
  });
  document.querySelectorAll('#nasSourceList button').forEach(el=>{
    el.disabled = nasConfigBusy || el.dataset.edgeDisabled === '1';
  });
}

function nasMakeInput(value, placeholder='', maxLength=0){
  const input = document.createElement('input');
  input.type = 'text';
  input.value = value == null ? '' : String(value);
  input.placeholder = placeholder;
  input.autocomplete = 'off';
  if(maxLength > 0) input.maxLength = maxLength;
  return input;
}

function nasMakeField(labelText, input, extraClass=''){
  const box = document.createElement('div');
  box.className = `nas-field${extraClass ? ` ${extraClass}` : ''}`;
  const label = document.createElement('label');
  label.textContent = labelText;
  box.append(label, input);
  return box;
}

function syncNasConfigFromDom(){
  document.querySelectorAll('#nasSourceList .nas-item').forEach((item, index)=>{
    const model = nasConfigSources[index];
    if(!model) return;
    model.expanded = item.open;
    model.name = item.querySelector('[data-field="name"]').value;
    model.relative_path = item.querySelector('[data-field="path"]').value;
    model.list_name = item.querySelector('[data-field="list"]').value;
    const active = item.querySelector('[data-field="active"]');
    if(active && active.checked) nasConfigActiveIndex = index;
  });
}

function moveNasSource(index, delta){
  if(nasConfigBusy) return;
  syncNasConfigFromDom();
  const target = index + delta;
  if(target < 0 || target >= nasConfigSources.length) return;
  const temp = nasConfigSources[index];
  nasConfigSources[index] = nasConfigSources[target];
  nasConfigSources[target] = temp;
  if(nasConfigActiveIndex === index) nasConfigActiveIndex = target;
  else if(nasConfigActiveIndex === target) nasConfigActiveIndex = index;
  renderNasSources();
}

function removeNasSource(index){
  if(nasConfigBusy || nasConfigSources.length <= 1){
    if(nasConfigSources.length <= 1) alert('至少保留一个NAS曲库源');
    return;
  }
  syncNasConfigFromDom();
  const model = nasConfigSources[index];
  if(model && model.name && !confirm(`确认删除曲库源“${model.name}”？`)) return;
  nasConfigSources.splice(index, 1);
  if(nasConfigActiveIndex === index) nasConfigActiveIndex = 0;
  else if(nasConfigActiveIndex > index) nasConfigActiveIndex -= 1;
  renderNasSources();
}

function addNasSource(){
  if(nasConfigBusy) return;
  syncNasConfigFromDom();
  if(nasConfigSources.length >= nasConfigMaxSources){
    alert(`最多配置 ${nasConfigMaxSources} 个NAS曲库源`);
    return;
  }
  nasConfigSources.push({
    name:'',relative_path:'',list_name:'net_music.txt',expanded:true
  });
  renderNasSources();
  const rows = document.querySelectorAll('#nasSourceList .nas-item');
  const last = rows[rows.length - 1];
  if(last){
    const input = last.querySelector('[data-field="name"]');
    if(input) input.focus();
  }
}

function renderNasSources(){
  const list = document.getElementById('nasSourceList');
  if(!list) return;
  list.textContent = '';
  const count = document.getElementById('nasSourceCount');
  if(count) count.textContent = `${nasConfigSources.length} 个`;

  if(!nasConfigSources.length){
    const empty = document.createElement('div');
    empty.className = 'nas-empty';
    empty.textContent = '至少需要一个NAS曲库源。';
    list.appendChild(empty);
    setNasConfigBusy(nasConfigBusy);
    return;
  }

  if(nasConfigActiveIndex < 0 || nasConfigActiveIndex >= nasConfigSources.length){
    nasConfigActiveIndex = 0;
  }

  nasConfigSources.forEach((model, index)=>{
    const item = document.createElement('details');
    item.className = 'nas-item';
    if(index === nasConfigActiveIndex) item.classList.add('is-current');
    item.open = !!model.expanded;
    item.addEventListener('toggle', ()=>{ model.expanded = item.open; });

    const summary = document.createElement('summary');
    const main = document.createElement('div');
    main.className = 'config-item-main';
    const order = document.createElement('span');
    order.className = 'config-item-title';
    order.textContent = `曲库 ${index + 1}`;
    const summaryName = document.createElement('span');
    summaryName.className = 'config-item-name';
    summaryName.textContent = model.name || '未命名曲库';
    main.append(order, summaryName);
    if(index === nasConfigActiveIndex){
      const badge = document.createElement('span');
      badge.className = 'nas-active-badge';
      badge.textContent = '当前源';
      main.appendChild(badge);
    }
    const meta = document.createElement('span');
    meta.className = 'config-item-meta';
    const updateMeta = ()=>{
      const folder = (model.relative_path || '').trim() || '根目录';
      const file = (model.list_name || '').trim() || 'net_music.txt';
      meta.textContent = `${folder} / ${file}`;
    };
    updateMeta();
    summary.append(main, meta);
    item.appendChild(summary);

    const body = document.createElement('div');
    body.className = 'nas-item-body';
    const actions = document.createElement('div');
    actions.className = 'config-item-actions-row';
    const up = document.createElement('button');
    up.type = 'button'; up.className = 'secondary'; up.textContent = '上移';
    up.dataset.edgeDisabled = index === 0 ? '1' : '0';
    up.disabled = index === 0;
    up.onclick = ()=>moveNasSource(index, -1);
    const down = document.createElement('button');
    down.type = 'button'; down.className = 'secondary'; down.textContent = '下移';
    down.dataset.edgeDisabled = index === nasConfigSources.length - 1 ? '1' : '0';
    down.disabled = index === nasConfigSources.length - 1;
    down.onclick = ()=>moveNasSource(index, 1);
    const remove = document.createElement('button');
    remove.type = 'button'; remove.className = 'danger'; remove.textContent = '删除';
    remove.onclick = ()=>removeNasSource(index);
    actions.append(up, down, remove);
    body.appendChild(actions);

    const grid = document.createElement('div');
    grid.className = 'nas-grid';
    const name = nasMakeInput(model.name, '例如：我喜欢', 96);
    name.dataset.field = 'name';
    name.addEventListener('input', ()=>{
      model.name = name.value;
      summaryName.textContent = name.value.trim() || '未命名曲库';
    });
    grid.appendChild(nasMakeField('曲库名称', name));
    const listName = nasMakeInput(model.list_name || 'net_music.txt', 'net_music.txt', 192);
    listName.dataset.field = 'list';
    listName.addEventListener('input', ()=>{
      model.list_name = listName.value;
      updateMeta();
    });
    grid.appendChild(nasMakeField('列表文件名', listName));
    const path = nasMakeInput(model.relative_path, '留空表示NAS根目录', 384);
    path.dataset.field = 'path';
    path.addEventListener('input', ()=>{
      model.relative_path = path.value;
      updateMeta();
    });
    grid.appendChild(nasMakeField('相对目录', path, 'nas-path-field'));
    body.appendChild(grid);

    const activeRow = document.createElement('label');
    activeRow.className = 'nas-active-row';
    const active = document.createElement('input');
    active.type = 'radio';
    active.name = 'nasActiveSource';
    active.dataset.field = 'active';
    active.checked = index === nasConfigActiveIndex;
    active.onchange = ()=>{
      if(active.checked){
        syncNasConfigFromDom();
        nasConfigActiveIndex = index;
        renderNasSources();
      }
    };
    activeRow.append(active, document.createTextNode('设为保存并应用后的当前NAS曲库源'));
    body.appendChild(activeRow);
    item.appendChild(body);
    list.appendChild(item);
  });

  setNasConfigBusy(nasConfigBusy);
}

async function loadNasConfig(silent=true){
  if(nasConfigBusy) return;
  setNasConfigBusy(true);
  const message = document.getElementById('nasConfigMessage');
  try{
    const r = await fetchWithTimeout('/api/config/nas', {cache:'no-store'}, 7000);
    const j = await r.json();
    if(!j || !j.ok) throw new Error((j && (j.error || j.message)) || '读取失败');
    nasConfigMaxSources = Number(j.max_sources) || 8;
    nasConfigActiveIndex = Number(j.active_index) || 0;
    nasConfigSources = (j.sources || []).map(source=>({
      name:source.name || '',
      relative_path:source.relative_path || '',
      list_name:source.list_name || 'net_music.txt',
      expanded:false
    }));
    if(!nasConfigSources.length){
      nasConfigSources = [{
        name:'NAS音乐',relative_path:'',list_name:'net_music.txt',expanded:false
      }];
      nasConfigActiveIndex = 0;
    }
    const base = document.getElementById('nasBaseUrl');
    if(base) base.value = j.base_url || '';
    const basePath = document.getElementById('nasBasePathText');
    if(basePath) basePath.textContent = j.base_path || '/System/config/net_music_base.txt';
    const sourcesPath = document.getElementById('nasSourcesPathText');
    if(sourcesPath) sourcesPath.textContent = j.sources_path || '/System/config/net_music_sources.txt';
    if(message){
      let baseMessage = '根地址和曲库源可在此管理；已有曲库源默认逐条收起。';
      if(j.write_pending) baseMessage = '当前显示的是PSRAM待写NAS配置；下次切歌后写入TF卡。';
      if(j.apply_pending) baseMessage += ' 写入完成后会自动应用。';
      message.textContent = j.warning ? `${baseMessage} 注意：${j.warning}` : baseMessage;
    }
    renderNasSources();
  }catch(e){
    if(message) message.textContent = `NAS配置读取失败：${e.message || '网络错误'}`;
    if(!silent) alert('NAS配置读取失败');
  }finally{
    setNasConfigBusy(false);
  }
}

async function saveNasConfig(apply){
  if(nasConfigBusy) return;
  syncNasConfigFromDom();
  const base = document.getElementById('nasBaseUrl');
  const params = new URLSearchParams();
  params.set('base_url', (base ? base.value : '').trim());
  params.set('count', String(nasConfigSources.length));
  params.set('active_index', String(nasConfigActiveIndex));
  nasConfigSources.forEach((source, index)=>{
    params.set(`name_${index}`, (source.name || '').trim());
    params.set(`path_${index}`, (source.relative_path || '').trim());
    params.set(`list_${index}`, (source.list_name || 'net_music.txt').trim());
  });

  setNasConfigBusy(true);
  const message = document.getElementById('nasConfigMessage');
  try{
    const endpoint = apply ? '/api/config/nas/apply' : '/api/config/nas/save';
    const r = await fetchWithTimeout(endpoint, {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:params.toString()
    }, 12000);
    const j = await r.json();
    if(!j || !j.ok){
      alert((j && j.message) || 'NAS配置保存失败');
      return;
    }
    if(message) message.textContent = j.message || 'NAS配置已保存';
    alert(j.message || 'NAS配置已保存');
    setNasConfigBusy(false);
    await loadNasConfig(true);
  }catch(e){
    alert(`NAS配置保存失败：${e.message || '网络错误'}`);
  }finally{
    setNasConfigBusy(false);
  }
}

let systemAssetBusy = false;

function formatAssetBytes(size){
  const bytes = Number(size) || 0;
  if(bytes < 1024) return `${bytes} B`;
  return `${(bytes / 1024).toFixed(bytes >= 102400 ? 0 : 1)} KB`;
}

function setSystemAssetBusy(busy){
  systemAssetBusy = !!busy;
  ['defaultCoverAssetUploadBtn','netCoverLoadingAssetUploadBtn'].forEach(id=>{
    const el = document.getElementById(id);
    if(el) el.disabled = systemAssetBusy;
  });
}

function renderSystemAssetStatus(prefix, asset){
  const summary = document.getElementById(`${prefix}AssetSummary`);
  const path = document.getElementById(`${prefix}AssetPath`);
  const status = document.getElementById(`${prefix}AssetStatus`);
  const exists = !!(asset && asset.exists);
  const pending = !!(asset && asset.pending);
  const size = asset ? Number(asset.size) || 0 : 0;
  if(summary){
    summary.textContent = pending
      ? `待写入 · ${formatAssetBytes(size)}`
      : (exists ? `已配置 · ${formatAssetBytes(size)}` : '尚未配置');
  }
  if(path && asset && asset.path) path.textContent = asset.path;
  if(status){
    status.textContent = pending
      ? 'PSRAM待写'
      : (exists ? `已存在，${formatAssetBytes(size)}` : '文件不存在，可直接上传创建');
  }
}

async function loadSystemAssets(silent=true){
  const message = document.getElementById('systemAssetMessage');
  try{
    const r = await fetchWithTimeout('/api/config/assets', {cache:'no-store'}, 5000);
    const j = await r.json();
    if(!j || !j.ok) throw new Error((j && (j.error || j.message)) || '读取失败');
    renderSystemAssetStatus('defaultCover', j.default_cover || {});
    renderSystemAssetStatus('netCoverLoading', j.net_cover_loading || {});
    const pending = !!((j.default_cover && j.default_cover.pending) ||
                       (j.net_cover_loading && j.net_cover_loading.pending));
    if(message){
      message.textContent = pending
        ? '当前有图片暂存在PSRAM；下次切歌后写入TF卡。'
        : '图片先暂存PSRAM；本地播放时在下次切歌安全窗口写入TF卡。';
    }
  }catch(e){
    if(message) message.textContent = `系统图片状态读取失败：${e.message || '网络错误'}`;
    if(!silent) alert('系统图片状态读取失败');
  }
}

async function uploadSystemAsset(kind){
  if(systemAssetBusy) return;
  const isDefault = kind === 'default_cover';
  const fileInput = document.getElementById(
      isDefault ? 'defaultCoverAssetFile' : 'netCoverLoadingAssetFile');
  const file = fileInput && fileInput.files ? fileInput.files[0] : null;
  if(!file){
    alert('请先选择JPEG图片');
    return;
  }
  if(file.size <= 0 || file.size > 400 * 1024){
    alert('图片必须是最大400KB的完整JPEG文件');
    return;
  }
  const lowerName = (file.name || '').toLowerCase();
  if(file.type !== 'image/jpeg' &&
     !lowerName.endsWith('.jpg') &&
     !lowerName.endsWith('.jpeg')){
    alert('只支持JPEG图片');
    return;
  }

  const endpoint = isDefault
      ? '/api/config/assets/default-cover'
      : '/api/config/assets/net-cover-loading';
  const form = new FormData();
  form.append('file', file, file.name || 'cover.jpg');
  const message = document.getElementById('systemAssetMessage');
  setSystemAssetBusy(true);
  if(message) message.textContent = '正在上传并暂存PSRAM...';
  try{
    const r = await fetchWithTimeout(endpoint, {method:'POST', body:form}, 30000);
    const j = await r.json();
    if(!j || !j.ok){
      throw new Error((j && (j.error || j.message)) || '上传失败');
    }
    if(message) message.textContent = j.message || '图片上传成功';
    if(fileInput) fileInput.value = '';
    alert(j.message || '图片上传成功');
    await loadSystemAssets(true);
  }catch(e){
    if(message) message.textContent = `图片上传失败：${e.message || '网络错误'}`;
    alert(`图片上传失败：${e.message || '网络错误'}`);
  }finally{
    setSystemAssetBusy(false);
  }
}

function btDeviceStateLabel(state){
  const labels = {
    unknown:'未查询',
    querying:'查询中',
    connected:'已连接',
    connected_no_identity:'无名称记录',
    not_connected:'未连接',
    timeout:'查询超时',
    parse_error:'解析失败'
  };
  return labels[state] || '未知';
}

function setAudioOutputBusy(busy){
  audioOutputBusy = !!busy;
  ['audioOutputApplyBtn','ampMuteApplyBtn','btQueryBtn','btPairBtn','btRestartBtn']
    .forEach(id=>{
      const el = document.getElementById(id);
      if(el) el.disabled = audioOutputBusy;
    });
  const route = document.getElementById('audio_output_route');
  if(route) route.disabled = audioOutputBusy;
  const amp = document.getElementById('amp_muted');
  if(amp) amp.disabled = audioOutputBusy || !(lastAudioOutputStatus && lastAudioOutputStatus.can_amp_control);
  const volume = document.getElementById('bt_volume');
  if(volume) volume.disabled = audioOutputBusy || !(lastAudioOutputStatus && lastAudioOutputStatus.can_bt_control);
}

function syncBtVolumeText(){
  const slider = document.getElementById('bt_volume');
  const text = document.getElementById('btVolumeText');
  if(slider && text) text.textContent = `${slider.value}%`;
}

function renderAudioOutputStatus(j){
  lastAudioOutputStatus = j;
  const routeKey = j.route || 'speaker';
  const route = document.getElementById('audio_output_route');
  if(route && !audioOutputBusy) route.value = routeKey;

  const status = document.getElementById('audioOutputStatusText');
  const routeLabels = {headphone:'耳机', speaker:'功放', bluetooth:'蓝牙'};
  if(status) status.textContent = j.bt_restart_in_progress ? '蓝牙重启中' : (routeLabels[routeKey] || '-');

  const speakerCard = document.getElementById('speakerSettingsCard');
  const bluetoothCard = document.getElementById('bluetoothSettingsCard');
  if(speakerCard) speakerCard.hidden = routeKey !== 'speaker';
  if(bluetoothCard) bluetoothCard.hidden = routeKey !== 'bluetooth';

  const ampPower = document.getElementById('ampPowerText');
  if(ampPower){
    ampPower.textContent = !j.amp_shutdown_known
      ? '读取失败'
      : (j.amp_shutdown ? '关断' : '工作');
  }
  const ampMute = document.getElementById('amp_muted');
  if(ampMute) ampMute.checked = !!j.amp_muted;

  const btPower = document.getElementById('btPowerText');
  if(btPower) btPower.textContent = j.bt_restart_in_progress ? '重启中' : (j.bt_power ? '已开启' : '已关闭');
  const btLink = document.getElementById('btLinkText');
  if(btLink){
    btLink.textContent = !j.bt_power
      ? '未上电'
      : (!j.bt_link_known ? '读取失败' : (j.bt_linked ? '已连接' : '未连接'));
  }
  const btDevice = document.getElementById('btDeviceText');
  if(btDevice){
    btDevice.textContent = j.bt_device_name || j.bt_device_mac || btDeviceStateLabel(j.bt_device_state);
  }
  const btVolume = document.getElementById('bt_volume');
  if(btVolume && document.activeElement !== btVolume) btVolume.value = String(Number(j.user_volume || 0));
  syncBtVolumeText();

  setAudioOutputBusy(audioOutputBusy);
  const ampApply = document.getElementById('ampMuteApplyBtn');
  if(ampApply) ampApply.disabled = audioOutputBusy || !j.can_amp_control;
  const btQuery = document.getElementById('btQueryBtn');
  if(btQuery) btQuery.disabled = audioOutputBusy || !j.can_bt_query;
  ['btPairBtn','btRestartBtn'].forEach(id=>{
    const el = document.getElementById(id);
    if(el) el.disabled = audioOutputBusy || !j.can_bt_control;
  });

  if(routeKey === 'bluetooth' && j.bt_linked &&
     (j.bt_device_state === 'unknown' || j.bt_device_state === 'not_connected') &&
     !btDeviceAutoQueryPending && !audioOutputBusy){
    btDeviceAutoQueryPending = true;
    queryBtDevice(true).finally(()=>{
      setTimeout(()=>{ btDeviceAutoQueryPending = false; }, 5000);
    });
  }
}

async function loadAudioOutputStatus(silent=false){
  try{
    const r = await fetch('/api/audio-output/status', {cache:'no-store'});
    const j = await r.json();
    if(j && j.ok) renderAudioOutputStatus(j);
    else if(!silent) alert((j && j.message) || '音频输出状态读取失败');
  }catch(e){
    if(!silent) alert('音频输出状态读取失败');
  }
}

async function postAudioOutput(url, params={}){
  const body = new URLSearchParams(params).toString();
  const r = await fetchWithTimeout(url, {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
    body
  }, 5000);
  return r.json();
}

async function refreshAudioOutputAfterAction(){
  await new Promise(resolve=>setTimeout(resolve, 350));
  await loadAudioOutputStatus(true);
  setTimeout(()=>loadAudioOutputStatus(true), 1000);
}

async function applyAudioOutputRoute(){
  if(audioOutputBusy) return;
  const select = document.getElementById('audio_output_route');
  const target = select ? select.value : 'speaker';
  const labels = {headphone:'耳机', speaker:'功放', bluetooth:'蓝牙'};
  if(lastAudioOutputStatus && lastAudioOutputStatus.route === target) return;
  if(!confirm(`确认切换音频输出到“${labels[target] || target}”？`)){
    if(lastAudioOutputStatus && select) select.value = lastAudioOutputStatus.route;
    return;
  }
  setAudioOutputBusy(true);
  try{
    const j = await postAudioOutput('/api/audio-output/route', {route:target});
    if(!j || !j.ok) alert((j && j.message) || '输出路径切换失败');
  }catch(e){
    alert('输出路径切换失败');
  }finally{
    setAudioOutputBusy(false);
    await refreshAudioOutputAfterAction();
  }
}

async function applyAmpMute(){
  if(audioOutputBusy) return;
  const enabled = document.getElementById('amp_muted').checked;
  setAudioOutputBusy(true);
  try{
    const j = await postAudioOutput('/api/audio-output/amp-mute', {enabled:enabled ? '1' : '0'});
    if(!j || !j.ok) alert((j && j.message) || '功放静音设置失败');
  }catch(e){
    alert('功放静音设置失败');
  }finally{
    setAudioOutputBusy(false);
    await refreshAudioOutputAfterAction();
  }
}

async function queryBtDevice(auto=false){
  if(audioOutputBusy && !auto) return;
  if(!auto) setAudioOutputBusy(true);
  try{
    const j = await postAudioOutput('/api/audio-output/bluetooth/query');
    if(!j || !j.ok){
      if(!auto) alert((j && j.message) || '蓝牙设备查询失败');
      return;
    }
    setTimeout(()=>loadAudioOutputStatus(true), 500);
    setTimeout(()=>loadAudioOutputStatus(true), 1500);
  }catch(e){
    if(!auto) alert('蓝牙设备查询失败');
  }finally{
    if(!auto) setAudioOutputBusy(false);
  }
}

async function applyBtVolume(){
  if(audioOutputBusy) return;
  const value = document.getElementById('bt_volume').value;
  setAudioOutputBusy(true);
  try{
    const j = await postAudioOutput('/api/audio-output/bluetooth/volume', {value});
    if(!j || !j.ok) alert((j && j.message) || '蓝牙音量设置失败');
  }catch(e){
    alert('蓝牙音量设置失败');
  }finally{
    setAudioOutputBusy(false);
    await refreshAudioOutputAfterAction();
  }
}

async function pairBluetooth(){
  if(audioOutputBusy || !confirm('确认发送蓝牙配对按键？')) return;
  setAudioOutputBusy(true);
  try{
    const j = await postAudioOutput('/api/audio-output/bluetooth/pair');
    alert(j && j.ok ? '已发送配对按键' : ((j && j.message) || '蓝牙配对操作失败'));
  }catch(e){
    alert('蓝牙配对操作失败');
  }finally{
    setAudioOutputBusy(false);
    await refreshAudioOutputAfterAction();
  }
}

async function restartBluetooth(){
  if(audioOutputBusy || !confirm('确认重启蓝牙模块？')) return;
  setAudioOutputBusy(true);
  try{
    const j = await postAudioOutput('/api/audio-output/bluetooth/restart');
    if(!j || !j.ok) alert((j && j.message) || '蓝牙重启失败');
  }catch(e){
    alert('蓝牙重启失败');
  }finally{
    setAudioOutputBusy(false);
    await refreshAudioOutputAfterAction();
  }
}


function diagSetText(id, text){
  const el = document.getElementById(id);
  if(el) el.textContent = text;
}
function diagFormatBytes(value){
  const n = Math.max(0, Number(value) || 0);
  if(n >= 1024 * 1024) return `${(n / 1024 / 1024).toFixed(2)}MB`;
  if(n >= 1024) return `${Math.round(n / 1024)}KB`;
  return `${n}B`;
}
function diagFormatDuration(ms){
  let seconds = Math.floor((Number(ms) || 0) / 1000);
  const days = Math.floor(seconds / 86400); seconds %= 86400;
  const hours = Math.floor(seconds / 3600); seconds %= 3600;
  const minutes = Math.floor(seconds / 60);
  if(days > 0) return `${days}天${hours}时`;
  if(hours > 0) return `${hours}时${minutes}分`;
  return `${minutes}分${seconds % 60}秒`;
}
function diagFormatRuntime(b){
  const minutes = Math.max(0, Number(b && b.runtime_minutes) || 0);
  if(b && b.runtime_ready && minutes > 0){
    if(minutes >= 1440) return '超过24小时';
    if(minutes < 60) return `约${minutes}分钟`;
    return `约${Math.floor(minutes / 60)}时${minutes % 60}分`;
  }
  return (b && b.runtime_label) || '-';
}
function diagPowerLabel(b){
  if(!b || !b.valid) return '不可用';
  if(b.power_state === 'charging') return '充电中';
  if(b.power_state === 'external_power') return '外接电源';
  return '电池供电';
}
function diagStack(value){
  const n = Number(value) || 0;
  return n > 0 ? `${n}B` : '未运行';
}
function renderSystemDiagnostics(j){
  const b = j.battery || {};
  const memory = j.memory || {};
  const tasks = j.tasks || {};
  const hardware = j.hardware || {};
  const network = j.network || {};
  const system = j.system || {};
  const audio = j.audio || {};

  diagSetText('diagBatteryPercent', b.valid ? `${Number(b.percent) || 0}%` : '未知');
  diagSetText('diagBatteryElectrical', b.valid
    ? `${formatDiagVoltage(b.voltage_mv)} / ${formatDiagCurrent(b.current_ma)}`
    : '-');
  diagSetText('diagBatteryPower', `${diagPowerLabel(b)} · BQ ${b.bq_ready ? 'OK' : 'ERR'}`);
  diagSetText('diagBatteryRuntime', diagFormatRuntime(b));
  const remaining = Number(b.remaining_capacity_mah) || 0;
  const capacity = Number(b.full_charge_capacity_mah) || Number(b.design_capacity_mah) || 0;
  diagSetText('diagBatteryCapacity', `${capacity > 0 ? `${remaining}/${capacity}mAh` : '-'} · SOH ${Number(b.state_of_health_percent) || 0}%`);

  diagSetText('diagInternalRam', `${diagFormatBytes(memory.internal_free)} / 最大连续 ${diagFormatBytes(memory.internal_largest)}`);
  diagSetText('diagDmaRam', `${diagFormatBytes(memory.dma_free)} / 最大连续 ${diagFormatBytes(memory.dma_largest)}`);
  diagSetText('diagPsram', memory.psram_ready
    ? `${diagFormatBytes(memory.psram_free)} / ${diagFormatBytes(memory.psram_total)}`
    : '无');
  diagSetText('diagMinHeap', diagFormatBytes(memory.heap_min_free));

  diagSetText('diagTaskAudioUi', `${diagStack(tasks.audio)} / ${diagStack(tasks.ui)}`);
  diagSetText('diagTaskAssetCover', `${diagStack(tasks.asset)} / ${diagStack(tasks.net_cover)}`);
  diagSetText('diagTaskFlacRescan', `${diagStack(tasks.flac_prefetch)} / ${diagStack(tasks.rescan)}`);
  diagSetText('diagTaskLoop', diagStack(tasks.loop));

  diagSetText('diagFirmwareUptime', `${system.firmware || '-'} / ${diagFormatDuration(system.uptime_ms)}`);
  diagSetText('diagI2cMcp', `${hardware.i2c_ready ? 'OK' : 'ERR'} ${Math.round((Number(hardware.i2c_clock_hz) || 0) / 1000)}kHz / ${hardware.mcp23017_ready ? 'OK' : 'ERR'}`);
  diagSetText('diagBqRtc', `${hardware.bq27441_ready ? 'OK' : 'ERR'} / ${hardware.rtc_ready ? (hardware.rtc_status || 'OK') : 'ERR'}`);
  const rssi = Number(network.rssi_dbm) || 0;
  diagSetText('diagNetwork', network.connected
    ? `${network.mode || '-'} · ${network.ip || '-'}${rssi ? ` · ${rssi}dBm` : ''}`
    : '未连接');
  const routeLabels = {headphone:'耳机', speaker:'功放', bluetooth:'蓝牙'};
  diagSetText('diagAudio', `${routeLabels[audio.route] || '-'} · 音量${Number(audio.volume) || 0}% · ${audio.playing ? (audio.paused ? '暂停' : '播放') : '停止'}`);
  diagSetText('diagUpdatedText', '已刷新');
}
function formatDiagVoltage(mv){
  const n = Number(mv) || 0;
  return n > 0 ? `${(n / 1000).toFixed(2)}V` : '-';
}
function formatDiagCurrent(ma){
  const n = Number(ma);
  return Number.isFinite(n) ? `${n > 0 ? '+' : ''}${Math.round(n)}mA` : '-';
}
async function loadSystemDiagnostics(silent=true){
  if(systemDiagnosticsBusy) return;
  systemDiagnosticsBusy = true;
  try{
    const r = await fetchWithTimeout('/api/system/diagnostics', {cache:'no-store'}, 4000);
    const j = await r.json();
    if(j && j.ok){
      lastSystemDiagnosticsAt = Date.now();
      renderSystemDiagnostics(j);
    }else if(!silent){
      alert((j && j.message) || '系统诊断读取失败');
    }
  }catch(e){
    diagSetText('diagUpdatedText', '读取失败');
    if(!silent) alert('系统诊断读取失败');
  }finally{
    systemDiagnosticsBusy = false;
  }
}
function handleSystemDiagnosticsToggle(){
  const card = document.getElementById('systemDiagnosticsCard');
  if(card && card.open) loadSystemDiagnostics(true);
}

function renderMusicScanStatus(rescanning){
  settingsRescanning = !!rescanning;
  const text = document.getElementById('scanStatusText');
  const btn = document.getElementById('settingsScanBtn');
  const mode = document.getElementById('scanMode');
  if(text){
    text.textContent = settingsRescanning
      ? '正在扫描（再次点击可请求取消）'
      : '空闲';
  }
  if(btn){
    btn.textContent = settingsRescanning ? '取消重扫' : '开始重扫';
    btn.className = settingsRescanning ? 'danger' : 'warn';
    btn.disabled = scanStatusBusy;
  }
  if(mode){
    mode.disabled = settingsRescanning || scanStatusBusy;
  }
}

async function refreshMusicScanStatus(){
  try{
    const r = await fetch('/api/status/check?token=0', {cache:'no-store'});
    const j = await r.json();
    if(j && j.ok){
      renderMusicScanStatus(!!j.rescanning);
    }
  }catch(e){
    const text = document.getElementById('scanStatusText');
    if(text) text.textContent = '状态读取失败';
  }
}

async function toggleMusicScan(){
  if(scanStatusBusy) return;

  const modeSelect = document.getElementById('scanMode');
  const mode = modeSelect ? modeSelect.value : 'fast';
  const modeLabel = modeSelect && modeSelect.selectedOptions.length
    ? modeSelect.selectedOptions[0].textContent
    : '快速增量';
  const message = settingsRescanning
    ? '确认取消当前曲库重扫？'
    : (mode === 'full'
        ? '强制全量会重建全部索引，确认开始？'
        : `确认开始“${modeLabel}”重扫？`);
  if(!confirm(message)) return;

  scanStatusBusy = true;
  renderMusicScanStatus(settingsRescanning);
  try{
    const options = {method:'POST'};
    if(!settingsRescanning){
      options.headers = {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'};
      options.body = new URLSearchParams({mode}).toString();
    }
    const r = await fetchWithTimeout('/api/scan', options, 5000);
    const j = await r.json();
    if(!j || !j.ok){
      alert((j && (j.message || j.error)) || '重扫操作失败');
    }
  }catch(e){
    alert('重扫请求失败');
  }finally{
    scanStatusBusy = false;
    setTimeout(refreshMusicScanStatus, 300);
  }
}

async function loadSettings(){
  try{
    const r = await fetch('/api/settings', {cache:'no-store'});
    const j = await r.json();
    if(!j.ok) return;
    document.getElementById('refresh_preset').value = j.refresh_preset || 'balanced';
    document.getElementById('lyric_sync_mode').value = j.lyric_sync_mode || 'balanced';
    document.getElementById('show_next_lyric').checked = !!j.show_next_lyric;
    document.getElementById('show_cover').checked = !!j.show_cover;
    document.getElementById('web_cover_spin').checked = !!j.web_cover_spin;
    document.getElementById('device_view').value = j.device_view || 'info';
    document.getElementById('screen_enabled').checked = !!j.screen_enabled;
    document.getElementById('sleep_timer_minutes').value = String(j.sleep_timer_minutes || 0);
    document.getElementById('hall_control_enabled').checked = !!j.hall_control_enabled;
    document.getElementById('solenoid_enabled').checked = !!j.solenoid_enabled;
    document.getElementById('solenoid_direction_inverted').checked = !!j.solenoid_direction_inverted;
    syncHallSolenoidUi();
    document.getElementById('status_led_enabled').checked = !!j.status_led_enabled;
    document.getElementById('status_led_brightness').value = j.status_led_brightness || 'medium';
    syncStatusLedUi();
  }catch(e){}
}
function syncHallSolenoidUi(){
  const hall = document.getElementById('hall_control_enabled');
  const solenoid = document.getElementById('solenoid_enabled');
  const direction = document.getElementById('solenoid_direction_inverted');
  const hint = document.getElementById('hall_solenoid_hint');
  const directionHint = document.getElementById('solenoid_direction_hint');
  if(!hall || !solenoid) return;
  if(solenoid.checked) hall.checked = true;
  hall.disabled = !!solenoid.checked;
  if(hint){
    hint.textContent = solenoid.checked
      ? '电磁铁联动中：播放键只驱动摆臂，霍尔到位后控制播放/暂停。'
      : (hall.checked
          ? '霍尔独立开启：播放键直接切换，霍尔只在位置变化时控制一次。'
          : '电磁铁和霍尔均关闭：播放键直接控制播放/暂停。');
  }
  if(directionHint && direction){
    directionHint.textContent = direction.checked
      ? '当前方向：A/B已反转。若目标离开但摆臂顶向靠近端，可取消反转。'
      : '当前方向：正常映射（A=靠近、B=离开）。';
  }
}
function syncStatusLedUi(){
  const enabled = document.getElementById('status_led_enabled').checked;
  document.getElementById('status_led_brightness').disabled = !enabled;
}
async function postSettings(params, successText){
  params.set('persist', '1');
  try{
    const r = await fetchWithTimeout('/api/settings', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:params.toString()
    }, 3500);
    const j = await r.json();
    alert(j && j.ok ? successText : ((j && j.message) ? j.message : '保存失败'));
    return !!(j && j.ok);
  }catch(e){
    alert('保存失败');
    return false;
  }
}

async function saveWebDisplaySettings(){
  const params = new URLSearchParams();
  params.set('refresh_preset', document.getElementById('refresh_preset').value);
  params.set('lyric_sync_mode', document.getElementById('lyric_sync_mode').value);
  params.set('show_next_lyric', document.getElementById('show_next_lyric').checked ? '1' : '0');
  params.set('show_cover', document.getElementById('show_cover').checked ? '1' : '0');
  params.set('web_cover_spin', document.getElementById('web_cover_spin').checked ? '1' : '0');
  const haptic = document.getElementById('web_haptic_feedback');
  if(haptic && window.webButtonFeedback){
    window.webButtonFeedback.setHapticEnabled(haptic.checked);
  }
  await postSettings(params, '显示设置已保存');
}

async function saveDeviceSettings(){
  syncHallSolenoidUi();
  const params = new URLSearchParams();
  params.set('device_view', document.getElementById('device_view').value);
  params.set('screen_enabled', document.getElementById('screen_enabled').checked ? '1' : '0');
  params.set('sleep_timer_minutes', document.getElementById('sleep_timer_minutes').value);
  params.set('hall_control_enabled', document.getElementById('hall_control_enabled').checked ? '1' : '0');
  params.set('solenoid_enabled', document.getElementById('solenoid_enabled').checked ? '1' : '0');
  params.set('solenoid_direction_inverted', document.getElementById('solenoid_direction_inverted').checked ? '1' : '0');
  params.set('status_led_enabled', document.getElementById('status_led_enabled').checked ? '1' : '0');
  params.set('status_led_brightness', document.getElementById('status_led_brightness').value);
  await postSettings(params, '设备设置已保存');
}

let rtcClockBaseMs = 0;
let rtcClockClientMs = 0;

function parseRtcDateTimeText(text){
  const m = String(text || '').match(/^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})$/);
  if(!m) return null;
  return new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]), Number(m[6]));
}

function pad2(n){
  return String(n).padStart(2, '0');
}

function formatRtcClock(d){
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

function updateRtcClockDisplay(){
  if(!rtcClockBaseMs || !rtcClockClientMs) return;
  const elapsed = Date.now() - rtcClockClientMs;
  document.getElementById('rtcTimeText').textContent = formatRtcClock(new Date(rtcClockBaseMs + elapsed));
}

function startRtcClockDisplay(datetimeText){
  const d = parseRtcDateTimeText(datetimeText);
  if(!d || Number.isNaN(d.getTime())) {
    rtcClockBaseMs = 0;
    rtcClockClientMs = 0;
    return false;
  }
  rtcClockBaseMs = d.getTime();
  rtcClockClientMs = Date.now();
  updateRtcClockDisplay();
  return true;
}

async function loadRtcStatus(){
  try{
    const r = await fetch('/api/rtc/status', {cache:'no-store'});
    const j = await r.json();
    if(!j.ok){
      rtcClockBaseMs = 0;
      rtcClockClientMs = 0;
      document.getElementById('rtcStatusText').textContent = (j && j.message) ? j.message : '时间读取失败';
      document.getElementById('rtcTimeText').textContent = '-';
      return;
    }
    if(j.time_valid && j.datetime && startRtcClockDisplay(j.datetime)){
      document.getElementById('rtcStatusText').textContent = '时间正常';
    }else{
      rtcClockBaseMs = 0;
      rtcClockClientMs = 0;
      document.getElementById('rtcTimeText').textContent = '未校准';
      document.getElementById('rtcStatusText').textContent = '请先校准时间';
    }
  }catch(e){
    rtcClockBaseMs = 0;
    rtcClockClientMs = 0;
    document.getElementById('rtcStatusText').textContent = '时间读取失败';
    document.getElementById('rtcTimeText').textContent = '-';
  }
}

async function syncRtcFromBrowser(){
  const d = new Date();
  const params = new URLSearchParams();
  params.set('year', String(d.getFullYear()));
  params.set('month', String(d.getMonth() + 1));
  params.set('day', String(d.getDate()));
  params.set('weekday', String(d.getDay()));
  params.set('hour', String(d.getHours()));
  params.set('minute', String(d.getMinutes()));
  params.set('second', String(d.getSeconds()));
  try{
    const r = await fetchWithTimeout('/api/rtc/time', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:params.toString()
    }, 3500);
    const j = await r.json();
    if(j && j.ok){
      alert('时间已校准');
      await refreshClockAlarmStatus();
    }else{
      alert((j && j.message) ? j.message : '时间校准失败');
    }
  }catch(e){
    alert('时间校准失败');
  }
}

function parseAlarmNumber(id, min, max){
  const el = document.getElementById(id);
  const raw = (el && el.value ? el.value : '').trim();
  if(raw === '') return null;
  const n = Number(raw);
  if(!Number.isInteger(n) || n < min || n > max) return null;
  return n;
}

function alarmWeekdayMaskFromForm(){
  let mask = 0;
  document.querySelectorAll('.alarmWeekday').forEach(cb => {
    if(cb.checked){
      const w = Number(cb.value);
      if(Number.isInteger(w) && w >= 0 && w <= 6){
        mask |= (1 << w);
      }
    }
  });
  return mask & 0x7F;
}

function setAlarmWeekdayMask(mask){
  const m = Number(mask || 0) & 0x7F;
  document.querySelectorAll('.alarmWeekday').forEach(cb => {
    const w = Number(cb.value);
    cb.checked = ((m & (1 << w)) !== 0);
  });
}

function updateAlarmRepeatUi(){
  const repeat = document.getElementById('alarmRepeat').value;
  const box = document.getElementById('alarmWeekdayBox');
  box.style.display = (repeat === 'weekly') ? 'block' : 'none';
}

function fillAlarmForm(j){
  if(!j || !j.ok) return;
  document.getElementById('alarmHour').value = String(j.hour ?? 7).padStart(2, '0');
  document.getElementById('alarmMinute').value = String(j.minute ?? 30).padStart(2, '0');
  document.getElementById('alarmSecond').value = String(j.second ?? 0).padStart(2, '0');
  document.getElementById('alarmRepeat').value = j.repeat || 'daily';
  setAlarmWeekdayMask(j.weekday_mask ?? 0x7F);
  document.getElementById('alarmAction').value = j.action || 'resume_last';
  document.getElementById('alarmVolume').value = String(j.volume ?? 30);
  updateAlarmRepeatUi();
}

function renderAlarmStatus(j){
  if(!j || !j.ok){
    document.getElementById('alarmEnabledText').textContent = (j && j.message) ? j.message : '读取失败';
    document.getElementById('alarmNextText').textContent = '-';
    return;
  }
  document.getElementById('alarmEnabledText').textContent = j.enabled ? '已启用' : '关闭';
  document.getElementById('alarmNextText').textContent = j.enabled ? (j.next_text || '-') : '未启用';
  fillAlarmForm(j);
}

async function loadAlarmStatus(){
  try{
    const r = await fetch('/api/alarm/status', {cache:'no-store'});
    const j = await r.json();
    renderAlarmStatus(j);
  }catch(e){
    document.getElementById('alarmEnabledText').textContent = '读取失败';
    document.getElementById('alarmNextText').textContent = '-';
  }
}

async function refreshClockAlarmStatus(){
  await loadRtcStatus();
  await loadAlarmStatus();
}

async function saveAlarm(){
  const hour = parseAlarmNumber('alarmHour', 0, 23);
  const minute = parseAlarmNumber('alarmMinute', 0, 59);
  const second = parseAlarmNumber('alarmSecond', 0, 59);
  const volume = parseAlarmNumber('alarmVolume', 0, 100);
  const repeat = document.getElementById('alarmRepeat').value;
  const weekdayMask = alarmWeekdayMaskFromForm();
  const action = document.getElementById('alarmAction').value;
  if(hour === null || minute === null || second === null || volume === null){
    alert('请输入有效闹钟参数：时 0-23，分/秒 0-59，音量 0-100');
    return;
  }
  if(repeat === 'weekly' && weekdayMask === 0){
    alert('每周指定模式至少选择一天');
    return;
  }

  const params = new URLSearchParams();
  params.set('hour', String(hour));
  params.set('minute', String(minute));
  params.set('second', String(second));
  params.set('repeat', repeat);
  params.set('weekday_mask', String(weekdayMask));
  params.set('volume', String(volume));
  params.set('action', action);

  try{
    const r = await fetchWithTimeout('/api/alarm/save', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:params.toString()
    }, 3500);
    const j = await r.json();
    if(j && j.ok){
      alert('收音机闹钟已保存并启用');
      renderAlarmStatus(j);
      await refreshClockAlarmStatus();
    }else{
      alert((j && j.message) ? j.message : '闹钟保存失败');
    }
  }catch(e){
    alert('闹钟保存失败');
  }
}

async function disableAlarm(){
  if(!confirm('确认关闭闹钟？配置会保留，之后可以重新启用。')) return;
  try{
    const r = await fetchWithTimeout('/api/alarm/disable', {method:'POST'}, 3500);
    const j = await r.json();
    if(j && j.ok){
      alert('闹钟已关闭');
      renderAlarmStatus(j);
      await refreshClockAlarmStatus();
    }else{
      alert((j && j.message) ? j.message : '闹钟关闭失败');
    }
  }catch(e){
    alert('闹钟关闭失败');
  }
}

async function deleteAlarm(){
  if(!confirm('确认删除闹钟配置？删除后会同时关闭RTC闹钟。')) return;
  try{
    const r = await fetchWithTimeout('/api/alarm/delete', {method:'POST'}, 3500);
    const j = await r.json();
    if(j && j.ok){
      alert('闹钟已删除');
      renderAlarmStatus(j);
      await refreshClockAlarmStatus();
    }else{
      alert((j && j.message) ? j.message : '闹钟删除失败');
    }
  }catch(e){
    alert('闹钟删除失败');
  }
}

function openTfConfigSectionFromHash(){
  const targetId = String(location.hash || '').replace(/^#/, '');
  if(!['wifiConfigCard','radioConfigCard','nasConfigCard','systemAssetCard'].includes(targetId)) return;
  const center = document.getElementById('tfConfigCenterCard');
  const section = document.getElementById(targetId);
  if(center) center.open = true;
  if(section) section.open = true;
  requestAnimationFrame(()=>{ if(section) section.scrollIntoView({block:'start'}); });
}

openTfConfigSectionFromHash();
window.addEventListener('hashchange', openTfConfigSectionFromHash);

refreshClockAlarmStatus();
loadSettings();
loadWifiConfig(true);
loadRadioConfig(true);
loadNasConfig(true);
loadSystemAssets(true);
loadAudioOutputStatus(true);
refreshMusicScanStatus();

// RTC时间只定期向设备校准一次，页面显示由浏览器每秒递增，避免“当前时间”看起来不走。
setInterval(updateRtcClockDisplay, 1000);
setInterval(loadRtcStatus, 60000);
setInterval(()=>{
  if(!document.hidden){
    refreshMusicScanStatus();
    loadAudioOutputStatus(true);
    const diagnosticsCard = document.getElementById('systemDiagnosticsCard');
    if(diagnosticsCard && diagnosticsCard.open &&
       Date.now() - lastSystemDiagnosticsAt >= 5000){
      loadSystemDiagnostics(true);
    }
  }
}, 2000);
</script>
</body>
</html>
)HTML";


static const char WEBCTRL_ARTISTS_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <script src="/web-feedback.js" defer></script>
  <title>ESP32S3 歌手页</title>
  <style>
    body{
      font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
      margin:0;
      background:#111;
      color:#eee;
      height:100dvh;
      overflow:hidden;
    }
    .wrap{
      max-width:760px;
      margin:0 auto;
      padding:16px;
      height:100dvh;
      box-sizing:border-box;
      display:flex;
      flex-direction:column;
      min-height:0;
    }
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .top{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
    .actions{display:flex;gap:8px;flex-wrap:wrap}
    a,button{border:none;border-radius:12px;padding:10px 14px;background:#2f6feb;color:#fff;font-size:15px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    a.secondary,button.secondary{background:#444}
    input{width:100%;padding:12px 14px;border-radius:12px;border:1px solid #444;background:#111;color:#eee;box-sizing:border-box}
    .muted{color:#aaa;font-size:14px}
    .listCard{
      flex:1;
      min-height:0;
      display:flex;
      flex-direction:column;
    }
    .list{
      flex:1;
      min-height:0;
      overflow:auto;
      max-height:none;
    }
    .item{padding:12px;border:1px solid #2e2e2e;border-radius:12px;margin-bottom:8px;cursor:pointer;background:#151515}
    .item.active{border-color:#2f6feb;background:#16233d}
    .item.current{border-color:#2f6feb}
    .name{font-size:16px;font-weight:700}
    .sub{font-size:13px;color:#bdbdbd;margin-top:4px}
    .sectionTitle{font-size:20px;font-weight:800;margin:0 0 6px}
    .track{display:grid;grid-template-columns:auto 1fr auto;gap:10px;align-items:center;padding:10px 0;border-bottom:1px solid #2a2a2a}
    .track:last-child{border-bottom:none}
    .idx{font-size:13px;color:#aaa;min-width:28px}
    .trackTitle{font-size:15px;font-weight:700}
    .trackSub{font-size:12px;color:#aaa;margin-top:3px}
    .empty{padding:24px 10px;color:#aaa;text-align:center}
    .itemHead{display:flex;justify-content:space-between;gap:12px;align-items:center}
    .itemMeta{min-width:0;flex:1}
    .expandBox{margin-top:10px;padding-top:10px;border-top:1px solid #2a2a2a}
    .expandActions{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
    .expandEmpty{padding:12px 0;color:#aaa}
    @media (max-width:900px){
      .layout{grid-template-columns:1fr}
    }
    
    /* 悬浮回到顶部按钮 */
    .scrollToTopBtn{position:fixed;bottom:20px;right:20px;width:50px;height:50px;border-radius:50%;background:#2f6feb;color:#fff;border:none;font-size:24px;cursor:pointer;box-shadow:0 4px 12px rgba(0,0,0,.3);opacity:0;transform:translateY(20px);transition:opacity 0.3s,transform 0.3s;z-index:1000;display:flex;align-items:center;justify-content:center}
    .scrollToTopBtn.visible{opacity:1;transform:translateY(0)}
    .scrollToTopBtn:hover{background:#1a5bd4}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card top">
      <div>
        <div class="sectionTitle">歌手页</div>
      </div>
      <div class="nav" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px">
        <a class="secondary" href="/">控制页</a>
        <a class="secondary" href="/albums">专辑页</a>
        <a class="secondary" href="/nfc">NFC管理</a>
        <a class="secondary" href="/radios">电台页</a>
        <a class="secondary" href="/netmusic">NAS页</a>
        <a class="secondary" href="/settings">网页设置</a>
      </div>
    </div>

    <div class="card">
      <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px">
        <button id="modeArtistBtn" type="button">搜歌手</button>
        <button id="modeSongBtn" class="secondary" type="button">搜歌名</button>
      </div>
      <input id="searchInput" placeholder="搜索歌手名">
    </div>

    <div class="card listCard">
      <div class="muted" id="countText">-</div>
      <div class="list" id="artistList"></div>
    </div>
  </div>
<script>
const $ = id => document.getElementById(id); 
 const esc = s => String(s ?? '').replace(/[&<>'"]/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m])); 
 async function postForm(url, obj){ const b=new URLSearchParams(); Object.keys(obj).forEach(k=>b.append(k,obj[k])); const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}); return r.json(); } 
 
  let allItems = [];
  let songSearchItems = [];
  let expandedIdx = -1;
  let detailCache = {};
  let currentTrackArtist = '';
  let initialScrollDone = false;

  function getDetailCacheKey(idx){
    const q = ($('searchInput').value || '').trim().toLowerCase();
    if(searchMode === 'song' && q){
      return `song:${idx}:${q}`;
    }
    return `normal:${idx}`;
  }
  let searchMode = 'artist';   // artist / song
  let songSearchTimer = 0;
  let artistSongSearchController = null; 
 
  function makeDetailState(idx, cacheKey){
    return {
      idx,
      cacheKey,
      name: '',
      track_count: 0,
      tracks: [],
      loaded: 0,
      done: false,
      loading: false
    };
  }

  function resetExpandedDetailState(){
    expandedIdx = -1;
    detailCache = {};
  }
 
 function renderArtistTracks(detail){ 
   const tracks = detail?.tracks || []; 
   let html = ''; 

   if(!tracks.length){ 
     html += `<div class="expandEmpty">${detail && detail.loading ? '加载中...' : '这一组里还没有歌曲'}</div>`; 
   }else{ 
     html += tracks.map((t,i)=>` 
       <div class="track"> 
         <div class="idx">${i+1}</div> 
         <div> 
           <div class="trackTitle">${esc(t.title||'未知标题')}</div> 
           <div class="trackSub">${esc(t.album||'-')}</div> 
         </div> 
         <div style="display:flex;gap:8px;flex-wrap:wrap">
           <button class="secondary" onclick="event.stopPropagation(); playTrack(${t.track_idx}, ${detail.idx})">播放</button>
           <button class="secondary" onclick="event.stopPropagation(); bindTrackNfc(${t.track_idx})">绑定NFC</button>
         </div> 
       </div> 
     `).join(''); 
   } 

   if(detail){ 
     if(detail.loading && tracks.length){ 
       html += `<div class="expandEmpty">正在加载更多...</div>`; 
     }else if(!detail.done){ 
       html += ` 
         <div class="expandActions"> 
           <button class="secondary" onclick="event.stopPropagation(); loadMoreArtist(${detail.idx})">加载更多</button> 
           <span class="muted">已加载 ${detail.loaded}/${detail.track_count||0}</span> 
         </div> 
       `; 
     }else if(detail.track_count > 0){ 
       html += `<div class="expandEmpty">已全部加载，共 ${detail.track_count} 首</div>`; 
     } 
   } 

   return html; 
 } 

  function updateSearchModeUi(){
    $('modeArtistBtn').className = searchMode === 'artist' ? '' : 'secondary';
    $('modeSongBtn').className = searchMode === 'song' ? '' : 'secondary';
    $('searchInput').placeholder = searchMode === 'artist' ? '搜索歌手名' : '搜索歌名';
  }

  function setSearchMode(mode){
    searchMode = mode;
    updateSearchModeUi();
    resetExpandedDetailState();

    if(artistSongSearchController){
      artistSongSearchController.abort();
      artistSongSearchController = null;
    }

    if(searchMode === 'song'){
      scheduleArtistSongSearch();
    }else{
      renderList();
    }
  }

  async function fetchArtistSongSearch(){
    const q = ($('searchInput').value || '').trim();

    if(artistSongSearchController){
      artistSongSearchController.abort();
      artistSongSearchController = null;
    }

    if(!q){
      songSearchItems = [];
      renderList();
      return;
    }

    const controller = new AbortController();
    artistSongSearchController = controller;

    try{
      const r = await fetch(`/api/artist/search_song?q=${encodeURIComponent(q)}`, {
        cache:'no-store',
        signal: controller.signal
      });
      const j = await r.json();
      if(!j.ok) throw new Error(j.message || 'search failed');

      if(artistSongSearchController !== controller) return;

      songSearchItems = j.items || [];
      renderList();
    }catch(e){
      if(e.name === 'AbortError') return;
      throw e;
    }finally{
      if(artistSongSearchController === controller){
        artistSongSearchController = null;
      }
    }
  }

  function scheduleArtistSongSearch(){
    if(songSearchTimer){
      clearTimeout(songSearchTimer);
      songSearchTimer = 0;
    }
    songSearchTimer = setTimeout(()=>{
      fetchArtistSongSearch().catch(e=>{
        alert(e.message || '搜索失败');
      });
    }, 220);
  }

    async function loadCurrentTrackArtist(){
      try{
        const r = await fetch('/api/status', {cache:'no-store'});
        const j = await r.json();
        currentTrackArtist = String(j.artist || '').trim();
      }catch(e){
        currentTrackArtist = '';
      }
    }

  function scrollToCurrentArtist(){
    if(initialScrollDone) return;
    if(!currentTrackArtist) return;
    if(searchMode !== 'artist') return;

    const idx = allItems.findIndex(x => String(x.name || '').trim() === currentTrackArtist);
    if(idx < 0) return;

    const box = $('artistList');
    const el = box.querySelector(`.item[data-idx="${idx}"]`);
    if(!box || !el) return;

    el.scrollIntoView({ behavior:'smooth', block:'center' });
    initialScrollDone = true;
  }

  function renderList(){
    const q = ($('searchInput').value || '').trim().toLowerCase();
    const box = $('artistList');

    let items = [];
    if(searchMode === 'artist'){
      items = allItems.filter(x => !q || (x.name || '').toLowerCase().includes(q));
      $('countText').textContent = `共 ${allItems.length} 位歌手，当前显示 ${items.length} 位`;
    }else{
      items = q ? songSearchItems : allItems;
      $('countText').textContent = q
        ? `按歌名命中 ${items.length} 位歌手`
        : `共 ${allItems.length} 位歌手`;
    }

    if(!items.length){
      box.innerHTML = `<div class="empty">${searchMode === 'song' ? '没有匹配的歌曲' : '没有匹配的歌手'}</div>`;
      return;
    }

    box.innerHTML = items.map(x => {
      const expanded = x.idx === expandedIdx;
      const current = searchMode === 'artist' &&
                      currentTrackArtist &&
                      String(x.name || '').trim() === currentTrackArtist;
      const detail = detailCache[getDetailCacheKey(x.idx)];

      let subText = `${x.track_count || 0} 首`;
      if(searchMode === 'song' && q){
        const tip = x.matched_titles_text ? ` · ${esc(x.matched_titles_text)}` : '';
        subText = `命中 ${x.matched_track_count || 0} 首${tip}`;
      }

      return `
        <div class="item ${expanded ? 'active' : ''} ${current ? 'current' : ''}" data-idx="${x.idx}" onclick="toggleArtist(${x.idx})"><div class="itemHead">
            <div class="itemMeta">
              <div class="name">${esc(x.name || '未知歌手')}</div>
              <div class="sub">${subText}</div>
            </div>
            <div class="muted">${expanded ? '▲ 收起' : '▼ 展开'}</div>
          </div>

          ${expanded ? `
            <div class="expandBox">
              <div class="expandActions">
                <button onclick="event.stopPropagation(); playGroup(${x.idx})" ${(x.track_count || 0) > 0 ? '' : 'disabled'}>播放这一组</button>
                <button class="secondary" onclick="event.stopPropagation(); bindArtistNfc(${x.idx})">绑定歌手到NFC</button>
              </div>
              ${detail ? renderArtistTracks(detail) : '<div class="expandEmpty">加载中...</div>'}
            </div>
          ` : ''}
        </div>
      `;
    }).join('');

    if(!q){
      setTimeout(scrollToCurrentArtist, 0);
    }
  } 

  async function loadArtists(){ 
    await loadCurrentTrackArtist();

    const r = await fetch('/api/artists', {cache:'no-store'}); 
    const j = await r.json(); 
    if(!j.ok) throw new Error(j.message || 'load failed'); 

    allItems = j.items || [];
    initialScrollDone = false;

    renderList(); 
  }

  async function loadDetail(idx, append){
    const cacheKey = getDetailCacheKey(idx);

    let state = detailCache[cacheKey];
    if(!state){
      state = makeDetailState(idx, cacheKey);
      detailCache[cacheKey] = state;
    }

    if(state.loading || state.done) return;

    state.loading = true;
    renderList();

    try{
      const offset = append ? state.loaded : 0;
      const limit = 20;

      const q = ($('searchInput').value || '').trim();
      let url = `/api/artist/detail?idx=${idx}&offset=${offset}&limit=${limit}`;

      if(searchMode === 'song' && q){
        url += `&q=${encodeURIComponent(q)}`;
      }

      const r = await fetch(url, {cache:'no-store'});
      const j = await r.json();
      if(!j.ok) throw new Error(j.message || 'detail failed');

      state.name = j.name || '';
      state.track_count = j.track_count || 0;

      if(append){
        state.tracks = state.tracks.concat(j.tracks || []);
      }else{
        state.tracks = j.tracks || [];
      }

      state.loaded = state.tracks.length;
      state.done = state.loaded >= state.track_count;
    } finally {
      state.loading = false;
      if(expandedIdx === idx) renderList();
    }
  }

 async function toggleArtist(idx){ 
   if(expandedIdx === idx){ 
     expandedIdx = -1; 
     renderList(); 
     return; 
   } 

   expandedIdx = idx; 
   renderList(); 

  const cacheKey = getDetailCacheKey(idx);

  if(!detailCache[cacheKey]){
    try{
      await loadDetail(idx, false);
    }catch(e){
      alert(e.message || '加载失败');
    }
  }
 } 

 async function loadMoreArtist(idx){ 
   try{ 
     await loadDetail(idx, true); 
   }catch(e){ 
     alert(e.message || '加载失败'); 
   } 
 } 

  async function playGroup(idx){ 
    const j = await postForm('/api/artist/play', {idx}); 
    alert(j && j.ok ? '已切到该歌手' : '播放失败'); 
  } 

  async function playTrack(trackIdx, groupIdx){ 
    const j = await postForm('/api/track/play', {idx:trackIdx}); 
    alert(j && j.ok ? '已开始播放' : '播放失败'); 
  }

 async function bindArtistNfc(idx){ 
   const j = await postForm('/api/artist/bind_nfc', {idx}); 
   alert(j && j.ok ? '请到设备前刷卡，并按播放键保存' : ((j && j.message) || '进入绑定失败')); 
 } 

 async function bindTrackNfc(trackIdx){ 
   const j = await postForm('/api/track/bind_nfc', {idx:trackIdx}); 
   alert(j && j.ok ? '请到设备前刷卡，并按播放键保存' : ((j && j.message) || '进入绑定失败')); 
 }

  $('modeArtistBtn').addEventListener('click', ()=>setSearchMode('artist'));
  $('modeSongBtn').addEventListener('click', ()=>setSearchMode('song'));

  $('searchInput').addEventListener('input', ()=>{
    resetExpandedDetailState();

    if(artistSongSearchController){
      artistSongSearchController.abort();
      artistSongSearchController = null;
    }

    if(searchMode === 'song') scheduleArtistSongSearch();
    else renderList();
  });

  updateSearchModeUi();
  loadArtists().catch(e=>{
    $('countText').textContent='加载失败';
    alert(e.message||'加载失败');
  });
 
 // 悬浮回到顶部按钮功能，对歌手列表生效
 const scrollToTopBtn = document.createElement('button');
  scrollToTopBtn.className = 'scrollToTopBtn';
  scrollToTopBtn.innerHTML = '↑';
  scrollToTopBtn.title = '回到顶部';

  function getArtistScrollTarget(){
    return $('artistList') || window;
  }

  function updateArtistScrollBtn(){
    const target = getArtistScrollTarget();
    const scrollTop = target === window
      ? (window.scrollY || document.documentElement.scrollTop || 0)
      : target.scrollTop;

    if (scrollTop > 300) {
      scrollToTopBtn.classList.add('visible');
    } else {
      scrollToTopBtn.classList.remove('visible');
    }
  }

  scrollToTopBtn.onclick = () => {
    const target = getArtistScrollTarget();
    if (target === window) {
      window.scrollTo({ top: 0, behavior: 'smooth' });
    } else {
      target.scrollTo({ top: 0, behavior: 'smooth' });
    }
  };

  document.body.appendChild(scrollToTopBtn);

  window.addEventListener('scroll', updateArtistScrollBtn);
  $('artistList').addEventListener('scroll', updateArtistScrollBtn);
</script>
</body>
</html>
)HTML";

static const char WEBCTRL_ALBUMS_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <script src="/web-feedback.js" defer></script>
  <title>ESP32S3 专辑页</title>
  <style>
    body{
      font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
      margin:0;
      background:#111;
      color:#eee;
      height:100dvh;
      overflow:hidden;
    }
    .wrap{
      max-width:760px;
      margin:0 auto;
      padding:16px;
      height:100dvh;
      box-sizing:border-box;
      display:flex;
      flex-direction:column;
      min-height:0;
    }
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .top{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
    .actions{display:flex;gap:8px;flex-wrap:wrap}
    a,button{border:none;border-radius:12px;padding:10px 14px;background:#2f6feb;color:#fff;font-size:15px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    a.secondary,button.secondary{background:#444}
    input{width:100%;padding:12px 14px;border-radius:12px;border:1px solid #444;background:#111;color:#eee;box-sizing:border-box}
    .muted{color:#aaa;font-size:14px}
    .listCard{
      flex:1;
      min-height:0;
      display:flex;
      flex-direction:column;
    }
    .list{
      flex:1;
      min-height:0;
      overflow:auto;
      max-height:none;
    }
    .item{padding:12px;border:1px solid #2e2e2e;border-radius:12px;margin-bottom:8px;cursor:pointer;background:#151515}
    .item.active{border-color:#2f6feb;background:#16233d}
    .item.current{border-color:#2f6feb}
    .name{font-size:16px;font-weight:700}
    .sub{font-size:13px;color:#bdbdbd;margin-top:4px}
    .sectionTitle{font-size:20px;font-weight:800;margin:0 0 6px}
    .track{display:grid;grid-template-columns:auto 1fr auto;gap:10px;align-items:center;padding:10px 0;border-bottom:1px solid #2a2a2a}
    .track:last-child{border-bottom:none}
    .idx{font-size:13px;color:#aaa;min-width:28px}
    .trackTitle{font-size:15px;font-weight:700}
    .trackSub{font-size:12px;color:#aaa;margin-top:3px}
    .empty{padding:24px 10px;color:#aaa;text-align:center}
    .itemHead{display:flex;justify-content:space-between;gap:12px;align-items:center}
    .itemMeta{min-width:0;flex:1}
    .expandBox{margin-top:10px;padding-top:10px;border-top:1px solid #2a2a2a}
    .expandActions{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
    .expandEmpty{padding:12px 0;color:#aaa}
    @media (max-width:900px){
      .layout{grid-template-columns:1fr}
    }
    
    /* 悬浮回到顶部按钮 */
    .scrollToTopBtn{position:fixed;bottom:20px;right:20px;width:50px;height:50px;border-radius:50%;background:#2f6feb;color:#fff;border:none;font-size:24px;cursor:pointer;box-shadow:0 4px 12px rgba(0,0,0,.3);opacity:0;transform:translateY(20px);transition:opacity 0.3s,transform 0.3s;z-index:1000;display:flex;align-items:center;justify-content:center}
    .scrollToTopBtn.visible{opacity:1;transform:translateY(0)}
    .scrollToTopBtn:hover{background:#1a5bd4}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card top">
      <div>
        <div class="sectionTitle">专辑页</div>
      </div>
      <div class="nav" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px">
        <a class="secondary" href="/">控制页</a>
        <a class="secondary" href="/artists">歌手页</a>
        <a class="secondary" href="/nfc">NFC管理</a>
        <a class="secondary" href="/radios">电台页</a>
        <a class="secondary" href="/netmusic">NAS页</a>
        <a class="secondary" href="/settings">网页设置</a>
      </div>
    </div>

    <div class="card">
      <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px">
        <button id="modeMetaBtn" type="button">搜专辑、歌手</button>
        <button id="modeSongBtn" class="secondary" type="button">搜歌名</button>
      </div>
      <input id="searchInput" placeholder="搜索 专辑名 / 歌手名">
    </div>

    <div class="card listCard">
      <div class="muted" id="countText">-</div>
      <div class="list" id="albumList"></div>
    </div>
  </div>
<script>
const $ = id => document.getElementById(id); 
 const esc = s => String(s ?? '').replace(/[&<>'"]/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m])); 
 async function postForm(url, obj){ const b=new URLSearchParams(); Object.keys(obj).forEach(k=>b.append(k,obj[k])); const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}); return r.json(); } 
 
  let allItems = [];
  let songSearchItems = [];
  let expandedIdx = -1;
  let detailCache = {};
  let currentTrackAlbum = '';
  let currentTrackArtist = '';
  let initialScrollDone = false;

  function getDetailCacheKey(idx){
    const q = ($('searchInput').value || '').trim().toLowerCase();
    if(searchMode === 'song' && q){
      return `song:${idx}:${q}`;
    }
    return `normal:${idx}`;
  }
  let searchMode = 'meta';   // meta / song
  let songSearchTimer = 0;
  let albumSongSearchController = null;
 
  function makeDetailState(idx, cacheKey){ 
    return { 
      idx, 
      cacheKey, 
      name: '', 
      track_count: 0, 
      tracks: [], 
      loaded: 0, 
      done: false, 
      loading: false 
    }; 
  }

  function resetExpandedDetailState(){
    expandedIdx = -1;
    detailCache = {};
  }
 
 function renderAlbumTracks(detail){ 
   const tracks = detail?.tracks || []; 
   let html = ''; 

   if(!tracks.length){ 
     html += `<div class="expandEmpty">${detail && detail.loading ? '加载中...' : '这一组里还没有歌曲'}</div>`; 
   }else{ 
     html += tracks.map((t,i)=>` 
       <div class="track"> 
         <div class="idx">${i+1}</div> 
         <div> 
           <div class="trackTitle">${esc(t.title||'未知标题')}</div> 
           <div class="trackSub">${esc(t.artist||'-')}</div> 
         </div> 
            <div style="display:flex;gap:8px;flex-wrap:wrap">
            <button class="secondary" onclick="event.stopPropagation(); playTrack(${t.track_idx}, ${detail.idx})">播放</button>
            <button class="secondary" onclick="event.stopPropagation(); bindTrackNfc(${t.track_idx})">绑定NFC</button>
          </div> 
       </div> 
     `).join(''); 
   } 

   if(detail){ 
     if(detail.loading && tracks.length){ 
       html += `<div class="expandEmpty">正在加载更多...</div>`; 
     }else if(!detail.done){ 
       html += ` 
         <div class="expandActions"> 
           <button class="secondary" onclick="event.stopPropagation(); loadMoreAlbum(${detail.idx})">加载更多</button> 
           <span class="muted">已加载 ${detail.loaded}/${detail.track_count||0}</span> 
         </div> 
       `; 
     }else if(detail.track_count > 0){ 
       html += `<div class="expandEmpty">已全部加载，共 ${detail.track_count} 首</div>`; 
     } 
   } 

   return html; 
 } 

  function updateSearchModeUi(){
    $('modeMetaBtn').className = searchMode === 'meta' ? '' : 'secondary';
    $('modeSongBtn').className = searchMode === 'song' ? '' : 'secondary';
    $('searchInput').placeholder = searchMode === 'meta' ? '搜索 专辑名 / 歌手名' : '搜索歌名';
  }   

  function setSearchMode(mode){
    searchMode = mode;
    updateSearchModeUi();
    resetExpandedDetailState();

    if(albumSongSearchController){
      albumSongSearchController.abort();
      albumSongSearchController = null;
    }

    if(searchMode === 'song'){
      scheduleAlbumSongSearch();
    }else{
      renderList();
    }
  }
  async function fetchAlbumSongSearch(){
    const q = ($('searchInput').value || '').trim();

    if(albumSongSearchController){
      albumSongSearchController.abort();
      albumSongSearchController = null;
    }

    if(!q){
      songSearchItems = [];
      renderList();
      return;
    }

    const controller = new AbortController();
    albumSongSearchController = controller;

    try{
      const r = await fetch(`/api/album/search_song?q=${encodeURIComponent(q)}`, {
        cache:'no-store',
        signal: controller.signal
      });
      const j = await r.json();
      if(!j.ok) throw new Error(j.message || 'search failed');

      if(albumSongSearchController !== controller) return;

      songSearchItems = j.items || [];
      renderList();
    }catch(e){
      if(e.name === 'AbortError') return;
      throw e;
    }finally{
      if(albumSongSearchController === controller){
        albumSongSearchController = null;
      }
    }
  }

  function scheduleAlbumSongSearch(){
    if(songSearchTimer){
      clearTimeout(songSearchTimer);
      songSearchTimer = 0;
    }
    songSearchTimer = setTimeout(()=>{
      fetchAlbumSongSearch().catch(e=>{
        alert(e.message || '搜索失败');
      });
    }, 220);
  }

  async function loadCurrentTrackAlbumInfo(){
    try{
      const r = await fetch('/api/status', {cache:'no-store'});
      const j = await r.json();
      currentTrackAlbum = String(j.album || '').trim();
      currentTrackArtist = String(j.artist || '').trim();
    }catch(e){
      currentTrackAlbum = '';
      currentTrackArtist = '';
    }
  }

  function scrollToCurrentAlbum(){
    if(initialScrollDone) return;
    if(!currentTrackAlbum) return;
    if(searchMode !== 'meta') return;

    const idx = allItems.findIndex(x =>
      String(x.name || '').trim() === currentTrackAlbum &&
      String(x.primary_artist || '').trim() === currentTrackArtist
    );
    if(idx < 0) return;

    const box = $('albumList');
    const el = box.querySelector(`.item[data-idx="${idx}"]`);
    if(!box || !el) return;

    el.scrollIntoView({ behavior:'smooth', block:'center' });
    initialScrollDone = true;
  }

  function renderList(){ 
    const q = ($('searchInput').value || '').trim().toLowerCase(); 
    const box = $('albumList'); 

    let items = [];
    if(searchMode === 'meta'){
      items = allItems.filter(x => !q || (x.name || '').toLowerCase().includes(q) || (x.primary_artist || '').toLowerCase().includes(q));
      $('countText').textContent = `共 ${allItems.length} 张专辑，当前显示 ${items.length} 张`;
    }else{
      items = q ? songSearchItems : allItems;
      $('countText').textContent = q
        ? `按歌名命中 ${items.length} 张专辑`
        : `共 ${allItems.length} 张专辑`;
    }

    if(!items.length){ 
      box.innerHTML = `<div class="empty">${searchMode === 'song' ? '没有匹配的歌曲' : '没有匹配的专辑'}</div>`; 
      return; 
    } 

    box.innerHTML = items.map(x => { 
      const expanded = x.idx === expandedIdx;
      const current = searchMode === 'meta' &&
                      currentTrackAlbum &&
                      String(x.name || '').trim() === currentTrackAlbum &&
                      String(x.primary_artist || '').trim() === currentTrackArtist;
      const detail = detailCache[getDetailCacheKey(x.idx)]; 

      let subText = `${esc(x.primary_artist || '未知歌手')} · ${x.track_count || 0} 首`;
      if(searchMode === 'song' && q){
        const tip = x.matched_titles_text ? ` · ${esc(x.matched_titles_text)}` : '';
        subText = `${esc(x.primary_artist || '未知歌手')} · 命中 ${x.matched_track_count || 0} 首${tip}`;
      }

      return ` 
        <div class="item ${expanded ? 'active' : ''} ${current ? 'current' : ''}" data-idx="${x.idx}" onclick="toggleAlbum(${x.idx})"><div class="itemHead">
            <div class="itemMeta">
              <div class="name">${esc(x.name || '未知专辑')}</div>
              <div class="sub">${subText}</div>
            </div>
            <div class="muted">${expanded ? '▲ 收起' : '▼ 展开'}</div>
          </div>

          ${expanded ? `
            <div class="expandBox">
              <div class="expandActions">
                <button onclick="event.stopPropagation(); playGroup(${x.idx})" ${(x.track_count || 0) > 0 ? '' : 'disabled'}>播放这一组</button>
                <button class="secondary" onclick="event.stopPropagation(); bindAlbumNfc(${x.idx})">绑定专辑到NFC</button>
              </div>
              ${detail ? renderAlbumTracks(detail) : '<div class="expandEmpty">加载中...</div>'}
            </div>
          ` : ''}
        </div>
      `; 
    }).join(''); 

    if(!q){
      setTimeout(scrollToCurrentAlbum, 0);
    }
  } 

  async function loadAlbums(){ 
    await loadCurrentTrackAlbumInfo();

    const r = await fetch('/api/albums', {cache:'no-store'}); 
    const j = await r.json(); 
    if(!j.ok) throw new Error(j.message || 'load failed'); 

    allItems = j.items || [];
    initialScrollDone = false;

    renderList(); 
  }

  async function loadDetail(idx, append){
    const cacheKey = getDetailCacheKey(idx);

    let state = detailCache[cacheKey];
    if(!state){
      state = makeDetailState(idx, cacheKey);
      detailCache[cacheKey] = state;
    }

    if(state.loading || state.done) return;

    state.loading = true;
    renderList();

    try{
      const offset = append ? state.loaded : 0;
      const limit = 20;

      const q = ($('searchInput').value || '').trim();
      let url = `/api/album/detail?idx=${idx}&offset=${offset}&limit=${limit}`;

      if(searchMode === 'song' && q){
        url += `&q=${encodeURIComponent(q)}`;
      }

      const r = await fetch(url, {cache:'no-store'});
      const j = await r.json();
      if(!j.ok) throw new Error(j.message || 'detail failed');

      state.name = j.name || '';
      state.track_count = j.track_count || 0;

      if(append){
        state.tracks = state.tracks.concat(j.tracks || []);
      }else{
        state.tracks = j.tracks || [];
      }

      state.loaded = state.tracks.length;
      state.done = state.loaded >= state.track_count;
    } finally {
      state.loading = false;
      if(expandedIdx === idx) renderList();
    }
  } 

 async function toggleAlbum(idx){ 
   if(expandedIdx === idx){ 
     expandedIdx = -1; 
     renderList(); 
     return; 
   } 

   expandedIdx = idx; 
   renderList(); 

  const cacheKey = getDetailCacheKey(idx);

  if(!detailCache[cacheKey]){
    try{
      await loadDetail(idx, false);
    }catch(e){
      alert(e.message || '加载失败');
    }
  }
 } 

 async function loadMoreAlbum(idx){ 
   try{ 
     await loadDetail(idx, true); 
   }catch(e){ 
     alert(e.message || '加载失败'); 
   } 
 } 

  async function playGroup(idx){ 
    const j = await postForm('/api/album/play', {idx}); 
    alert(j && j.ok ? '已切到该专辑' : '播放失败'); 
  } 

  async function playTrack(trackIdx, groupIdx){ 
    const j = await postForm('/api/track/play', {idx:trackIdx}); 
    alert(j && j.ok ? '已开始播放' : '播放失败'); 
  }

 async function bindAlbumNfc(idx){ 
   const j = await postForm('/api/album/bind_nfc', {idx}); 
   alert(j && j.ok ? '请到设备前刷卡，并按播放键保存' : ((j && j.message) || '进入绑定失败')); 
 } 

 async function bindTrackNfc(trackIdx){ 
   const j = await postForm('/api/track/bind_nfc', {idx:trackIdx}); 
   alert(j && j.ok ? '请到设备前刷卡，并按播放键保存' : ((j && j.message) || '进入绑定失败')); 
 } 

  $('modeMetaBtn').addEventListener('click', ()=>setSearchMode('meta'));
  $('modeSongBtn').addEventListener('click', ()=>setSearchMode('song'));

  $('searchInput').addEventListener('input', ()=>{
    resetExpandedDetailState();

    if(albumSongSearchController){
      albumSongSearchController.abort();
      albumSongSearchController = null;
    }

    if(searchMode === 'song') scheduleAlbumSongSearch();
    else renderList();
  });

  updateSearchModeUi();
  loadAlbums().catch(e=>{ $('countText').textContent='加载失败'; alert(e.message||'加载失败'); });
 
 // 悬浮回到顶部按钮功能，对专辑列表和窗口都生效
  const scrollToTopBtn = document.createElement('button');
  scrollToTopBtn.className = 'scrollToTopBtn';
  scrollToTopBtn.innerHTML = '↑';
  scrollToTopBtn.title = '回到顶部';

  function getAlbumScrollTarget(){
    return $('albumList') || window;
  }

  function updateAlbumScrollBtn(){
    const target = getAlbumScrollTarget();
    const scrollTop = target === window
      ? (window.scrollY || document.documentElement.scrollTop || 0)
      : target.scrollTop;

    if (scrollTop > 300) {
      scrollToTopBtn.classList.add('visible');
    } else {
      scrollToTopBtn.classList.remove('visible');
    }
  }

  scrollToTopBtn.onclick = () => {
    const target = getAlbumScrollTarget();
    if (target === window) {
      window.scrollTo({ top: 0, behavior: 'smooth' });
    } else {
      target.scrollTo({ top: 0, behavior: 'smooth' });
    }
  };

  document.body.appendChild(scrollToTopBtn);

  window.addEventListener('scroll', updateAlbumScrollBtn);
  $('albumList').addEventListener('scroll', updateAlbumScrollBtn);
</script>
</body>
</html>
)HTML";

static const char WEBCTRL_NFC_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <script src="/web-feedback.js" defer></script>
  <title>ESP32S3 NFC管理</title>
  <style>
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .top{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
    .actions{display:flex;gap:8px;flex-wrap:wrap}
    a,button{border:none;border-radius:12px;padding:10px 14px;background:#2f6feb;color:#fff;font-size:15px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    a.secondary,button.secondary{background:#444}
    button.warn{background:#a04040}
    input{width:100%;padding:12px 14px;border-radius:12px;border:1px solid #444;background:#111;color:#eee;box-sizing:border-box}
    .muted{color:#aaa;font-size:14px}
    .sectionTitle{font-size:20px;font-weight:800;margin:0 0 6px}
    .toolbar{display:flex;gap:8px;flex-wrap:wrap}
    .chipRow{display:flex;gap:8px;flex-wrap:wrap}
    .chip{padding:8px 12px;border-radius:999px;background:#2c2c2c;color:#ddd;cursor:pointer;border:none;font-size:14px}
    .chip.active{background:#2f6feb;color:#fff}
    .list{display:flex;flex-direction:column;gap:10px}
    details.item{padding:0;border:1px solid #2e2e2e;border-radius:14px;background:#151515;overflow:hidden}
    details.item>summary{display:flex;justify-content:space-between;gap:12px;align-items:center;padding:14px;cursor:pointer;list-style:none;user-select:none}
    details.item>summary::-webkit-details-marker{display:none}
    details.item>summary::after{content:'›';flex:0 0 auto;font-size:24px;color:#999;transform:rotate(0deg);transition:transform .16s ease}
    details.item[open]>summary{border-bottom:1px solid #2e2e2e}
    details.item[open]>summary::after{transform:rotate(90deg)}
    .itemSummaryMain{min-width:0;display:flex;align-items:center;gap:10px;flex:1}
    .itemSummaryText{min-width:0;flex:1}
    .itemSummaryName{font-size:17px;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
    .itemSummaryMeta{font-size:12px;color:#999;margin-top:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
    .itemDetails{padding:14px}
    .detailRow{display:grid;grid-template-columns:76px minmax(0,1fr);gap:10px;margin-bottom:10px;align-items:start}
    .detailLabel{font-size:13px;color:#999}
    .detailValue{font-size:13px;color:#ddd;word-break:break-all}
    .badge{display:inline-flex;align-items:center;justify-content:center;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:700;flex:0 0 auto}
    .badge.track{background:#224a8a;color:#cfe3ff}
    .badge.artist{background:#245b3d;color:#d5ffe6}
    .badge.album{background:#5a3978;color:#f0dcff}
    .rowActions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
    .empty{padding:30px 12px;text-align:center;color:#999}
    .summary{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
    .small{font-size:12px;color:#aaa}
    
    /* 悬浮回到顶部按钮 */
    .scrollToTopBtn{position:fixed;bottom:20px;right:20px;width:50px;height:50px;border-radius:50%;background:#2f6feb;color:#fff;border:none;font-size:24px;cursor:pointer;box-shadow:0 4px 12px rgba(0,0,0,.3);opacity:0;transform:translateY(20px);transition:opacity 0.3s,transform 0.3s;z-index:1000;display:flex;align-items:center;justify-content:center}
    .scrollToTopBtn.visible{opacity:1;transform:translateY(0)}
    .scrollToTopBtn:hover{background:#1a5bd4}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card top">
      <div>
        <div class="sectionTitle">NFC 绑定管理</div>
      </div>
      <div class="actions">
        <a class="secondary" href="/">控制页</a>
        <a class="secondary" href="/artists">歌手页</a>
        <a class="secondary" href="/albums">专辑页</a>
        <a class="secondary" href="/netmusic">NAS页</a>
        <a class="secondary" href="/settings">网页设置</a>
        <button onclick="loadBindings()">刷新</button>
      </div>
    </div>

    <div class="card">
      <div class="chipRow" id="filterRow">
        <button class="chip active" data-type="all" onclick="setFilter('all')">全部</button>
        <button class="chip" data-type="track" onclick="setFilter('track')">单曲</button>
        <button class="chip" data-type="artist" onclick="setFilter('artist')">歌手</button>
        <button class="chip" data-type="album" onclick="setFilter('album')">专辑</button>
      </div>
      <div style="margin-top:12px">
        <input id="searchInput" placeholder="搜索 UID / 名称 / key">
      </div>
    </div>

    <div class="card">
      <div class="summary">
        <div class="muted" id="countText">-</div>
      </div>
      <div class="list" id="bindingList" style="margin-top:12px"></div>
    </div>
  </div>

<script>
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>'"]/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
let allBindings = [];
let currentFilter = 'all';
const openBindingUids = new Set();

function encodedUid(uid){
  return encodeURIComponent(String(uid || ''));
}
function shortUid(uid){
  const text = String(uid || '');
  if(text.length <= 18) return text || '-';
  return `${text.slice(0, 8)}…${text.slice(-8)}`;
}
function bindingTargetLabel(type){
  if(type === 'track') return '歌曲路径';
  if(type === 'artist') return '歌手名称';
  if(type === 'album') return '专辑名称';
  return '目标键值';
}

function bindTypeLabel(type){
  if(type === 'track') return '单曲';
  if(type === 'artist') return '歌手';
  if(type === 'album') return '专辑';
  return '未知';
}
function bindTypeClass(type){
  if(type === 'track') return 'track';
  if(type === 'artist') return 'artist';
  if(type === 'album') return 'album';
  return 'track';
}
function setFilter(type){
  currentFilter = type;
  document.querySelectorAll('#filterRow .chip').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.type === type);
  });
  renderBindings();
}
function matchSearch(x, q){
  if(!q) return true;
  const text = [
    x.uid || '',
    x.display || '',
    x.key || '',
    x.type_label || ''
  ].join(' ').toLowerCase();
  return text.includes(q);
}
function renderBindings(){
  const q = ($('searchInput').value || '').trim().toLowerCase();
  const items = allBindings.filter(x => {
    const passType = currentFilter === 'all' || x.type === currentFilter;
    return passType && matchSearch(x, q);
  });

  $('countText').textContent = `共 ${allBindings.length} 条绑定，当前显示 ${items.length} 条`;

  const box = $('bindingList');
  if(!items.length){
    box.innerHTML = '<div class="empty">没有匹配的绑定</div>';
    return;
  }

  box.innerHTML = items.map(x => {
    const uid = String(x.uid || '');
    const uidToken = encodedUid(uid);
    const isOpen = openBindingUids.has(uid);
    return `
      <details class="item" data-binding-uid="${esc(uidToken)}"${isOpen ? ' open' : ''}>
        <summary>
          <div class="itemSummaryMain">
            <span class="badge ${bindTypeClass(x.type)}">${esc(bindTypeLabel(x.type))}</span>
            <div class="itemSummaryText">
              <div class="itemSummaryName">${esc(x.display || '-')}</div>
              <div class="itemSummaryMeta">#${Number(x.index || 0) || '-'} · UID ${esc(shortUid(uid))}</div>
            </div>
          </div>
        </summary>
        <div class="itemDetails">
          <div class="detailRow"><div class="detailLabel">绑定序号</div><div class="detailValue">#${Number(x.index || 0) || '-'}</div></div>
          <div class="detailRow"><div class="detailLabel">绑定类型</div><div class="detailValue">${esc(bindTypeLabel(x.type))}</div></div>
          <div class="detailRow"><div class="detailLabel">完整 UID</div><div class="detailValue">${esc(uid || '-')}</div></div>
          <div class="detailRow"><div class="detailLabel">显示名称</div><div class="detailValue">${esc(x.display || '-')}</div></div>
          <div class="detailRow"><div class="detailLabel">${esc(bindingTargetLabel(x.type))}</div><div class="detailValue">${esc(x.key || '-')}</div></div>
          <div class="rowActions">
            <button class="secondary" data-binding-action="test" data-binding-uid="${esc(uidToken)}">测试播放</button>
            <button class="warn" data-binding-action="delete" data-binding-uid="${esc(uidToken)}">删除绑定</button>
          </div>
        </div>
      </details>`;
  }).join('');
}

async function loadBindings(){
  $('countText').textContent = '加载中...';
  try{
    const r = await fetch('/api/nfc/bindings', {cache:'no-store'});
    const j = await r.json();
    if(!j.ok) throw new Error(j.message || '加载失败');
    allBindings = j.items || [];
    renderBindings();
  }catch(e){
    $('countText').textContent = '加载失败';
    alert(e.message || '加载失败');
  }
}

async function deleteBinding(uid, display){
  if(!confirm(`确认删除这条绑定？\\n\\n${display || uid}`)) return;
  try{
    const b = new URLSearchParams();
    b.append('uid', uid);
    const r = await fetch('/api/nfc/binding/delete', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:b.toString()
    });
    const j = await r.json();
    if(!j.ok) throw new Error(j.message || '删除失败');
    await loadBindings();
  }catch(e){
    alert(e.message || '删除失败');
  }
}

async function testPlay(uid){
  try{
    const b = new URLSearchParams();
    b.append('uid', uid);
    const r = await fetch('/api/nfc/binding/test_play', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:b.toString()
    });
    const j = await r.json();
    alert(j && j.ok ? (j.message || '已触发播放') : ((j && j.message) || '测试播放失败'));
  }catch(e){
    alert('测试播放失败');
  }
}

$('bindingList').addEventListener('toggle', event => {
  const details = event.target.closest('details[data-binding-uid]');
  if(!details) return;
  const uid = decodeURIComponent(details.dataset.bindingUid || '');
  if(details.open) openBindingUids.add(uid);
  else openBindingUids.delete(uid);
}, true);

$('bindingList').addEventListener('click', event => {
  const button = event.target.closest('button[data-binding-action]');
  if(!button) return;
  event.preventDefault();
  event.stopPropagation();
  const uid = decodeURIComponent(button.dataset.bindingUid || '');
  if(button.dataset.bindingAction === 'test'){
    testPlay(uid);
    return;
  }
  if(button.dataset.bindingAction === 'delete'){
    const entry = allBindings.find(x => String(x.uid || '') === uid);
    deleteBinding(uid, entry ? entry.display : '');
  }
});

$('searchInput').addEventListener('input', renderBindings);
loadBindings();

// 悬浮回到顶部按钮功能
const scrollToTopBtn = document.createElement('button');
scrollToTopBtn.className = 'scrollToTopBtn';
scrollToTopBtn.innerHTML = '↑';
scrollToTopBtn.title = '回到顶部';
scrollToTopBtn.onclick = () => window.scrollTo({top:0,behavior:'smooth'});
document.body.appendChild(scrollToTopBtn);

// 滚动检测
window.addEventListener('scroll', () => {
  const scrollY = window.scrollY;
  if (scrollY > 300) {
    scrollToTopBtn.classList.add('visible');
  } else {
    scrollToTopBtn.classList.remove('visible');
  }
});
</script>
</body>
</html>
)HTML";

static const char WEBCTRL_RADIOS_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <script src="/web-feedback.js" defer></script>
  <title>ESP32S3 电台页</title>
  <style>
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .topbar{display:flex;gap:8px;flex-wrap:wrap;align-items:center;justify-content:space-between}
    .nav{display:flex;gap:8px;flex-wrap:wrap}
    a,button{border:none;border-radius:12px;padding:10px 12px;background:#2f6feb;color:#fff;font-size:14px;font-weight:600;text-decoration:none}
    a.secondary,button.secondary{background:#444}
    .muted{color:#aaa;font-size:13px}
    .list{display:grid;gap:10px}
    .item{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:center;padding:12px;border-radius:12px;background:#161616;border:1px solid #2a2a2a}
    .name{font-size:16px;font-weight:700}.meta{font-size:12px;color:#aaa;margin-top:4px}
    .err{color:#ff8f8f;font-size:13px;white-space:pre-wrap}
    
    /* 悬浮回到顶部按钮 */
    .scrollToTopBtn{position:fixed;bottom:20px;right:20px;width:50px;height:50px;border-radius:50%;background:#2f6feb;color:#fff;border:none;font-size:24px;cursor:pointer;box-shadow:0 4px 12px rgba(0,0,0,.3);opacity:0;transform:translateY(20px);transition:opacity 0.3s,transform 0.3s;z-index:1000;display:flex;align-items:center;justify-content:center}
    .scrollToTopBtn.visible{opacity:1;transform:translateY(0)}
    .scrollToTopBtn:hover{background:#1a5bd4}
  </style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div class="topbar">
      <div>
        <div style="font-size:22px;font-weight:800">网络电台</div>
      </div>
      <div class="nav" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px">
        <a class="secondary" href="/">控制页</a>
        <a class="secondary" href="/artists">歌手页</a>
        <a class="secondary" href="/albums">专辑页</a>
        <a class="secondary" href="/settings">网页设置</a>
        <a class="secondary" href="/settings#radioConfigCard">管理电台</a>
      </div>
    </div>
  </div>
  <div class="card">
    <div id="statusText">加载中...</div>
    <div id="err" class="err"></div>
  </div>
  <div class="card">
    <div class="muted" id="pathInfo">-</div>
    <div class="list" id="radioList"></div>
  </div>
</div>
<script>
async function loadRadios(){
  try{
    const r = await fetch('/api/radios',{cache:'no-store'});
    const j = await r.json();
    document.getElementById('pathInfo').textContent = `列表：${j.path||'-'} / 共 ${j.total||0} 项`;
    document.getElementById('err').textContent = j.ok ? '' : `加载提示：${j.error||'unknown'}`;
    const box = document.getElementById('radioList');
    box.innerHTML = '';
    (j.items||[]).forEach(it=>{
      const row = document.createElement('div'); row.className='item';
      row.innerHTML = `<div><div class="name">${it.name||'-'}</div><div class="meta">${it.format||'-'} · ${it.region||'-'}</div></div>`;
      const btn = document.createElement('button'); btn.textContent='选择';
      btn.onclick = async()=>{
        const resp = await fetch(`/api/radio/play?idx=${it.idx}`, {method:'POST'});
        const j = await resp.json();
        alert(j && j.ok ? (j.message || '已开始播放电台') : (j.message || '操作失败'));
      };
      row.appendChild(btn); box.appendChild(row);
    });
  }catch(e){ document.getElementById('err').textContent='电台列表获取失败'; }
}
async function loadStatus(){
  try{
    const r = await fetch('/api/status',{cache:'no-store'});
    const j = await r.json();
    let t = '当前源：-';
    if (j.source_type === 'radio') {
      t = `当前源：电台 / ${j.radio_name||'-'}`;
      if (j.radio_state) t += ` / ${j.radio_state}`;
      if (j.radio_backend) t += ` / ${j.radio_backend}`;
      if (j.radio_bitrate) t += ` / ${j.radio_bitrate}kbps`;
      if (j.radio_stream_title) t += ` / ${j.radio_stream_title}`;
    } else {
      t = `当前源：${j.source_type||'-'}`;
    }
    document.getElementById('statusText').textContent = t;
    if(j.radio_error){ document.getElementById('err').textContent = j.radio_error; }
  }catch(e){}
}

loadRadios(); loadStatus();

// 悬浮回到顶部按钮功能
const scrollToTopBtn = document.createElement('button');
scrollToTopBtn.className = 'scrollToTopBtn';
scrollToTopBtn.innerHTML = '↑';
scrollToTopBtn.title = '回到顶部';
scrollToTopBtn.onclick = () => window.scrollTo({top:0,behavior:'smooth'});
document.body.appendChild(scrollToTopBtn);

// 滚动检测
window.addEventListener('scroll', () => {
  const scrollY = window.scrollY;
  if (scrollY > 300) {
    scrollToTopBtn.classList.add('visible');
  } else {
    scrollToTopBtn.classList.remove('visible');
  }
});
</script>
</body>
</html>
)HTML";

static const char WEBCTRL_NETMUSIC_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <script src="/web-feedback.js" defer></script>
  <title>ESP32S3 NAS音乐页</title>
  <style>
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .topbar{display:flex;gap:8px;flex-wrap:wrap;align-items:center;justify-content:space-between}
    .nav{display:flex;gap:8px;flex-wrap:wrap}
    a,button{border:none;border-radius:12px;padding:10px 12px;background:#2f6feb;color:#fff;font-size:14px;font-weight:600;text-decoration:none}
    a.secondary,button.secondary{background:#444}
    button:disabled{opacity:.45}
    .muted{color:#aaa;font-size:13px}
    .err{color:#ff8f8f;font-size:13px;white-space:pre-wrap}
    .list{display:grid;gap:10px}
    .item{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:center;padding:10px 12px;border-radius:12px;background:#161616;border:1px solid #2a2a2a}
    .item.active{border-color:#2f6feb;background:#182235}
    .item.focused{border-color:#b8860b;background:#2a2415}
    .name{font-size:15px;font-weight:700;word-break:break-word;line-height:1.35}
    .meta{font-size:12px;color:#999;margin-top:4px;word-break:break-word;line-height:1.3}
    .pager{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px}
    select{background:#222;color:#eee;border:1px solid #444;border-radius:10px;padding:9px}
    .scrollToTopBtn{position:fixed;bottom:20px;right:20px;width:50px;height:50px;border-radius:50%;background:#2f6feb;color:#fff;border:none;font-size:24px;cursor:pointer;box-shadow:0 4px 12px rgba(0,0,0,.3);opacity:0;transform:translateY(20px);transition:opacity .3s,transform .3s;z-index:1000;display:flex;align-items:center;justify-content:center}
    .scrollToTopBtn.visible{opacity:1;transform:translateY(0)}
  </style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div class="topbar">
      <div>
        <div style="font-size:22px;font-weight:800">NAS音乐</div>
      </div>
      <div class="nav" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px">
        <a class="secondary" href="/">控制页</a>
        <a class="secondary" href="/artists">歌手页</a>
        <a class="secondary" href="/albums">专辑页</a>
        <a class="secondary" href="/radios">电台页</a>
        <a class="secondary" href="/settings">网页设置</a>
      </div>
    </div>
  </div>

  <div class="card">
    <div id="statusText">加载中...</div>
    <div id="err" class="err"></div>
  </div>

  <div class="card">
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
      <input id="searchInput" placeholder="搜索歌名 / 歌手 / 专辑"
             style="flex:1;min-width:180px;background:#222;color:#eee;border:1px solid #444;border-radius:12px;padding:10px;font-size:14px">
      <button onclick="searchNetMusic()">搜索</button>
      <button class="secondary" onclick="clearSearch()">清除</button>
    </div>
    <div class="muted" id="searchInfo" style="margin-top:8px">未搜索</div>
  </div>

  <div class="card">
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px">
      <label class="muted" for="sourceSelect">NAS歌曲文件夹</label>
      <select id="sourceSelect" onchange="changeNetMusicSource()" style="min-width:160px">
        <option value="0">加载中...</option>
      </select>
    </div>
    <div class="muted" id="pathInfo">-</div>
    <div class="pager">
      <button onclick="prevPage()">上一页</button>
      <button onclick="nextPage()">下一页</button>
      <button class="secondary" onclick="refreshPage()">刷新</button>
      <button class="secondary" onclick="focusCurrentPlaying()">定位当前播放</button>

      <label class="muted">每页</label>
      <select id="limitSelect" onchange="changeLimit()">
        <option value="20">20</option>
        <option value="30">30</option>
        <option value="50">50</option>
      </select>

      <input id="pageInput" type="number" min="1" placeholder="页码"
             style="width:78px;background:#222;color:#eee;border:1px solid #444;border-radius:10px;padding:9px">
      <button class="secondary" onclick="goToPage()">跳页</button>

      <input id="indexInput" type="number" min="1" placeholder="序号"
             style="width:78px;background:#222;color:#eee;border:1px solid #444;border-radius:10px;padding:9px">
      <button class="secondary" onclick="goToIndex()">跳序号</button>

      <span class="muted" id="pageInfo">-</span>
    </div>
  </div>

  <div class="card">
    <div class="list" id="musicList"></div>
  </div>
</div>

<script>
let offset = 0;
let limit = 20;
let total = 0;
let currentIdx = -1;
let focusedIdx = -1;
let autoLocatePending = false;
let searchMode = false;
let searchQuery = '';
let activeSourceIndex = -1;
let activeSourceName = 'NAS音乐';

function setText(id, text){
  const el = document.getElementById(id);
  if(el) el.textContent = text;
}

function clearNode(el){
  while(el && el.firstChild) el.removeChild(el.firstChild);
}

function formatDuration(ms){
  ms = Number(ms || 0);
  if(!ms || ms < 0) return '';
  const totalSec = Math.floor(ms / 1000);
  const m = Math.floor(totalSec / 60);
  const s = totalSec % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

function renderNetMusicSources(data){
  const select = document.getElementById('sourceSelect');
  if(!select) return;

  const sources = Array.isArray(data && data.sources) ? data.sources : [];
  const idx = Number(data && data.source_index);
  activeSourceIndex = Number.isInteger(idx) ? idx : 0;
  activeSourceName = (data && data.source_name) ? data.source_name : 'NAS音乐';

  clearNode(select);
  if(!sources.length){
    const option = document.createElement('option');
    option.value = String(activeSourceIndex);
    option.textContent = activeSourceName;
    select.appendChild(option);
    select.disabled = true;
    return;
  }

  sources.forEach(source => {
    const option = document.createElement('option');
    option.value = String(source.idx);
    option.textContent = source.name || `曲库 ${Number(source.idx) + 1}`;
    select.appendChild(option);
  });

  select.value = String(activeSourceIndex);
  select.disabled = sources.length <= 1;
}

async function changeNetMusicSource(){
  const select = document.getElementById('sourceSelect');
  if(!select) return;

  const requestedIdx = parseInt(select.value || '-1', 10);
  if(!Number.isInteger(requestedIdx) || requestedIdx < 0 || requestedIdx === activeSourceIndex){
    return;
  }

  select.disabled = true;
  setText('err', '正在切换 NAS 歌曲文件夹...');

  try{
    const r = await fetch(`/api/netmusic/source?idx=${requestedIdx}`, {method:'POST'});
    const j = await r.json();
    if(!r.ok || !j.ok){
      throw new Error((j && (j.message || j.error)) || '切换失败');
    }

    activeSourceIndex = Number.isInteger(j.source_index) ? j.source_index : requestedIdx;
    activeSourceName = j.source_name || activeSourceName;
    total = Number(j.total || 0);
    currentIdx = -1;
    focusedIdx = Number.isInteger(j.focus_idx) ? j.focus_idx : -1;
    autoLocatePending = focusedIdx >= 0;
    offset = focusedIdx >= 0
      ? Math.floor(focusedIdx / limit) * limit
      : 0;
    searchMode = false;
    searchQuery = '';

    const searchInput = document.getElementById('searchInput');
    if(searchInput) searchInput.value = '';

    await loadStatus();
    await loadNetMusic();
  }catch(e){
    setText('err', `NAS 歌曲文件夹切换失败：${e && e.message ? e.message : 'unknown'}`);
    await loadNetMusic();
  }
}

function renderNetMusicItems(items){
  const box = document.getElementById('musicList');
  clearNode(box);

  (items || []).forEach(it => {
    const row = document.createElement('div');
    row.className = 'item';
    row.dataset.idx = String(it.idx);
    if(it.idx === currentIdx){
      row.classList.add('active');
    }else if(it.idx === focusedIdx){
      row.classList.add('focused');
    }

    const left = document.createElement('div');

    const name = document.createElement('div');
    name.className = 'name';
    name.textContent = `${it.idx + 1}. ${it.title || '-'}`;

    const meta = document.createElement('div');
    meta.className = 'meta';

    const artist = it.artist || '';
    const album = it.album || '';
    const format = it.format || 'mp3';

    let metaParts = [];
    if (artist && artist !== 'NAS') metaParts.push(artist);
    if (album && album !== 'NAS') metaParts.push(album);
    metaParts.push(format.toUpperCase());

    const dur = formatDuration(it.duration_ms);
    if (dur) metaParts.push(dur);

    meta.textContent = metaParts.join(' · ');

    left.appendChild(name);
    left.appendChild(meta);

    const btn = document.createElement('button');
    btn.textContent = '播放';
    btn.onclick = async () => {
      focusedIdx = it.idx;
      const resp = await fetch(`/api/netmusic/play?idx=${it.idx}`, {method:'POST'});
      const ret = await resp.json();
      alert(ret && ret.ok ? (ret.message || '已开始播放 NAS 歌曲') : (ret.message || '操作失败'));
      await loadStatus();
      if (searchMode) {
        await searchNetMusic(false);
      } else {
        await loadNetMusic();
      }
    };

    row.appendChild(left);
    row.appendChild(btn);
    box.appendChild(row);
  });

  if(autoLocatePending){
    const targetIdx = currentIdx >= 0 ? currentIdx : focusedIdx;
    const target = box && targetIdx >= 0
      ? box.querySelector(`[data-idx="${targetIdx}"]`)
      : null;
    autoLocatePending = false;
    if(target){
      requestAnimationFrame(() => target.scrollIntoView({behavior:'smooth', block:'center'}));
    }
  }
}

async function loadNetMusic(options = {}){
  searchMode = false;
  const locateSaved = options.locateSaved === true;
  try{
    const locateArg = locateSaved ? '&locate=saved' : '';
    const r = await fetch(`/api/netmusic?offset=${offset}&limit=${limit}${locateArg}`, {cache:'no-store'});
    const j = await r.json();

    total = j.total || 0;
    if(locateSaved){
      offset = Math.max(0, Number(j.offset) || 0);
      focusedIdx = Number.isInteger(j.focus_idx) ? j.focus_idx : -1;
      autoLocatePending = focusedIdx >= 0;
    }
    if(Number.isInteger(j.playing_idx)){
      currentIdx = j.playing_idx;
    }
    if(offset >= total && total > 0){
      offset = Math.max(0, total - limit);
    }

    renderNetMusicSources(j);
    const pathInfo = document.getElementById('pathInfo');
    if(pathInfo) pathInfo.title = j.base || '';
    setText('pathInfo', `当前文件夹：${activeSourceName || '-'} / 共 ${total} 首`);
    setText('err', j.ok ? '' : `加载提示：${j.error || 'unknown'}`);

    const pageNo = total > 0 ? Math.floor(offset / limit) + 1 : 0;
    const pageTotal = total > 0 ? Math.ceil(total / limit) : 0;
    setText('pageInfo', `第 ${pageNo} / ${pageTotal} 页，当前 ${offset + 1} - ${Math.min(offset + limit, total)}`);
    setText('searchInfo', '未搜索');

    const pageInput = document.getElementById('pageInput');
    if(pageInput && pageNo > 0){
      pageInput.value = pageNo;
    }

    const indexInput = document.getElementById('indexInput');
    if(indexInput){
      indexInput.value = offset + 1;
    }

    renderNetMusicItems(j.items || []);
  }catch(e){
    setText('err', 'NAS音乐列表获取失败');
  }
}

async function searchNetMusic(showAlert = true){
  const input = document.getElementById('searchInput');
  const q = (input && input.value ? input.value : '').trim();

  if(!q){
    if(showAlert) alert('请输入搜索关键词');
    return;
  }

  searchMode = true;
  searchQuery = q;

  try{
    const r = await fetch(`/api/netmusic/search?q=${encodeURIComponent(q)}&limit=50`, {cache:'no-store'});
    const j = await r.json();

    total = j.matched || 0;
    renderNetMusicSources(j);

    setText('pathInfo', `当前文件夹：${activeSourceName || '-'} / 搜索：${q}`);
    setText('pageInfo', `匹配 ${j.matched || 0} 首，显示 ${j.returned || 0} 首`);
    setText('searchInfo', `搜索模式：${q}`);
    setText('err', j.ok ? '' : `搜索提示：${j.error || 'unknown'}`);

    renderNetMusicItems(j.items || []);
  }catch(e){
    setText('err', 'NAS音乐搜索失败');
  }
}

function clearSearch(){
  const input = document.getElementById('searchInput');
  if(input) input.value = '';
  searchMode = false;
  searchQuery = '';
  offset = 0;
  loadNetMusic();
}

async function loadStatus(){
  try{
    const r = await fetch('/api/status', {cache:'no-store'});
    const j = await r.json();

    currentIdx = Number.isInteger(j.net_track_idx) ? j.net_track_idx : -1;

    let t = '当前源：-';
    if(j.source_type === 'net_track'){
      t = `当前源：NAS / ${j.net_track_title || j.title || '-'}`;
      if(j.net_track_state) t += ` / ${j.net_track_state}`;
    }else if(j.source_type === 'radio'){
      t = `当前源：电台 / ${j.radio_name || '-'}`;
      if(j.radio_state) t += ` / ${j.radio_state}`;
    }else{
      t = `当前源：${j.source_type || '-'}`;
      if(j.title) t += ` / ${j.title}`;
    }

    setText('statusText', t);
    if(j.net_track_error){
      setText('err', j.net_track_error);
    }

    return j;
  }catch(e){
    return null;
  }
}

function pageTotal(){
  if(!total || !limit) return 0;
  return Math.ceil(total / limit);
}

function clampPage(page){
  const maxPage = pageTotal();
  if(maxPage <= 0) return 1;
  if(page < 1) return 1;
  if(page > maxPage) return maxPage;
  return page;
}

function goToPage(){
  if(searchMode){
    alert('搜索模式下不能跳页，请先清除搜索');
    return;
  }

  const input = document.getElementById('pageInput');
  const page = clampPage(parseInt(input && input.value ? input.value : '1', 10));

  offset = (page - 1) * limit;
  loadNetMusic();
}

function goToIndex(){
  if(searchMode){
    alert('搜索模式下不能跳序号，请先清除搜索');
    return;
  }

  const input = document.getElementById('indexInput');
  let idx = parseInt(input && input.value ? input.value : '1', 10);

  if(!total || total <= 0){
    return;
  }

  if(idx < 1) idx = 1;
  if(idx > total) idx = total;

  // 用户输入的是 1-based 序号，内部 offset 是 0-based。
  const zeroBased = idx - 1;
  offset = Math.floor(zeroBased / limit) * limit;

  loadNetMusic();
}

function prevPage(){
  if(searchMode) return;
  offset -= limit;
  if(offset < 0) offset = 0;
  loadNetMusic();
}

function nextPage(){
  if(searchMode) return;
  offset += limit;
  if(offset >= total){
    offset = Math.max(0, Math.floor((Math.max(total - 1, 0)) / limit) * limit);
  }
  loadNetMusic();
}

function refreshPage(){
  loadStatus().then(loadNetMusic);
}

function changeLimit(){
  if(searchMode) return;
  const v = parseInt(document.getElementById('limitSelect').value || '20', 10);
  limit = Math.max(1, Math.min(50, v));
  offset = Math.floor(offset / limit) * limit;
  loadNetMusic();
}

async function focusCurrentPlaying(){
  const status = await loadStatus();

  if(!status || status.source_type !== 'net_track'){
    alert('当前没有播放 NAS 歌曲');
    return;
  }

  const idx = Number.isInteger(status.net_track_idx) ? status.net_track_idx : -1;
  if(idx < 0){
    alert('当前 NAS 歌曲序号无效');
    return;
  }

  searchMode = false;
  searchQuery = '';

  const input = document.getElementById('searchInput');
  if(input) input.value = '';

  currentIdx = idx;
  focusedIdx = idx;
  autoLocatePending = true;
  offset = Math.floor(idx / limit) * limit;

  await loadNetMusic();
}

document.addEventListener('DOMContentLoaded', () => {
  const searchInput = document.getElementById('searchInput');
  if(searchInput){
    searchInput.addEventListener('keydown', (e) => {
      if(e.key === 'Enter'){
        searchNetMusic();
      }
    });
  }

  const pageInput = document.getElementById('pageInput');
  if(pageInput){
    pageInput.addEventListener('keydown', (e) => {
      if(e.key === 'Enter'){
        goToPage();
      }
    });
  }

  const indexInput = document.getElementById('indexInput');
  if(indexInput){
    indexInput.addEventListener('keydown', (e) => {
      if(e.key === 'Enter'){
        goToIndex();
      }
    });
  }
});

loadStatus().then((status) => {
  if(status && status.source_type === 'net_track' && Number.isInteger(status.net_track_idx) && status.net_track_idx >= 0){
    currentIdx = status.net_track_idx;
  }
  // 首次进入由后端按“当前播放 → NVS快照 → 列表保存位置”定位并返回正确分页。
  return loadNetMusic({locateSaved:true});
});

setInterval(loadStatus, 2000);

const scrollToTopBtn = document.createElement('button');
scrollToTopBtn.className = 'scrollToTopBtn';
scrollToTopBtn.innerHTML = '↑';
scrollToTopBtn.title = '回到顶部';
scrollToTopBtn.onclick = () => window.scrollTo({top:0,behavior:'smooth'});
document.body.appendChild(scrollToTopBtn);

window.addEventListener('scroll', () => {
  if (window.scrollY > 300) {
    scrollToTopBtn.classList.add('visible');
  } else {
    scrollToTopBtn.classList.remove('visible');
  }
});
</script>
</body>
</html>
)HTML";