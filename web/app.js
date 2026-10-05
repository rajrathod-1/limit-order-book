// Drives the WebAssembly build of the engine: a random order flow in, the
// book, the trades and the benchmark out. All matching happens in C++.
import createEngine from "./engine.js";

const BUY = 0;
const SELL = 1;
const LEVELS = 12;
const $ = (id) => document.getElementById(id);
const money = (cents) => "$" + (cents / 100).toFixed(2);
const int = (n) => Math.round(n).toLocaleString("en-US");
const rand = (n) => Math.floor(Math.random() * n);
const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;

let wasm;
try {
  wasm = await createEngine();
} catch (err) {
  $("load-error").hidden = false;
  throw err;
}

/** The last call's results, as `n` records of `stride` numbers. */
const read = (n, stride) => {
  const p = wasm._out_ptr() / 8;
  return Array.from(wasm.HEAPF64.subarray(p, p + n * stride));
};

// ── Ladder: asks above the spread (best nearest it), bids below ──────────────

const ladder = $("ladder");
const makeRow = (side) => {
  const row = document.createElement("div");
  row.className = `row ${side}`;
  row.innerHTML = '<span class="bar"></span><span class="px"></span><span></span><span class="qty"></span>';
  ladder.append(row);
  return { row, bar: row.children[0], px: row.children[1], qty: row.children[3] };
};
const askRows = Array.from({ length: LEVELS }, () => makeRow("ask")).reverse();
const spreadEl = Object.assign(document.createElement("div"), { className: "spread mono dim" });
ladder.append(spreadEl);
const bidRows = Array.from({ length: LEVELS }, () => makeRow("bid"));

// ── Order flow: mostly passive orders near a wandering mid, cancels, and a few
//    orders that cross the spread. Prices are integer cents. ─────────────────

const resting = []; // ids that may still rest; a cancel of a filled one is a harmless reject
let mid = 10000;
let paused = false;
let rate = reduced ? 3 : 12;
let trades = 0;
let volume = 0;
const away = () => 1 + Math.floor(-Math.log(1 - Math.random()) * 3);

function step() {
  const roll = Math.random();
  if (roll < 0.52 || resting.length < 200) {
    const side = rand(2);
    const id = wasm._limit(side, side === BUY ? mid - away() : mid + away(), 100 * (1 + rand(10)));
    if (id > 0) resting.push(id);
  } else if (roll < 0.9 || resting.length > 1800) {
    const k = rand(resting.length);
    wasm._cancel(resting[k]);
    resting[k] = resting[resting.length - 1];
    resting.pop();
  } else if (roll < 0.98) {
    const side = rand(2);
    const id = wasm._limit(side, side === BUY ? mid + rand(3) : mid - rand(3), 100 * (1 + rand(8)));
    if (id > 0) resting.push(id);
  } else {
    wasm._market(rand(2), 100 * (1 + rand(15)));
  }
}

// ── Rendering ───────────────────────────────────────────────────────────────

const tape = $("tape");
const hits = new Map(); // price -> time until which its row stays highlighted

function paintSide(rows, levels, max, now) {
  rows.forEach((r, i) => {
    const price = levels[i * 2];
    const qty = levels[i * 2 + 1];
    if (i * 2 >= levels.length) {
      r.px.textContent = r.qty.textContent = "";
      r.bar.style.width = "0";
      r.row.classList.remove("hit");
      return;
    }
    r.px.textContent = money(price);
    r.qty.textContent = int(qty);
    r.bar.style.width = `${(qty / max) * 100}%`;
    r.row.classList.toggle("hit", (hits.get(price) ?? 0) > now);
  });
}

function render() {
  const now = performance.now();

  const fills = read(wasm._fills(), 3);
  for (let i = 0; i < fills.length; i += 3) {
    trades++;
    volume += fills[i + 1];
    if (!reduced) hits.set(fills[i], now + 260);
  }
  // Newest first; a sweep can print dozens in a frame, so keep the last few.
  const time = new Date().toLocaleTimeString("en-GB");
  for (let i = Math.max(0, fills.length - 14 * 3); i < fills.length; i += 3) {
    const li = document.createElement("li");
    const side = fills[i + 2] === BUY ? "B" : "S";
    li.innerHTML = `<span class="dim">${time}</span><span class="${side}">${side === "B" ? "BUY" : "SELL"}</span><span>${money(fills[i])}</span><span>${int(fills[i + 1])}</span>`;
    tape.prepend(li);
  }
  while (tape.children.length > 14) tape.lastChild.remove();

  const asks = read(wasm._depth(SELL, LEVELS), 2);
  const bids = read(wasm._depth(BUY, LEVELS), 2);
  let max = 1;
  for (let i = 1; i < asks.length; i += 2) max = Math.max(max, asks[i]);
  for (let i = 1; i < bids.length; i += 2) max = Math.max(max, bids[i]);
  paintSide(askRows, asks, max, now);
  paintSide(bidRows, bids, max, now);

  if (asks.length && bids.length) {
    spreadEl.innerHTML = `<span>Spread ${money(asks[0] - bids[0])}</span><span>${int(wasm._live_orders())} resting</span>`;
    $("mid").textContent = `Mid ${money((asks[0] + bids[0]) / 2)}`;
    // Let the flow follow the book, with a little drift of its own.
    mid = Math.round((asks[0] + bids[0]) / 2) + (Math.random() < 0.3 ? rand(3) - 1 : 0);
    mid = Math.min(30000, Math.max(500, mid + Math.sign(10000 - mid) * (Math.random() < 0.02 ? 1 : 0)));
  }

  $("s-orders").textContent = int(wasm._live_orders());
  $("s-trades").textContent = int(trades);
  $("s-volume").textContent = int(volume);
}

function frame() {
  if (!paused) for (let i = 0; i < rate; i++) step();
  render();
  requestAnimationFrame(frame);
}

// Seed a book so the first frame isn't empty.
for (let i = 0; i < 1200; i++) step();
requestAnimationFrame(frame);

// ── Controls ────────────────────────────────────────────────────────────────

$("rate").value = String(rate);
$("rate").addEventListener("input", (e) => (rate = Number(e.target.value)));

const pauseBtn = $("pause");
pauseBtn.addEventListener("click", () => {
  paused = !paused;
  pauseBtn.textContent = paused ? "Resume" : "Pause";
});

document.querySelectorAll("[data-sweep]").forEach((btn) =>
  btn.addEventListener("click", () => wasm._market(Number(btn.dataset.sweep), 5000))
);

// ── Benchmark ───────────────────────────────────────────────────────────────

const benchBtn = $("run-bench");
const status = $("bench-status");
const table = $("bench-table");
const nextPaint = () => new Promise((r) => setTimeout(r, 30));

benchBtn.addEventListener("click", async () => {
  benchBtn.disabled = true;
  const wasPaused = paused;
  paused = true;
  table.hidden = false;
  const body = table.tBodies[0];
  body.innerHTML = "";

  for (const width of [8, 64, 512, 4096]) {
    const median = [];
    for (const useMap of [0, 1]) {
      status.textContent = `Running ${useMap ? "std::map" : "flat array"} · ${int(width)} levels…`;
      await nextPaint();
      const runs = [0, 1, 2].map(() => wasm._bench(useMap, width, 300000)).sort((a, b) => a - b);
      median.push(runs[1]);
    }
    const [array, map] = median;
    body.insertAdjacentHTML(
      "beforeend",
      `<tr><td>${int(width)} levels</td><td>${array.toFixed(1)} ns</td><td>${map.toFixed(1)} ns</td><td><strong>${(map / array).toFixed(2)}×</strong></td></tr>`
    );
  }

  status.textContent = "Done. Measured on your machine, in your browser.";
  paused = wasPaused;
  benchBtn.disabled = false;
});
