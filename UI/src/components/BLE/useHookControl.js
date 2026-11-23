import { useRef, useState } from "react";

export function useHookControl() {
  const [enabled, setEnabled] = useState(false);
  const [pending, setPending] = useState(null);
  const [error, setError] = useState("");
  const request = useRef(null);
  const sequence = useRef(Date.now() % 65535);
  const finish = message => {
    request.current = null;
    setPending(null);
    setError(message);
  };
  const toggle = desired => {
    if (request.current) return;
    if (window.socket?.readyState !== 1) {
      finish("Board disconnected. Reconnect before changing hooks.");
      return;
    }
    // The browser reserves request ID 65535 for read-only bridge status.
    sequence.current = sequence.current % 65534 + 1;
    request.current = { id: sequence.current, desired, time: Date.now() };
    setPending(desired);
    setError("");
    window.socket.send(JSON.stringify({ opt: 2, action: "enable_hooks", enable: desired,
      request_id: sequence.current }));
  };
  const receive = data => {
    if (typeof data.hooks_enabled === "boolean") {
      setEnabled(data.hooks_enabled);
      if (request.current?.desired === data.hooks_enabled) finish("");
    }
    if (request.current && data.request_id === request.current.id && data.gpio) {
      if (data.gpio.startsWith("error\t")) finish(data.gpio.slice(6));
      else if (window.socket?.readyState === 1)
        window.socket.send(JSON.stringify({ opt: 2, action: "status" }));
    }
    if (data.connected === false && request.current)
      finish("Board disconnected before confirming the hook change.");
  };
  const poll = () => {
    if (request.current && Date.now() - request.current.time > 5000)
      finish("The board has not confirmed the hook change. Check its connection and retry.");
  };
  return { enabled, pending, error, toggle, receive, poll };
}
