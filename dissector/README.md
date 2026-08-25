# Wireshark dissector for TSN Test App

## Installation

1. Open Wireshark and navigate to Help → About Wireshark → Folders.
2. Double-click on "Personal Lua Plugins". Agree to create the folder if it doesn't exist.
3. Copy `tsn_test_app.lua` to the folder opened on a previous step.
4. Open a `pcap` file with test app payload, the fields should now be extracted.

## Extracted fields
- `total_packets` 
- `cycle_time`    
- `packet_number` 
- `tx_time_s`     
- `tx_time_ns`    
- `tx_time` (Epoch converted)   

