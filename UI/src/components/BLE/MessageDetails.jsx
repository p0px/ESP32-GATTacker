import {
  Box, Button, Dialog, DialogActions, DialogContent, DialogTitle, Typography,
} from "@mui/material";
import { directionLabel, exportMessage, payloadView, timeLabel } from "./messageFormat";

export function MessageDetails({ message, onClose, onCopy }) {
  if (!message) return null;
  const payload = payloadView(message.data);
  return <Dialog open fullWidth maxWidth="sm" onClose={onClose}>
    <DialogTitle>Message #{message._uiId}</DialogTitle>
    <DialogContent sx={{ overflowWrap: "anywhere" }}>
      <Typography>{timeLabel(message._receivedAt)} · received locally</Typography>
      <Typography sx={{ my: 1 }}>{message.operation || "Info"}
        {message.direction && ` · ${directionLabel(message.direction)}`}</Typography>
      {message.operation && <>
        <Typography variant="body2">Payload / hex</Typography>
        <Box component="pre" sx={{ whiteSpace: "pre-wrap", fontSize: 13 }}>{payload.hex}</Box>
        {payload.ascii && <><Typography variant="body2">ASCII</Typography>
          <Box component="pre" sx={{ whiteSpace: "pre-wrap", fontSize: 13 }}>
            {payload.ascii}
          </Box></>}
      </>}
      <Typography variant="body2">Full event</Typography>
      <Box component="pre" sx={{ whiteSpace: "pre-wrap", fontSize: 12 }}>
        {JSON.stringify(exportMessage(message), null, 2)}
      </Box>
    </DialogContent>
    <DialogActions>
      <Button onClick={() => onCopy([message])}>Copy</Button>
      <Button onClick={onClose}>Close</Button>
    </DialogActions>
  </Dialog>;
}
