"""The audition page for the modern sounds: every effect's original, its Algol version and its candidates side by
side, to be picked from by ear.

    python tools/sfxlib/audition.py     -> H:/PSIV Project/sfx-library/index.html (+ cand/XX/*.wav)

The page lives outside the repo: the originals beside the candidates are the game's own sound (the clips in
H:/PSIV Project/sfx-algol, made from the ROM). Picks are kept in the browser; "Copy my picks" gives the text to
paste back, which build.py turns into res/sounds.
"""
import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CAND = os.path.join(ROOT, 'build', 'sfxlib-cand')
OUT = r'H:\PSIV Project\sfx-library'
ALGOL = r'H:\PSIV Project\sfx-algol'

GROUPS = [
    ('Menus', ['F2', 'F3', 'F4', 'F1']),
    ('Weapons and blows', ['F5', 'B5', 'B6', 'B7', 'B8', 'E8', 'BA', 'D7', 'D5', 'D8', 'DA', 'D2', 'D0', 'B9', 'D4']),
    ('Casts, buffs and healing', ['BB', 'EB', 'BC', 'C9', 'DC', 'BD', 'CC', 'CD', 'C4', 'CF', 'D9', 'DE', 'F0', 'D1']),
    ('Fire, wind, light, thunder, things breaking', ['BE', 'C2', 'C6', 'BF', 'C0', 'C1', 'C3', 'C5', 'C7', 'C8', 'CA', 'CB', 'CE', 'D3', 'EC', 'EA', 'EE', 'E6', 'DD']),
    ('Doors, stairs, lifts, machines', ['E2', 'ED', 'E1', 'DF', 'E0', 'E5', 'E3', 'E4', 'D6', 'DB', 'EF']),
]


def main():
    man = json.load(open(os.path.join(CAND, 'candidates.json'), encoding='utf-8'))
    dest = os.path.join(OUT, 'cand')
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    shutil.copytree(CAND, dest, ignore=shutil.ignore_patterns('candidates.json'))
    for sub in ('chip', 'algol'):                         # the originals, beside the page
        d = os.path.join(OUT, sub)
        os.makedirs(d, exist_ok=True)
        for sid in man:
            src = os.path.join(ALGOL, sub, sid + '.wav')
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(d, sid + '.wav'))
    groups = []
    for title, ids in GROUPS:
        rows = []
        for sid in ids:
            e = man.get(sid)
            if not e:
                continue
            rows.append({'id': sid, 'name': e['name'], 'what': e['what'], 'secs': e['original_secs'],
                         'cands': [{'f': c['file'], 'label': c['label'], 'mix': c['kind'] == 'mix',
                                    'by': ', '.join(sorted({s['author'] for s in c['sources']}))} for c in e['candidates']]})
        groups.append({'title': title, 'rows': rows})
    page = PAGE.replace('/*DATA*/', json.dumps(groups))
    open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8', newline='\n').write(page)
    print('index.html:', sum(len(g['rows']) for g in groups), 'effects,', sum(len(r['cands']) for g in groups for r in g['rows']), 'candidates ->', OUT)


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PSIV Recomp: modern sounds, audition</title>
<style>
:root { --bg:#0a0e24; --panel:#121a3c; --line:#2a3670; --ink:#e8ecff; --dim:#9aa6d8; --gold:#ffd45a; --blue:#78dcff; --green:#7be0a0; }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--ink); font:16px/1.5 "Segoe UI", system-ui, sans-serif; }
main { max-width:1100px; margin:0 auto; padding:28px 16px 90px; }
h1 { font-size:30px; margin:0 0 6px; color:var(--gold); }
h2 { font-size:21px; margin:36px 0 8px; color:var(--gold); }
p { margin:6px 0; color:var(--dim); max-width:72ch; }
p b { color:var(--ink); }
.fx { background:var(--panel); border:1px solid var(--line); border-radius:14px; padding:12px 14px; margin:10px 0; }
.fx.done { border-color:var(--green); }
.fx.none { border-color:#a05a5a; }
.head { display:flex; gap:10px; align-items:baseline; flex-wrap:wrap; }
.head b { font-size:17px; }
.head span { color:var(--dim); font-size:14px; }
.row { display:flex; gap:8px; flex-wrap:wrap; margin-top:8px; align-items:center; }
button.play { font:inherit; font-weight:600; border:1px solid var(--line); background:#0d1330; color:var(--ink); border-radius:999px; padding:5px 13px; cursor:pointer; }
button.play:hover { border-color:var(--blue); }
button.play.ref { color:var(--dim); }
button.play.on { background:var(--blue); color:#0a0e24; border-color:var(--blue); }
.cand { display:inline-flex; align-items:center; gap:6px; background:#0d1330; border:1px solid var(--line); border-radius:999px; padding:2px 10px 2px 2px; }
.cand.picked { border-color:var(--green); background:#12301f; }
.cand button.play { border:0; background:transparent; padding:4px 10px; max-width:260px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
.cand small { color:var(--gold); font-weight:700; }
.cand label { display:inline-flex; align-items:center; gap:4px; color:var(--dim); font-size:13px; cursor:pointer; }
.cand input { accent-color:var(--green); width:17px; height:17px; }
input[type=text] { font:inherit; background:#0d1330; color:var(--ink); border:1px solid var(--line); border-radius:8px; padding:5px 8px; flex:1; min-width:220px; }
label.nope { color:var(--dim); font-size:14px; display:inline-flex; gap:5px; align-items:center; cursor:pointer; }
.bar { position:sticky; bottom:0; background:#0a0e24ee; border-top:1px solid var(--line); padding:10px 16px; }
.bar div { max-width:1100px; margin:0 auto; display:flex; gap:12px; align-items:center; flex-wrap:wrap; }
.bar button { font:inherit; font-weight:700; background:var(--gold); color:#141a3a; border:0; border-radius:10px; padding:9px 18px; cursor:pointer; }
.bar span { color:var(--dim); }
textarea { width:100%; height:170px; margin-top:10px; display:none; background:#0d1330; color:var(--ink); border:1px solid var(--line); border-radius:8px; font:13px/1.4 Consolas, monospace; padding:8px; }
</style>
</head>
<body>
<main>
<h1>Modern sounds: audition</h1>
<p>For each of the game's sound effects: the <b>Original</b> on the chip, the same on <b>Algol</b>, and up to eight candidates for its
new modern sound, made from public-domain (CC0) recordings. Each candidate is already cut to fit the animation and set to the
original's loudness. A gold <b>MIX</b> is two sounds layered.</p>
<p>They were picked by file name and by measurement, not by ear, so expect misses. <b>Tick every candidate you would accept</b>
(several ticks = variants the game picks from at random), or tick <b>None of these</b> and say what it should sound like.
<b>Copy my picks</b> at the bottom gives you text to paste back. Your ticks are kept in this browser.</p>
<div id="groups"></div>
<textarea id="out" readonly></textarea>
</main>
<div class="bar"><div><button id="copy">Copy my picks</button><span id="count"></span></div></div>
<script>
const GROUPS = /*DATA*/;
let picks = {};
try { picks = JSON.parse(localStorage.getItem('psiv-sfx-library-picks') || '{}'); } catch (e) {}
const save = () => { try { localStorage.setItem('psiv-sfx-library-picks', JSON.stringify(picks)); } catch (e) {} count(); };
let audio = null, lit = null;
function play(btn, src) {
  if (audio) { audio.pause(); if (lit) lit.classList.remove('on'); if (lit === btn) { audio = lit = null; return; } }
  audio = new Audio(src); lit = btn; btn.classList.add('on');
  audio.onended = () => { btn.classList.remove('on'); if (lit === btn) audio = lit = null; };
  audio.play();
}
const root = document.getElementById('groups');
let total = 0;
for (const g of GROUPS) {
  const h = document.createElement('h2'); h.textContent = g.title; root.appendChild(h);
  for (const r of g.rows) {
    total++;
    const st = picks[r.id] = picks[r.id] || {c: [], none: false, note: ''};
    const box = document.createElement('div'); box.className = 'fx';
    const head = document.createElement('div'); head.className = 'head';
    const b = document.createElement('b'); b.textContent = '$' + r.id + '  ' + r.name;
    const s = document.createElement('span'); s.textContent = r.what + ' \u00b7 the original lasts ' + r.secs + ' s';
    head.appendChild(b); head.appendChild(s); box.appendChild(head);
    const refs = document.createElement('div'); refs.className = 'row';
    for (const [dir, label] of [['chip', 'Original'], ['algol', 'Algol']]) {
      const pb = document.createElement('button'); pb.className = 'play ref'; pb.textContent = label;
      pb.onclick = () => play(pb, dir + '/' + r.id + '.wav'); refs.appendChild(pb);
    }
    box.appendChild(refs);
    const row = document.createElement('div'); row.className = 'row';
    const mark = () => { box.classList.toggle('done', st.c.length > 0); box.classList.toggle('none', st.none && !st.c.length); };
    for (const c of r.cands) {
      const key = c.f.replace('.wav', '');
      const wrap = document.createElement('span'); wrap.className = 'cand';
      const pb = document.createElement('button'); pb.className = 'play'; pb.title = c.label + ' (' + c.by + ')';
      if (c.mix) { const m = document.createElement('small'); m.textContent = 'MIX '; pb.appendChild(m); }
      pb.appendChild(document.createTextNode(key + ': ' + c.label));
      pb.onclick = () => play(pb, 'cand/' + r.id + '/' + c.f);
      const lab = document.createElement('label'); const cb = document.createElement('input'); cb.type = 'checkbox';
      cb.checked = st.c.includes(key); wrap.classList.toggle('picked', cb.checked);
      cb.onchange = () => { if (cb.checked) { if (!st.c.includes(key)) st.c.push(key); } else st.c = st.c.filter(x => x !== key);
                            wrap.classList.toggle('picked', cb.checked); mark(); save(); };
      lab.appendChild(cb); lab.appendChild(document.createTextNode('use'));
      wrap.appendChild(pb); wrap.appendChild(lab); row.appendChild(wrap);
    }
    box.appendChild(row);
    const foot = document.createElement('div'); foot.className = 'row';
    const nl = document.createElement('label'); nl.className = 'nope'; const nc = document.createElement('input'); nc.type = 'checkbox';
    nc.checked = !!st.none; nc.onchange = () => { st.none = nc.checked; mark(); save(); };
    nl.appendChild(nc); nl.appendChild(document.createTextNode('None of these'));
    const inp = document.createElement('input'); inp.type = 'text'; inp.placeholder = 'a note: what it should sound like, what is wrong, too long, too loud...';
    inp.value = st.note || ''; inp.oninput = () => { st.note = inp.value; save(); };
    foot.appendChild(nl); foot.appendChild(inp); box.appendChild(foot);
    mark(); root.appendChild(box);
  }
}
function count() {
  const ids = Object.keys(picks);
  const done = ids.filter(id => picks[id].c.length).length, none = ids.filter(id => picks[id].none && !picks[id].c.length).length;
  document.getElementById('count').textContent = done + ' of ' + total + ' effects have a pick' + (none ? ', ' + none + ' marked none' : '');
}
count();
document.getElementById('copy').onclick = () => {
  const lines = [];
  for (const g of GROUPS) for (const r of g.rows) {
    const st = picks[r.id]; if (!st || (!st.c.length && !st.none && !st.note)) continue;
    lines.push(r.id + ' ' + r.name + ': ' + (st.c.length ? st.c.join(', ') : st.none ? 'NONE' : '-') + (st.note ? ' | ' + st.note : ''));
  }
  const text = lines.length ? 'Modern sounds, my picks:\n' + lines.join('\n') : 'No picks yet.';
  const out = document.getElementById('out'); out.style.display = 'block'; out.value = text; out.focus(); out.select();
  let ok = false; try { ok = document.execCommand('copy'); } catch (e) {}
  document.getElementById('count').textContent = ok ? 'Copied: paste it in the chat' : 'Select the text above and copy it';
  out.scrollIntoView({block: 'nearest'});
};
</script>
</body>
</html>
"""

if __name__ == '__main__':
    main()
