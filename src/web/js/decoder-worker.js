/*
 * Terebikko / See 'N Say / Videosmarts pulse decoder worker.
 * Embedded as a text/plain script by src/web/build.js so the generated HTML
 * files remain standalone (no external worker file is fetched at runtime).
 */
'use strict';
self.onmessage = function(ev){
  try{
    const samples = ev.data.samples, sr = ev.data.sampleRate;
    const carrier = ev.data.freq || detectCarrier(samples, sr);
    const packets = decode(samples, sr, carrier);
    self.postMessage({type:'done', packets, carrier});
  } catch(err){ self.postMessage({type:'error', message: err && err.message ? err.message : String(err)}); }
};
function postProgress(value, text){ self.postMessage({type:'progress', value, text}); }
function detectCarrier(samples, sr){
  // The control tone is a single narrow carrier, but the exact frequency varies by
  // device: ~8 kHz on Telebikko/Terebikko tapes, ~7 kHz on See 'N Say. Sweep a few
  // candidates with a strided Goertzel and pick the one whose envelope is the most
  // "bursty" (peak-to-median ratio) - i.e. silent most of the time with strong pulses.
  const cands = [6000,6500,7000,7500,8000,8500,9000];
  const win = Math.max(24, Math.round(sr * 0.0015));
  const stride = win * 16;
  let best = 8000, bestRatio = 0;
  for(const f of cands){
    const c2 = 2*Math.cos(2*Math.PI*f/sr);
    const vals = [];
    for(let pos=0; pos+win<samples.length; pos+=stride){
      let s1=0, s2=0;
      for(let k=0;k<win;k++){ const s0 = samples[pos+k] + c2*s1 - s2; s2=s1; s1=s0; }
      vals.push(Math.sqrt(Math.max(0, s1*s1 + s2*s2 - c2*s1*s2)) / win);
    }
    vals.sort((a,b)=>a-b);
    const med = vals[Math.floor(vals.length/2)] || 1e-9;
    const mx = vals[vals.length-1] || 0;
    const ratio = mx / (med || 1e-9);
    if(ratio > bestRatio){ bestRatio = ratio; best = f; }
  }
  return best;
}
function percentile(arr, pct){
  const maxN = 220000, step = Math.max(1, Math.floor(arr.length / maxN)), vals = [];
  for(let i=0;i<arr.length;i+=step){ const v = arr[i]; if(Number.isFinite(v)) vals.push(v); }
  vals.sort((a,b)=>a-b);
  if(!vals.length) return 0;
  return vals[Math.max(0, Math.min(vals.length-1, Math.floor((pct/100)*(vals.length-1))))];
}
function makeEnvelope(samples, sr, freq, showProgress=true){
  freq = freq || 8000;
  const step = Math.max(1, Math.round(sr * 0.001));
  const win = Math.max(24, Math.round(sr * 0.0015));
  const bins = Math.max(0, Math.floor((samples.length - win) / step));
  const env = new Float32Array(bins);
  const cos = new Float32Array(win), sin = new Float32Array(win);
  const w = 2*Math.PI*freq/sr;
  for(let n=0;n<win;n++){ cos[n]=Math.cos(w*n); sin[n]=Math.sin(w*n); }
  for(let i=0,pos=0;i<bins;i++,pos+=step){
    let re=0, im=0;
    for(let n=0;n<win;n++){ const x=samples[pos+n]; re += x*cos[n]; im += x*sin[n]; }
    env[i] = Math.sqrt(re*re + im*im) / win;
    if(showProgress && (i % 50000) === 0) postProgress(Math.min(70, (i / Math.max(1,bins))*70), 'Scanning ' + Math.round(freq/1000) + ' kHz envelope... ' + Math.round((i/Math.max(1,bins))*100) + '%');
  }
  return env;
}
function bitsToBytes(bits){
  const out = [], usable = bits.length - (bits.length % 8);
  for(let i=0;i<usable;i+=8){ out.push(parseInt(bits.slice(i,i+8),2)); }
  return out;
}
function hammingByte(a, b){
  let x = (a ^ b) & 0xFF, n = 0;
  while(x){ n += x & 1; x >>= 1; }
  return n;
}
function decodeAnswerFromBits(bits){
  // Generic answer-pulse decode. Answer pulses are framed by opcode byte 0 = 0x0D /
  // 0x1D, i.e. the bit pattern "000 x 1101" at the start of the packet. Byte 2 holds
  // the correct button in its top two bits; its low nibble is a 0xE/0xF validity
  // marker. answer = top-2-bits(byte2)+1. The tapes send answer pulses in redundant
  // pairs, so accept a one-bit opcode error on longer packets at lower confidence; the
  // event-level vote can then use the clean companion pulse without per-tape signatures.
  if(bits.length < 24) return {answer:null, confidence:0};
  const byte0 = parseInt(bits.slice(0, 8), 2);
  const byte2 = parseInt(bits.slice(16, 24), 2);
  const low = byte2 & 0x0F;
  if(!(low === 0x0F || low === 0x0E)) return {answer:null, confidence:0};
  const opcodeErr = Math.min(hammingByte(byte0, 0x0D), hammingByte(byte0, 0x1D));
  if(opcodeErr > 1 || (opcodeErr === 1 && bits.length < 32)) return {answer:null, confidence:0};
  const markerConfidence = low === 0x0F ? 1.0 : 0.85;
  return {answer:((byte2 >> 6) & 3) + 1, confidence: opcodeErr === 0 ? markerConfidence : markerConfidence * 0.55, candidate: opcodeErr === 1};
}
function classifyControl(bits){
  // Control opcodes share the high nibble 0xA (byte0 1010xxxx). The next two bits
  // pick the event: 101010=pick-up ring (0xA8), 101011=talk/sound (0xAC),
  // 101000=intro/demo (0xA2).
  if(bits.length < 6) return null;
  const p = bits.slice(0,6);
  if(p === '101010'){
    // Intro/demo pulse trains sometimes decode a single noisy 0xAA/0xAB-style byte
    // while retaining the intro payload marker 0xD?. Do not turn those into calls.
    if(bits.length >= 32){
      const byte3 = parseInt(bits.slice(24, 32), 2);
      if((byte3 & 0xF0) === 0xD0) return 'intro';
    }
    return 'pickup';
  }
  if(p === '101011') return 'sound';
  if(p === '101000') return 'intro';
  return null;
}
function buildPacket(run, bits, mode, leaderMark, leaderGap, markErr, gapErr, nbits){
  const bytes = bitsToBytes(bits);
  const meta = decodeAnswerFromBits(bits);
  const control = meta.answer != null ? null : classifyControl(bits);
  const quality = Math.max(0, 1 - (Math.abs(leaderMark-20)/12 + Math.abs(leaderGap-10)/8 + (markErr+gapErr)/Math.max(1,nbits)/4) / 4);
  return {time:run.s/1000, bits, bytes, answer:meta.answer, answerConfidence:meta.confidence,
          answerCandidate: !!meta.candidate, kind: meta.answer != null && !meta.candidate ? 'answer' : control,
          quality, nbits:bits.length, mode};
}
function parseAtThreshold(env, threshold){
  const runs = [];
  let value = env[0] > threshold, start = 0;
  for(let i=1;i<env.length;i++){ const v = env[i] > threshold; if(v !== value){ runs.push({v:value,s:start,e:i}); value=v; start=i; } }
  runs.push({v:value,s:start,e:env.length});
  const packets = [];
  for(let i=0;i<runs.length-3;i++){
    const r = runs[i], gap = runs[i+1];
    if(!r.v || gap.v) continue;
    const leaderMark = r.e - r.s, leaderGap = gap.e - gap.s;
    if(!(leaderMark>=14 && leaderMark<=31 && leaderGap>=5 && leaderGap<=17)) continue;
    // Parser A: classic mark+gap run lengths.
    let bits = '', j = i+2, markErr = 0, gapErr = 0, nbits = 0;
    while(j+1 < runs.length && bits.length < 48){
      const m = runs[j], g = runs[j+1];
      if(!m.v || g.v) break;
      const ml = m.e-m.s, gl = g.e-g.s;
      if(!(ml>=1 && ml<=6)) break;
      if(!(gl>=1 && gl<=10)) break;
      bits += (gl >= 3 ? '1' : '0');
      markErr += Math.abs(ml - 2);
      gapErr += Math.min(Math.abs(gl - 2), Math.abs(gl - 4));
      nbits++; j += 2;
    }
    if(bits.length >= 16) packets.push(buildPacket(r, bits, 'gap-run', leaderMark, leaderGap, markErr, gapErr, nbits));
    // Parser B: data-mark start-to-start periods (survives clipped/split silences).
    const marks = [];
    for(let k=i+2;k<runs.length;k++){
      const rr = runs[k];
      if(!rr.v) continue;
      const len = rr.e - rr.s;
      if(len >= 12) break;
      if(rr.s - r.s > 260) break;
      if(len < 1 || len > 7) continue;
      const last = marks[marks.length - 1];
      if(last && rr.s - last.e <= 1 && rr.e - last.s <= 7){ last.e = rr.e; }
      else { if(last && rr.s - last.s > 10) break; marks.push({s:rr.s,e:rr.e}); }
      if(marks.length >= 50) break;
    }
    if(marks.length >= 17){
      let pbits = '', pMarkErr = 0, pPeriodErr = 0, pN = 0;
      for(let k=0;k<marks.length-1 && pbits.length<48;k++){
        const ml = marks[k].e - marks[k].s;
        const dt = marks[k+1].s - marks[k].s;
        if(!(dt >= 3 && dt <= 9)) break;
        pbits += (dt >= 5 ? '1' : '0');
        pMarkErr += Math.abs(ml - 2);
        pPeriodErr += Math.min(Math.abs(dt - 4), Math.abs(dt - 6));
        pN++;
      }
      if(pbits.length >= 16) packets.push(buildPacket(r, pbits, 'start-period', leaderMark, leaderGap, pMarkErr, pPeriodErr, pN));
    }
  }
  return packets;
}
// Videosmarts answer words are four 10/20 ms bursts: start bit, two answer bits,
// stop bit. Each word is sent four times. Some 9 kHz copies append a short zero burst;
// the A/Apple sample uses an extended final mark.
function videosmartsMarkBit(run){
  const len = run.e-run.s;
  if(len>=7 && len<=13) return '0';
  if(len>=16 && len<=24) return '1';
  return null;
}
function videosmartsAnswer(data){
  // Screen order is yellow=1, blue=2, green=3, red=4; the payload is cyclically offset.
  return [2,3,4,1][data & 3];
}
function videosmartsFrameAt(runs, start){
  if(start>=runs.length || !runs[start].v || start+7>=runs.length) return null;
  let bits='', mark=start, extendedStop=false;
  for(let b=0;b<4;b++){
    if(mark>=runs.length || !runs[mark].v) return null;
    const markLen=runs[mark].e-runs[mark].s;
    const bit=b===3 && markLen>=25 && markLen<=36 ? '1' : videosmartsMarkBit(runs[mark]);
    if(bit==null) return null;
    if(b===3 && markLen>=25 && markLen<=36) extendedStop=true;
    bits += bit;
    if(b<3){
      if(mark+2>=runs.length || runs[mark+1].v) return null;
      const gap=runs[mark+1].e-runs[mark+1].s;
      if(gap<5 || gap>17) return null;
      mark += 2;
    }
  }
  if(bits[0]!=='1' || bits[3]!=='1' || mark+1>=runs.length || runs[mark+1].v) return null;
  const endGap=runs[mark+1].e-runs[mark+1].s;
  let nextRun;
  if(endGap>=12 && endGap<=30 && mark+3<runs.length && runs[mark+2].v){
    const tail=videosmartsMarkBit(runs[mark+2]);
    const tailGap=runs[mark+3].e-runs[mark+3].s;
    if(tail!=='0' || runs[mark+3].v || tailGap<40) return null;
    nextRun=mark+4;
  } else if(endGap>=40){
    nextRun=mark+2;
  } else return null;
  const data=(bits[1]==='1'?2:0) | (bits[2]==='1'?1:0);
  const answer=extendedStop && data===0 ? 1 : videosmartsAnswer(data);
  return {start:runs[start].s, answer, nextRun};
}
function parseVideosmartsAtThreshold(env, threshold){
  const runs=[];
  let value=env[0]>threshold, start=0;
  for(let i=1;i<env.length;i++){
    const v=env[i]>threshold;
    if(v!==value){ runs.push({v:value,s:start,e:i}); value=v; start=i; }
  }
  runs.push({v:value,s:start,e:env.length});
  const packets=[];
  for(let i=0;i<runs.length;){
    const frames=[];
    let at=i;
    while(frames.length<4){
      const f=videosmartsFrameAt(runs,at);
      if(!f) break;
      if(frames.length){
        const delta=f.start-frames[frames.length-1].start;
        if(delta<120 || delta>240) break;
      }
      frames.push(f);
      if(f.nextRun<0 || f.nextRun>=runs.length) break;
      at=f.nextRun;
    }
    if(frames.length>=3){
      const votes=[0,0,0,0,0];
      for(const f of frames) votes[f.answer]++;
      let answer=0;
      for(let a=1;a<=4;a++) if(votes[a]>votes[answer]) answer=a;
      if(answer>=1 && votes[answer]>=3){
        const data=(answer+2)&3;
        const code=8|(data<<1)|1;
        const payload=data.toString(2).padStart(2,'0');
        packets.push({time:frames[0].start/1000, bits:'1'+payload+'1', bytes:[code], answer,
          answerConfidence:votes[answer]/frames.length, answerCandidate:false,
          kind:'answer', quality:0.95, nbits:4, mode:'videosmarts'});
        i=frames[frames.length-1].nextRun;
        if(i>=runs.length) break;
      } else i++;
    } else i++;
  }
  return packets;
}
function votePulseAnswer(reads){
  const valid = (reads || []).filter(p => p.answer >= 1 && p.answer <= 4);
  if(!valid.length) return {answer:null, confidence:0};
  const scores = new Map();
  for(const p of valid){
    const w = (Number(p.answerConfidence)||0.5) + (Number(p.quality)||0) + Math.min(1, (Number(p.nbits)||0)/40);
    scores.set(p.answer, (scores.get(p.answer)||0) + w);
  }
  let best = null, bestScore = -Infinity, total = 0;
  for(const [a,s] of scores.entries()){ total += s; if(s > bestScore){ best = a; bestScore = s; } }
  return {answer:best, confidence: total > 0 ? bestScore/total : 0};
}
function packetScore(p){
  return (p.kind === 'answer' ? 8 : (p.answerCandidate ? 4 : 0)) + (Number(p.answerConfidence)||0)*8 + (p.kind?2:0) + p.nbits/36 + p.quality + (p.mode === 'start-period' ? 0.15 : 0);
}
function dedupe(packets){
  packets.sort((a,b)=>a.time-b.time);
  const out=[];
  for(const p of packets){
    if(p.mode === 'videosmarts' && p.answer >= 1 && p.answer <= 4){
      let duplicate = -1;
      for(let i=out.length-1;i>=0;i--){
        if(p.time-out[i].time>=0.8) break;
        if(out[i].mode==='videosmarts' && out[i].answer===p.answer){ duplicate=i; break; }
      }
      if(duplicate>=0){ if(packetScore(p)>packetScore(out[duplicate])) out[duplicate]=p; continue; }
    }
    const last = out[out.length-1];
    if(last && Math.abs(p.time-last.time)<0.035){ if(packetScore(p) > packetScore(last)) out[out.length-1] = p; }
    else out.push(p);
  }
  return out;
}
function decode(samples, sr, freq){
  freq = freq || detectCarrier(samples, sr);
  postProgress(0, 'Preparing ' + Math.round(freq/1000) + ' kHz envelope...');
  const env = makeEnvelope(samples, sr, freq);
  postProgress(72, 'Choosing thresholds...');
  const base = percentile(env, 50), high = percentile(env, 99.9);
  const ratios = [0.22,0.28,0.34,0.40,0.46,0.52,0.60];
  const videosmartsRatios = [0.28,0.40,0.52];
  let all=[];
  if(high>base){
    for(let i=0;i<ratios.length;i++){
      const threshold=base + ratios[i]*(high-base);
      all = all.concat(parseAtThreshold(env, threshold));
      postProgress(75 + (i+1)/ratios.length*22, 'Parsing pulse packets... threshold '+(i+1)+'/'+ratios.length);
    }
    if(freq>=7500 && freq<=9500){
      for(const ratio of videosmartsRatios) all=all.concat(parseVideosmartsAtThreshold(env,base+ratio*(high-base)));
    }
  }
  // The supplied Videosmarts samples switch between 8 and 9 kHz within one tape.
  // Inspect both carriers so a global carrier estimate cannot discard either one.
  for(const f of [8000,9000]){
    if(Math.abs(f-freq)<1) continue;
    const venv=makeEnvelope(samples,sr,f,false);
    const vbase=percentile(venv,50), vhigh=percentile(venv,99.9);
    if(vhigh>vbase){
      for(const ratio of videosmartsRatios) all=all.concat(parseVideosmartsAtThreshold(venv,vbase+ratio*(vhigh-vbase)));
    }
  }
  all.sort((a,b)=>a.time-b.time);
  // Vote each answer pulse over ALL raw threshold reads before dedupe collapses them.
  const answered = all.filter(p => p.answer >= 1 && p.answer <= 4);
  const pulses = [];
  for(const p of answered){
    const last = pulses[pulses.length-1];
    if(last && p.time - last.t1 < 0.12){ last.reads.push(p); last.t1 = p.time; }
    else pulses.push({t0:p.time, t1:p.time, reads:[p]});
  }
  for(const pu of pulses){ const v = votePulseAnswer(pu.reads); pu.answer = v.answer; pu.confidence = v.confidence; }
  const packets = dedupe(all).filter(p => p.nbits>=20 || p.answer!=null || p.answerCandidate || p.kind);
  for(const p of packets){
    if(p.answer >= 1 && p.answer <= 4){
      const pu = pulses.find(u => p.time >= u.t0 - 0.06 && p.time <= u.t1 + 0.06);
      if(pu && pu.answer){ p.answer = pu.answer; p.answerConfidence = pu.confidence; }
    }
  }
  postProgress(99, 'Decoded ' + packets.length + ' packets.');
  return packets;
}
