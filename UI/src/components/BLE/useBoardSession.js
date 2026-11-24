import { useEffect, useRef, useState } from "react";
import { isHookValue } from "./hookValue";
import { startTrafficFeed } from "./trafficFeed";
import { useHookControl } from "./useHookControl";

function replyToHook(data) {
  let value = data.value;
  try {
    const fn = data.onRead ? window.hook_read : window.hook_write;
    const result = fn?.(data);
    if (isHookValue(result)) value = result;
  } catch {
    // Preserve original traffic when a browser hook throws.
  }
  if (window.socket?.readyState === 1) window.socket.send(JSON.stringify({
    opt: 2, action: "hookret", request_id: data.request_id, hook_ret: value,
  }));
}

function trafficText(data) {
  if (data.msg) return data.msg;
  const type = data.type ? `${data.type}` : "";
  const value = data.data ? ` | Data: ${data.data}` : "";
  return `[${data.operation}] ${type} | ${data.direction} | TX_ID: ${data.trans_id} | ` +
    `Handle: ${data.handle} | UUID: ${data.uuid}${value}`;
}

export function useBoardSession() {
  const [session, setSession] = useState({ state: -1 });
  const state = session.state;
  const [connected, setConnected] = useState(false);
  const [results, setResults] = useState([]);
  const [selectedMac, setSelectedMac] = useState("");
  const hooks = useHookControl();
  const [error, setError] = useState("");
  const [hookError, setHookError] = useState("");
  const [messages, setMessages] = useState([]);
  const [pending, setPending] = useState("");
  const [confirmation, setConfirmation] = useState(null);
  const queue = useRef([]);
  const sequence = useRef(0);
  const request = useRef({ action: "", time: 0 });
  const scan = useRef({ state: -1, loaded: false, requestedAt: 0 });

  const dispatch = (action, fields = {}) => {
    if (window.socket?.readyState !== 1) {
      setError("Board disconnected. Reconnect before changing the session.");
      return;
    }
    setError("");
    if (action === "scan") {
      scan.current.loaded = false;
      scan.current.requestedAt = 0;
    }
    if (["scan", "start", "stop"].includes(action)) {
      request.current = { action, time: Date.now() };
      setPending(action);
    }
    window.socket.send(JSON.stringify({ opt: 2, action, ...fields }));
  };

  const send = (action, fields = {}) => {
    if (action === "start" || (action === "stop" && state >= 3))
      setConfirmation({ action, fields });
    else dispatch(action, fields);
  };
  const select = device => {
    const mac = device?.mac || "-";
    dispatch("select", { mac });
    if (window.socket?.readyState === 1)
      setSelectedMac(mac === "-" ? "" : mac.toUpperCase());
  };
  const confirm = () => {
    if (confirmation) dispatch(confirmation.action, confirmation.fields);
    setConfirmation(null);
  };

  useEffect(() => {
    let linksReceivedAt = 0;
    let retryLinksAt = 0;
    let nextLinks = null;
    const requestLinks = () => {
      const now = Date.now();
      if (window.socket?.readyState !== 1 || now - linksReceivedAt < 1800 || now < retryLinksAt)
        return false;
      retryLinksAt = now + 6000;
      window.socket.send(JSON.stringify({ opt: 90, id: 65535, command: "status" }));
      return true;
    };
    const requestResults = () => {
      if (scan.current.loaded || window.socket?.readyState !== 1 ||
          Date.now() - scan.current.requestedAt < 3000) return;
      scan.current.requestedAt = Date.now();
      window.socket.send(JSON.stringify({ opt: 2, action: "results" }));
    };
    const enqueue = (message, receivedAt = Date.now()) => {
      if (queue.current.length >= 1000) queue.current.shift();
      queue.current.push({ ...message, msg: trafficText(message),
        _uiId: ++sequence.current, _receivedAt: receivedAt });
    };
    const stopTraffic = startTrafficFeed(events => {
      for (const event of events) enqueue(event);
    }, restarted => enqueue({ msg: restarted
      ? "Board restarted; live GATT traffic resumed."
      : "Live GATT view missed events while catching up; PCAP is unaffected." }),
    () => enqueue({ msg: "Live GATT feed unavailable; retrying. PCAP recording continues." }));
    const poll = () => {
      const open = window.socket?.readyState === 1;
      setConnected(open);
      if (linksReceivedAt && Date.now() - linksReceivedAt > 6000) {
        linksReceivedAt = 0;
        setSession(previous => ({ state: previous.state }));
      }
      if (open && !(scan.current.state >= 3 && requestLinks()))
        window.socket.send(JSON.stringify({ opt: 2, action: "status" }));
      if (open && scan.current.state === 2) requestResults();
      hooks.poll();
      if (request.current.action && Date.now() - request.current.time > 30000) {
        setError("The board has not confirmed the action. Check its connection and try again.");
        request.current.action = "";
        setPending("");
      }
    };
    const callback = (data) => {
      // Hook replies stay synchronous; UI telemetry is batched separately.
      if (data.onRead || data.onWrite) replyToHook(data);
      hooks.receive(data);
      if ("connected" in data) {
        setConnected(Boolean(data.connected));
        setSession(previous => ({ state: data.connected ? -1 : previous.state }));
        linksReceivedAt = retryLinksAt = 0;
        nextLinks = null;
        if (data.connected) {
          scan.current.state = -1;
          scan.current.loaded = false;
          scan.current.requestedAt = 0;
          poll();
        }
      }
      // The bridge sends the same ordered status/target pair to the Flipper and browser.
      if (data.gpio?.startsWith("status\t")) {
        const [, phase, target, central] = data.gpio.split("\t", 4);
        nextLinks = /^[0-8]$/.test(phase) && /^[01]$/.test(target) && /^[01]$/.test(central)
          ? { state: Number(phase), targetConnected: target === "1",
            centralConnected: central === "1" }
          : null;
      } else if (data.gpio?.startsWith("target\t") && nextLinks) {
        const mac = data.gpio.slice(7).toUpperCase();
        if (nextLinks.state >= 3) {
          setSession({ ...nextLinks, mac: /^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac) ? mac : "" });
          linksReceivedAt = Date.now();
        } else {
          setSession({ state: nextLinks.state });
          linksReceivedAt = 0;
        }
        retryLinksAt = 0;
        nextLinks = null;
      }
      if (data.success === false) {
        setError(data.error || "The board rejected the action.");
        request.current.action = "";
        setPending("");
      }
      if ("hook_error" in data) setHookError(data.hook_error);
      if (typeof data.selected_mac === "string") {
        const mac = data.selected_mac.toUpperCase();
        const valid = /^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac);
        setSelectedMac(valid ? mac : "");
        if (valid && scan.current.state !== 1) requestResults();
      }
      if (typeof data.state === "number") {
        const phase = data.state;
        if (typeof data.target_connected === "boolean" &&
            typeof data.central_connected === "boolean") {
          setSession({ state: phase, targetConnected: data.target_connected,
            centralConnected: data.central_connected, mac: data.target_mac || "" });
          linksReceivedAt = Date.now();
        } else {
          setSession(previous => previous.state === phase ? previous :
            phase >= 3 && previous.state >= 3 ? { ...previous, state: phase } : { state: phase });
        }
        if (phase >= 3 && scan.current.state < 3) requestLinks();
        if (data.state !== scan.current.state && data.state !== 2) {
          scan.current.loaded = false;
          scan.current.requestedAt = 0;
        }
        scan.current.state = data.state;
        const action = request.current.action;
        if ((action === "scan" && data.state === 2) ||
            (action === "start" && data.state >= 3) || (action === "stop" && data.state === 0)) {
          request.current.action = "";
          setPending("");
        }
        if (data.state === 2 && !data.results) requestResults();
      }
      if (data.results) {
        scan.current.loaded = true;
        scan.current.requestedAt = 0;
        setResults(data.results);
      }
      if (data.msg || data.operation) enqueue(data);
    };
    window.socketCallback = callback;
    poll();
    const polling = setInterval(poll, 2000);
    const telemetry = setInterval(() => {
      if (!queue.current.length) return;
      const batch = queue.current;
      queue.current = [];
      setMessages(previous => [...previous, ...batch].slice(-1000));
    }, 100);
    return () => {
      stopTraffic();
      clearInterval(polling);
      clearInterval(telemetry);
      if (window.socketCallback === callback) window.socketCallback = null;
    };
  }, []);

  const clearMessages = () => { queue.current = []; setMessages([]); };
  return { state, session, connected, results, selectedMac, select,
    hooksEnabled: hooks.enabled,
    hooksPending: hooks.pending, toggleHooks: hooks.toggle,
    error, hookError: hooks.error || hookError, messages, pending,
    send, clearMessages, confirmation, confirm, cancel: () => setConfirmation(null) };
}
