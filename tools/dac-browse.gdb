set pagination off
set max-value-size unlimited
set print pretty on
ptype /o DAC_PARAM_BROWSE_INFO
ptype /o struct tag_browse_info_t
ptype /o BROWSE_LIST
ptype /o REBROWSE_WATCH
info types BROWSE_STAGE
info types BROWSE_
info types DAC_BROWSE
info types tagDacParamBrowse
disassemble /s notifyBrowseComplete
disassemble /s dacCmdParamConvBrowseComplete
disassemble /s set_BROWSE_RESTART
disassemble /s set_BROWSE_RESTART_ALL
disassemble /s get_BROWSE_COMPLETE
disassemble /s get_BROWSE_COMPLETE_ALL
disassemble /s dacBrowseRestart
disassemble /s dacBrowseStart
disassemble /s set_BROWSE_REBUILD_DEV_LIST
disassemble /s rebrowseCheckDeviceProc
