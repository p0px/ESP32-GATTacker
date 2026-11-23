import { memo } from "react";
import { Box, ButtonBase, Stack, Typography } from "@mui/material";
import { directionLabel, handleLabel, payloadView, timeLabel } from "./messageFormat";

export const MessageRow = memo(function MessageRow({ message, top, height, onInspect }) {
  const data = message.data === undefined ? undefined : String(message.data);
  const payload = payloadView(data?.slice(0, 96));
  const preview = data !== undefined ? `Data: ${payload.hex}${data.length > 96 ? "…" : ""}`
    : "No payload in this event";
  const failed = message.status !== undefined && Number(message.status) !== 0;
  return <ButtonBase aria-label={`Inspect message ${message._uiId}`}
    data-message-id={message._uiId} onClick={() => onInspect(message)}
    sx={{ position: "absolute", top, height, width: "100%", px: 1.5, py: 0.5,
      display: "block", textAlign: "left", borderBottom: "1px solid", borderColor: "divider",
      "&:hover, &:focus-visible": { bgcolor: "action.hover" } }}>
    <Stack direction="row" gap={1} alignItems="center">
      <Typography component="time" dateTime={new Date(message._receivedAt).toISOString()}
        sx={{ fontSize: 12, flexShrink: 0 }} color="text.secondary">
        {timeLabel(message._receivedAt)}
      </Typography>
      <Typography noWrap sx={{ fontSize: 12, fontWeight: 700, flex: 1, minWidth: 0 }}
        color={failed ? "error.main" : "primary.main"}>
        {message.operation || "INFO"}
      </Typography>
      {message.direction && <Typography noWrap sx={{ fontSize: 12,
        display: { xs: "none", sm: "block" } }}>{directionLabel(message.direction)}</Typography>}
      {failed && <Typography sx={{ fontSize: 12, flexShrink: 0 }} color="error.main">
        Status {message.status}
      </Typography>}
    </Stack>
    {message.operation ? <Stack direction={{ xs: "column", sm: "row" }}
      gap={{ xs: 0, sm: 1 }} sx={{ minWidth: 0 }}>
      <Typography noWrap color="text.secondary"
        sx={{ fontSize: 12, flexShrink: 1, maxWidth: { xs: "100%", sm: "60%" } }}>
        <Box component="span" sx={{ display: { xs: "inline", sm: "none" }, color: "text.primary" }}>
          {directionLabel(message.direction)} · </Box>
        H {handleLabel(message.handle)} · TX {message.trans_id ?? "—"}
        <Box component="span" sx={{ display: { xs: "none", sm: "inline" } }}>
          {` · ${message.uuid || "—"}`}
        </Box>
      </Typography>
      <Typography noWrap sx={{ fontSize: 13, minWidth: 0, flex: { sm: 1 } }}>
        {preview}
      </Typography>
    </Stack> : <Box sx={{ fontSize: 13, lineHeight: 1.5, overflow: "hidden",
      overflowWrap: "anywhere", display: "-webkit-box", WebkitLineClamp: { xs: 2, sm: 1 },
      WebkitBoxOrient: "vertical" }}>{message.msg}</Box>}
  </ButtonBase>;
});
