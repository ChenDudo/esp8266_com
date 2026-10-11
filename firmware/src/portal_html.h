#pragma once
/* ============================================================================
 * portal_html.h —— 配网页模板（全部放 Flash 的 PROGMEM 里）
 *
 * 设计原则：**不依赖 JavaScript**。
 *   手机连上热点后，iOS 的"强制门户"网页视图和部分安卓浏览器对 JS 支持很弱，
 *   所以主流程走 表单 POST + <meta refresh> 轮询，纯 HTML 就能跑完整套配网。
 *   没有任何外部 CDN 引用（配网时本来就没外网，引 CDN 必白屏）。
 *
 * 占位符：%TITLE% %REFRESH% %DEVNAME% %APNAME% %MSG% %OPTIONS% %SSID% %IP% %TIMEOUT%
 * ==========================================================================*/

/* 页面骨架（含内嵌样式，约 1.5KB） */
static const char PROV_PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">%REFRESH%
<title>%TITLE%</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:16px;background:#eef1f6;color:#1b1f24;
 font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.card{max-width:440px;margin:0 auto;background:#fff;border-radius:16px;padding:20px;
 box-shadow:0 4px 18px rgba(16,24,40,.08)}
h1{font-size:19px;margin:0 0 6px}
.sub{color:#697386;font-size:13px;margin-bottom:14px;word-break:break-all}
label{display:block;font-size:13px;color:#4a5468;margin:14px 0 5px}
select,input{width:100%;padding:11px;font-size:16px;border:1px solid #d6dbe4;
 border-radius:10px;background:#fff;color:#1b1f24}
button{width:100%;margin-top:18px;padding:13px;font-size:17px;font-weight:600;
 color:#fff;background:#2d7ff9;border:0;border-radius:10px;cursor:pointer}
button:active{background:#1f6ae0}
button.secondary{background:#8a94a6;font-size:15px}
.msg{padding:11px 13px;border-radius:10px;font-size:14px;margin-bottom:12px}
.msg.err{background:#fdecea;color:#b3261e}
.msg.ok{background:#e6f6ec;color:#146c2e}
.msg.info{background:#eaf1fe;color:#1b4db1}
.hint{font-size:12px;color:#7a8699;margin-top:12px;line-height:1.7}
.row{display:flex;gap:8px;align-items:center}
.row select{flex:1}
.row a{font-size:13px;white-space:nowrap;color:#2d7ff9;text-decoration:none}
code{background:#f1f3f7;padding:1px 5px;border-radius:5px}
</style></head><body><div class="card">
)HTML";

static const char PROV_PAGE_TAIL[] PROGMEM = R"HTML(
</div></body></html>
)HTML";

/* 主表单页 */
static const char PROV_FORM_BODY[] PROGMEM = R"HTML(<h1>设备配网</h1>
<div class="sub">设备：%DEVNAME% ｜ 当前热点：%APNAME%</div>
%MSG%
<form method="POST" action="/connect">
<label>选择你的 WiFi（必须是 2.4GHz）</label>
<div class="row"><select name="ssid">%OPTIONS%</select><a href="/?r=1">刷新</a></div>
<label>或手动输入网络名称（隐藏网络 / 列表里没有）</label>
<input name="ssid_manual" placeholder="留空则用上面选中的" autocapitalize="off" autocorrect="off" spellcheck="false">
<label>WiFi 密码</label>
<input name="pass" type="password" placeholder="开放网络请留空" autocapitalize="off" autocorrect="off" spellcheck="false">
<button type="submit">连接</button>
</form>
<div class="hint">· 提交后设备会立刻尝试连接，成功后本热点会在几秒内自动关闭。<br>
· 列表里没有你的路由器，请确认是 2.4GHz 频段，再点“刷新”。<br>
· 超过 %TIMEOUT% 分钟没人提交，设备会自动重启并重试联网。</div>
)HTML";

/* 连接中（靠 meta refresh 自动轮询 /status） */
static const char PROV_CONNECTING_BODY[] PROGMEM = R"HTML(<h1>正在连接…</h1>
<div class="sub">目标网络：%SSID%</div>
<div class="msg info">设备正在连接路由器。请保持手机连在 <b>%APNAME%</b> 上不要切走。</div>
<div class="hint">页面每 3 秒自动刷新，通常 5~20 秒内出结果。</div>
)HTML";

/* 成功 */
static const char PROV_OK_BODY[] PROGMEM = R"HTML(<h1>配网成功</h1>
<div class="msg ok">设备已连上 <b>%SSID%</b><br>设备 IP：<code>%IP%</code></div>
<div class="hint">本热点将在几秒后自动关闭，手机会自动回到原来的 WiFi。<br>
现在可以放下手机，开始使用设备了。</div>
)HTML";

/* 失败 */
static const char PROV_FAIL_BODY[] PROGMEM = R"HTML(<h1>连接失败</h1>
<div class="msg err">%MSG%</div>
<div class="hint">请确认密码是否正确、路由器是否为 2.4GHz 频段、信号是否够强，然后重新提交。</div>
<a href="/" style="display:block;text-align:center;margin-top:6px;color:#2d7ff9">
← 重新选择网络并填写密码</a>
<form method="POST" action="/reprov">
<button type="submit" class="secondary">清除设备里保存的 WiFi 记录</button>
</form>
)HTML";
