set pagination off
set max-value-size unlimited
ptype /o struct module_config_header
ptype /o struct module_config
ptype /o struct mc_buffer
ptype /o struct mc_ptrs
ptype /o struct uboot_config
ptype /o struct uboot_eth_config
ptype /o struct uboot_uart_config
ptype /o uboot_spi_config_t
ptype /o clock_config_t
ptype /o audio_config_t
ptype /o audio_srate_config_t
ptype /o chan_encodings_config_t
ptype /o device_config_t
ptype /o hmi_config_t
ptype /o struct label_config
ptype /o struct app_config
ptype /o struct aud_module_config_apec
ptype /o struct aud_mc_manf_version_config
ptype /o struct aud_mc_version
ptype /o struct aud_mc_id64
ptype /o struct aud_mc_friendly_name
info variables -q sentinel
info variables -q -n MC_
info functions -q -n ModuleConfig
info functions -q -n mc_buffer
disassemble mc_buffer_read_binary
