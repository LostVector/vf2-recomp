"""Generate the optional VF2 effects program from a local Mini capture."""

import argparse
from pathlib import Path


def fingerprint(words):
    value = 2166136261
    for word in words:
        value = ((value ^ word) * 16777619) & 0xffffffff
    return value


def generate(words):
    program = ["// Generated locally from the active SCSP effects program.",
               "constexpr u16 native_words[] = {"]
    calls = ["void execute_native_program(SCSPDSP &dsp, SampleState &state, u32 dec) {"]
    for index in range(0, len(words), 4):
        opcode = ", ".join(f"0x{word:04x}" for word in words[index:index + 4])
        program.append(f"    {opcode},")
        odd = "true" if (index // 4) & 1 else "false"
        calls.append(f"    native_op<{opcode}, {odd}>(dsp, state, dec);")
    return "\n".join([*program, "};", "", *calls, "}", ""])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_log", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/gen/vf2-scsp-native-steps.inc"))
    args = parser.parse_args()
    for line in args.capture_log.read_text(errors="replace").splitlines():
        if not line.startswith("SCSP local program "):
            continue
        words = tuple(int(word, 16) for word in line.split()[3:])
        if len(words) == 328 and fingerprint(words) == 0xec101e66:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(generate(words), encoding="ascii", newline="\n")
            return
    parser.error("No matching 82-step VF2 effects program found in the capture")


if __name__ == "__main__":
    main()
