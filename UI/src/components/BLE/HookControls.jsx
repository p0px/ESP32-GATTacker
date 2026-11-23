import {
  Accordion, AccordionDetails, AccordionSummary, Box, FormControlLabel, Switch, Typography,
} from "@mui/material";
import CodeIcon from "@mui/icons-material/Code";
import ExpandMoreIcon from "@mui/icons-material/ExpandMore";
import { HookEditor } from "./HookEditor";
import { useBoard } from "./BoardSessionProvider";

export function HookControls({ enabled, connected }) {
  const { hooksPending, toggleHooks } = useBoard();
  const waiting = hooksPending !== null;
  return <Box component="section" aria-label="Hooks" sx={{ borderTop: 1, borderColor: "divider" }}>
    <FormControlLabel label="Enable Hooks" labelPlacement="start" sx={{ m: 0, px: 2, py: 0.5,
      display: "flex", justifyContent: "space-between", "& .MuiTypography-root": { fontSize: 15 } }}
      control={<Switch checked={enabled} disabled={!connected || waiting}
        onChange={event => toggleHooks(event.target.checked)} />} />
    {waiting && <Typography role="status" variant="caption" color="text.secondary"
      sx={{ display: "block", px: 2, pb: 1 }}>
      {hooksPending ? "Enabling hooks…" : "Disabling hooks…"}
    </Typography>}
    {enabled && <Typography variant="caption" color="text.secondary"
      sx={{ display: "block", px: 2, pb: 1.5 }}>
      Hooks run in this browser and reset on page reload.
    </Typography>}
    {enabled && ["read", "write"].map(hook => {
      const label = hook === "read" ? "Read Hook" : "Write Hook";
      return <Accordion key={hook} disableGutters elevation={0}
        sx={{ bgcolor: "transparent", backgroundImage: "none",
          "&::before": { display: "none" }, borderTop: 1, borderColor: "divider" }}>
        <AccordionSummary expandIcon={<ExpandMoreIcon />} aria-label={label}
          sx={{ px: 2, minHeight: 48, "& .MuiAccordionSummary-content": { gap: 1 } }}>
          <CodeIcon sx={{ fontSize: 18, color: "text.secondary" }} />
          <Typography sx={{ fontSize: 13 }}>{label}</Typography>
        </AccordionSummary>
        <AccordionDetails sx={{ px: 2, pt: 0, pb: 2 }}><HookEditor hook={hook} /></AccordionDetails>
      </Accordion>;
    })}
  </Box>;
}
