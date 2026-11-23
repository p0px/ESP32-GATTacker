import {
  Alert, Box, Button, Dialog, DialogActions, DialogContent, DialogContentText, DialogTitle,
} from "@mui/material";
import { DevicePicker } from "./DevicePicker";
import { DeviceSession } from "./DeviceSession";
import { useBoard } from "./BoardSessionProvider";

export function GattAttackApp() {
  const board = useBoard();
  const target = board.selectedMac ? board.results.find(device =>
    device.mac.toUpperCase() === board.selectedMac) ||
    { mac: board.selectedMac, name: "Selected target" } : null;
  return (
    <Box>
      {board.error && <Alert severity="error" sx={{ mb: 2 }}>{board.error}</Alert>}
      {board.state < 0 ? <Box role="status">Waiting for board status…</Box> :
        (target && board.state !== 1) || board.state >= 3 ?
        <DeviceSession board={board} target={target} onBack={() => board.select(null)} /> :
        <DevicePicker board={board} onSelect={board.select} />}
      <Dialog open={Boolean(board.confirmation)} onClose={board.cancel}
        aria-labelledby="session-confirm-title" aria-describedby="session-confirm-description">
        <DialogTitle id="session-confirm-title">r u sure?</DialogTitle>
        <DialogContent><DialogContentText id="session-confirm-description">
          {board.confirmation?.action === "start"
            ? `Start a session with ${board.confirmation.fields.mac}?`
            : "Stop the Bluetooth session? Packet recording will keep running."}
        </DialogContentText></DialogContent>
        <DialogActions>
          <Button autoFocus onClick={board.cancel}>Cancel</Button>
          <Button variant="contained" disabled={!board.connected} onClick={board.confirm}>
            {board.confirmation?.action === "start" ? "Start session" : "Stop"}
          </Button>
        </DialogActions>
      </Dialog>
    </Box>
  );
}

export default GattAttackApp;
