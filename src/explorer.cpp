#include "explorer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace plk {

namespace {

const char* PAGE = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PHYLKA Explorer</title>
<style>
:root{--bg:#f5f6fa;--card:#fff;--text:#1b1d24;--dim:#6b7080;--line:#e6e8ef;--accent:#5b5ff0;--up:#16a34a;--down:#dc2626;--mono:ui-monospace,SFMono-Regular,Menlo,monospace}
@media(prefers-color-scheme:dark){:root{--bg:#121318;--card:#1b1d24;--text:#e8e9ee;--dim:#8d92a3;--line:#2a2d37;--accent:#8b8ff8;--up:#4ade80;--down:#f87171}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
header{display:flex;gap:16px;align-items:center;padding:14px 24px;background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:0}
header a.logo{font-weight:800;letter-spacing:.06em;color:var(--text);text-decoration:none;font-size:18px}header a.logo span{color:var(--accent)}
header input{flex:1;max-width:620px;padding:9px 14px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--text);font:14px var(--mono)}
main{max-width:1100px;margin:0 auto;padding:22px 16px 60px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px 18px}
.stat .k{color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.05em}.stat .v{font-size:22px;font-weight:700;margin-top:2px;word-break:break-all}
h2{font-size:17px;margin:22px 0 10px}table{width:100%;border-collapse:collapse}td,th{padding:9px 10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{color:var(--dim);font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.04em}tr:last-child td{border-bottom:none}
.mono{font-family:var(--mono);font-size:13px;word-break:break-all}a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
.kv td:first-child{color:var(--dim);width:190px;white-space:nowrap}.pos{color:var(--up)}.neg{color:var(--down)}.dim{color:var(--dim)}
.badge{display:inline-block;padding:1px 8px;border-radius:99px;background:var(--line);font-size:12px;margin-left:6px}
.io{display:grid;grid-template-columns:1fr 32px 1fr;gap:10px;align-items:start}.io .arrow{text-align:center;color:var(--dim);font-size:22px;padding-top:10px}
.quote{font-style:italic;padding:12px 14px;border-left:3px solid var(--accent);background:var(--bg);border-radius:6px}
@media(max-width:720px){.io{grid-template-columns:1fr}.io .arrow{transform:rotate(90deg)}.hide-m{display:none}}
</style></head><body>
<header><a class="logo" href="#/">PHYL<span>KA</span></a><input id="q" placeholder="Block height, block hash, transaction ID or address plk1…" autocomplete="off"></header>
<main id="app">Loading…</main>
<script>
const $=s=>document.querySelector(s), app=$('#app');
const api=p=>fetch('/api/'+p).then(r=>r.json());
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const plk=f=>{const n=Number(f)/1e8;return n.toLocaleString('en-US',{maximumFractionDigits:8})+' PLK'};
const when=t=>new Date(t*1000).toLocaleString('en-US');
const ago=t=>{const s=Math.max(0,Date.now()/1000-t);return s<60?Math.floor(s)+'s ago':s<3600?Math.floor(s/60)+'m ago':s<86400?Math.floor(s/3600)+'h ago':Math.floor(s/86400)+'d ago'};
const short=h=>h.slice(0,10)+'…'+h.slice(-8);
const lb=h=>`<a class="mono" href="#/block/${h}">${h}</a>`, lt=(id,s)=>`<a class="mono" href="#/tx/${id}">${s?short(id):id}</a>`, la=(a,s)=>a?`<a class="mono" href="#/address/${a}">${s?a.slice(0,12)+'…'+a.slice(-8):a}</a>`:'<span class="dim">—</span>';
let timer;
async function home(){
  const [i,b]=await Promise.all([api('info'),api('blocks?count=25')]);
  app.innerHTML=`<div class="grid">
   <div class="card stat"><div class="k">Height</div><div class="v">${i.height.toLocaleString('en-US')}</div></div>
   <div class="card stat"><div class="k">Supply</div><div class="v">${plk(i.supply)}</div></div>
   <div class="card stat"><div class="k">Difficulty</div><div class="v">${Number(i.difficulty).toPrecision(4)}</div></div>
   <div class="card stat"><div class="k">Node hashrate</div><div class="v">${(i.hashrate/1e6).toFixed(2)} MH/s</div></div>
   <div class="card stat"><div class="k">Peers · mempool</div><div class="v">${i.peers} · ${i.mempool}</div></div>
   <div class="card stat"><div class="k">Finalized</div><div class="v">${i.finalized?'#'+i.finalized:'—'}</div></div>
   <div class="card stat"><div class="k">Validators · stake</div><div class="v">${i.validators} · ${plk(i.total_stake)}</div></div></div>
  ${i.total_stake<i.min_total_stake?`<div class="card" style="margin-bottom:16px">Finality activates once the total stake reaches ${plk(i.min_total_stake)}; until then the network runs as pure proof of work.</div>`:''}
  <div class="card"><h2 style="margin-top:0">Latest blocks</h2><table><tr><th>Height</th><th>Hash</th><th class="hide-m">Time</th><th>Txs</th><th class="hide-m">Miner</th><th>Reward</th></tr>
  ${b.map(x=>`<tr><td><a href="#/block/${x.height}">${x.height}</a></td><td>${lt(x.hash,1).replace('#/tx/','#/block/')}</td><td class="hide-m dim">${ago(x.time)}</td><td>${x.txs}</td><td class="hide-m">${x.height?la(x.miner,1):'<span class="dim">genesis</span>'}</td><td>${plk(x.reward)}</td></tr>`).join('')}</table></div>`;
  clearTimeout(timer);timer=setTimeout(()=>{if(location.hash<='#/')route()},10000);
}
async function block(h){
  const b=await api('block?h='+encodeURIComponent(h)); if(b.error) return notFound(b.error);
  app.innerHTML=`<h2>Block #${b.height}${b.height===0?'<span class="badge">genesis</span>':''}${b.finalized?'<span class="badge" style="background:var(--up);color:#fff">finalized</span>':''}${b.checkpoint?'<span class="badge">checkpoint</span>':''}</h2>
  <div class="card"><table class="kv">
   <tr><td>Hash</td><td class="mono">${b.hash}</td></tr>
   <tr><td>Previous</td><td>${b.height?lb(b.prev):'<span class="dim">—</span>'}</td></tr>
   <tr><td>Next</td><td>${b.next?lb(b.next):'<span class="dim">none yet</span>'}</td></tr>
   <tr><td>Time</td><td>${when(b.time)} <span class="dim">(${ago(b.time)})</span></td></tr>
   <tr><td>Difficulty</td><td>${Number(b.difficulty).toPrecision(6)} <span class="dim mono">bits ${b.bits}</span></td></tr>
   <tr><td>Nonce</td><td>${Number(b.nonce).toLocaleString('en-US')}</td></tr>
   <tr><td>Merkle root</td><td class="mono">${b.merkle}</td></tr>
   <tr><td>Size</td><td>${b.size.toLocaleString('en-US')} bytes</td></tr>
   <tr><td>Confirmations</td><td>${b.confirmations}</td></tr>
   ${b.text?`<tr><td>Message</td><td><div class="quote">${esc(b.text)}</div></td></tr>`:''}
  </table></div><h2>Transactions (${b.txs.length})</h2><div class="card"><table><tr><th>ID</th><th>Inputs → outputs</th><th>Amount</th></tr>
  ${b.txs.map(t=>`<tr><td>${lt(t.txid,1)}${t.coinbase?'<span class="badge">coinbase</span>':''}</td><td>${t.inputs} → ${t.outputs}</td><td>${plk(t.out)}</td></tr>`).join('')}</table></div>`;
}
async function tx(id){
  const t=await api('tx?id='+id); if(t.error) return notFound(t.error);
  app.innerHTML=`<h2>Transaction${t.coinbase?'<span class="badge">coinbase</span>':''}</h2><div class="card"><table class="kv">
   <tr><td>ID</td><td class="mono">${t.txid}</td></tr>
   <tr><td>Status</td><td>${t.height===null?'<b>waiting to be included in a block</b>':`in block <a href="#/block/${t.height}">#${t.height}</a>, ${t.confirmations} confirmations`}</td></tr>
   ${t.time?`<tr><td>Time</td><td>${when(t.time)}</td></tr>`:''}
   <tr><td>Size</td><td>${t.size.toLocaleString('en-US')} bytes <span class="dim">(witness ${t.witness_size.toLocaleString('en-US')} bytes)</span></td></tr>
   ${t.coinbase?'':`<tr><td>Fee</td><td>${plk(t.fee)}</td></tr><tr><td>Signatures</td><td>Schnorr secp256k1 + ML-DSA-44 <span class="badge">hybrid, post-quantum</span></td></tr>`}
   ${t.text?`<tr><td>Message</td><td><div class="quote">${esc(t.text)}</div></td></tr>`:''}
  </table></div><h2>Value flow</h2><div class="io">
  <div class="card">${t.coinbase?'<span class="dim">New coins (block reward)</span>':t.vin.map(i=>`<div>${la(i.address)}<br><b>${plk(i.value)}</b> <span class="dim">from ${lt(i.txid,1)}:${i.vout}</span></div>`).join('<hr style="border:none;border-top:1px solid var(--line)">')}</div>
  <div class="arrow">→</div>
  <div class="card">${t.vout.map(o=>`<div>${la(o.address)}<br><b>${plk(o.value)}</b> ${o.spent?`<span class="badge">spent</span>`:'<span class="badge">unspent</span>'}</div>`).join('<hr style="border:none;border-top:1px solid var(--line)">')}</div></div>`;
}
async function address(a){
  const d=await api('address?a='+a); if(d.error) return notFound(d.error);
  app.innerHTML=`<h2>Address</h2><div class="card"><div class="mono" style="font-size:15px">${d.address}</div>
  <div class="grid" style="margin:14px 0 0"><div class="stat"><div class="k">Balance</div><div class="v">${plk(d.balance)}</div></div>
  <div class="stat"><div class="k">Received</div><div class="v">${plk(d.received)}</div></div><div class="stat"><div class="k">Sent</div><div class="v">${plk(d.sent)}</div></div>
  <div class="stat"><div class="k">Transactions</div><div class="v">${d.history.length}</div></div></div></div>
  <h2>History</h2><div class="card"><table><tr><th>Block</th><th class="hide-m">Time</th><th>Transaction</th><th>Amount</th></tr>
  ${d.history.map(x=>`<tr><td>${x.height===null?'<span class="dim">pending</span>':`<a href="#/block/${x.height}">${x.height}</a>`}</td><td class="hide-m dim">${x.time?when(x.time):''}</td><td>${lt(x.txid,1)}${x.mined?'<span class="badge">mined</span>':''}</td><td class="${x.delta>0?'pos':'neg'}">${x.delta>0?'+':''}${plk(x.delta)}</td></tr>`).join('')||'<tr><td colspan=4 class="dim">No transactions</td></tr>'}</table></div>`;
}
function notFound(e){app.innerHTML=`<div class="card">Not found: ${esc(e)}. <a href="#/">Home</a></div>`}
async function route(){
  const [,kind,arg]=location.hash.split('/');
  try{ if(kind==='block')await block(arg); else if(kind==='tx')await tx(arg); else if(kind==='address')await address(arg); else await home(); }
  catch(e){app.innerHTML='<div class="card">The node is not responding. Is <code>phylka node</code> running?</div>'}
  window.scrollTo(0,0);
}
$('#q').addEventListener('keydown',async e=>{
  if(e.key!=='Enter')return; const v=e.target.value.trim(); e.target.value='';
  if(/^\d+$/.test(v)) location.hash='#/block/'+v;
  else if(/^plkd?1/i.test(v)) location.hash='#/address/'+v.toLowerCase();
  else if(/^[0-9a-f]{64}$/i.test(v)){const b=await api('block?h='+v); location.hash=b.error?'#/tx/'+v:'#/block/'+v}
  else notFound(v);
});
window.addEventListener('hashchange',route);route();
</script></body></html>)HTML";

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}

std::string query(const std::string& path, const std::string& key) {
    auto q = path.find('?');
    if (q == std::string::npos) return "";
    std::istringstream in(path.substr(q + 1));
    for (std::string kv; std::getline(in, kv, '&');) {
        auto eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
    }
    return "";
}

Hash256 parse_hash(const std::string& hex) {
    Bytes b;
    Hash256 h{};
    if (hex.size() == 64 && from_hex(hex, b)) std::reverse_copy(b.begin(), b.end(), h.begin());
    return h;
}

/// Coinbase text without the binary prefix (height, and extranonce for mined blocks)
std::string coinbase_text(const ChainParams& p, const Transaction& cb, uint32_t height) {
    auto& d = cb.vin[0].coinbase_data;
    size_t skip = std::min<size_t>(d.size(), coinbase_prefix_len(p, height));
    std::string s;
    for (size_t i = skip; i < d.size(); i++)
        if (d[i] >= 0x20) s += char(d[i]);
    return s;
}

double difficulty(uint32_t bits) {
    return double(U256::from_compact(bits).work().to_ld() / U256::from_compact(0x1d00ffff).work().to_ld());
}

/// Transaction and address index of the active chain, extended block by block as the chain grows
struct Index {
    Hash256 tip{};
    uint32_t height = 0;
    bool built = false;
    struct Loc { uint32_t height, index; };
    std::map<Hash256, Loc> txs;
    std::map<OutPoint, TxOut> outs;
    std::map<OutPoint, Hash256> spent_by;
    struct Event { uint32_t height, time; Hash256 txid; int64_t delta; bool mined; };
    std::map<Hash256, std::vector<Event>> history;   // by address program
    uint64_t supply = 0;

    void add_block(const Block& b, uint32_t h) {
        for (uint32_t i = 0; i < b.txs.size(); i++) {
            const Transaction& t = b.txs[i];
            Hash256 id = t.txid();
            txs[id] = Loc{h, i};
            std::map<Hash256, int64_t> delta;
            if (!t.is_coinbase())
                for (auto& in : t.vin) {
                    auto o = outs.find(in.prev);
                    if (o != outs.end()) delta[o->second.to.program] -= int64_t(o->second.value);
                    spent_by[in.prev] = id;
                }
            for (uint32_t o = 0; o < t.vout.size(); o++) {
                outs[OutPoint{id, o}] = t.vout[o];
                delta[t.vout[o].to.program] += int64_t(t.vout[o].value);
            }
            if (h > 0 && t.is_coinbase()) supply += block_reward_cached(h);
            for (auto& [prog, d] : delta)
                if (d) history[prog].push_back(Event{h, b.header.time, id, d, t.is_coinbase()});
        }
    }
    const ChainParams* params = nullptr;
    uint64_t block_reward_cached(uint32_t h) const { return block_reward(*params, h); }

    void update(Node& n) {
        params = &n.params();
        bool extend = built && n.at(height) && n.at(height)->hash == tip;
        if (!extend) { *this = Index{}; params = &n.params(); }
        uint32_t from = extend ? height + 1 : 0;
        for (uint32_t h = from; h <= n.height(); h++)
            if (auto b = n.read_block(n.at(h)->hash)) add_block(*b, h);
        tip = n.tip()->hash;
        height = n.height();
        built = true;
    }
};

Index g_index;
std::mutex g_index_m;

}  // namespace

bool explorer_route(Node& n, size_t peers, double hashrate, const std::string& path, std::string& body, std::string& ctype) {
    std::string route = path.substr(0, path.find('?'));
    if (route == "/" || route == "/explorer") {
        ctype = "text/html; charset=utf-8";
        body = PAGE;
        return true;
    }
    if (route.rfind("/api/", 0) != 0) return false;
    ctype = "application/json; charset=utf-8";
    const ChainParams& p = n.params();
    std::lock_guard<std::mutex> lk(n.mutex());
    std::lock_guard<std::mutex> ik(g_index_m);
    g_index.update(n);
    auto addr_of = [&](const TxOut& o) { return Address{o.to.version, o.to.program}.str(p.hrp); };
    std::ostringstream o;

    if (route == "/api/info") {
        o << "{\"network\":" << json_str(p.name) << ",\"height\":" << n.height() << ",\"tip\":\"" << hash_hex(n.tip()->hash)
          << "\",\"difficulty\":" << difficulty(n.tip()->header.bits) << ",\"next_difficulty\":" << difficulty(n.next_bits(n.tip()))
          << ",\"hashrate\":" << hashrate << ",\"peers\":" << peers << ",\"mempool\":" << n.mempool().size()
          << ",\"supply\":" << g_index.supply << ",\"max_supply\":" << MAX_MONEY;
        uint32_t cp = n.height() / p.checkpoint_interval * p.checkpoint_interval;
        const StakeSet* st = cp ? n.stake_at(cp) : nullptr;
        o << ",\"finalized\":" << n.finalized_height() << ",\"checkpoint\":" << cp << ",\"validators\":" << (st ? st->size() : 0)
          << ",\"total_stake\":" << (st ? n.total_stake(*st) : 0) << ",\"voted_stake\":" << (cp ? n.voted_stake(cp) : 0)
          << ",\"min_total_stake\":" << p.min_total_stake << "}";
    } else if (route == "/api/blocks") {
        int count = std::clamp(std::atoi(query(path, "count").c_str()), 1, 100);
        std::string before = query(path, "before");
        int64_t top = before.empty() ? int64_t(n.height()) : std::min<int64_t>(std::atoll(before.c_str()) - 1, n.height());
        o << "[";
        for (int64_t h = top, k = 0; h >= 0 && k < count; h--, k++) {
            auto bi = n.at(uint32_t(h));
            auto b = n.read_block(bi->hash);
            if (!b) continue;
            o << (k ? "," : "") << "{\"height\":" << h << ",\"hash\":\"" << hash_hex(bi->hash) << "\",\"time\":" << bi->header.time
              << ",\"txs\":" << b->txs.size() << ",\"size\":" << b->serialize().size() << ",\"reward\":" << b->txs[0].vout[0].value
              << ",\"miner\":" << json_str(addr_of(b->txs[0].vout[0])) << "}";
        }
        o << "]";
    } else if (route == "/api/block") {
        std::string h = query(path, "h");
        const BlockIndex* bi = nullptr;
        if (!h.empty() && h.size() < 12 && h.find_first_not_of("0123456789") == std::string::npos) bi = n.at(uint32_t(std::stoul(h)));
        else {
            Hash256 hh = parse_hash(h);
            for (uint32_t i = 0; i <= n.height() && !bi; i++) if (n.at(i)->hash == hh) bi = n.at(i);
        }
        auto b = bi ? n.read_block(bi->hash) : std::nullopt;
        if (!b) { body = "{\"error\":\"block not found in the main chain\"}"; return true; }
        o << "{\"height\":" << bi->height << ",\"hash\":\"" << hash_hex(bi->hash) << "\",\"prev\":\"" << hash_hex(bi->header.prev)
          << "\",\"next\":" << (n.at(bi->height + 1) ? "\"" + hash_hex(n.at(bi->height + 1)->hash) + "\"" : "null")
          << ",\"time\":" << bi->header.time << ",\"bits\":\"" << std::hex << bi->header.bits << std::dec << "\",\"nonce\":" << bi->header.nonce
          << ",\"difficulty\":" << difficulty(bi->header.bits) << ",\"merkle\":\"" << hash_hex(bi->header.merkle) << "\",\"size\":"
          << b->serialize().size() << ",\"confirmations\":" << n.height() - bi->height + 1
          << ",\"finalized\":" << (bi->height <= n.finalized_height() && n.finalized_height() > 0 ? "true" : "false")
          << ",\"checkpoint\":" << (bi->height % p.checkpoint_interval == 0 && bi->height ? "true" : "false") << ",\"text\":"
          << json_str(coinbase_text(n.params(), b->txs[0], bi->height)) << ",\"txs\":[";
        for (size_t i = 0; i < b->txs.size(); i++) {
            const Transaction& t = b->txs[i];
            uint64_t out = 0;
            for (auto& v : t.vout) out += v.value;
            o << (i ? "," : "") << "{\"txid\":\"" << hash_hex(t.txid()) << "\",\"coinbase\":" << (t.is_coinbase() ? "true" : "false")
              << ",\"inputs\":" << (t.is_coinbase() ? 0 : t.vin.size()) << ",\"outputs\":" << t.vout.size() << ",\"out\":" << out << "}";
        }
        o << "]}";
    } else if (route == "/api/tx") {
        Hash256 id = parse_hash(query(path, "id"));
        std::optional<Transaction> t;
        std::optional<uint32_t> height;
        uint32_t time = 0;
        auto loc = g_index.txs.find(id);
        if (loc != g_index.txs.end()) {
            if (auto b = n.read_block(n.at(loc->second.height)->hash)) {
                t = b->txs[loc->second.index];
                height = loc->second.height;
                time = b->header.time;
            }
        } else if (n.mempool().count(id)) {
            t = n.mempool().at(id);
        }
        if (!t) { body = "{\"error\":\"transaction not found\"}"; return true; }
        Bytes raw = t->serialize(true);
        size_t base = t->serialize(false).size();
        uint64_t in_sum = 0, out_sum = 0;
        o << "{\"txid\":\"" << hash_hex(id) << "\",\"height\":" << (height ? std::to_string(*height) : "null") << ",\"time\":" << time
          << ",\"confirmations\":" << (height ? n.height() - *height + 1 : 0) << ",\"coinbase\":" << (t->is_coinbase() ? "true" : "false")
          << ",\"size\":" << raw.size() << ",\"witness_size\":" << raw.size() - base << ",\"text\":"
          << json_str(t->is_coinbase() && height ? coinbase_text(n.params(), *t, *height) : "") << ",\"vin\":[";
        if (!t->is_coinbase())
            for (size_t i = 0; i < t->vin.size(); i++) {
                auto& in = t->vin[i];
                auto prev = g_index.outs.find(in.prev);
                uint64_t v = prev != g_index.outs.end() ? prev->second.value : 0;
                in_sum += v;
                o << (i ? "," : "") << "{\"txid\":\"" << hash_hex(in.prev.txid) << "\",\"vout\":" << in.prev.vout << ",\"value\":" << v
                  << ",\"address\":" << (prev != g_index.outs.end() ? json_str(addr_of(prev->second)) : "null") << "}";
            }
        o << "],\"vout\":[";
        for (uint32_t i = 0; i < t->vout.size(); i++) {
            out_sum += t->vout[i].value;
            o << (i ? "," : "") << "{\"value\":" << t->vout[i].value << ",\"address\":" << json_str(addr_of(t->vout[i]))
              << ",\"spent\":" << (g_index.spent_by.count(OutPoint{id, i}) ? "true" : "false") << "}";
        }
        o << "],\"fee\":" << (t->is_coinbase() || in_sum < out_sum ? 0 : in_sum - out_sum) << "}";
    } else if (route == "/api/address") {
        auto a = Address::parse(query(path, "a"), p.hrp);
        if (!a) { body = "{\"error\":\"invalid address or wrong network\"}"; return true; }
        uint64_t balance = 0, received = 0, sent = 0;
        for (auto& [op, c] : n.utxo().coins)
            if (c.out.to.program == a->program) balance += c.out.value;
        auto& ev = g_index.history[a->program];
        for (auto& e : ev) (e.delta > 0 ? received : sent) += uint64_t(e.delta > 0 ? e.delta : -e.delta);
        o << "{\"address\":" << json_str(a->str(p.hrp)) << ",\"balance\":" << balance << ",\"received\":" << received
          << ",\"sent\":" << sent << ",\"history\":[";
        bool first = true;
        for (auto& [mid, t] : n.mempool()) {
            int64_t d = 0;
            for (auto& in : t.vin) {
                auto prev = g_index.outs.find(in.prev);
                if (prev != g_index.outs.end() && prev->second.to.program == a->program) d -= int64_t(prev->second.value);
            }
            for (auto& v : t.vout) if (v.to.program == a->program) d += int64_t(v.value);
            if (d) { o << (first ? "" : ",") << "{\"height\":null,\"time\":0,\"txid\":\"" << hash_hex(mid) << "\",\"delta\":" << d << ",\"mined\":false}"; first = false; }
        }
        for (auto it = ev.rbegin(); it != ev.rend(); ++it) {
            o << (first ? "" : ",") << "{\"height\":" << it->height << ",\"time\":" << it->time << ",\"txid\":\"" << hash_hex(it->txid)
              << "\",\"delta\":" << it->delta << ",\"mined\":" << (it->mined ? "true" : "false") << "}";
            first = false;
        }
        o << "]}";
    } else {
        o << "{\"error\":\"unknown request\"}";
    }
    body = o.str();
    return true;
}

}  // namespace plk
