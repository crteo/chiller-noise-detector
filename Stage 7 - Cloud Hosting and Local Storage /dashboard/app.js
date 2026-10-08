import {initializeApp} from "https://www.gstatic.com/firebasejs/12.4.0/firebase-app.js";
import {getAuth,GoogleAuthProvider,onAuthStateChanged,signInWithPopup,signOut} from "https://www.gstatic.com/firebasejs/12.4.0/firebase-auth.js";
import {collection,doc,getDocs,getFirestore,limit,onSnapshot,orderBy,query} from "https://www.gstatic.com/firebasejs/12.4.0/firebase-firestore.js";
import {getDownloadURL,getStorage,ref} from "https://www.gstatic.com/firebasejs/12.4.0/firebase-storage.js";

const ui={notice:document.querySelector("#notice"),content:document.querySelector("#content"),title:document.querySelector("#title"),identity:document.querySelector("#identity"),signin:document.querySelector("#signin"),signout:document.querySelector("#signout"),system:document.querySelector("#system"),comparison:document.querySelector("#comparison"),captures:document.querySelector("#captures"),canvas:document.querySelector("#spectrum"),selected:document.querySelector("#selected-label"),audio:document.querySelector("#audio"),refresh:document.querySelector("#refresh")};
let config,auth,db,storage,unsubscribe,captures=[];
const fmt=(value,digits=2)=>Number.isFinite(Number(value))?Number(value).toFixed(digits):"—";
const escapeText=value=>String(value??"");

function notice(message,error=false){ui.notice.textContent=message;ui.notice.classList.toggle("error",error);ui.notice.hidden=false}
function row(label,value){const div=document.createElement("div");div.className="row";const left=document.createElement("span");left.textContent=label;const right=document.createElement("span");right.textContent=value;div.append(left,right);return div}
function nodeByRole(capture,role){return Object.values(capture.nodes||{}).find(node=>node.role===role)}
function renderStatus(data){ui.system.replaceChildren();const current=data.current||{};ui.system.append(row("Master",current.master_id||"No status"),row("Clock",current.clock_set?"UTC synchronized":"Not synchronized"),row("Master upstream",current.upstream?.connected?`Connected (${current.upstream.ip})`:"Offline / optional"),row("Last cloud update",data.current_updated_at?.toDate?.().toLocaleString()||"—"));const contrast=current.contrast;if(contrast?.available)ui.comparison.textContent=`${fmt(contrast.broadband_db_difference)} dB near − reference (capture ${contrast.capture_id})`;else ui.comparison.textContent="Waiting for a matched capture"}

async function loadCaptures(){
  const reference=collection(db,"assets",config.assetId,"captures");
  const result=await getDocs(query(reference,orderBy("capture_start_utc","desc"),limit(Number(config.captureLimit)||50)));
  captures=result.docs.map(item=>({id:item.id,...item.data()}));
  ui.captures.replaceChildren();
  for(const capture of captures){
    const near=nodeByRole(capture,"near"),background=nodeByRole(capture,"reference");
    const tr=document.createElement("tr");
    const values=[new Date(capture.capture_start_utc).toLocaleString(),capture.capture_id,`${fmt(near?.rms_dbfs)} dBFS`,`${fmt(background?.rms_dbfs)} dBFS`,near&&background?`${fmt(near.rms_dbfs-background.rms_dbfs)} dB`:"—",near?.preview_storage_path||background?.preview_storage_path?"Available":"Not uploaded"];
    for(const value of values){const td=document.createElement("td");td.textContent=escapeText(value);tr.append(td)}
    tr.addEventListener("click",()=>selectCapture(capture));ui.captures.append(tr);
  }
  if(captures[0])selectCapture(captures[0]);
}

function drawSpectrum(capture){
  const canvas=ui.canvas,ctx=canvas.getContext("2d"),near=nodeByRole(capture,"near"),background=nodeByRole(capture,"reference");
  ctx.clearRect(0,0,canvas.width,canvas.height);ctx.fillStyle="#0c1216";ctx.fillRect(0,0,canvas.width,canvas.height);
  const series=[{node:near,color:"#4fd1c5"},{node:background,color:"#ffad66"}].filter(item=>item.node?.spectrum_hz?.length);
  if(!series.length){ctx.fillStyle="#94a3ad";ctx.font="18px system-ui";ctx.fillText("No spectrum stored for this capture",30,50);return}
  const allDb=series.flatMap(item=>item.node.spectrum_dbfs);const minDb=Math.floor(Math.min(...allDb)/10)*10,maxDb=Math.min(0,Math.ceil(Math.max(...allDb)/10)*10);const pad={l:65,r:20,t:20,b:45};
  ctx.strokeStyle="#27333c";ctx.fillStyle="#94a3ad";ctx.font="12px system-ui";
  for(let i=0;i<=5;i++){const y=pad.t+(canvas.height-pad.t-pad.b)*i/5;const db=maxDb-(maxDb-minDb)*i/5;ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(canvas.width-pad.r,y);ctx.stroke();ctx.fillText(`${db.toFixed(0)} dBFS`,8,y+4)}
  for(const item of series){const hz=item.node.spectrum_hz,db=item.node.spectrum_dbfs;ctx.strokeStyle=item.color;ctx.lineWidth=2;ctx.beginPath();db.forEach((value,index)=>{const x=pad.l+(canvas.width-pad.l-pad.r)*index/(db.length-1);const y=pad.t+(canvas.height-pad.t-pad.b)*(maxDb-value)/(maxDb-minDb||1);index?ctx.lineTo(x,y):ctx.moveTo(x,y)});ctx.stroke();ctx.fillStyle="#94a3ad";ctx.fillText(`${Math.round(hz[0])} Hz`,pad.l,canvas.height-15);ctx.fillText(`${Math.round(hz.at(-1))} Hz`,canvas.width-pad.r-55,canvas.height-15)}
}

async function selectCapture(capture){ui.selected.textContent=`Capture ${capture.capture_id} · ${new Date(capture.capture_start_utc).toLocaleString()}`;drawSpectrum(capture);ui.audio.replaceChildren();for(const role of ["near","reference"]){const node=nodeByRole(capture,role);const card=document.createElement("div");card.className="audio-card";const label=document.createElement("strong");label.textContent=role==="near"?"Near-field microphone":"Reference microphone";card.append(label);if(node?.preview_storage_path){try{const player=document.createElement("audio");player.controls=true;player.preload="none";player.src=await getDownloadURL(ref(storage,node.preview_storage_path));card.append(player)}catch(error){const message=document.createElement("p");message.className="muted";message.textContent="Audio is unavailable or access was denied.";card.append(message)}}else{const message=document.createElement("p");message.className="muted";message.textContent="Preview audio was not uploaded for this capture.";card.append(message)}ui.audio.append(card)}}

async function start(){
  try{config=await fetch("config.json",{cache:"no-store"}).then(response=>{if(!response.ok)throw new Error("Missing dashboard/config.json");return response.json()});ui.title.textContent=config.displayName||"Acoustic Monitor";const app=initializeApp(config.firebase);auth=getAuth(app);db=getFirestore(app);storage=getStorage(app);ui.signin.onclick=()=>signInWithPopup(auth,new GoogleAuthProvider());ui.signout.onclick=()=>signOut(auth);ui.refresh.onclick=()=>loadCaptures().catch(error=>notice(error.message,true));onAuthStateChanged(auth,async user=>{if(unsubscribe){unsubscribe();unsubscribe=null}ui.identity.textContent=user?.email||"Signed out";ui.signin.hidden=Boolean(user);ui.signout.hidden=!user;ui.content.hidden=!user;if(!user){notice("Sign in with an authorized Google account to view pilot data.");return}notice("Loading pilot data…");try{unsubscribe=onSnapshot(doc(db,"assets",config.assetId),snapshot=>renderStatus(snapshot.data()||{}));await loadCaptures();ui.notice.hidden=true}catch(error){notice(`Access failed: ${error.message}`,true)}})}catch(error){notice(`Dashboard setup error: ${error.message}`,true)}}
start();
