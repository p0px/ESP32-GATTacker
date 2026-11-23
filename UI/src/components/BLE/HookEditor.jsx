import { useState } from "react";
import { Box, Typography } from "@mui/material";
import { isHookValue } from "./hookValue";

const defaultRead = `(function hookRead({ uuid, value }) {
  // your code here
  return value;
})`;
const defaultWrite = `(function hookWrite({ uuid, value }) {
  // your code here
  return value;
})`;

// Preserve the upstream browser hook contract and live validation behavior.
if (!window.hook_read) window.hook_read = eval(defaultRead);
if (!window.hook_write) window.hook_write = eval(defaultWrite);

export function HookEditor({ hook = "read" }) {
  const [error, setError] = useState("");
  const [code, setCode] = useState(window[`hook_${hook}`]?.toString() ||
    (hook === "read" ? defaultRead : defaultWrite));
  const label = hook === "read" ? "Read Hook" : "Write Hook";
  const handleChange = event => {
    setCode(event.target.value);
    try {
      const candidate = eval(`(${event.target.value})`);
      if (typeof candidate !== "function") throw new Error("Expected a function");
      if (!isHookValue(candidate({ uuid: "test", value: "01020304" })))
        throw new Error("Return an even-length hex string, up to 517 bytes; empty is allowed");
      window[`hook_${hook}`] = candidate;
      setError("");
    } catch (failure) {
      setError(`Invalid function ${failure}`);
    }
  };
  return <Box sx={{ minWidth: 0 }}>
    <Box component="textarea" aria-label={`${label} code`} aria-invalid={Boolean(error)}
      aria-describedby={`${hook}-hook-feedback`} value={code} onChange={handleChange}
      rows={7} wrap="off" spellCheck={false} autoCapitalize="off" autoCorrect="off"
      sx={{ display: "block", width: "100%", boxSizing: "border-box", p: 1.5,
        minHeight: 140, maxHeight: 360, resize: "vertical", overflow: "auto",
        bgcolor: "background.default", color: "text.primary", fontFamily: "inherit",
        fontSize: 12, lineHeight: 1.6, borderRadius: 1, border: "1px solid",
        borderColor: error ? "error.main" : "divider",
        "&:focus-visible": { outline: "2px solid", outlineColor: "primary.main",
          outlineOffset: 1 } }} />
    <Typography id={`${hook}-hook-feedback`} variant="caption" display="block"
      color={error ? "error.main" : "text.secondary"} sx={{ mt: 1, overflowWrap: "anywhere" }}>
      {error ? `${error}. Last valid hook remains active.` : "Edits apply when valid."}
    </Typography>
  </Box>;
}
