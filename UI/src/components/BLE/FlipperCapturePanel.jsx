import { Alert, Button, CircularProgress, Stack, Typography } from "@mui/material";
import FiberManualRecordIcon from "@mui/icons-material/FiberManualRecord";
import StopIcon from "@mui/icons-material/Stop";

export function FlipperCapturePanel({ status, offline, busy, blocked, onStart, onStop }) {
  const active = ["starting", "recording", "saving"].includes(status?.state);
  const waiting = status?.state === "starting" || status?.state === "saving";
  const connected = status?.connected && !offline;
  return (
    <Stack gap={1} sx={{ mt: 1 }}>
      {!connected && <Alert severity="warning">
        Open GATTacker on the connected Flipper. Recording status is unconfirmed until it replies.
      </Alert>}
      {connected && !status.available && <Alert severity="warning">
        Flipper SD card unavailable. Insert a working card before recording.
      </Alert>}
      {status?.error && <Alert severity="warning">{status.error}</Alert>}
      {blocked && <Typography variant="body2">
        Save the ESP32 recording before starting on SD.
      </Typography>}
      {status?.id > 0 && <>
        <Stack direction="row" useFlexGap flexWrap="wrap" gap={1}>
          <Typography variant="body2">{status.packets.toLocaleString()} packets</Typography>
          <Typography variant="body2" color={status.dropped ? "warning.main" : "text.secondary"}>
            {status.dropped} dropped
          </Typography>
          <Typography variant="body2">{(status.bytes / 1024).toFixed(1)} KiB</Typography>
        </Stack>
        {status.path && <Typography variant="caption" sx={{ overflowWrap: "anywhere" }}>
          {status.state === "ready" && status.complete ? "Saved to " : "File: "}{status.path}
        </Typography>}
      </>}
      <Stack direction="row" gap={1}>
        <Button size="small" variant="contained"
          startIcon={waiting ? <CircularProgress size={16} /> : active ? <StopIcon />
            : <FiberManualRecordIcon />}
          disabled={!connected || Boolean(busy) || waiting ||
            (!active && (!status.available || blocked))}
          onClick={active ? onStop : onStart}>
          {status?.state === "starting" ? "Starting…" : status?.state === "saving" ? "Saving…"
            : active ? "Stop & save" : "Start recording"}
        </Button>
      </Stack>
      <Typography variant="caption" color="text.secondary">
        Records to Flipper SD. Keep GATTacker open. Retrieve saved PCAPNG files with qFlipper.
      </Typography>
    </Stack>
  );
}
