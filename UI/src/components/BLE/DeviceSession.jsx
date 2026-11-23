import { useState } from "react";
import { Button, Stack, Tooltip, Typography } from "@mui/material";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import { MitmPanel } from "./MitmPanel";
import { PacketCapture } from "./PacketCapture";

const states = { 3: "Starting", 4: "Connecting", 5: "Connect failed", 6: "Cloning", 8: "Failed" };

function sessionLabel(board) {
  if (!board.connected) return "Board disconnected · session status unavailable";
  if (board.state !== 7) return states[board.state] || "Unknown";
  const { targetConnected, centralConnected } = board.session;
  if (targetConnected === false) return "Target disconnected";
  if (targetConnected === true && centralConnected === false) return "Waiting for central";
  if (targetConnected === true && centralConnected === true) return "Connected";
  return "Session running · connection status unavailable";
}

function linkLabel(connected) {
  if (typeof connected !== "boolean") return "Unknown";
  return connected ? "Connected" : "Disconnected";
}

function activeDevice({ session, results }, selected) {
  if (!session.mac) return { mac: "Unavailable", name: "Active target" };
  const matches = device => device?.mac?.toUpperCase() === session.mac;
  const metadata = results.find(matches) || (matches(selected) ? selected : null);
  return { ...metadata, mac: session.mac };
}

export function DeviceSession({ board, target, onBack }) {
  const [captureHeld, setCaptureHeld] = useState(false);
  const [captureState, setCaptureState] = useState("empty");
  const active = board.state >= 3;
  const device = active ? activeDevice(board, target) : target;
  const pending = Boolean(board.pending);
  return (
    <>
      <Stack direction="row" flexWrap="wrap" gap={1} sx={{ mb: 2,
        "& .MuiButton-root": { minHeight: { xs: 44, sm: 0 } } }}>
        <Tooltip title={active ? "Stop the session before going back" : captureHeld
          ? "Download and discard the capture before going back" : "Back to devices"}>
          <span><Button startIcon={<ArrowBackIcon />} onClick={onBack}
            disabled={active || pending || captureHeld}>Back</Button></span>
        </Tooltip>
        {active ? <Button variant="contained" disabled={pending || !board.connected}
          onClick={() => board.send("stop")}>
          {board.pending === "stop" ? "Stopping…" : "Stop"}
        </Button> : <Button variant="contained"
          disabled={!target || pending || !board.connected || board.state < 0}
          onClick={() => board.send("start", { mac: target.mac })}>
          {board.pending === "start" ? "Starting…" : "Start session"}
        </Button>}
      </Stack>
      {active && <Stack component="section" aria-label="Session status" gap={0.25} sx={{ mb: 2 }}>
        <Typography>{sessionLabel(board)}</Typography>
        {board.connected && <Typography variant="body2" color="text.secondary">
          Central: {linkLabel(board.session.centralConnected)}
          {" · "}Target: {linkLabel(board.session.targetConnected)}
        </Typography>}
      </Stack>}
      {captureHeld && !active && <Typography variant="caption" color="text.secondary"
        sx={{ display: "block", mb: 2 }}>
        Finish and discard the capture before going back. Download it first to keep a copy.
      </Typography>}
      {board.hookError && <Typography color="error" sx={{ mb: 2 }}>{board.hookError}</Typography>}
      <MitmPanel target={device}
        messages={board.messages} setMessages={board.clearMessages}
        hooksEnabled={board.hooksEnabled} captureState={captureState} connected={board.connected}>
        <PacketCapture onRetainedChange={setCaptureHeld} onStateChange={setCaptureState} />
      </MitmPanel>
    </>
  );
}
