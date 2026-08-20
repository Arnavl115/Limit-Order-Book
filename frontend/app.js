// ADM/USDT — image-matched dark, candlestick + MA + volume, rAF batching
(() => {
  const byId = id => document.getElementById(id);
  const book = { bids: new Map(), asks: new Map(), bestBid: null, bestAsk: null };
  const trades = [];
  const candles = [];
  let candleMap = new Map();
  const orders = new Map();
  let inventory = 0, lastMid=null, lastPrice=null, lastChg=0, lastSeq=0;
  let ws=null, reconnectDelay=500, latencyMs=null, sendTimes=new Map(), dirty=false, rAF=null, selectedSide='buy';
  const els = {
    lastPx: byId('lastPx'), lastChg: byId('lastChg'), statHigh: byId('statHigh'), statLow: byId('statLow'), statVolADM: byId('statVolADM'), statVolUSDT: byId('statVolUSDT'),
    oVal: byId('oVal'), cVal: byId('cVal'), hVal: byId('hVal'), lVal: byId('lVal'), volVal: byId('volVal'), chgVal: byId('chgVal'),
    topBid: byId('topBid'), topAsk: byId('topAsk'), topSpread: byId('topSpread'), connDot: byId('connDot'), connText: byId('connText'),
    wsStatus: byId('wsStatus'), latMs: byId('latMs'), lastSeq: byId('lastSeq'), depth: byId('depth'), bookStats: byId('bookStats'),
    bids: byId('bids'), asks: byId('asks'), spreadMid: byId('spreadMid'), spreadVal: byId('spreadVal'),
    trades: byId('trades'), orders: byId('orders'), posSummary: byId('posSummary'),
    inPrice: byId('inPrice'), inQty: byId('inQty'), inType: byId('inType'), inTotal: byId('inTotal'),
    sideBuy: byId('sideBuy'), sideSell: byId('sideSell'), btnSubmit: byId('btnSubmit'), btnCancelAll: byId('btnCancelAll'),
    chart: byId('chart'), chartTip: byId('chartTip'), toasts: byId('toasts')
  };
  const fmt = p=>p==null?'—':String(p);
  const nowTime=()=>new Date().toLocaleTimeString();
  function toast(m,err=false){ const d=document.createElement('div'); d.className='toast'+(err?' error':''); d.textContent=m; els.toasts.appendChild(d); setTimeout(()=>d.remove(),3000); }
  function setConn(s){ els.connDot.className='conn-dot '+s; els.connText.textContent=s; els.wsStatus.textContent='WS: '+s; }
  function scheduleRender(){ if(dirty) return; dirty=true; if(!rAF) rAF=requestAnimationFrame(()=>{dirty=false;rAF=null;render();}); }

  function applySnapshot(m){
    book.bids.clear(); book.asks.clear();
    for(let l of (m.bids||[])) book.bids.set(l.price,l.qty);
    for(let l of (m.asks||[])) book.asks.set(l.price,l.qty);
    book.bestBid=m.bestBid ?? (book.bids.size?Math.max(...book.bids.keys()):null);
    book.bestAsk=m.bestAsk ?? (book.asks.size?Math.min(...book.asks.keys()):null);
    lastSeq=m.seq||lastSeq; scheduleRender();
  }
  function applyTick(m){
    const map=m.side==='buy'?book.bids:book.asks;
    if(m.removed||m.qty===0) map.delete(m.price); else map.set(m.price,m.qty);
    book.bestBid=book.bids.size?Math.max(...book.bids.keys()):null;
    book.bestAsk=book.asks.size?Math.min(...book.asks.keys()):null;
    if(m.seq) lastSeq=m.seq;
    scheduleRender();
    setTimeout(()=>{
      const con=m.side==='buy'?els.bids:els.asks;
      const row=con.querySelector(`[data-price="${m.price}"]`);
      if(row){ row.classList.add(m.side==='buy'?'flash-buy':'flash-sell'); setTimeout(()=>row.classList.remove('flash-buy','flash-sell'),400); }
    },30);
  }
  function addTrade(m){
    const t={time:Date.now(), price:m.price, qty:m.qty, side:m.side};
    trades.unshift(t); if(trades.length>200) trades.pop();
    lastPrice=t.price; if(trades.length>1) lastChg=t.price-trades[1].price;
    const sec=Math.floor(t.time/1000);
    let c=candleMap.get(sec);
    if(!c){ c={time:sec*1000, open:t.price, high:t.price, low:t.price, close:t.price, vol:t.qty}; candleMap.set(sec,c); candles.push(c); if(candles.length>160){ const o=candles.shift(); candleMap.delete(Math.floor(o.time/1000)); } }
    else { c.high=Math.max(c.high,t.price); c.low=Math.min(c.low,t.price); c.close=t.price; c.vol+=t.qty; }
    if(m.seq) lastSeq=m.seq;
    scheduleRender();
  }
  function updateOrder(m){
    const id=m.orderId, status=m.status;
    let o=orders.get(id);
    if(!o) o={id, side:m.side, price:m.price, qty:m.qty, status, time:nowTime(), filled:m.filled||0}, orders.set(id,o);
    else { o.status=status; o.filled=m.filled; o.remaining=m.remaining; }
    if(m.seq) lastSeq=m.seq;
    if(status==='rejected') toast(`Order ${id} rejected: ${m.reason||''}`,true);
    const s=sendTimes.get(id); if(s){ latencyMs=Date.now()-s; sendTimes.delete(id); }
    scheduleRender();
  }

  let chartScale=1, chartOffset=0, isDrag=false, dragX=0, dragOff=0;
  function render(){
    els.lastPx.textContent=fmt(lastPrice); els.lastChg.textContent=lastChg===0?'—':(lastChg>0?`+${lastChg}`:`${lastChg}`); els.lastChg.className='value chg '+(lastChg>0?'up':lastChg<0?'down':'');
    els.topBid.textContent=fmt(book.bestBid); els.topAsk.textContent=fmt(book.bestAsk);
    els.topSpread.textContent=(book.bestBid!=null&&book.bestAsk!=null)?String(book.bestAsk-book.bestBid):'—';
    els.lastSeq.textContent=lastSeq||'—'; els.latMs.textContent=latencyMs==null?'—':String(latencyMs);
    els.depth.textContent=`${book.bids.size}/${book.asks.size}`; els.bookStats.textContent=`bids ${book.bids.size} asks ${book.asks.size} trades ${trades.length}`;
    if(book.bestBid!=null&&book.bestAsk!=null){ els.spreadMid.textContent=`${(book.bestBid+book.bestAsk)/2}`; els.spreadVal.textContent=`${book.bestAsk-book.bestBid} spread`; } else { els.spreadMid.textContent='—'; els.spreadVal.textContent='—'; }
    // top stats from candles
    if(candles.length){
      const last=candles[candles.length-1];
      const high=Math.max(...candles.slice(-20).map(c=>c.high)), low=Math.min(...candles.slice(-20).map(c=>c.low));
      if(els.statHigh) els.statHigh.textContent=high.toFixed(6);
      if(els.statLow) els.statLow.textContent=low.toFixed(6);
      if(els.oVal) els.oVal.textContent=last.open.toFixed(6);
      if(els.cVal) els.cVal.textContent=last.close.toFixed(6);
      if(els.hVal) els.hVal.textContent=last.high.toFixed(6);
      if(els.lVal) els.lVal.textContent=last.low.toFixed(6);
      if(els.volVal) els.volVal.textContent=(last.vol/1000).toFixed(2)+'K';
      if(els.chgVal){ const chg=((last.close-last.open)/last.open*100).toFixed(2)+'%'; els.chgVal.textContent=chg; els.chgVal.style.color=last.close>=last.open?'#26A69A':'#EF5350'; }
      if(els.statVolADM) els.statVolADM.textContent=(candles.reduce((s,c)=>s+c.vol,0)/1000000).toFixed(2)+' M';
      if(els.statVolUSDT) els.statVolUSDT.textContent=(lastPrice? (candles.reduce((s,c)=>s+c.vol,0)*lastPrice/1000).toFixed(2)+' K':'—');
    }
    const maxQty=Math.max(1,...[...book.bids.values(), ...book.asks.values()]);
    els.asks.innerHTML=''; const asks=[...book.asks.entries()].sort((a,b)=>a[0]-b[0]);
    for(let [p,q] of asks){
      const row=document.createElement('div'); row.className='row sell'+(p===book.bestAsk?' best':''); row.dataset.price=p;
      const d=document.createElement('div'); d.className='depth'; d.style.width=`${(q/maxQty*100).toFixed(1)}%`; row.appendChild(d);
      row.innerHTML+=`<span>${p.toFixed(6)}</span><span>${q.toFixed(4)}</span><span>${q.toFixed(4)}</span>`; row.insertBefore(d,row.firstChild); els.asks.appendChild(row);
    }
    els.bids.innerHTML=''; const bids=[...book.bids.entries()].sort((a,b)=>b[0]-a[0]);
    for(let [p,q] of bids){
      const row=document.createElement('div'); row.className='row buy'+(p===book.bestBid?' best':''); row.dataset.price=p;
      const d=document.createElement('div'); d.className='depth'; d.style.width=`${(q/maxQty*100).toFixed(1)}%`; row.appendChild(d);
      row.innerHTML+=`<span>${p.toFixed(6)}</span><span>${q.toFixed(4)}</span><span>${q.toFixed(4)}</span>`; row.insertBefore(d,row.firstChild); els.bids.appendChild(row);
    }
    els.trades.innerHTML=''; for(let t of trades.slice(0,80)){ const r=document.createElement('div'); r.className='row '+t.side; const tm=new Date(t.time).toLocaleTimeString().slice(0,8); r.innerHTML=`<span>${tm}</span><span>${t.price.toFixed(6)}</span><span>${t.qty}</span>`; els.trades.appendChild(r); }
    els.orders.innerHTML=''; for(let [id,o] of [...orders.entries()].sort((a,b)=>b[0]-a[0]).slice(0,80)){ const r=document.createElement('div'); r.className='row'; r.innerHTML=`<span>${o.time}</span><span>${o.id}</span><span class="side ${o.side}">${o.side}</span><span>${o.price}</span><span>${o.qty}</span><span class="status ${o.status}">${o.status}</span><span><button data-id="${o.id}">✕</button></span>`; els.orders.appendChild(r); }
    els.orders.querySelectorAll('button').forEach(b=>b.addEventListener('click',()=>sendCancel(Number(b.dataset.id))));
    els.posSummary.textContent=`Inv ${inventory} — ${orders.size} open`;
    drawChart();
  }

  function ma(values, period, idx){
    if(idx < period-1) return null;
    let sum=0; for(let i=idx-period+1;i<=idx;i++) sum+=values[i];
    return sum/period;
  }
  function drawChart(){
    const c=els.chart, ctx=c.getContext('2d');
    const dpr=window.devicePixelRatio||1, w=c.clientWidth, h=c.clientHeight;
    if(c.width!==w*dpr||c.height!==h*dpr){ c.width=w*dpr; c.height=h*dpr; ctx.setTransform(dpr,0,0,dpr,0,0); }
    ctx.clearRect(0,0,w,h); ctx.fillStyle='#0F1114'; ctx.fillRect(0,0,w,h);
    if(candles.length<2){ ctx.fillStyle='#6A6D78'; ctx.font='12px monospace'; ctx.fillText('Waiting for trades...',12,20); return; }
    const visible=Math.min(80,candles.length), start=Math.max(0,candles.length-visible-Math.floor(chartOffset)), end=Math.min(candles.length,start+visible), slice=candles.slice(start,end);
    if(slice.length<1) return;
    let min=Math.min(...slice.map(x=>x.low)), max=Math.max(...slice.map(x=>x.high));
    const pad=(max-min)*0.12||0.0001; min-=pad; max+=pad; const range=max-min||1;
    const chartH = h*0.72, volH = h*0.18, gapH = h - chartH - volH - 30;
    const candleW=Math.max(3,(w-50)/slice.length*0.65*chartScale), gap=(w-50)/slice.length;
    // grid
    ctx.strokeStyle='#1E222D'; ctx.lineWidth=1;
    for(let i=0;i<4;i++){ const y=14 + chartH*i/3; ctx.beginPath(); ctx.moveTo(44,y); ctx.lineTo(w,y); ctx.stroke(); ctx.fillStyle='#6A6D78'; ctx.font='10px monospace'; ctx.fillText(max - range*i/3 < 0.1 ? (max - range*i/3).toFixed(6) : (max - range*i/3).toFixed(4), 2, y+3); }
    // candles - teal #26A69A up, red #EF5350 down (image)
    const closes=slice.map(c=>c.close);
    slice.forEach((c,i)=>{
      const x=44 + i*gap + gap*0.15;
      const yO=14 + chartH*(1-(c.open-min)/range), yC=14+chartH*(1-(c.close-min)/range), yH=14+chartH*(1-(c.high-min)/range), yL=14+chartH*(1-(c.low-min)/range);
      const up=c.close>=c.open;
      ctx.strokeStyle=up?'#26A69A':'#EF5350'; ctx.fillStyle=up?'#26A69A':'#EF5350';
      ctx.beginPath(); ctx.moveTo(x+candleW/2, yH); ctx.lineTo(x+candleW/2, yL); ctx.stroke();
      const bodyY=Math.min(yO,yC), bodyH=Math.max(1.5, Math.abs(yO-yC));
      if(up) ctx.fillRect(x, bodyY, candleW, bodyH||1); else ctx.fillRect(x, bodyY, candleW, bodyH||1);
    });
    // MA lines: MA5 orange #FFA726, MA10 purple #AB47BC, MA30 blue #42A5F5, MA60 pink #EF5350 (image)
    const maPeriods=[{p:5,c:'#FFA726'},{p:10,c:'#AB47BC'},{p:30,c:'#42A5F5'},{p:60,c:'#EC407A'}];
    // need full closes for MA calc, not just slice
    const allCloses=candles.map(c=>c.close);
    maPeriods.forEach(({p,c:col})=>{
      ctx.strokeStyle=col; ctx.lineWidth=1; ctx.beginPath();
      let started=false;
      slice.forEach((_,i)=>{
        const globalIdx=start+i;
        const v=ma(allCloses,p,globalIdx);
        if(v==null) return;
        const x=44 + i*gap + gap*0.15 + candleW/2;
        const y=14 + chartH*(1-(v-min)/range);
        if(!started){ ctx.moveTo(x,y); started=true; } else ctx.lineTo(x,y);
      });
      ctx.stroke();
    });
    // volume
    const maxVol=Math.max(...slice.map(c=>c.vol),1);
    slice.forEach((c,i)=>{
      const x=44 + i*gap + gap*0.15;
      const hV = (c.vol/maxVol)*volH*0.9;
      const y = 14+chartH+ gapH + volH - hV;
      const up=c.close>=c.open;
      ctx.fillStyle=up?'rgba(38,166,154,0.9)':'rgba(239,83,80,0.9)';
      ctx.fillRect(x, y, candleW, hV);
    });
    // volume MA
    const volMA5=[], volMA10=[];
    slice.forEach((_,i)=>{
      const gi=start+i;
      const v5=ma(candles.map(c=>c.vol),5,gi), v10=ma(candles.map(c=>c.vol),10,gi);
      if(v5!=null) volMA5.push({x:44+i*gap+gap*0.15+candleW/2, y:14+chartH+gapH+volH - (v5/maxVol)*volH*0.9});
      if(v10!=null) volMA10.push({x:44+i*gap+gap*0.15+candleW/2, y:14+chartH+gapH+volH - (v10/maxVol)*volH*0.9});
    });
    ctx.strokeStyle='#FFA726'; ctx.beginPath(); volMA5.forEach((p,i)=>i?ctx.lineTo(p.x,p.y):ctx.moveTo(p.x,p.y)); ctx.stroke();
    ctx.strokeStyle='#AB47BC'; ctx.beginPath(); volMA10.forEach((p,i)=>i?ctx.lineTo(p.x,p.y):ctx.moveTo(p.x,p.y)); ctx.stroke();
    // time labels
    ctx.fillStyle='#6A6D78'; ctx.font='10px monospace';
    if(slice.length>0){ ctx.fillText(new Date(slice[0].time).toLocaleTimeString().slice(0,5),44,14+chartH+gapH+volH+12); ctx.fillText(new Date(slice[slice.length-1].time).toLocaleTimeString().slice(0,5),w-60,14+chartH+gapH+volH+12); }
  }

  let ws=null;
  function wsUrl(){ const p=location.protocol==='https:'?'wss:':'ws:'; return p+'//'+location.host+'/ws'; }
  function connect(){
    const dot=byId('connDot'); if(dot) dot.className='conn-dot offline';
    const url=wsUrl(); ws=new WebSocket(url);
    ws.onopen=()=>{ byId('connDot').className='conn-dot online'; byId('connText').textContent='online'; byId('wsStatus').textContent='WS: online'; ws.send(JSON.stringify({type:'subscribe',channel:'book'})); };
    ws.onmessage=e=>{
      let m; try{m=JSON.parse(e.data)}catch{return;}
      const t=m.type;
      if(t==='marketdata.snapshot') applySnapshot(m);
      else if(t==='marketdata.tick') applyTick(m);
      else if(t==='trade'){ addTrade(m); const tid=m.takerId,mid=m.makerId; const to=orders.get(tid), mo=orders.get(mid); let side=null,qty=m.qty; if(mo) side=mo.side; else if(to) side=m.side; else side=m.side; if(mo){ if(mo.side==='buy') inventory+=qty; else inventory-=qty; } else if(to){ if(to.side==='buy') inventory+=qty; else inventory-=qty; } }
      else if(t==='execution.report') updateOrder(m);
      else if(t==='error') toast(m.reason||'error',true);
      else if(t==='pong') latencyMs=Date.now()-(m.id||0);
    };
    ws.onclose=()=>{ byId('connDot').className='conn-dot offline'; byId('connText').textContent='offline'; byId('wsStatus').textContent='WS: offline'; setTimeout(connect, Math.min(5000, (reconnectDelay*=1.5))); };
    ws.onerror=()=>{ try{ws.close();}catch{} };
  }
  let reconnectDelay=500;
  function handleMessage(m){ /* same as above for length-prefix fallback, not needed for WS */ }

  // order entry
  function updateTotal(){ const p=Number(byId('inPrice').value)||0,q=Number(byId('inQty').value)||0; byId('inTotal').textContent=String(p*q); const t=byId('inType').value; byId('inPrice').disabled=t==='market'; byId('inPrice').style.opacity=t==='market'?'0.5':'1'; }
  byId('inPrice').addEventListener('input',updateTotal); byId('inQty').addEventListener('input',updateTotal); byId('inType').addEventListener('change',updateTotal); updateTotal();
  function setSide(s){ selectedSide=s; byId('sideBuy').classList.toggle('active',s==='buy'); byId('sideSell').classList.toggle('active',s==='sell'); byId('btnSubmit').textContent=`Place ${s==='buy'?'Buy':'Sell'}`; byId('btnSubmit').className='submit '+s; }
  byId('sideBuy').addEventListener('click',()=>setSide('buy')); byId('sideSell').addEventListener('click',()=>setSide('sell'));
  function sendOrder(){
    const side=selectedSide, price=Number(byId('inPrice').value)||0, qty=Number(byId('inQty').value)||0, orderType=byId('inType').value;
    if(qty<=0){toast('Qty must be >0',true);return;}
    if(orderType!=='market'&&price<=0){toast('Price must be >0',true);return;}
    const id=Date.now()+Math.floor(Math.random()*1000);
    const msg={type:'order.new',id,side,price,qty,orderType};
    sendTimes.set(id,Date.now());
    orders.set(id,{id,side,price,qty,status:'pending',time:nowTime(),filled:0}); scheduleRender();
    if(ws&&ws.readyState===1) ws.send(JSON.stringify(msg)); else toast('WS offline',true);
  }
  function sendCancel(id){ if(ws&&ws.readyState===1) ws.send(JSON.stringify({type:'order.cancel',id})); else toast('WS offline',true); }
  function cancelAll(){ for(let [id,o] of orders) if(['resting','partially_filled','new','pending'].includes(o.status)) sendCancel(id); }
  byId('btnSubmit').addEventListener('click',sendOrder);
  byId('btnCancelAll').addEventListener('click',cancelAll);
  byId('bids').addEventListener('click',e=>{ const r=e.target.closest('.row'); if(!r) return; const p=r.dataset.price; if(p) byId('inPrice').value=p; });
  byId('asks').addEventListener('click',e=>{ const r=e.target.closest('.row'); if(!r) return; const p=r.dataset.price; if(p) byId('inPrice').value=p; });
  document.addEventListener('keydown',e=>{
    if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT'){ if(e.key==='Enter'){e.preventDefault();sendOrder();} return; }
    if(e.key==='1') setSide('buy'); else if(e.key==='2') setSide('sell'); else if(e.key==='Enter') sendOrder(); else if(e.key==='Escape') cancelAll();
  });
  const chartEl=byId('chart');
  chartEl.addEventListener('wheel',e=>{ e.preventDefault(); const d=e.deltaY>0?0.9:1.1; chartScale=Math.max(0.5,Math.min(3,chartScale*d)); scheduleRender();},{passive:false});
  chartEl.addEventListener('mousedown',e=>{ isDrag=true; dragX=e.clientX; dragOff=chartOffset; });
  window.addEventListener('mousemove',e=>{ if(isDrag){ chartOffset=dragOff+(e.clientX-dragX)/20; scheduleRender(); }});
  window.addEventListener('mouseup',()=>isDrag=false);
  chartEl.addEventListener('mousemove',e=>{
    const rect=chartEl.getBoundingClientRect(), x=e.clientX-rect.left, w=rect.width, visible=Math.min(80,candles.length), start=Math.max(0,candles.length-visible-Math.floor(chartOffset)), idx=Math.floor((x-44)/((w-44)/visible)), c=candles[start+idx];
    const tip=byId('chartTip');
    if(c){ tip.style.display='block'; tip.style.left=(e.clientX+10)+'px'; tip.style.top=(e.clientY-30)+'px'; tip.textContent=`O:${c.open.toFixed(4)} H:${c.high.toFixed(4)} L:${c.low.toFixed(4)} C:${c.close.toFixed(4)} V:${c.vol}`; } else tip.style.display='none';
  });
  chartEl.addEventListener('mouseleave',()=>byId('chartTip').style.display='none');
  setInterval(()=>{ if(ws&&ws.readyState===1){ const id=Date.now(); sendTimes.set(id,id); ws.send(JSON.stringify({type:'ping',id})); } },5000);
  connect(); setInterval(()=>scheduleRender(),200);
  window._lob={book,trades,orders,wsConnect:connect};
})();
