#pragma once
#include <Arduino.h>

const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Chess Clock Games</title>
<style>
  :root { --bg:#f4f1ea; --card:#ffffff; --ink:#1d1d1d; --dim:#6b6b6b; --accent:#2f6f4f; --light:#ecdcbf; --dark:#a97c58; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--ink); font:16px/1.4 system-ui, -apple-system, Segoe UI, Roboto, sans-serif; }
  main { max-width: 980px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 24px; margin: 4px 0 12px; }
  h2 { font-size: 18px; margin: 0 0 10px; }
  .card { background:var(--card); border-radius:12px; padding:14px; margin-bottom:14px; box-shadow:0 1px 3px rgba(0,0,0,.08); }
  .cols { display:flex; flex-wrap:wrap; gap:14px; }
  .cols > div { flex: 1 1 320px; min-width: 0; }
  label { display:block; font-size:14px; color:var(--dim); margin:6px 0 2px; }
  input, select, textarea, button { font:inherit; }
  input, select, textarea { width:100%; padding:8px; border:1px solid #c9c4b8; border-radius:8px; background:#fff; color:var(--ink); }
  textarea { height:120px; font-family: ui-monospace, Menlo, Consolas, monospace; font-size:13px; }
  button { padding:9px 14px; border:0; border-radius:8px; background:var(--accent); color:#fff; cursor:pointer; }
  button.plain { background:#e3ded3; color:var(--ink); }
  button:disabled { opacity:.5; cursor:default; }
  .row { display:flex; gap:8px; flex-wrap:wrap; align-items:end; margin-top:8px; }
  .row > * { flex: 1 1 120px; }
  .row > button { flex: 0 0 auto; }
  #board { display:grid; grid-template-columns:repeat(8, 1fr); grid-template-rows:repeat(8, 1fr); width:100%; max-width:440px; aspect-ratio:1; border-radius:6px; overflow:hidden; user-select:none; }
  .sq { display:flex; align-items:center; justify-content:center; cursor:pointer; position:relative; aspect-ratio:1; min-width:0; }
  .light { background:var(--light); } .dark { background:var(--dark); }
  .sq.last { box-shadow: inset 0 0 0 100px rgba(235, 205, 60, .45); }
  .sq.sel { box-shadow: inset 0 0 0 4px #2f6f4f; }
  .sq.target::after { content:""; width:28%; height:28%; border-radius:50%; background:rgba(0,0,0,.25); position:absolute; }
  .pc { font-size: min(9vw, 42px); line-height:1; }
  .pc.w { color:#fff; text-shadow: 0 0 2px #000, 0 0 1px #000, 0 0 1px #000; }
  .pc.b { color:#111; }
  table { width:100%; border-collapse:collapse; font-size:14px; }
  th, td { padding:4px 6px; text-align:left; border-bottom:1px solid #eee; }
  th { color:var(--dim); font-weight:600; }
  td.used { color:var(--dim); }
  .tablewrap { max-height:440px; overflow:auto; }
  .note { color:var(--dim); font-size:14px; }
  .ok { color:#1d6b3a; } .err { color:#b3261e; }
</style>
</head>
<body>
<main>
  <h1>&#9822; Chess Clock games</h1>
  <p id="topMessage" class="note"></p>

  <div class="card">
    <label for="gameSelect">Saved game</label>
    <select id="gameSelect"></select>
    <p id="gameInfo" class="note"></p>
  </div>

  <div class="cols">
    <div class="card">
      <h2>1. Replay the moves</h2>
      <div id="board"></div>
      <div class="row">
        <button id="undoBtn" class="plain">Undo</button>
        <button id="flipBtn" class="plain">Flip board</button>
        <div>
          <label for="promoSelect">Promote to</label>
          <select id="promoSelect">
            <option value="q">Queen</option><option value="r">Rook</option>
            <option value="b">Bishop</option><option value="n">Knight</option>
          </select>
        </div>
      </div>
      <p id="progress" class="note"></p>
    </div>
    <div class="card">
      <h2>Clock times</h2>
      <div class="tablewrap">
        <table>
          <thead><tr><th>#</th><th>White</th><th>Clock</th><th>Black</th><th>Clock</th></tr></thead>
          <tbody id="timeBody"></tbody>
        </table>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>2. Game details</h2>
    <div class="row">
      <div><label for="whiteName">White</label><input id="whiteName" value="White"></div>
      <div><label for="blackName">Black</label><input id="blackName" value="Black"></div>
    </div>
    <div class="row">
      <div><label for="eventName">Event</label><input id="eventName" value="Casual game"></div>
      <div><label for="gameDate">Date</label><input id="gameDate" type="date"></div>
      <div>
        <label for="resultSelect">Result</label>
        <select id="resultSelect">
          <option value="1-0">1-0 (White won)</option>
          <option value="0-1">0-1 (Black won)</option>
          <option value="1/2-1/2">1/2-1/2 (Draw)</option>
          <option value="*">* (Unfinished)</option>
        </select>
      </div>
    </div>
    <label for="pgnBox">PGN (with each move's clock time)</label>
    <textarea id="pgnBox" readonly></textarea>
    <div class="row"><button id="copyBtn" class="plain">Copy PGN</button></div>
  </div>

  <div class="card">
    <h2>3. Export to a Lichess study</h2>
    <div class="row">
      <div><label for="studyInput">Study link or ID</label><input id="studyInput" placeholder="https://lichess.org/study/AbCd1234"></div>
      <div><label for="chapterName">Chapter name</label><input id="chapterName" maxlength="100"></div>
      <button id="exportBtn">Export</button>
    </div>
    <p id="exportStatus" class="note"></p>
  </div>
</main>

<script src="https://cdnjs.cloudflare.com/ajax/libs/chess.js/0.10.3/chess.min.js"></script>
<script>
const GLYPHS = { k: "\u265A", q: "\u265B", r: "\u265C", b: "\u265D", n: "\u265E", p: "\u265F" };
const byId = (id) => document.getElementById(id);
let chess = null;
let game = null;
let flipped = false;
let selected = null;

function pad2(n) { return String(n).padStart(2, "0"); }

// h:mm:ss for the PGN [%clk] tags
function clockTag(ms) {
  const t = Math.floor(ms / 1000);
  return Math.floor(t / 3600) + ":" + pad2(Math.floor(t / 60) % 60) + ":" + pad2(t % 60);
}

// Short form for the table
function shortTime(ms) {
  if (ms < 60000) return (ms / 1000).toFixed(1) + "s";
  const t = Math.floor(ms / 1000);
  const h = Math.floor(t / 3600);
  const m = Math.floor(t / 60) % 60;
  return (h > 0 ? h + ":" + pad2(m) : m) + ":" + pad2(t % 60);
}

function timeControlText(g) {
  const w = g.whiteBaseMs / 60000;
  const b = g.blackBaseMs / 60000;
  const base = (w === b) ? String(w) : w + "/" + b;
  return base + (g.mode === "delay" ? " delay " : "+") + g.bonusSec + (g.mode === "delay" ? "s" : "");
}

async function loadGameList() {
  let data;
  try {
    const res = await fetch("/api/games");
    data = await res.json();
  } catch (e) {
    byId("topMessage").textContent = "Could not reach the clock. Is it still in Wi-Fi mode?";
    return;
  }
  if (!data.tokenSet) {
    byId("topMessage").innerHTML = "No Lichess token in <b>secrets.h</b> yet, so Export won't work. Everything else does.";
  }
  if (data.studyId) byId("studyInput").value = data.studyId;

  const games = data.games.sort((a, b) => b.number - a.number);
  const select = byId("gameSelect");
  select.innerHTML = "";
  if (games.length === 0) {
    select.innerHTML = "<option>No saved games yet</option>";
    byId("gameInfo").textContent = "Play a game first. It's saved when a flag falls, or when you hold Start while paused.";
    return;
  }
  for (const g of games) {
    const opt = document.createElement("option");
    opt.value = g.slot;
    opt.textContent = "Game " + g.number + "  \u00B7  " + timeControlText(g) + "  \u00B7  " + g.plies + " moves  \u00B7  " + g.result;
    select.appendChild(opt);
  }
  select.onchange = () => loadGame(select.value);
  loadGame(games[0].slot);
}

async function loadGame(slot) {
  const res = await fetch("/api/game?slot=" + slot);
  game = await res.json();
  if (typeof Chess === "undefined") {
    byId("topMessage").textContent = "chess.js didn't load. Your phone or laptop needs internet for the board.";
    return;
  }
  chess = new Chess();
  selected = null;
  byId("resultSelect").value = game.result;
  byId("chapterName").value = "Game " + game.number;
  byId("gameInfo").textContent = "Time control " + timeControlText(game) + ". The clock recorded " +
    game.clocks.length + " moves (half-moves). Replay them on the board below.";
  render();
}

function render() {
  drawBoard();
  drawTable();
  byId("pgnBox").value = buildPgn();
  const entered = chess.history().length;
  const recorded = game.clocks.length;
  let text = "Moves entered: " + entered + " of " + recorded + ". ";
  if (entered < recorded) text += (chess.turn() === "w" ? "White" : "Black") + " to move.";
  else if (entered === recorded) text += "All moves entered.";
  else text += "Extra moves have no clock time.";
  byId("progress").textContent = text;
}

function drawBoard() {
  const board = chess.board();
  const el = byId("board");
  el.innerHTML = "";
  const targets = selected ? chess.moves({ square: selected, verbose: true }).map((m) => m.to) : [];
  const history = chess.history({ verbose: true });
  const last = history.length ? history[history.length - 1] : null;
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const row = flipped ? 7 - r : r;
      const col = flipped ? 7 - c : c;
      const square = "abcdefgh"[col] + (8 - row);
      const piece = board[row][col];
      const div = document.createElement("div");
      div.className = "sq " + ((row + col) % 2 ? "dark" : "light");
      if (last && (square === last.from || square === last.to)) div.classList.add("last");
      if (square === selected) div.classList.add("sel");
      if (targets.includes(square)) div.classList.add("target");
      if (piece) {
        const span = document.createElement("span");
        span.className = "pc " + piece.color;
        span.textContent = GLYPHS[piece.type] + "\uFE0E";
        div.appendChild(span);
      }
      div.onclick = () => clickSquare(square);
      el.appendChild(div);
    }
  }
}

function clickSquare(square) {
  if (!chess) return;
  if (selected) {
    const move = chess.move({ from: selected, to: square, promotion: byId("promoSelect").value });
    if (move) {
      selected = null;
      render();
      return;
    }
  }
  const piece = chess.get(square);
  selected = (piece && piece.color === chess.turn()) ? square : null;
  drawBoard();
}

function drawTable() {
  const sans = chess.history();
  const bonus = game.mode === "increment" ? game.bonusSec * 1000 : 0;
  const before = [game.whiteBaseMs, game.blackBaseMs];
  const cells = [];
  const total = Math.max(game.clocks.length, sans.length);
  for (let i = 0; i < total; i++) {
    const side = i % 2;
    const clock = game.clocks[i];
    let used = "";
    let shown = "";
    if (clock !== undefined) {
      used = shortTime(Math.max(0, before[side] - (clock - bonus)));
      shown = shortTime(clock);
      before[side] = clock;
    }
    cells.push({ san: sans[i] || "\u2026", used, shown });
  }
  let html = "";
  for (let i = 0; i < cells.length; i += 2) {
    const w = cells[i];
    const b = cells[i + 1];
    html += "<tr><td>" + (i / 2 + 1) + "</td>" + cellHtml(w) + (b ? cellHtml(b) : "<td></td><td></td>") + "</tr>";
  }
  byId("timeBody").innerHTML = html;
}

// One player's half of a table row: the move, then the clock (and time used)
function cellHtml(cell) {
  const used = cell.used ? " <span class='used'>(" + cell.used + ")</span>" : "";
  return "<td>" + cell.san + "</td><td>" + cell.shown + used + "</td>";
}

function pgnHeader(name, value) {
  return "[" + name + " \"" + String(value).replace(/["\\]/g, "") + "\"]\n";
}

function buildPgn() {
  const sans = chess.history();
  const result = byId("resultSelect").value;
  const date = byId("gameDate").value ? byId("gameDate").value.replace(/-/g, ".") : "????.??.??";
  let pgn = "";
  pgn += pgnHeader("Event", byId("eventName").value || "Casual game");
  pgn += pgnHeader("Site", "Chess clock");
  pgn += pgnHeader("Date", date);
  pgn += pgnHeader("White", byId("whiteName").value || "White");
  pgn += pgnHeader("Black", byId("blackName").value || "Black");
  pgn += pgnHeader("Result", result);
  if (game.mode === "increment" && game.whiteBaseMs === game.blackBaseMs) {
    pgn += pgnHeader("TimeControl", (game.whiteBaseMs / 1000) + "+" + game.bonusSec);
  }
  pgn += "\n";
  let moves = "";
  sans.forEach((san, i) => {
    const moveNumber = Math.floor(i / 2) + 1;
    if (i % 2 === 0) moves += moveNumber + ". ";
    else if (i - 1 < game.clocks.length) moves += moveNumber + "... ";
    moves += san + " ";
    if (i < game.clocks.length) moves += "{ [%clk " + clockTag(game.clocks[i]) + "] } ";
  });
  return pgn + moves + result + "\n";
}

async function exportGame() {
  const status = byId("exportStatus");
  const raw = byId("studyInput").value.trim();
  const fromLink = raw.match(/study\/([A-Za-z0-9]{8})/);
  const studyId = fromLink ? fromLink[1] : raw;
  if (!/^[A-Za-z0-9]{8}$/.test(studyId)) {
    status.className = "note err";
    status.textContent = "Paste your study link (lichess.org/study/...) or its 8-character ID.";
    return;
  }
  if (!chess || chess.history().length === 0) {
    status.className = "note err";
    status.textContent = "Replay at least one move first.";
    return;
  }
  const button = byId("exportBtn");
  button.disabled = true;
  status.className = "note";
  status.textContent = "Sending to Lichess...";
  try {
    const body = new URLSearchParams({ study: studyId, name: byId("chapterName").value, pgn: buildPgn() });
    const res = await fetch("/api/export", { method: "POST", body: body });
    const data = await res.json();
    if (data.ok) {
      status.className = "note ok";
      status.innerHTML = "Done! <a href='" + data.url + "' target='_blank' rel='noopener'>Open the chapter on Lichess</a>";
    } else {
      status.className = "note err";
      status.textContent = data.message;
    }
  } catch (e) {
    status.className = "note err";
    status.textContent = "The clock didn't answer. Check that it's still in Wi-Fi mode.";
  }
  button.disabled = false;
}

byId("undoBtn").onclick = () => { if (chess) { chess.undo(); selected = null; render(); } };
byId("flipBtn").onclick = () => { flipped = !flipped; if (chess) drawBoard(); };
byId("copyBtn").onclick = () => { byId("pgnBox").select(); navigator.clipboard && navigator.clipboard.writeText(byId("pgnBox").value); };
byId("exportBtn").onclick = exportGame;
for (const id of ["whiteName", "blackName", "eventName", "gameDate", "resultSelect"]) {
  byId(id).addEventListener("input", () => { if (chess) byId("pgnBox").value = buildPgn(); });
}
const today = new Date();
byId("gameDate").value = today.getFullYear() + "-" + pad2(today.getMonth() + 1) + "-" + pad2(today.getDate());
loadGameList();
</script>
</body>
</html>
)HTML";
