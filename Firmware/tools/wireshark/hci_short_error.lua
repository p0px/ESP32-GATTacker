-- Bluetooth Core Vol 4 Part E 4.2 permits status-only error returns for Read Local Name.
-- Wireshark 4.4 and current upstream read 248 name bytes even when Status is an error.
-- Decode only that exact legal short reply; delegate everything else, including truncation.
-- Wrap before Bluetooth creates its private context; Lua cannot forward that context.
local captures = DissectorTable.get("wtap_encap")
local encapsulation = wtap.BLUETOOTH_H4_WITH_PHDR
local original = captures:get_dissector(encapsulation)
local short_error = Proto("gattacker_hci_error", "HCI Command Complete (short error)")
local event = ProtoField.uint8("gattacker_hci_error.event", "Event", base.HEX)
local length = ProtoField.uint8("gattacker_hci_error.length", "Parameter length", base.DEC)
local credits = ProtoField.uint8("gattacker_hci_error.credits", "Command credits", base.DEC)
local opcode = ProtoField.uint16("gattacker_hci_error.opcode", "Command opcode", base.HEX)
local status = ProtoField.uint8("gattacker_hci_error.status", "Status", base.HEX,
  {[1] = "Unknown HCI Command"})
short_error.fields = {event, length, credits, opcode, status}

function short_error.dissector(buffer, info, tree)
  if buffer:len() ~= 7 or buffer:reported_len() ~= 7 or buffer(0, 1):uint() ~= 4 or
      buffer(1, 1):uint() ~= 0x0e or buffer(2, 1):uint() ~= 4 or
      buffer(4, 2):le_uint() ~= 0x0c14 or buffer(6, 1):uint() == 0 then
    return original:call(buffer, info, tree)
  end
  info.cols.protocol = "HCI_EVT"
  info.cols.info = string.format("Read Local Name: error 0x%02x (legal status-only reply)",
    buffer(6, 1):uint())
  local node = tree:add(short_error, buffer())
  node:add(event, buffer(1, 1))
  node:add(length, buffer(2, 1))
  node:add(credits, buffer(3, 1))
  node:add_le(opcode, buffer(4, 2))
  node:add(status, buffer(6, 1))
  node:add(buffer(6, 1), "Error return: optional Local_Name field is absent; bytes unchanged")
  return buffer:len()
end

captures:set(encapsulation, short_error)
