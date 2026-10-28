#!/bin/sh
#
# EC test suite:  sh tests/run.sh     (or: make test)
#
#   CC=clang CFLAGS="-O0 -g" sh tests/run.sh
#
set -u

cd "$(dirname "$0")/.." || exit 1

EC=${EC:-./ec}
CC=${CC:-cc}
CFLAGS=${CFLAGS:--Wall -Wextra -O2}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

pass=0
fail=0
ok()  { printf 'PASS  %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }

echo "== translate, compile and run =="
for t in tests/t_*.ec; do
    n=$(basename "$t" .ec)
    if $EC -o "$work/$n.c" "$t" 2>"$work/$n.err" &&
       $CC $CFLAGS -o "$work/$n" "$work/$n.c" 2>>"$work/$n.err" &&
       "$work/$n" >"$work/$n.out" 2>&1
    then
        ok "$n"
    else
        bad "$n"
        sed 's/^/      | /' "$work/$n.err" 2>/dev/null
        sed 's/^/      | /' "$work/$n.out" 2>/dev/null
    fi
done

echo "== plain C passes through untouched =="
if $EC -E tests/t_plain.ec > "$work/plain.out" 2>"$work/plain.err" &&
   cmp -s tests/t_plain.ec "$work/plain.out"
then
    ok "identity"
else
    bad "identity"
    diff -u tests/t_plain.ec "$work/plain.out" 2>/dev/null | sed 's/^/      | /'
fi

echo "== diagnostics point at the .ec source =="
cat > "$work/err.ec" <<'EOF'
int main(void)
{
    int x = 0;
    return x@;
}
EOF
if $EC -E "$work/err.ec" >/dev/null 2>"$work/err.txt"; then :; fi
if grep -q "err.ec:4:" "$work/err.txt"
then
    ok "error line number"
else
    bad "error line number"
    sed 's/^/      | /' "$work/err.txt"
fi
if grep -qi "error" "$work/err.txt" && [ -s "$work/err.txt" ]
then ok "error reported"; else bad "error reported"; fi

echo "== #line keeps compiler errors on the right line =="
cat > "$work/line.ec" <<'EOF'
typedef unsigned long long ULL;
ASM(cmp2, ULL, neg %rdi; mov %rdi, %rax; ret)

int main(void)
{
    ULL v = cmp2(3);
    v = nosuchvariable;
    return (int)v;
}
EOF
if $EC -o "$work/line.c" "$work/line.ec" 2>"$work/line.err"; then :; fi
if $CC $CFLAGS -c -o "$work/line.o" "$work/line.c" 2>>"$work/line.err"; then :; fi
if grep -q "line.ec:7:" "$work/line.err"
then
    ok "#line directives"
else
    bad "#line directives"
    sed 's/^/      | /' "$work/line.err"
fi

echo "== command line =="
if $EC --version | grep -q "^ec "; then ok "--version"; else bad "--version"; fi
if printf 'int f(int x){return x@2;}' | $EC -E - | grep -qF '(((x) >> (2)) & 1)'
then ok "stdin"; else bad "stdin"; fi
if $EC -E tests/t_plain.ec --no-lines | cmp -s - tests/t_plain.ec
then ok "--no-lines"; else bad "--no-lines"; fi
if printf 'ASM(f, ULL, ret)\n' | $EC -E - | grep -q '__attribute__((naked)) ULL f(ULL)'
then ok "ASM() shape"; else bad "ASM() shape"; fi
if printf 'int f(int x)\n{\n    [\n      "=r"(x) : "r"(x)\n      mov %%[x], %%[x]\n    ]\n}\n' |
       $EC -E - --no-pct | grep -qF 'mov %[x], %[x]'
then ok "--no-pct"; else bad "--no-pct"; fi

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
