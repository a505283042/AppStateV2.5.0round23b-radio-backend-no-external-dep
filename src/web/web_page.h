#pragma once

#include <pgmspace.h>
#include "web/web_config.h"

static const char WEBCTRL_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
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
    .controls{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
    .controls2{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:10px}
    button,.linkbtn{border:none;border-radius:12px;padding:14px 10px;background:#2f6feb;color:#fff;font-size:16px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    button.secondary,.linkbtn.secondary{background:#444}
    button.warn{background:#a04040}
    .status{display:flex;justify-content:space-between;gap:8px;align-items:center;flex-wrap:wrap}
    .small{font-size:12px;color:#aaa}
    .media{display:grid;grid-template-columns:112px 1fr;gap:14px;align-items:start}
    .media.noCover{grid-template-columns:112px 1fr}
    .cover{width:112px;height:112px;border-radius:14px;background:#2a2a2a;overflow:hidden;display:flex;align-items:center;justify-content:center;color:#8e8e8e;font-size:13px;cursor:pointer;user-select:none}
    .cover img{width:100%;height:100%;object-fit:cover;display:block}
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
    .nettrack-only{display:none}
    body.nettrack-mode .nettrack-only{display:block}
    body.nettrack-mode .hide-when-nettrack{display:none!important}
    .nettrack-controls{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
    .nettrack-controls button{min-width:92px}
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
          <div class="muted" id="net">连接中...</div>
        </div>
        <div class="small" id="pollInfo">刷新：加载中...</div>
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
          <div class="small" style="margin-top:8px;text-align:center">点击封面：切换显示样式</div>
        </div>
        <div>
          <div class="title" id="title">-</div>
          <div class="sub" id="artist">-</div>
          <div class="sub" id="album">-</div>
          <div class="bar"><div class="fill" id="progressFill"></div></div>
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
      <div class="controls">
        <button class="secondary" id="prevBtn" onclick="handlePrev()">上一首</button>
        <button id="playPauseBtn" onclick="sendCmd('/api/playpause')">播放/暂停</button>
        <button class="secondary" id="nextBtn" onclick="handleNext()">下一首</button>
      </div>
      <div class="controls2" id="modeRow">
        <button class="secondary" id="modeToggleBtn" onclick="sendCmd('/api/mode/toggle')">顺序/随机</button>
        <button class="secondary" id="modeCategoryBtn" onclick="sendCmd('/api/mode/category')">全部/歌手/专辑</button>
        <button class="warn" id="scanBtn" onclick="sendCmd('/api/scan')">开始重扫</button>
      </div>
      <div class="controls2" style="grid-template-columns:1fr 1fr">
        <button class="secondary" id="radioBackBtn" style="display:none" onclick="returnFromRadio()">返回音乐播放</button>
        <button class="secondary" onclick="savePlayerState()">保存当前状态</button>
        <button class="secondary" id="wifiInfoBtn" onclick="toggleWifiInfo()">隐藏WiFi信息</button>
        <div></div>
      </div>
      <div class="small" style="margin-top:8px">保存到设备内部 NVS：音量、当前歌曲、播放模式、当前分组与视图</div>
    </div>

    <div class="card nettrack-only" id="netTrackControlCard">
      <div style="font-size:18px;font-weight:800">NAS 播放控制</div>
      <div class="muted" id="netTrackNow">-</div>

      <div class="nettrack-controls">
        <button onclick="nasControl('/api/netmusic/prev')">上一首</button>
        <button onclick="nasControl('/api/netmusic/toggle')">播放/暂停</button>
        <button onclick="nasControl('/api/netmusic/next')">下一首</button>
        <button onclick="nasControl('/api/netmusic/mode')" id="netTrackModeBtn">顺序/随机</button>
        <button class="secondary" onclick="nasControl('/api/netmusic/return-local')">返回本地播放</button>
      </div>
    </div>
  </div>

<script>
let POLL_MS = 1000;
const MIN_STATUS_POLL_MS = 1500;
const STATUS_POLL_JITTER_MS = 700;
let LYRIC_WAIT_POLL_THRESHOLD_MS = 150;
let lastCoverTrack = '';
let pollTimer = null;
let lyricTimer = null;
let volumeTimer = null;
let inFlight = false;
let statusController = null;
let statusFetchStartedAt = 0;
let currentPollMs = POLL_MS;
let nextPollAt = Date.now() + POLL_MS;
let lastStatus = null;
let lastStatusAt = 0;
let coverToggleBusy = false;
let pageActive = !document.hidden;
let pagePausedByVisibility = false;
const LOCK_STORAGE_KEY = 'webctrl_page_locked';
let pageLocked = false;
let volumeLocked = true;

function getLockTargets(){
  return [
    ...document.querySelectorAll('.nav a'),
    document.getElementById('coverBox'),
    document.getElementById('prevBtn'),
    document.getElementById('playPauseBtn'),
    document.getElementById('nextBtn'),
    document.getElementById('modeToggleBtn'),
    document.getElementById('modeCategoryBtn'),
    document.getElementById('scanBtn'),
    document.getElementById('radioBackBtn'),
    document.getElementById('wifiInfoBtn'),
    
    ...document.querySelectorAll('button.secondary[onclick="savePlayerState()"]')
  ].filter(Boolean);
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

    if(el.tagName === 'BUTTON' || el.tagName === 'INPUT'){
      el.disabled = pageLocked;
    }else{
      el.style.pointerEvents = pageLocked ? 'none' : '';
      el.style.opacity = pageLocked ? '0.45' : '';
    }
  });

  const coverBox = document.getElementById('coverBox');
  if(coverBox){
    coverBox.style.pointerEvents = pageLocked ? 'none' : '';
    coverBox.style.opacity = '';
  }

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

  const baseDelay = Math.max(MIN_STATUS_POLL_MS, Number(ms) || POLL_MS);
  const delay = baseDelay + Math.floor(Math.random() * STATUS_POLL_JITTER_MS);
  if(pollTimer) clearTimeout(pollTimer);
  nextPollAt = Date.now() + delay;
  pollTimer = setTimeout(fetchStatus, delay);
}
function fmt(ms){ const s=Math.max(0,Math.floor((ms||0)/1000)); const m=Math.floor(s/60); const r=s%60; return `${m}:${String(r).padStart(2,'0')}`; }
function estimatePlayMs(snap){ let play=Number(snap?.play_ms)||0; if(snap&&snap.is_playing&&!snap.is_paused&&!snap.rescanning){ play += Math.max(0, Date.now()-lastStatusAt);} return play; }
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
  const msToLyric = nextStart - estimatePlayMs(j);
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
      scheduleNext(1500);
      return;
    }
  }

  inFlight = true;
  statusFetchStartedAt = Date.now();

  const controller = new AbortController();
  statusController = controller;
  const timeoutId = setTimeout(() => controller.abort(), 8000);

  try{
    const r = await fetch(`/api/status?t=${Date.now()}`, {
      cache:'no-store',
      signal: controller.signal
    });
    const j = await r.json();
    lastStatusAt = Date.now();
    lastStatus = j;
    syncVolumeLockFromStatus(j);
    applyNetTrackMode(j);
    render(j);
    currentPollMs = Math.max(120, Number(j.next_poll_ms) || POLL_MS);
    if(Number(j.refresh_poll_ms) > 0) POLL_MS = Number(j.refresh_poll_ms);
    if(Number(j.lyric_wait_poll_threshold_ms) > 0) LYRIC_WAIT_POLL_THRESHOLD_MS = Number(j.lyric_wait_poll_threshold_ms);
    const lyricThreshold = (typeof j.lyric_wait_poll_threshold_ms !== 'undefined' && Number(j.lyric_wait_poll_threshold_ms) > 0) ? Number(j.lyric_wait_poll_threshold_ms) : LYRIC_WAIT_POLL_THRESHOLD_MS;
    document.getElementById('pollInfo').textContent = `刷新：${currentPollMs} ms / 歌词：${j.lyric_sync_mode_label||'平衡'} / 阈值：${lyricThreshold}ms`;
    scheduleNext(currentPollMs); scheduleLyricTransition(j);
  }catch(e){
    document.getElementById('net').textContent = e.name === 'AbortError' ? '网页请求超时，正在重试' : '网页状态获取失败';
    scheduleNext(Math.max(POLL_MS, 3000));
  }finally{
    clearTimeout(timeoutId);
    if(statusController === controller){
      statusController = null;
    }
    inFlight = false;
    statusFetchStartedAt = 0;
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
  abortStatusFetch();
}

function resumePagePolling(){
  pageActive = true;

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
  const allowSpin = j.web_cover_spin !== false;
  const base = j.cover_url && j.cover_url.length ? j.cover_url : '';

  // 封面区域保持原有布局，只是控制图片显示
  media.classList.toggle('noCover', !allowCover);
  if(!allowCover){ 
    // 不显示封面图片，但保持封面区域
    box.classList.remove('coverPanel','coverReady');
    img.style.display='none';
    img.removeAttribute('src');
    fallback.style.display='block';
    fallback.innerHTML='网页端封面<br>显示已关闭';
    return; 
  }

  box.classList.toggle('rotate', rotateView);
  box.classList.toggle('coverPanel', coverPanelView);
  box.classList.toggle('spin', (rotateView || coverPanelView) && allowSpin && !!j.is_playing && !j.is_paused && !j.rescanning);

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

  const now = document.getElementById('netTrackNow');
  if(now){
    if(isNetTrack){
      const title = j.net_track_title || j.title || '-';
      const artist = j.net_track_artist && j.net_track_artist !== 'NAS'
        ? ` · ${j.net_track_artist}`
        : '';
      const idx = Number.isInteger(j.net_track_idx) ? `#${j.net_track_idx + 1}` : '';
      now.textContent = `${idx} ${title}${artist}`;
    }else{
      now.textContent = '-';
    }
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
  document.getElementById('time').textContent=`${fmt(j.play_ms)} / ${fmt(j.total_ms)}`;
  document.getElementById('playState').textContent=j.rescanning ? (j.can_cancel_scan ? '扫描中（可取消）' : '扫描中（取消中）') : (j.is_paused ? '已暂停' : (j.is_playing ? '播放中' : '已停止'));
    if(j.show_wifi_info === false){
    document.getElementById('net').textContent = `${j.net_mode||'-'} · WiFi信息已隐藏`;
  }else{
    document.getElementById('net').textContent = `${j.net_mode||'-'} · ${j.ip||'-'} · ${j.wifi_name||'-'}`;
  }

  if(j.show_wifi_info !== undefined){
    updateWifiInfoButton(!!j.show_wifi_info);
  }
  const total=Math.max(1,j.total_ms||0); const pct=Math.max(0,Math.min(100,Math.floor(((j.play_ms||0)*100)/total))); document.getElementById('progressFill').style.width=`${pct}%`;
  document.getElementById('scanBtn').textContent=j.scan_action_label || (j.rescanning ? '取消重扫' : '开始重扫');
  updateLyricsFromState(j);
  const slider=document.getElementById('volumeSlider'); if(document.activeElement !== slider){ slider.value=Number(j.volume ?? 0); }

  const isRadio = (j.source_type === 'radio');

  const prevBtn = document.getElementById('prevBtn');
  const nextBtn = document.getElementById('nextBtn');
  const modeRow = document.getElementById('modeRow');
  const scanBtn = document.getElementById('scanBtn');

  prevBtn.textContent = isRadio ? '上一电台' : '上一首';
  nextBtn.textContent = isRadio ? '下一电台' : '下一首';

  modeRow.style.display = isRadio ? 'none' : 'grid';
  scanBtn.style.display = isRadio ? 'none' : 'inline-flex';

  const radioBackBtn = document.getElementById('radioBackBtn');
  if (radioBackBtn) {
    radioBackBtn.style.display = isRadio ? 'inline-flex' : 'none';
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

async function toggleWifiInfo(){
  if(pageLocked) return;
  try{
    const r = await fetch('/api/wifiinfo/toggle', {method:'POST'});
    const j = await r.json();
    if(j && j.ok && j.show_wifi_info !== undefined) {
      updateWifiInfoButton(j.show_wifi_info);
    }
  }catch(e){}
  scheduleNext(500);
}

function updateWifiInfoButton(showWifiInfo) {
  const btn = document.getElementById('wifiInfoBtn');
  if(btn) {
    btn.textContent = showWifiInfo ? '隐藏WiFi信息' : '显示WiFi信息';
  }
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

setTimeout(fetchStatus, 200 + Math.floor(Math.random() * 900));
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
  <title>网页设置</title>
  <style>
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .row{display:grid;grid-template-columns:1fr auto;gap:12px;align-items:center;margin-bottom:12px}
    label{font-size:15px}
    input[type=number],select{width:180px;padding:10px;border-radius:10px;border:1px solid #444;background:#111;color:#eee}
    input[type=checkbox]{transform:scale(1.2)}
    button,a{border:none;border-radius:12px;padding:12px 14px;background:#2f6feb;color:#fff;font-size:15px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    a.secondary,button.secondary{background:#444}
    button.danger{background:#b42318}
    .actions{display:flex;gap:10px;flex-wrap:wrap}
    .time-fields{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:12px 0}
    .time-fields label{font-size:14px;color:#ccc}
    .time-fields input{width:72px;padding:10px;border-radius:10px;border:1px solid #444;background:#111;color:#eee}
    .weekday-fields{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin:8px 0 12px}
    .weekday-fields label{font-size:14px;color:#ddd;display:inline-flex;gap:6px;align-items:center}
    .muted{color:#aaa;font-size:14px}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <div class="actions">
        <a class="secondary" href="/">返回控制页</a>
      </div>
      <h2>网页设置</h2>
      <div class="muted">设置会保存到设备内部配置区（更稳定）</div>
    </div>

    <div class="card">
      <h2>时间与闹钟</h2>
      <div class="row"><label>当前时间</label><div id="rtcTimeText">-</div></div>
      <div class="row"><label>闹钟状态</label><div id="alarmEnabledText">-</div></div>
      <div class="row"><label>下次触发</label><div id="alarmNextText">-</div></div>
      <div class="actions">
        <button onclick="syncRtcFromBrowser()">校准时间</button>
        <button class="secondary" onclick="refreshClockAlarmStatus()">刷新</button>
      </div>
      <div class="muted" id="rtcStatusText">首次使用闹钟前，请先校准时间。</div>

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

    <div class="card">
      <div class="row"><label>页面刷新速度</label>
        <select id="refresh_preset">
          <option value="power">省流量 / 省电</option>
          <option value="balanced">平衡</option>
          <option value="smooth">流畅</option>
        </select>
      </div>
      <div class="row"><label>歌词更新时间策略</label>
        <select id="lyric_sync_mode">
          <option value="precise">精准优先</option>
          <option value="balanced">平衡</option>
          <option value="follow_poll">等轮询优先</option>
        </select>
      </div>
      <div class="row"><label>显示下一句歌词</label><input id="show_next_lyric" type="checkbox"></div>
      <div class="row"><label>显示封面</label><input id="show_cover" type="checkbox"></div>
      <div class="row"><label>旋转视图时网页封面旋转</label><input id="web_cover_spin" type="checkbox"></div>
      <div class="actions">
        <button onclick="saveSettings()">保存设置</button>
        <button class="secondary" onclick="loadSettings()">重新读取</button>
      </div>
    </div>
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
  }catch(e){}
}
async function saveSettings(){
  const params = new URLSearchParams();
  params.set('refresh_preset', document.getElementById('refresh_preset').value);
  params.set('lyric_sync_mode', document.getElementById('lyric_sync_mode').value);
  params.set('show_next_lyric', document.getElementById('show_next_lyric').checked ? '1' : '0');
  params.set('show_cover', document.getElementById('show_cover').checked ? '1' : '0');
  params.set('web_cover_spin', document.getElementById('web_cover_spin').checked ? '1' : '0');
  try{
    const r = await fetchWithTimeout('/api/settings', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:params.toString()
    }, 3500);
    const j = await r.json();
    alert(j && j.ok ? '保存成功' : ((j && j.message) ? j.message : '保存失败'));
  }catch(e){ alert('保存失败'); }
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

refreshClockAlarmStatus();
loadSettings();

// RTC时间只定期向设备校准一次，页面显示由浏览器每秒递增，避免“当前时间”看起来不走。
setInterval(updateRtcClockDisplay, 1000);
setInterval(loadRtcStatus, 60000);
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
        <div class="muted" id="statusText">加载中...</div>
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
        $('statusText').textContent = '搜索失败';
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

    $('statusText').textContent = `当前歌曲歌手：${currentTrackArtist || '-'}；当前模式：${j.mode_label || '-'}`;
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
    $('statusText').textContent='加载失败';
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
        <div class="muted" id="statusText">加载中...</div>
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
        $('statusText').textContent = '搜索失败';
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

    $('statusText').textContent = `当前歌曲专辑：${currentTrackAlbum || '-'} / ${currentTrackArtist || '-'}；当前模式：${j.mode_label || '-'}`;
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
  loadAlbums().catch(e=>{ $('statusText').textContent='加载失败'; alert(e.message||'加载失败'); });
 
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
    .item{padding:14px;border:1px solid #2e2e2e;border-radius:14px;background:#151515}
    .itemHead{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
    .badge{display:inline-flex;align-items:center;justify-content:center;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:700}
    .badge.track{background:#224a8a;color:#cfe3ff}
    .badge.artist{background:#245b3d;color:#d5ffe6}
    .badge.album{background:#5a3978;color:#f0dcff}
    .uid{font-size:12px;color:#aaa;word-break:break-all}
    .name{font-size:18px;font-weight:700;margin-top:10px}
    .key{font-size:13px;color:#bdbdbd;margin-top:6px;word-break:break-all}
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
        <div class="muted" id="statusText">加载中...</div>
      </div>
      <div class="actions">
        <a class="secondary" href="/">控制页</a>
        <a class="secondary" href="/artists">歌手页</a>
        <a class="secondary" href="/albums">专辑页</a>
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
        <div class="small">列表读取自当前内存绑定表</div>
      </div>
      <div class="list" id="bindingList" style="margin-top:12px"></div>
    </div>
  </div>

<script>
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>'"]/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
let allBindings = [];
let currentFilter = 'all';

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

  box.innerHTML = items.map(x => `
    <div class="item">
      <div class="itemHead">
        <span class="badge ${bindTypeClass(x.type)}">${esc(bindTypeLabel(x.type))}</span>
        <div class="uid">UID：${esc(x.uid || '-')}</div>
      </div>
      <div class="name">${esc(x.display || '-')}</div>
      <div class="key">${esc(x.key || '-')}</div>
      <div class="rowActions">
        <button class="secondary" onclick="testPlay('${esc(x.uid || '')}')">测试播放</button>
        <button class="warn" onclick="deleteBinding('${esc(x.uid || '')}','${esc(x.display || '')}')">删除绑定</button>
      </div>
    </div>
  `).join('');
}

async function loadBindings(){
  $('statusText').textContent = '加载中...';
  try{
    const r = await fetch('/api/nfc/bindings', {cache:'no-store'});
    const j = await r.json();
    if(!j.ok) throw new Error(j.message || '加载失败');
    allBindings = j.items || [];
    $('statusText').textContent = `已加载 ${allBindings.length} 条绑定`;
    renderBindings();
  }catch(e){
    $('statusText').textContent = '加载失败';
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
        <div class="muted">网络电台功能已上线 - 支持电台目录浏览与实时状态显示</div>
      </div>
      <div class="nav" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px">
        <a class="secondary" href="/">控制页</a>
        <a class="secondary" href="/artists">歌手页</a>
        <a class="secondary" href="/albums">专辑页</a>
        <a class="secondary" href="/settings">网页设置</a>
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
        <div class="muted">通过 /System/net_music.txt 按需分页读取，不全量加载到网页</div>
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
let searchMode = false;
let searchQuery = '';

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

function renderNetMusicItems(items){
  const box = document.getElementById('musicList');
  clearNode(box);

  (items || []).forEach(it => {
    const row = document.createElement('div');
    row.className = 'item';
    if(it.idx === currentIdx) row.classList.add('active');

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
}

async function loadNetMusic(){
  searchMode = false;
  try{
    const r = await fetch(`/api/netmusic?offset=${offset}&limit=${limit}`, {cache:'no-store'});
    const j = await r.json();

    total = j.total || 0;
    if(offset >= total && total > 0){
      offset = Math.max(0, total - limit);
    }

    setText('pathInfo', `Base：${j.base || '-'} / 共 ${total} 首`);
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

    setText('pathInfo', `搜索：${q}`);
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

function goToPage(){
  if(searchMode) return;
  const input = document.getElementById('pageInput');
  const page = parseInt(input && input.value ? input.value : '0', 10);
  if(!page || page < 1){
    alert('请输入有效页码');
    return;
  }
  const pageTotal = total > 0 ? Math.ceil(total / limit) : 0;
  if(page > pageTotal){
    alert(`最大页码为 ${pageTotal}`);
    return;
  }
  offset = (page - 1) * limit;
  loadNetMusic();
}

function goToIndex(){
  if(searchMode) return;
  const input = document.getElementById('indexInput');
  const idx = parseInt(input && input.value ? input.value : '0', 10);
  if(!idx || idx < 1){
    alert('请输入有效序号');
    return;
  }
  if(idx > total){
    alert(`最大序号为 ${total}`);
    return;
  }
  offset = idx - 1;
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
    offset = Math.floor(currentIdx / limit) * limit;
  }
  return loadNetMusic();
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