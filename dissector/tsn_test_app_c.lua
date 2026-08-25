-- SPDX-FileCopyrightText: Copyright 2025 Siemens AG
--
-- SPDX-License-Identifier: Apache-2.0

tsn_test_app_proto_C = Proto ("TSNAPP_C", "TSN Test App C")

local pf_total_packets = ProtoField.uint16("tsn_test_app_proto_C.pf_total_packets", "Total packets")
local pf_cycle_time    = ProtoField.uint16("tsn_test_app_proto_C.pf_cycle_time",    "Cycle time")
local pf_packet_number = ProtoField.uint16("tsn_test_app_proto_C.pf_packet_number", "Packet number")

local pf_tx_time_s     = ProtoField.uint16("tsn_test_app_proto_C.pf_tx_time_s",     "TX time s")
local pf_tx_time_ns    = ProtoField.uint16("tsn_test_app_proto_C.pf_tx_time_ns",    "TX time ns")
local pf_tx_time       = ProtoField.absolute_time("tsn_test_app_proto_C.tx_time",   "TX timestamp", base.UTC)

local orig_pf_tx_time_s     = ProtoField.uint16("tsn_test_app_proto_C.orig_pf_tx_time_s",     "original TX time s")
local orig_pf_tx_time_ns    = ProtoField.uint16("tsn_test_app_proto_C.orig_pf_tx_time_ns",    "original TX time ns")
local orig_pf_tx_time       = ProtoField.absolute_time("tsn_test_app_proto_C.orig_tx_time",   "original TX timestamp", base.UTC)

local orig_pf_rx_time_s     = ProtoField.uint16("tsn_test_app_proto_C.orig_pf_rx_time_s",     "original RX time s")
local orig_pf_rx_time_ns    = ProtoField.uint16("tsn_test_app_proto_C.orig_pf_rx_time_ns",    "original RX time ns")
local orig_pf_rx_time       = ProtoField.absolute_time("tsn_test_app_proto_C.orig_rx_time",   "original RX timestamp", base.UTC)

tsn_test_app_proto_C.fields = {pf_total_packets, pf_cycle_time, pf_packet_number, pf_tx_time, pf_tx_time_s, pf_tx_time_ns, orig_pf_tx_time, orig_pf_tx_time_s, orig_pf_tx_time_ns, orig_pf_rx_time, orig_pf_rx_time_s, orig_pf_rx_time_ns}

function tsn_test_app_proto_C.dissector (buf, pkt, root)
  if buf:len() == 0 then return end
  pkt.cols.protocol = tsn_test_app_proto_C.name

  local subtree = root:add(tsn_test_app_proto_C, buf(0))

  -- total_packets and cycle_time alternate every packet
  local packet_number = buf(4,4)
  if (packet_number:le_int() % 2 == 0) then
    subtree:add_le(pf_total_packets, buf(0,4))
  else
    subtree:add_le(pf_cycle_time, buf(0,4))
  end
  subtree:add_le(pf_packet_number, packet_number)
  subtree:append_text(", packet #" .. packet_number:le_int())
  pkt.cols.info:append("  PKT: ".. packet_number:le_int())

  local tx_time_s = buf(8,4)
  local tx_time_ns = buf(12,4)
  subtree:add(pf_tx_time, buf(8,8), NSTime.new(tx_time_s:le_int(), tx_time_ns:le_int()))
  subtree:add_le(pf_tx_time_s, tx_time_s)
  subtree:add_le(pf_tx_time_ns, tx_time_ns)

  local orig_tx_time_s = buf(16,4)
  local orig_tx_time_ns = buf(20,4)

  local orig_rx_time_s = buf(24,4)
  local orig_rx_time_ns = buf(28,4)

  --- the other two timestamps are only valid if not all values are 0x42 which we use as padding
  if ((orig_tx_time_s ~= orig_tx_time_ns) or (orig_rx_time_s ~= orig_rx_time_ns) or (orig_tx_time_ns ~= orig_rx_time_ns)) then
    subtree:add(orig_pf_tx_time, buf(16,8), NSTime.new(orig_tx_time_s:le_int(), orig_tx_time_ns:le_int()))
    subtree:add_le(orig_pf_tx_time_s, orig_tx_time_s)
    subtree:add_le(orig_pf_tx_time_ns, orig_tx_time_ns)

    subtree:add(orig_pf_rx_time, buf(24,8), NSTime.new(orig_rx_time_s:le_int(), orig_rx_time_ns:le_int()))
    subtree:add_le(orig_pf_rx_time_s, orig_rx_time_s)
    subtree:add_le(orig_pf_rx_time_ns, orig_rx_time_ns)
  end

end

function tsn_test_app_proto_C.init()
end

-- 56028 == 0xDADC
local eth_table = DissectorTable.get("ethertype")
eth_table:add(56028, tsn_test_app_proto_C)

