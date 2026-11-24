export function startTrafficFeed(onBatch, onGap, onError) {
  let cursor = 0;
  let active = true;
  let timer;
  let request;
  let failures = 0;

  const schedule = delay => {
    if (active) timer = setTimeout(poll, delay);
  };
  const poll = async () => {
    request = new AbortController();
    try {
      const response = await fetch(`/api/traffic?after=${cursor}&limit=64`, {
        cache: "no-store", signal: request.signal,
      });
      if (!response.ok) throw new Error(`Traffic feed returned ${response.status}`);
      const batch = await response.json();
      if (!Array.isArray(batch.events) || !Number.isSafeInteger(batch.next_cursor) ||
          !Number.isFinite(batch.uptime_ms)) throw new Error("Invalid traffic feed");
      if (!active) return;
      failures = 0;
      if (batch.gap && cursor) onGap(batch.newest < cursor);
      cursor = batch.next_cursor;
      onBatch(batch.events, batch.uptime_ms);
      schedule(batch.has_more ? 0 : 200);
    } catch {
      if (active && ++failures === 3) onError();
      schedule(750);
    }
  };
  poll();
  return () => {
    active = false;
    clearTimeout(timer);
    request?.abort();
  };
}
