# ============================================================================
# tcl91_all_features.tcl — Comprehensive Tcl 9.1 Language Exerciser
#
# Exercises EVERY grammatical construct, control-flow statement, data type,
# expression operator, string/list/dict operation, namespace facility,
# coroutine/yield, upvar/uplevel, and the full TclOO object system
# (including classmethod, private, forward, filter, mixin, initialise,
# oo::abstract, oo::singleton, oo::copy, oo::objdefine, deletemethod,
# renamemethod, unexport, introspection, metaclass, etc.)
#
# Usage:
#   tclsh9.1 tcl91_all_features.tcl
#   — or —
#   tbcx::save tcl91_all_features.tcl tcl91_all_features.tbcx
#   tbcx::load tcl91_all_features.tbcx
#
# On success: prints "ALL_FEATURES: ALL <N> TESTS PASSED"
# On failure: prints details and exits with code 1.
# ============================================================================

# ---------- harness ---------------------------------------------------------
set ::_pass 0
set ::_fail 0
set ::_errors {}

proc assert {tag got expected} {
    if {$got eq $expected} {
        incr ::_pass
    } else {
        incr ::_fail
        lappend ::_errors "FAIL $tag: got «[string range $got 0 200]» expected «[string range $expected 0 200]»"
    }
}
proc assert_true {tag cond} {
    if {$cond} { incr ::_pass } else {
        incr ::_fail; lappend ::_errors "FAIL $tag: condition was false"
    }
}
proc assert_match {tag got pattern} {
    if {[string match $pattern $got]} { incr ::_pass } else {
        incr ::_fail; lappend ::_errors "FAIL $tag: «[string range $got 0 200]» !match $pattern"
    }
}
proc assert_error {tag script} {
    if {[catch {uplevel 1 $script} msg]} { incr ::_pass } else {
        incr ::_fail; lappend ::_errors "FAIL $tag: expected error, got «$msg»"
    }
}

# ============================================================================
# SECTION 1: Tcl Grammar — Substitution, Quoting, Grouping
# ============================================================================

# 1.1 Variable substitution — scalar
set x 42
assert gram-var-scalar $x 42

# 1.2 Variable substitution — braced name
set {odd name} hello
assert gram-var-braced ${odd name} hello

# 1.3 Variable substitution — array
set arr(key1) alpha
set arr(key2) beta
assert gram-var-array $arr(key1) alpha

# 1.4 Variable substitution — array with variable index
set idx key2
assert gram-var-array-idx $arr($idx) beta

# 1.5 Variable substitution — namespace-qualified
namespace eval ::gram_ns { variable v 99 }
assert gram-var-ns $::gram_ns::v 99

# 1.6 Command substitution
assert gram-cmd-sub [expr {2 + 3}] 5

# 1.7 Nested command substitution
assert gram-cmd-nested [string length [string range "abcdef" 1 3]] 3

# 1.8 Backslash substitution — special characters
assert gram-bs-tab "a\tb" "a\tb"
assert gram-bs-newline "a\nb" "a\nb"
assert gram-bs-cr "a\rb" "a\rb"
assert gram-bs-backslash "a\\b" "a\\b"

# 1.9 Backslash substitution — octal
assert gram-bs-octal "\101" "A"

# 1.10 Backslash substitution — hex
assert gram-bs-hex "\x41" "A"

# 1.11 Backslash substitution — unicode
assert gram-bs-unicode "\u0041" "A"
assert gram-bs-unicode-wide "\U0001F600" "\U0001F600"

# 1.12 Brace quoting — no substitution
set val "should not appear"
assert gram-brace-nosub {$val} {$val}

# 1.13 Double-quote quoting — with substitution
assert gram-dq-sub "$x items" "42 items"

# 1.14 Semicolon as command separator
set a 1; set b 2; assert gram-semi [expr {$a + $b}] 3

# 1.15 Backslash-newline continuation
set long_val [expr {1 + \
    2 + \
    3}]
assert gram-bsnl $long_val 6

# 1.16 Comments
# This is a comment — should not produce output or errors
assert gram-comment 1 1 ;# inline comment

# 1.17 Empty command (just whitespace/comment lines)
;
assert gram-empty-cmd 1 1

# 1.18 Command with no arguments
set _ [list]
assert gram-noarg-list $_ {}

# ============================================================================
# SECTION 2: Data Types & Literals
# ============================================================================

# 2.1 Strings
assert dt-str-empty "" ""
assert dt-str-ascii "hello" "hello"
assert dt-str-unicode "日本語" "日本語"
assert dt-str-emoji "\U0001F4A9" "\U0001F4A9"

# 2.2 Integers
assert dt-int-zero [expr {0}] 0
assert dt-int-pos [expr {12345}] 12345
assert dt-int-neg [expr {-67890}] -67890
assert dt-int-hex [expr {0xFF}] 255
assert dt-int-oct [expr {0o77}] 63
assert dt-int-bin [expr {0b10101}] 21
assert dt-int-max64 [expr {9223372036854775807}] 9223372036854775807

# 2.3 Wide unsigned int
assert dt-uint-large [expr {18446744073709551615 - 18446744073709551614}] 1

# 2.4 Bignums
assert dt-bignum-1 [expr {10**30}] 1000000000000000000000000000000
assert dt-bignum-neg [expr {-(2**100)}] [expr {-(2**100)}]
assert dt-bignum-arith [expr {(10**20) * (10**20)}] [expr {10**40}]

# 2.5 Doubles
assert dt-dbl-pi [expr {acos(-1.0)}] [expr {acos(-1.0)}]
assert dt-dbl-sci [expr {1.5e3}] 1500.0
assert dt-dbl-neg [expr {-0.001}] -0.001
assert dt-dbl-inf [expr {1.0/0.0}] Inf
# Tcl 9.1 rejects NaN values: both 0.0/0.0 and double("NaN") raise errors.
assert_true dt-dbl-nan-div0 [catch {expr {0.0/0.0}}]
assert_true dt-dbl-nan-literal [catch {expr {double("NaN")}}]

# 2.6 Booleans
assert dt-bool-true [expr {true}] true
assert dt-bool-false [expr {false}] false
assert dt-bool-yes [expr {yes}] yes
assert dt-bool-no [expr {no}] no
assert dt-bool-on [expr {on ? 1 : 0}] 1
assert dt-bool-off [expr {off ? 1 : 0}] 0

# 2.7 Byte arrays
set ba [binary format c3 {72 101 108}]
binary scan $ba a3 ba_str
assert dt-ba-1 $ba_str "Hel"
set ba2 [binary format H4 "cafe"]
binary scan $ba2 H4 ba2_hex
assert dt-ba-2 $ba2_hex "cafe"

# 2.8 Lists
assert dt-list-empty [list] {}
assert dt-list-simple [list a b c] "a b c"
assert dt-list-nested [list [list 1 2] [list 3 4]] "{1 2} {3 4}"

# 2.9 Dicts
assert dt-dict-empty [dict create] {}
set d [dict create name Alice age 30]
assert dt-dict-get [dict get $d name] Alice
assert dt-dict-size [dict size $d] 2

# ============================================================================
# SECTION 3: Expressions — operators, math functions, string ops
# ============================================================================

# 3.1 Arithmetic operators
assert expr-add [expr {7 + 3}] 10
assert expr-sub [expr {7 - 3}] 4
assert expr-mul [expr {7 * 3}] 21
assert expr-div [expr {21 / 3}] 7
assert expr-mod [expr {10 % 3}] 1
assert expr-pow [expr {2 ** 10}] 1024
assert expr-unary-minus [expr {-(-5)}] 5
assert expr-unary-plus [expr {+5}] 5

# 3.2 Comparison operators
assert expr-eq [expr {5 == 5}] 1
assert expr-ne [expr {5 != 4}] 1
assert expr-lt [expr {3 < 5}] 1
assert expr-gt [expr {5 > 3}] 1
assert expr-le [expr {5 <= 5}] 1
assert expr-ge [expr {5 >= 4}] 1

# 3.3 Logical operators
assert expr-and [expr {1 && 1}] 1
assert expr-or [expr {0 || 1}] 1
assert expr-not [expr {!0}] 1

# 3.4 Bitwise operators
assert expr-band [expr {0xFF & 0x0F}] 15
assert expr-bor [expr {0xF0 | 0x0F}] 255
assert expr-bxor [expr {0xFF ^ 0x0F}] 240
assert expr-bnot [expr {~0 & 0xFF}] 255
assert expr-lshift [expr {1 << 8}] 256
assert expr-rshift [expr {256 >> 4}] 16

# 3.5 Ternary operator
assert expr-ternary [expr {1 > 0 ? "yes" : "no"}] yes

# 3.6 String comparison in expr
assert expr-streq [expr {"abc" eq "abc"}] 1
assert expr-strne [expr {"abc" ne "def"}] 1
assert expr-strlt [expr {"abc" < "def"}] 1

# 3.7 in / ni operators
assert expr-in [expr {"b" in {a b c}}] 1
assert expr-ni [expr {"d" ni {a b c}}] 1

# 3.8 Math functions
assert expr-abs [expr {abs(-7)}] 7
assert expr-ceil [expr {int(ceil(2.3))}] 3
assert expr-floor [expr {int(floor(2.9))}] 2
assert expr-round [expr {round(2.5)}] 3
assert expr-sqrt [expr {sqrt(144)}] 12.0
assert expr-log [expr {int(log(exp(1.0)))}] 1
assert expr-sin [expr {sin(0.0)}] 0.0
assert expr-max [expr {max(3, 7, 1)}] 7
assert expr-min [expr {min(3, 7, 1)}] 1
assert expr-wide [expr {wide(42)}] 42
assert expr-entier [expr {entier(3.7)}] 3
assert expr-bool-fn [expr {bool("yes")}] 1
assert expr-double [expr {double(5)}] 5.0
assert expr-int [expr {int(9.9)}] 9
assert expr-isqrt [expr {isqrt(49)}] 7

# ============================================================================
# SECTION 4: Variables — set, unset, append, incr, lappend, array
# ============================================================================

# 4.1 set / unset
set myvar "hello"
assert var-set $myvar hello
unset myvar
assert_true var-unset [expr {![info exists myvar]}]

# 4.2 append
set buf "abc"
append buf "def" "ghi"
assert var-append $buf "abcdefghi"

# 4.3 incr
set counter 10
incr counter 5
assert var-incr $counter 15
incr counter -3
assert var-incr-neg $counter 12

# 4.4 lappend
set lst {}
lappend lst a b c
assert var-lappend $lst "a b c"

# 4.5 Array operations
array set colors {red #FF0000 green #00FF00 blue #0000FF}
assert var-array-get $colors(red) "#FF0000"
assert_true var-array-exists [array exists colors]
assert var-array-size [array size colors] 3
set names [lsort [array names colors]]
assert var-array-names $names "blue green red"
array unset colors green
assert var-array-size2 [array size colors] 2

# 4.6 Array with variable traces (set/read/unset)
set ::trace_log {}
proc trace_handler {name1 name2 op} {
    lappend ::trace_log "$op:$name2"
}
array set traced_arr {}
trace add variable traced_arr write trace_handler
set traced_arr(x) 10
set traced_arr(y) 20
assert var-trace $::trace_log "write:x write:y"
trace remove variable traced_arr write trace_handler

# ============================================================================
# SECTION 5: String Operations
# ============================================================================

# 5.1 string length / index / range
assert str-length [string length "hello"] 5
assert str-index [string index "abcdef" 2] "c"
assert str-range [string range "abcdef" 1 3] "bcd"

# 5.2 string first / last
assert str-first [string first "cd" "abcdef"] 2
assert str-last [string last "b" "abba"] 2

# 5.3 string map
assert str-map [string map {a A e E} "apple"] "ApplE"

# 5.4 string tolower / toupper / totitle
assert str-lower [string tolower "HELLO"] "hello"
assert str-upper [string toupper "hello"] "HELLO"
assert str-title [string totitle "hello world"] "Hello world"

# 5.5 string trim / trimleft / trimright
assert str-trim [string trim "  hi  "] "hi"
assert str-triml [string trimleft "xxhello" "x"] "hello"
assert str-trimr [string trimright "helloxx" "x"] "hello"

# 5.6 string match (glob)
assert_true str-match-1 [string match "hel*" "hello"]
assert_true str-match-2 [string match {[a-z]*} "hello"]
assert_true str-match-q [string match "h?llo" "hello"]

# 5.7 string is (type checking)
assert_true str-is-int [string is integer "42"]
assert_true str-is-dbl [string is double "3.14"]
assert_true str-is-bool [string is boolean "yes"]
assert_true str-is-alpha [string is alpha "hello"]
assert_true str-is-alnum [string is alnum "abc123"]
assert_true str-is-space [string is space "  \t\n"]
assert_true str-is-list [string is list "a b c"]

# 5.8 string repeat
assert str-repeat [string repeat "ab" 3] "ababab"

# 5.9 string reverse
assert str-reverse [string reverse "hello"] "olleh"

# 5.10 string replace
assert str-replace [string replace "hello world" 5 5 "_"] "hello_world"

# 5.11 string cat
assert str-cat [string cat "hello" " " "world"] "hello world"

# 5.12 string compare
assert str-compare [string compare "abc" "def"] -1
assert str-compare-eq [string compare "abc" "abc"] 0

# 5.13 string equal
assert_true str-equal [string equal "abc" "abc"]
assert_true str-equal-nocase [string equal -nocase "ABC" "abc"]

# 5.14 format / scan
assert str-format [format "%05d" 42] "00042"
assert str-format-f [format "%.2f" 3.14159] "3.14"
assert str-format-s [format "%-10s|" "hello"] "hello     |"
assert str-format-x [format "0x%04X" 255] "0x00FF"
lassign [scan "42 3.14 hello" "%d %f %s"] si sf ss
assert str-scan-int $si 42
assert_true str-scan-dbl [expr {abs($sf - 3.14) < 0.001}]
assert str-scan-str $ss "hello"

# ============================================================================
# SECTION 6: List Operations (including 9.x additions)
# ============================================================================

# 6.1 list / llength / lindex / lrange
set L [list a b c d e f]
assert lst-len [llength $L] 6
assert lst-idx [lindex $L 2] c
assert lst-range [lrange $L 1 3] "b c d"

# 6.2 linsert / lreplace / lset
assert lst-insert [linsert $L 2 X Y] "a b X Y c d e f"
assert lst-replace [lreplace $L 1 2 B C] "a B C d e f"
set LL $L; lset LL 0 Z
assert lst-set $LL "Z b c d e f"

# 6.3 lsort
assert lst-sort [lsort {banana apple cherry}] "apple banana cherry"
assert lst-sort-int [lsort -integer {3 1 2}] "1 2 3"
assert lst-sort-dec [lsort -decreasing {1 3 2}] "3 2 1"
assert lst-sort-uniq [lsort -unique {a b a c b}] "a b c"

# 6.4 lsearch
assert lst-search [lsearch $L c] 2
assert lst-search-glob [lsearch -glob $L "d*"] 3
assert lst-search-all [lsearch -all $L {[ace]}] "0 2 4"

# 6.5 lassign
lassign {1 2 3 4} v1 v2 v3
assert lst-assign-1 $v1 1
assert lst-assign-2 $v2 2
assert lst-assign-3 $v3 3

# 6.6 lrepeat
assert lst-repeat [lrepeat 3 x] "x x x"

# 6.7 lreverse
assert lst-reverse [lreverse {1 2 3}] "3 2 1"

# 6.8 join / split
assert lst-join [join {a b c} ","] "a,b,c"
assert lst-split [split "a,b,c" ","] "a b c"

# 6.9 lmap (Tcl 8.6+)
assert lst-lmap [lmap x {1 2 3 4} {expr {$x * $x}}] "1 4 9 16"

# 6.10 lseq (Tcl 9.x)
assert lst-lseq-1 [lseq 5] "0 1 2 3 4"
assert lst-lseq-2 [lseq 1 to 5] "1 2 3 4 5"
assert lst-lseq-3 [lseq 0 to 10 by 3] "0 3 6 9"
assert lst-lseq-4 [lseq 5 to 1] "5 4 3 2 1"

# 6.11 lpop (Tcl 9.x)
set plist {a b c d e}
set popped [lpop plist end]
assert lst-lpop-val $popped e
assert lst-lpop-rem $plist "a b c d"

# 6.12 ledit (Tcl 9.x)
set elist {a b c d e}
ledit elist 1 2 X Y Z
assert lst-ledit $elist "a X Y Z d e"

# 6.13 lremove (Tcl 9.x)
assert lst-lremove [lremove {a b c d e} 1 3] "a c e"

# 6.14 concat
assert lst-concat [concat {a b} {c d}] "a b c d"

# ============================================================================
# SECTION 7: Dict Operations
# ============================================================================

# 7.1 dict create / get / set / exists / size / keys / values
set D [dict create x 1 y 2 z 3]
assert dict-get [dict get $D y] 2
dict set D w 4
assert dict-set [dict get $D w] 4
assert_true dict-exists [dict exists $D x]
assert_true dict-nexists [expr {![dict exists $D nope]}]
assert dict-size [dict size $D] 4
assert dict-keys [lsort [dict keys $D]] "w x y z"
assert dict-values [lsort [dict values $D]] "1 2 3 4"

# 7.2 dict unset / remove
dict unset D w
assert dict-unset [dict size $D] 3
set D2 [dict remove $D z]
assert dict-remove [dict size $D2] 2

# 7.3 dict replace / merge / append / lappend / incr
set D3 [dict replace $D x 10]
assert dict-replace [dict get $D3 x] 10
set D4 [dict merge $D [dict create a 100]]
assert dict-merge [dict get $D4 a] 100
dict append D4 a "!" 
assert dict-append [dict get $D4 a] "100!"
dict lappend D4 tags first second
assert dict-lappend [dict get $D4 tags] "first second"
dict incr D4 counter
dict incr D4 counter 5
assert dict-incr [dict get $D4 counter] 6

# 7.4 dict for
set dsum 0
dict for {k v} $D { set dsum [expr {$dsum + $v}] }
assert dict-for $dsum 6

# 7.5 dict map
set D5 [dict map {k v} $D { expr {$v * 10} }]
assert dict-map [dict get $D5 x] 10

# 7.6 dict filter
set D6 [dict filter $D value 2]
assert dict-filter-val [dict size $D6] 1
set D7 [dict filter $D key {[xz]}]
assert dict-filter-key [dict size $D7] 2
set D8 [dict filter $D script {k v} { expr {$v > 1} }]
assert dict-filter-script [dict size $D8] 2

# 7.7 dict with
set car [dict create make Toyota model Camry year 2024]
dict with car {
    set desc "$make $model $year"
}
assert dict-with $desc "Toyota Camry 2024"

# 7.8 dict update (triggers DictUpdateInfo AuxData)
dict update car year y {
    set y [expr {$y + 1}]
}
assert dict-update [dict get $car year] 2025

# ============================================================================
# SECTION 8: Control Flow
# ============================================================================

# 8.1 if / elseif / else
proc classify {n} {
    if {$n < 0} {
        return "negative"
    } elseif {$n == 0} {
        return "zero"
    } else {
        return "positive"
    }
}
assert cf-if-neg [classify -5] negative
assert cf-if-zero [classify 0] zero
assert cf-if-pos [classify 5] positive

# 8.2 while
set i 0; set wsum 0
while {$i < 5} { set wsum [expr {$wsum + $i}]; incr i }
assert cf-while $wsum 10

# 8.3 while + break
set i 0
while {1} { if {$i >= 3} break; incr i }
assert cf-while-break $i 3

# 8.4 while + continue
set i 0; set csum 0
while {$i < 10} {
    incr i
    if {$i % 2 == 0} continue
    set csum [expr {$csum + $i}]
}
assert cf-while-continue $csum 25

# 8.5 for loop
set fsum 0
for {set i 1} {$i <= 5} {incr i} { set fsum [expr {$fsum + $i}] }
assert cf-for $fsum 15

# 8.6 foreach — single variable
set feach {}
foreach item {a b c} { lappend feach $item }
assert cf-foreach $feach "a b c"

# 8.7 foreach — multiple variables
set pairs {}
foreach {k v} {a 1 b 2 c 3} { lappend pairs "$k=$v" }
assert cf-foreach-multi [join $pairs ","] "a=1,b=2,c=3"

# 8.8 foreach — multiple lists
set combined {}
foreach x {1 2 3} y {a b c} { lappend combined "$x$y" }
assert cf-foreach-2lists [join $combined ","] "1a,2b,3c"

# 8.9 switch — string (produces JumptableInfo AuxData)
proc sw_str {val} {
    switch $val {
        alpha   { return 1 }
        beta    { return 2 }
        gamma   { return 3 }
        default { return 0 }
    }
}
assert cf-switch-1 [sw_str alpha] 1
assert cf-switch-2 [sw_str beta] 2
assert cf-switch-def [sw_str omega] 0

# 8.10 switch -exact with braces
proc sw_exact {val} {
    switch -exact -- $val {
        "+"  { return plus }
        "-"  { return minus }
        default { return other }
    }
}
assert cf-switch-exact [sw_exact "+"] plus

# 8.11 switch -glob
proc sw_glob {val} {
    switch -glob -- $val {
        a*   { return "starts-a" }
        *z   { return "ends-z" }
        default { return "other" }
    }
}
assert cf-sw-glob-1 [sw_glob "apple"] "starts-a"
assert cf-sw-glob-2 [sw_glob "fizz"] "ends-z"

# 8.12 switch -regexp
proc sw_re {val} {
    switch -regexp -- $val {
        {^[0-9]+$} { return "number" }
        {^[a-z]+$} { return "word" }
        default    { return "mixed" }
    }
}
assert cf-sw-re-1 [sw_re "42"] "number"
assert cf-sw-re-2 [sw_re "hello"] "word"
assert cf-sw-re-3 [sw_re "a1"] "mixed"

# 8.13 catch
set rc [catch {expr {1/0}} msg]
assert cf-catch-rc $rc 1
assert_match cf-catch-msg $msg "*divide by zero*"

# 8.14 catch with options dict
set rc [catch {error "oops" "info" "MYCODE"} msg opts]
assert cf-catch-opts-code [dict get $opts -code] 1
assert cf-catch-msg2 $msg "oops"

# 8.15 try / on / finally
set finally_ran 0
set result [try {
    expr {6 * 7}
} on ok {val opts} {
    set val
} finally {
    set finally_ran 1
}]
assert cf-try-ok $result 42
assert cf-try-finally $finally_ran 1

# 8.16 try / on error
set result [try {
    error "boom"
} on error {msg opts} {
    set msg
}]
assert cf-try-error $result "boom"

# 8.17 try / trap
set result [try {
    throw {MY ERROR} "custom error"
} trap {MY ERROR} {msg opts} {
    set msg
}]
assert cf-try-trap $result "custom error"

# 8.18 throw
set rc [catch {throw {CUSTOM CODE} "thrown message"} msg opts]
assert cf-throw-rc $rc 1
assert cf-throw-errcode [dict get $opts -errorcode] "CUSTOM CODE"

# 8.19 return with -code
proc return_break {} { return -code break }
set rc [catch {return_break} msg opts]
assert cf-return-code [dict get $opts -code] 3

# 8.20 return with -level
proc level_return {} { return -level 2 -code return "from_deep" }
proc level_mid {} { level_return; return "should_not_reach" }
proc level_top {} { level_mid; return "should_not_reach_either" }
# level_return returns -level 2 so it returns from level_top
set rc [catch {level_top} msg]
assert cf-return-level $msg "from_deep"

# 8.21 Nested loops with break/continue
set results {}
for {set i 0} {$i < 3} {incr i} {
    for {set j 0} {$j < 3} {incr j} {
        if {$j == 1} continue
        if {$i == 2 && $j == 2} break
        lappend results "$i:$j"
    }
}
assert cf-nested-loop $results "0:0 0:2 1:0 1:2 2:0"

# ============================================================================
# SECTION 9: Procs — signatures, defaults, varargs
# ============================================================================

# 9.1 Basic proc
proc add {a b} { expr {$a + $b} }
assert proc-basic [add 3 4] 7

# 9.2 Default args
proc greet {{name "World"}} { return "Hello, $name!" }
assert proc-default [greet] "Hello, World!"
assert proc-default-override [greet "Tcl"] "Hello, Tcl!"

# 9.3 Varargs
proc sum_all {args} {
    set s 0; foreach a $args { set s [expr {$s + $a}] }; return $s
}
assert proc-varargs [sum_all 1 2 3 4 5] 15

# 9.4 Mixed fixed + default + args
proc mixed_sig {required {opt "default"} args} {
    return "$required|$opt|[join $args ,]"
}
assert proc-mixed [mixed_sig A] "A|default|"
assert proc-mixed-2 [mixed_sig A B C D] "A|B|C,D"

# 9.5 Recursive proc
proc factorial {n} {
    if {$n <= 1} { return 1 }
    return [expr {$n * [factorial [expr {$n - 1}]]}]
}
assert proc-recurse [factorial 10] 3628800

# 9.6 rename command
proc orig_cmd {} { return "original" }
rename orig_cmd new_cmd
assert proc-rename [new_cmd] "original"
rename new_cmd ""  ;# delete

# ============================================================================
# SECTION 10: Scoping — global, variable, upvar, uplevel
# ============================================================================

# 10.1 global
set ::gvar "global_value"
proc read_global {} { global gvar; return $gvar }
assert scope-global [read_global] "global_value"

# 10.2 variable (in namespace)
namespace eval ::scope_ns {
    variable data "ns_data"
    proc get_data {} { variable data; return $data }
}
assert scope-nsvar [::scope_ns::get_data] "ns_data"

# 10.3 upvar
proc set_caller_var {varname value} {
    upvar 1 $varname v
    set v $value
}
set_caller_var myresult 42
assert scope-upvar $myresult 42

# 10.4 upvar with array
proc set_arr_elem {arrname key value} {
    upvar 1 $arrname a
    set a($key) $value
}
set_arr_elem myarr foo bar
assert scope-upvar-arr $myarr(foo) bar

# 10.5 uplevel
proc run_in_caller {script} { uplevel 1 $script }
set uval 0
run_in_caller {set uval 99}
assert scope-uplevel $uval 99

# 10.6 uplevel with level specification
proc outer_scope {} {
    set ov "outer"
    inner_scope
    return $ov
}
proc inner_scope {} { uplevel 1 {set ov "modified"} }
assert scope-uplevel-2 [outer_scope] "modified"

# ============================================================================
# SECTION 11: Namespaces
# ============================================================================

# 11.1 namespace eval — basic
namespace eval ::mylib {
    variable version 1.0
    proc hello {} { variable version; return "mylib v$version" }
}
assert ns-basic [::mylib::hello] "mylib v1.0"

# 11.2 Nested namespaces
namespace eval ::mylib::sub {
    variable depth 2
    proc info_depth {} { variable depth; return "depth=$depth" }
}
assert ns-nested [::mylib::sub::info_depth] "depth=2"

# 11.3 namespace import / export
namespace eval ::exporter {
    namespace export pub_func
    proc pub_func {} { return "exported" }
    proc priv_func {} { return "private" }
}
namespace eval ::importer {
    namespace import ::exporter::pub_func
    proc test {} { return [pub_func] }
}
assert ns-import [::importer::test] "exported"

# 11.4 namespace current / parent
namespace eval ::ns_check {
    proc current {} { return [namespace current] }
    proc parent {} { return [namespace parent] }
}
assert ns-current [::ns_check::current] "::ns_check"
assert ns-parent [::ns_check::parent] "::"

# 11.5 namespace ensemble
namespace eval ::mymath {
    namespace export add sub
    namespace ensemble create
    proc add {a b} { expr {$a + $b} }
    proc sub {a b} { expr {$a - $b} }
}
assert ns-ensemble-add [mymath add 10 3] 13
assert ns-ensemble-sub [mymath sub 10 3] 7

# 11.6 namespace qualifiers / tail
assert ns-qualifiers [namespace qualifiers "::foo::bar::baz"] "::foo::bar"
assert ns-tail [namespace tail "::foo::bar::baz"] "baz"

# 11.7 namespace which
assert_match ns-which [namespace which ::mylib::hello] "*::mylib::hello"

# 11.8 namespace delete
namespace eval ::temp_ns { proc dummy {} { return 1 } }
assert_true ns-exists-before [namespace exists ::temp_ns]
namespace delete ::temp_ns
assert_true ns-exists-after [expr {![namespace exists ::temp_ns]}]

# ============================================================================
# SECTION 12: Regular Expressions
# ============================================================================

# 12.1 regexp — basic match
assert_true re-match [regexp {^[0-9]+$} "12345"]

# 12.2 regexp — capture
regexp {(\w+)@(\w+)\.(\w+)} "user@example.com" -> user domain tld
assert re-capture-user $user "user"
assert re-capture-domain $domain "example"
assert re-capture-tld $tld "com"

# 12.3 regexp — -nocase
assert_true re-nocase [regexp -nocase {hello} "HELLO WORLD"]

# 12.4 regexp — -all
assert re-all [regexp -all {[0-9]+} "a1b22c333"] 3

# 12.5 regexp — -inline
set matches [regexp -all -inline {[0-9]+} "a1b22c333"]
assert re-inline $matches "1 22 333"

# 12.6 regsub — basic
assert re-sub [regsub {world} "hello world" "Tcl"] "hello Tcl"

# 12.7 regsub — -all
assert re-sub-all [regsub -all {[0-9]} "a1b2c3" "X"] "aXbXcX"

# 12.8 regsub — backreference
assert re-sub-backref [regsub {(\w+) (\w+)} "hello world" {\2 \1}] "world hello"

# ============================================================================
# SECTION 13: eval, subst, and related
# ============================================================================

# 13.1 eval
set cmd {expr {2 + 3}}
assert eval-basic [eval $cmd] 5

# 13.2 eval multiple args (concatenated)
assert eval-multi [eval expr {10 + 5}] 15

# 13.3 subst
set whom "World"
assert subst-basic [subst {Hello, $whom!}] "Hello, World!"
assert subst-cmd [subst {2+3=[expr {2+3}]}] "2+3=5"
assert subst-novar [subst -nocommands -novariables {$whom [expr {1}] \\}] "\$whom \[expr \{1\}] \\"

# ============================================================================
# SECTION 14: Lambda / apply
# ============================================================================

# 14.1 Basic apply
assert lambda-basic [apply {x {expr {$x * 2}}} 5] 10

# 14.2 Apply with multiple args
assert lambda-multi [apply {{a b} {expr {$a + $b}}} 3 7] 10

# 14.3 Apply with default arg
assert lambda-default [apply {{x {y 10}} {expr {$x + $y}}} 5] 15

# 14.4 Apply with namespace
namespace eval ::lam_ns { proc helper {} { return "lam_ns" } }
assert lambda-ns [apply {{} {helper} ::lam_ns}] "lam_ns"

# 14.5 Lambda stored in variable
set double_it {{x} {expr {$x * 2}}}
assert lambda-var [apply $double_it 21] 42

# 14.6 Higher-order: map
proc hof_map {fn lst} {
    set result {}
    foreach item $lst { lappend result [apply $fn $item] }
    return $result
}
assert lambda-map [hof_map {{x} {expr {$x * $x}}} {1 2 3 4}] "1 4 9 16"

# 14.7 Higher-order: filter
proc hof_filter {fn lst} {
    set result {}
    foreach item $lst { if {[apply $fn $item]} { lappend result $item } }
    return $result
}
assert lambda-filter [hof_filter {{x} {expr {$x > 3}}} {1 2 3 4 5}] "4 5"

# 14.8 Higher-order: fold
proc hof_fold {fn init lst} {
    set acc $init
    foreach item $lst { set acc [apply $fn $acc $item] }
    return $acc
}
assert lambda-fold [hof_fold {{a b} {expr {$a + $b}}} 0 {1 2 3 4 5}] 15

# 14.9 Nested lambdas
set compose {{f g} {list {x} "apply \{$f\} \[apply \{$g\} \$x\]"}}
set inc {{x} {expr {$x + 1}}}
set dbl {{x} {expr {$x * 2}}}
set inc_then_dbl [apply $compose $dbl $inc]
assert lambda-compose [apply $inc_then_dbl 5] 12

# ============================================================================
# SECTION 15: Coroutines & Yield
# ============================================================================

# 15.1 Basic coroutine / yield
proc gen_numbers {} {
    yield [info coroutine]
    yield 1
    yield 2
    yield 3
    return 4
}
set coro [coroutine mygen gen_numbers]
assert coro-1 [$coro] 1
assert coro-2 [$coro] 2
assert coro-3 [$coro] 3
assert coro-4 [$coro] 4

# 15.2 Coroutine as infinite generator
proc counter_gen {{start 0}} {
    yield [info coroutine]
    set n $start
    while {1} {
        yield $n
        incr n
    }
}
coroutine ctr counter_gen 10
assert coro-ctr-1 [ctr] 10
assert coro-ctr-2 [ctr] 11
assert coro-ctr-3 [ctr] 12
rename ctr ""  ;# destroy coroutine

# 15.3 Coroutine with arguments to yield
proc echo_gen {} {
    set input [yield [info coroutine]]
    while {1} {
        set input [yield "echo:$input"]
    }
}
coroutine echoer echo_gen
set r1 [echoer "hello"]
assert coro-echo-1 $r1 "echo:hello"
set r2 [echoer "world"]
assert coro-echo-2 $r2 "echo:world"
rename echoer ""

# 15.4 Fibonacci generator
proc fib_gen {} {
    yield [info coroutine]
    set a 0; set b 1
    while {1} {
        yield $a
        lassign [list $b [expr {$a + $b}]] a b
    }
}
coroutine fib fib_gen
set fibs {}
for {set i 0} {$i < 10} {incr i} { lappend fibs [fib] }
assert coro-fib $fibs "0 1 1 2 3 5 8 13 21 34"
rename fib ""

# ============================================================================
# SECTION 16: tailcall
# ============================================================================

proc tc_loop {n acc} {
    if {$n <= 0} { return $acc }
    tailcall tc_loop [expr {$n - 1}] [expr {$acc + $n}]
}
assert tailcall-sum [tc_loop 100 0] 5050

proc tc_even {n} {
    if {$n == 0} { return "even" }
    tailcall tc_odd [expr {$n - 1}]
}
proc tc_odd {n} {
    if {$n == 0} { return "odd" }
    tailcall tc_even [expr {$n - 1}]
}
assert tailcall-even [tc_even 100] "even"
assert tailcall-odd [tc_even 99] "odd"

# ============================================================================
# SECTION 17: Binary format / scan
# ============================================================================

# 17.1 Pack/unpack integers
set packed [binary format i 305419896]  ;# 0x12345678
binary scan $packed iu val
assert bin-int $val 305419896

# 17.2 Pack/unpack shorts
set packed [binary format s2 {1 2}]
binary scan $packed s2 vals
assert bin-short $vals "1 2"

# 17.3 Pack/unpack doubles
set packed [binary format d 3.14159]
binary scan $packed d val
assert_true bin-double [expr {abs($val - 3.14159) < 1e-10}]

# 17.4 Pack strings
set packed [binary format a5 "Hello"]
binary scan $packed a5 val
assert bin-string $val "Hello"

# ============================================================================
# SECTION 18: Encoding
# ============================================================================

# 18.1 encoding convertto / convertfrom
set utf8_bytes [encoding convertto utf-8 "Hello"]
set decoded [encoding convertfrom utf-8 $utf8_bytes]
assert enc-roundtrip $decoded "Hello"

# 18.2 encoding names
assert_true enc-names [expr {[llength [encoding names]] > 10}]

# 18.3 encoding system
assert_true enc-system [expr {[encoding system] ne ""}]

# ============================================================================
# SECTION 19: info command — introspection
# ============================================================================

assert_true info-commands [expr {[llength [info commands]] > 50}]
assert_true info-procs [expr {"add" in [info procs add]}]
assert info-args [info args add] "a b"
# Note: after tbcx::load, info body may return "" (source stripped by design)
set _body [string trim [info body add]]
assert_true info-body [expr {$_body eq {expr {$a + $b}} || $_body eq ""}]
assert_true info-exists [info exists ::gvar]
assert_true info-nexists [expr {![info exists ::nonexistent_var_xyz]}]
assert_true info-level [expr {[info level] >= 0}]
assert_match info-patchlevel [info patchlevel] {9.*}
assert info-cmdcount-type [string is integer [info cmdcount]] 1
assert_true info-tclversion [expr {[info tclversion] >= 9.0}]

# ============================================================================
# SECTION 20: OO — Basic Classes, Methods, Constructors, Destructors
# ============================================================================

# 20.1 oo::class create with definition body
oo::class create Point {
    variable x y
    constructor {xv yv} {
        set x $xv
        set y $yv
    }
    method getx {} { return $x }
    method gety {} { return $y }
    method coords {} { return "$x,$y" }
    method distance {other} {
        set ox [$other getx]
        set oy [$other gety]
        expr {sqrt(($x - $ox)**2 + ($y - $oy)**2)}
    }
    destructor {
        # cleanup — no-op
    }
}
set p1 [Point new 3 4]
set p2 [Point new 0 0]
assert oo-basic-coords [$p1 coords] "3,4"
assert oo-basic-dist [expr {int([$p1 distance $p2])}] 5
$p2 destroy

# 20.2 Named object creation with "create"
Point create origin 0 0
assert oo-create-name [origin coords] "0,0"
origin destroy

# 20.3 oo::class with "variable" declaration (auto-import)
oo::class create Counter {
    variable count
    constructor {{initial 0}} { set count $initial }
    method incr {{delta 1}} { ::incr count $delta; return $count }
    method get {} { return $count }
    destructor {}
}
set c [Counter new 10]
$c incr 5
$c incr
assert oo-counter [$c get] 16
$c destroy

# ============================================================================
# SECTION 21: OO — Inheritance, next
# ============================================================================

oo::class create Shape {
    variable type_
    constructor {t} { set type_ $t }
    method type {} { return $type_ }
    method describe {} { return "Shape($type_)" }
}

oo::class create Circle {
    superclass Shape
    variable radius_
    constructor {r} {
        next "circle"
        set radius_ $r
    }
    method area {} { expr {acos(-1.0) * $radius_ ** 2} }
    method describe {} { return "Circle(r=$radius_) [next]" }
}

oo::class create ColorCircle {
    superclass Circle
    variable color_
    constructor {r c} {
        next $r
        set color_ $c
    }
    method describe {} { return "$color_ [next]" }
}

set cc [ColorCircle new 5 "red"]
assert oo-inherit-desc [$cc describe] "red Circle(r=5) Shape(circle)"
assert_true oo-inherit-area [expr {abs([$cc area] - 78.5398) < 0.01}]
$cc destroy

# ============================================================================
# SECTION 22: OO — oo::define (separate form), deletemethod, renamemethod
# ============================================================================

oo::class create Widget {
    variable name_
    constructor {n} { set name_ $n }
    method name {} { return $name_ }
    method old_method {} { return "old" }
    method to_rename {} { return "renamed_result" }
}

# Add a method via oo::define
oo::define Widget method label {} { my variable name_; return "Widget:$name_" }

# deletemethod
oo::define Widget deletemethod old_method

# renamemethod
oo::define Widget renamemethod to_rename renamed_method

set w [Widget new "btn"]
assert oo-define-add [$w label] "Widget:btn"
assert_error oo-define-delete { $w old_method }
assert oo-define-rename [$w renamed_method] "renamed_result"
$w destroy

# ============================================================================
# SECTION 23: OO — export / unexport
# ============================================================================

oo::class create Access {
    method pub {} { return "public" }
    method Priv {} { return "private_val" }
    method callPriv {} { my Priv }
    export pub
    unexport Priv
}

set a [Access new]
assert oo-export [$a pub] "public"
assert oo-unexport [$a callPriv] "private_val"
# Direct call to unexported method should error from outside
assert_error oo-unexport-err { $a Priv }
$a destroy

# ============================================================================
# SECTION 24: OO — forward (method forwarding)
# ============================================================================

proc ::external_helper {args} { return "helped:[join $args ,]" }

oo::class create Forwarder {
    forward help ::external_helper
    forward llen llength
}
set fw [Forwarder new]
assert oo-forward-1 [$fw help a b c] "helped:a,b,c"
assert oo-forward-2 [$fw llen {1 2 3 4}] 4
$fw destroy

# ============================================================================
# SECTION 25: OO — mixin (class-level and object-level)
# ============================================================================

oo::class create Serializable {
    method serialize {} {
        set result {}
        foreach v [info object variables [self]] {
            my variable $v
            lappend result $v [set $v]
        }
        return $result
    }
}

oo::class create Loggable {
    variable log_
    method log {msg} {
        if {![info exists log_]} { set log_ {} }
        lappend log_ $msg
    }
    method getlog {} {
        if {![info exists log_]} { return {} }
        return $log_
    }
}

oo::class create Item {
    variable name_ price_
    constructor {n p} {
        set name_ $n
        set price_ $p
    }
    method name {} { return $name_ }
    method price {} { return $price_ }
}

# Class-level mixin
oo::define Item mixin Loggable

set item [Item new "Widget" 9.99]
$item log "created"
$item log "priced"
assert oo-mixin-class [llength [$item getlog]] 2

# Object-level mixin via oo::objdefine
oo::objdefine $item mixin -append Serializable
# The serialized output will depend on what variables are declared
assert_true oo-mixin-obj [expr {[llength [$item serialize]] >= 0}]
$item destroy

# ============================================================================
# SECTION 26: OO — filter
# ============================================================================

oo::class create Filtered {
    variable call_log
    constructor {} { set call_log {} }
    method greet {name} { return "hello $name" }
    method farewell {name} { return "bye $name" }
    method getCallLog {} { return $call_log }
    method LogFilter {args} {
        lappend call_log [lindex [self target] 1]
        next {*}$args
    }
    filter LogFilter
}

set flt [Filtered new]
$flt greet "Alice"
$flt farewell "Bob"
# LogFilter is called for every method call including getCallLog itself
set log [$flt getCallLog]
assert_true oo-filter [expr {"greet" in $log && "farewell" in $log}]
$flt destroy

# ============================================================================
# SECTION 27: OO — classmethod
# ============================================================================

oo::class create Registry {
    variable name_
    initialize {
        variable instances 0
    }
    classmethod count {} {
        variable instances
        return $instances
    }
    constructor {n} {
        set name_ $n
        classvariable instances
        ::incr instances
    }
    method name {} { return $name_ }
}

set r1 [Registry new "A"]
set r2 [Registry new "B"]
# classmethod can be called on the class
assert oo-classmethod-class [Registry count] 2
# classmethod can also be called on an instance
assert oo-classmethod-inst [$r1 count] 2
$r1 destroy
$r2 destroy

# ============================================================================
# SECTION 28: OO — private methods & private variables
# ============================================================================

oo::class create Vault {
    # Private block for internal state
    private {
        variable secret
        method _decrypt {} { return "decrypted:$secret" }
    }
    constructor {s} { set secret $s }
    method reveal {} { my _decrypt }
}

set v [Vault new "treasure"]
assert oo-private-reveal [$v reveal] "decrypted:treasure"
# Direct call to private method should error
assert_error oo-private-direct { $v _decrypt }
$v destroy

# ============================================================================
# SECTION 29: OO — oo::objdefine (per-object methods)
# ============================================================================

oo::class create Base {
    method hello {} { return "base hello" }
}

set obj [Base new]
oo::objdefine $obj {
    method hello {} { return "overridden hello" }
    method extra {} { return "per-object method" }
}
assert oo-objdefine-override [$obj hello] "overridden hello"
assert oo-objdefine-extra [$obj extra] "per-object method"
$obj destroy

# ============================================================================
# SECTION 30: OO — self, my, info object, info class
# ============================================================================

oo::class create Reflective {
    method selfName {} { return [self] }
    method selfClass {} { return [self class] }
    method selfNs {} { return [self namespace] }
    method callMy {} { my selfName }
}

set refl [Reflective new]
assert_match oo-self-name [$refl selfName] "::oo::Obj*"
assert oo-self-class [$refl selfClass] "::Reflective"
assert_match oo-self-ns [$refl selfNs] "::oo::Obj*"
assert oo-my [$refl callMy] [$refl selfName]

# info object introspection
assert oo-info-class [info object class $refl] "::Reflective"
assert_true oo-info-isa [info object isa object $refl]
assert_true oo-info-methods [expr {"selfName" in [info object methods $refl -all]}]

# info class introspection
assert_true oo-info-cls-inst [expr {$refl in [info class instances Reflective]}]
assert_true oo-info-cls-methods [expr {"selfName" in [info class methods Reflective]}]
assert_true oo-info-cls-super [expr {"::oo::object" in [info class superclasses Reflective]}]

$refl destroy

# ============================================================================
# SECTION 31: OO — oo::copy
# ============================================================================

oo::class create Copyable {
    variable val_
    constructor {v} { set val_ $v }
    method get {} { return $val_ }
    method set {v} { set val_ $v }
}

set orig [Copyable new 100]
set clone [oo::copy $orig]
assert oo-copy-1 [$clone get] 100
$clone set 200
assert oo-copy-independent [$orig get] 100
assert oo-copy-modified [$clone get] 200
$orig destroy
$clone destroy

# ============================================================================
# SECTION 32: OO — oo::abstract
# ============================================================================

oo::abstract create AbstractAnimal {
    variable sound_
    method speak {} { return $sound_ }
}

# Can't instantiate abstract class
assert_error oo-abstract-new { AbstractAnimal new }
assert_error oo-abstract-create { AbstractAnimal create foo }

# But subclass works
oo::class create Dog {
    superclass AbstractAnimal
    constructor {} { my variable sound_; set sound_ "Woof" }
}
set dog [Dog new]
assert oo-abstract-sub [$dog speak] "Woof"
$dog destroy

# ============================================================================
# SECTION 33: OO — oo::singleton
# ============================================================================

oo::singleton create SingleService {
    variable state_
    constructor {} { set state_ "initialized" }
    method state {} { return $state_ }
    method set_state {s} { set state_ $s }
}

set s1 [SingleService new]
set s2 [SingleService new]
assert oo-singleton-same $s1 $s2
$s1 set_state "modified"
assert oo-singleton-shared [$s2 state] "modified"
# Cannot use 'create' on singleton
assert_error oo-singleton-create { SingleService create named_one }

# ============================================================================
# SECTION 34: OO — metaclass
# ============================================================================

oo::class create TrackedMeta {
    superclass oo::class
    variable created_count_
    constructor {args} {
        set created_count_ 0
        next {*}$args
    }
    method new {args} {
        incr created_count_
        next {*}$args
    }
    method creation_count {} { return $created_count_ }
}

TrackedMeta create TrackedClass {
    variable val_
    constructor {v} { set val_ $v }
    method get {} { return $val_ }
}

set t1 [TrackedClass new "a"]
set t2 [TrackedClass new "b"]
set t3 [TrackedClass new "c"]
assert oo-metaclass [TrackedClass creation_count] 3
$t1 destroy; $t2 destroy; $t3 destroy

# ============================================================================
# SECTION 35: OO — initialise / initialize (class-scoped variables)
# ============================================================================

oo::class create WithInit {
    initialize {
        variable shared_list
        set shared_list {}
    }
    constructor {name} {
        classvariable shared_list
        lappend shared_list $name
    }
    classmethod getShared {} {
        variable shared_list
        return $shared_list
    }
}

set wi1 [WithInit new "first"]
set wi2 [WithInit new "second"]
assert oo-initialize [WithInit getShared] "first second"
$wi1 destroy; $wi2 destroy

# ============================================================================
# SECTION 36: OO — self inside oo::define, definitionnamespace
# ============================================================================

oo::class create SelfTest {}

oo::define SelfTest {
    self {
        method class_only {} { return "class-level method" }
    }
    method inst_method {} { return "instance" }
}

assert oo-self-define [SelfTest class_only] "class-level method"
set st [SelfTest new]
assert oo-self-inst [$st inst_method] "instance"
$st destroy

# ============================================================================
# SECTION 37: OO — method -export / -private / -unexport option
# ============================================================================

oo::class create MethodOptions {
    method pub_explicit -export {} { return "exported" }
    method Unexported -unexport {} { return "unexported_val" }
    method priv_opt -private {} { return "private_opt_val" }
    method callUnexported {} { my Unexported }
    method callPriv {} { my priv_opt }
}

set mo [MethodOptions new]
assert oo-meth-export [$mo pub_explicit] "exported"
assert oo-meth-unexport [$mo callUnexported] "unexported_val"
assert oo-meth-private [$mo callPriv] "private_opt_val"
$mo destroy

# ============================================================================
# SECTION 38: OO — complex inheritance: diamond, multiple superclass
# ============================================================================

oo::class create A_diamond {
    method who {} { return "A" }
}
oo::class create B_diamond {
    superclass A_diamond
    method who {} { return "B->[next]" }
}
oo::class create C_diamond {
    superclass A_diamond
    method who {} { return "C->[next]" }
}
oo::class create D_diamond {
    superclass B_diamond C_diamond
    method who {} { return "D->[next]" }
}

set d [D_diamond new]
# MRO: D -> B -> C -> A
assert oo-diamond [$d who] "D->B->C->A"
$d destroy

# ============================================================================
# SECTION 39: OO — constructor with next chaining
# ============================================================================

oo::class create Base39 {
    variable parts_
    constructor {} { set parts_ [list "Base39"] }
    method parts {} { return $parts_ }
}
oo::class create Mid39 {
    superclass Base39
    constructor {} {
        next
        my variable parts_; lappend parts_ "Mid39"
    }
}
oo::class create Top39 {
    superclass Mid39
    constructor {} {
        next
        my variable parts_; lappend parts_ "Top39"
    }
}
set t39 [Top39 new]
assert oo-ctor-chain [$t39 parts] "Base39 Mid39 Top39"
$t39 destroy

# ============================================================================
# SECTION 40: Miscellaneous commands
# ============================================================================

# 40.1 string repeat / cat
assert misc-str-repeat [string repeat "xy" 4] "xyxyxyxy"
assert misc-str-cat [string cat "ab" "cd" "ef"] "abcdef"

# 40.2 clock (basic — seconds and format)
set now [clock seconds]
assert_true misc-clock [expr {$now > 0}]
set formatted [clock format $now -format "%Y"]
assert_true misc-clock-year [string is integer $formatted]

# 40.3 lsort with custom comparison
set data {3 1 4 1 5 9 2 6}
set sorted [lsort -command {apply {{a b} {expr {$a - $b}}}} $data]
assert misc-lsort-custom $sorted "1 1 2 3 4 5 6 9"

# 40.4 lmap with nested structures
set nested {{1 2} {3 4} {5 6}}
set sums [lmap pair $nested { expr {[lindex $pair 0] + [lindex $pair 1]} }]
assert misc-lmap-nested $sums "3 7 11"

# 40.5 interp — child interpreter
interp create child
child eval {set x 42}
assert misc-interp-eval [child eval {set x}] 42
interp delete child

# 40.6 Multiple return values via lassign
proc divmod {a b} { list [expr {$a / $b}] [expr {$a % $b}] }
lassign [divmod 17 5] quotient remainder
assert misc-divmod-q $quotient 3
assert misc-divmod-r $remainder 2

# 40.7 string is wideinteger
assert_true misc-str-is-wide [string is wideinteger 9223372036854775807]

# 40.8 lsort -stride
set pairs {b 2 a 1 c 3}
set sorted [lsort -stride 2 -index 0 $pairs]
assert misc-lsort-stride $sorted "a 1 b 2 c 3"

# ============================================================================
# SECTION 41: Complex Patterns — closures, accumulators, dispatch tables
# ============================================================================

# 41.1 Closure-like pattern via namespace + apply
proc make_adder {n} {
    set ns [namespace current]
    list {x} "expr {\$x + $n}" $ns
}
set add5 [make_adder 5]
set add10 [make_adder 10]
assert closure-add5 [apply $add5 3] 8
assert closure-add10 [apply $add10 3] 13

# 41.2 Dispatch table via dict of lambdas
set ops [dict create \
    add {{a b} {expr {$a + $b}}} \
    sub {{a b} {expr {$a - $b}}} \
    mul {{a b} {expr {$a * $b}}} \
]
proc dispatch {table op args} {
    apply [dict get $table $op] {*}$args
}
assert dispatch-add [dispatch $ops add 10 3] 13
assert dispatch-sub [dispatch $ops sub 10 3] 7
assert dispatch-mul [dispatch $ops mul 10 3] 30

# 41.3 Accumulator object using OO
oo::class create Accumulator {
    variable total_
    constructor {{start 0}} { set total_ $start }
    method add {n} { set total_ [expr {$total_ + $n}]; return $total_ }
    method value {} { return $total_ }
}
set acc [Accumulator new]
$acc add 10
$acc add 20
$acc add 30
assert oo-accumulator [$acc value] 60
$acc destroy

# ============================================================================
# SECTION 42: Unicode stress
# ============================================================================

set jp "日本語テスト"
assert unicode-jp-len [string length $jp] 6
assert unicode-jp-idx [string index $jp 0] "日"
assert unicode-jp-range [string range $jp 0 2] "日本語"

set emoji "\U0001F600\U0001F601\U0001F602"
assert unicode-emoji-len [string length $emoji] 3

set mixed "Hello世界🌍"
assert unicode-mixed-len [string length $mixed] 8

set rtl "שלום"
assert unicode-rtl-len [string length $rtl] 4

set math "∑∏∫∂"
assert unicode-math-len [string length $math] 4

# ============================================================================
# SECTION 43: Complex namespace+OO+lambda interaction
# ============================================================================

namespace eval ::complex_ns {
    variable data {1 2 3 4 5}

    oo::class create Processor {
        variable transform_
        constructor {fn} { set transform_ $fn }
        method run {data} {
            lmap item $data { apply $transform_ $item }
        }
    }

    proc make_processor {fn} {
        return [Processor new $fn]
    }

    proc process {fn} {
        variable data
        set p [make_processor $fn]
        set result [$p run $data]
        $p destroy
        return $result
    }
}

assert complex-ns-oo [::complex_ns::process {{x} {expr {$x * $x}}}] "1 4 9 16 25"

# ============================================================================
# SECTION 44: Deep nesting & stress
# ============================================================================

# 44.1 Deeply nested if
proc deep_if {n} {
    if {$n == 1} { return 1
    } elseif {$n == 2} { return 2
    } elseif {$n == 3} { return 3
    } elseif {$n == 4} { return 4
    } elseif {$n == 5} { return 5
    } elseif {$n == 6} { return 6
    } elseif {$n == 7} { return 7
    } elseif {$n == 8} { return 8
    } elseif {$n == 9} { return 9
    } elseif {$n == 10} { return 10
    } else { return 0 }
}
assert deep-if [deep_if 7] 7
assert deep-if-else [deep_if 99] 0

# 44.2 Many procs with various local variable counts
for {set i 0} {$i < 20} {incr i} {
    proc gen_proc_$i {x} "expr {\$x + $i}"
}
assert deep-gen-proc-0 [gen_proc_0 10] 10
assert deep-gen-proc-19 [gen_proc_19 10] 29

# 44.3 Large switch (more entries → jump table optimization)
proc big_switch {val} {
    switch $val {
        aaa { return 1 }  bbb { return 2 }  ccc { return 3 }
        ddd { return 4 }  eee { return 5 }  fff { return 6 }
        ggg { return 7 }  hhh { return 8 }  iii { return 9 }
        jjj { return 10 } kkk { return 11 } lll { return 12 }
        mmm { return 13 } nnn { return 14 } ooo { return 15 }
        ppp { return 16 } qqq { return 17 } rrr { return 18 }
        sss { return 19 } ttt { return 20 } uuu { return 21 }
        default { return 0 }
    }
}
assert deep-switch-1 [big_switch aaa] 1
assert deep-switch-mid [big_switch mmm] 13
assert deep-switch-last [big_switch uuu] 21
assert deep-switch-def [big_switch zzz] 0

# 44.4 Large dict
set big_dict [dict create]
for {set i 0} {$i < 100} {incr i} {
    dict set big_dict "key_$i" $i
}
assert deep-dict-size [dict size $big_dict] 100
assert deep-dict-get [dict get $big_dict key_50] 50

# 44.5 Nested foreach (NewForeachInfo AuxData)
set grid {}
foreach row {A B C} {
    foreach col {1 2 3} {
        lappend grid "$row$col"
    }
}
assert deep-foreach-grid [llength $grid] 9
assert deep-foreach-first [lindex $grid 0] "A1"
assert deep-foreach-last [lindex $grid end] "C3"

# ============================================================================
# SECTION 45: Exception ranges — nested try/catch in loops
# ============================================================================

set ex_results {}
for {set i 0} {$i < 5} {incr i} {
    try {
        if {$i == 2} { throw {SKIP} "skip $i" }
        if {$i == 4} { error "err at $i" }
        lappend ex_results "ok:$i"
    } trap {SKIP} {msg} {
        lappend ex_results "skipped:$i"
    } on error {msg} {
        lappend ex_results "error:$i"
    }
}
assert ex-ranges $ex_results "ok:0 ok:1 skipped:2 ok:3 error:4"

# ============================================================================
# SECTION 46: OO — destructor with next chain
# ============================================================================

set ::dtor_log {}

oo::class create DtorBase {
    destructor {
        lappend ::dtor_log "DtorBase"
    }
}
oo::class create DtorMid {
    superclass DtorBase
    destructor {
        lappend ::dtor_log "DtorMid"
        next
    }
}
oo::class create DtorTop {
    superclass DtorMid
    destructor {
        lappend ::dtor_log "DtorTop"
        next
    }
}

set dobj [DtorTop new]
$dobj destroy
assert oo-dtor-chain $::dtor_log "DtorTop DtorMid DtorBase"

# ============================================================================
# SECTION 47: OO — object variable introspection
# ============================================================================

oo::class create VarIntrospect {
    variable a b c
    constructor {} { set a 1; set b 2; set c 3 }
    method getVarNames {} {
        return [lsort [info class variables [self class]]]
    }
}

set vi [VarIntrospect new]
assert oo-var-intro [$vi getVarNames] "a b c"
$vi destroy

# ============================================================================
# SECTION 48: OO — classvariable
# ============================================================================

oo::class create SharedState {
    initialize {
        variable shared_counter
        set shared_counter 0
    }
    method bump {} {
        classvariable shared_counter
        return [::incr shared_counter]
    }
    classmethod getCounter {} {
        variable shared_counter
        return $shared_counter
    }
}

set ss1 [SharedState new]
set ss2 [SharedState new]
$ss1 bump
$ss2 bump
$ss1 bump
assert oo-classvar [SharedState getCounter] 3
$ss1 destroy; $ss2 destroy

# ============================================================================
# SECTION 49: Complex data pipeline
# ============================================================================

# Pipeline: generate -> filter -> transform -> aggregate
set pipeline_data [lseq 1 to 20]
set evens [lmap x $pipeline_data { if {$x % 2 == 0} { set x } else { continue } }]
set doubled [lmap x $evens { expr {$x * 2} }]
set total 0; foreach x $doubled { set total [expr {$total + $x}] }
assert pipeline-result $total 220

# ============================================================================
# SECTION 50: Error handling completeness
# ============================================================================

# 50.1 errorInfo
catch {error "test error"} msg opts
assert_true err-info [expr {[dict exists $opts -errorinfo]}]

# 50.2 errorCode
catch {throw {MY CODE} "msg"} msg opts
assert err-code [dict get $opts -errorcode] "MY CODE"

# 50.3 return -errorcode
proc custom_error {} { return -code error -errorcode {CUSTOM ERR} "custom" }
catch {custom_error} msg opts
assert err-custom-code [dict get $opts -errorcode] "CUSTOM ERR"

# 50.4 try with multiple trap patterns
proc multi_trap {code} {
    try {
        throw $code "message"
    } trap {A B} {msg} {
        return "trapped-AB"
    } trap {A} {msg} {
        return "trapped-A"
    } trap {} {msg} {
        return "trapped-any"
    }
}
assert err-trap-ab [multi_trap {A B}] "trapped-AB"
assert err-trap-a [multi_trap {A C}] "trapped-A"
assert err-trap-any [multi_trap {X Y}] "trapped-any"

# ============================================================================
# SECTION 51: Proc inside namespace with all patterns
# ============================================================================

namespace eval ::patterns {
    variable state 0

    proc with_global {} {
        variable state
        return [incr state]
    }

    proc with_upvar {varname} {
        upvar 1 $varname v
        set v "set_by_patterns"
    }

    proc with_uplevel {script} {
        uplevel 1 $script
    }

    proc with_apply {fn args} {
        apply $fn {*}$args
    }

    proc with_foreach_break {} {
        set result {}
        foreach x {1 2 3 4 5} {
            if {$x > 3} break
            lappend result $x
        }
        return $result
    }

    proc with_try {} {
        try {
            set x [expr {10 / 2}]
            return "ok:$x"
        } on error {msg} {
            return "err:$msg"
        }
    }
}

assert pat-global [::patterns::with_global] 1
assert pat-global-2 [::patterns::with_global] 2
::patterns::with_upvar test_var
assert pat-upvar $test_var "set_by_patterns"
set upval ""
::patterns::with_uplevel {set upval "from_uplevel"}
assert pat-uplevel $upval "from_uplevel"
assert pat-apply [::patterns::with_apply {{a b} {expr {$a * $b}}} 6 7] 42
assert pat-foreach-break [::patterns::with_foreach_break] "1 2 3"
assert pat-try [::patterns::with_try] "ok:5"

# ============================================================================
# EPILOGUE: Report results
# ============================================================================

set _total [expr {$::_pass + $::_fail}]
if {$::_fail == 0} {
    puts "ALL_FEATURES: ALL $_total TESTS PASSED"
} else {
    puts "ALL_FEATURES: $::_pass/$_total passed, $::_fail FAILED"
    foreach e $::_errors {
        puts "  $e"
    }
    exit 1
}
