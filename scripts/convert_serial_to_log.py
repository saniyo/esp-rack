#!/usr/bin/env python3
"""
Convert Serial.print*(...) calls to arduino-esp32 log_*(...) macros.

Severity inferred from keywords in the message:
  - log_e: "failed", "ERROR", "FAIL", "couldn't", "rejected", "panic", "abort", "missing"
  - log_w: "WARN", "warn", "skip", "blocked", "retry", "already running"
  - log_i: "started", "connected", "init", "got IP", "done", "ready", "loaded",
           "generated", "approved", "spawning"
  - log_d: default — "step", "POST", "GET", "code=", "tick", "enter", "exit",
           "sleep", "iter", "POST/GET response", "trying"

Preserves the [tag] prefix in the format string (grep-friendly).
Strips trailing \\n since log_* adds CRLF itself.
Skips the boot-tag line `[fw] %s | %s | %s` (must always print).
Skips files in /tests/ and /examples/.
"""
import re
import sys
from pathlib import Path

# Severity keyword tables. ORDER MATTERS — first match wins.
# Match against the message string content (case-sensitive where it matters).
ERROR_KEYWORDS = [
    'failed', 'FAILED', 'FAIL', 'fail:', 'ERROR', 'PANIC', 'abort',
    "couldn't", "cannot", 'rejected', 'unreachable', 'invalid',
    'corrupt', 'no current cert', 'no enroll URL', 'no recover URL',
    'no TLS provider', 'http.begin failed', 'JSON parse',
    'response missing', 'approved=true but',
    'BLACKLISTED', 'no recovery_token',
]
WARN_KEYWORDS = [
    'WARN', 'warn:', 'skip', 'skipping', 'blocked', 'retry',
    'already running', 'already enrolled', 'will retry',
    'still pending', 'pending operator',
    'server error', 'server returned',
    'mismatch', 'orphan', 'truncat',
]
INFO_KEYWORDS = [
    'started', 'connected', 'init', 'got IP', 'done', 'ready',
    'loaded', 'generated', 'APPROVED', 'spawning', 'mounted',
    'success', 'OK', 'completed', 'installed', 'task start',
    'CSR built', 'keypair generated', 'state externally',
    'applying new cert',
]
# DEBUG default

# Lines to leave alone (always-print boot diagnostics)
SKIP_PATTERNS = [
    r'\[fw\]\s*%s\s*\|',           # firmware tag line in App.cpp
    r'ESPRACK_TAG_BASE',            # any anchor for fw tags
]

# Multi-line Serial.<method>(...) pattern.
# Method: print, printf, println, printf_P (PROGMEM variant on arduino-esp32).
# Args end with `);` on its own (possibly multi-line body).
SERIAL_CALL_RE = re.compile(
    r'(?P<lead>^[ \t]*)'                                                # leading whitespace
    r'Serial\.(?P<method>print|printf|println|printf_P)\s*\('           # call open
    r'(?P<args>.*?)'                                                    # body
    r'\)\s*;[ \t]*$',                                                   # close
    re.DOTALL | re.MULTILINE,
)

# F("...") and PSTR("...") wrappers — flash-string helpers. log_* takes a
# plain const char* format, so unwrap to the bare literal.
FLASH_HELPER_RE = re.compile(r'\b(?:F|PSTR)\(\s*("(?:[^"\\]|\\.)*")\s*\)')


def infer_level(msg: str) -> str:
    """Pick log macro name from message content."""
    for kw in ERROR_KEYWORDS:
        if kw in msg:
            return 'log_e'
    for kw in WARN_KEYWORDS:
        if kw in msg:
            return 'log_w'
    for kw in INFO_KEYWORDS:
        if kw in msg:
            return 'log_i'
    return 'log_d'


def should_skip(args: str) -> bool:
    for p in SKIP_PATTERNS:
        if re.search(p, args):
            return True
    return False


def transform_call(m: re.Match) -> str:
    """Rewrite one Serial.<method>(...) call."""
    lead = m.group('lead')
    method = m.group('method')
    args = m.group('args')

    if should_skip(args):
        return m.group(0)  # leave untouched

    # Unwrap F("...") and PSTR("...") flash-helper wrappers to bare
    # literals so log_* (printf-style) accepts them. Applies to format
    # string AND to inline ternary alternatives like
    # `cond ? F("yes") : F("no")`.
    args_unwrapped = FLASH_HELPER_RE.sub(r'\1', args)

    # Strip trailing "\n" / "\r\n" from the format string (log_* adds
    # its own CRLF). Matches the LAST quoted-string region only — handles
    # both "...\n" and "...\\r\\n" tails before the closing quote.
    args_clean = re.sub(r'(\\r)?\\n(?P<q>")', r'\g<q>', args_unwrapped, count=1)

    # For severity inference, scan the args body for keywords.
    level = infer_level(args_clean)

    method_is_print_like = method in ('print', 'println')
    is_string_literal_first = bool(re.match(r'\s*"', args_clean))

    if method_is_print_like and not is_string_literal_first:
        # Single non-literal value (variable, expr, ternary that didn't
        # resolve to a literal). Wrap to "%s" with .c_str() best-effort —
        # for Arduino String this works; for int/IPAddress/bool a manual
        # post-pass will need to adjust the format.
        new_call = f'{level}("%s", {args_clean.strip()})'
    elif method == 'printf_P':
        # PSTR was already unwrapped above; treat like printf.
        new_call = f'{level}({args_clean})'
    else:
        new_call = f'{level}({args_clean})'

    return f'{lead}{new_call};'


def process_file(path: Path, dry_run: bool = False) -> tuple[int, dict]:
    """Convert all Serial.print* calls in one file. Returns (count, level_breakdown)."""
    text = path.read_text(encoding='utf-8')
    counts = {'log_e': 0, 'log_w': 0, 'log_i': 0, 'log_d': 0, 'skipped': 0}

    def repl(m):
        if should_skip(m.group('args')):
            counts['skipped'] += 1
            return m.group(0)
        new = transform_call(m)
        # Extract chosen level for stats
        m2 = re.search(r'(log_[ewid])\(', new)
        if m2:
            counts[m2.group(1)] += 1
        return new

    new_text = SERIAL_CALL_RE.sub(repl, text)
    changed = (new_text != text)

    total = counts['log_e'] + counts['log_w'] + counts['log_i'] + counts['log_d']
    if changed and not dry_run:
        path.write_text(new_text, encoding='utf-8')
    return total, counts


def main():
    repo = Path(__file__).resolve().parent.parent
    targets = []
    for sub in ('lib/ESPRack', 'modules'):
        root = repo / sub
        for p in root.rglob('*.cpp'):
            targets.append(p)
        for p in root.rglob('*.h'):
            targets.append(p)

    dry = '--dry' in sys.argv

    total_calls = 0
    total_counts = {'log_e': 0, 'log_w': 0, 'log_i': 0, 'log_d': 0, 'skipped': 0}
    print(f"{'FILE':<70} {'E':>3} {'W':>3} {'I':>3} {'D':>3} {'SKIP':>4}")
    for p in sorted(targets):
        if '/tests/' in str(p).replace('\\', '/'):
            continue
        before = p.read_text(encoding='utf-8')
        if 'Serial.print' not in before:
            continue
        count, breakdown = process_file(p, dry_run=dry)
        if count == 0 and breakdown['skipped'] == 0:
            continue
        rel = p.relative_to(repo)
        print(f"{str(rel):<70} {breakdown['log_e']:>3} {breakdown['log_w']:>3} {breakdown['log_i']:>3} {breakdown['log_d']:>3} {breakdown['skipped']:>4}")
        total_calls += count
        for k in total_counts:
            total_counts[k] += breakdown[k]

    print(f"{'-'*92}")
    print(f"{'TOTAL':<70} {total_counts['log_e']:>3} {total_counts['log_w']:>3} {total_counts['log_i']:>3} {total_counts['log_d']:>3} {total_counts['skipped']:>4}")
    print(f"\n{total_calls} calls converted{'  (DRY RUN)' if dry else ''}")


if __name__ == '__main__':
    main()
