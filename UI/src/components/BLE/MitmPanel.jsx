import { memo, useState } from "react";
import {
  Accordion, AccordionDetails, AccordionSummary, Box, Grid, Paper,
  Stack, Typography, useMediaQuery, useTheme,
} from "@mui/material";
import BluetoothIcon from "@mui/icons-material/Bluetooth";
import ExpandMoreIcon from "@mui/icons-material/ExpandMore";
import SignalCellularAltIcon from "@mui/icons-material/SignalCellularAlt";
import { HookControls } from "./HookControls";
import { MessagesPanel } from "./MessagesPanel";

export const MitmPanel = memo(function MitmPanel({ target, messages, setMessages,
  hooksEnabled = false, children, captureState, connected }) {
  const desktop = useMediaQuery(useTheme().breakpoints.up("lg"));
  const [controlsOpen, setControlsOpen] = useState(false);
  return <Grid container spacing={2} sx={{ alignItems: "start" }}>
    <Grid size={{ xs: 12, lg: 8 }} sx={{ minWidth: 0 }}>
      <MessagesPanel messages={messages} onClear={() => setMessages([])} connected={connected} />
    </Grid>
    <Grid size={{ xs: 12, lg: 4 }} sx={{ minWidth: 0, order: { xs: -1, lg: 1 } }}>
      <Accordion expanded={desktop || controlsOpen} disableGutters
        onChange={(_, expanded) => setControlsOpen(expanded)}
        sx={{ bgcolor: "transparent", backgroundImage: "none", boxShadow: "none",
          "&::before": { display: "none" } }}>
        <AccordionSummary expandIcon={<ExpandMoreIcon />} aria-label="Target and capture controls"
          sx={{ display: { xs: "flex", lg: "none" }, px: 1 }}>
          <Box sx={{ minWidth: 0 }}>
            <Typography>Target &amp; capture</Typography>
            <Typography variant="caption" sx={{ overflowWrap: "anywhere" }}>
              {target.name ? `${target.name} · ` : ""}{target.mac}
            </Typography>
          </Box>
          {captureState && captureState !== "empty" && <Typography variant="caption"
            sx={{ ml: 1 }} color={captureState === "recording" ? "error.main" : "text.secondary"}>
            {captureState === "ready" ? "Capture ready" : captureState}
          </Typography>}
        </AccordionSummary>
        <AccordionDetails sx={{ p: 0 }}>
          <Paper variant="outlined" sx={{ overflow: "hidden", backgroundImage: "none" }}>
            <Box component="section" aria-label="Target" sx={{ p: 2 }}>
              <Stack direction="row" alignItems="center" justifyContent="space-between" gap={1}>
                <Stack direction="row" alignItems="center" gap={1}>
                  <BluetoothIcon sx={{ fontSize: 18 }} color="primary" />
                  <Typography component="h2" sx={{ fontSize: 15, fontWeight: 500 }}>
                    Target
                  </Typography>
                </Stack>
                {Number.isFinite(target.rssi) && <Stack direction="row" alignItems="center"
                  gap={0.5} sx={{ color: "text.secondary", flexShrink: 0 }}>
                  <SignalCellularAltIcon sx={{ fontSize: 16 }} />
                  <Typography variant="caption" title="RSSI at scan time">
                    {target.rssi} dBm
                  </Typography>
                </Stack>}
              </Stack>
              <Typography sx={{ mt: 1.5, fontSize: 15, overflowWrap: "anywhere" }}>
                {target.name || "Unnamed device"}
              </Typography>
              <Typography variant="body2" color="text.secondary"
                sx={{ mt: 0.5, fontSize: 12, overflowWrap: "anywhere" }}>
                MAC: {target.mac}
              </Typography>
            </Box>
            <Box sx={{ borderTop: 1, borderColor: "divider" }}>{children}</Box>
            <HookControls enabled={hooksEnabled} connected={connected} />
          </Paper>
        </AccordionDetails>
      </Accordion>
    </Grid>
  </Grid>;
});
