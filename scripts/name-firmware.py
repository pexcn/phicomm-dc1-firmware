Import('env')
from pathlib import Path
import shutil


def copy_firmware(source, target, env):
    output = Path(env.subst('$PROJECT_DIR')) / 'output'
    output.mkdir(exist_ok=True)
    shutil.copyfile(str(target[0]), output / (env.subst('$PIOENV') + '.bin'))


env.AddPostAction('$BUILD_DIR/${PROGNAME}.bin', copy_firmware)
