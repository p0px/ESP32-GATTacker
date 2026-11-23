import { memo, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import {
  Alert, Box, Button, Dialog, DialogActions, DialogContent, DialogTitle, MenuItem,
  Paper, Stack, TextField, Typography, useMediaQuery, useTheme,
} from "@mui/material";
import { MessageDetails } from "./MessageDetails";
import { MessageRow } from "./MessageRow";
import { exportMessage, messageKind, searchable } from "./messageFormat";

const lastId = messages => messages.at(-1)?._uiId || 0;

export const MessagesPanel = memo(function MessagesPanel({ messages, onClear, connected }) {
  const [snapshot, setSnapshot] = useState(null);
  const [query, setQuery] = useState("");
  const [kind, setKind] = useState("All");
  const [selected, setSelected] = useState(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [height, setHeight] = useState(360);
  const [copyStatus, setCopyStatus] = useState("");
  const [manualCopy, setManualCopy] = useState("");
  const viewport = useRef(null);
  const copyInput = useRef(null);
  const mobile = useMediaQuery(useTheme().breakpoints.down("sm"));
  const rowHeight = mobile ? 72 : 52;
  const paused = snapshot !== null;
  const source = snapshot || messages;
  const unseen = paused ? Math.max(0, lastId(messages) - lastId(snapshot)) : 0;
  const filtered = useMemo(() => source.filter(message =>
    (kind === "All" || messageKind(message) === kind) &&
    searchable(message).includes(query.toLowerCase())), [source, kind, query]);

  useEffect(() => {
    const observer = new ResizeObserver(entries => setHeight(entries[0].contentRect.height));
    observer.observe(viewport.current);
    return () => observer.disconnect();
  }, []);

  useLayoutEffect(() => {
    if (!paused && viewport.current) {
      viewport.current.scrollTop = viewport.current.scrollHeight;
      setScrollTop(viewport.current.scrollTop);
    }
  }, [filtered, paused, rowHeight, height]);

  const pause = () => { if (!paused) setSnapshot(messages); };
  const inspect = message => { pause(); setSelected(message); };
  const changeFilter = (setter, value) => {
    pause();
    setter(value);
    viewport.current.scrollTop = 0;
    setScrollTop(0);
  };
  const clear = () => {
    onClear();
    setSnapshot(null);
    setSelected(null);
    setCopyStatus("");
    viewport.current.scrollTop = 0;
    setScrollTop(0);
  };
  const copy = async items => {
    const text = JSON.stringify(items.map(exportMessage), null, 2);
    try {
      if (!navigator.clipboard || !window.isSecureContext) throw new Error("Manual copy needed");
      await navigator.clipboard.writeText(text);
      setCopyStatus(`Copied ${items.length} message${items.length === 1 ? "" : "s"}.`);
    } catch {
      // The board serves HTTP: offer selectable text instead of a deprecated clipboard API.
      setManualCopy(text);
    }
  };
  const start = Math.max(0, Math.floor(scrollTop / rowHeight) - 3);
  const end = Math.min(filtered.length, start + Math.ceil(height / rowHeight) + 6);
  return <Box component="section" aria-label="Messages" sx={{ minWidth: 0,
    "& .MuiButton-root, & .MuiInputBase-root": { minHeight: { xs: 44, sm: 0 } } }}>
    <Stack direction="row" alignItems="center" flexWrap="wrap" gap={0.5} sx={{ mb: 1 }}>
      <Typography variant="h6" component="h2" sx={{ mr: "auto" }}>Messages</Typography>
      <Button size="small" onClick={() => paused ? setSnapshot(null) : pause()}>
        {paused ? "Resume live" : "Pause"}
      </Button>
      <Button size="small" disabled={!source.length} onClick={clear}>Clear</Button>
      <Button size="small" disabled={!filtered.length} onClick={() => copy(filtered)}>Copy</Button>
    </Stack>
    <Stack direction="row" gap={1} sx={{ mb: 1 }}>
      <TextField label="Search messages" size="small" value={query}
        placeholder="UUID, handle, TX, data…" sx={{ flex: 1, minWidth: 0 }}
        onChange={event => changeFilter(setQuery, event.target.value)} />
      <TextField select label="Show" size="small" value={kind} sx={{ minWidth: 112 }}
        onChange={event => changeFilter(setKind, event.target.value)}>
        {["All", "Requests", "Responses", "Notifications", "Info"].map(option =>
          <MenuItem key={option} value={option}>{option}</MenuItem>)}
      </TextField>
    </Stack>
    <Stack direction="row" justifyContent="space-between" gap={1} sx={{ mb: 1 }}>
      <Typography variant="caption" color={paused ? "warning.main" : "text.secondary"}>
        {paused ? `Paused · ${unseen} new` : connected ? "Live" : "Board offline"}
      </Typography>
      <Typography variant="caption" color="text.secondary">
        {filtered.length} shown / {source.length} retained
      </Typography>
    </Stack>
    <Paper ref={viewport} role="region" aria-label="Message list" tabIndex={0}
      onScroll={event => {
        const element = event.currentTarget;
        setScrollTop(element.scrollTop);
        if (!paused && element.scrollHeight - element.scrollTop - element.clientHeight > 20)
          pause();
      }} sx={{ height: { xs: "52dvh", md: "62dvh" }, minHeight: 280, maxHeight: 650,
        overflow: "auto", overflowAnchor: "none", overscrollBehavior: "contain" }}>
      {filtered.length ? <Box sx={{ height: filtered.length * rowHeight, position: "relative" }}>
        {filtered.slice(start, end).map((message, index) => <MessageRow key={message._uiId}
          message={message} top={(start + index) * rowHeight} height={rowHeight}
          onInspect={inspect} />)}
      </Box> : <Typography color="text.secondary" sx={{ p: 2, fontSize: 13 }}>
        {source.length ? "No matching messages. Adjust the search or filter."
          : "Waiting for requests and responses."}
      </Typography>}
    </Paper>
    <Typography variant="caption" color="text.secondary" display="block" sx={{ mt: 1 }}>
      Local receive times · Sampled live preview · Latest 1,000 events · Tap a row for details.
      {paused && " Receiving and packet capture continue while paused."}
    </Typography>
    {copyStatus && <Alert severity="success" onClose={() => setCopyStatus("")} sx={{ mt: 1 }}>
      {copyStatus}
    </Alert>}
    <MessageDetails message={selected} onClose={() => setSelected(null)} onCopy={copy} />
    <Dialog open={Boolean(manualCopy)} onClose={() => setManualCopy("")} fullWidth maxWidth="sm">
      <DialogTitle>Copy messages</DialogTitle>
      <DialogContent>
        <Typography sx={{ mb: 2 }}>
          Select the text, then copy using your device’s copy command.
        </Typography>
        <TextField multiline fullWidth minRows={6} maxRows={16} label="Messages JSON"
          value={manualCopy} inputRef={copyInput} slotProps={{ input: { readOnly: true } }} />
      </DialogContent>
      <DialogActions>
        <Button onClick={() => { copyInput.current.focus(); copyInput.current.select(); }}>
          Select all
        </Button>
        <Button onClick={() => setManualCopy("")}>Close</Button>
      </DialogActions>
    </Dialog>
  </Box>;
});
