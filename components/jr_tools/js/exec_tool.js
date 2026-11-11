// The execute_tool program: source of JR_TOOLS_EXEC_JS in
// ../src/jr_tools_templates.c. Edit THIS file, then run
// `python3 scripts/gen-exec-js.py`; never hand-edit the escaped C line.
// The generator drops full-line comments and indentation and joins the lines,
// so every line must end where a statement may continue (`;`, `{`, `}`, `,`).
//
// __PATH__ and __ARGS__ become the two %s of the snprintf in
// jr_tools_build_code: the path array, then the args object (both printed by
// cJSON, so they are JSON literals, never code). __BUDGET__ is a number the
// generator derives from JR_TOOLS_RESPONSE_CAP.
//
// ORD pins the documented positional order for the tools a voice assistant
// actually reaches for, because Object.values() follows the model's key
// order and weather(longitude, latitude) is a different place.
const P=__PATH__;let A=__ARGS__;
const ORD={websearch:["query"],weather:["latitude","longitude"],wiki:["query"],time:["timezone"],crypto:["coin","currency"],exchange:["base","targets"],country:["name"],geocode:["query"],translate:["text","target","source"],hackernews:["type","count"],research:["question"],"stocks.quote":["symbols"],"reddit.search":["query"],"memory.search":["query"],"holidays.list":["country","year"]};
const name=P.join(".");
let o=jarvis;
for(const k of P.slice(0,-1)){o=o[k];if(o==null)return{error:"unknown tool"}}
const f=o[P[P.length-1]];
if(typeof f!=="function")return{error:"unknown tool"};
const board=P[0]==="coordination";
const note=name==="memory.capture";
if(board){const i=A.input||A;if(!i.identity)i.identity={runtime:"pi",agentId:"jarvisnano",hostId:"jarvisnano",sessionId:new Date().toISOString().slice(0,10)};A=i}
if(note){const i=A.input||A;i.type=i.type||"note";i.source=i.source||"jarvisnano";i.title=i.title||String(i.body||"").slice(0,60);A=i}
const named=P.length>=3||board||note;
const ord=ORD[name];
const v=ord?ord.map(k=>A[k]):Object.values(A);
const first=named?[A]:v;
const second=named?v:[A];
let r;
try{r=await f.call(o,...first)}catch(e1){try{r=await f.call(o,...second)}catch(e2){return{error:String(e1&&e1.message||e1).slice(0,200)}}}
if(board){const L=Array.isArray(r)?r:(r&&(r.workItems||r.items));if(Array.isArray(L))r={total:L.length,items:L.slice(0,6).map(w=>({id:String(w.id||"").slice(0,12),title:String(w.title||"").slice(0,60),status:w.status,result:String(w.resultSummary||"").slice(0,120)}))}}
// THE BYTE BUDGET. Whatever the target returned, what leaves this program
// serialises to at most MAX bytes (UTF-8), the room the device's 3 KB response
// slot leaves for it. A result that already fits is only trimmed as before
// (six items, 320 characters, depth 4) and is not flagged. One that does not
// fit is projected through ever tighter tiers until it does: arrays keep their
// first N items and say how many there were, objects keep their first K keys,
// long strings are cut, deep branches become a marker. The last resort is a
// string head. A projection forced by the budget carries truncated:true and a
// hint, so the model says the answer was cut instead of inventing the rest.
const MAX=__BUDGET__;
const HINT="cut to fit the device; ask for fewer items or a narrower question";
const u8=s=>{let n=0;for(let i=0;i<s.length;i++){const c=s.charCodeAt(i);if(c<128)n++;else if(c<2048)n+=2;else if(c>=55296&&c<56320){n+=4;i++}else n+=3}return n};
const sz=x=>{try{const s=JSON.stringify(x);return s===undefined?0:u8(s)}catch(e){return 1e9}};
let N,S,K,D,cut;
const T=(x,d)=>{
if(typeof x==="string"){if(x.length>S){cut=true;return x.slice(0,S)+"\u2026"}return x}
if(typeof x==="bigint")return String(x);
if(Array.isArray(x)){if(d>=D){cut=true;return"[\u2026]"}if(x.length>N)cut=true;return x.slice(0,N).map(y=>T(y,d+1))}
if(x&&typeof x==="object"){if(d>=D){cut=true;return"{\u2026}"}const e=Object.entries(x);if(e.length>K)cut=true;const q={};for(const[k,y]of e.slice(0,K)){q[k.length>40?k.slice(0,40):k]=T(y,d+1);if(Array.isArray(y)&&y.length>N)q[k.slice(0,36)+"_total"]=y.length}return q}
return x};
const wrap=(w,c)=>{
if(!c)return w;
if(Array.isArray(w))return{items:w,total:r.length,truncated:true,hint:HINT};
if(w&&typeof w==="object"){w.truncated=true;w.hint=HINT;return w}
if(typeof w==="string")return{text:w,total_chars:r.length,truncated:true,hint:HINT};
return w};
const big=sz(r)>MAX;
const TIERS=[[6,320,24,4],[6,160,16,3],[4,100,12,3],[3,60,8,2],[2,40,6,2],[1,24,4,2]];
for(let i=0;i<TIERS.length;i++){[N,S,K,D]=TIERS[i];cut=false;const w=wrap(T(r,0),cut&&(big||i>0));if(sz(w)<=MAX)return w}
let s;
try{s=JSON.stringify(r)}catch(e){s=String(r)}
if(s===undefined)s="null";
let n=1200;
for(;;){const w={text:s.slice(0,n),total_chars:s.length,truncated:true,hint:HINT};if(n<=16||sz(w)<=MAX)return w;n=n>>1}
