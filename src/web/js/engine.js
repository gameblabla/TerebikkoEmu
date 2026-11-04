
/*
 * Shared Terebikko engine: both-channel audio analysis, event timeline
 * construction and decoder-worker plumbing. Pure logic (no DOM) except for
 * reading the embedded worker source when a decode starts.
 */

// The decoder worker is embedded as plain text by src/web/build.js so each generated
// HTML file stays standalone. Read it only when a decode actually starts.
function decoderWorkerSource(){
  const node = document.getElementById('decoderWorkerSource');
  if(!node) throw new Error('Decoder worker source is missing from the page.');
  return node.textContent;
}

function clamp(n,a,b){ return Math.max(a, Math.min(b,n)); }
function round2(x){ return Math.round(x*100)/100; }
function fmtTime(t){
  if(!Number.isFinite(t)) return '--:--';
  const m = Math.floor(t/60), s = t - m*60;
  return String(m).padStart(2,'0') + ':' + s.toFixed(1).padStart(4,'0');
}
function byteHex(bytes){ return (bytes||[]).map(b=>b.toString(16).toUpperCase().padStart(2,'0')).join(' '); }
function hammingByteForEvent(a,b){ let x=(a^b)&255, n=0; while(x){ n += x&1; x >>= 1; } return n; }
function byteAnswerForEvent(bytes){
  if(!bytes || bytes.length < 3) return null;
  const opcodeErr = hammingByteForEvent(bytes[0], 0x0D);
  const marker = bytes[2] & 0x0F;
  if(opcodeErr > 1 || !(marker === 0x0E || marker === 0x0F)) return null;
  return ((bytes[2] >> 6) & 3) + 1;
}
function exactByteAnswerForEvent(bytes){
  if(!bytes || bytes.length < 3 || bytes[0] !== 0x0D) return null;
  const marker = bytes[2] & 0x0F;
  if(!(marker === 0x0E || marker === 0x0F)) return null;
  return ((bytes[2] >> 6) & 3) + 1;
}

// Decide an answer (1-4) from a set of reads by weighted majority.
function voteAnswer(reads){
  const valid = (reads||[]).filter(p => p && p.answer >= 1 && p.answer <= 4);
  if(!valid.length) return null;
  const scores = new Map();
  for(const p of valid){
    const w = (Number(p.answerConfidence)||0.5) + (Number(p.quality)||0) + Math.min(1, (Number(p.nbits)||0)/40);
    scores.set(p.answer, (scores.get(p.answer)||0) + w);
  }
  let best = null, bestScore = -Infinity;
  for(const [a,s] of scores.entries()){ if(s > bestScore){ best = a; bestScore = s; } }
  return best;
}

// Run the off-thread decoder on one channel's samples.
function runDecodeWorker(samples, sampleRate, onProgress){
  return new Promise((resolve, reject)=>{
    const blob = new Blob([decoderWorkerSource()], {type:'application/javascript'});
    const url = URL.createObjectURL(blob);
    const worker = new Worker(url);
    worker.onmessage = ev => {
      const m = ev.data;
      if(m.type === 'progress' && onProgress) onProgress(m.value, m.text);
      if(m.type === 'done'){ URL.revokeObjectURL(url); worker.terminate(); resolve({packets:m.packets, carrier:m.carrier}); }
      if(m.type === 'error'){ URL.revokeObjectURL(url); worker.terminate(); reject(new Error(m.message)); }
    };
    worker.onerror = err => { URL.revokeObjectURL(url); worker.terminate(); reject(err); };
    worker.postMessage({samples, sampleRate}, [samples.buffer]);
  });
}

// Decode every channel and pick the one with the most answer pulses. The Telebikko /
// See 'N Say signal can live on either the left or right channel, so we never assume.
async function analyzeChannels(audioBuf, prefer, onProgress){
  const sr = audioBuf.sampleRate;
  const n = audioBuf.numberOfChannels;
  const order = [];
  if(prefer === 'left') order.push(0);
  else if(prefer === 'right' && n > 1) order.push(1);
  else { for(let c=0;c<n;c++) order.push(c); }
  const results = [];
  for(const c of order){
    const samples = audioBuf.getChannelData(c).slice();
    const { packets, carrier } = await runDecodeWorker(samples, sr, onProgress);
    const answers = packets.filter(p => p.answer >= 1 && p.answer <= 4).length;
    results.push({channel:c, packets, answers, carrier});
  }
  results.sort((a,b)=> b.answers - a.answers || a.channel - b.channel);
  return { chosen: results[0], all: results, channelName: c => (c===0?'Left':c===1?'Right':'Ch'+c) };
}

const DELAYED_CUE_DB = [
  {
    id:'see-n-say-treasure-hunt-red-countdown',
    carrier:7000,
    answerBytes:'0D E8 7F EE',
    minute:7,
    secondMin:35,
    secondMax:45,
    previousType:'sound',
    gapMin:6.5,
    gapMax:8.5
  }
];
function applyDelayedCueDatabase(events, meta){
  const carrier = Math.round((meta && meta.carrier) || 0);
  for(const entry of DELAYED_CUE_DB){
    if(entry.carrier && Math.abs(carrier - entry.carrier) > 250) continue;
    for(let i=0;i<events.length;i++){
      const e = events[i];
      if(e.type !== 'answer' || e.bytes !== entry.answerBytes) continue;
      const minute = Math.floor(e.start / 60), second = e.start - minute * 60;
      if(minute !== entry.minute || second < entry.secondMin || second > entry.secondMax) continue;
      const prev = events[i-1];
      if(entry.previousType && (!prev || prev.type !== entry.previousType)) continue;
      const gap = prev ? e.start - prev.start : 0;
      if(gap < entry.gapMin || gap > entry.gapMax) continue;
      e.delayedCue = true;
      e.delayedCueId = entry.id;
      e.earlyGuardStart = round2(prev.start);
      e.cueGap = round2(gap);
    }
  }
}

// Build the play timeline: pick-up rings, talk/sound pulses and answer questions.
function buildEvents(packets, meta){
  const ps = (packets||[]).filter(p => p.kind || p.answerCandidate).slice().sort((a,b)=>a.time-b.time);
  const groups = [];
  for(const p of ps){
    const g = groups[groups.length-1];
    if(g && p.time - g.t1 < 1.0){ g.items.push(p); g.t1 = p.time; }
    else groups.push({t0:p.time, t1:p.time, items:[p]});
  }
  const events = [];
  for(const g of groups){
    const exact = g.items.filter(p => p.kind === 'answer' && p.answer >= 1 && p.answer <= 4);
    const ans = g.items.filter(p => (p.kind === 'answer' || p.answerCandidate) && p.answer >= 1 && p.answer <= 4);
    if(exact.length){
      const t0 = Math.min(...exact.map(p => p.time));
      const primary = exact.filter(p => p.time - t0 < 0.12);
      let answer = voteAnswer(primary.length ? primary : exact);
      if(ans.some(p => p.answerCandidate)){
        let correctedByCandidate = false;
        const candidates = ans.filter(p => p.answerCandidate && p.time - t0 >= 0.15 && p.time - t0 <= 0.45);
        const primaryAnswers = [...new Set((primary.length ? primary : exact).map(p => p.answer).filter(a => a >= 1 && a <= 4))];
        const candidateAnswers = [...new Set(candidates.map(p => p.answer).filter(a => a >= 1 && a <= 4))];
        if(primaryAnswers.length === 1 && candidateAnswers.length === 1 && primaryAnswers[0] !== candidateAnswers[0]){
          const candidateQuality = Math.max(...candidates.map(p => Number(p.quality)||0));
          const primaryHasGapRun = (primary.length ? primary : exact).some(p => p.mode === 'gap-run');
          if(candidateQuality >= 0.82 && primaryHasGapRun){ answer = candidateAnswers[0]; correctedByCandidate = true; }
        }
        const candidateVote = !correctedByCandidate && answer === primaryAnswers[0] ? voteAnswer(ans) : null;
        if(candidateVote != null) answer = candidateVote;
      }
      const companions = ans.filter(p => p.time - t0 >= 0.15 && p.time - t0 <= 0.45);
      if(companions.length){
        const primarySet = primary.length ? primary : exact.filter(p => p.time - t0 < 0.15);
        const primaryByteAnswers = [...new Set(primarySet.map(p => exactByteAnswerForEvent(p.bytes)).filter(a => a >= 1 && a <= 4))];
        const companionByteAnswers = [...new Set(companions.map(p => exactByteAnswerForEvent(p.bytes)).filter(a => a >= 1 && a <= 4))];
        if(primaryByteAnswers.length === 1 && companionByteAnswers.length === 1 &&
           primaryByteAnswers[0] === companionByteAnswers[0] && answer !== primaryByteAnswers[0]){
          answer = primaryByteAnswers[0];
        }
      }
      // Window length from the primary->companion pulse spacing, else a default.
      // Exact-only pairs keep the primary-pulse answer. Recovered one-bit candidates
      // are allowed to join the vote so a noisy primary can be corrected by its clean
      // companion without game-specific rules.
      const times = [...new Set(ans.map(p => round2(p.time)))].sort((x,y)=>x-y);
      let dur = 10;
      if(times.length >= 2){ const gap = times[1] - times[0]; if(gap >= 0.15 && gap <= 0.45) dur = clamp(gap*40, 7, 14); }
      events.push({type:'answer', start:round2(t0), end:round2(t0+dur), answer, bytes:byteHex(exact[0].bytes)});
    } else {
      const typed = g.items.filter(p => p.kind);
      if(!typed.length) continue;
      const counts = {};
      for(const p of typed) counts[p.kind] = (counts[p.kind]||0) + 1;
      const kind = Object.entries(counts).sort((a,b)=>b[1]-a[1])[0][0];
      events.push({type:kind, start:round2(g.t0), end:round2(g.t1), bytes:byteHex(typed[0].bytes)});
    }
  }
  // Pick-up rings get a reaction window that runs until the caller starts talking.
  for(let i=0;i<events.length;i++){
    if(events[i].type === 'pickup'){
      const next = events[i+1];
      const winEnd = next ? Math.min(next.start, events[i].start + 7) : events[i].start + 5;
      events[i].end = round2(Math.max(events[i].start + 2.0, winEnd));
    }
  }
  // Resume point for a failed QTE: replay from just before the prompt.
  for(let i=0;i<events.length;i++){
    const e = events[i];
    if(e.type === 'answer'){
      const prev = events[i-1];
      e.checkpoint = round2(prev && (e.start - prev.start) < 16 ? Math.max(0, prev.start - 0.4) : Math.max(0, e.start - 6));
    } else if(e.type === 'pickup'){
      e.checkpoint = round2(Math.max(0, e.start - 0.6));
    }
  }
  applyDelayedCueDatabase(events, meta || {});
  return events;
}

// The interactive subset that hard mode scores / fails on.
function playEvents(events){ return (events||[]).filter(e => e.type === 'pickup' || e.type === 'answer'); }
