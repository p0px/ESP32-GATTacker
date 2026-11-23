export function messageKind(message) {
  const operation = String(message.operation || "").toUpperCase();
  if (/RSP|RESPONSE/.test(operation)) return "Responses";
  if (/NOTIFY|INDICATE/.test(operation)) return "Notifications";
  if (/READ|WRITE|EXEC/.test(operation)) return "Requests";
  return "Info";
}

export function parseSampledTraffic(value) {
  if (typeof value !== "string" || !value.startsWith("traffic\t")) return null;
  const fields = value.split("\t");
  if (fields.length < 7) return null;
  const [, code, direction, uuid, rawHandle, data, operation] = fields;
  const handle = Number(rawHandle);
  if (!/^[RWN]$/.test(code) || !Number.isInteger(handle) || handle < 0) return null;
  return { operation, direction, uuid, handle, data, sampled: true,
    msg: `[${operation}] ${direction} | Handle: ${handle} | UUID: ${uuid} | Data: ${data}` };
}

export function directionLabel(value) {
  return String(value || "").replace(/victim/gi, "central").replace(/->/g, "→");
}

export function handleLabel(value) {
  return Number.isInteger(value) ? `0x${value.toString(16).padStart(4, "0")}` : "—";
}

export function timeLabel(value) {
  if (!value) return "—";
  const date = new Date(value);
  return [date.getHours(), date.getMinutes(), date.getSeconds()]
    .map(part => String(part).padStart(2, "0")).join(":") +
    `.${String(date.getMilliseconds()).padStart(3, "0")}`;
}

export function payloadView(value) {
  if (value === undefined || value === null) return { hex: "Not included", ascii: "" };
  const text = String(value);
  if (!text) return { hex: "(empty)", ascii: "" };
  if (text.length % 2 || !/^[0-9a-f]+$/i.test(text)) return { hex: text, ascii: "" };
  const bytes = text.match(/../g);
  return { hex: bytes.join(" ").toUpperCase(), ascii: bytes.map(byte => {
    const code = parseInt(byte, 16);
    return code >= 32 && code <= 126 ? String.fromCharCode(code) : ".";
  }).join("") };
}

export function searchable(message) {
  return [message.msg, message.operation, directionLabel(message.direction), message.uuid,
    message.handle, handleLabel(message.handle), message.trans_id, message.data]
    .join(" ").toLowerCase();
}

export function exportMessage(message) {
  const { _uiId, _receivedAt, ...original } = message;
  return { ...original, sequence: _uiId,
    browser_received_at: _receivedAt ? new Date(_receivedAt).toISOString() : null };
}
