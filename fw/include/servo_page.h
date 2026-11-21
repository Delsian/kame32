#pragma once

constexpr char ServoPageHead[] = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Kame32 Servo Control</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#eef3ef;color:#182823;font:16px system-ui,sans-serif}
main{max-width:32rem;margin:auto;padding:1.25rem}h1{font-size:1.6rem;margin:.3rem 0 1rem}
button{width:100%;padding:.8rem;border:0;border-radius:.5rem;background:#962b34;color:white;font:inherit;font-weight:700}
section{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:.75rem}
fieldset{min-width:0;margin:0;padding:.15rem .65rem .35rem;border:1px solid #d4ded7;border-radius:.65rem;background:white}
legend{padding:0 .25rem;font-weight:700}
label{display:block;padding:.65rem 0;border-bottom:1px solid #e1e8e3}
label:last-child{border:0}output{float:right;font-variant-numeric:tabular-nums}input{display:block;width:100%;margin:.45rem 0 0;accent-color:#23654e}
nav{margin-bottom:1rem}a{color:#23654e}
.bounds{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:.4rem}
.bounds label{border:0;padding:.25rem 0 .65rem}.bounds input{font:inherit}
#save{background:#23654e;margin-top:1rem}
@media(max-width:420px){.bounds{grid-template-columns:1fr;gap:0}}
</style><main>
)HTML";

constexpr char ServoPageStart[] = R"HTML(<h1>Kame32 Servo Control</h1>
<nav><a href="/limits">Servo limits</a></nav><button id="stop">STOP · Release servos</button>
<p id="status" aria-live="polite">Move a slider to set its pulse width.</p><section>
)HTML";

constexpr char LimitsPageStart[] = R"HTML(<h1>Servo limits</h1>
<nav><a href="/">Back to servo control</a></nav>
<p>Set each servo’s minimum and maximum pulse width in µs.</p>
<form id="limits"><section>
)HTML";

constexpr char LimitsPageEnd[] = R"HTML(</section><button id="save">Save limits</button>
<p id="status" role="status"></p></form></main><script>
const form = document.querySelector('#limits');
const save = document.querySelector('#save');
const status = document.querySelector('#status');
form.addEventListener('submit', async event => {
  event.preventDefault();
  save.disabled = true;
  status.textContent = 'Saving…';
  try {
    const response = await fetch('/limits', {
      method: 'POST', headers: {'X-Kame32-Control': '1'},
      body: new URLSearchParams(new FormData(form))
    });
    status.textContent = await response.text();
  } catch {
    status.textContent = 'Connection lost. Reload this page to check the saved limits.';
  } finally {
    save.disabled = false;
  }
});
</script></html>)HTML";

constexpr char ServoPageEnd[] = R"HTML(</section></main><script>
const pending = new Map();
const status = document.querySelector('#status');
let busy = false, holding = false, stopPending = false;

async function send(path, body = '') {
  const response = await fetch(path, {method: 'POST', headers: {'X-Kame32-Control': '1'},
    body, signal: AbortSignal.timeout(1500)});
  if (!response.ok) throw Error();
}

async function flush() {
  if (busy) return;
  busy = true;
  try {
    while (stopPending || pending.size) {
      if (stopPending) {
        stopPending = false;
        holding = false;
        await send('/stop');
        status.textContent = 'Stop sent.';
      } else {
        const [channel, {pulse, location}] = pending.entries().next().value;
        pending.delete(channel);
        await send('/servo', `${channel} ${pulse}`);
        holding = true;
        status.textContent = `Sent ${pulse} µs to ${location}.`;
      }
    }
  } catch {
    holding = false;
    pending.clear();
    status.textContent = 'Command failed. Check the Wi-Fi connection and try again.';
  } finally {
    busy = false;
    if (stopPending || pending.size) flush();
  }
}

document.querySelectorAll('input[data-channel]').forEach(slider => slider.addEventListener('input', () => {
  slider.previousElementSibling.value = `${slider.value} µs`;
  pending.set(slider.dataset.channel, {pulse: slider.value, location: slider.dataset.location});
  flush();
}));
document.querySelector('#stop').onclick = () => {
  pending.clear();
  stopPending = true;
  flush();
};
setInterval(async () => {
  if (!holding || busy || document.hidden) return;
  busy = true;
  try { await send('/hold'); }
  catch { holding = false; }
  finally { busy = false; flush(); }
}, 500);

</script></html>)HTML";
