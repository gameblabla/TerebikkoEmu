/*
 * Player UI and game-state controller.
 * Shared by the release and debug pages; window.TEREBIKKO_DEBUG enables the
 * developer-only panels and window.TEREBIKKO_PICKUP_COIN_PRESET supplies the
 * optional coin sounds settings injected at build time.
 */
const DEBUG = !!window.TEREBIKKO_DEBUG;
const PICKUP_COIN_PRESET = window.TEREBIKKO_PICKUP_COIN_PRESET || null;
const $ = id => document.getElementById(id);
const video = $('video');

const DEFAULT_KEYS = { ans1:'1', ans2:'2', ans3:'3', ans4:'4', pickup:'Enter', pause:'p', fullscreen:'f' };
const DEFAULT_GAMEPAD = {
  ans1:{type:'button', index:0}, ans2:{type:'button', index:1}, ans3:{type:'button', index:2}, ans4:{type:'button', index:3},
  pickup:{type:'button', index:4}, pause:{type:'button', index:9}, fullscreen:{type:'button', index:8}
};
const KEY_STORE = 'terebikko.keys', GAMEPAD_STORE = 'terebikko.gamepad', OPT_STORE = 'terebikko.opts', MANIA_STORE = 'terebikko.maniaHigh', STATE_STORE = 'terebikko.saveStates';
const STATE_DB_NAME = 'TerebikkoEmuStateDatabase', STATE_DB_VERSION = 1, STATE_DB_STORE = 'saveStates';

const G = {
  audioBuf:null, audioDecoded:false, audioDecoding:false, audioDecodeError:null, decodeProgressTimer:null, events:[], play:[], analysis:null, videosmarts:false,
  subtitleCues:[], subtitleLabel:'', subtitleMode:'none', subtitleTrack:null, currentFileKey:null, currentFileMeta:null,
  curIdx:0, active:null, running:false, internalSeek:false, pauseLockUntil:0,
  score:0, streak:0, best:0, lives:3, correct:0, wrong:0, ended:false,
  keys: loadJSON(KEY_STORE, DEFAULT_KEYS),
  pads: loadJSON(GAMEPAD_STORE, DEFAULT_GAMEPAD),
  opts: loadJSON(OPT_STORE, { mode:'easy', hard:false, lives:3, fullscreenControls:true, gamepadRumble:true }),
  saveStates: loadJSON(STATE_STORE, {}), stateDb:null, stateDbReady:false, stateDbPromise:null, stateDbError:null,
  pendingDifficultyAction:null,
  mania:{active:false, prompt:null, round:0, high:loadNumber(MANIA_STORE, 0), nextAt:0, breakUntil:0, clipEnd:0, wasMuted:false, beatTimer:null, music:null, musicSource:null, musicGain:null, musicClip:null, usingMusic:false, musicUrl:null, musicFileName:'', transitionToken:0},
  sfx:null, awaitingRebind:null, awaitingPadRebind:null, padPrev:{}, padLoopStarted:false, padConnected:false, padStatus:'No gamepad detected', rumbleTimer:null, rumbleUntil:0
};

function loadJSON(k, d){ try{ return Object.assign({}, d, JSON.parse(localStorage.getItem(k)||'{}')); }catch(_){ return Object.assign({}, d); } }
G.opts.channel = 'auto';
function loadNumber(k, d){ try{ const n = Number(localStorage.getItem(k)); return Number.isFinite(n) ? n : d; }catch(_){ return d; } }
function saveJSON(k, v){ try{ localStorage.setItem(k, JSON.stringify(v)); }catch(_){ } }

function normalizeSaveName(name){ return String(name || 'media').trim().toLowerCase(); }
function displayFileName(name){ return String(name || 'media'); }
function bytesLabel(n){ n = Number(n)||0; if(n >= 1073741824) return (n/1073741824).toFixed(2) + ' GB'; if(n >= 1048576) return (n/1048576).toFixed(1) + ' MB'; if(n >= 1024) return (n/1024).toFixed(1) + ' KB'; return n + ' B'; }
function makeFileMeta(file){
  return {
    fileName:displayFileName(file && file.name),
    nameKey:normalizeSaveName(file && file.name),
    size:Number(file && file.size) || 0,
    lastModified:Number(file && file.lastModified) || 0,
    duration:0
  };
}
function stateIdForMeta(meta){
  if(!meta) return '';
  return normalizeSaveName(meta.fileName || meta.name || '') + '|' + String(Number(meta.size) || 0);
}
function currentSaveMeta(){
  if(G.currentFileMeta) return G.currentFileMeta;
  if(video && video.dataset && video.dataset.fileName){
    return {
      fileName:video.dataset.fileName || 'media',
      nameKey:normalizeSaveName(video.dataset.fileName || 'media'),
      size:Number(video.dataset.fileSize) || 0,
      lastModified:Number(video.dataset.fileLastModified) || 0,
      duration:Number(video.dataset.fileDuration) || 0
    };
  }
  return null;
}
function legacyStateMetaFromKey(key){
  const parts = String(key || '').split('|');
  if(parts.length >= 3){
    const size = Number(parts[parts.length - 2]) || 0;
    const lastModified = Number(parts[parts.length - 1]) || 0;
    const fileName = parts.slice(0, parts.length - 2).join('|') || '';
    return {fileName, nameKey:normalizeSaveName(fileName), size, lastModified, duration:0};
  }
  return null;
}
function normalizeSaveStateRecord(st, fallbackKey){
  if(!st || typeof st !== 'object') return null;
  const legacy = legacyStateMetaFromKey(st.fileKey || fallbackKey || '');
  const fileName = displayFileName(st.fileName || (legacy && legacy.fileName) || 'media');
  const size = Number(st.fileSize != null ? st.fileSize : (legacy && legacy.size)) || 0;
  const meta = {fileName, nameKey:normalizeSaveName(st.fileNameKey || fileName), size, lastModified:Number(st.fileLastModified != null ? st.fileLastModified : (legacy && legacy.lastModified)) || 0, duration:Number(st.fileDuration) || 0};
  const id = st.id || stateIdForMeta(meta) || String(fallbackKey || '');
  return Object.assign({}, st, {
    id,
    fileKey:id,
    fileName,
    fileNameKey:meta.nameKey,
    fileSize:meta.size,
    fileLastModified:meta.lastModified,
    fileDuration:meta.duration,
    dbVersion:1
  });
}
function loadSaveStates(){ return G.saveStates && typeof G.saveStates === 'object' ? G.saveStates : {}; }
function cacheSaveStateRecord(st){
  const rec = normalizeSaveStateRecord(st, st && (st.id || st.fileKey));
  if(!rec || !rec.id) return null;
  const map = loadSaveStates();
  map[rec.id] = rec;
  G.saveStates = map;
  return rec;
}
function storeSaveStates(map){
  G.saveStates = map && typeof map === 'object' ? map : {};
  try{ localStorage.setItem(STATE_STORE, JSON.stringify(G.saveStates)); return true; }catch(_){ return false; }
}
function idbReq(req){ return new Promise((resolve,reject)=>{ req.onsuccess=()=>resolve(req.result); req.onerror=()=>reject(req.error || new Error('IndexedDB request failed.')); }); }
function openSaveStateDb(){
  if(!('indexedDB' in window)) return Promise.reject(new Error('IndexedDB is not available in this browser.'));
  return new Promise((resolve,reject)=>{
    const req = indexedDB.open(STATE_DB_NAME, STATE_DB_VERSION);
    req.onupgradeneeded = ()=>{
      const db = req.result;
      let store;
      if(!db.objectStoreNames.contains(STATE_DB_STORE)) store = db.createObjectStore(STATE_DB_STORE, {keyPath:'id'});
      else store = req.transaction.objectStore(STATE_DB_STORE);
      if(store && !store.indexNames.contains('fileNameKey')) store.createIndex('fileNameKey', 'fileNameKey', {unique:false});
      if(store && !store.indexNames.contains('fileSize')) store.createIndex('fileSize', 'fileSize', {unique:false});
      if(store && !store.indexNames.contains('savedAt')) store.createIndex('savedAt', 'savedAt', {unique:false});
      if(store && !store.indexNames.contains('mode')) store.createIndex('mode', 'mode', {unique:false});
    };
    req.onsuccess = ()=>resolve(req.result);
    req.onerror = ()=>reject(req.error || new Error('Could not open save-state database.'));
    req.onblocked = ()=>reject(new Error('Save-state database is blocked by another open tab.'));
  });
}
async function ensureSaveStateDb(){
  if(G.stateDbReady && G.stateDb) return G.stateDb;
  if(G.stateDbPromise) return G.stateDbPromise;
  G.stateDbPromise = openSaveStateDb().then(db=>{ G.stateDb = db; G.stateDbReady = true; G.stateDbError = null; return db; }).catch(err=>{ G.stateDbReady = false; G.stateDbError = err && err.message ? err.message : String(err); throw err; });
  return G.stateDbPromise;
}
async function readSaveStateDb(){
  const db = await ensureSaveStateDb();
  const tx = db.transaction(STATE_DB_STORE, 'readonly');
  return await idbReq(tx.objectStore(STATE_DB_STORE).getAll());
}
async function writeSaveStateDb(st){
  const rec = normalizeSaveStateRecord(st, st && (st.id || st.fileKey));
  if(!rec || !rec.id) throw new Error('Save state has no file database key.');
  const db = await ensureSaveStateDb();
  await new Promise((resolve,reject)=>{
    const tx = db.transaction(STATE_DB_STORE, 'readwrite');
    tx.oncomplete = resolve;
    tx.onerror = ()=>reject(tx.error || new Error('Could not write save-state database.'));
    tx.onabort = ()=>reject(tx.error || new Error('Save-state database write was aborted.'));
    tx.objectStore(STATE_DB_STORE).put(rec);
  });
  return rec;
}
async function initSaveStateDatabase(){
  const merged = {};
  const legacy = loadJSON(STATE_STORE, {});
  Object.keys(legacy || {}).forEach(k=>{ const rec = normalizeSaveStateRecord(legacy[k], k); if(rec && rec.id) merged[rec.id] = rec; });
  try{
    const rows = await readSaveStateDb();
    (rows || []).forEach(st=>{ const rec = normalizeSaveStateRecord(st, st && (st.id || st.fileKey)); if(rec && rec.id) merged[rec.id] = rec; });
    G.stateDbReady = true;
    G.stateDbError = null;
    Object.keys(merged).forEach(k=>{ writeSaveStateDb(merged[k]).catch(()=>{}); });
  }catch(err){
    G.stateDbError = err && err.message ? err.message : String(err || 'Save-state database unavailable.');
  }
  storeSaveStates(merged);
  updateStateButtons();
}
function findSavedStateForMeta(meta){
  if(!meta) return null;
  const map = loadSaveStates();
  const exact = map[stateIdForMeta(meta)];
  if(exact) return exact;
  const nameKey = normalizeSaveName(meta.fileName || meta.name || '');
  const size = Number(meta.size) || 0;
  let best = null;
  Object.keys(map).forEach(k=>{
    const st = map[k];
    if(!st) return;
    const stName = normalizeSaveName(st.fileNameKey || st.fileName || '');
    const stSize = Number(st.fileSize) || 0;
    if(stName === nameKey && stSize === size){
      if(!best || Number(st.savedAt || 0) > Number(best.savedAt || 0)) best = st;
    }
  });
  return best;
}
function on(id, ev, fn){ const n = $(id); if(n) n.addEventListener(ev, fn); }
function show(id, yes){ const n = $(id); if(n) n.classList.toggle('hidden', !yes); }
function setText(id, t){ const n = $(id); if(n) n.textContent = t; }
function log(msg){ if(!DEBUG) return; const n = $('log'); if(!n) return; n.textContent += '[' + fmtTime(video.currentTime||0) + '] ' + msg + '\n'; n.scrollTop = n.scrollHeight; }

/* ---------- sound effects (answer feedback only; ring/pick-up live in the video audio) ---------- */
function sfxCtx(){ const AC = window.AudioContext||window.webkitAudioContext; if(!AC) return null; if(!G.sfx) G.sfx = new AC(); if(G.sfx.state==='suspended') G.sfx.resume().catch(()=>{}); return G.sfx; }
function tone(freq, dur, type, gain, when){ const c = sfxCtx(); if(!c) return; const t = c.currentTime+(when||0); const o = c.createOscillator(), g = c.createGain(); o.type=type||'sine'; o.frequency.setValueAtTime(freq,t); g.gain.setValueAtTime(0.0001,t); g.gain.exponentialRampToValueAtTime(gain||0.08,t+0.01); g.gain.exponentialRampToValueAtTime(0.0001,t+dur); o.connect(g).connect(c.destination); o.start(t); o.stop(t+dur+0.03); }
function sfxCoin(){
  const p = PICKUP_COIN_PRESET || {};
  const c = sfxCtx(); if(!c) return false;
  const now = c.currentTime;
  const base = clamp(Math.pow(Number(p.p_base_freq)||0.72, 2) * 1900, 420, 1800);
  const arp = clamp(1 + (Number(p.p_arp_mod)||0.55) * 0.95, 1.2, 2.1);
  const vol = clamp((Number(p.sound_vol)||0.25) * 0.72, 0.12, 0.24);
  const notes = [base, base * arp, base * arp * 1.22];
  notes.forEach((f,i)=>{
    const t = now + i * 0.055;
    const o = c.createOscillator(), g = c.createGain();
    o.type = (Number(p.wave_type) === 1) ? 'square' : 'triangle';
    o.frequency.setValueAtTime(f, t);
    g.gain.setValueAtTime(0.0001, t);
    g.gain.exponentialRampToValueAtTime(vol * (1 - i * 0.12), t + 0.006);
    g.gain.exponentialRampToValueAtTime(0.0001, t + 0.16 + i * 0.03);
    o.connect(g).connect(c.destination);
    o.start(t); o.stop(t + 0.22 + i * 0.03);
  });
  return true;
}
function sfxCorrect(){ if(sfxCoin()) return; tone(880,0.09,'square',0.16,0); tone(1320,0.12,'square',0.13,0.07); tone(1760,0.14,'triangle',0.11,0.14); }
function sfxManiaAnswer(){ if(sfxCoin()) return; tone(880,0.055,'square',0.14,0); tone(1320,0.07,'square',0.12,0.055); tone(1760,0.085,'triangle',0.1,0.12); }
function sfxManiaPickup(){ tone(520,0.055,'square',0.13,0); tone(780,0.07,'square',0.11,0.05); }
function sfxManiaStep(){ tone(620,0.04,'square',0.09,0); }
function sfxWrong(){ tone(200,0.18,'square',0.05,0); tone(150,0.22,'square',0.045,0.05); }
function sfxGameOver(){ const c = sfxCtx(); if(!c) return; const o = c.createOscillator(), g = c.createGain(); o.type='sawtooth'; const now=c.currentTime; o.frequency.setValueAtTime(300,now); o.frequency.exponentialRampToValueAtTime(60,now+0.8); g.gain.setValueAtTime(0.0001,now); g.gain.exponentialRampToValueAtTime(0.08,now+0.04); g.gain.exponentialRampToValueAtTime(0.0001,now+0.85); o.connect(g).connect(c.destination); o.start(now); o.stop(now+0.9); }
function sfxWin(){ [523,659,784,1046].forEach((f,i)=>tone(f,0.18,'triangle',0.05,i*0.12)); }

/* ---------- decoded-audio playback gate ---------- */
function decodedAudioReady(){
  return !!(video.src && G.audioDecoded && G.audioBuf && !G.audioDecoding && !G.audioDecodeError);
}
function decodedAudioBlockReason(){
  if(G.audioDecodeError) return G.audioDecodeError;
  if(G.audioDecoding) return 'Audio is still decoding. Playback is locked until decoding finishes.';
  if(!video.src) return 'Load a video with a decodable audio track first.';
  return 'No fully decoded audio is available. Load a video with a decodable audio track.';
}
function requireDecodedAudio(action){
  if(decodedAudioReady()) return true;
  const msg = decodedAudioBlockReason();
  log((action || 'play') + ' blocked: ' + msg);
  try{ video.pause(); }catch(_){ }
  if(G.audioDecodeError) setProgress(0, 'Audio decode error: ' + msg);
  else if(G.audioDecoding) setProgress(2, msg);
  else setProgress(0, msg);
  updateHud();
  return false;
}
function stopDecodeProgressPulse(){
  if(G.decodeProgressTimer){ clearInterval(G.decodeProgressTimer); G.decodeProgressTimer = null; }
}
function startDecodeProgressPulse(from, to, text){
  stopDecodeProgressPulse();
  const start = performance.now();
  const lo = clamp(Number(from)||0, 0, 100);
  const hi = clamp(Number(to)||lo, lo, 99);
  setProgress(lo, text || 'Decoding audio...');
  G.decodeProgressTimer = setInterval(()=>{
    const elapsed = performance.now() - start;
    const eased = 1 - Math.exp(-elapsed / 2600);
    setProgress(lo + (hi - lo) * eased, text || 'Decoding audio...');
  }, 120);
}
function showDecodeProgress(on){
  const n = $('decodeOverlay');
  if(n) n.classList.toggle('hidden', !on);
}
function readFileBufferWithProgress(file){
  return new Promise((resolve, reject)=>{
    const reader = new FileReader();
    reader.onerror = ()=>reject(new Error('Could not read the selected file.'));
    reader.onabort = ()=>reject(new Error('File read was aborted.'));
    reader.onprogress = ev=>{
      if(ev.lengthComputable && ev.total > 0){
        const pct = ev.loaded / ev.total;
        setProgress(2 + pct * 18, 'Reading file... ' + Math.round(pct * 100) + '%');
      } else setProgress(6, 'Reading file...');
    };
    reader.onload = ()=>resolve(reader.result);
    reader.readAsArrayBuffer(file);
  });
}
function playVideo(action){
  if(!requireDecodedAudio(action || 'play')) return;
  video.play().catch(err=>{ log('play failed: ' + (err && err.message ? err.message : err)); updateHud(); });
}
function beginAudioDecodeLoad(){
  try{ video.pause(); }catch(_){ }
  G.currentFileKey = null;
  G.currentFileMeta = null;
  if(video && video.dataset){ delete video.dataset.fileKey; delete video.dataset.fileName; delete video.dataset.fileSize; delete video.dataset.fileLastModified; delete video.dataset.fileDuration; }
  stopDecodeProgressPulse();
  showDecodeProgress(true);
  G.audioBuf = null;
  G.audioDecoded = false;
  G.audioDecoding = true;
  G.audioDecodeError = null;
  G.analysis = null;
  setVideosmartsMode(false);
  clearSubtitles();
  G.events = [];
  G.play = [];
  G.mania.musicClip = null;
  G.curIdx = 0;
  G.active = null;
  G.running = false;
  G.ended = false;
}
function failAudioDecodeLoad(err){
  stopDecodeProgressPulse();
  G.currentFileKey = null;
  G.currentFileMeta = null;
  if(video && video.dataset){ delete video.dataset.fileKey; delete video.dataset.fileName; delete video.dataset.fileSize; delete video.dataset.fileLastModified; delete video.dataset.fileDuration; }
  showDecodeProgress(false);
  const raw = err && err.message ? err.message : String(err || 'Audio decoding failed.');
  const msg = raw || 'Audio decoding failed.';
  G.audioBuf = null;
  G.audioDecoded = false;
  G.audioDecoding = false;
  G.audioDecodeError = msg;
  G.analysis = null;
  setVideosmartsMode(false);
  G.events = [];
  G.play = [];
  G.mania.musicClip = null;
  stopMania();
  clearActive();
  try{ video.pause(); }catch(_){ }
  if(video.dataset.url){ try{ URL.revokeObjectURL(video.dataset.url); }catch(_){ } delete video.dataset.url; }
  video.removeAttribute('src');
  try{ video.load(); }catch(_){ }
  showStartOverlay(false);
  showDifficulty(false);
  show('emptyOverlay', true);
  setText('analysisPill', 'Audio error');
  setProgress(0, 'Audio decode error: ' + msg);
  updateHud();
}


/* ---------- subtitle support ---------- */
function fileExt(name){ const m = String(name||'').toLowerCase().match(/\.([^.]+)$/); return m ? m[1] : ''; }
function fileStem(name){ return String(name||'').replace(/\.[^.]*$/, ''); }
function normStem(name){ return fileStem(name).toLowerCase().replace(/[\s._-]+$/,''); }
function isSubtitleFile(file){
  const ext = fileExt(file && file.name);
  return !!file && (['vtt','srt','ass','ssa'].includes(ext) || /subrip|webvtt|text\/vtt/i.test(file.type||''));
}
function isMediaFile(file){
  if(!file || isSubtitleFile(file)) return false;
  if(/^(video|audio)\//i.test(file.type||'')) return true;
  return ['mp4','m4v','mov','webm','mkv','ogv','ogg','avi','mpg','mpeg','mp3','m4a','aac','wav','flac'].includes(fileExt(file.name));
}
function pickMediaAndSubtitle(files){
  const list = Array.from(files || []).filter(Boolean);
  const media = list.find(isMediaFile) || list.find(f=>!isSubtitleFile(f));
  if(!media) return {media:null, subtitle:null};
  const subs = list.filter(isSubtitleFile);
  if(!subs.length) return {media, subtitle:null};
  const mediaStem = normStem(media.name);
  let best = null, bestScore = -1;
  subs.forEach(sub=>{
    const subStem = normStem(sub.name);
    let score = 0;
    if(subStem === mediaStem) score = 4;
    else if(subStem.startsWith(mediaStem + '.') || subStem.startsWith(mediaStem + '_') || subStem.startsWith(mediaStem + '-')) score = 3;
    else if(mediaStem && subStem.includes(mediaStem)) score = 2;
    else score = 1;
    if(score > bestScore){ best = sub; bestScore = score; }
  });
  return {media, subtitle:best};
}
function readEntryFile(entry, prefix){
  return new Promise(resolve=>{
    if(!entry) return resolve([]);
    if(entry.isFile){
      entry.file(file=>{
        try{ Object.defineProperty(file, 'relativePath', {value:(prefix || '') + file.name, configurable:true}); }catch(_){ }
        resolve([file]);
      }, ()=>resolve([]));
      return;
    }
    if(entry.isDirectory){
      const reader = entry.createReader();
      const out = [];
      const readBatch = ()=>reader.readEntries(async entries=>{
        if(!entries.length) return resolve(out);
        for(const child of entries){ out.push(...await readEntryFile(child, (prefix || '') + entry.name + '/')); }
        readBatch();
      }, ()=>resolve(out));
      readBatch();
      return;
    }
    resolve([]);
  });
}
async function filesFromDrop(dataTransfer){
  if(!dataTransfer) return [];
  const items = Array.from(dataTransfer.items || []);
  if(items.length && items.some(item=>item.webkitGetAsEntry)){
    const out = [];
    for(const item of items){
      const entry = item.webkitGetAsEntry && item.webkitGetAsEntry();
      if(entry) out.push(...await readEntryFile(entry, ''));
      else { const f = item.getAsFile && item.getAsFile(); if(f) out.push(f); }
    }
    if(out.length) return out;
  }
  return Array.from(dataTransfer.files || []);
}
function htmlEntityDecode(text){
  if(!/[&]/.test(text||'')) return text || '';
  const ta = htmlEntityDecode.ta || (htmlEntityDecode.ta = document.createElement('textarea'));
  ta.innerHTML = text;
  return ta.value;
}
function cleanSubtitleText(text){
  return htmlEntityDecode(String(text||'')
    .replace(/\{[^}]*\}/g, '')
    .replace(/\\[Nn]/g, '\n')
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<[^>]+>/g, '')
    .split('\n').map(line=>line.trim()).join('\n').replace(/\n{3,}/g, '\n\n').trim());
}
function parseSubtitleTime(raw){
  const s = String(raw||'').trim().replace(',', '.');
  const m = s.match(/^(?:(\d+):)?(\d{1,2}):(\d{2})(?:\.(\d{1,3}))?/);
  if(!m) return NaN;
  const h = Number(m[1] || 0), min = Number(m[2] || 0), sec = Number(m[3] || 0);
  let frac = 0;
  if(m[4]) frac = Number(m[4]) / Math.pow(10, m[4].length);
  return h*3600 + min*60 + sec + frac;
}
function parseVttOrSrt(text){
  const src = String(text||'').replace(/^\uFEFF/, '').replace(/\r/g, '');
  const blocks = src.split(/\n{2,}/);
  const cues = [];
  blocks.forEach(block=>{
    const lines = block.split('\n').map(line=>line.trim()).filter(line=>line.length);
    if(!lines.length) return;
    if(/^WEBVTT/i.test(lines[0]) || /^NOTE\b/i.test(lines[0]) || /^STYLE\b/i.test(lines[0]) || /^REGION\b/i.test(lines[0])) return;
    const arrowIndex = lines.findIndex(line=>line.includes('-->'));
    if(arrowIndex < 0) return;
    const parts = lines[arrowIndex].split(/\s+-->\s+/);
    if(parts.length < 2) return;
    const start = parseSubtitleTime(parts[0]);
    const end = parseSubtitleTime(parts[1].split(/\s+/)[0]);
    if(!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return;
    const body = cleanSubtitleText(lines.slice(arrowIndex + 1).join('\n'));
    if(body) cues.push({start, end, text:body});
  });
  cues.sort((a,b)=>a.start-b.start || a.end-b.end);
  return cues;
}
function splitAssFields(s, count){
  const out = [];
  let rest = String(s||'');
  for(let i=0;i<count-1;i++){
    const idx = rest.indexOf(',');
    if(idx < 0){ out.push(rest); rest = ''; continue; }
    out.push(rest.slice(0, idx));
    rest = rest.slice(idx + 1);
  }
  out.push(rest);
  return out;
}
function parseAss(text){
  const lines = String(text||'').replace(/^\uFEFF/, '').replace(/\r/g, '').split('\n');
  let inEvents = false, format = null;
  const cues = [];
  for(const raw of lines){
    const line = raw.trim();
    if(/^\[Events\]/i.test(line)){ inEvents = true; continue; }
    if(/^\[/.test(line)){ inEvents = false; continue; }
    if(!inEvents) continue;
    if(/^Format:/i.test(line)){
      format = line.replace(/^Format:\s*/i,'').split(',').map(x=>x.trim().toLowerCase());
      continue;
    }
    if(!format || !/^Dialogue:/i.test(line)) continue;
    const vals = splitAssFields(line.replace(/^Dialogue:\s*/i,''), format.length);
    const si = format.indexOf('start'), ei = format.indexOf('end'), ti = format.indexOf('text');
    if(si < 0 || ei < 0 || ti < 0) continue;
    const start = parseSubtitleTime(vals[si]);
    const end = parseSubtitleTime(vals[ei]);
    const body = cleanSubtitleText(vals[ti]);
    if(Number.isFinite(start) && Number.isFinite(end) && end > start && body) cues.push({start, end, text:body});
  }
  cues.sort((a,b)=>a.start-b.start || a.end-b.end);
  return cues;
}
function parseSubtitleText(text, filename){
  const ext = fileExt(filename);
  if(ext === 'ass' || ext === 'ssa') return parseAss(text);
  return parseVttOrSrt(text);
}


function isMkvName(name){
  const ext = fileExt(name);
  return ext === 'mkv' || ext === 'mka' || ext === 'mks' || ext === 'webm';
}
function ebmlReadVint(view, pos, keepMarker){
  if(pos >= view.byteLength) return null;
  const first = view.getUint8(pos);
  if(first === 0) return null;
  let mask = 0x80, len = 1;
  while(len <= 8 && !(first & mask)){ mask >>= 1; len++; }
  if(len > 8 || pos + len > view.byteLength) return null;
  let value = keepMarker ? first : (first & (mask - 1));
  let unknown = !keepMarker && ((first & (mask - 1)) === (mask - 1));
  for(let i=1;i<len;i++){
    const b = view.getUint8(pos + i);
    value = value * 256 + b;
    if(!keepMarker && b !== 0xFF) unknown = false;
  }
  return {value, length:len, unknown};
}
function ebmlReadElement(view, pos, limit){
  const id = ebmlReadVint(view, pos, true);
  if(!id) return null;
  const size = ebmlReadVint(view, pos + id.length, false);
  if(!size) return null;
  const dataStart = pos + id.length + size.length;
  let dataEnd = size.unknown ? limit : dataStart + size.value;
  if(dataEnd > limit) dataEnd = limit;
  if(dataStart > dataEnd) return null;
  return {id:id.value, size:size.value, dataStart, dataEnd, next:dataEnd, unknown:size.unknown};
}
function ebmlEachChild(view, start, end, fn){
  let pos = start, guard = 0;
  end = Math.min(end, view.byteLength);
  while(pos < end && guard++ < 1000000){
    const el = ebmlReadElement(view, pos, end);
    if(!el || el.next <= pos) break;
    fn(el);
    pos = el.next;
  }
}
function ebmlUint(view, start, end){
  let v = 0;
  for(let p=start;p<end;p++) v = v * 256 + view.getUint8(p);
  return v;
}
function ebmlFloat(view, start, end){
  const n = end - start;
  if(n === 4) return view.getFloat32(start, false);
  if(n === 8) return view.getFloat64(start, false);
  return NaN;
}
function ebmlBytes(view, start, end){
  return new Uint8Array(view.buffer, view.byteOffset + start, Math.max(0, end - start));
}
function ebmlString(view, start, end){
  try{ return new TextDecoder('utf-8').decode(ebmlBytes(view, start, end)).replace(/\0+$/g,'').trim(); }
  catch(_){ return ''; }
}
function parseMkvInfo(view, start, end){
  const info = {timecodeScale:1000000, duration:null};
  ebmlEachChild(view, start, end, el=>{
    if(el.id === 0x2AD7B1) info.timecodeScale = ebmlUint(view, el.dataStart, el.dataEnd) || 1000000;
    else if(el.id === 0x4489) info.duration = ebmlFloat(view, el.dataStart, el.dataEnd);
  });
  return info;
}
function parseMkvTrackEntry(view, start, end){
  const tr = {number:null, type:null, codec:'', name:'', language:'', defaultDuration:null, flagDefault:0, codecPrivate:null};
  ebmlEachChild(view, start, end, el=>{
    if(el.id === 0xD7) tr.number = ebmlUint(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x83) tr.type = ebmlUint(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x86) tr.codec = ebmlString(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x536E) tr.name = ebmlString(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x22B59C || el.id === 0x22B59D) tr.language = ebmlString(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x23E383) tr.defaultDuration = ebmlUint(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x88) tr.flagDefault = ebmlUint(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x63A2) tr.codecPrivate = ebmlString(view, el.dataStart, el.dataEnd);
  });
  return tr;
}
function parseMkvTracks(view, start, end){
  const tracks = [];
  ebmlEachChild(view, start, end, el=>{
    if(el.id === 0xAE){
      const tr = parseMkvTrackEntry(view, el.dataStart, el.dataEnd);
      if(tr.number != null) tracks.push(tr);
    }
  });
  return tracks;
}
function isMkvTextSubtitleTrack(track){
  if(!track || track.type !== 0x11) return false;
  const c = String(track.codec || '').toUpperCase();
  return c === 'S_TEXT/UTF8' || c === 'S_TEXT/ASS' || c === 'S_TEXT/SSA' || c === 'S_TEXT/WEBVTT' || c.indexOf('D_WEBVTT/') === 0;
}
function mkvTrackLabel(track){
  const bits = [];
  if(track.name) bits.push(track.name);
  if(track.language) bits.push(track.language);
  if(track.codec) bits.push(track.codec.replace(/^S_TEXT\//i,'').replace(/^D_WEBVTT\//i,'WebVTT '));
  return bits.length ? bits.join(' · ') : 'MKV subtitle track ' + track.number;
}
function parseMkvBlock(view, start, end){
  const tn = ebmlReadVint(view, start, false);
  if(!tn) return null;
  let p = start + tn.length;
  if(p + 3 > end) return null;
  const relTime = view.getInt16(p, false); p += 2;
  const flags = view.getUint8(p++);
  const lacing = (flags >> 1) & 0x03;
  if(lacing) return null;
  return {trackNumber:tn.value, relTime, payloadStart:p, payloadEnd:end};
}
function parseMkvBlockGroup(view, start, end){
  let block = null, duration = null;
  ebmlEachChild(view, start, end, el=>{
    if(el.id === 0xA1) block = parseMkvBlock(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x9B) duration = ebmlUint(view, el.dataStart, el.dataEnd);
  });
  if(block) block.duration = duration;
  return block;
}
function decodeMkvSubtitlePayload(view, track, start, end){
  let raw = ebmlString(view, start, end).replace(/^\uFEFF/, '').replace(/\0/g, '').trim();
  if(!raw) return '';
  const codec = String(track.codec || '').toUpperCase();
  if(codec === 'S_TEXT/ASS' || codec === 'S_TEXT/SSA'){
    if(/^Dialogue:/i.test(raw)){
      const vals = splitAssFields(raw.replace(/^Dialogue:\s*/i,''), 10);
      raw = vals.length >= 10 ? vals[9] : raw;
    } else {
      const vals = splitAssFields(raw, 9);
      raw = vals.length >= 9 ? vals[8] : raw;
    }
  } else if(codec === 'S_TEXT/WEBVTT' || codec.indexOf('D_WEBVTT/') === 0){
    const lines = raw.replace(/\r/g,'').split('\n');
    const arrow = lines.findIndex(line=>line.indexOf('-->') >= 0);
    if(arrow >= 0) raw = lines.slice(arrow + 1).join('\n');
  }
  return cleanSubtitleText(raw);
}
function addMkvSubtitleCue(view, cuesByTrack, track, block, clusterTime, timecodeScale){
  if(!track || !block) return;
  const text = decodeMkvSubtitlePayload(view, track, block.payloadStart, block.payloadEnd);
  if(!text) return;
  const start = Math.max(0, (clusterTime + block.relTime) * timecodeScale / 1000000000);
  let duration = block.duration != null ? block.duration * timecodeScale / 1000000000 : 0;
  let fallback = false;
  if(!Number.isFinite(duration) || duration <= 0){
    if(track.defaultDuration) duration = track.defaultDuration / 1000000000;
    if(!Number.isFinite(duration) || duration <= 0){ duration = 4.0; fallback = true; }
  }
  const cue = {start, end:start + Math.max(0.2, duration), text, fallback};
  let list = cuesByTrack.get(track.number);
  if(!list){ list = []; cuesByTrack.set(track.number, list); }
  list.push(cue);
}
function parseMkvCluster(view, start, end, textTrackByNumber, cuesByTrack, timecodeScale){
  let clusterTime = 0;
  const pending = [];
  ebmlEachChild(view, start, end, el=>{
    if(el.id === 0xE7) clusterTime = ebmlUint(view, el.dataStart, el.dataEnd);
    else if(el.id === 0xA3){
      const block = parseMkvBlock(view, el.dataStart, el.dataEnd);
      if(block && textTrackByNumber.has(block.trackNumber)) pending.push(block);
    } else if(el.id === 0xA0){
      const block = parseMkvBlockGroup(view, el.dataStart, el.dataEnd);
      if(block && textTrackByNumber.has(block.trackNumber)) pending.push(block);
    }
  });
  pending.forEach(block=>addMkvSubtitleCue(view, cuesByTrack, textTrackByNumber.get(block.trackNumber), block, clusterTime, timecodeScale));
}
function finalizeMkvCues(cues){
  cues.sort((a,b)=>a.start-b.start || a.end-b.end);
  for(let i=0;i<cues.length;i++){
    const cue = cues[i], next = cues[i+1];
    if(cue.fallback && next && next.start > cue.start){
      cue.end = Math.min(cue.end, Math.max(cue.start + 0.2, next.start - 0.02));
    }
    delete cue.fallback;
  }
  return cues.filter(c=>Number.isFinite(c.start) && Number.isFinite(c.end) && c.end > c.start && c.text);
}
function extractMkvEmbeddedSubtitles(buf, filename){
  if(!buf || !isMkvName(filename)) return null;
  const view = new DataView(buf);
  let segment = null;
  ebmlEachChild(view, 0, view.byteLength, el=>{ if(!segment && el.id === 0x18538067) segment = el; });
  if(!segment) return null;
  let info = {timecodeScale:1000000}, tracks = [], clusters = [];
  ebmlEachChild(view, segment.dataStart, segment.dataEnd, el=>{
    if(el.id === 0x1549A966) info = parseMkvInfo(view, el.dataStart, el.dataEnd);
    else if(el.id === 0x1654AE6B) tracks = tracks.concat(parseMkvTracks(view, el.dataStart, el.dataEnd));
    else if(el.id === 0x1F43B675) clusters.push(el);
  });
  const textTracks = tracks.filter(isMkvTextSubtitleTrack);
  if(!textTracks.length) return {cues:[], reason:'No text subtitle track found in the MKV.'};
  const byNumber = new Map(textTracks.map(t=>[t.number, t]));
  const cuesByTrack = new Map();
  clusters.forEach(cluster=>parseMkvCluster(view, cluster.dataStart, cluster.dataEnd, byNumber, cuesByTrack, info.timecodeScale || 1000000));
  const candidates = textTracks.map(track=>({track, cues:finalizeMkvCues(cuesByTrack.get(track.number) || [])})).filter(x=>x.cues.length);
  if(!candidates.length) return {cues:[], reason:'No readable text cues found in MKV subtitle tracks.'};
  candidates.sort((a,b)=>{
    const df = (b.track.flagDefault ? 1 : 0) - (a.track.flagDefault ? 1 : 0);
    if(df) return df;
    return b.cues.length - a.cues.length;
  });
  return {cues:candidates[0].cues, label:mkvTrackLabel(candidates[0].track), track:candidates[0].track, trackCount:textTracks.length};
}
function loadMkvEmbeddedSubtitlesFromBuffer(buf, file){
  if(G.subtitleMode === 'sidecar' || (G.subtitleCues && G.subtitleCues.length)) return false;
  const res = extractMkvEmbeddedSubtitles(buf, file && file.name);
  if(!res) return false;
  if(res.cues && res.cues.length){
    G.subtitleCues = res.cues;
    G.subtitleLabel = 'MKV embedded: ' + (res.label || 'text track');
    G.subtitleMode = 'mkv';
    G.subtitleTrack = null;
    renderSubtitles();
    log('Loaded MKV embedded subtitles: ' + G.subtitleLabel + ' (' + res.cues.length + ' cues)');
    return true;
  }
  if(res.reason) log('MKV subtitle scan: ' + res.reason);
  return false;
}
function clearSubtitles(){
  G.subtitleCues = [];
  G.subtitleLabel = '';
  G.subtitleMode = 'none';
  G.subtitleTrack = null;
  if(video.textTracks){ Array.from(video.textTracks).forEach(t=>{ try{ t.mode = 'disabled'; }catch(_){ } }); }
  renderSubtitles();
}
function subtitleSummary(){
  if(G.subtitleCues && G.subtitleCues.length) return ' Subtitles: ' + (G.subtitleLabel || 'sidecar') + ' (' + G.subtitleCues.length + ' cues).';
  if(G.subtitleTrack) return ' Subtitles: ' + (G.subtitleLabel || 'embedded') + '.';
  return '';
}
async function loadSidecarSubtitles(file, mediaFile){
  if(!file) return false;
  const text = await file.text();
  const cues = parseSubtitleText(text, file.name);
  if(!cues.length) throw new Error('Subtitle file has no readable cues: ' + file.name);
  G.subtitleCues = cues;
  G.subtitleLabel = file.name;
  G.subtitleMode = 'sidecar';
  G.subtitleTrack = null;
  renderSubtitles();
  log('Loaded sidecar subtitles: ' + file.name + ' (' + cues.length + ' cues) for ' + (mediaFile && mediaFile.name ? mediaFile.name : 'media'));
  return true;
}
function refreshEmbeddedSubtitles(){
  if(G.subtitleMode === 'sidecar' || (G.subtitleCues && G.subtitleCues.length)) return false;
  const tracks = video.textTracks ? Array.from(video.textTracks) : [];
  const cand = tracks.find(t=>/^(subtitles|captions)$/i.test(t.kind || '')) || tracks.find(t=>/subtitle|caption/i.test((t.label||'') + ' ' + (t.language||'')));
  tracks.forEach(t=>{ try{ t.mode = (t === cand) ? 'hidden' : 'disabled'; }catch(_){ } });
  if(cand){
    G.subtitleTrack = cand;
    G.subtitleMode = 'embedded';
    G.subtitleLabel = cand.label || cand.language || 'embedded track';
    log('Enabled embedded subtitles: ' + G.subtitleLabel);
    renderSubtitles();
    return true;
  }
  G.subtitleTrack = null;
  if(G.subtitleMode !== 'sidecar') G.subtitleMode = 'none';
  renderSubtitles();
  return false;
}
function renderSubtitles(){
  const out = $('subtitleOverlay');
  if(!out) return;
  // Keep the preview thumbnail clean. Sidecar/embedded subtitle cues are rendered only after
  // the user actually starts playback; otherwise a cue at the 120-frame preview point can
  // appear burned over the idle thumbnail.
  if(!G.running && !G.ended){
    out.textContent = '';
    out.classList.add('hidden');
    return;
  }
  const t = video.currentTime || 0;
  let texts = [];
  if(G.subtitleCues && G.subtitleCues.length){
    texts = G.subtitleCues.filter(c=>t + 0.02 >= c.start && t <= c.end + 0.02).map(c=>c.text).filter(Boolean);
  } else if(G.subtitleTrack && G.subtitleTrack.activeCues){
    try{ texts = Array.from(G.subtitleTrack.activeCues).map(c=>cleanSubtitleText(c.text || '')).filter(Boolean); }catch(_){ texts = []; }
  }
  if(texts.length){
    out.textContent = texts.join('\n');
    out.classList.remove('hidden');
  } else {
    out.textContent = '';
    out.classList.add('hidden');
  }
}

/* ---------- file loading + analysis ---------- */
function handleFiles(files){
  const picked = pickMediaAndSubtitle(files);
  if(!picked.media){ setProgress(0, 'No playable video/audio file found.'); return; }
  handleFile(picked.media, picked.subtitle);
}
async function handleFile(file, sidecarSubtitle){
  if(!file) return;
  beginAudioDecodeLoad();
  G.currentFileMeta = makeFileMeta(file);
  G.currentFileKey = stateIdForMeta(G.currentFileMeta);
  video.dataset.fileKey = G.currentFileKey;
  video.dataset.fileName = G.currentFileMeta.fileName;
  video.dataset.fileSize = String(G.currentFileMeta.size || 0);
  video.dataset.fileLastModified = String(G.currentFileMeta.lastModified || 0);
  if(video.dataset.url) URL.revokeObjectURL(video.dataset.url);
  const url = URL.createObjectURL(file);
  video.dataset.url = url; video.src = url; video.controls = false;
  video.addEventListener('loadedmetadata', ()=>{
    if(G.currentFileMeta){
      G.currentFileMeta.duration = Number.isFinite(video.duration) ? video.duration : 0;
      if(video.dataset) video.dataset.fileDuration = String(G.currentFileMeta.duration || 0);
      updateStateButtons();
    }
    if(!G.running && !G.ended){
      G.internalSeek = true;
      // Use a real preview frame instead of the first black/opening frame. 120 frames at the emulator's nominal 30 fps is 4 seconds.
      try{
        const d = Number.isFinite(video.duration) ? video.duration : 0;
        const previewAt = Math.min(120 / 30, Math.max(0, d - 0.01));
        video.currentTime = previewAt;
      }catch(_){ }
      setTimeout(()=>{ G.internalSeek = false; renderSubtitles(); }, 120);
    }
    refreshEmbeddedSubtitles();
    setTimeout(refreshEmbeddedSubtitles, 250);
    setTimeout(refreshEmbeddedSubtitles, 1200);
  }, {once:true});
  show('emptyOverlay', false);
  showStartOverlay(false);
  showDifficulty(false);
  resetGame();
  if(sidecarSubtitle){
    loadSidecarSubtitles(sidecarSubtitle, file).catch(err=>{
      log('subtitle load failed: ' + (err && err.message ? err.message : err));
      setProgress(2, 'Subtitle error: ' + (err && err.message ? err.message : err) + ' Audio decode continues...');
    });
  }
  setText('analysisPill', 'Loaded: ' + file.name);
  setProgress(2, 'Decoding audio... Play is locked until decoding completes.');
  updateHud();
  try { await analyze(file); }
  catch(err){ failAudioDecodeLoad(err); log('scan failed: ' + (err && err.message ? err.message : err)); }
}
function setProgress(p, text){
  const value = clamp(p,0,100).toFixed(1)+'%';
  const b = $('progressBar'); if(b) b.style.width = value;
  const db = $('decodeProgressBar'); if(db) db.style.width = value;
  if(text){ setText('progressText', text); setText('decodeProgressText', text); }
}

async function analyze(file){
  const AC = window.AudioContext||window.webkitAudioContext;
  if(!AC) throw new Error('Web Audio is not available in this browser. Audio cannot be decoded.');
  const ctx = new AC();
  try{
    const buf = await readFileBufferWithProgress(file);
    if(!(G.subtitleMode === 'sidecar' || (G.subtitleCues && G.subtitleCues.length)) && isMkvName(file && file.name)){
      setProgress(21, 'Scanning MKV embedded subtitles...');
      try{ loadMkvEmbeddedSubtitlesFromBuffer(buf, file); }
      catch(err){ log('MKV subtitle scan failed: ' + (err && err.message ? err.message : err)); }
    }
    let audioBuf;
    try{
      startDecodeProgressPulse(22, 55, 'Decoding audio... Play is locked until decoding completes.');
      audioBuf = await ctx.decodeAudioData(buf.slice(0));
      stopDecodeProgressPulse();
      setProgress(55, 'Audio decoded. Scanning channels for control tones...');
    }catch(err){
      stopDecodeProgressPulse();
      throw new Error('No decodable audio track found. Load a video/audio file with an audio track supported by this browser.');
    }
    if(!audioBuf || !audioBuf.numberOfChannels || !Number.isFinite(audioBuf.duration) || audioBuf.duration <= 0){
      throw new Error('No decodable audio track found.');
    }
    G.audioBuf = audioBuf;
    await reanalyze();
    G.audioDecoded = true;
    G.audioDecoding = false;
    G.audioDecodeError = null;
    updateHud();
  }finally{
    stopDecodeProgressPulse();
    await ctx.close().catch(()=>{});
  }
}
async function reanalyze(){
  if(!G.audioBuf) return;
  setProgress(58, 'Scanning channels for control tones...');
  const res = await analyzeChannels(G.audioBuf, 'auto', (v,t)=>setProgress(58 + v*0.42, t));
  G.analysis = res;
  setVideosmartsMode(res.chosen.packets.some(p=>p.mode==='videosmarts'));
  G.events = buildEvents(res.chosen.packets, {carrier:res.chosen.carrier});
  G.play = playEvents(G.events);
  G.mania.musicClip = null;
  const nQ = G.play.filter(e=>e.type==='answer').length;
  const nR = G.play.filter(e=>e.type==='pickup').length;
  const kHz = Math.round((res.chosen.carrier||8000)/1000);
  const chName = res.all.map(r=>res.channelName(r.channel)+'='+r.answers+'@'+Math.round((r.carrier||8000)/1000)+'k').join(', ');
  refreshEmbeddedSubtitles();
  setProgress(100, 'Scan complete. ' + nQ + ' questions, ' + nR + ' calls, ' + kHz + ' kHz carrier. Channels: ' + chName + '.' + subtitleSummary());
  showDecodeProgress(false);
  setText('analysisPill', 'Ready · ' + res.channelName(res.chosen.channel) + ' · ' + kHz + ' kHz · ' + nQ + ' Q');
  resetGame();
  G.pendingDifficultyAction = null;
  showDifficulty(false);
  showStartOverlay(true);
  updateStateButtons();
  if(DEBUG) renderDebug();
  log('Decoded on ' + res.channelName(res.chosen.channel) + ' channel: ' + nQ + ' questions, ' + nR + ' pick-up calls.');
}

/* ---------- game state ---------- */
function gameMode(){ return G.opts.mode || (G.opts.hard ? 'hard' : 'easy'); }
function maniaMode(){ return gameMode() === 'mania'; }
function hardMode(){ return gameMode() === 'hard' || gameMode() === 'veryHard'; }
function veryHardMode(){ return gameMode() === 'veryHard'; }
function modeLabel(){ return maniaMode() ? 'MINIGAME' : veryHardMode() ? 'VERY HARD' : hardMode() ? 'HARD' : 'EASY'; }
function stateModeAllowed(mode){ mode = mode || gameMode(); return mode === 'easy' || mode === 'hard'; }
function makeFileKey(file){ return stateIdForMeta(makeFileMeta(file)); }
function currentSaveKey(){ return G.currentFileKey || (video && video.dataset ? (video.dataset.fileKey || '') : ''); }
function currentSavedState(){ return findSavedStateForMeta(currentSaveMeta()) || (currentSaveKey() ? loadSaveStates()[currentSaveKey()] || null : null); }
function saveStateAllowedNow(){ return decodedAudioReady() && !!currentSaveKey() && stateModeAllowed(); }
function saveStateStatus(){
  if(!stateModeAllowed()) return 'Save states are only available in Easy and Hard.';
  if(!video.src) return 'Load a video first. Save states are matched by exact file name plus file size.';
  if(!decodedAudioReady()) return decodedAudioBlockReason();
  const meta = currentSaveMeta();
  const fileLabel = meta ? (meta.fileName + ' (' + bytesLabel(meta.size) + ')') : 'this loaded video';
  const st = currentSavedState();
  const dbNote = G.stateDbError ? ' IndexedDB is unavailable; using browser storage backup.' : '';
  if(!st) return 'No save-state database entry for ' + fileLabel + '. Save states are matched by exact file name plus file size.' + dbNote;
  const when = st.savedAt ? new Date(st.savedAt).toLocaleString() : 'unknown time';
  return 'Database match: ' + fileLabel + '. Saved ' + (st.mode === 'hard' ? 'Hard' : 'Easy') + ' state at ' + fmtTime(st.time || 0) + ' on ' + when + '.' + dbNote;
}
function snapshotPlayState(){
  return (G.play || []).map(e=>({done:!!e.done, got:e.got == null ? null : e.got}));
}
function restorePlayState(list){
  const arr = Array.isArray(list) ? list : [];
  for(let i=0;i<G.play.length;i++){
    const src = arr[i] || {};
    G.play[i].done = !!src.done;
    G.play[i].got = src.got == null ? null : src.got;
  }
}
function updateStateButtons(){
  const allowedMode = stateModeAllowed();
  const ready = decodedAudioReady();
  const hasFile = !!currentSaveKey();
  const canOpen = allowedMode && ready && hasFile;
  const hasState = !!currentSavedState();
  const title = allowedMode ? (ready && hasFile ? 'Save/load state database entry for this exact file name and size.' : saveStateStatus()) : 'Save states are not available in Very hard or Minigame.';
  ['stateBtn','stateSideBtn'].forEach(id=>{ const b = $(id); if(b){ b.disabled = !canOpen; b.title = title; } });
  const save = $('saveStateAction'); if(save) save.disabled = !canOpen;
  const load = $('loadStateAction'); if(load) load.disabled = !canOpen || !hasState;
  const status = $('stateStatus'); if(status) status.textContent = saveStateStatus();
}
function openStateModal(){
  updateStateButtons();
  if(!stateModeAllowed()){ bigStatus('SAVE UNAVAILABLE','warn',900); setProgress(100, 'Save states are only available in Easy and Hard.'); return; }
  if(!decodedAudioReady() || !currentSaveKey()){ bigStatus('NO STATE','warn',900); setProgress(0, saveStateStatus()); return; }
  show('stateModal', true);
}
async function saveState(){
  if(!saveStateAllowedNow()){ updateStateButtons(); bigStatus('SAVE UNAVAILABLE','warn',900); setProgress(100, saveStateStatus()); return; }
  const meta = currentSaveMeta();
  const key = stateIdForMeta(meta);
  const map = loadSaveStates();
  const state = {
    version:2,
    id:key,
    fileKey:key,
    fileName:meta ? meta.fileName : ((video.dataset && video.dataset.fileName) || ''),
    fileNameKey:meta ? meta.nameKey : normalizeSaveName((video.dataset && video.dataset.fileName) || ''),
    fileSize:meta ? meta.size : Number(video.dataset && video.dataset.fileSize) || 0,
    fileLastModified:meta ? meta.lastModified : Number(video.dataset && video.dataset.fileLastModified) || 0,
    fileDuration:meta ? meta.duration : Number(video.dataset && video.dataset.fileDuration) || 0,
    mode:gameMode(),
    time:Math.max(0, video.currentTime || 0),
    curIdx:Math.max(0, G.curIdx || 0),
    score:G.score || 0,
    streak:G.streak || 0,
    best:G.best || 0,
    lives:Number.isFinite(G.lives) ? G.lives : null,
    correct:G.correct || 0,
    wrong:G.wrong || 0,
    pauseLockUntil:G.pauseLockUntil || 0,
    play:snapshotPlayState(),
    eventCount:(G.play || []).length,
    savedAt:Date.now()
  };
  const rec = cacheSaveStateRecord(state);
  const cacheOk = storeSaveStates(map);
  let dbOk = false, dbErr = null;
  try{ await writeSaveStateDb(rec || state); dbOk = true; }
  catch(err){ dbErr = err && err.message ? err.message : String(err || 'database write failed'); G.stateDbError = dbErr; }
  if(!cacheOk && !dbOk){ bigStatus('SAVE FAILED','bad',1100); setProgress(100, 'Save state could not be written to the save-state database or browser backup.'); return; }
  updateStateButtons();
  const label = (state.fileName || 'media') + ' (' + bytesLabel(state.fileSize) + ')';
  setProgress(100, 'Save state stored in database for ' + label + ' at ' + fmtTime(state.time) + (dbOk ? '.' : '. Browser backup used because IndexedDB failed: ' + dbErr));
  bigStatus('STATE SAVED','good',850);
}
function loadState(){
  if(!stateModeAllowed()){ updateStateButtons(); bigStatus('SAVE UNAVAILABLE','warn',900); setProgress(100, 'Save states are only available in Easy and Hard.'); return; }
  if(!decodedAudioReady() || !currentSaveKey()){ updateStateButtons(); bigStatus('NO STATE','warn',900); setProgress(0, saveStateStatus()); return; }
  const st = currentSavedState();
  if(!st){ updateStateButtons(); bigStatus('NO SAVE','warn',900); setProgress(100, saveStateStatus()); return; }
  if(!stateModeAllowed(st.mode)){ bigStatus('INVALID SAVE','bad',1100); setProgress(100, 'This save state is not an Easy or Hard state.'); return; }
  stopMania();
  try{ video.pause(); }catch(_){ }
  G.opts.mode = st.mode;
  G.opts.hard = st.mode === 'hard';
  G.opts.lives = st.mode === 'hard' ? 3 : 3;
  saveJSON(OPT_STORE, G.opts);
  showStartOverlay(false);
  showDifficulty(false);
  show('results', false);
  show('gameOver', false);
  show('statusOverlay', false);
  show('stateModal', false);
  clearActive();
  restorePlayState(st.play);
  G.curIdx = clamp(parseInt(st.curIdx, 10) || 0, 0, G.play.length);
  G.score = Number(st.score) || 0;
  G.streak = Number(st.streak) || 0;
  G.best = Number(st.best) || 0;
  G.lives = st.mode === 'hard' ? clamp(parseInt(st.lives, 10) || 3, 1, 9) : Infinity;
  G.correct = Number(st.correct) || 0;
  G.wrong = Number(st.wrong) || 0;
  G.ended = false;
  G.running = true;
  G.failing = false;
  G.pauseLockUntil = Number(st.pauseLockUntil) || 0;
  G.active = null;
  G.internalSeek = true;
  try{ video.currentTime = Math.max(0, Number(st.time) || 0); }catch(_){ }
  setTimeout(()=>{ G.internalSeek = false; renderSubtitles(); }, 180);
  const dbg = $('debugModeSelect'); if(dbg) dbg.value = gameMode();
  updateHud();
  updateStateButtons();
  setProgress(100, 'Loaded ' + (st.mode === 'hard' ? 'Hard' : 'Easy') + ' save state at ' + fmtTime(st.time || 0) + '. Press Play to continue.');
  bigStatus('STATE LOADED','good',850);
}
function modeEnd(e){
  if(!e) return 0;
  if(!veryHardMode()) return e.end;
  const d = Math.max(0.1, e.end - e.start);
  const max = e.type === 'answer' ? 3.0 : 2.4;
  const min = e.type === 'answer' ? 1.15 : 0.9;
  return round2(e.start + clamp(d * 0.28, min, max));
}
function pauseLocked(){
  return !G.ended && !G.internalSeek && ((hardMode() && (G.failing || (G.active && !G.active.done) || ((video.currentTime || 0) < (G.pauseLockUntil || 0)))) || (maniaMode() && G.mania.active && G.mania.prompt));
}
function togglePause(){
  if(!video.src) return;
  if(!requireDecodedAudio('play/pause')) return;
  if(pauseLocked()){ log('Pause blocked during timed mode.'); return; }
  if(video.paused){
    if(!G.running && !G.ended) G.running = true;
    playVideo('play/pause');
  } else video.pause();
}

function parseEventBytes(bytes){
  return String(bytes || '').trim().split(/\s+/).map(x=>parseInt(x, 16)).filter(n=>Number.isFinite(n));
}
function eventBytesDistance(bytes, pattern){
  const a = parseEventBytes(bytes), b = parseEventBytes(pattern);
  if(a.length !== b.length) return Infinity;
  let d = 0;
  for(let i=0;i<a.length;i++) d += hammingByteForEvent(a[i], b[i]);
  return d;
}
function findSailorMoonManiaMusicAnchor(){
  const events = G.events || [];
  let best = null;
  for(let i=0;i<events.length;i++){
    const e = events[i];
    if(!e || e.type !== 'sound') continue;
    const dist = eventBytesDistance(e.bytes, 'AC B2 55 35 FE');
    if(dist > 1) continue;
    const prev = events.slice(0, i);
    const next = events.slice(i + 1);
    const nextIntroOffset = next.findIndex(x=>x && x.type === 'intro');
    const nextIntro = nextIntroOffset >= 0 ? next[nextIntroOffset] : null;
    const answersBefore = prev.filter(x=>x && x.type === 'answer').length;
    const pickupsBefore = prev.filter(x=>x && x.type === 'pickup').length;
    const answersUntilIntro = nextIntro ? next.slice(0, nextIntroOffset).filter(x=>x && x.type === 'answer').length : 0;
    const introGap = nextIntro ? nextIntro.start - e.start : 0;
    let score = 0;
    score += dist === 0 ? 40 : 18;
    score += Math.min(18, answersBefore * 3);
    score += Math.min(12, pickupsBefore * 3);
    if(nextIntro && introGap >= 150 && introGap <= 360) score += 24;
    if(nextIntro && answersUntilIntro === 0) score += 12;
    if(e.start > 120) score += 4;
    if(!best || score > best.score) best = {event:e, index:i, nextIntro, score};
  }
  return best && best.score >= 55 ? best : null;
}
function quantileSorted(vals, pct){
  if(!vals.length) return 0;
  const i = clamp(Math.floor((vals.length - 1) * pct), 0, vals.length - 1);
  return vals[i];
}
function findQuietRuns(data, quiet, minFrames, from, to){
  const out = [];
  let run = 0, start = -1;
  const lo = Math.max(0, Number(from)||0), hi = Math.min(data.length, to == null ? data.length : to);
  for(let i=lo;i<hi;i++){
    if(data[i].sm <= quiet){
      if(run === 0) start = i;
      run++;
    } else {
      if(run >= minFrames) out.push({start, end:i-1, frames:run, t0:data[start].t, t1:data[i-1].t});
      run = 0; start = -1;
    }
  }
  if(run >= minFrames && start >= 0) out.push({start, end:hi-1, frames:run, t0:data[start].t, t1:data[hi-1].t});
  return out;
}
function meanEnergy(data, from, count){
  let n = 0, sum = 0;
  const lo = Math.max(0, from), hi = Math.min(data.length, from + Math.max(0, count));
  for(let i=lo;i<hi;i++){ n++; sum += data[i].sm; }
  return n ? sum / n : 0;
}
function buildAudioEnergyTrack(buf, start, end){
  const sr = buf.sampleRate;
  const dur = Number(buf.duration) || 0;
  const t0 = clamp(Number(start)||0, 0, Math.max(0, dur - 0.1));
  const t1 = clamp(Number(end)||0, t0 + 0.5, dur);
  const hop = 0.25, win = 0.28;
  const step = Math.max(1, Math.floor(sr / 12000));
  const channels = Math.max(1, Math.min(buf.numberOfChannels || 1, 2));
  const data = [];
  const ch = [];
  for(let c=0;c<channels;c++) ch.push(buf.getChannelData(c));
  for(let t=t0; t + win <= t1; t += hop){
    const s0 = Math.max(0, Math.floor(t * sr));
    const s1 = Math.min(ch[0].length, Math.floor((t + win) * sr));
    let sum = 0, n = 0;
    for(let s=s0; s<s1; s+=step){
      let x = 0;
      for(let c=0;c<channels;c++) x += ch[c][s] || 0;
      x /= channels;
      sum += x * x;
      n++;
    }
    data.push({t, rms:Math.sqrt(sum / Math.max(1, n))});
  }
  for(let i=0;i<data.length;i++){
    const a = data[Math.max(0, i-1)].rms, b = data[i].rms, c = data[Math.min(data.length-1, i+1)].rms;
    data[i].sm = (a + b + c) / 3;
  }
  return {data, hop, start:t0, end:t1};
}
function activeRatio(data, from, count, threshold){
  let n = 0, active = 0, sum = 0;
  for(let i=Math.max(0, from); i<data.length && i<from+count; i++){
    n++; sum += data[i].sm;
    if(data[i].sm >= threshold) active++;
  }
  return {ratio:n ? active/n : 0, avg:n ? sum/n : 0, n};
}
function findSustainedQuietRun(data, from, to, quiet, minFrames){
  let run = 0;
  const lo = Math.max(0, from), hi = Math.min(data.length, to);
  for(let i=lo;i<hi;i++){
    if(data[i].sm <= quiet){
      run++;
      if(run >= minFrames) return i - run + 1;
    } else run = 0;
  }
  return -1;
}
function detectSailorMoonManiaMusicClip(){
  if(!G.audioBuf || !(G.events || []).length) return null;
  const anchor = findSailorMoonManiaMusicAnchor();
  if(!anchor) return null;
  const audioDur = Number(G.audioBuf.duration) || 0;
  const anchorStart = Number(anchor.event.start) || 0;
  const boundEnd = anchor.nextIntro ? Math.min(anchor.nextIntro.start - 4, audioDur) : Math.min(anchorStart + 285, audioDur);
  const scanStart = Math.min(boundEnd - 12, anchorStart + 20);
  if(!(boundEnd > scanStart + 80)) return null;
  const track = buildAudioEnergyTrack(G.audioBuf, scanStart, boundEnd);
  const data = track.data;
  if(data.length < 80) return null;
  const vals = data.map(x=>x.sm).filter(Number.isFinite).sort((a,b)=>a-b);
  const p05 = quantileSorted(vals, 0.05), p10 = quantileSorted(vals, 0.10), p25 = quantileSorted(vals, 0.25), p40 = quantileSorted(vals, 0.40), p60 = quantileSorted(vals, 0.60), p88 = quantileSorted(vals, 0.88);
  // Use a stricter silence threshold than the prompt detector.  The Sailor Moon
  // long song is preceded by a real quiet trough after the final sound packet;
  // speech before that packet can also be quiet-ish, so score the quiet run whose
  // following audible segment is longest rather than the first quiet-to-audio edge.
  const quiet = Math.max(0.00025, Math.min(p25 * 0.70, p10 * 1.50, p05 * 2.00));
  const softQuiet = Math.max(quiet * 1.45, p25 * 0.95);
  const active = Math.max(p40, quiet * 1.70, p60 * 0.78);
  const minQuietFrames = Math.max(3, Math.round(0.70 / track.hop));
  const startLo = Math.max(0, Math.round((anchorStart + 30 - track.start) / track.hop));
  const startHi = Math.min(data.length - 1, Math.round((boundEnd - 35 - track.start) / track.hop));
  const quietRuns = findQuietRuns(data, quiet, minQuietFrames, startLo, startHi);
  let bestRun = null, bestScore = -Infinity;
  for(const run of quietRuns){
    const cand = Math.min(data.length - 1, run.end + 1);
    const t = data[cand].t;
    if(t < anchorStart + 34 || t > boundEnd - 45) continue;
    let nextQuiet = boundEnd;
    for(const q of quietRuns){
      if(q.start > cand + Math.round(4 / track.hop) && q.frames >= minQuietFrames){ nextQuiet = q.t0; break; }
    }
    const audibleDur = Math.max(0, nextQuiet - t);
    const followFrames = Math.max(16, Math.round(18 / track.hop));
    const f = activeRatio(data, cand, followFrames, active);
    const avg = meanEnergy(data, cand, followFrames);
    if(f.n < followFrames * 0.65) continue;
    // The true song is the only long, sustained section after the row-31 sound
    // anchor.  Earlier spoken passages after smaller silences are much shorter.
    const score = Math.min(160, audibleDur) * 1.35 + f.ratio * 58 + Math.min(2.2, avg / Math.max(active, 1e-9)) * 18 + Math.min(24, run.frames * 1.45) - Math.max(0, 70 - audibleDur) * 1.1;
    if(score > bestScore){ bestScore = score; bestRun = {run, cand, t, audibleDur, ratio:f.ratio, avg}; }
  }

  let dynamicStart = !!bestRun;
  let clipStart = dynamicStart ? bestRun.t : anchorStart + 74.6;
  if(dynamicStart){
    // Keep the crop on the leading edge of the song, not in the middle of the
    // quiet window.  The frame timestamp is the RMS window start, so one hop past
    // the last quiet window lands just after the silence floor.
    clipStart = data[bestRun.cand].t;
  }

  const preferredDur = 122;
  let clipEnd = Math.min(clipStart + preferredDur, boundEnd - 2);
  let bestEnd = null, bestEndScore = -Infinity;
  if(dynamicStart){
    const startIdx = bestRun.cand;
    const shortQuietRuns = findQuietRuns(data, quiet, Math.max(2, Math.round(0.45 / track.hop)), startIdx + Math.round(80 / track.hop), data.length);
    for(const q of shortQuietRuns){
      const dur = q.t0 - clipStart;
      if(dur < 92 || dur > 155) continue;
      const beforeFrames = Math.max(10, Math.round(4 / track.hop));
      const afterFrames = Math.max(8, Math.round(3 / track.hop));
      const before = meanEnergy(data, Math.max(startIdx, q.start - beforeFrames), beforeFrames);
      const after = meanEnergy(data, q.start, afterFrames);
      if(before < active * 0.75) continue;
      const drop = (before - after) / Math.max(before, 1e-9);
      const targetBias = 1 - Math.min(1, Math.abs(dur - preferredDur) / 48);
      const earlyBias = 1 - Math.min(1, Math.max(0, dur - preferredDur) / 42);
      const latePenalty = Math.max(0, dur - preferredDur - 4) * 2.1;
      const score = Math.max(0, drop) * 46 + targetBias * 88 + earlyBias * 12 + Math.min(10, q.frames * 1.3) + (after <= softQuiet ? 14 : 0) - latePenalty;
      if((after <= softQuiet * 1.35 || drop >= 0.22) && score > bestEndScore){ bestEndScore = score; bestEnd = q.t0; }
    }
    // Some copies do not hit absolute silence at the song tail.  Also accept a
    // sustained post-song energy drop after roughly a full song-length of audio.
    const beforeFrames = Math.max(10, Math.round(4 / track.hop));
    const afterFrames = Math.max(10, Math.round(3 / track.hop));
    const dropLo = Math.max(startIdx + Math.round(92 / track.hop), 0);
    const dropHi = Math.min(data.length - afterFrames - 1, startIdx + Math.round(155 / track.hop));
    for(let i=dropLo;i<=dropHi;i++){
      const before = meanEnergy(data, i - beforeFrames, beforeFrames);
      const after = meanEnergy(data, i, afterFrames);
      if(before < active * 0.85) continue;
      const drop = (before - after) / Math.max(before, 1e-9);
      const dur = data[i].t - clipStart;
      const targetBias = 1 - Math.min(1, Math.abs(dur - preferredDur) / 42);
      const score = drop * 48 + targetBias * 68 + (after <= softQuiet * 1.25 ? 12 : 0) - Math.max(0, dur - preferredDur - 4) * 1.8;
      if(drop >= 0.20 && after <= softQuiet * 1.28 && score > bestEndScore){ bestEndScore = score; bestEnd = data[i].t; }
    }
  }
  if(bestEnd != null) clipEnd = bestEnd;
  if(clipEnd - clipStart < 45) clipEnd = Math.min(clipStart + preferredDur, boundEnd - 2);
  clipStart = clamp(clipStart, anchorStart + 15, Math.max(anchorStart + 16, audioDur - 2));
  clipEnd = clamp(clipEnd, clipStart + 20, audioDur);
  return {id:'sailor-moon-dynamic-song', start:round2(clipStart), end:round2(clipEnd), anchor:round2(anchorStart), anchorBytes:anchor.event.bytes, score:Math.round(anchor.score), dynamic:dynamicStart, startScore:Math.round(bestScore)};
}
function getManiaMusicClip(){
  if(G.mania.musicClip && G.mania.musicClip.id) return G.mania.musicClip;
  G.mania.musicClip = detectSailorMoonManiaMusicClip();
  return G.mania.musicClip;
}
function startDecodedManiaMusicClip(clip){
  if(!clip || !G.audioBuf) return false;
  const c = sfxCtx();
  if(!c) return false;
  const start = clamp(Number(clip.start)||0, 0, Math.max(0, G.audioBuf.duration - 0.1));
  const end = clamp(Number(clip.end)||0, start + 1, G.audioBuf.duration);
  if(end - start < 15) return false;
  try{
    const source = c.createBufferSource();
    const gain = c.createGain();
    source.buffer = G.audioBuf;
    source.loop = true;
    source.loopStart = start;
    source.loopEnd = end;
    gain.gain.value = 0.55;
    source.connect(gain).connect(c.destination);
    source.start(0, start);
    G.mania.musicSource = source;
    G.mania.musicGain = gain;
    G.mania.usingMusic = true;
    log('Minigame music: ' + clip.id + ' ' + fmtTime(start) + '-' + fmtTime(end) + ' from anchor ' + fmtTime(clip.anchor));
    return true;
  }catch(err){
    log('decoded minigame music failed: ' + (err && err.message ? err.message : err));
    G.mania.musicSource = null;
    G.mania.musicGain = null;
    G.mania.usingMusic = false;
    return false;
  }
}
function stopManiaMusic(){
  if(G.mania.beatTimer){ clearTimeout(G.mania.beatTimer); G.mania.beatTimer = null; }
  if(G.mania.musicSource){
    try{ G.mania.musicSource.stop(); }catch(_){ }
    try{ G.mania.musicSource.disconnect(); }catch(_){ }
    G.mania.musicSource = null;
  }
  if(G.mania.musicGain){
    try{ G.mania.musicGain.disconnect(); }catch(_){ }
    G.mania.musicGain = null;
  }
  if(G.mania.music){
    try{ G.mania.music.pause(); G.mania.music.currentTime = 0; }catch(_){ }
  }
  G.mania.usingMusic = false;
}
function isSupportedManiaMusicFile(file){
  if(!file) return false;
  if(/^audio\//i.test(file.type || '')) return true;
  const ext = fileExt(file.name);
  return ['mp3','m4a','aac','ogg','oga','opus','wav','flac','webm'].includes(ext);
}
function renderManiaMusicSetting(){
  const label = $('maniaMusicName');
  const clear = $('clearManiaMusic');
  if(label) label.textContent = G.mania.musicFileName ? G.mania.musicFileName : 'No custom music selected';
  if(clear) clear.disabled = !G.mania.musicUrl;
}
function revokeManiaMusicUrl(){
  if(G.mania.musicUrl){ try{ URL.revokeObjectURL(G.mania.musicUrl); }catch(_){ } }
  G.mania.musicUrl = null;
}
function setManiaMusicFile(file){
  if(!file) return;
  if(!isSupportedManiaMusicFile(file)){
    log('Minigame music upload rejected: unsupported audio file.');
    setProgress(0, 'Choose an audio file for Minigame music.');
    return;
  }
  stopManiaMusic();
  revokeManiaMusicUrl();
  G.mania.music = null;
  G.mania.musicUrl = URL.createObjectURL(file);
  G.mania.musicFileName = file.name || 'custom music';
  renderManiaMusicSetting();
  log('Minigame custom music selected: ' + G.mania.musicFileName);
  if(G.mania.active && maniaMode()) startManiaMusic();
}
function clearManiaMusicFile(){
  stopManiaMusic();
  revokeManiaMusicUrl();
  G.mania.music = null;
  G.mania.musicFileName = '';
  renderManiaMusicSetting();
  log('Minigame custom music cleared; falling back to detected/generated music.');
  if(G.mania.active && maniaMode()) startManiaMusic();
}
function openManiaMusicPicker(){
  const f = $('maniaMusicInput');
  if(f){ f.value = ''; f.click(); }
}
function startUploadedManiaMusic(){
  if(!G.mania.musicUrl) return false;
  try{
    if(!G.mania.music || G.mania.music.dataset.src !== G.mania.musicUrl){
      if(G.mania.music){ try{ G.mania.music.pause(); }catch(_){ } }
      G.mania.music = new Audio(G.mania.musicUrl);
      G.mania.music.dataset.src = G.mania.musicUrl;
    }
    const audio = G.mania.music;
    audio.loop = true;
    audio.preload = 'auto';
    audio.volume = 0.55;
    let fallbackStarted = false;
    const fallbackToDetectedMusic = ()=>{
      if(fallbackStarted || !G.mania.active || !maniaMode()) return;
      fallbackStarted = true;
      G.mania.usingMusic = false;
      try{ audio.pause(); audio.currentTime = 0; }catch(_){ }
      log('Minigame custom music failed; falling back to detected/generated music.');
      startFallbackManiaMusic();
    };
    try{ audio.onerror = fallbackToDetectedMusic; }catch(_){ }
    try{ audio.currentTime = 0; }catch(_){ }
    const p = audio.play();
    if(p && p.then){
      p.then(()=>{
        if(fallbackStarted) return;
        G.mania.usingMusic = true;
        log('Minigame music: custom upload ' + (G.mania.musicFileName || 'audio file'));
      }).catch(fallbackToDetectedMusic);
      return true;
    }
    G.mania.usingMusic = true;
    log('Minigame music: custom upload ' + (G.mania.musicFileName || 'audio file'));
    return true;
  }catch(err){
    log('Minigame custom music failed: ' + (err && err.message ? err.message : err));
    startFallbackManiaMusic();
    return true;
  }
}
function stopMania(){
  stopManiaMusic();
  cancelManiaScreenTransition();
  if(G.mania.active && G.mania.wasMuted != null) video.muted = G.mania.wasMuted;
  G.mania.active = false;
  G.mania.prompt = null;
  G.mania.breakUntil = 0;
  G.mania.clipEnd = 0;
  video.playbackRate = 1;
  show('statusOverlay', false);
}
function resetGame(){
  stopMania();
  G.curIdx = 0; G.active = null; G.score = 0; G.streak = 0; G.correct = 0; G.wrong = 0; G.ended = false; G.failing = false;
  G.pauseLockUntil = 0;
  G.lives = veryHardMode() ? 1 : hardMode() ? 3 : Infinity;
  for(const e of G.play){ e.done = false; e.got = null; }
  buttonsEnabled(false); pickupEnabled(false);
  show('gameOver', false); show('results', false); show('callOverlay', false); show('statusOverlay', false);
  $('qteRing') && $('qteRing').classList.remove('show');
  updateHud();
}
function startRun(opts){
  if(!video.src) return;
  if(!requireDecodedAudio('start')) return;
  if(maniaMode()){ startMania(); return; }
  sfxCtx();
  showStartOverlay(false);
  showDifficulty(false);
  show('results', false);
  resetGame();
  G.running = true;
  const restart = !opts || opts.restart !== false;
  if(restart){
    G.internalSeek = true;
    try{ video.currentTime = 0; }catch(_){ }
    setTimeout(()=>{ G.internalSeek = false; }, 120);
  }
  playVideo('play');
}
function restartRun(){ startRun({restart:true}); }
function askDifficulty(action){
  if(!video.src) return;
  try{ video.pause(); }catch(_){ }
  showStartOverlay(false);
  G.pendingDifficultyAction = action || null;
  showDifficulty(true);
}
function requestStartFromThumbnail(){
  if(!video.src || !G.play.length) return;
  if(!requireDecodedAudio('start')) return;
  sfxCtx();
  showStartOverlay(false);
  if(DEBUG) startRun({restart:true});
  else {
    G.pendingDifficultyAction = 'start';
    showDifficulty(true);
  }
}
function buttonsEnabled(on){ document.querySelectorAll('.phoneBtn').forEach(b=>b.disabled = !on); }
function pickupEnabled(on){ const b = $('pickupBtn'); if(b) b.disabled = !on; b && b.classList.toggle('live', on); }
function setVideosmartsMode(on){
  G.videosmarts = !!on;
  if(document.body) document.body.classList.toggle('videosmarts', G.videosmarts);
  const names = ['Yellow','Blue','Green','Red'];
  document.querySelectorAll('.phoneBtn').forEach(b=>{
    const answer = Number(b.dataset.answer);
    b.textContent = G.videosmarts ? names[answer-1] : String(answer);
  });
  const pickup = $('pickupBtn');
  if(pickup){
    pickup.classList.toggle('hidden', G.videosmarts);
    if(G.videosmarts){ pickup.disabled = true; pickup.classList.remove('live'); }
  }
}

function updateHud(){
  setText('score', G.score);
  setText('streak', G.streak + 'x');
  setText('lives', maniaMode() ? ('HI ' + (G.mania.high || 0)) : hardMode() ? '♥'.repeat(Math.max(0, G.lives)) || '—' : '∞');
  const nQ = G.play.filter(e=>e.type==='answer').length;
  setText('qCount', maniaMode() ? ('R ' + (G.mania.round || 0)) : (G.correct + '/' + nQ));
  setText('curTime', fmtTime(video.currentTime||0));
  setText('durTime', fmtTime(video.duration));
  const badge = $('modeBadge'); if(badge){ badge.textContent = modeLabel(); badge.className = 'badge ' + (maniaMode()?'mania':veryHardMode()?'veryhard':hardMode()?'hard':'practice'); }
  const pp = $('playPause');
  if(pp){
    const ready = decodedAudioReady();
    const minigameBlocked = maniaMode();
    pp.disabled = !ready || minigameBlocked;
    pp.title = minigameBlocked ? 'Play is disabled in Minigame mode. Use the start overlay or Restart to begin Minigame.' : (ready ? '' : decodedAudioBlockReason());
    pp.textContent = minigameBlocked ? 'Play' : (ready ? (video.paused ? 'Play' : 'Pause') : (G.audioDecoding ? 'Decoding…' : 'Play'));
  }
  const fsb = $('fullscreenSideBtn');
  if(fsb){ fsb.title = document.fullscreenElement ? 'Exit fullscreen' : 'Fullscreen'; fsb.textContent = '⛶'; }
  updateStateButtons();
}

function bigStatus(text, kind, ms){
  setText('statusText', text);
  const s = $('statusText'); if(s) s.className = 'bigStatus ' + (kind||'warn');
  show('statusOverlay', true);
  if(ms>0) setTimeout(()=>show('statusOverlay', false), ms);
}
function showStartOverlay(on){
  const m = $('startOverlay');
  if(m) m.classList.toggle('hidden', !on);
  renderSubtitles();
}
function startOverlayVisible(){
  const m = $('startOverlay');
  return !!(m && !m.classList.contains('hidden'));
}
function showDifficulty(on){
  const m = $('difficultyOverlay');
  if(m) m.classList.toggle('hidden', !on);
  document.querySelectorAll('.difficultyChoice').forEach(b=>b.classList.toggle('selected', b.dataset.mode === gameMode()));
}
function difficultyOpen(){
  const m = $('difficultyOverlay');
  return !!(m && !m.classList.contains('hidden'));
}
function setMode(mode){
  G.opts.mode = mode || 'easy';
  G.opts.hard = hardMode();
  G.opts.lives = veryHardMode() ? 1 : hardMode() ? 3 : 3;
  saveJSON(OPT_STORE, G.opts);
  const action = G.pendingDifficultyAction;
  G.pendingDifficultyAction = null;
  resetGame();
  showDifficulty(false);
  const dbg = $('debugModeSelect'); if(dbg) dbg.value = gameMode();
  if(action === 'restart') startRun({restart:true});
  else if(action === 'start') startRun({restart:true});
}
function setDebugMode(mode){
  G.pendingDifficultyAction = null;
  G.opts.mode = mode || 'easy';
  G.opts.hard = hardMode();
  G.opts.lives = veryHardMode() ? 1 : hardMode() ? 3 : 3;
  saveJSON(OPT_STORE, G.opts);
  resetGame();
  showDifficulty(false);
  const dbg = $('debugModeSelect'); if(dbg) dbg.value = gameMode();
  updateHud();
}

/* ---------- Minigame mode ---------- */
const MANIA_BREAK_CLIP_SECONDS = 2.4;
const MANIA_BREAK_PROMPT_PAD_SECONDS = 20;
const MANIA_BREAK_EDGE_PAD_SECONDS = 30;
function mediaDurationSeconds(){
  const vals = [];
  if(video && Number.isFinite(video.duration) && video.duration > 0) vals.push(video.duration);
  if(G.audioBuf && Number.isFinite(G.audioBuf.duration) && G.audioBuf.duration > 0) vals.push(G.audioBuf.duration);
  for(const e of G.events || []){
    const end = Number(e && (e.end != null ? e.end : e.start));
    if(Number.isFinite(end) && end > 0) vals.push(end + 1);
  }
  return vals.length ? Math.max(...vals) : 0;
}
function addTimeRange(ranges, start, end, duration){
  start = Number(start); end = Number(end);
  if(!Number.isFinite(start) || !Number.isFinite(end)) return;
  if(Number.isFinite(duration) && duration > 0){ start = clamp(start, 0, duration); end = clamp(end, 0, duration); }
  if(end > start) ranges.push({start, end});
}
function mergeTimeRanges(ranges){
  const sorted = (ranges || []).filter(r=>r && Number.isFinite(r.start) && Number.isFinite(r.end) && r.end > r.start).sort((a,b)=>a.start-b.start || a.end-b.end);
  const out = [];
  for(const r of sorted){
    const last = out[out.length - 1];
    if(last && r.start <= last.end) last.end = Math.max(last.end, r.end);
    else out.push({start:r.start, end:r.end});
  }
  return out;
}
function maniaPromptSafetyEvents(){
  const out = [], seen = new Set();
  const add = e=>{
    if(!e || (e.type !== 'answer' && e.type !== 'pickup')) return;
    const start = Number(e.start), end = Number(e.end != null ? e.end : e.start);
    if(!Number.isFinite(start)) return;
    const key = e.type + ':' + start.toFixed(2) + ':' + (Number.isFinite(end) ? end.toFixed(2) : '');
    if(seen.has(key)) return;
    seen.add(key);
    out.push(e);
  };
  (G.play || []).forEach(add);
  (G.events || []).forEach(add);
  return out;
}
function maniaBreakForbiddenRanges(duration){
  const ranges = [];
  const edge = MANIA_BREAK_EDGE_PAD_SECONDS;
  if(Number.isFinite(duration) && duration > 0){
    addTimeRange(ranges, 0, Math.min(edge, duration), duration);
    addTimeRange(ranges, Math.max(0, duration - edge), duration, duration);
  }
  const pad = MANIA_BREAK_PROMPT_PAD_SECONDS;
  for(const e of maniaPromptSafetyEvents()){
    const start = Number(e.start), end = Number(e.end != null ? e.end : e.start);
    const safeEnd = Number.isFinite(end) ? Math.max(start, end) : start;
    addTimeRange(ranges, start - pad, safeEnd + pad, duration);
  }
  return mergeTimeRanges(ranges);
}
function maniaBreakSafeIntervals(duration, clipSeconds){
  duration = Number(duration) || 0;
  clipSeconds = Number(clipSeconds) || MANIA_BREAK_CLIP_SECONDS;
  if(duration <= MANIA_BREAK_EDGE_PAD_SECONDS * 2 + clipSeconds) return [];
  const forbidden = maniaBreakForbiddenRanges(duration);
  const intervals = [];
  let cur = 0;
  for(const r of forbidden){
    if(r.start - cur >= clipSeconds) intervals.push({start:cur, end:r.start});
    cur = Math.max(cur, r.end);
  }
  if(duration - cur >= clipSeconds) intervals.push({start:cur, end:duration});
  return intervals.filter(r=>r.end - r.start >= clipSeconds);
}
function maniaFindSafeBreakInterval(start, clipSeconds, intervals){
  for(const r of intervals){
    if(start >= r.start && start + clipSeconds <= r.end) return r;
  }
  return null;
}
function maniaBreakCandidates(){
  const duration = mediaDurationSeconds();
  const clipSeconds = MANIA_BREAK_CLIP_SECONDS;
  const intervals = maniaBreakSafeIntervals(duration, clipSeconds);
  const out = [], seen = new Set();
  const pushStart = (start, label)=>{
    start = Number(start);
    if(!Number.isFinite(start)) return;
    const interval = maniaFindSafeBreakInterval(start, clipSeconds, intervals);
    if(!interval) return;
    start = clamp(start, interval.start, interval.end - clipSeconds);
    const key = start.toFixed(2);
    if(seen.has(key)) return;
    seen.add(key);
    out.push({type:'break', start:round2(start), end:round2(start + clipSeconds), source:label || 'safe'});
  };
  if(!intervals.length) return out;
  for(const e of G.events || []){
    if(!e || (e.type !== 'intro' && e.type !== 'sound')) continue;
    pushStart(e.start, e.type);
  }
  for(const interval of intervals){
    const lo = interval.start, hi = interval.end - clipSeconds;
    if(hi < lo) continue;
    const span = hi - lo;
    const count = Math.max(1, Math.min(10, Math.floor(span / 14) + 1));
    for(let i=0;i<count;i++) pushStart(lo + (span * (i + 0.5) / count), 'safe-window');
  }
  return out;
}
function maniaPools(){
  return {
    pickups: G.play.filter(e=>e.type === 'pickup'),
    answers: G.play.filter(e=>e.type === 'answer'),
    clips: G.events.filter(e=>e.type === 'intro' || e.type === 'sound'),
    breakClips: maniaBreakCandidates()
  };
}
function maniaPickBreakClip(){
  const candidates = maniaBreakCandidates();
  if(candidates.length) return maniaPick(candidates);
  const duration = mediaDurationSeconds();
  const clipSeconds = MANIA_BREAK_CLIP_SECONDS;
  const edgeStart = MANIA_BREAK_EDGE_PAD_SECONDS;
  const edgeEnd = duration - MANIA_BREAK_EDGE_PAD_SECONDS - clipSeconds;
  if(Number.isFinite(duration) && edgeEnd > edgeStart){
    return {type:'break', start:round2(edgeStart + Math.random() * (edgeEnd - edgeStart)), end:0, source:'edge-fallback'};
  }
  const clips = (G.events || []).filter(e=>e && (e.type === 'intro' || e.type === 'sound'));
  return maniaPick(clips.length ? clips : (G.events || []));
}
function maniaSpeed(){ return clamp(1 + G.mania.round * 0.085, 1, 3.8); }
function maniaWindow(){ return clamp(3.0 - G.mania.round * 0.085, 0.65, 3.0); }
function maniaPick(pool){
  if(!pool.length) return null;
  return pool[Math.floor(Math.random() * pool.length)];
}
function maniaPromptStart(e, speed){
  if(!e) return 0;
  if(e.type === 'answer'){
    // Answer packets can precede the full four-choice card. Use a fixed sixty-frame
    // offset, then freeze there so Minigame reliably shows the four answers.
    return Math.max(0, e.start + (60 / 30));
  }
  return Math.max(0, e.start - 0.05);
}
function maniaTransitionDirection(){
  const dirs = ['right','top','bottom','left'];
  return dirs[Math.floor(Math.random() * dirs.length)];
}
function maniaTransitionTransform(dir, entering){
  if(dir === 'right') return entering ? 'translate3d(100%,0,0)' : 'translate3d(-100%,0,0)';
  if(dir === 'left') return entering ? 'translate3d(-100%,0,0)' : 'translate3d(100%,0,0)';
  if(dir === 'top') return entering ? 'translate3d(0,-100%,0)' : 'translate3d(0,100%,0)';
  return entering ? 'translate3d(0,100%,0)' : 'translate3d(0,-100%,0)';
}
function drawVideoFrameToCanvas(canvas){
  if(!canvas || !video || !video.videoWidth || !video.videoHeight) return false;
  try{
    const w = Math.max(2, video.videoWidth|0), h = Math.max(2, video.videoHeight|0);
    if(canvas.width !== w) canvas.width = w;
    if(canvas.height !== h) canvas.height = h;
    const ctx = canvas.getContext('2d', {alpha:false});
    if(!ctx) return false;
    ctx.drawImage(video, 0, 0, w, h);
    return true;
  }catch(_){ return false; }
}
function cancelManiaScreenTransition(){
  G.mania.transitionToken = (G.mania.transitionToken || 0) + 1;
  const layer = $('maniaTransition');
  if(layer) layer.classList.add('hidden');
  ['maniaTransitionOld','maniaTransitionNew'].forEach(id=>{
    const c = $(id);
    if(c){
      c.style.transition = 'none';
      c.style.transform = 'translate3d(0,0,0)';
      c.style.opacity = '1';
    }
  });
}
function beginManiaScreenTransition(){
  const layer = $('maniaTransition'), oldFrame = $('maniaTransitionOld'), newFrame = $('maniaTransitionNew');
  if(!layer || !oldFrame || !newFrame || !maniaMode() || !G.mania.active) return null;
  const token = (G.mania.transitionToken || 0) + 1;
  G.mania.transitionToken = token;
  const dir = maniaTransitionDirection();
  const hasOld = drawVideoFrameToCanvas(oldFrame);
  oldFrame.style.transition = 'none';
  newFrame.style.transition = 'none';
  oldFrame.style.opacity = hasOld ? '1' : '0';
  newFrame.style.opacity = '1';
  oldFrame.style.transform = 'translate3d(0,0,0)';
  newFrame.style.transform = maniaTransitionTransform(dir, true);
  layer.classList.remove('hidden');
  return {token, dir, hasOld};
}
function finishManiaScreenTransition(tx, durationMs){
  const layer = $('maniaTransition'), oldFrame = $('maniaTransitionOld'), newFrame = $('maniaTransitionNew');
  if(!tx || !layer || !oldFrame || !newFrame || tx.token !== G.mania.transitionToken) return false;
  if(!drawVideoFrameToCanvas(newFrame)){ cancelManiaScreenTransition(); return false; }
  const reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const dur = reduce ? 1 : Math.max(120, Number(durationMs) || 280);
  oldFrame.style.transition = 'none';
  newFrame.style.transition = 'none';
  oldFrame.style.transform = 'translate3d(0,0,0)';
  newFrame.style.transform = maniaTransitionTransform(tx.dir, true);
  layer.classList.remove('hidden');
  requestAnimationFrame(()=>{
    if(tx.token !== G.mania.transitionToken) return;
    const easing = 'transform ' + dur + 'ms cubic-bezier(.22,.78,.2,1)';
    oldFrame.style.transition = easing;
    newFrame.style.transition = easing;
    oldFrame.style.transform = maniaTransitionTransform(tx.dir, false);
    newFrame.style.transform = 'translate3d(0,0,0)';
    setTimeout(()=>{
      if(tx.token !== G.mania.transitionToken) return;
      layer.classList.add('hidden');
      oldFrame.style.transition = 'none';
      newFrame.style.transition = 'none';
      oldFrame.style.transform = 'translate3d(0,0,0)';
      newFrame.style.transform = 'translate3d(0,0,0)';
    }, dur + 40);
  });
  return true;
}
function afterManiaSeekSettles(tx, fn, maxDelayMs){
  let done = false;
  const finish = ()=>{
    if(done) return;
    done = true;
    video.removeEventListener('seeked', finish);
    fn && fn();
  };
  try{ video.addEventListener('seeked', finish, {once:true}); }catch(_){ }
  setTimeout(finish, Math.max(50, Number(maxDelayMs) || 150));
}
function startFallbackManiaMusic(){
  const decodedClip = getManiaMusicClip();
  if(decodedClip && startDecodedManiaMusicClip(decodedClip)) return;
  startSyntheticManiaMusic();
}
function startManiaMusic(){
  stopManiaMusic();
  if(startUploadedManiaMusic()) return;
  startFallbackManiaMusic();
}
function startSyntheticManiaMusic(){
  const notes = [330,392,494,392,523,494,392,659];
  let i = 0;
  const beat = ()=>{
    if(!G.mania.active || !maniaMode()) return;
    const tempo = clamp(170 - G.mania.round * 3.5, 70, 170);
    tone(notes[i++ % notes.length], 0.055, 'square', 0.018, 0);
    G.mania.beatTimer = setTimeout(beat, tempo);
  };
  beat();
}
function startMania(){
  if(!video.src || !G.play.length) return;
  if(!requireDecodedAudio('minigame start')) return;
  sfxCtx();
  showStartOverlay(false);
  showDifficulty(false);
  show('results', false);
  resetGame();
  G.running = true; G.ended = false;
  G.mania.active = true;
  G.mania.round = 0;
  G.mania.prompt = null;
  G.mania.nextAt = performance.now() + 350;
  G.mania.breakUntil = 0;
  G.mania.clipEnd = 0;
  G.mania.wasMuted = video.muted;
  video.muted = true;
  video.playbackRate = 1;
  startManiaMusic();
  bigStatus('MINIGAME', 'warn', 700);
  playVideo('play');
  log('Minigame started.');
}
function maniaEnd(reason){
  if(!G.mania.active) return;
  stopMania();
  G.ended = true; G.running = false;
  try{ video.pause(); }catch(_){ }
  sfxGameOver();
  if(G.score > G.mania.high){
    G.mania.high = G.score;
    try{ localStorage.setItem(MANIA_STORE, String(G.mania.high)); }catch(_){ }
  }
  configureResults('Minigame Over', 'Minigame round ' + G.mania.round + ' · high score ' + G.mania.high + (reason ? ' · ' + reason : ''), {retry:true, difficulty:true});
  show('results', true);
  log('Minigame ended: ' + (reason || 'done') + '. Score ' + G.score + ', high ' + G.mania.high + '.');
  updateHud();
}
function maniaBreak(now){
  const e = maniaPickBreakClip();
  G.mania.prompt = null;
  G.mania.breakUntil = now + 2600;
  G.mania.nextAt = G.mania.breakUntil + 220;
  clearActive();
  bigStatus('BREAK', 'warn', 0);
  if(e){
    G.internalSeek = true;
    try{ video.currentTime = Math.max(0, e.start); }catch(_){ }
    G.mania.clipEnd = (e.end && e.end > e.start) ? e.end : e.start + MANIA_BREAK_CLIP_SECONDS;
    video.playbackRate = clamp(maniaSpeed() + 0.6, 1.5, 4);
    setTimeout(()=>{ G.internalSeek = false; playVideo('play'); }, 80);
  }
}
function maniaPrompt(now){
  show('statusOverlay', false);
  const pools = maniaPools();
  if(!pools.pickups.length && !pools.answers.length){ maniaEnd('no decoded prompts'); return; }
  G.mania.round++;
  const forcePickup = G.mania.round === 1 && pools.pickups.length;
  const forceAnswer = G.mania.round === 2 && pools.answers.length;
  const useAnswer = forceAnswer || (!forcePickup && pools.answers.length && Math.random() < clamp(0.28 + G.mania.round * 0.015, 0.28, 0.62));
  const source = useAnswer ? maniaPick(pools.answers) : maniaPick(pools.pickups.length ? pools.pickups : pools.answers);
  const speed = maniaSpeed();
  const win = maniaWindow();
  const multi = source.type === 'pickup' ? Math.min(1 + Math.floor(Math.max(0, G.mania.round - 6) / 5), 4) : 1;
  const promptStart = maniaPromptStart(source, speed);
  const transitionMs = 300;
  const transition = beginManiaScreenTransition();
  G.mania.prompt = {source, type:source.type, answer:source.answer, need:multi, got:0, deadline:now + win * 1000 + transitionMs, promptStart};
  G.active = source;
  G.internalSeek = true;
  try{ video.currentTime = promptStart; }catch(_){ }
  G.mania.clipEnd = Math.min(source.end || promptStart + 2, promptStart + clamp(2.2 / speed, 0.55, 2.2));
  video.playbackRate = speed;
  if(source.type === 'pickup'){
    pickupEnabled(true);
    show('callOverlay', true);
    setText('callTitle', multi > 1 ? ('PICK UP x' + multi) : 'PICK UP');
    setText('callHint', 'Fast!');
    startPickupRumble(source, win * 1000 + transitionMs);
    afterManiaSeekSettles(transition, ()=>{ finishManiaScreenTransition(transition, transitionMs); G.internalSeek = false; playVideo('play'); }, 150);
  } else {
    G.mania.clipEnd = 0;
    buttonsEnabled(true);
    show('callOverlay', false);
    afterManiaSeekSettles(transition, ()=>{ finishManiaScreenTransition(transition, transitionMs); G.internalSeek = false; try{ video.pause(); }catch(_){ } }, 170);
  }
  log('Minigame round ' + G.mania.round + ': ' + source.type + (source.type === 'answer' ? (' ' + source.answer) : (' x' + multi)) + ' @ ' + fmtTime(promptStart));
  updateHud();
}
function maniaSuccess(){
  const p = G.mania.prompt; if(!p) return;
  show('statusOverlay', false);
  clearActive();
  G.active = null;
  G.correct++;
  G.streak++;
  G.best = Math.max(G.best, G.streak);
  G.score += Math.round(75 + G.mania.round * 12 + G.streak * 8);
  if(p.type === 'answer') sfxManiaAnswer();
  else sfxManiaPickup();
  G.mania.prompt = null;
  G.mania.nextAt = performance.now() + clamp(520 - G.mania.round * 12, 80, 520);
  if(G.score > G.mania.high){
    G.mania.high = G.score;
    try{ localStorage.setItem(MANIA_STORE, String(G.mania.high)); }catch(_){ }
  }
  updateHud();
}
function maniaFail(reason){
  G.wrong++;
  G.streak = 0;
  clearActive();
  G.mania.prompt = null;
  maniaEnd(reason || 'miss');
}
function maniaLoop(){
  const now = performance.now();
  if(!G.mania.active) return;
  if(G.mania.prompt && now > G.mania.prompt.deadline){ maniaFail('too slow'); return; }
  if(G.mania.clipEnd && (video.currentTime || 0) > G.mania.clipEnd){
    const p = G.mania.prompt;
    if(p && p.source){
      G.internalSeek = true;
      try{ video.currentTime = Math.max(0, p.promptStart != null ? p.promptStart : maniaPromptStart(p.source, video.playbackRate || 1)); }catch(_){ }
      setTimeout(()=>{ G.internalSeek = false; }, 50);
    } else if(G.mania.breakUntil && now < G.mania.breakUntil) {
      const e = maniaPickBreakClip();
      if(e){ try{ video.currentTime = Math.max(0, e.start); G.mania.clipEnd = (e.end && e.end > e.start) ? e.end : e.start + MANIA_BREAK_CLIP_SECONDS; }catch(_){ } }
    }
  }
  if(!G.mania.prompt && now >= G.mania.nextAt){
    if(G.mania.round > 0 && (G.mania.round % 9) === 0 && (!G.mania.breakUntil || now > G.mania.breakUntil + 500)){
      maniaBreak(now);
    } else {
      G.mania.breakUntil = 0;
      maniaPrompt(now);
    }
  }
}

/* ---------- main loop / QTE ---------- */
function activate(e){
  G.active = e;
  if(e.type === 'pickup'){
    pickupEnabled(true);
    show('callOverlay', true);
    setText('callTitle', 'INCOMING CALL');
    setText('callHint', hardMode() ? 'Pick up before the cue ends' : 'Pick up the phone');
    startPickupRumble(e);
    log('Pick-up window @' + fmtTime(e.start) + (hardMode()?' (' + modeLabel() + ')':''));
  } else if(e.type === 'answer'){
    buttonsEnabled(true);
    if(!hardMode()) bigStatus('ANSWER', 'warn', 700);
    log('Question @' + fmtTime(e.start) + ' expects button ' + e.answer + (hardMode()?' (' + modeLabel() + ')':''));
  }
}
function clearActive(){
  stopGamepadRing(true);
  G.active = null; buttonsEnabled(false); pickupEnabled(false);
  show('callOverlay', false);
  $('qteRing') && $('qteRing').classList.remove('show');
}
function succeed(e, label){
  e.done = true; clearActive();
  if(e.type === 'answer'){ G.correct++; }
  G.streak++; G.best = Math.max(G.best, G.streak);
  const bonus = e.type === 'answer' ? (100 + (G.streak-1)*25) : 50;
  G.score += bonus;
  sfxCorrect();
  if(!hardMode()) bigStatus(label || 'CORRECT', 'good', 800);
  if(hardMode()) G.pauseLockUntil = Math.max(G.pauseLockUntil || 0, modeEnd(e));
  if(G.curIdx < G.play.length && G.play[G.curIdx] === e) G.curIdx++;
  if(veryHardMode() && e.type === 'answer'){
    const t = modeEnd(e) + 0.05;
    if((video.currentTime || 0) < t){
      G.internalSeek = true;
      try{ video.currentTime = t; }catch(_){ }
      setTimeout(()=>{ G.internalSeek = false; }, 100);
    }
  }
  updateHud();
}
function configureResults(title, line, actions){
  setText('resultTitle', title || 'Results');
  setText('resultScore', G.score);
  setText('resultLine', line || '');
  const retry = $('resultRetry'), diff = $('resultDifficulty'), close = $('resultsClose');
  if(retry) retry.classList.toggle('hidden', !(actions && actions.retry));
  if(diff) diff.classList.toggle('hidden', !(actions && actions.difficulty && !DEBUG));
  if(close) close.classList.toggle('hidden', !!(actions && actions.forceChoice && !DEBUG));
}
function showChoiceAfterGameOver(sub, delay){
  G.ended = true; G.running = false; G.failing = true;
  clearActive();
  sfxGameOver();
  updateHud();
  try{ video.pause(); }catch(_){ }
  show('gameOver', true);
  setText('gameOverText', 'GAME OVER');
  setText('gameOverSub', sub || 'Try again');
  setTimeout(()=>{
    show('gameOver', false);
    G.failing = false;
    configureResults('Game Over', (sub || 'Try again') + (DEBUG ? ' · mode is selected in the debug panel' : ''), {retry:true, difficulty:true, forceChoice:true});
    show('results', true);
    updateHud();
  }, delay || 1000);
}
function veryHardStartOver(reason){
  if(reason === 'early') G.wrong++;
  G.streak = 0;
  G.lives = 0;
  showChoiceAfterGameOver(reason === 'early' ? 'Early input' : 'Start over', 1000);
}
function fail(e, reason){
  if(e.done) return;
  if(e.type === 'answer') G.wrong++;
  G.streak = 0;
  clearActive();
  if(!hardMode()){
    e.done = true;
    bigStatus(reason === 'timeout' ? 'TIME OUT' : 'WRONG', 'bad', 1000);
    sfxWrong();
    if(G.curIdx < G.play.length && G.play[G.curIdx] === e) G.curIdx++;
    updateHud(); return;
  }
  if(veryHardMode()){ veryHardStartOver(reason); return; }
  // hard mode: lose a life, Dragon's-Lair style retry from the checkpoint. While the
  // game-over transition plays, G.failing freezes the loop so it can't mis-advance.
  G.lives--;
  G.failing = true;
  sfxGameOver();
  updateHud();
  try{ video.pause(); }catch(_){ }
  show('gameOver', true);
  const dead = G.lives <= 0;
  setText('gameOverText', dead ? 'GAME OVER' : 'MISS!');
  setText('gameOverSub', dead ? 'Out of lives' : (G.lives + ' ' + (G.lives===1?'life':'lives') + ' left - try again'));
  setTimeout(()=>{
    show('gameOver', false);
    if(dead){
      G.ended = true; G.running = false; G.failing = false;
      configureResults('Game Over', 'Out of lives' + (DEBUG ? ' · mode is selected in the debug panel' : ''), {retry:true, difficulty:true, forceChoice:true});
      show('results', true);
      updateHud();
      return;
    }
    const t = Math.max(0, e.checkpoint != null ? e.checkpoint : e.start - 0.6);
    G.internalSeek = true; video.currentTime = t;
    G.active = null;
    G.curIdx = G.play.indexOf(e);
    setTimeout(()=>{ G.internalSeek = false; G.failing = false; playVideo('play'); }, 220);
    log('Retry from ' + fmtTime(t));
  }, 1300);
}

function loop(){
  const t = video.currentTime || 0;
  if(maniaMode() && G.mania.active){
    maniaLoop();
    updateHud();
    requestAnimationFrame(loop);
    return;
  }
  if(G.failing){ updateHud(); requestAnimationFrame(loop); return; }
  if(!G.ended && !G.internalSeek){
    // expire the active QTE (and stop processing this frame - a fail may rewind us)
    if(G.active && t > modeEnd(G.active) + 0.02 && !G.active.done){
      fail(G.active, 'timeout'); updateHud(); requestAnimationFrame(loop); return;
    }
    // activate the next interactive event
    if(!G.active && G.curIdx < G.play.length){
      const e = G.play[G.curIdx];
      if(e.done){ G.curIdx++; }
      else if(t >= e.start && t <= e.end){ activate(e); }
      else if(t > e.end){ /* skipped (e.g. resumed past it) */ e.done = true; G.curIdx++; }
    }
    // Timed modes deliberately do not show an on-screen countdown.
    const ring = $('qteRing');
    if(ring) ring.classList.remove('show');
  }
  updateHud();
  requestAnimationFrame(loop);
}

function earlyCueAt(t){
  return G.play.find(e => e && e.type === 'answer' && e.delayedCue && !e.done &&
    Number.isFinite(e.earlyGuardStart) && t >= e.earlyGuardStart && t < e.start);
}
function onAnswer(n){
  if(maniaMode() && G.mania.active){
    const p = G.mania.prompt;
    if(!p || p.type !== 'answer'){ maniaFail('early input'); return; }
    if(Number(n) === Number(p.answer)) maniaSuccess();
    else maniaFail('wrong answer');
    return;
  }
  const e = G.active;
  if(!e || e.type !== 'answer' || e.done){
    const early = earlyCueAt(video.currentTime || 0);
    if(early && veryHardMode() && G.running && !G.ended && !G.failing) veryHardStartOver('early');
    else if(early && !hardMode()) bigStatus('TOO EARLY','warn',700);
    else if(veryHardMode() && G.running && !G.ended && !G.failing) veryHardStartOver('early');
    else if(!hardMode()) bigStatus('WAIT','warn',500);
    return;
  }
  if(Number(n) === Number(e.answer)) succeed(e, 'CORRECT');
  else fail(e, 'wrong');
}
function onPickup(){
  if(maniaMode() && G.mania.active){
    const p = G.mania.prompt;
    if(!p || p.type !== 'pickup'){ maniaFail('early input'); return; }
    p.got++;
    if(p.got >= p.need) maniaSuccess();
    else {
      setText('callTitle', 'PICK UP x' + (p.need - p.got));
      sfxManiaStep();
    }
    return;
  }
  const e = G.active;
  if(!e || e.type !== 'pickup' || e.done){
    if(veryHardMode() && G.running && !G.ended && !G.failing) veryHardStartOver('early');
    return;
  }
  succeed(e, 'CONNECTED');
}

function endRun(){
  if(maniaMode() && G.mania.active){ maniaEnd('video ended'); return; }
  G.ended = true; G.running = false; clearActive();
  stopMania();
  try{ video.pause(); }catch(_){ }
  const nQ = G.play.filter(e=>e.type==='answer').length;
  const pct = nQ ? Math.round(100*G.correct/nQ) : 0;
  configureResults('Results', G.correct + ' / ' + nQ + ' correct  ·  best streak ' + G.best + (hardMode() ? '  ·  ' + (G.lives>0?'cleared!':'out of lives') : ''), {retry:true, difficulty:true});
  show('results', true);
  if(hardMode() && G.lives>0) sfxWin();
  log('Run ended. Score ' + G.score + ', ' + G.correct + '/' + nQ + ' (' + pct + '%).');
}

/* ---------- fullscreen ---------- */
function toggleFullscreen(){
  const target = $('appGrid') || document.documentElement;
  if(!document.fullscreenElement){ (target.requestFullscreen||target.webkitRequestFullscreen).call(target).catch(()=>{}); }
  else { (document.exitFullscreen||document.webkitExitFullscreen).call(document); }
}
function fullscreenControlsWanted(){
  return window.matchMedia('(max-width:920px), (pointer:coarse)').matches || G.opts.fullscreenControls !== false;
}
function applyFullscreenLayout(){
  document.body.classList.toggle('fs', !!document.fullscreenElement);
  document.body.classList.toggle('fsControls', !!document.fullscreenElement && fullscreenControlsWanted());
  const opt = $('fullscreenControlsOpt');
  if(opt) opt.checked = G.opts.fullscreenControls !== false;
  updateHud();
}
document.addEventListener('fullscreenchange', applyFullscreenLayout);
window.addEventListener('resize', applyFullscreenLayout);

/* ---------- key/gamepad remapping ---------- */
function keyLabel(k){ return k === ' ' ? 'Space' : k.length === 1 ? k.toUpperCase() : k; }
function actionLabel(a){ return {ans1:'Answer 1', ans2:'Answer 2', ans3:'Answer 3', ans4:'Answer 4', pickup:'Pick up phone', pause:'Play / pause', fullscreen:'Fullscreen'}[a] || a; }
function samePadMap(a, b){ return !!(a && b && a.type === b.type && Number(a.index) === Number(b.index) && Number(a.dir || 0) === Number(b.dir || 0)); }
function padMapLabel(m){
  if(!m || typeof m !== 'object') return 'Unmapped';
  if(m.type === 'button') return 'Button ' + (Number(m.index) + 1);
  if(m.type === 'axis') return 'Axis ' + (Number(m.index) + 1) + ' ' + (Number(m.dir) < 0 ? '-' : '+');
  return 'Unmapped';
}
function renderKeys(){
  document.querySelectorAll('.keyRow').forEach(row=>{
    const a = row.dataset.action; const span = row.querySelector('.keyVal');
    if(span) span.textContent = G.awaitingRebind === a ? 'press a key…' : keyLabel(G.keys[a] || '');
  });
  document.querySelectorAll('.padRow').forEach(row=>{
    const a = row.dataset.action; const span = row.querySelector('.padVal');
    if(span) span.textContent = G.awaitingPadRebind === a ? 'press button / move stick…' : padMapLabel(G.pads[a]);
  });
  const st = $('gamepadStatus');
  if(st){
    const suffix = G.awaitingPadRebind ? (' · waiting for ' + actionLabel(G.awaitingPadRebind)) : '';
    st.textContent = (G.padStatus || 'No gamepad detected') + suffix;
  }
}
function actionForKey(k){
  const lk = k.length === 1 ? k.toLowerCase() : k;
  for(const a in G.keys){ const kk = G.keys[a]; if(kk && (kk.length===1?kk.toLowerCase():kk) === lk) return a; }
  return null;
}
function getGamepadsList(){
  if(!navigator.getGamepads) return [];
  try{ return Array.prototype.slice.call(navigator.getGamepads() || []).filter(gp=>gp && gp.connected); }
  catch(_){ return []; }
}
function padButtonDown(gp, index){
  const b = gp && gp.buttons ? gp.buttons[Number(index)] : null;
  return !!(b && (b.pressed || Number(b.value) > 0.55));
}
function padAxisDown(gp, index, dir){
  const v = gp && gp.axes ? Number(gp.axes[Number(index)]) : 0;
  return Number.isFinite(v) && (Number(dir) < 0 ? v < -0.55 : v > 0.55);
}
function padMapDown(m, pads){
  if(!m || typeof m !== 'object') return false;
  for(const gp of pads){
    if(m.type === 'button' && padButtonDown(gp, m.index)) return true;
    if(m.type === 'axis' && padAxisDown(gp, m.index, m.dir)) return true;
  }
  return false;
}
function detectPadBind(pads){
  for(const gp of pads){
    const buttons = gp.buttons || [];
    for(let i=0;i<buttons.length;i++){ if(padButtonDown(gp, i)) return {type:'button', index:i}; }
    const axes = gp.axes || [];
    for(let i=0;i<axes.length;i++){
      const v = Number(axes[i]);
      if(Number.isFinite(v) && Math.abs(v) > 0.65) return {type:'axis', index:i, dir:v < 0 ? -1 : 1};
    }
  }
  return null;
}
function assignPadMap(action, map){
  const next = Object.assign({}, G.pads || {});
  Object.keys(next).forEach(a=>{ if(a !== action && samePadMap(next[a], map)) next[a] = null; });
  next[action] = map;
  G.pads = next;
  saveJSON(GAMEPAD_STORE, G.pads);
  G.padPrev = {};
  renderKeys();
}
function runControlAction(a){
  sfxCtx();
  if(a === 'ans1') onAnswer(1);
  else if(a === 'ans2') onAnswer(2);
  else if(a === 'ans3') onAnswer(3);
  else if(a === 'ans4') onAnswer(4);
  else if(a === 'pickup') onPickup();
  else if(a === 'pause') togglePause();
  else if(a === 'fullscreen') toggleFullscreen();
}
function gamepadInputBlocked(){ const m = $('settingsModal'); return !!(m && !m.classList.contains('hidden')); }
function pollGamepads(){
  const pads = getGamepadsList();
  const status = pads.length ? ((pads[0].id || 'Gamepad') + (pads.length > 1 ? (' +' + (pads.length - 1)) : '')) : (navigator.getGamepads ? 'No gamepad detected' : 'Gamepad API unavailable');
  if(status !== G.padStatus){ G.padStatus = status; renderKeys(); }
  if(G.awaitingPadRebind){
    const bind = detectPadBind(pads);
    if(bind){
      assignPadMap(G.awaitingPadRebind, bind);
      G.awaitingPadRebind = null;
      renderKeys();
    }
  } else {
    const blocked = gamepadInputBlocked();
    Object.keys(DEFAULT_GAMEPAD).forEach(a=>{
      const down = padMapDown(G.pads[a], pads);
      if(down && !G.padPrev[a] && !blocked) runControlAction(a);
      G.padPrev[a] = down;
    });
  }
  requestAnimationFrame(pollGamepads);
}
function startGamepadLoop(){
  if(G.padLoopStarted) return;
  G.padLoopStarted = true;
  pollGamepads();
}
function gamepadRumble(duration, weak, strong){
  if(G.opts.gamepadRumble === false) return false;
  const pads = getGamepadsList();
  let used = false;
  for(const gp of pads){
    const d = Math.max(1, Math.round(Number(duration) || 1));
    const w = clamp(Number(weak), 0, 1), s = clamp(Number(strong), 0, 1);
    const va = gp.vibrationActuator;
    try{
      if(va && va.playEffect){ va.playEffect('dual-rumble', {startDelay:0, duration:d, weakMagnitude:w, strongMagnitude:s}).catch(()=>{}); used = true; }
      else if(va && va.pulse){ va.pulse(Math.max(w, s), d).catch(()=>{}); used = true; }
      else if(gp.hapticActuators && gp.hapticActuators.length){
        gp.hapticActuators.forEach(h=>{ if(h && h.pulse) h.pulse(Math.max(w, s), d).catch(()=>{}); });
        used = true;
      }
    }catch(_){ }
  }
  return used;
}
function stopGamepadRing(cancel){
  if(G.rumbleTimer){ clearInterval(G.rumbleTimer); G.rumbleTimer = null; }
  G.rumbleUntil = 0;
  if(cancel) gamepadRumble(1, 0, 0);
}
function startGamepadRing(ms){
  if(G.opts.gamepadRumble === false) return;
  stopGamepadRing(false);
  G.rumbleUntil = performance.now() + clamp(Number(ms) || 1200, 250, 4500);
  const pulse = ()=>{
    if(performance.now() > G.rumbleUntil){ stopGamepadRing(false); return; }
    gamepadRumble(190, 0.75, 1.0);
  };
  pulse();
  G.rumbleTimer = setInterval(pulse, 420);
}
function startPickupRumble(e, fallbackMs){
  let ms = Number(fallbackMs) || 0;
  if(!ms && e && Number.isFinite(e.end)) ms = Math.max(250, (e.end - (video.currentTime || e.start || 0)) * 1000);
  startGamepadRing(ms || 1200);
}

/* ---------- debug-only rendering ---------- */
function renderDebug(){
  if(!DEBUG) return;
  const sel = $('eventSelect');
  if(sel){
    sel.innerHTML = '';
    G.events.forEach((e,i)=>{
      const o = document.createElement('option'); o.value = i;
      o.textContent = String(i+1).padStart(2,'0') + ' · ' + e.type + ' @' + fmtTime(e.start) + (e.type==='answer'?(' · ans '+e.answer):'') + (e.delayedCue?(' · delayed cue '+e.cueGap+'s'):'') + (e.bytes?(' · '+e.bytes):'');
      sel.appendChild(o);
    });
  }
  const tbl = $('eventTable');
  if(tbl){
    let h = '<table><thead><tr><th>#</th><th>Type</th><th>Start</th><th>End</th><th>Answer</th><th>Checkpoint</th><th>Cue</th><th>Bytes</th></tr></thead><tbody>';
    G.events.forEach((e,i)=>{ h += '<tr><td>'+(i+1)+'</td><td>'+e.type+'</td><td>'+fmtTime(e.start)+'</td><td>'+fmtTime(e.end)+'</td><td>'+(e.answer||'')+'</td><td>'+(e.checkpoint!=null?fmtTime(e.checkpoint):'')+'</td><td>'+(e.delayedCue?('delayed '+e.cueGap+'s'):'')+'</td><td>'+(e.bytes||'')+'</td></tr>'; });
    tbl.innerHTML = h + '</tbody></table>';
  }
  if(G.analysis){
    setText('channelReport', G.analysis.all.map(r=>G.analysis.channelName(r.channel)+': '+r.answers+' answers, '+r.packets.length+' packets, '+Math.round((r.carrier||8000)/1000)+' kHz carrier').join('  |  '));
  }
  const dbg = $('debugModeSelect'); if(dbg) dbg.value = gameMode();
}
function jumpToEvent(i){
  const e = G.events[i]; if(!e || !video.src) return;
  G.internalSeek = true; video.currentTime = e.start - 0.1 < 0 ? 0 : e.start - 0.1;
  // re-arm play index at the matching interactive event if any
  const pi = G.play.indexOf(e); if(pi >= 0){ G.curIdx = pi; G.play.forEach((pe,k)=>{ pe.done = k < pi; pe.got = null; }); }
  G.active = null;
  G.ended = false;
  G.running = true;
  clearActive();
  setTimeout(()=>{ G.internalSeek = false; }, 200);
  log('Jumped to event ' + (i+1) + ' (' + e.type + ') @' + fmtTime(e.start));
}

/* ---------- wiring ---------- */
function openPicker(){ const f = $('fileInput'); if(f){ f.value=''; f.click(); } }
on('fileInput','change', ()=>handleFiles($('fileInput').files));
on('maniaMusicInput','change', ()=>{ const f = $('maniaMusicInput'); if(f && f.files && f.files[0]) setManiaMusicFile(f.files[0]); });
on('chooseManiaMusic','click', openManiaMusicPicker);
on('clearManiaMusic','click', clearManiaMusicFile);
['uploadBtn','uploadBtnTop','dropZone'].forEach(id=>on(id,'click',openPicker));
on('startOverlay','click', requestStartFromThumbnail);
on('playPause','click', ()=>{ sfxCtx(); if(!video.src) return; if(maniaMode()){ log('Play button disabled in Minigame mode.'); updateHud(); return; } if(!requireDecodedAudio('play button')) return; if(startOverlayVisible()){ requestStartFromThumbnail(); return; } if(difficultyOpen()){ G.pendingDifficultyAction = 'start'; return; } if(!G.running && !G.ended && (video.currentTime||0) <= 0.2){ startRun({restart:true}); return; } if(G.ended){ if(DEBUG) startRun({restart:true}); else askDifficulty('restart'); return; } togglePause(); });
on('restartBtn','click', ()=>{ if(DEBUG) startRun({restart:true}); else askDifficulty('restart'); });
on('fullscreenBtn','click', toggleFullscreen);
on('fullscreenSideBtn','click', toggleFullscreen);
on('stateBtn','click', openStateModal);
on('stateSideBtn','click', openStateModal);
on('settingsBtn','click', ()=>{ show('settingsModal', true); renderKeys(); renderManiaMusicSetting(); });
on('closeSettings','click', ()=>show('settingsModal', false));
on('closeState','click', ()=>show('stateModal', false));
on('saveStateAction','click', saveState);
on('loadStateAction','click', loadState);
on('resetKeys','click', ()=>{ G.keys = Object.assign({}, DEFAULT_KEYS); saveJSON(KEY_STORE, G.keys); renderKeys(); });
on('resetGamepad','click', ()=>{ G.pads = Object.assign({}, DEFAULT_GAMEPAD); G.padPrev = {}; saveJSON(GAMEPAD_STORE, G.pads); renderKeys(); });
document.querySelectorAll('.phoneBtn').forEach(b=>b.addEventListener('click', ()=>onAnswer(Number(b.dataset.answer))));
on('pickupBtn','click', onPickup);
on('startBtn','click', startRun);
on('resultsClose','click', ()=>{ show('results', false); });
on('resultRetry','click', ()=>{ show('results', false); startRun({restart:true}); });
on('resultDifficulty','click', ()=>{ show('results', false); askDifficulty('restart'); });

const hardEl = $('hardMode');
if(hardEl){ hardEl.checked = hardMode(); hardEl.addEventListener('change', ()=>{ setMode(hardEl.checked ? 'hard' : 'easy'); }); }
const livesEl = $('livesInput');
if(livesEl){ livesEl.value = G.opts.lives; livesEl.addEventListener('change', ()=>{ G.opts.lives = clamp(parseInt(livesEl.value)||3,1,9); saveJSON(OPT_STORE, G.opts); resetGame(); }); }
const fsControlsEl = $('fullscreenControlsOpt');
if(fsControlsEl){
  fsControlsEl.checked = G.opts.fullscreenControls !== false;
  fsControlsEl.addEventListener('change', ()=>{
    G.opts.fullscreenControls = !!fsControlsEl.checked;
    saveJSON(OPT_STORE, G.opts);
    applyFullscreenLayout();
  });
}

const gpRumbleEl = $('gamepadRumbleOpt');
if(gpRumbleEl){
  gpRumbleEl.checked = G.opts.gamepadRumble !== false;
  gpRumbleEl.addEventListener('change', ()=>{
    G.opts.gamepadRumble = !!gpRumbleEl.checked;
    saveJSON(OPT_STORE, G.opts);
  });
}
renderManiaMusicSetting();

document.querySelectorAll('.keyRow').forEach(row=>row.addEventListener('click', ()=>{ G.awaitingRebind = row.dataset.action; renderKeys(); }));
document.querySelectorAll('.difficultyChoice').forEach(b=>b.addEventListener('click', ()=>setMode(b.dataset.mode)));
const debugMode = $('debugModeSelect');
if(debugMode){ debugMode.value = gameMode(); debugMode.addEventListener('change', ()=>setDebugMode(debugMode.value)); }

document.addEventListener('keydown', ev=>{
  const tag = (ev.target && ev.target.tagName || '').toLowerCase();
  if(tag === 'input' || tag === 'select' || tag === 'textarea') return;
  if(G.awaitingRebind){ G.keys[G.awaitingRebind] = ev.key; G.awaitingRebind = null; G.awaitingPadRebind = null; saveJSON(KEY_STORE, G.keys); renderKeys(); ev.preventDefault(); return; }
  const a = actionForKey(ev.key);
  if(!a) return;
  ev.preventDefault();
  runControlAction(a);
});

// drag & drop anywhere
['dragenter','dragover'].forEach(t=>window.addEventListener(t, e=>{ if(e.dataTransfer){ e.preventDefault(); e.dataTransfer.dropEffect='copy'; document.body.classList.add('drag'); } }, true));
['dragleave','drop'].forEach(t=>window.addEventListener(t, e=>{ document.body.classList.remove('drag'); }, true));
window.addEventListener('drop', async e=>{ e.preventDefault(); const files = await filesFromDrop(e.dataTransfer); if(files && files.length) handleFiles(files); }, true);

video.addEventListener('ended', endRun);
video.addEventListener('loadedmetadata', ()=>{ updateHud(); refreshEmbeddedSubtitles(); renderSubtitles(); });
video.addEventListener('timeupdate', renderSubtitles);
video.addEventListener('seeked', renderSubtitles);
video.addEventListener('emptied', ()=>{ clearSubtitles(); });
video.addEventListener('play', ()=>{ if(!decodedAudioReady()){ requireDecodedAudio('native video play'); return; } updateHud(); renderSubtitles(); });
video.addEventListener('pause', ()=>{ updateHud(); renderSubtitles(); });
if(video.textTracks && video.textTracks.addEventListener){
  video.textTracks.addEventListener('addtrack', ()=>setTimeout(refreshEmbeddedSubtitles, 50));
  video.textTracks.addEventListener('change', renderSubtitles);
}
// debug jump wiring
const eventSel = $('eventSelect');
if(eventSel) eventSel.addEventListener('change', ()=>jumpToEvent(Number(eventSel.value)));
on('nextBtn','click', ()=>{ const t = video.currentTime||0; const i = G.events.findIndex(e=>e.start > t + 0.3); if(i>=0) jumpToEvent(i); });

window.addEventListener('gamepadconnected', ev=>{ G.padStatus = (ev.gamepad && ev.gamepad.id) || 'Gamepad connected'; renderKeys(); startGamepadLoop(); });
window.addEventListener('gamepaddisconnected', ()=>{ G.padStatus = getGamepadsList().length ? G.padStatus : 'No gamepad detected'; G.padPrev = {}; renderKeys(); });
startGamepadLoop();

initSaveStateDatabase();
renderKeys();
applyFullscreenLayout();
requestAnimationFrame(loop);
