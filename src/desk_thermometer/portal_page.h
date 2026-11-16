#pragma once
static const char PORTAL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>温度牌设置</title><style>
*{box-sizing:border-box}body{font:17px/1.6 system-ui,sans-serif;background:#f5f5f0;color:#202921;margin:0;padding:24px 18px}main{max-width:440px;margin:20px auto;background:white;padding:26px;border-radius:20px}h1{font-size:27px;margin:0 0 8px}p{color:#536057}label{display:block;margin-top:16px}input,button,select{font:inherit;width:100%;padding:13px;border-radius:10px;border:1px solid #b5bfb5}input{margin-top:6px}button{margin-top:16px;background:#285a43;color:white;cursor:pointer}.secondary{background:white;color:#285a43}button:disabled{opacity:.55}#status{white-space:pre-wrap;overflow-wrap:anywhere;min-height:54px}small{display:block;color:#657166;margin-top:18px}
</style><main><h1>温度牌设置</h1><p>连接家里或公司的 2.4GHz Wi-Fi，自动校准时间。换地方后可以再次设置。</p>
<button id="scan" class="secondary">重新扫描附近 Wi-Fi</button><label for="networks">选择附近 Wi-Fi<select id="networks"><option value="">正在扫描……</option></select></label><form id="wifi"><label for="ssid">Wi-Fi 名称（也可手动输入）<input id="ssid" name="ssid" autocomplete="off" required maxlength="32"></label><label for="password">Wi-Fi 密码<input id="password" name="password" type="password" autocomplete="off" required minlength="8" maxlength="63"></label><button id="save">连接并保存</button></form>
<p id="status" role="status" aria-live="polite">密码只提交到这块温度牌，不上传到云端。</p>
<button id="phone" class="secondary">使用手机时间</button><button id="done" class="secondary">返回温度牌</button><small>无需联网也可使用手机时间，请先确认手机日期正确。误触进入时，可短按板上的 BOOT 退出。设置热点也会在 5 分钟后自动关闭。</small></main>
<script>
const token='@@TOKEN@@',status=document.querySelector('#status');
let scanning=false;
function busy(value){document.querySelectorAll('button').forEach(b=>b.disabled=value);}
async function send(path,data){
 busy(true);
 status.textContent=path==='/save'?'正在连接和校时，请稍等约30秒……':'正在处理……';
 try{const res=await fetch(path,{method:'POST',body:new URLSearchParams({...data,token})});status.textContent=await res.text();if(path==='/save'&&res.ok)document.querySelector('#password').value='';}
 catch(e){status.textContent='连接中断。请确认手机仍连接温度牌热点，再打开 http://192.168.4.1。';}
 finally{busy(false);}
}
async function scan(){
 if(scanning)return;scanning=true;busy(true);
 const list=document.querySelector('#networks');list.disabled=true;
 status.textContent='正在扫描附近的2.4GHz网络……';
 try{
  let res;
  for(let i=0;i<20;i++){
   res=await fetch('/scan',{method:'POST',body:new URLSearchParams({token,start:i===0?'1':'0'})});
   if(res.status!==202)break;
   await new Promise(r=>setTimeout(r,700));
  }
  if(res.status===202)throw Error('扫描超时，请重试，或手动输入名称。');
  if(!res.ok)throw Error(await res.text());
  const networks=await res.json();
  networks.sort((a,b)=>b.rssi-a.rssi);
  list.replaceChildren(new Option(networks.length?'请选择你的 Wi-Fi':'未发现网络，可手动输入',''));
  for(const n of networks)list.add(new Option(n.ssid+' · '+(n.rssi>=-60?'信号强':n.rssi>=-75?'信号中':'信号弱'),n.ssid));
  status.textContent=networks.length?'点选名称后输入密码即可。隐藏网络请手动输入；仅能扫描2.4GHz网络。':'未发现网络。可以重新扫描，或手动输入隐藏网络名称。';
 }catch(e){status.textContent=e.message||'扫描失败，请重试。';list.replaceChildren(new Option('扫描失败，可手动输入',''));}
 finally{scanning=false;busy(false);list.disabled=false;}
}
document.querySelector('#networks').onchange=e=>{if(e.target.value)document.querySelector('#ssid').value=e.target.value;};
document.querySelector('#scan').onclick=scan;
scan();
document.querySelector('#wifi').onsubmit=e=>{e.preventDefault();send('/save',{ssid:document.querySelector('#ssid').value,password:document.querySelector('#password').value});};
document.querySelector('#phone').onclick=()=>send('/time',{epoch:Math.floor(Date.now()/1000)});
document.querySelector('#done').onclick=()=>send('/done',{});
</script></html>)HTML";
