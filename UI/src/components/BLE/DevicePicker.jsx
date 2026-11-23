import { useState } from "react";
import { Box, Button, CircularProgress, Grid, Stack, TextField, Typography } from "@mui/material";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import { Device } from "../common";

export function DevicePicker({ board, onSelect }) {
  const [mac, setMac] = useState("");
  const scanning = board.state === 1 || board.pending === "scan";
  const results = [...board.results].sort((a, b) => b.rssi - a.rssi);
  if (scanning) return <Stack direction="row" alignItems="center" gap={1.5}>
    <CircularProgress size={18} aria-label="Scanning for devices" />
    <Typography>Scanning for devices…</Typography>
  </Stack>;
  if (board.state === 2) return <>
    <Stack direction="row" gap={1} sx={{ mb: 3 }}>
      <Button startIcon={<ArrowBackIcon />} onClick={() => board.send("stop")}
        disabled={!board.connected || Boolean(board.pending)}>Back</Button>
      <Button variant="contained" onClick={() => board.send("scan")}
        disabled={!board.connected || Boolean(board.pending)}>Scan again</Button>
    </Stack>
    <Typography sx={{ mb: 2 }}>Devices: {results.length}</Typography>
    {!results.length && <Typography color="text.secondary" sx={{ mb: 2 }}>
      No devices found. Check that the device is advertising, then scan again.
    </Typography>}
    <Grid container spacing={3}>
      {results.map(device => <Grid size={{ xs: 12, sm: 6, md: 4, lg: 3 }} key={device.mac}>
        <Device dev={device} onClick={() => onSelect(device)} />
      </Grid>)}
    </Grid>
  </>;
  return <Box sx={{ display: "flex", flexDirection: "column", gap: 2, maxWidth: 400 }}>
    <Button variant="contained" disabled={!board.connected}
      onClick={() => board.send("scan")}>Scan</Button>
    <Typography sx={{ textAlign: "center" }}>OR</Typography>
    <Box component="form" sx={{ display: "flex", gap: 1 }} onSubmit={event => {
      event.preventDefault();
      if (/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(mac))
        onSelect({ mac: mac.toUpperCase(), name: "Manual Device" });
    }}>
      <TextField label="MAC Address" placeholder="AA:BB:CC:DD:EE:FF" size="small" fullWidth
        value={mac} onChange={event => setMac(event.target.value.trim())}
        slotProps={{ htmlInput: { maxLength: 17 } }} />
      <Button type="submit" variant="contained" disabled={!board.connected ||
        !/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(mac)}>Select</Button>
    </Box>
  </Box>;
}
