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
    @keyframes webCoverSpin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
    .lyrics{line-height:1.5}
    .lyrics .line{font-size:18px;font-weight:700;margin:0 0 8px}
    .lyrics .next{font-size:14px;color:#bdbdbd}
    .volrow{display:flex;align-items:center;gap:12px;margin-top:8px}
    .volrow input[type=range]{flex:1}
    input[type=range]{accent-color:#79c0ff}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <div class="status">
        <div>
          <h1>ESP32S3 播放器</h1>
          <div class="muted" id="net">连接中...</div>
        </div>
        <div class="small" id="pollInfo">刷新：加载中...</div>
      </div>
      <div class="nav" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px">
        <a class="linkbtn secondary" href="/artists" style="padding:10px 12px;font-size:14px">歌手页</a>
        <a class="linkbtn secondary" href="/albums" style="padding:10px 12px;font-size:14px">专辑页</a>
        <a class="linkbtn secondary" href="/radios" style="padding:10px 12px;font-size:14px">电台页</a>
        <a class="linkbtn secondary" href="/settings" style="padding:10px 12px;font-size:14px">网页设置</a>
      </div>
    </div>

    <div class="card">
      <div class="media" id="mediaBox">
        <div class="cover" id="coverBox"><span id="coverFallback">无封面</span><img id="coverImg" alt="封面" style="display:none"></div>
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
      <div class="k">网页音量调节</div>
      <div class="small">点击封面：切换 信息视图 / 旋转视图</div>
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

    <div class="card">
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
  </div>

<script>
let POLL_MS = 1000;
let LYRIC_WAIT_POLL_THRESHOLD_MS = 150;
let lastCoverTrack = '';
let pollTimer = null;
let lyricTimer = null;
let volumeTimer = null;
let inFlight = false;
let currentPollMs = POLL_MS;
let nextPollAt = Date.now() + POLL_MS;
let lastStatus = null;
let lastStatusAt = 0;
let coverToggleBusy = false;

function scheduleNext(ms){
  const delay = Math.max(120, Number(ms) || POLL_MS);
  if(pollTimer) clearTimeout(pollTimer);
  nextPollAt = Date.now() + delay;
  pollTimer = setTimeout(fetchStatus, delay);
}
function fmt(ms){ const s=Math.max(0,Math.floor((ms||0)/1000)); const m=Math.floor(s/60); const r=s%60; return `${m}:${String(r).padStart(2,'0')}`; }
function estimatePlayMs(snap){ let play=Number(snap?.play_ms)||0; if(snap&&snap.is_playing&&!snap.is_paused&&!snap.rescanning){ play += Math.max(0, Date.now()-lastStatusAt);} return play; }
function clearLyricTimer(){ if(lyricTimer){ clearTimeout(lyricTimer); lyricTimer=null; } }
function updateLyricsFromState(j){
  document.getElementById('lyricCurrent').textContent = j.has_lyrics ? (j.current_lyric || '...') : '当前曲目暂无歌词';
  const nextNode = document.getElementById('lyricNext');
  if(j.show_next_lyric===false){ nextNode.style.display='none'; return; }
  nextNode.style.display='block';
  nextNode.textContent = j.has_lyrics ? ((j.next_lyric && j.next_lyric.length) ? `下一句：${j.next_lyric}` : '下一句：-') : '-';
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
  if(inFlight) return;
  inFlight = true;
  try{
    const r = await fetch('/api/status', {cache:'no-store'});
    const j = await r.json();
    lastStatusAt = Date.now(); lastStatus = j; render(j);
    currentPollMs = Math.max(120, Number(j.next_poll_ms) || POLL_MS);
    if(Number(j.refresh_poll_ms) > 0) POLL_MS = Number(j.refresh_poll_ms);
    if(Number(j.lyric_wait_poll_threshold_ms) > 0) LYRIC_WAIT_POLL_THRESHOLD_MS = Number(j.lyric_wait_poll_threshold_ms);
    const lyricThreshold = (typeof j.lyric_wait_poll_threshold_ms !== 'undefined' && Number(j.lyric_wait_poll_threshold_ms) > 0) ? Number(j.lyric_wait_poll_threshold_ms) : LYRIC_WAIT_POLL_THRESHOLD_MS;
    document.getElementById('pollInfo').textContent = `刷新：${currentPollMs} ms / 歌词：${j.lyric_sync_mode_label||'平衡'} / 阈值：${lyricThreshold}ms`;
    scheduleNext(currentPollMs); scheduleLyricTransition(j);
  }catch(e){ document.getElementById('net').textContent = '网页状态获取失败'; scheduleNext(Math.max(POLL_MS, 1500)); }
  finally{ inFlight = false; }
}
function updateCover(j){
  const media=document.getElementById('mediaBox');
  const box=document.getElementById('coverBox');
  const img=document.getElementById('coverImg');
  const fallback=document.getElementById('coverFallback');

  const track=Number.isInteger(j.track_idx)?j.track_idx:-1;
  const rotateView=(j.view==='rotate');
  const allowCover = j.show_cover !== false;
  const allowSpin = j.web_cover_spin !== false;
  const base = j.cover_url && j.cover_url.length ? j.cover_url : '';

  // 封面区域保持原有布局，只是控制图片显示
  media.classList.toggle('noCover', !allowCover);
  if(!allowCover){ 
    // 不显示封面图片，但保持封面区域
    img.style.display='none';
    img.removeAttribute('src');
    fallback.style.display='block';
    fallback.innerHTML='网页端封面<br>显示已关闭';
    return; 
  }

  box.classList.toggle('rotate', rotateView);
  box.classList.toggle('spin', rotateView && allowSpin && !!j.is_playing && !j.is_paused && !j.rescanning);

  if(!j.has_cover || !base){ 
    lastCoverTrack = '';
    img.style.display='none';
    img.removeAttribute('src');
    fallback.style.display='block';
    fallback.textContent='无封面';
    return;
  }

  fallback.style.display='none';
  img.style.display='block';

  const coverKey = (j.source_type==='radio')
    ? `radio:${j.radio_idx||-1}:${base}`
    : `track:${track}:${base}`;

  if(coverKey !== lastCoverTrack){ 
    lastCoverTrack = coverKey;
    img.onerror=()=>{ 
      img.style.display='none';
      fallback.style.display='block';
      fallback.textContent='封面读取失败';
    };
    img.src=`${base}${base.includes('?')?'&':'?'}t=${Date.now()}`;
  }
}
async function toggleViewFromCover(){ if(coverToggleBusy) return; coverToggleBusy=true; try{ await fetch('/api/view/toggle',{method:'POST'});}catch(e){} scheduleNext(120); setTimeout(()=>{coverToggleBusy=false;},250); }
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
  document.getElementById('net').textContent=`${j.net_mode||'-'} · ${j.ip||'-'} · ${j.wifi_name||'-'}`;
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
  await sendCmd('/api/prev');
}

async function handleNext(){
  await sendCmd('/api/next');
}

async function toggleWifiInfo(){
  try{
    const r = await fetch('/api/wifiinfo/toggle', {method:'POST'});
    const j = await r.json();
    if(j && j.ok && j.show_wifi_info !== undefined) {
      updateWifiInfoButton(j.show_wifi_info);
    }
  }catch(e){}
  scheduleNext(120);
}

function updateWifiInfoButton(showWifiInfo) {
  const btn = document.getElementById('wifiInfoBtn');
  if(btn) {
    btn.textContent = showWifiInfo ? '隐藏WiFi信息' : '显示WiFi信息';
  }
}

async function sendCmd(path){ try{ await fetch(path,{method:'POST'});}catch(e){} scheduleNext(120); }
async function returnFromRadio(){
  try{
    const r = await fetch('/api/radio/stop', {method:'POST'});
    const j = await r.json();
    alert(j && j.ok ? (j.message || '已返回本地播放') : ((j && j.message) ? j.message : '操作失败'));
  }catch(e){
    alert('操作失败');
  }
  scheduleNext(120);
}
async function savePlayerState(){
  try{
    const r = await fetch('/api/state/save', {method:'POST'});
    const j = await r.json();
    alert(j && j.ok ? '当前状态已保存到 NVS' : ((j && j.message) ? j.message : '保存失败'));
  }catch(e){
    alert('保存失败');
  }
  scheduleNext(120);
}
function sendVolumeDebounced(v){ if(volumeTimer) clearTimeout(volumeTimer); volumeTimer=setTimeout(async()=>{ try{ await fetch('/api/volume',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:`value=${encodeURIComponent(v)}`}); }catch(e){} scheduleNext(180); },80); }
const slider=document.getElementById('volumeSlider');
slider.addEventListener('input',(e)=>{ const v=Number(e.target.value||0); document.getElementById('volume').textContent=`${v}%`; sendVolumeDebounced(v); });
slider.addEventListener('change',(e)=>{ const v=Number(e.target.value||0); sendVolumeDebounced(v); });
document.getElementById('coverBox').addEventListener('click',toggleViewFromCover);
fetchStatus();
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
    .actions{display:flex;gap:10px;flex-wrap:wrap}
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
      <div class="muted">设置会保存到设备内部配置区（更稳定），旧版 SD 配置会自动兼容导入</div>
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
    const r = await fetch('/api/settings', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body:params.toString()});
    const j = await r.json();
    alert(j && j.ok ? '保存成功' : ((j && j.message) ? j.message : '保存失败'));
  }catch(e){ alert('保存失败'); }
}
loadSettings();
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
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .top{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
    .actions{display:flex;gap:8px;flex-wrap:wrap}
    a,button{border:none;border-radius:12px;padding:10px 14px;background:#2f6feb;color:#fff;font-size:15px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    a.secondary,button.secondary{background:#444}
    input{width:100%;padding:12px 14px;border-radius:12px;border:1px solid #444;background:#111;color:#eee;box-sizing:border-box}
    .muted{color:#aaa;font-size:14px}
    .list{max-height:70vh;overflow:auto}
    .item{padding:12px;border:1px solid #2e2e2e;border-radius:12px;margin-bottom:8px;cursor:pointer;background:#151515}
    .item.active{border-color:#2f6feb;background:#16233d}
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
    @media (max-width:900px){.layout{grid-template-columns:1fr}.list{max-height:none}}
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
        <a class="secondary" href="/radios">电台页</a>
        <a class="secondary" href="/settings">网页设置</a>
      </div>
    </div>

    <div class="card">
      <input id="searchInput" placeholder="搜索歌手名">
    </div>

    <div class="card">
      <div class="muted" id="countText">-</div>
      <div class="list" id="artistList"></div>
    </div>
  </div>
<script>
const $ = id => document.getElementById(id); 
 const esc = s => String(s ?? '').replace(/[&<>'"]/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m])); 
 async function postForm(url, obj){ const b=new URLSearchParams(); Object.keys(obj).forEach(k=>b.append(k,obj[k])); const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}); return r.json(); } 
 
 let allItems = []; 
 let expandedIdx = -1; 
 let detailCache = {}; 
 
 function makeDetailState(idx){ 
   return { 
     idx, 
     name: '', 
     track_count: 0, 
     tracks: [], 
     loaded: 0, 
     done: false, 
     loading: false 
   }; 
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
         <button class="secondary" onclick="event.stopPropagation(); playTrack(${t.track_idx}, ${detail.idx})">播放</button> 
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

 function renderList(){ 
   const q = ($('searchInput').value || '').trim().toLowerCase(); 
   const box = $('artistList'); 
   const items = allItems.filter(x => !q || (x.name || '').toLowerCase().includes(q)); 

   $('countText').textContent = `共 ${allItems.length} 位歌手，当前显示 ${items.length} 位`; 

   if(!items.length){ 
     box.innerHTML = '<div class="empty">没有匹配的歌手</div>'; 
     return; 
   } 

   box.innerHTML = items.map(x => { 
     const expanded = x.idx === expandedIdx; 
     const detail = detailCache[x.idx]; 

     return ` 
       <div class="item ${expanded ? 'active' : ''}" onclick="toggleArtist(${x.idx})"><div class="itemHead">
           <div class="itemMeta">
             <div class="name">${esc(x.name || '未知歌手')}</div>
             <div class="sub">${x.track_count || 0} 首</div>
           </div>
           <div class="muted">${expanded ? '▲ 收起' : '▼ 展开'}</div>
         </div>

         ${expanded ? `
           <div class="expandBox">
             <div class="expandActions">
               <button onclick="event.stopPropagation(); playGroup(${x.idx})" ${(x.track_count || 0) > 0 ? '' : 'disabled'}>播放这一组</button>
             </div>
             ${detail ? renderArtistTracks(detail) : '<div class="expandEmpty">加载中...</div>'}
           </div>
         ` : ''}
       </div>
     `; 
   }).join(''); 
 } 

 async function loadArtists(){ 
   const r = await fetch('/api/artists', {cache:'no-store'}); 
   const j = await r.json(); 
   if(!j.ok) throw new Error(j.message || 'load failed'); 

   allItems = j.items || []; 
   $('statusText').textContent = `当前模式：${j.mode_label || '-'}；当前分组：${(j.current_group_idx ?? -1) >= 0 ? (j.current_group_idx + 1) : '-'}`; 
   renderList(); 
 } 

 async function loadDetail(idx, append){ 
   let state = detailCache[idx]; 
   if(!state){ 
     state = makeDetailState(idx); 
     detailCache[idx] = state; 
   } 

   if(state.loading || state.done) return; 

   state.loading = true; 
   renderList(); 

   try{ 
     const offset = append ? state.loaded : 0; 
     const limit = 20; 
     const r = await fetch(`/api/artist/detail?idx=${idx}&offset=${offset}&limit=${limit}`, {cache:'no-store'}); 
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

   if(!detailCache[idx]){ 
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
   loadArtists().catch(()=>{}); 
 } 

 async function playTrack(trackIdx, groupIdx){ 
   const j = await postForm('/api/track/play', {idx:trackIdx, mode:'artist', group_idx:groupIdx}); 
   alert(j && j.ok ? '已开始播放' : '播放失败'); 
   loadArtists().catch(()=>{}); 
 } 

 $('searchInput').addEventListener('input', renderList); 
 loadArtists().catch(e=>{ $('statusText').textContent='加载失败'; alert(e.message||'加载失败'); });
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
    body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111;color:#eee}
    .wrap{max-width:760px;margin:0 auto;padding:16px}
    .card{background:#1b1b1b;border-radius:16px;padding:16px;margin-bottom:12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
    .top{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
    .actions{display:flex;gap:8px;flex-wrap:wrap}
    a,button{border:none;border-radius:12px;padding:10px 14px;background:#2f6feb;color:#fff;font-size:15px;font-weight:600;text-decoration:none;display:inline-flex;align-items:center;justify-content:center}
    a.secondary,button.secondary{background:#444}
    input{width:100%;padding:12px 14px;border-radius:12px;border:1px solid #444;background:#111;color:#eee;box-sizing:border-box}
    .muted{color:#aaa;font-size:14px}
    .list{max-height:70vh;overflow:auto}
    .item{padding:12px;border:1px solid #2e2e2e;border-radius:12px;margin-bottom:8px;cursor:pointer;background:#151515}
    .item.active{border-color:#2f6feb;background:#16233d}
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
    @media (max-width:900px){.layout{grid-template-columns:1fr}.list{max-height:none}}
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
        <a class="secondary" href="/radios">电台页</a>
        <a class="secondary" href="/settings">网页设置</a>
      </div>
    </div>

    <div class="card">
      <input id="searchInput" placeholder="搜索 专辑名 / 歌手名">
    </div>

    <div class="card">
      <div class="muted" id="countText">-</div>
      <div class="list" id="albumList"></div>
    </div>
  </div>
<script>
const $ = id => document.getElementById(id); 
 const esc = s => String(s ?? '').replace(/[&<>'"]/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m])); 
 async function postForm(url, obj){ const b=new URLSearchParams(); Object.keys(obj).forEach(k=>b.append(k,obj[k])); const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}); return r.json(); } 
 
 let allItems = []; 
 let expandedIdx = -1; 
 let detailCache = {}; 
 
 function makeDetailState(idx){ 
   return { 
     idx, 
     name: '', 
     track_count: 0, 
     tracks: [], 
     loaded: 0, 
     done: false, 
     loading: false 
   }; 
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
         <button class="secondary" onclick="event.stopPropagation(); playTrack(${t.track_idx}, ${detail.idx})">播放</button> 
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

 function renderList(){ 
   const q = ($('searchInput').value || '').trim().toLowerCase(); 
   const box = $('albumList'); 
   const items = allItems.filter(x => !q || (x.name || '').toLowerCase().includes(q) || (x.primary_artist || '').toLowerCase().includes(q)); 

   $('countText').textContent = `共 ${allItems.length} 张专辑，当前显示 ${items.length} 张`; 

   if(!items.length){ 
     box.innerHTML = '<div class="empty">没有匹配的专辑</div>'; 
     return; 
   } 

   box.innerHTML = items.map(x => { 
     const expanded = x.idx === expandedIdx; 
     const detail = detailCache[x.idx]; 

     return ` 
       <div class="item ${expanded ? 'active' : ''}" onclick="toggleAlbum(${x.idx})"><div class="itemHead">
           <div class="itemMeta">
             <div class="name">${esc(x.name || '未知专辑')}</div>
             <div class="sub">${esc(x.primary_artist || '未知歌手')} · ${x.track_count || 0} 首</div>
           </div>
           <div class="muted">${expanded ? '▲ 收起' : '▼ 展开'}</div>
         </div>

         ${expanded ? `
           <div class="expandBox">
             <div class="expandActions">
               <button onclick="event.stopPropagation(); playGroup(${x.idx})" ${(x.track_count || 0) > 0 ? '' : 'disabled'}>播放这一组</button>
             </div>
             ${detail ? renderAlbumTracks(detail) : '<div class="expandEmpty">加载中...</div>'}
           </div>
         ` : ''}
       </div>
     `; 
   }).join(''); 
 } 

 async function loadAlbums(){ 
   const r = await fetch('/api/albums', {cache:'no-store'}); 
   const j = await r.json(); 
   if(!j.ok) throw new Error(j.message || 'load failed'); 

   allItems = j.items || []; 
   $('statusText').textContent = `当前模式：${j.mode_label || '-'}；当前分组：${(j.current_group_idx ?? -1) >= 0 ? (j.current_group_idx + 1) : '-'}`; 
   renderList(); 
 } 

 async function loadDetail(idx, append){ 
   let state = detailCache[idx]; 
   if(!state){ 
     state = makeDetailState(idx); 
     detailCache[idx] = state; 
   } 

   if(state.loading || state.done) return; 

   state.loading = true; 
   renderList(); 

   try{ 
     const offset = append ? state.loaded : 0; 
     const limit = 20; 
     const r = await fetch(`/api/album/detail?idx=${idx}&offset=${offset}&limit=${limit}`, {cache:'no-store'}); 
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

   if(!detailCache[idx]){ 
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
   loadAlbums().catch(()=>{}); 
 } 

 async function playTrack(trackIdx, groupIdx){ 
   const j = await postForm('/api/track/play', {idx:trackIdx, mode:'album', group_idx:groupIdx}); 
   alert(j && j.ok ? '已开始播放' : '播放失败'); 
   loadAlbums().catch(()=>{}); 
 } 

 $('searchInput').addEventListener('input', renderList); 
 loadAlbums().catch(e=>{ $('statusText').textContent='加载失败'; alert(e.message||'加载失败'); });
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
</script>
</body>
</html>
)HTML";