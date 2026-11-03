"""Guard project-owned C++ against exception-based error propagation."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
# Keep only the external-library adapters exception-enabled.
boundaries = {
    "tools/cli/main.cpp": ("CLI::ParseError",),
    "src/engines/studio/capture_windows.cpp": ("hresult_error",),
    # Existing exception-enabled logging initialization adapter, separate from Client.
    "src/backends/livekit/logging.cpp": ("std::exception", "..."),
}
trivia = re.compile(
    r"""/\*.*?\*/|//[^\n]*|R"([^ ()\\\t\r\n]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'""",
    re.DOTALL,
)
forbidden = re.compile(r"\b(?:throw|try|catch|exception_ptr|current_exception|make_exception_ptr|rethrow_exception|set_exception)\b")
errors = []
checked = 0
for directory in ("include", "src", "support", "examples", "tools", "tests"):
    for path in (root / directory).rglob("*"):
        if path.suffix not in {".h", ".hpp", ".cpp", ".cc", ".mm"}:
            continue
        checked += 1
        source = path.read_text(encoding="utf-8-sig")
        code = trivia.sub(lambda m: "\n" * m.group().count("\n"), source)
        relative = path.relative_to(root).as_posix()
        if relative in boundaries:
            expected = boundaries[relative]
            catches = re.findall(r"\bcatch\s*\(([^)]*)\)", code)
            if not catches or any(not any(kind in item for kind in expected) for item in catches):
                errors.append(f"{relative}: unexpected external exception boundary")
            code = re.sub(r"\b(?:try|catch)\b", "", code)
        for match in forbidden.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            errors.append(f"{relative}:{line}: {match.group()} is not an explicit error result")
if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
print(f"PASS: {checked} project C++ files; only declared CLI11, WinRT and logging adapter catches allowed")
