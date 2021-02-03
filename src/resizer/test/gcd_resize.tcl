# gcd full meal deal
read_liberty Nangate45/Nangate45_typ.lib
read_lef Nangate45/Nangate45.lef
read_def gcd_placed.def
read_sdc gcd.sdc

set_wire_rc -layer metal3
estimate_parasitics -placement

report_worst_slack

set_dont_use {AOI211_X1 OAI211_X1}


#Common
set db [ord::get_db]
set chip [$db getChip]
set block [$chip getBlock]
#End Common


#Journaling
odb::dbDatabase_beginEco $block
# #
#Begin Updates

buffer_ports
repair_design
repair_tie_fanout LOGIC0_X1/Z
repair_tie_fanout LOGIC1_X1/Z
repair_timing -setup
repair_timing -hold -slack_margin .2

# End Updates
# # 
odb::dbDatabase_endEco $block
odb::writeEco $block results/gcd_resize.journal
#End Journaling


report_checks -path_delay min_max 
report_check_types -max_slew -max_fanout -max_capacitance
report_worst_slack
report_long_wires 10
report_floating_nets -verbose
report_design_area


