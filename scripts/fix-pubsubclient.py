"""Correct the signed length declaration in pinned PubSubClient 2.8.0."""
from pathlib import Path

Import('env')

# Match the unsigned write count in publish_P; keep the pinned receive layout.
client = Path(env.subst('$PROJECT_LIBDEPS_DIR')) / env.subst('$PIOENV') / 'PubSubClient/src/PubSubClient.cpp'
original = client.read_text()
fixed = original.replace('    int expectedLength;', '    unsigned int expectedLength;')
if fixed != original:
    client.write_text(fixed)
