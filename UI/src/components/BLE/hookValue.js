export function isHookValue(value) {
  return typeof value === "string" && value.length <= 1034 &&
    value.length % 2 === 0 && /^[0-9a-fA-F]*$/.test(value);
}
