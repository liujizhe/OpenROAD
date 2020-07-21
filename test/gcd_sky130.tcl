# gcd flow pipe cleaner
source "helpers.tcl"
source "sky130/sky130.vars"

set synth_verilog "gcd_sky130.v"
set design "gcd"
set top_module "gcd"
set sdc_file "gcd_sky130.sdc"
set init_floorplan_cmd "initialize_floorplan -site $site \
    -die_area {0 0 299.96 300.128} \
    -core_area {9.996 10.08 289.964 290.048} \
    -tracks $tracks_file"
set max_drv_count 4

source -echo "flow.tcl"
