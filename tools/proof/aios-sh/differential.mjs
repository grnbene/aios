// aios-sh held against bash: each case runs through both, in identical fresh fixture directories, and their
// stdout, stderr's presence and exit status must agree. bash (Git for Windows' on Windows) is the oracle,
// as real git is the oracle for the kernel's git.
//
//   node tools/proof/aios-sh/differential.mjs <aios-sh> [bash] [--only substring] [--verbose]
//
// Exit 0 when every case agrees, 1 otherwise (each disagreement printed with both outputs).
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const args = process.argv.slice(2);
const AIOS = args[0] && path.resolve(args[0]);
const BASH = args[1] && !args[1].startsWith('--') ? args[1] : (process.platform === 'win32' ? 'C:\\Program Files\\Git\\bin\\bash.exe' : '/bin/bash');
const only = args.includes('--only') ? args[args.indexOf('--only') + 1] : '';
const verbose = args.includes('--verbose');
if (!AIOS || !fs.existsSync(AIOS)) { console.error('usage: differential.mjs <aios-sh> [bash]'); process.exit(2); }

// The fixture every case starts from.
function fixture(dir) {
  const w = (p, s) => { fs.mkdirSync(path.dirname(path.join(dir, p)), { recursive: true }); fs.writeFileSync(path.join(dir, p), s); };
  w('a.txt', 'alpha\nbeta\ngamma\ndelta\nepsilon\n');
  w('nums.txt', '10\n2\n33\n4\n2\n');
  w('csv.txt', 'name,age,city\nann,31,oslo\nbob,25,rome\ncid,40,lima\n');
  w('words.txt', 'the quick brown fox\njumps over the lazy dog\nThe End\n');
  w('src/main.js', "const x = 1;\nfunction add(a, b) {\n  return a + b;\n}\nconsole.log(add(x, 2));\n");
  w('src/util.js', 'export const twice = (n) => n * 2;\n// TODO: thrice\n');
  w('src/deep/z.txt', 'deep file\n');
  w('crlf.txt', 'one\r\ntwo\r\nthree\r\n');
  w('nonl.txt', 'last line without newline');
  w('empty.txt', '');
  w('sp ace.txt', 'file with a space\n');
}

const cases = [
  // ---- the language
  ['echo', 'echo hello world'],
  ['echo -n', 'echo -n a; echo b'],
  ['echo -e', 'echo -e "a\\tb\\nc"'],
  ['single quotes', "echo 'a  b' '$HOME'"],
  ['double quotes', 'x="a  b"; echo "$x" $x'],
  ['escapes', 'echo a\\ b "c\\"d" \'e\\f\''],
  ['variables', 'a=1 b=two; echo "$a-$b" ${a}x'],
  ['default', 'unset u; echo ${u:-dflt} ${u-x} "${u:+set}"; v=; echo "[${v:-e}]" "[${v-e}]"'],
  ['assign default', 'unset u; echo ${u:=7}; echo $u'],
  ['length', 's=hello; echo ${#s}'],
  ['prefix/suffix', 'f=dir/sub/file.tar.gz; echo ${f#*/} ${f##*/} ${f%.*} ${f%%.*}'],
  ['replace', 's=a-b-c; echo ${s/-/+} ${s//-/+}'],
  ['substring', 's=abcdef; echo ${s:2} ${s:1:3} ${s: -2}'],
  ['case conversion', 's=Hello; echo ${s^^} ${s,,}'],
  ['positional', 'set -- a "b c" d; echo $# "$1" "$2"; for x in "$@"; do echo "[$x]"; done'],
  ['$* vs $@', 'set -- a "b c"; for x in $*; do echo "<$x>"; done; for x in "$*"; do echo "{$x}"; done'],
  ['shift', 'set -- 1 2 3; shift; echo "$@"; shift 2; echo "$#"'],
  ['command substitution', 'echo "x$(echo inner)y" `echo back`'],
  ['nested substitution', 'echo $(echo $(echo deep))'],
  ['substitution strips newlines', 'v=$(printf "a\\n\\n\\n"); echo "[$v]"'],
  ['arithmetic', 'echo $((1+2*3)) $((10/3)) $((10%3)) $((2**10)) $(( (1+2)*3 ))'],
  ['arithmetic vars', 'i=5; echo $((i+1)) $((i*=2)) $i; echo $((i++)) $i $((++i))'],
  ['arithmetic compare', 'echo $((3>2)) $((3<2)) $((1==1&&0)) $((1||0)) $((5?6:7))'],
  ['field splitting', 'v="a b   c"; for x in $v; do echo "<$x>"; done'],
  ['IFS', 'IFS=:; v="a:b:c"; for x in $v; do echo $x; done'],
  ['globbing', 'echo *.txt'],
  ['glob in dir', 'echo src/*.js'],
  ['glob no match', 'echo *.nomatch'],
  ['quoted glob', 'echo "*.txt"'],
  ['tilde', 'echo ~ | grep -c .'],
  ['&& ||', 'true && echo yes; false && echo no; false || echo alt; true || echo never'],
  ['! ', '! false && echo negated; ! true; echo $?'],
  ['exit status', 'false; echo $?; true; echo $?; (exit 7); echo $?'],
  ['if elif else', 'x=2; if [ $x = 1 ]; then echo one; elif [ $x = 2 ]; then echo two; else echo other; fi'],
  ['for loop', 'for i in 1 2 3; do echo item$i; done'],
  ['for over glob', 'for f in src/*.js; do echo "$f"; done'],
  ['while loop', 'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'],
  ['until loop', 'i=0; until [ $i -ge 2 ]; do echo u$i; i=$((i+1)); done'],
  ['break continue', 'for i in 1 2 3 4 5; do [ $i = 2 ] && continue; [ $i = 4 ] && break; echo $i; done'],
  ['nested break', 'for a in 1 2; do for b in x y; do [ $b = y ] && break 2; echo $a$b; done; done; echo after'],
  ['case', 'for w in apple Banana 42 x.txt; do case $w in a*) echo A;; [Bb]*) echo B;; [0-9]*) echo N;; *.txt|*.md) echo F;; esac; done'],
  ['case default', 'case zzz in a) echo a;; *) echo star;; esac'],
  ['functions', 'f() { echo "in f: $1 $#"; return 3; }; f a b; echo $?'],
  ['function local', 'x=outer; f() { local x=inner; echo $x; }; f; echo $x'],
  ['function recursion', 'fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(( $1 - 1 ))) )); fi; }; fact 5'],
  ['subshell isolation', 'x=1; (x=2; cd src; echo $x); echo $x; pwd | grep -c src'],
  ['group', '{ echo a; echo b; } | wc -l'],
  ['pipeline', 'cat a.txt | grep a | wc -l'],
  ['pipefail', 'set -o pipefail; false | true; echo $?'],
  ['no pipefail', 'false | true; echo $?'],
  ['set -e', 'set -e; true; false; echo unreachable'],
  ['set -e if', 'set -e; if false; then echo no; fi; echo survived'],
  ['set -e &&', 'set -e; false && true; echo survived'],
  ['set -u', 'set -u; echo ${undefined_var}; echo after'],
  ['redirect out', 'echo hi > o.txt; cat o.txt; echo more >> o.txt; cat o.txt'],
  ['redirect in', 'wc -l < a.txt'],
  ['redirect stderr', 'ls nosuchfile 2>/dev/null; echo $?'],
  ['2>&1', 'ls nosuchfile 2>&1 | grep -c nosuchfile'],
  ['&>', 'ls nosuchfile &> e.txt; grep -c nosuchfile e.txt'],
  ['here-doc', 'cat <<EOF\nline $((1+1))\n  indented\nEOF'],
  ['quoted here-doc', "cat <<'EOF'\nno $expansion here\nEOF"],
  ['<<- strips tabs', 'cat <<-EOF\n\ttabbed\n\tEOF'],
  ['here-doc to file', "cat > h.txt <<'X'\na\nb\nX\nwc -l < h.txt"],
  ['read', 'printf "a b c\\n" | { read x y; echo "$x|$y"; }'],
  ['while read', 'while read line; do echo "<$line>"; done < a.txt'],
  ['while read IFS', "while IFS=, read n a c; do echo $a; done < csv.txt"],
  ['eval', 'cmd="echo evaluated"; eval $cmd'],
  ['export to child', 'export FOO=bar; sh -c "echo \\$FOO"'],
  ['prefix assignment', 'X=1 sh -c "echo \\$X"; echo "[$X]"'],
  ['sh -c args', 'sh -c "echo \\$0 \\$1 \\$2" name a b'],
  ['command -v', 'command -v cat >/dev/null && echo found'],
  ['test strings', '[ abc = abc ] && echo eq; [ -n "x" ] && echo nonempty; [ -z "" ] && echo empty; [ a != b ] && echo ne'],
  ['test numbers', '[ 3 -gt 2 ] && echo gt; [ 2 -le 2 ] && echo le; test 1 -eq 2 || echo noteq'],
  ['test files', '[ -f a.txt ] && echo file; [ -d src ] && echo dir; [ -e nope ] || echo absent; [ -s empty.txt ] || echo zero'],
  ['test combinators', '[ -f a.txt -a -d src ] && echo both; [ ! -f nope ] && echo not'],
  ['printf formats', 'printf "%s=%d %5.2f|%-4s|%03d %x\\n" k 42 3.14159 ab 7 255'],
  ['printf reuse', 'printf "%s\\n" a b c'],
  ['printf -v', 'printf -v out "%s-%s" x y; echo $out'],
  ['$( ) in args', 'echo "$(cat a.txt | head -2)"'],
  ['backslash newline', 'echo one \\\ntwo'],
  ['comments', 'echo a # comment\n# whole line\necho b'],
  ['semicolons newlines', 'echo 1; echo 2\necho 3'],
  ['empty command sub', 'x=$(true); echo "[$x]"'],
  ['special params', 'echo $# ; set -- x y; echo $#'],
  ['quoted empty', 'set -- "" b; echo $#'],
  ['nested quotes', 'echo "a $(echo "b c") d"'],
  ['arith in test', 'n=5; if [ $((n % 2)) -eq 1 ]; then echo odd; fi'],
  ['background and wait', 'echo first; wait; echo second'],
  // ---- utilities
  ['cat', 'cat a.txt nonl.txt'],
  ['cat -n', 'cat -n a.txt'],
  ['head', 'head -2 a.txt; head -n 1 nums.txt; head -n -3 a.txt'],
  ['tail', 'tail -2 a.txt; tail -n +4 a.txt; tail -n 1 nonl.txt; echo'],
  ['wc', 'wc -l a.txt; wc -w words.txt; wc -c a.txt; cat a.txt | wc -l'],
  ['sort', 'sort a.txt; sort -n nums.txt; sort -rn nums.txt; sort -u nums.txt'],
  ['sort -k -t', 'tail -n +2 csv.txt | sort -t, -k2 -n'],
  ['uniq', 'sort nums.txt | uniq; sort nums.txt | uniq -c; sort nums.txt | uniq -d'],
  ['cut', 'cut -d, -f2 csv.txt; cut -c1-3 a.txt; cut -d, -f1,3 csv.txt'],
  ['tr', 'echo hello | tr a-z A-Z; echo "a  b" | tr -s " "; echo abc | tr -d b; echo hello | tr "[:lower:]" "[:upper:]"'],
  ['grep basic', 'grep a a.txt'],
  ['grep -n -i -c', 'grep -n the words.txt; grep -ic the words.txt; grep -i "^the" words.txt'],
  ['grep -v -x -w', 'grep -v a a.txt; grep -x beta a.txt; grep -w the words.txt'],
  ['grep -E', 'grep -E "alp|gam" a.txt; grep -E "^[a-c]" a.txt; grep -E "l+" a.txt'],
  ['grep BRE groups', 'grep "\\(al\\|ga\\)" a.txt; grep "ta$" a.txt'],
  ['grep -o', 'grep -o "[0-9]\\+" csv.txt'],
  ['grep -r', 'grep -rn "return" src; grep -rl TODO .'],
  ['grep --include', 'grep -r --include="*.js" const . | sort'],
  ['grep -l -L', 'grep -l alpha a.txt nums.txt; grep -L alpha a.txt nums.txt'],
  ['grep -q status', 'grep -q zzz a.txt; echo $?; grep -q beta a.txt; echo $?'],
  ['grep context', 'grep -A1 beta a.txt; echo --; grep -B1 delta a.txt; echo --; grep -C1 gamma a.txt'],
  ['grep -F', 'grep -F "a+b" a.txt; echo $?; echo "x a+b y" | grep -F "a+b"'],
  ['grep -e multiple', 'grep -e alpha -e delta a.txt'],
  ['grep -m', 'grep -m2 a a.txt'],
  ['grep stdin', 'echo foo bar | grep -o bar'],
  ['grep \\d (GNU -P)', 'grep -P "\\d{2}" csv.txt'],
  ['sed s', 'sed "s/a/A/" a.txt; sed "s/a/A/g" a.txt'],
  ['sed -n p range', 'sed -n "2,4p" a.txt; sed -n "\\$p" a.txt; sed -n "/beta/,/delta/p" a.txt'],
  ['sed d', 'sed "/a/d" a.txt; sed "1d" a.txt; sed "2,3d" a.txt'],
  ['sed groups', 'echo "john smith" | sed "s/\\(.*\\) \\(.*\\)/\\2, \\1/"; echo "abc" | sed -E "s/(a)(b)/\\2\\1/"'],
  ['sed &', 'echo hello | sed "s/l\\+/[&]/"'],
  ['sed multiple -e', 'sed -e "s/a/1/" -e "s/e/2/" a.txt'],
  ['sed ; separated', 'sed "s/a/1/;s/b/2/" a.txt'],
  ['sed -i', 'cp a.txt b.txt; sed -i "s/beta/BETA/" b.txt; cat b.txt'],
  ['sed a i c', 'sed "2a\\inserted after" a.txt; sed "1i\\before" a.txt | head -2; sed "3c\\changed" a.txt'],
  ['sed y', 'echo hello | sed "y/hel/HEL/"'],
  ['sed nth', 'echo aaaa | sed "s/a/b/2"; echo aaaa | sed "s/a/b/2g"'],
  ['sed = and q', 'sed -n "/gamma/=" a.txt; sed "2q" a.txt'],
  ['sed I flag', 'echo HeLLo | sed "s/hello/hi/I"'],
  ['sed hold', 'sed -n "1h;1!H;\\${g;s/\\n/,/g;p}" a.txt'],
  // MSYS sed -i drops the CR of CRLF lines (GNU sed keeps them, as aios-sh does): the text is compared
  ['sed -i on CRLF', 'cp crlf.txt c2.txt; sed -i "s/two/TWO/" c2.txt; tr -d "\r" < c2.txt'],
  ['awk print fields', "awk '{print $2}' words.txt; awk -F, '{print $1, $3}' csv.txt"],
  ['awk NR NF', "awk 'NR==2' a.txt; awk '{print NF}' words.txt; awk 'END {print NR}' a.txt"],
  ['awk sum', "awk '{s+=$1} END {print s}' nums.txt"],
  ['awk patterns', "awk '/^[bd]/' a.txt; awk 'length($0) > 5' a.txt"],
  ['awk printf', "awk -F, 'NR>1 {printf \"%-5s|%3d\\n\", $1, $2}' csv.txt"],
  ['awk arrays', "awk '{c[$1]++} END {for (k in c) n++; print n}' nums.txt"],
  ['awk BEGIN vars', "awk -v x=5 'BEGIN {print x*2}'; awk 'BEGIN {OFS=\"-\"} {$1=$1; print}' words.txt"],
  ['awk functions', "awk 'BEGIN {print length(\"abc\"), substr(\"hello\", 2, 3), index(\"hello\",\"l\"), toupper(\"x\")}'"],
  ['awk split gsub', "awk 'BEGIN {n=split(\"a:b:c\", p, \":\"); print n, p[3]; s=\"aaa\"; gsub(/a/, \"b\", s); print s}'"],
  ['awk user function', "awk 'function sq(x) { return x*x } BEGIN { print sq(7) }'"],
  ['awk ranges', "awk '/beta/,/delta/' a.txt"],
  ['awk if else', "awk '{ if ($1 > 5) print \"big\", $1; else print \"small\", $1 }' nums.txt"],
  ['awk match', "awk 'BEGIN { if (match(\"foobar\", /ob/)) print RSTART, RLENGTH }'"],
  ['awk getline', "awk 'BEGIN { while ((getline l < \"a.txt\") > 0) n++; print n }'"],
  ['find', 'find src -name "*.js" | sort; find . -type d | sort; find src -maxdepth 1 | sort'],
  ['find -exec', 'find src -name "*.js" -exec grep -l add {} \\;'],
  ['find -not -path', 'find . -name "*.txt" -not -path "*/deep/*" | sort'],
  ['xargs', 'printf "a.txt\\nnums.txt\\n" | xargs wc -l; echo a b c | xargs -n1 echo x'],
  ['xargs -I', 'printf "1\\n2\\n" | xargs -I{} echo n{}'],
  ['mkdir -p rm -r', 'mkdir -p x/y/z; touch x/y/z/f; ls x/y/z; rm -r x; ls x 2>/dev/null || echo gone'],
  ['cp mv', 'cp a.txt c.txt; mv c.txt d.txt; ls c.txt 2>/dev/null; cat d.txt | head -1; cp -r src s2; ls s2 | sort'],
  ['basename dirname', 'basename /a/b/c.txt; basename /a/b/c.txt .txt; dirname /a/b/c.txt; dirname file'],
  ['seq', 'seq 3; seq 2 5; seq 1 2 7; seq -s, 1 4'],
  ['expr', 'expr 2 + 3; expr 7 \\* 6; expr 5 \\> 3'],
  ['tee', 'echo teed | tee t.txt; cat t.txt'],
  ['tac', 'tac a.txt | head -2'],   // (Git for Windows has no rev)
  ['base64', 'echo -n hello | base64; echo aGVsbG8= | base64 -d; echo'],
  ['ls', 'ls | sort; ls src'],
  ['ls -1 -a', 'ls -A src | sort'],
  ['diff', 'printf "a\\nb\\n" > p; printf "a\\nc\\n" > q; diff p q; echo $?'],
  ['diff same', 'diff a.txt a.txt; echo $?'],
  ['true false', 'true; echo $?; false; echo $?'],
  ['nl', 'nl a.txt | head -2'],
  // Git for Windows' coreutils mark stdin "*-" (binary mode); GNU on Linux prints "  -": the hashes are compared
  ['sha256sum', 'echo -n abc | sha256sum | cut -c1-64'],
  ['md5sum', 'echo -n abc | md5sum | cut -c1-32'],
  ['spaces in names', 'cat "sp ace.txt"; for f in sp*; do echo "[$f]"; done'],
  ['yes | head', 'yes | head -3'],
  ['sort file then uniq -c spacing', 'printf "b\\na\\nb\\n" | sort | uniq -c'],
  ['cat missing', 'cat nope.txt; echo $?'],
  ['command not found', 'definitely_not_a_command_xyz 2>/dev/null; echo $?'],
];

// bash extensions (what agents actually write), kept as plain text so ${...} and backslashes need no escaping:
// "### name" then the script, up to the next "###".
{
  const text = fs.readFileSync(new URL('./bash-cases.txt', import.meta.url), 'utf8').replace(/\r\n/g, '\n');
  for (const block of text.split(/^### /m).slice(1)) {
    const nl = block.indexOf('\n');
    cases.push([block.slice(0, nl).trim(), block.slice(nl + 1).replace(/\n$/, '')]);
  }
}

function run(shell, script, dir) {
  const r = spawnSync(shell, ['-c', script], { cwd: dir, encoding: 'utf8', timeout: 30000, env: { ...process.env, LC_ALL: 'C', LANG: 'C' } });
  return { out: (r.stdout || '').replace(/\r\n/g, '\n'), err: (r.stderr || ''), code: r.status, error: r.error && r.error.message };
}

let agree = 0, differ = 0;
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'aios-sh-diff-'));
for (const [name, script] of cases) {
  if (only && !name.includes(only)) continue;
  const da = fs.mkdtempSync(path.join(root, 'a-')), db = fs.mkdtempSync(path.join(root, 'b-'));
  fixture(da); fixture(db);
  const want = run(BASH, script, db), got = run(AIOS, script, da);
  const same = want.out === got.out && want.code === got.code && (!!want.err.trim()) === (!!got.err.trim());
  if (same) { agree++; if (verbose) console.log('ok    ' + name); continue; }
  differ++;
  console.log(`\nDIFF  ${name}\n  script: ${JSON.stringify(script)}`);
  console.log(`  bash    (${want.code}): ${JSON.stringify(want.out).slice(0, 400)}${want.err.trim() ? '  stderr: ' + JSON.stringify(want.err.trim()).slice(0, 160) : ''}`);
  console.log(`  aios-sh (${got.code}): ${JSON.stringify(got.out).slice(0, 400)}${got.err.trim() ? '  stderr: ' + JSON.stringify(got.err.trim()).slice(0, 160) : ''}${got.error ? '  ' + got.error : ''}`);
}
fs.rmSync(root, { recursive: true, force: true });
console.log(`\n${agree} agree, ${differ} differ`);
process.exit(differ ? 1 : 0);
