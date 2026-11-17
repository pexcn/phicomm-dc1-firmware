"""Keep the pinned libm's word-accessed range-reduction tables in flash."""
from pathlib import Path

Import('env')


def flash_math_tables(target, source, env):
    # These two Newlib objects contain four aligned int32/float tables (976 B).
    # Their readers use L32I, which can read ESP8266's memory-mapped flash.
    # Keep the functions, table contents and SDK files unchanged.
    script = Path(str(target[0]))
    content = script.read_text()
    anchor = '    *libm.a:(.literal .text .literal.* .text.*)'
    if content.count(anchor) != 1:
        raise RuntimeError('SDK libm linker rule changed; review flash table placement')
    content = content.replace(anchor, anchor + '\n'
        '    *libm.a:lib_a-ef_rem_pio2.o(.rodata)\n'
        '    *libm.a:lib_a-kf_rem_pio2.o(.rodata)')
    script.write_text(content)


common = '$BUILD_DIR/ld/local.eagle.app.v6.common.ld'
env.Depends(common, '$PROJECT_DIR/scripts/flash-math.py')
env.AddPostAction(common, env.VerboseAction(flash_math_tables, 'Placing libm tables in flash'))
