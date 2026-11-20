#pragma once

constexpr char ServoPageStart[] = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Kame32 Servo Control</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#eef3ef;color:#182823;font:16px system-ui,sans-serif}
main{max-width:32rem;margin:auto;padding:1.25rem}h1{font-size:1.6rem;margin:.3rem 0 1rem}
button{width:100%;padding:.8rem;border:0;border-radius:.5rem;background:#962b34;color:white;font:inherit;font-weight:700}
section{background:white;padding:.3rem 1rem;border-radius:.65rem}label{display:block;padding:.65rem 0;border-bottom:1px solid #e1e8e3}
label:last-child{border:0}output{float:right;font-variant-numeric:tabular-nums}input{display:block;width:100%;margin:.45rem 0 0;accent-color:#23654e}
</style><main><h1>Kame32 Servo Control</h1><button id="stop">STOP · Release servos</button>
<p id="status" aria-live="polite">Move a slider to set its pulse width.</p><section>
)HTML";

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
        const [channel, pulse] = pending.entries().next().value;
        pending.delete(channel);
        await send('/servo', `${channel} ${pulse}`);
        holding = true;
        status.textContent = `Sent ${pulse} µs to channel ${channel}.`;
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
  pending.set(slider.dataset.channel, slider.value);
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
