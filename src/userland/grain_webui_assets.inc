// The page a pane is shown in, as text: shared by grain_webui.cpp (the server's page) and the WebAssembly build,
// whose build writes kStyle and kPaneFactory out as grain-pane.css and grain-pane.js (grain/wasm/extract-assets.cmake).
// Included inside grain_webui.cpp's anonymous namespace.
//
// kPaneFactory is the whole client: grainPane(app, io) makes the element `app` a live pane. It sends the fields with
// every event, swaps the widgets in, keeps the focused field and its caret. What differs between hosts is `io`:
//   io.send(body)        -> a promise of the frame the action's answer is ({seq, version, html})
//   io.subscribe(fn)     -> pushed frames, each handed to fn; absent: the page asks again every data-refresh seconds
//   io.reload()          -> a newer version of the program is out: start over
//   io.hidden()          -> the page is not being looked at

const char* kStyle = R"CSS(
:root{
  --bg:#090d14;--panel:rgba(18,24,38,0.72);--panel-solid:#121826;--ink:#f1f5f9;--dim:#94a3b8;
  --line:rgba(255,255,255,0.09);--line-strong:rgba(255,255,255,0.18);
  --accent:#3b82f6;--accent-ink:#ffffff;--accent-glow:rgba(59,130,246,0.35);
  --ok:#10b981;--ok-glow:rgba(16,185,129,0.35);--warn:#f59e0b;--bad:#ef4444;
}
@media (prefers-color-scheme:light){
  :root{
    --bg:#f8fafc;--panel:rgba(255,255,255,0.85);--panel-solid:#ffffff;--ink:#0f172a;--dim:#64748b;
    --line:rgba(0,0,0,0.08);--line-strong:rgba(0,0,0,0.16);
    --accent:#2563eb;--accent-ink:#ffffff;--accent-glow:rgba(37,99,235,0.25);
    --ok:#059669;--ok-glow:rgba(5,150,105,0.25);--warn:#d97706;--bad:#dc2626;
  }
}
*{box-sizing:border-box}
@keyframes ui-flash{0%{background-color:rgba(59,130,246,.28)}25%{background-color:rgba(59,130,246,.22)}100%{background-color:rgba(59,130,246,0)}}
.ui-changed{animation:ui-flash 2.6s ease-out;border-radius:4px}
@media (prefers-reduced-motion:reduce){.ui-changed{animation:none;background-color:rgba(59,130,246,.15)}}
body{
  margin:0;background:radial-gradient(ellipse 80% 50% at 50% -20%,rgba(59,130,246,0.12),transparent),var(--bg);
  min-height:100vh;color:var(--ink);font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  -webkit-font-smoothing:antialiased;
}
#app{max-width:var(--w,860px);margin:0 auto;padding:28px 20px}
.w-column{display:flex;flex-direction:column}.w-row{display:flex;flex-direction:row;flex-wrap:wrap;align-items:stretch}
.w-row>.w-field{flex:1 1 180px}
.w-text.dim{color:var(--dim)}.w-text.bold{font-weight:650}
.w-btn{
  font:inherit;font-size:13px;font-weight:600;border:1px solid var(--line);background:var(--panel);
  color:var(--ink);border-radius:10px;padding:9px 18px;cursor:pointer;
  transition:all 0.18s cubic-bezier(0.16,1,0.3,1);backdrop-filter:blur(8px);
}
.w-btn:hover{border-color:var(--line-strong);transform:translateY(-1px);box-shadow:0 4px 12px rgba(0,0,0,0.25)}
.w-btn:active{transform:translateY(0);filter:brightness(0.95)}
.w-btn.primary{
  background:linear-gradient(180deg,#3b82f6 0%,#2563eb 100%);border-color:rgba(255,255,255,0.16);
  color:#fff;box-shadow:0 2px 10px var(--accent-glow),inset 0 1px 0 rgba(255,255,255,0.25);
}
.w-btn.primary:hover{filter:brightness(1.08);box-shadow:0 4px 16px var(--accent-glow),inset 0 1px 0 rgba(255,255,255,0.3)}
.w-field{
  font:inherit;font-size:14px;border:1px solid var(--line);background:rgba(0,0,0,0.25);
  color:var(--ink);border-radius:10px;padding:9px 12px;min-width:0;
  transition:border-color 0.15s ease,box-shadow 0.15s ease;
}
.w-field:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-glow)}
.w-tile{
  background:var(--panel);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);
  border:1px solid var(--line);border-radius:14px;padding:16px 18px;min-width:110px;flex:1 1 0;
  box-shadow:0 4px 20px -2px rgba(0,0,0,0.3),inset 0 1px 0 rgba(255,255,255,0.06);
  transition:all 0.2s cubic-bezier(0.16,1,0.3,1);
}
.w-tile:hover{transform:translateY(-2px);border-color:var(--line-strong);box-shadow:0 8px 24px -2px rgba(0,0,0,0.45),inset 0 1px 0 rgba(255,255,255,0.1)}
.w-tile .tv{font-size:26px;font-weight:750;letter-spacing:-0.03em;font-variant-numeric:tabular-nums}
.w-tile .tl{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);font-weight:650;margin-top:4px}
.w-tile.ok .tv{color:var(--ok)}.w-tile.warn .tv{color:var(--warn)}.w-tile.bad .tv{color:var(--bad)}
.w-table{
  border-collapse:separate;border-spacing:0;width:100%;background:var(--panel);
  backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);border:1px solid var(--line);
  border-radius:14px;overflow:hidden;box-shadow:0 4px 20px -2px rgba(0,0,0,0.25),inset 0 1px 0 rgba(255,255,255,0.05);
}
.w-table th{
  font-size:11px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);
  text-align:left;padding:11px 16px;background:rgba(255,255,255,0.03);border-bottom:1px solid var(--line);
}
.w-table td{padding:10px 16px;border-bottom:1px solid var(--line);font-size:14px;transition:background 0.12s ease}
.w-table tr:last-child td{border-bottom:0}
.w-table tbody tr{animation:row-in 0.25s cubic-bezier(0.16,1,0.3,1) both}
@keyframes row-in{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:translateY(0)}}
.w-table tbody tr:hover td{background:rgba(255,255,255,0.035)}
.w-table .r{text-align:right}.w-table .empty{color:var(--dim);text-align:center;padding:24px}
.error{color:var(--bad);white-space:pre-wrap;font-family:ui-monospace,monospace}
.w-heading{font-size:24px;font-weight:750;letter-spacing:-0.03em}
.w-text.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace}
.w-text.italic{font-style:italic}
.w-card{
  background:var(--panel);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);
  border:1px solid var(--line);border-radius:16px;
  box-shadow:0 4px 24px -2px rgba(0,0,0,0.3),inset 0 1px 0 rgba(255,255,255,0.06);
  transition:all 0.25s cubic-bezier(0.16,1,0.3,1);
}
.w-card:hover{border-color:var(--line-strong);box-shadow:0 8px 32px -4px rgba(0,0,0,0.45),inset 0 1px 0 rgba(255,255,255,0.1)}
.w-divider{border:0;height:1px;background:var(--line);margin:6px 0;width:100%}
.w-btn.bad{
  background:linear-gradient(180deg,#ef4444 0%,#dc2626 100%);color:#fff;
  border-color:rgba(255,255,255,0.16);box-shadow:0 2px 10px rgba(239,68,68,0.35);
}
.w-btn.bad:hover{filter:brightness(1.1);box-shadow:0 4px 16px rgba(239,68,68,0.45)}
.w-btn.ok{
  background:linear-gradient(180deg,#10b981 0%,#059669 100%);color:#fff;
  border-color:rgba(255,255,255,0.16);box-shadow:0 2px 10px var(--ok-glow);
}
.w-btn.ghost{background:transparent;border-color:transparent}
.w-btn.ghost:hover{background:rgba(255,255,255,0.06);border-color:var(--line)}
.w-btn.outline{background:transparent;border-color:var(--line-strong)}
.w-tile .sub{font-size:11px;font-weight:600;letter-spacing:0.02em;margin-top:6px;color:var(--dim)}
.w-tile .sub.ok{color:var(--ok)}.w-tile .sub.warn{color:var(--warn)}.w-tile .sub.bad{color:var(--bad)}
.w-badge{
  display:inline-flex;align-items:center;gap:7px;align-self:flex-start;font-size:11px;font-weight:700;
  letter-spacing:.06em;text-transform:uppercase;padding:4px 12px;border-radius:999px;
  color:var(--c,var(--accent));border:1px solid color-mix(in srgb,var(--c,var(--accent)) 30%,transparent);
  background:color-mix(in srgb,var(--c,var(--accent)) 12%,transparent);
}
.w-badge.ok{--c:var(--ok)}.w-badge.warn{--c:var(--warn)}.w-badge.bad{--c:var(--bad)}
.w-badge.dot::before{
  content:"";width:7px;height:7px;border-radius:50%;background:currentColor;
  box-shadow:0 0 10px currentColor;animation:w-pulse 2s infinite ease-in-out;
}
@keyframes w-pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:0.4;transform:scale(0.8)}}
.w-bar{display:flex;flex-direction:column;gap:6px;min-width:120px;flex:1 1 auto}
.w-bar .bl{display:flex;justify-content:space-between;font-size:12px;color:var(--dim)}.w-bar .bv{font-variant-numeric:tabular-nums;color:var(--ink);font-weight:600}
.w-bar .bt{height:8px;border-radius:999px;background:rgba(255,255,255,0.08);overflow:hidden}
.w-bar .bf{height:100%;border-radius:999px;transition:width .4s ease}
.w-spark,.w-chart{
  position:relative;background:var(--panel);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);
  border:1px solid var(--line);border-radius:14px;padding:8px 12px;min-width:120px;flex:1 1 auto;
  box-shadow:0 4px 20px -2px rgba(0,0,0,0.25),inset 0 1px 0 rgba(255,255,255,0.05);
}
.w-spark svg,.w-chart svg{width:100%;height:100%;display:block}
.w-chart{padding:12px 14px 12px 48px}.w-chart .ax{position:absolute;left:10px;top:10px;bottom:10px;display:flex;flex-direction:column;justify-content:space-between;font-size:10px;color:var(--dim);font-variant-numeric:tabular-nums}
.w-row>.w-column{flex:1 1 0;min-width:0}
.w-bar .bl{gap:12px}.w-bar .bl span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.w-table td .w-badge{font-size:10px;padding:2px 8px}

/* Switches / Toggles */
.w-switch-wrap{display:inline-flex;align-items:center;gap:10px;cursor:pointer;user-select:none}
.w-switch{position:absolute;opacity:0;pointer-events:none}
.w-switch-pill{
  position:relative;width:40px;height:22px;background:rgba(255,255,255,0.12);
  border:1px solid var(--line);border-radius:999px;transition:all 0.22s cubic-bezier(0.16,1,0.3,1);
}
.w-switch-pill::after{
  content:"";position:absolute;top:2px;left:2px;width:16px;height:16px;
  background:#fff;border-radius:50%;box-shadow:0 1px 4px rgba(0,0,0,0.3);
  transition:transform 0.22s cubic-bezier(0.16,1,0.3,1);
}
.w-switch:checked + .w-switch-pill{background:var(--accent);border-color:var(--accent);box-shadow:0 0 10px var(--accent-glow)}
.w-switch:checked + .w-switch-pill::after{transform:translateX(18px)}
.w-switch-text{font-size:14px;font-weight:550;color:var(--ink)}

/* Checkboxes */
.w-check-wrap{display:inline-flex;align-items:center;gap:9px;cursor:pointer;user-select:none}
.w-check{position:absolute;opacity:0;pointer-events:none}
.w-check-box{
  width:18px;height:18px;border:1px solid var(--line-strong);border-radius:5px;
  background:rgba(0,0,0,0.25);transition:all 0.18s ease;display:flex;align-items:center;justify-content:center;
}
.w-check:checked + .w-check-box{
  background:var(--accent);border-color:var(--accent);box-shadow:0 0 8px var(--accent-glow);
}
.w-check:checked + .w-check-box::after{
  content:"";width:5px;height:9px;border:solid #fff;border-width:0 2px 2px 0;
  transform:rotate(45deg);margin-bottom:2px;
}
.w-check-text{font-size:14px;color:var(--ink)}

/* Sliders */
.w-slider-wrap{display:flex;flex-direction:column;gap:6px;min-width:140px;flex:1 1 auto}
.w-slider-head{display:flex;justify-content:space-between;font-size:12px;color:var(--dim)}
.w-slider-num{font-variant-numeric:tabular-nums;color:var(--ink);font-weight:650}
.w-slider{
  -webkit-appearance:none;appearance:none;width:100%;height:6px;border-radius:999px;
  background:rgba(255,255,255,0.12);outline:none;transition:background 0.2s ease;cursor:pointer;
}
.w-slider::-webkit-slider-thumb{
  -webkit-appearance:none;appearance:none;width:18px;height:18px;border-radius:50%;
  background:var(--accent);border:2px solid #fff;box-shadow:0 2px 8px var(--accent-glow);cursor:pointer;
  transition:transform 0.15s ease,box-shadow 0.15s ease;
}
.w-slider::-webkit-slider-thumb:hover{transform:scale(1.15);box-shadow:0 4px 12px var(--accent-glow)}

/* Select Menus */
.w-select-wrap{display:flex;flex-direction:column;gap:5px;min-width:120px}
.w-select-label{font-size:11px;font-weight:650;letter-spacing:0.06em;text-transform:uppercase;color:var(--dim)}
.w-select{
  font:inherit;font-size:14px;border:1px solid var(--line);background:var(--panel-solid);
  color:var(--ink);border-radius:10px;padding:9px 32px 9px 12px;min-width:0;cursor:pointer;
  appearance:none;-webkit-appearance:none;
  background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 24 24' fill='none' stroke='%2394a3b8' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='m6 9 6 6 6-6'/%3E%3C/svg%3E");
  background-repeat:no-repeat;background-position:right 12px center;
  transition:border-color 0.15s ease,box-shadow 0.15s ease;
}
.w-select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-glow)}

/* Field Groups */
.w-field-group{display:flex;flex-direction:column;gap:5px;flex:1 1 180px}
.w-field-label{font-size:11px;font-weight:650;letter-spacing:0.06em;text-transform:uppercase;color:var(--dim)}

/* Built-in Preset Themes */
[data-theme="cyber"], .theme-cyber{
  --bg:#050814;--panel:rgba(10,16,32,0.78);--panel-solid:#0c1427;--ink:#f0fdf4;--dim:#7dd3fc;
  --line:rgba(6,182,212,0.18);--line-strong:rgba(6,182,212,0.38);
  --accent:#06b6d4;--accent-ink:#020617;--accent-glow:rgba(6,182,212,0.45);
  --ok:#10b981;--ok-glow:rgba(16,185,129,0.45);--warn:#f59e0b;--bad:#f43f5e;
}
body:has([data-theme="cyber"]),body.theme-cyber{
  background:radial-gradient(ellipse 90% 60% at 50% -20%,rgba(6,182,212,0.18),transparent),
             radial-gradient(ellipse 60% 40% at 85% 85%,rgba(236,72,153,0.12),transparent),#050814 !important;
}

[data-theme="nord"], .theme-nord{
  --bg:#242933;--panel:rgba(46,52,64,0.82);--panel-solid:#2e3440;--ink:#eceff4;--dim:#9aa8be;
  --line:rgba(236,239,244,0.09);--line-strong:rgba(136,192,208,0.28);
  --accent:#88c0d0;--accent-ink:#242933;--accent-glow:rgba(136,192,208,0.35);
  --ok:#a3be8c;--ok-glow:rgba(163,190,140,0.35);--warn:#ebcb8b;--bad:#bf616a;
}
body:has([data-theme="nord"]),body.theme-nord{
  background:radial-gradient(ellipse 80% 50% at 50% -20%,rgba(136,192,208,0.14),transparent),#242933 !important;
}

[data-theme="matrix"], .theme-matrix{
  --bg:#030704;--panel:rgba(6,20,10,0.8);--panel-solid:#091a0e;--ink:#86efac;--dim:#4ade80;
  --line:rgba(34,197,94,0.2);--line-strong:rgba(34,197,94,0.45);
  --accent:#22c55e;--accent-ink:#030704;--accent-glow:rgba(34,197,94,0.45);
  --ok:#4ade80;--ok-glow:rgba(74,222,128,0.45);--warn:#eab308;--bad:#ef4444;
}
body:has([data-theme="matrix"]),body.theme-matrix{
  background:radial-gradient(ellipse 80% 50% at 50% -20%,rgba(34,197,94,0.16),transparent),#030704 !important;
}

[data-theme="sunset"], .theme-sunset{
  --bg:#0f0a1c;--panel:rgba(28,18,48,0.78);--panel-solid:#1c1230;--ink:#fdf2f8;--dim:#f472b6;
  --line:rgba(244,114,182,0.15);--line-strong:rgba(249,115,22,0.35);
  --accent:#f97316;--accent-ink:#fff;--accent-glow:rgba(249,115,22,0.45);
  --ok:#10b981;--ok-glow:rgba(16,185,129,0.35);--warn:#fbbf24;--bad:#f43f5e;
}
body:has([data-theme="sunset"]),body.theme-sunset{
  background:radial-gradient(ellipse 90% 60% at 50% -20%,rgba(249,115,22,0.18),transparent),
             radial-gradient(ellipse 70% 50% at 80% 80%,rgba(236,72,153,0.15),transparent),#0f0a1c !important;
}

[data-theme="glass"], .theme-glass{
  --bg:#0b0f19;--panel:rgba(255,255,255,0.04);--panel-solid:rgba(255,255,255,0.08);--ink:#ffffff;--dim:#94a3b8;
  --line:rgba(255,255,255,0.12);--line-strong:rgba(255,255,255,0.28);
  --accent:#60a5fa;--accent-ink:#ffffff;--accent-glow:rgba(96,165,250,0.4);
  --ok:#34d399;--ok-glow:rgba(52,211,153,0.4);--warn:#fbbf24;--bad:#f87171;
}
body:has([data-theme="glass"]),body.theme-glass{
  background:radial-gradient(ellipse 90% 70% at 30% 0%,rgba(99,102,241,0.15),transparent),
             radial-gradient(ellipse 80% 60% at 70% 100%,rgba(236,72,153,0.12),transparent),#0b0f19 !important;
}

/* Built-in Utility Classes */
.glow{box-shadow:0 0 24px var(--accent-glow) !important}
.glow-ok{box-shadow:0 0 24px var(--ok-glow) !important}
.glow-text{text-shadow:0 0 16px var(--accent-glow)}
.gradient-text{
  background:linear-gradient(135deg,var(--accent) 0%,#c084fc 100%);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;font-weight:800;
}
.glass{
  background:rgba(255,255,255,0.04) !important;
  backdrop-filter:blur(20px) saturate(180%) !important;-webkit-backdrop-filter:blur(20px) saturate(180%) !important;
  border:1px solid var(--line-strong) !important;
}
.sunken{
  background:rgba(0,0,0,0.4) !important;
  box-shadow:inset 0 2px 8px rgba(0,0,0,0.5) !important;border:1px solid var(--line) !important;
}
.flat{box-shadow:none !important;border:1px solid var(--line) !important}
.hover-lift{
  transition:transform 0.22s cubic-bezier(0.16,1,0.3,1),box-shadow 0.22s,border-color 0.22s !important;
}
.hover-lift:hover{
  transform:translateY(-3px) !important;box-shadow:0 10px 28px -4px rgba(0,0,0,0.5) !important;
  border-color:var(--line-strong) !important;
}
.hover-glow:hover{
  border-color:var(--accent) !important;box-shadow:0 0 20px var(--accent-glow) !important;
}
.caps{font-size:11px;font-weight:700;letter-spacing:0.08em;text-transform:uppercase;color:var(--dim)}
.truncate{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.shimmer{position:relative;overflow:hidden}
.shimmer::after{
  content:"";position:absolute;top:0;right:0;bottom:0;left:0;
  transform:translateX(-100%);background:linear-gradient(90deg,transparent,rgba(255,255,255,0.08),transparent);
  animation:w-shimmer 2.5s infinite;
}
@keyframes w-shimmer{100%{transform:translateX(100%)}}

/* Aegis DatePicker - Precision Ergonomic & Non-Material Date & Range Picker */
.w-datepicker-wrap{display:flex;flex-direction:column;gap:5px;position:relative;min-width:200px;user-select:none}
.w-dp-label{font-size:11px;font-weight:650;letter-spacing:0.06em;text-transform:uppercase;color:var(--dim)}
.w-dp-box{
  display:flex;align-items:center;gap:10px;padding:9px 12px;background:var(--panel);
  border:1px solid var(--line);border-radius:10px;cursor:pointer;
  transition:all 0.18s cubic-bezier(0.16,1,0.3,1);backdrop-filter:blur(8px);
}
.w-dp-box:hover{border-color:var(--line-strong);box-shadow:0 4px 12px rgba(0,0,0,0.25)}
.w-dp-box:focus,.w-dp-box.open{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-glow)}
.w-dp-icon{width:16px;height:16px;color:var(--accent);flex:none;opacity:0.9}
.w-dp-display{flex:1 1 auto;font-size:13px;font-weight:550;color:var(--ink);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.w-dp-display.empty{color:var(--dim);font-weight:400}
.w-dp-badge{font-size:10px;font-weight:700;letter-spacing:0.04em;padding:2px 7px;border-radius:999px;background:var(--accent-glow);color:var(--accent);border:1px solid var(--accent);display:none}
.w-dp-badge.show{display:inline-block}
.w-dp-clear{background:transparent;border:0;color:var(--dim);cursor:pointer;font-size:16px;line-height:1;padding:0 2px;opacity:0;transition:opacity 0.15s ease}
.w-dp-box:hover .w-dp-clear.show{opacity:0.8}
.w-dp-clear:hover{color:var(--bad);opacity:1 !important}
.w-dp-chevron{width:14px;height:14px;color:var(--dim);transition:transform 0.2s cubic-bezier(0.16,1,0.3,1);flex:none}
.w-dp-box.open .w-dp-chevron{transform:rotate(180deg);color:var(--accent)}

/* Popover Floating Panel */
.w-dp-popover{
  position:absolute;top:calc(100% + 8px);left:0;z-index:999;
  background:var(--panel-solid, #0d1322);
  border:1px solid var(--line-strong);border-radius:16px;
  box-shadow:0 20px 48px -4px rgba(0,0,0,0.65), 0 0 0 1px rgba(255,255,255,0.06), inset 0 1px 0 rgba(255,255,255,0.1);
  backdrop-filter:blur(24px) saturate(180%);-webkit-backdrop-filter:blur(24px) saturate(180%);
  display:none;animation:w-dp-pop 0.2s cubic-bezier(0.16,1,0.3,1);overflow:hidden;
}
.w-dp-popover.open{display:flex}
@keyframes w-dp-pop{from{opacity:0;transform:translateY(-8px) scale(0.97)}to{opacity:1;transform:translateY(0) scale(1)}}

/* Sidebar Presets */
.w-dp-presets{display:flex;flex-direction:column;gap:3px;padding:12px;border-right:1px solid var(--line);min-width:130px;background:rgba(0,0,0,0.18)}
.w-dp-preset-btn{
  background:transparent;border:0;color:var(--dim);font:inherit;font-size:12px;font-weight:600;
  padding:7px 10px;border-radius:8px;text-align:left;cursor:pointer;transition:all 0.14s ease;
}
.w-dp-preset-btn:hover{background:rgba(255,255,255,0.06);color:var(--ink);transform:translateX(2px)}
.w-dp-preset-btn.active{background:var(--accent);color:var(--accent-ink,#fff);box-shadow:0 2px 8px var(--accent-glow)}

/* Calendar Body */
.w-dp-main{display:flex;flex-direction:column}
.w-dp-cals{display:flex;gap:16px;padding:14px}
.w-dp-cal{width:238px;display:flex;flex-direction:column;gap:10px}
.w-dp-cal-head{display:flex;align-items:center;justify-content:space-between}
.w-dp-nav-btn{
  background:transparent;border:1px solid transparent;color:var(--dim);width:26px;height:26px;border-radius:6px;
  display:flex;align-items:center;justify-content:center;cursor:pointer;font-size:14px;font-weight:700;
  transition:all 0.15s ease;
}
.w-dp-nav-btn:hover{background:rgba(255,255,255,0.07);border-color:var(--line);color:var(--ink)}
.w-dp-title-btn{
  background:transparent;border:0;color:var(--ink);font:inherit;font-size:13px;font-weight:700;
  padding:4px 8px;border-radius:6px;cursor:pointer;letter-spacing:-0.01em;transition:all 0.15s ease;
}
.w-dp-title-btn:hover{background:rgba(255,255,255,0.08);color:var(--accent)}

/* Weekdays */
.w-dp-weekdays{display:grid;grid-template-columns:repeat(7,1fr);text-align:center;font-size:11px;font-weight:700;color:var(--dim);letter-spacing:0.04em}
.w-dp-weekdays span{padding:4px 0}

/* Days Grid */
.w-dp-days{display:grid;grid-template-columns:repeat(7,1fr);gap:2px 0}
.w-dp-day-wrap{position:relative;padding:1px 0}
.w-dp-day{
  width:32px;height:32px;margin:0 auto;display:flex;align-items:center;justify-content:center;
  font-size:12px;font-weight:600;color:var(--ink);background:transparent;border:0;border-radius:8px;
  cursor:pointer;transition:all 0.12s ease;font-variant-numeric:tabular-nums;position:relative;z-index:2;
}
.w-dp-day:hover{background:rgba(255,255,255,0.12);transform:scale(1.08)}
.w-dp-day.muted{color:var(--dim);opacity:0.35}
.w-dp-day.today::after{
  content:"";position:absolute;bottom:3px;width:4px;height:4px;border-radius:50%;background:var(--accent);
  box-shadow:0 0 6px var(--accent);
}
.w-dp-day.selected{
  background:var(--accent) !important;color:var(--accent-ink,#fff) !important;
  box-shadow:0 2px 10px var(--accent-glow);font-weight:750;
}
.w-dp-day-wrap.in-range{background:rgba(59,130,246,0.15)}
.w-dp-day-wrap.range-start{border-top-left-radius:8px;border-bottom-left-radius:8px;background:linear-gradient(90deg,transparent 0%,rgba(59,130,246,0.15) 50%)}
.w-dp-day-wrap.range-end{border-top-right-radius:8px;border-bottom-right-radius:8px;background:linear-gradient(90deg,rgba(59,130,246,0.15) 50%,transparent 100%)}

/* Fast Month/Year Picker Grid Overlay */
.w-dp-fast-grid{
  position:absolute;top:0;left:0;right:0;bottom:0;background:var(--panel-solid,#0d1322);
  display:grid;grid-template-columns:repeat(3,1fr);gap:8px;padding:16px;z-index:10;
  border-radius:15px;align-content:center;
}
.w-dp-fast-btn{
  background:rgba(255,255,255,0.04);border:1px solid var(--line);border-radius:10px;
  color:var(--ink);font:inherit;font-size:12px;font-weight:650;padding:10px 0;cursor:pointer;
  transition:all 0.15s ease;
}
.w-dp-fast-btn:hover{border-color:var(--accent);background:var(--accent-glow);color:var(--accent)}
.w-dp-fast-btn.active{background:var(--accent);color:#fff;border-color:var(--accent)}

/* Popover Footer */
.w-dp-footer{
  display:flex;align-items:center;justify-content:space-between;gap:10px;
  padding:10px 14px;border-top:1px solid var(--line);background:rgba(0,0,0,0.14);
}
.w-dp-input-manual{
  font:inherit;font-size:12px;background:rgba(0,0,0,0.3);border:1px solid var(--line);
  color:var(--ink);border-radius:6px;padding:5px 8px;width:190px;
}
.w-dp-input-manual:focus{outline:none;border-color:var(--accent)}
.w-dp-actions{display:flex;gap:6px}
.w-dp-btn{
  font:inherit;font-size:11px;font-weight:650;padding:5px 12px;border-radius:6px;cursor:pointer;
  border:1px solid var(--line);background:transparent;color:var(--dim);transition:all 0.14s ease;
}
.w-dp-btn:hover{color:var(--ink);border-color:var(--line-strong)}
.w-dp-btn.primary{background:var(--accent);color:var(--accent-ink,#fff);border-color:var(--accent);box-shadow:0 2px 8px var(--accent-glow)}
.w-dp-btn.primary:hover{filter:brightness(1.1)}

/* DatePicker Mobile Responsiveness */
@media (max-width: 640px) {
  .w-dp-popover {
    position: fixed !important; top: auto !important; bottom: 0 !important; left: 0 !important; right: 0 !important;
    width: 100vw !important; max-width: 100vw !important; border-radius: 20px 20px 0 0 !important;
    flex-direction: column !important; max-height: 85vh !important; overflow-y: auto !important;
    box-shadow: 0 -12px 40px rgba(0,0,0,0.85) !important; z-index: 999999 !important;
    animation: w-dp-slide-up 0.22s cubic-bezier(0.16,1,0.3,1) !important;
  }
  @keyframes w-dp-slide-up { from { transform: translateY(100%); } to { transform: translateY(0); } }
  .w-dp-presets {
    flex-direction: row !important; min-width: unset !important; border-right: none !important;
    border-bottom: 1px solid var(--line) !important; overflow-x: auto !important; white-space: nowrap !important;
    padding: 10px 12px !important; -webkit-overflow-scrolling: touch !important;
  }
  .w-dp-preset-btn { padding: 6px 12px !important; flex: none !important; }
  .w-dp-cals {
    flex-direction: column !important; align-items: center !important; gap: 14px !important; padding: 12px !important;
  }
  .w-dp-cal { width: 100% !important; max-width: 310px !important; }
  .w-dp-footer {
    flex-direction: column !important; gap: 10px !important; padding: 12px !important;
  }
  .w-dp-input-manual { width: 100% !important; }
  .w-dp-actions { width: 100% !important; justify-content: flex-end !important; }
}
)CSS";

// Sends the fields with every event, swaps the widgets in, keeps the focused field and its caret.
// The page's own URL, query and all, for its events and its stream: behind a proxy that picks the app by a query
// parameter (?route=...&key=...), a request to the bare path would go somewhere else.
const char* kPaneFactory = R"JS(
function grainPane(app,io){
  var doc=app.ownerDocument,seq=app.dataset.seq,busy=false,again=null;
  function fields(){
    var p=new URLSearchParams();
    app.querySelectorAll('input[data-id], select[data-id]').forEach(function(i){
      if(i.type==='checkbox'){
        p.append('f.'+i.dataset.id, i.checked ? '1' : '0');
      } else {
        p.append('f.'+i.dataset.id, i.value);
      }
    });
    return p;
  }
  // A pushed update (not the answer to this viewer's own action) marks the text it changes, briefly: what moved.
  function flash(el){
    if(!el||!el.classList)return;
    el.classList.remove('ui-changed');void el.offsetWidth;el.classList.add('ui-changed');
    clearTimeout(el._uiFlash);el._uiFlash=setTimeout(function(){el.classList.remove('ui-changed')},2700);
  }
  function morph(target,source,fromAction){
    if(target.nodeType!==source.nodeType||target.nodeName!==source.nodeName){
      target.replaceWith(source.cloneNode(true));return;
    }
    if(target.nodeType===Node.TEXT_NODE){
      if(target.textContent!==source.textContent){
        target.textContent=source.textContent;
        if(!fromAction&&target.textContent.trim()!=='')flash(target.parentElement);
      }
      return;
    }
    if(target.nodeType!==Node.ELEMENT_NODE)return;
    // data-ui-keep: the page's own (a clock its script keeps): an update leaves it as it is
    if(target.hasAttribute('data-ui-keep'))return;
    var sa=source.attributes,ta=target.attributes;
    for(var i=0;i<sa.length;i++){
      var a=sa[i];if(target.getAttribute(a.name)!==a.value)target.setAttribute(a.name,a.value);
    }
    for(var j=ta.length-1;j>=0;j--){
      var n=ta[j].name;if(!source.hasAttribute(n))target.removeAttribute(n);
    }
    if(target.nodeName==='INPUT'||target.nodeName==='SELECT'){
      if(fromAction){
        if(target.type==='checkbox'){
          target.checked=source.checked;
        } else if(target.type==='text' && source.value===''){
          target.value='';
        } else if(target.nodeName==='SELECT'||target.type==='range'){
          target.value=source.value;
        }
      }
      return;
    }
    var sc=Array.from(source.childNodes),tc=Array.from(target.childNodes);
    // children with data-key (a table's rows, by who they are): matched by key, so a row that moved is moved, not
    // rewritten in place - and only what changed in it is marked
    var keyed=sc.length>0&&sc.every(function(n){return n.nodeType===Node.ELEMENT_NODE&&n.hasAttribute('data-key')});
    if(keyed){
      var have={};
      tc.forEach(function(n){if(n.nodeType===Node.ELEMENT_NODE&&n.hasAttribute('data-key'))have[n.getAttribute('data-key')]=n;});
      sc.forEach(function(src,i){
        var k=src.getAttribute('data-key'),t=have[k];
        if(t){delete have[k];morph(t,src,fromAction);}
        else{t=src.cloneNode(true);}
        if(target.childNodes[i]!==t)target.insertBefore(t,target.childNodes[i]||null);
      });
      Array.from(target.childNodes).slice(sc.length).forEach(function(n){target.removeChild(n);});
      return;
    }
    var max=Math.max(sc.length,tc.length);
    for(var k=0;k<max;k++){
      if(!tc[k]&&sc[k])target.appendChild(sc[k].cloneNode(true));
      else if(tc[k]&&!sc[k])target.removeChild(tc[k]);
      else if(tc[k]&&sc[k])morph(tc[k],sc[k],fromAction);
    }
  }
  // a frame from another version of the program than this page's (a deploy): load the page again
  function stale(d){
    if(d.reload||(d.version!==undefined&&app.dataset.version&&d.version!==app.dataset.version)){io.reload();return true}
    return false;
  }
  function apply(d,fromAction){
    if(stale(d))return;
    if(d.html===undefined)return;
    seq=String(d.seq);
    var wrap=doc.createElement('div');
    wrap.innerHTML=d.html;
    // the pane's own element (its id, width and theme) is not part of a frame: diffing against a bare <div> stripped them
    Array.from(app.attributes).forEach(function(a){wrap.setAttribute(a.name,a.value)});
    morph(app,wrap,fromAction);
    var f=app.querySelector('input[data-focus]');
    if(f&&(!doc.activeElement||doc.activeElement===doc.body))f.focus();
    initDatepickers();
  }
  function send(action){
    if(busy){again=action;return}
    busy=true;var p=fields();p.append('action',action||'');
    io.send(p.toString()).then(function(d){
        if(d.html!==undefined){apply(d,true)}   // always: the event stream may have shown this seq already, but only an action's answer clears a field
        else if(d.error){app.insertAdjacentHTML('afterbegin','<div class="error"></div>');app.firstChild.textContent=d.error}
      }).catch(function(){}).then(function(){busy=false;if(again!==null){var x=again;again=null;send(x)}});
  }
  // any element with an action (a sortable <th>, a row), but not a field: its action is sent on change
  app.addEventListener('click',function(e){var b=e.target.closest('[data-act]');if(b&&!/^(INPUT|SELECT|TEXTAREA)$/.test(b.tagName))send(b.dataset.act)});
  app.addEventListener('change',function(e){
    var b=e.target.closest('input[data-act], select[data-act]');
    if(b)send(b.dataset.act);
  });
  app.addEventListener('input',function(e){
    if(e.target.type==='range'){
      var p=e.target.closest('.w-slider-wrap');
      if(p){var n=p.querySelector('.w-slider-num');if(n)n.textContent=e.target.value;}
    }
  });
  app.addEventListener('keydown',function(e){if(e.key==='Enter'&&e.target.dataset&&e.target.dataset.enter){e.preventDefault();send(e.target.dataset.enter)}});
  var f=app.querySelector('input[data-focus]');if(f)f.focus();
  if(io.subscribe){
    io.subscribe(function(d){
      try{
        if(stale(d))return;
        if(String(d.seq)!==seq)apply(d,false);
      }catch(x){}
    });
  }else{
    var r=parseInt(app.dataset.refresh||'0',10);
    if(r>0)setInterval(function(){if(!io.hidden())send('')},r*1000);
  }

  /* ---- Aegis DatePicker Runtime ---- */
  var DP_MONTHS=['January','February','March','April','May','June','July','August','September','October','November','December'];
  var DP_MONTHS_SHORT=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
  var DP_DAYS_SHORT=['Su','Mo','Tu','We','Th','Fr','Sa'];

  function dpPad(n){return (n<10?'0':'')+n;}
  function dpIso(d){return d.getFullYear()+'-'+dpPad(d.getMonth()+1)+'-'+dpPad(d.getDate());}
  function dpParse(s){
    if(!s)return null;
    var p=String(s).trim().split('-');
    if(p.length!==3)return null;
    var y=parseInt(p[0],10),m=parseInt(p[1],10)-1,d=parseInt(p[2],10);
    if(isNaN(y)||isNaN(m)||isNaN(d))return null;
    var dt=new Date(y,m,d);
    return isNaN(dt.getTime())?null:dt;
  }
  function dpFmtDate(d){return DP_MONTHS_SHORT[d.getMonth()]+' '+d.getDate()+', '+d.getFullYear();}
  function dpFmtDisplay(val,mode){
    if(!val)return '';
    if(mode==='range'){
      var parts=val.indexOf('..')!==-1?val.split('..'):val.split(',');
      var d1=dpParse(parts[0]),d2=parts[1]?dpParse(parts[1]):null;
      if(!d1)return val;
      if(!d2||dpIso(d1)===dpIso(d2))return dpFmtDate(d1);
      return dpFmtDate(d1)+' \u2013 '+dpFmtDate(d2);
    }
    var d=dpParse(val);
    return d?dpFmtDate(d):val;
  }
  function dpDaysBetween(d1,d2){
    return Math.round(Math.abs(d2.getTime()-d1.getTime())/86400000)+1;
  }

  function setupDatePicker(wrap){
    var inp=wrap.querySelector('.w-dp-val');
    var box=wrap.querySelector('.w-dp-box');
    var disp=wrap.querySelector('.w-dp-display');
    var badge=wrap.querySelector('.w-dp-badge');
    var clearBtn=wrap.querySelector('.w-dp-clear');
    var mode=wrap.dataset.mode||'single';
    var minDate=dpParse(wrap.dataset.min);
    var maxDate=dpParse(wrap.dataset.max);
    var showPresets=wrap.dataset.presets==='1';

    if(wrap._dpController){
      wrap._dpController.sync();
      return;
    }

    var startD=null,endD=null,hoverD=null,selecting=false;
    var now=new Date();
    var viewY=now.getFullYear(),viewM=now.getMonth();

    function syncFromVal(){
      var curVal=inp?inp.value:'';
      if(curVal){
        if(mode==='range'){
          var parts=curVal.indexOf('..')!==-1?curVal.split('..'):curVal.split(',');
          startD=dpParse(parts[0]);
          endD=parts[1]?dpParse(parts[1]):startD;
          if(startD){viewY=startD.getFullYear();viewM=startD.getMonth();}
          if(startD&&endD&&badge){
            var days=dpDaysBetween(startD,endD);
            badge.textContent=days+(days===1?' day':' days');
            badge.classList.add('show');
          }else if(badge){
            badge.classList.remove('show');
          }
        }else{
          startD=dpParse(curVal);
          endD=null;
          if(startD){viewY=startD.getFullYear();viewM=startD.getMonth();}
        }
        disp.textContent=dpFmtDisplay(curVal,mode);
        disp.classList.remove('empty');
        if(clearBtn)clearBtn.classList.add('show');
      }else{
        startD=null;endD=null;
        disp.textContent=mode==='range'?'Select date range...':'Select date...';
        disp.classList.add('empty');
        if(badge)badge.classList.remove('show');
        if(clearBtn)clearBtn.classList.remove('show');
      }
    }
    syncFromVal();

    var pop=document.createElement('div');
    pop.className='w-dp-popover';
    wrap.appendChild(pop);

    function buildPopover(){
      pop.innerHTML='';

      if(showPresets){
        var presetsDiv=document.createElement('div');
        presetsDiv.className='w-dp-presets';
        var presets=[
          {label:'Today',fn:function(){var n=new Date();return [n,n];}},
          {label:'Yesterday',fn:function(){var y=new Date();y.setDate(y.getDate()-1);return [y,y];}},
          {label:'Last 7 Days',fn:function(){var n=new Date(),s=new Date();s.setDate(s.getDate()-6);return [s,n];}},
          {label:'Last 30 Days',fn:function(){var n=new Date(),s=new Date();s.setDate(s.getDate()-29);return [s,n];}},
          {label:'This Month',fn:function(){var n=new Date(),s=new Date(n.getFullYear(),n.getMonth(),1);return [s,n];}},
          {label:'Last Month',fn:function(){var n=new Date(),s=new Date(n.getFullYear(),n.getMonth()-1,1),e=new Date(n.getFullYear(),n.getMonth(),0);return [s,e];}},
          {label:'Year to Date',fn:function(){var n=new Date(),s=new Date(n.getFullYear(),0,1);return [s,n];}}
        ];
        presets.forEach(function(p){
          var btn=document.createElement('button');
          btn.type='button';
          btn.className='w-dp-preset-btn';
          btn.textContent=p.label;
          if(startD&&endD){
            var range=p.fn();
            if(dpIso(startD)===dpIso(range[0])&&dpIso(endD)===dpIso(range[1]))btn.classList.add('active');
          }
          btn.addEventListener('click',function(e){
            e.stopPropagation();
            var range=p.fn();
            applySelection(range[0],range[1]);
          });
          presetsDiv.appendChild(btn);
        });
        pop.appendChild(presetsDiv);
      }

      var main=document.createElement('div');
      main.className='w-dp-main';
      pop.appendChild(main);

      var calsWrap=document.createElement('div');
      calsWrap.className='w-dp-cals';
      main.appendChild(calsWrap);

      var calCount=(mode==='range'&&window.innerWidth>640)?2:1;
      for(var c=0;c<calCount;c++){
        var cm=(viewM+c)%12;
        var cy=viewY+Math.floor((viewM+c)/12);
        calsWrap.appendChild(renderMonthCal(cy,cm,c===0,c===calCount-1));
      }

      var footer=document.createElement('div');
      footer.className='w-dp-footer';
      var manInp=document.createElement('input');
      manInp.type='text';
      manInp.className='w-dp-input-manual';
      manInp.placeholder=mode==='range'?'YYYY-MM-DD..YYYY-MM-DD':'YYYY-MM-DD';
      manInp.value=inp?inp.value:'';
      manInp.addEventListener('keydown',function(e){
        if(e.key==='Enter'){
          e.preventDefault();
          var v=manInp.value.trim();
          if(mode==='range'){
            var parts=v.indexOf('..')!==-1?v.split('..'):v.split(',');
            var d1=dpParse(parts[0]),d2=parts[1]?dpParse(parts[1]):d1;
            if(d1&&d2)applySelection(d1,d2);
          }else{
            var d=dpParse(v);
            if(d)applySelection(d,null);
          }
        }
      });
      footer.appendChild(manInp);

      var actions=document.createElement('div');
      actions.className='w-dp-actions';
      var cancelBtn=document.createElement('button');
      cancelBtn.type='button';
      cancelBtn.className='w-dp-btn';
      cancelBtn.textContent='Cancel';
      cancelBtn.addEventListener('click',function(e){e.stopPropagation();close();});
      actions.appendChild(cancelBtn);

      var applyBtn=document.createElement('button');
      applyBtn.type='button';
      applyBtn.className='w-dp-btn primary';
      applyBtn.textContent='Apply';
      applyBtn.addEventListener('click',function(e){
        e.stopPropagation();
        if(startD)applySelection(startD,endD||startD);
        else close();
      });
      actions.appendChild(applyBtn);
      footer.appendChild(actions);
      main.appendChild(footer);
    }

    function renderMonthCal(year,month,isFirst,isLast){
      var cal=document.createElement('div');
      cal.className='w-dp-cal';

      var head=document.createElement('div');
      head.className='w-dp-cal-head';

      var leftBtns=document.createElement('div');
      leftBtns.style.display='flex';leftBtns.style.gap='2px';
      if(isFirst){
        var prevY=document.createElement('button');
        prevY.type='button';prevY.className='w-dp-nav-btn';prevY.innerHTML='&laquo;';
        prevY.title='Previous Year';
        prevY.addEventListener('click',function(e){e.stopPropagation();viewY--;buildPopover();});
        leftBtns.appendChild(prevY);

        var prevM=document.createElement('button');
        prevM.type='button';prevM.className='w-dp-nav-btn';prevM.innerHTML='&lsaquo;';
        prevM.title='Previous Month';
        prevM.addEventListener('click',function(e){
          e.stopPropagation();
          viewM--;
          if(viewM<0){viewM=11;viewY--;}
          buildPopover();
        });
        leftBtns.appendChild(prevM);
      }
      head.appendChild(leftBtns);

      var titleBtn=document.createElement('button');
      titleBtn.type='button';
      titleBtn.className='w-dp-title-btn';
      titleBtn.textContent=DP_MONTHS[month]+' '+year;
      titleBtn.addEventListener('click',function(e){
        e.stopPropagation();
        openFastGrid(cal,year,month);
      });
      head.appendChild(titleBtn);

      var rightBtns=document.createElement('div');
      rightBtns.style.display='flex';rightBtns.style.gap='2px';
      if(isLast){
        var nextM=document.createElement('button');
        nextM.type='button';nextM.className='w-dp-nav-btn';nextM.innerHTML='&rsaquo;';
        nextM.title='Next Month';
        nextM.addEventListener('click',function(e){
          e.stopPropagation();
          viewM++;
          if(viewM>11){viewM=0;viewY++;}
          buildPopover();
        });
        rightBtns.appendChild(nextM);

        var nextY=document.createElement('button');
        nextY.type='button';nextY.className='w-dp-nav-btn';nextY.innerHTML='&raquo;';
        nextY.title='Next Year';
        nextY.addEventListener('click',function(e){e.stopPropagation();viewY++;buildPopover();});
        rightBtns.appendChild(nextY);
      }
      head.appendChild(rightBtns);
      cal.appendChild(head);

      var weekdays=document.createElement('div');
      weekdays.className='w-dp-weekdays';
      DP_DAYS_SHORT.forEach(function(day){
        var s=document.createElement('span');
        s.textContent=day;
        weekdays.appendChild(s);
      });
      cal.appendChild(weekdays);

      var daysGrid=document.createElement('div');
      daysGrid.className='w-dp-days';

      var firstDayIndex=new Date(year,month,1).getDay();
      var daysInMonth=new Date(year,month+1,0).getDate();
      var daysInPrevMonth=new Date(year,month,0).getDate();

      for(var p=firstDayIndex-1;p>=0;p--){
        var dNum=daysInPrevMonth-p;
        daysGrid.appendChild(makeDayCell(new Date(year,month-1,dNum),true));
      }
      for(var d=1;d<=daysInMonth;d++){
        daysGrid.appendChild(makeDayCell(new Date(year,month,d),false));
      }
      var totalCells=firstDayIndex+daysInMonth;
      var remaining=(7-(totalCells%7))%7;
      for(var n=1;n<=remaining;n++){
        daysGrid.appendChild(makeDayCell(new Date(year,month+1,n),true));
      }

      cal.appendChild(daysGrid);
      return cal;
    }

    function makeDayCell(date,isMuted){
      var iso=dpIso(date);
      var wrapCell=document.createElement('div');
      wrapCell.className='w-dp-day-wrap';

      var btn=document.createElement('button');
      btn.type='button';
      btn.className='w-dp-day';
      btn.textContent=date.getDate();
      btn.dataset.iso=iso;

      if(isMuted)btn.classList.add('muted');
      if(iso===dpIso(new Date()))btn.classList.add('today');

      if((minDate&&date<minDate)||(maxDate&&date>maxDate)){
        btn.classList.add('disabled');
        btn.disabled=true;
      }

      var curStart=startD?dpIso(startD):null;
      var curEnd=endD?dpIso(endD):null;
      var effEnd=selecting&&hoverD?dpIso(hoverD):curEnd;

      if(mode==='range'&&curStart){
        var isStart=iso===curStart;
        var isEnd=effEnd&&iso===effEnd;
        var sDate=startD,eDate=effEnd?dpParse(effEnd):startD;
        if(sDate&&eDate&&sDate>eDate){var tmp=sDate;sDate=eDate;eDate=tmp;}

        if(isStart||isEnd)btn.classList.add('selected');
        if(sDate&&eDate&&date>=sDate&&date<=eDate){
          wrapCell.classList.add('in-range');
          if(iso===dpIso(sDate))wrapCell.classList.add('range-start');
          if(iso===dpIso(eDate))wrapCell.classList.add('range-end');
        }
      }else if(curStart&&iso===curStart){
        btn.classList.add('selected');
      }

      btn.addEventListener('mouseenter',function(){
        if(mode==='range'&&selecting){
          hoverD=date;
          updateRangeHover();
        }
      });

      btn.addEventListener('click',function(e){
        e.stopPropagation();
        if(mode==='single'){
          applySelection(date,null);
        }else{
          if(!selecting){
            startD=date;endD=null;selecting=true;hoverD=date;
            buildPopover();
          }else{
            var d1=startD,d2=date;
            if(d1>d2){var tmp=d1;d1=d2;d2=tmp;}
            selecting=false;
            applySelection(d1,d2);
          }
        }
      });

      wrapCell.appendChild(btn);
      return wrapCell;
    }

    function updateRangeHover(){
      if(!startD||!hoverD)return;
      var s=startD,e=hoverD;
      if(s>e){var t=s;s=e;e=t;}
      pop.querySelectorAll('.w-dp-day-wrap').forEach(function(wrapC){
        var b=wrapC.querySelector('.w-dp-day');
        if(!b||!b.dataset.iso)return;
        var d=dpParse(b.dataset.iso);
        if(!d)return;
        if(d>=s&&d<=e){
          wrapC.classList.add('in-range');
          if(b.dataset.iso===dpIso(s))wrapC.classList.add('range-start');else wrapC.classList.remove('range-start');
          if(b.dataset.iso===dpIso(e))wrapC.classList.add('range-end');else wrapC.classList.remove('range-end');
        }else{
          wrapC.classList.remove('in-range','range-start','range-end');
        }
      });
    }

    function openFastGrid(calEl,year,month){
      var overlay=document.createElement('div');
      overlay.className='w-dp-fast-grid';

      var topBar=document.createElement('div');
      topBar.style.gridColumn='span 3';topBar.style.display='flex';
      topBar.style.alignItems='center';topBar.style.justifyContent='space-between';
      topBar.style.marginBottom='4px';

      var py=document.createElement('button');py.type='button';py.className='w-dp-nav-btn';py.innerHTML='&laquo;';
      var yrSpan=document.createElement('span');yrSpan.style.fontWeight='750';yrSpan.style.fontSize='14px';yrSpan.textContent=year;
      py.addEventListener('click',function(e){e.stopPropagation();year--;yrSpan.textContent=year;});
      var ny=document.createElement('button');ny.type='button';ny.className='w-dp-nav-btn';ny.innerHTML='&raquo;';
      ny.addEventListener('click',function(e){e.stopPropagation();year++;yrSpan.textContent=year;});

      topBar.appendChild(py);topBar.appendChild(yrSpan);topBar.appendChild(ny);
      overlay.appendChild(topBar);

      DP_MONTHS_SHORT.forEach(function(mName,idx){
        var mBtn=document.createElement('button');
        mBtn.type='button';mBtn.className='w-dp-fast-btn';mBtn.textContent=mName;
        if(idx===month)mBtn.classList.add('active');
        mBtn.addEventListener('click',function(e){
          e.stopPropagation();
          viewY=year;viewM=idx;
          buildPopover();
        });
        overlay.appendChild(mBtn);
      });

      calEl.style.position='relative';
      calEl.appendChild(overlay);
    }

    function applySelection(s,e){
      startD=s;endD=e;selecting=false;
      var newVal=mode==='range'?(dpIso(s)+'..'+dpIso(e||s)):dpIso(s);
      if(inp){
        inp.value=newVal;
        inp.dispatchEvent(new Event('change',{bubbles:true}));
      }
      syncFromVal();
      close();
    }

    function open(){
      buildPopover();
      box.classList.add('open');
      pop.classList.add('open');
      box.setAttribute('aria-expanded','true');
    }

    function close(){
      box.classList.remove('open');
      pop.classList.remove('open');
      box.setAttribute('aria-expanded','false');
      selecting=false;
    }

    box.addEventListener('click',function(e){
      if(e.target.closest('.w-dp-clear'))return;
      if(pop.classList.contains('open'))close();else open();
    });

    box.addEventListener('keydown',function(e){
      if(e.key==='Enter'||e.key===' '){
        e.preventDefault();
        if(pop.classList.contains('open'))close();else open();
      }else if(e.key==='Escape'){
        close();
      }
    });

    if(clearBtn){
      clearBtn.addEventListener('click',function(e){
        e.stopPropagation();
        if(inp){
          inp.value='';
          inp.dispatchEvent(new Event('change',{bubbles:true}));
        }
        syncFromVal();
        close();
      });
    }

    document.addEventListener('click',function(e){
      if(!wrap.contains(e.target))close();
    });

    wrap._dpController={sync:syncFromVal,close:close};
  }

  function initDatepickers(){
    document.querySelectorAll('.w-datepicker-wrap[data-dp="1"]').forEach(setupDatePicker);
  }

  initDatepickers();
}
)JS";

// The server's page: the pane's own URL, query and all, for its events and its stream (behind a proxy that picks the
// app by a query parameter (?route=...&key=...) a request to the bare path would go somewhere else).
const char* kPageBootstrap = R"JS(
(function(){
  var url=location.pathname+location.search;
  grainPane(document.getElementById('app'),{
    send:function(body){
      return fetch(url,{method:'POST',headers:{'X-Grain-UI':'1','Content-Type':'application/x-www-form-urlencoded'},body:body,credentials:'same-origin'})
        .then(function(r){return r.json()});
    },
    subscribe:window.EventSource?function(onFrame){
      var es=new EventSource(url);
      es.onmessage=function(m){var d;try{d=JSON.parse(m.data)}catch(x){return}onFrame(d)};
    }:null,
    reload:function(){location.reload()},
    hidden:function(){return document.hidden}
  });
})();
)JS";
