set pagination off
set print pretty on
disassemble /s get_DEVICE_CLOCK_STATUS
disassemble /s get_ROUTING_PATCHES
disassemble /s dacCmdParamConvPatches
disassemble /s dacCmdParamConvPatchesWithResult
disassemble /s dacCmdParamConvPatch
ptype /o DAC_PARAM_FLOW
ptype /o DAC_PARAM_ID_SET
ptype /o DAC_PARAM_TX_CHANNEL
ptype /o DAC_PARAM_BATCH_SUBSCRIBE
ptype /o DAC_PARAM_BATCH_SUBSCRIBE_WITH_RESULT
