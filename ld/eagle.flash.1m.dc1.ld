/* 1 MiB: firmware/OTA below 0xEA000; LittleFS at 0xEA000..0xF9FFF;
 * read-only legacy relay state at 0xFA000;
 * legacy configuration at 0xFB000; RF/SDK data at 0xFC000..0xFFFFF.
 * _FS_start also bounds the Arduino OTA staging area. Runtime files use a 64 KiB filesystem.
 */
MEMORY
{
  dport0_0_seg : org = 0x3FF00000, len = 0x10
  dram0_0_seg : org = 0x3FFE8000, len = 0x14000
  irom0_0_seg : org = 0x40201010, len = 0xE8FF0
}

PROVIDE ( _RELAY_STATE_start = 0x402FA000 );
PROVIDE ( _EEPROM_start = 0x402FB000 );
PROVIDE ( _FS_start = 0x402EA000 );
PROVIDE ( _FS_end = 0x402FA000 );
PROVIDE ( _FS_page = 0x100 );
PROVIDE ( _FS_block = 0x1000 );
PROVIDE ( _SPIFFS_start = 0x402EA000 );
PROVIDE ( _SPIFFS_end = 0x402FA000 );
PROVIDE ( _SPIFFS_page = 0x100 );
PROVIDE ( _SPIFFS_block = 0x1000 );

INCLUDE "local.eagle.app.v6.common.ld"
