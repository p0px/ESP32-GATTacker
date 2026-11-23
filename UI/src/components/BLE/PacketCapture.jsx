import { useEffect, useRef, useState } from "react";
import {
  Alert, Box, Button, CircularProgress, Dialog, DialogActions, DialogContent,
  DialogContentText, DialogTitle, LinearProgress, Stack, Typography,
  MenuItem, Select,
} from "@mui/material";
import DownloadIcon from "@mui/icons-material/Download";
import FiberManualRecordIcon from "@mui/icons-material/FiberManualRecord";
import StopIcon from "@mui/icons-material/Stop";
import { FlipperCapturePanel } from "./FlipperCapturePanel";

async function request(path = "", payload, signal) {
  const timeout = AbortSignal.timeout(5000);
  const response = await fetch(`/api/capture${path}`, {
    method: payload ? "POST" : "GET",
    headers: payload ? { "Content-Type": "application/json" } : undefined,
    body: payload ? JSON.stringify(payload) : undefined,
    cache: "no-store",
    signal: signal ? AbortSignal.any([signal, timeout]) : timeout,
  });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || "Capture request failed");
  return result;
}

const sizeLabel = (bytes) => `${(bytes / 1024).toFixed(1)} KiB`;
const labels = { empty: "", recording: "Recording", saving: "Saving",
  ready: "Ready to download" };

export function PacketCapture({ onRetainedChange, onStateChange }) {
  const [status, setStatus] = useState(null);
  const [offline, setOffline] = useState(false);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState("");
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const [destination, setDestination] = useState(() =>
    sessionStorage.getItem("gattacker-capture-destination") === "flipper" ? "flipper" : "web");
  const operation = useRef(false);
  const revision = useRef(0);

  useEffect(() => {
    sessionStorage.setItem("gattacker-capture-destination", destination);
  }, [destination]);

  useEffect(() => {
    if (!status) return;
    const sdActive = !status.auto_export && status.sd?.connected &&
      ["starting", "recording", "saving"].includes(status.sd.state);
    onRetainedChange?.(status.state !== "empty" || sdActive);
    onStateChange?.(sdActive ? status.sd.state : status.state);
    if (status.auto_export || (sdActive && status.sd?.connected))
      setDestination("flipper");
  }, [status, onRetainedChange, onStateChange]);

  useEffect(() => {
    const controller = new AbortController();
    let timer;
    const poll = async () => {
      try {
        if (!operation.current) {
          const current = revision.current;
          const next = await request("", undefined, controller.signal);
          if (!operation.current && revision.current === current) {
            setStatus(next);
            setOffline(false);
          }
        }
      } catch (failure) {
        if (failure.name !== "AbortError") setOffline(true);
      } finally {
        if (!controller.signal.aborted) timer = setTimeout(poll, 1000);
      }
    };
    poll();
    return () => { controller.abort(); clearTimeout(timer); };
  }, []);

  const action = async (name) => {
    if (operation.current) return;
    operation.current = true;
    revision.current++;
    setBusy(name);
    setError("");
    setConfirmDiscard(false);
    try {
      const liveSd = status?.owner === "flipper";
      const body = name === "start" ? { seconds: Math.floor(Date.now() / 1000) }
        : { id: liveSd && name === "stop" ? status.sd.id : status.id };
      if (name === "start" && destination === "flipper") body.destination = "flipper";
      if (name === "stop" && liveSd) body.destination = "flipper_live";
      setStatus(await request(`/${name}`, body));
      setOffline(false);
    } catch (failure) {
      setError(failure.message);
    } finally {
      operation.current = false;
      setBusy("");
    }
  };

  const download = async () => {
    if (operation.current) return;
    operation.current = true;
    revision.current++;
    setBusy("download");
    setError("");
    try {
      const response = await fetch(`/api/capture/download?id=${status.id}`, {
        cache: "no-store", signal: AbortSignal.timeout(120000),
      });
      if (!response.ok) {
        const result = await response.json();
        throw new Error(result.error || "Download failed; try again");
      }
      const blob = await response.blob();
      if (blob.size !== status.bytes) throw new Error("Incomplete download; try again");
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = `esp32-capture-${status.id}.pcapng`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    } catch (failure) {
      setError(failure.message);
    } finally {
      operation.current = false;
      setBusy("");
    }
  };

  const disabled = Boolean(busy) || offline || !status?.available || status?.downloading;
  const recording = status?.state === "recording";
  const saving = status?.state === "saving";
  const ready = status?.state === "ready";
  const flipper = status?.owner === "flipper";
  const sd = destination === "flipper";
  const sdState = status?.sd?.state;
  const sdActive = !status?.auto_export && status?.sd?.connected &&
    ["starting", "recording", "saving"].includes(sdState);
  const exportCurrent = status?.export?.id === status?.id;
  const exporting = exportCurrent && status?.export?.state === "transferring" &&
    status?.downloading;
  const copied = ready && status?.sd?.id === status.id && status?.sd?.complete;
  const sdLabels = { starting: "Starting…", recording: "Recording to SD", saving: "Saving…",
    ready: "Saved to SD", error: "Check recording" };
  return (
    <Box component="section" aria-label="Packet capture"
      sx={{ px: 2, py: 1.5,
      "& .MuiTypography-body2": { fontSize: 12, lineHeight: 1.5 },
      "& .MuiTypography-caption": { fontSize: 12, lineHeight: 1.5 },
      "& .MuiAlert-message": { fontSize: 13 },
      "& .MuiButton-root": { fontSize: 12, minHeight: { xs: 44, sm: 0 } },
    }}>
      <Stack direction="row" alignItems="center" justifyContent="space-between"
        useFlexGap flexWrap="wrap" gap={1}>
        <Typography variant="h6" component="h2" sx={{ fontSize: 15 }}>Packet capture</Typography>
        <Typography variant="caption" color={recording ? "error.main" : "text.secondary"}>
          {offline ? "Board offline" : !status ? "Connecting…" : !status.available
            ? "Unavailable" : sdActive ? sdLabels[sdState] || "" : flipper
              ? "Flipper recording" : exporting ? "Copying to Flipper SD"
                : copied ? "Saved to SD" : labels[status.state]}
        </Typography>
      </Stack>
      {(status?.sd?.connected || sd || sdActive) && <Stack direction="row" gap={1}
        alignItems="center" sx={{ mt: 1 }}>
        <Typography variant="caption">Save to</Typography>
        <Select size="small" value={destination}
          inputProps={{ "aria-label": "Capture destination" }}
          onChange={event => setDestination(event.target.value)}
          disabled={Boolean(busy) || recording || saving || sdActive}
          sx={{ fontSize: 12, "& .MuiSelect-select": { py: 0.75 } }}>
          <MenuItem value="web">ESP32 RAM</MenuItem>
          <MenuItem value="flipper" disabled={!status?.sd?.connected}>
            Flipper SD (copy after stop)
          </MenuItem>
        </Select>
      </Stack>}
      {offline && <Alert severity="warning" sx={{ my: 1 }}>
        Cannot reach the board. Recording may still be running; reconnect to check.
      </Alert>}
      {status && !status.available && !offline && <Alert severity="info" sx={{ my: 1 }}>
        Web capture is unavailable. This feature requires compatible firmware and PSRAM.
      </Alert>}
      {flipper && !sd && <Alert severity="info" sx={{ my: 1 }}>
        The Flipper owns the current recording. Save it there before starting a web capture.
      </Alert>}
      {error && <Alert severity="error" sx={{ my: 1 }}>{error}</Alert>}
      {((status?.export_failed && !exporting && !copied) ||
        (exportCurrent && status?.export?.state === "failed")) &&
        <Alert severity="error" sx={{ my: 1 }}>
          Flipper SD copy failed. The ESP32 RAM file is still available; check the
          Flipper and retry.
        </Alert>}
      {sdActive ? <FlipperCapturePanel status={status?.sd} offline={offline} busy={busy}
        blocked={recording || saving} onStart={() => action("start")}
        onStop={() => action("stop")} />
        : <>
      {status?.reason && status.reason !== "Stopped by user." &&
        !status.reason.startsWith("Local demo:") &&
        <Alert severity={status.dropped ? "warning" : "info"} sx={{ my: 1 }}>
        {status.reason}
      </Alert>}
      {status && status.state !== "empty" && <Box sx={{ my: 1 }}>
        <Stack direction="row" useFlexGap flexWrap="wrap" gap={1} sx={{ mb: 1 }}>
          <Typography variant="body2">{status.packets.toLocaleString()} packets</Typography>
          <Typography variant="body2" color={status.dropped ? "warning.main" : "text.secondary"}>
            {status.dropped} dropped
          </Typography>
          <Typography variant="body2">{sizeLabel(status.bytes)} / {
            (status.capacity / 1048576).toFixed(0)} MiB</Typography>
        </Stack>
        <LinearProgress variant={saving ? "indeterminate" : "determinate"}
          aria-label="Capture buffer usage" value={100 * status.bytes / status.capacity} />
      </Box>}
      {exporting && <Typography variant="body2" color="text.secondary" sx={{ mt: 1 }}>
        Copying to Flipper SD: {sizeLabel(status.export.bytes)} / {sizeLabel(status.bytes)}
      </Typography>}
      {copied && !exporting && <Typography variant="body2" color="text.secondary" sx={{ mt: 1 }}>
        Saved on Flipper SD · {status.sd.path}
      </Typography>}
      <Stack direction="row" useFlexGap flexWrap="wrap" gap={1} sx={{ mt: 1 }}>
        {(!status || status.state === "empty") && <Button size="small" variant="contained"
          startIcon={<FiberManualRecordIcon />} disabled={disabled || flipper}
          onClick={() => action("start")}>
          {busy === "start" ? "Starting…" : "Start recording"}</Button>}
        {(recording || saving) && <Button size="small" variant="contained"
          disabled={disabled || saving}
          startIcon={saving ? <CircularProgress size={16} /> : <StopIcon />}
          onClick={() => action("stop")}>
          {saving ? "Saving…" : busy === "stop" ? "Stopping…" : "Stop & save"}</Button>}
        {ready && <>
          <Button size="small" variant="contained" startIcon={<DownloadIcon />} disabled={disabled}
            onClick={download}>{busy === "download" ? "Downloading…" : "Download PCAPNG"}</Button>
          {status?.sd?.connected && !copied && <Button size="small"
            disabled={disabled || !status.sd.available}
            onClick={() => action("export")}>Copy to Flipper SD</Button>}
          <Button size="small" disabled={disabled} onClick={() => setConfirmDiscard(true)}>
            Discard
          </Button>
        </>}
      </Stack>
      <Box component="details" sx={{ mt: 0.75, fontSize: 12 }}>
        <Box component="summary" sx={{ cursor: "pointer", color: "primary.main", py: 0.25 }}>
          Capture info
          <Box component="span" sx={{ color: "text.secondary", ml: 1 }}>
            · up to 4 MiB RAM
          </Box>
        </Box>
        <Typography variant="body2" color="text.secondary">
          Start recording before the session to include discovery. Saves PCAPNG for Wireshark.
          Stops when full. Refreshing is safe; Stop session does not stop recording.
          Download before powering off the ESP32.
          {sd && " Flipper SD copies after recording stops; keep both devices powered."}
        </Typography>
      </Box>
      </>}
      <Dialog open={confirmDiscard} onClose={() => setConfirmDiscard(false)}>
        <DialogTitle>Discard this capture?</DialogTitle>
        <DialogContent><DialogContentText>
          This removes the capture from ESP32 memory. Download it first if you want to keep it.
        </DialogContentText></DialogContent>
        <DialogActions>
          <Button onClick={() => setConfirmDiscard(false)}>Cancel</Button>
          <Button color="error" onClick={() => action("clear")}>Discard capture</Button>
        </DialogActions>
      </Dialog>
    </Box>
  );
}
