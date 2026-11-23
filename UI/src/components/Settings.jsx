import { useCallback, useEffect, useRef, useState } from "react";
import {
  Alert, Box, Button, FormControlLabel, MenuItem, Stack, Switch, TextField, Typography,
} from "@mui/material";

const initial = { mode: "ap", ap_ssid: "", station_ssid: "", mcp_enabled: false };
const grantKey = "gattacker-settings-grant";

async function pairRequest(path, body, signal) {
  const response = await fetch(`/api/network/pair/${path}`, {
    method: "POST", cache: "no-store",
    headers: body ? { "Content-Type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.any([signal, AbortSignal.timeout(5000)]),
  });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || "Approval request failed");
  return result;
}

async function copyKey(value) {
  // Clipboard API is unavailable on the board's plain HTTP LAN origin.
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(value);
      return;
    } catch {
      // Fall back to the browser's user-gesture copy command.
    }
  }
  const field = document.createElement("textarea");
  field.value = value;
  field.style.position = "fixed";
  field.style.opacity = "0";
  document.body.appendChild(field);
  try {
    field.select();
    if (!document.execCommand("copy")) throw new Error("Clipboard unavailable");
  } finally {
    field.remove();
  }
}

export default function Settings() {
  const [settings, setSettings] = useState(initial);
  const [status, setStatus] = useState(null);
  const [token, setToken] = useState("");
  const [mcpKey, setMcpKey] = useState("");
  const [locked, setLocked] = useState(false);
  const [error, setError] = useState("");
  const [connectionError, setConnectionError] = useState("");
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);
  const [pairing, setPairing] = useState(false);
  const [apChange, setApChange] = useState(false);
  const [stationChange, setStationChange] = useState(false);
  const [apPassword, setApPassword] = useState("");
  const [stationPassword, setStationPassword] = useState("");
  const dirty = useRef(false), tokenRef = useRef(sessionStorage.getItem(grantKey) || "");
  const pairAbort = useRef(null);
  const update = (name, value) => {
    dirty.current = true;
    setSettings(previous => ({ ...previous, [name]: value }));
  };
  const request = useCallback(async (path = "", body, signal, method) => {
    const credential = tokenRef.current;
    const response = await fetch(`/api/network${path}`, {
      method: method || (body === undefined ? "GET" : "POST"),
      headers: { "Content-Type": "application/json",
        ...(credential ? { Authorization: `Bearer ${credential}` } : {}) },
      body: body === undefined ? undefined : JSON.stringify(body), cache: "no-store",
      signal: signal ? AbortSignal.any([signal, AbortSignal.timeout(5000)])
        : AbortSignal.timeout(5000),
    });
    if (response.status === 401 && credential === tokenRef.current) {
      setLocked(true);
      if (credential && credential === sessionStorage.getItem(grantKey)) {
        sessionStorage.removeItem(grantKey);
        tokenRef.current = "";
      }
    }
    if (!response.headers.get("content-type")?.includes("application/json")) {
      throw new Error("Firmware does not provide Wi-Fi settings. Install the matching build.");
    }
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "Wi-Fi request failed");
    setLocked(false);
    return data;
  }, []);
  const refresh = useCallback(async (signal) => {
    const next = await request("", undefined, signal);
    setStatus(next);
    if (!dirty.current) setSettings({ mode: next.mode, ap_ssid: next.ap_ssid,
      station_ssid: next.station_ssid, mcp_enabled: next.mcp_enabled });
  }, [request]);
  useEffect(() => {
    const controller = new AbortController();
    let timer;
    const poll = async () => {
      try { await refresh(controller.signal); setConnectionError(""); }
      catch (failure) { if (!controller.signal.aborted) setConnectionError(failure.message); }
      finally { if (!controller.signal.aborted) timer = setTimeout(poll, 2000); }
    };
    poll();
    return () => { controller.abort(); clearTimeout(timer); };
  }, [refresh]);
  useEffect(() => () => pairAbort.current?.abort(), []);
  const approve = async () => {
    pairAbort.current?.abort();
    const controller = new AbortController();
    pairAbort.current = controller;
    setPairing(true);
    setError("");
    try {
      const { ticket } = await pairRequest("start", null, controller.signal);
      for (let attempt = 0; attempt < 60; attempt++) {
        await new Promise(resolve => setTimeout(resolve, 1000));
        if (controller.signal.aborted) return;
        const { state } = await pairRequest("status", { ticket }, controller.signal);
        if (state === "pending") continue;
        if (state !== "approved") throw new Error("Approval was denied or expired");
        sessionStorage.setItem(grantKey, ticket);
        tokenRef.current = ticket;
        await refresh(controller.signal);
        setConnectionError("");
        return;
      }
      throw new Error("Approval expired. Try again.");
    } catch (failure) {
      if (!controller.signal.aborted) setError(failure.message);
    } finally {
      if (!controller.signal.aborted) setPairing(false);
    }
  };
  const unlock = async () => {
    tokenRef.current = token.trim();
    try { await refresh(); setError(""); } catch (failure) { setError(failure.message); }
  };
  const showKey = async () => {
    if (mcpKey) { setMcpKey(""); return; }
    try {
      const result = await request("/key");
      setMcpKey(result.key);
      setError("");
    } catch (failure) { setError(failure.message); }
  };
  const rotateKey = async () => {
    if (!window.confirm("Rotate the MCP key? Existing agent connections will need the new key."))
      return;
    setBusy(true); setError(""); setNotice(""); setMcpKey("");
    try {
      const result = await request("/key/rotate", undefined, undefined, "POST");
      if (tokenRef.current !== sessionStorage.getItem(grantKey)) {
        tokenRef.current = result.key;
        setToken(result.key);
      }
      setMcpKey(result.key);
      setNotice("Key rotated. Update your MCP clients with the new key.");
    } catch (failure) { setError(failure.message); }
    finally { setBusy(false); }
  };
  const copyMcpKey = async () => {
    try { await copyKey(mcpKey); setNotice("Access key copied."); setError(""); }
    catch { setError("Copy failed. Select the key and copy it manually."); }
  };
  const save = async (event) => {
    event.preventDefault();
    setBusy(true); setError(""); setNotice("");
    try {
      const patch = { ...settings };
      if (apChange) patch.ap_password = apPassword;
      if (stationChange) patch.station_password = stationPassword;
      setStatus(await request("", patch));
      dirty.current = false;
      setApChange(false); setStationChange(false); setApPassword(""); setStationPassword("");
      setNotice("Saved. Reconnect if Wi-Fi changes. The Flipper shows the LAN address. " +
        "Network mode keeps the recovery AP until connected.");
    } catch (failure) { setError(failure.message); }
    finally { setBusy(false); }
  };
  const scan = async () => {
    setBusy(true);
    try { setStatus(await request("/scan", {})); setError(""); }
    catch (failure) { setError(failure.message); }
    finally { setBusy(false); }
  };
  const endpoint = `http://${status?.station_ip || status?.ap_ip || "1.3.3.7"}/mcp`;
  return <Box sx={{ maxWidth: 640 }}>
    <Typography variant="h5" sx={{ mb: 2 }}>Settings</Typography>
    {(error || (locked ? "" : connectionError)) && <Alert severity="error" sx={{ mb: 2 }}>
      {error || connectionError}
    </Alert>}
    {notice && <Alert severity="info" sx={{ mb: 2 }}>{notice}</Alert>}
    {locked && <Stack spacing={1} sx={{ mb: 2, alignItems: "flex-start" }}>
      <Typography>Approve network settings on Flipper.</Typography>
      <Button variant="contained" disabled={pairing} onClick={approve}>
        {pairing ? "Waiting for Flipper..." : "Approve on Flipper"}
      </Button>
      <Box component="details" sx={{ width: "100%" }}>
        <Box component="summary" sx={{ cursor: "pointer" }}>Use access key instead</Box>
        <Stack spacing={1} sx={{ mt: 1 }}>
          <Typography variant="body2">
            Find the key on Flipper or through the ESP32 setup AP.
          </Typography>
          <TextField label="Access key" type="password" value={token} size="small"
            onChange={e => setToken(e.target.value)} />
          <Button onClick={unlock} sx={{ alignSelf: "flex-start" }}>Unlock settings</Button>
        </Stack>
      </Box>
    </Stack>}
    {status && !locked && <Typography variant="body2" sx={{ mb: 2, overflowWrap: "anywhere" }}>
      {status.state.replaceAll("_", " ")} | AP: {status.ap_active ? status.ap_ip : "off"}
      {" | "}Network: {status.station_ip || "not connected"}
      {status.error ? ` | ${status.error}` : ""}
    </Typography>}
    {status && !locked && <>
    <Typography variant="h6" sx={{ mb: 1 }}>Wi-Fi & MCP</Typography>
    <Stack component="form" spacing={2} onSubmit={save}>
      <TextField select label="Wi-Fi mode" size="small" value={settings.mode}
        onChange={e => update("mode", e.target.value)}>
        <MenuItem value="ap">Access point</MenuItem>
        <MenuItem value="station">Join network (AP on failure)</MenuItem>
        <MenuItem value="both">Join network + setup AP</MenuItem>
      </TextField>
      <TextField label="Setup AP name" size="small" required value={settings.ap_ssid}
        inputProps={{ maxLength: 32 }} onChange={e => update("ap_ssid", e.target.value)} />
      <FormControlLabel label="Change setup AP password"
        control={<Switch checked={apChange} onChange={e => setApChange(e.target.checked)} />} />
      {apChange && <TextField label="New AP password" type="password" size="small" required
        autoComplete="new-password" value={apPassword} inputProps={{ minLength: 8, maxLength: 63 }}
        onChange={e => setApPassword(e.target.value)} />}
      {settings.mode !== "ap" && <>
        <Stack direction={{ xs: "column", sm: "row" }} spacing={1}>
          <TextField label="Network name (SSID)" size="small" required fullWidth
            value={settings.station_ssid} inputProps={{ maxLength: 32 }}
            onChange={e => update("station_ssid", e.target.value)} />
          <Button onClick={scan} disabled={busy || status?.scanning || locked}>
            {status?.scanning ? "Scanning..." : "Find networks"}
          </Button>
        </Stack>
        {Boolean(status?.networks?.length) && <TextField select size="small" label="Nearby networks"
          value="" onChange={e => update("station_ssid", e.target.value)}>
          {status.networks.map((n, i) => <MenuItem key={i} value={n.ssid}>
            {n.ssid || "Hidden network"} | {n.rssi} dBm{n.secured ? "" : " | open"}
          </MenuItem>)}
        </TextField>}
        <FormControlLabel label="Set / change network password"
          control={<Switch checked={stationChange}
            onChange={e => setStationChange(e.target.checked)} />} />
        {stationChange && <TextField label="Network password" type="password" size="small"
          helperText="Leave empty only for an open network." autoComplete="new-password"
          value={stationPassword} inputProps={{ minLength: 8, maxLength: 63 }}
          onChange={e => setStationPassword(e.target.value)} />}
      </>}
      <FormControlLabel label="Enable MCP agent control"
        control={<Switch checked={settings.mcp_enabled}
          onChange={e => update("mcp_enabled", e.target.checked)} />} />
      <Button variant="contained" type="submit" disabled={busy || locked || !status}>
        Save & apply
      </Button>
    </Stack>
    <Stack spacing={1} sx={{ mt: 3 }}>
      <Typography variant="subtitle1">Agent connection</Typography>
      <TextField label="Streamable HTTP endpoint" size="small" value={endpoint}
        InputProps={{ readOnly: true }} />
      <Typography variant="body2">
        Use the access key as a Bearer token. Supports MCP 2026-07-28 and 2025-11-25.
        Keep the key private; use this HTTP endpoint on your trusted local network.
      </Typography>
      <Button onClick={showKey} disabled={busy}>
        {mcpKey ? "Hide access key" : "Show access key"}
      </Button>
      {mcpKey && <>
        <TextField label="Access key" size="small" value={mcpKey}
          onFocus={event => event.target.select()} InputProps={{ readOnly: true }} />
        <Stack direction="row" spacing={1}>
          <Button onClick={copyMcpKey} disabled={busy}>Copy key</Button>
          <Button onClick={rotateKey} disabled={busy}>Rotate key</Button>
        </Stack>
      </>}
    </Stack>
    <Button sx={{ mt: 3 }} onClick={() => {
      if (window.socket?.readyState !== 1) { setError("Board is disconnected"); return; }
      if (confirm("Reboot the ESP32? Unsaved RAM captures will be lost.")) {
        window.socket.send(JSON.stringify({ opt: 1, value: 9001 }));
      }
    }}>Reboot ESP32</Button>
    </>}
  </Box>;
}
