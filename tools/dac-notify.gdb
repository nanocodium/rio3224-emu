set pagination off
set max-value-size unlimited
x/8xb 0xb0a90
disassemble /s dacCmdSendStartNotify
disassemble /s dacCmdSendManfNotify
disassemble /s dacCmdSendClockStateNotify
disassemble /s sendNotifyCommand
disassemble /s dacModeNotify
disassemble /s dacDeviceNotifyDeviceConnect
disassemble /s dacDeviceNotifyBrowseComplete
disassemble /s notifyRestart
disassemble /s dacCmdSendNotify
ptype /o DEVICE_INFO
info functions dac_main
info functions ^main$
info functions dacMain
