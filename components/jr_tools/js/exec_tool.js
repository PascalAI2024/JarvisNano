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
const ORD={websearch:"query",weather:"latitude,longitude",wiki:"query",time:"timezone",crypto:"coin,currency",exchange:"base,targets",country:"name",geocode:"query",translate:"text,target,source",hackernews:"type,count",research:"question","stocks.quote":"symbols","reddit.search":"query","memory.search":"query","holidays.list":"country,year"};
const nm=P.join("."),isA=Array.isArray,sl=(x,n)=>String(x||"").slice(0,n),J=JSON.stringify;
let o=jarvis;
for(const k of P.slice(0,-1)){o=o[k];if(o==null)return{error:"unknown tool"}}
const f=o[P[P.length-1]];
if(typeof f!=="function")return{error:"unknown tool"};
const B=P[0]==="coordination",M=nm==="memory.capture";
if(B){const i=A.input||A;if(!i.identity)i.identity={runtime:"pi",agentId:"jarvisnano",hostId:"jarvisnano",sessionId:new Date().toISOString().slice(0,10)};A=i}
if(M){const i=A.input||A;i.type=i.type||"note";i.source=i.source||"jarvisnano";i.title=i.title||sl(i.body,60);A=i}
const nd=P.length>=3||B||M,od=ORD[nm],v=od?od.split(",").map(k=>A[k]):Object.values(A),a1=nd?[A]:v,a2=nd?v:[A];
let r;
try{r=await f.call(o,...a1)}catch(e1){try{r=await f.call(o,...a2)}catch(e2){return{error:sl(e1&&e1.message||e1,200)}}}
// THE BYTE BUDGET. Whatever the target returned, what leaves this program
// serialises to at most G bytes (UTF-8), the room the device's 3 KB response
// slot leaves for it. A result that already fits is only trimmed as before
// (six items, 320 characters, depth 4) and is not flagged. One that does not
// fit is projected through ever tighter tiers until it does: arrays keep their
// first N items and say how many there were, objects keep their first K keys,
// long strings are cut, deep branches become a marker. The last resort is a
// string head. A projection forced by the budget carries truncated:true and a
// hint, so the model says the answer was cut instead of inventing the rest.
const MAX=__BUDGET__;
const HINT="cut to fit the device; ask for fewer items or a narrower question",F={truncated:true,hint:HINT};
const u=s=>{let n=0;for(let i=0;i<s.length;i++){const c=s.charCodeAt(i);n+=c<128?1:c<2048?2:c>=55296&&c<56320?(i++,4):3}return n};
const z=x=>{try{const s=J(x);return s===undefined?0:u(s)}catch(e){return 1e9}};
if(nm==="coordination.portfolio"){const L=isA(r)?r:(r&&(r.projects||r.items));if(isA(L)){r={total:L.length,projects:L.slice(0,8).map(p=>{p=p||{};const c=p.counts||p.summary||p.stats||p;return{id:sl(p.id||p.projectId,24),name:sl(p.name||p.title,40),open:+c.open||0,inProgress:+(c.inProgress||c.in_progress)||0,blocked:+c.blocked||0,completed:+(c.completed||c.done)||0}})};if(z(r)<=MAX)return r}}
else if(B){const L=isA(r)?r:(r&&(r.workItems||r.items));if(isA(L))r={total:L.length,items:L.slice(0,6).map(w=>({id:sl(w.id,12),title:sl(w.title,60),status:w.status,result:sl(w.resultSummary,120)}))}}
let N,S,K,D,cut;
const T=(x,d)=>{
if(typeof x==="string"){if(x.length>S){cut=true;return x.slice(0,S)+"\u2026"}return x}
if(typeof x==="bigint")return String(x);
if(isA(x)){if(d>=D){cut=true;return"[\u2026]"}if(x.length>N)cut=true;return x.slice(0,N).map(y=>T(y,d+1))}
if(x&&typeof x==="object"){if(d>=D){cut=true;return"{\u2026}"}const e=Object.entries(x);if(e.length>K)cut=true;const q={};for(const[k,y]of e.slice(0,K)){q[k.length>40?k.slice(0,40):k]=T(y,d+1);if(isA(y)&&y.length>N)q[k.slice(0,36)+"_total"]=y.length}return q}
return x};
const W=(w,c)=>!c?w:isA(w)?{items:w,total:r.length,...F}:w&&typeof w==="object"?Object.assign(w,F):typeof w==="string"?{text:w,total_chars:r.length,...F}:w;
const big=z(r)>MAX,TIERS=[[6,320,24,4],[6,160,16,3],[4,100,12,3],[3,60,8,2],[2,40,6,2],[1,24,4,2]];
for(let i=0;i<TIERS.length;i++){[N,S,K,D]=TIERS[i];cut=false;const w=W(T(r,0),cut&&(big||i>0));if(z(w)<=MAX)return w}
let s;
try{s=J(r)}catch(e){s=String(r)}
if(s===undefined)s="null";
let n=1200;
for(;;){const w={text:s.slice(0,n),total_chars:s.length,...F};if(n<=16||z(w)<=MAX)return w;n=n>>1}
