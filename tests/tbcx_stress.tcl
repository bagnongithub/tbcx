# ======================================================================
# tbcx_stress.tcl — comprehensive TBCX capability exerciser
#
# This script is designed to be:
#   1. Compiled:  tbcx::save tbcx_stress.tcl tbcx_stress.tbcx
#   2. Loaded:    tbcx::load tbcx_stress.tbcx
#   3. Executed:  the load itself runs all tests
#
# It exercises every serialization path in TBCX:
#   - All literal types (string, int, wideint, wideuint, double, boolean,
#     bignum, bytearray, list, dict, bytecode, lambda)
#   - All AuxData types (JumptableInfo, JumptableNumInfo, DictUpdateInfo,
#     NewForeachInfo)
#   - Exception ranges (loop + catch)
#   - Procs (various signatures, namespaces, defaults, varargs)
#   - OO (classes, methods, classmethods, constructors, destructors,
#     builder form, multi-word form, inheritance)
#   - Namespace eval (nested, multi-level)
#   - Control flow (if, while, for, foreach, switch, try/catch/finally,
#     break, continue, return, tailcall)
#   - Lambdas / apply (with and without namespace, shimmer recovery)
#   - Top-level locals and expressions
#   - Unicode strings
#   - Large literal pools, deep nesting, stress patterns
#
# No external libraries.  No terminal input.
# On success, prints a single line: "TBCX_STRESS: ALL <N> TESTS PASSED"
# On failure, prints details and exits with code 1.
# ======================================================================

set ::_pass 0
set ::_fail 0
set ::_errors {}

proc assert {tag got expected} {
    if {$got eq $expected} {
        incr ::_pass
    } else {
        incr ::_fail
        set msg "FAIL $tag: got [string range $got 0 120] expected [string range $expected 0 120]"
        lappend ::_errors $msg
    }
}

proc assert_match {tag got pattern} {
    if {[string match $pattern $got]} {
        incr ::_pass
    } else {
        incr ::_fail
        lappend ::_errors "FAIL $tag: got [string range $got 0 120] !match $pattern"
    }
}

proc assert_true {tag cond} {
    if {$cond} {
        incr ::_pass
    } else {
        incr ::_fail
        lappend ::_errors "FAIL $tag: condition was false"
    }
}

# ======================================================================
# SECTION 1: Literal types
# ======================================================================

# 1.1 Strings
assert lit-str-empty "" ""
assert lit-str-hello "hello world" "hello world"
assert lit-str-special "tab\there" "tab\there"
assert lit-str-newline "line1\nline2" "line1\nline2"
assert lit-str-brace "\{braced\}" "\{braced\}"

# 1.2 Integers (wideint path — fits in 64 bits)
assert lit-int-zero [expr {0}] 0
assert lit-int-pos [expr {42}] 42
assert lit-int-neg [expr {-99}] -99
assert lit-int-large [expr {1000000000}] 1000000000
assert lit-int-max64 [expr {9223372036854775807}] 9223372036854775807
assert lit-int-min64 [expr {-9223372036854775807}] -9223372036854775807

# 1.3 Wide unsigned int (positive values that use wideuint path)
assert lit-uint-1 [expr {4294967296}] 4294967296
assert lit-uint-2 [expr {18446744073709551615 - 18446744073709551614}] 1

# 1.4 Doubles
assert lit-dbl-zero [expr {0.0}] 0.0
assert lit-dbl-pi [expr {3.141592653589793}] 3.141592653589793
assert lit-dbl-neg [expr {-2.718281828}] -2.718281828
assert lit-dbl-sci [expr {1.23e10}] 12300000000.0
assert lit-dbl-tiny [expr {1e-300}] 1e-300

# 1.5 Booleans (Tcl 9.1 preserves the original boolean string form)
assert lit-bool-true [expr {true}] true
assert lit-bool-false [expr {false}] false
assert lit-bool-yes [expr {yes}] yes
assert lit-bool-no [expr {no}] no
# Verify they still behave as booleans in conditions
assert lit-bool-true-cond [expr {true ? "T" : "F"}] "T"
assert lit-bool-false-cond [expr {false ? "T" : "F"}] "F"

# 1.6 Bignums (exceed 64-bit range)
assert lit-big-pos [expr {99999999999999999999999999999 + 1}] 100000000000000000000000000000
assert lit-big-neg [expr {-99999999999999999999999999999 - 1}] -100000000000000000000000000000
assert lit-big-mul [expr {12345678901234567890 * 2}] 24691357802469135780

# 1.7 Byte arrays
set ba [binary format c4 {65 66 67 68}]
assert lit-ba-1 $ba "ABCD"
set ba2 [binary format H8 "deadbeef"]
assert lit-ba-2 [binary scan $ba2 H8 hex; set hex] "deadbeef"

# 1.8 Lists
assert lit-list-empty [list] {}
assert lit-list-simple [list a b c] "a b c"
assert lit-list-nested [list [list 1 2] [list 3 4]] "{1 2} {3 4}"
assert lit-list-len [llength {a b c d e f}] 6

# 1.9 Dicts
assert lit-dict-empty [dict create] {}
assert lit-dict-simple [dict get [dict create a 1 b 2] b] 2
assert lit-dict-nested [dict get [dict create x [dict create y 42]] x y] 42
set d [dict create one 1 two 2 three 3]
assert lit-dict-size [dict size $d] 3
assert lit-dict-keys [lsort [dict keys $d]] "one three two"

# ======================================================================
# SECTION 2: Procs — varied signatures
# ======================================================================

proc add {a b} { expr {$a + $b} }
assert proc-basic [add 3 4] 7

proc greet {{who "world"}} { return "hello $who" }
assert proc-default-used [greet] "hello world"
assert proc-default-override [greet "tcl"] "hello tcl"

proc varsum {args} {
    set s 0
    foreach x $args { set s [expr {$s + $x}] }
    return $s
}
assert proc-varargs [varsum 1 2 3 4 5] 15
assert proc-varargs-0 [varsum] 0

proc mixed {a {b 10} args} {
    return "$a $b [llength $args]"
}
assert proc-mixed-1 [mixed X] "X 10 0"
assert proc-mixed-2 [mixed X 20] "X 20 0"
assert proc-mixed-3 [mixed X 20 a b c] "X 20 3"

proc factorial {n} {
    if {$n <= 1} { return 1 }
    return [expr {$n * [factorial [expr {$n - 1}]]}]
}
assert proc-recursive [factorial 10] 3628800

proc earlyret {x} {
    if {$x < 0} { return "negative" }
    if {$x == 0} { return "zero" }
    return "positive"
}
assert proc-earlyret-neg [earlyret -5] "negative"
assert proc-earlyret-zero [earlyret 0] "zero"
assert proc-earlyret-pos [earlyret 7] "positive"

# ======================================================================
# SECTION 3: Namespace eval — nested, multi-level
# ======================================================================

namespace eval ::nsA {
    proc hello {} { return "nsA::hello" }
    variable counter 0
    proc bump {} { variable counter; incr counter; return $counter }
}
assert ns-basic [::nsA::hello] "nsA::hello"
assert ns-var-1 [::nsA::bump] 1
assert ns-var-2 [::nsA::bump] 2

namespace eval ::nsA::nested {
    proc deep {} { return "nsA::nested::deep" }
}
assert ns-nested [::nsA::nested::deep] "nsA::nested::deep"

namespace eval ::nsB {
    namespace eval inner {
        proc f {} { return "nsB::inner::f" }
    }
}
assert ns-double-nested [::nsB::inner::f] "nsB::inner::f"

namespace eval ::nsC {
    variable data "nsC-data"
    proc get {} { variable data; return $data }
    proc set_data {v} { variable data; set data $v }
}
assert ns-var-get [::nsC::get] "nsC-data"
::nsC::set_data "updated"
assert ns-var-set [::nsC::get] "updated"

# ======================================================================
# SECTION 4: OO — classes, methods, constructors, destructors, inheritance
# ======================================================================

# 4.1 Basic class with pure builder body (my variable inside methods)
oo::class create Animal {
    constructor {n s} {
        my variable name sound
        set name $n
        set sound $s
    }
    method speak {} {
        my variable name sound
        return "$name says $sound"
    }
    method getname {} {
        my variable name
        return $name
    }
}
set a1 [Animal new "Cat" "meow"]
assert oo-basic-speak [$a1 speak] "Cat says meow"
assert oo-basic-name [$a1 getname] "Cat"
$a1 destroy

# 4.2 Inheritance
oo::class create Vehicle {
    constructor {{s 0}} {
        my variable speed
        set speed $s
    }
    method getspeed {} { my variable speed; return $speed }
    method accelerate {delta} { my variable speed; set speed [expr {$speed + $delta}] }
}
oo::class create Car
oo::define Car superclass Vehicle
oo::define Car constructor {{s 0} {d 4}} {
    my variable doors
    next $s
    set doors $d
}
oo::define Car method info {} { my variable doors; return "speed=[my getspeed] doors=$doors" }
set c [Car new 60 2]
assert oo-inherit-info [$c info] "speed=60 doors=2"
$c accelerate 20
assert oo-inherit-accel [$c getspeed] 80
$c destroy

# 4.3 Destructor
set ::dtor_log {}
oo::class create Tracked {
    constructor {id} {
        my variable id_
        set id_ $id
    }
    destructor {
        my variable id_
        lappend ::dtor_log "destroyed:$id_"
    }
    method getid {} { my variable id_; return $id_ }
}
set t1 [Tracked new "A"]
set t2 [Tracked new "B"]
assert oo-dtor-pre $::dtor_log {}
$t1 destroy
assert oo-dtor-one $::dtor_log "destroyed:A"
$t2 destroy
assert oo-dtor-two $::dtor_log "destroyed:A destroyed:B"

# 4.4 oo::define multi-word form
oo::class create Shape
oo::define Shape constructor {{type "unknown"}} {
    my variable type_
    set type_ $type
}
oo::define Shape method gettype {} {
    my variable type_
    return $type_
}
oo::define Shape method describe {} {
    return "Shape([my gettype])"
}
set s1 [Shape new "circle"]
assert oo-multiword [$s1 describe] "Shape(circle)"
$s1 destroy

# 4.5 Counter class
oo::class create Counter {
    constructor {{init 0}} {
        my variable val_
        set val_ $init
    }
    method get {} { my variable val_; return $val_ }
    method incr {} { my variable val_; ::incr val_; return $val_ }
}
set ct [Counter new 10]
assert oo-counter-1 [$ct get] 10
$ct incr; $ct incr; $ct incr
assert oo-counter-2 [$ct get] 13
$ct destroy

# 4.6 Mixin (no library needed — plain oo::define)
oo::class create Describable {
    method describe {} {
        return "I am [self]"
    }
}
oo::class create Config {
    constructor {h p} {
        my variable host port
        set host $h
        set port $p
    }
    method gethost {} { my variable host; return $host }
    method getport {} { my variable port; return $port }
}
oo::define Config mixin Describable
set cfg [Config new "localhost" 8080]
assert oo-mixin-host [$cfg gethost] "localhost"
assert oo-mixin-port [$cfg getport] 8080
# The describe method comes from the mixin
assert_match oo-mixin-describe [$cfg describe] "I am *"
$cfg destroy

# ======================================================================
# SECTION 5: Control flow — if, while, for, foreach, switch, try, etc.
# ======================================================================

# 5.1 if/elseif/else
proc classify {x} {
    if {$x < 0} { return "neg" } elseif {$x == 0} { return "zero" } else { return "pos" }
}
assert cf-if-neg [classify -1] "neg"
assert cf-if-zero [classify 0] "zero"
assert cf-if-pos [classify 1] "pos"

# 5.2 while + break
proc while_break {n} {
    set i 0; set s 0
    while {$i < $n} {
        if {$s > 100} break
        set s [expr {$s + $i}]
        incr i
    }
    return $s
}
assert cf-while-1 [while_break 10] 45
assert cf-while-break [while_break 1000] 105

# 5.3 while + continue
proc sum_odd {n} {
    set i 0; set s 0
    while {$i < $n} {
        incr i
        if {$i % 2 == 0} continue
        set s [expr {$s + $i}]
    }
    return $s
}
assert cf-while-cont [sum_odd 10] 25

# 5.4 for loop
proc for_sum {n} {
    set s 0
    for {set i 0} {$i < $n} {incr i} {
        set s [expr {$s + $i}]
    }
    return $s
}
assert cf-for [for_sum 10] 45

# 5.5 foreach — single variable (NewForeachInfo AuxData)
proc foreach_join {lst sep} {
    set res ""
    foreach x $lst {
        if {$res ne ""} { append res $sep }
        append res $x
    }
    return $res
}
assert cf-foreach [foreach_join {a b c d} ","] "a,b,c,d"

# 5.6 foreach — multiple variables
proc foreach_pairs {lst} {
    set res {}
    foreach {a b} $lst {
        lappend res "$a=$b"
    }
    return $res
}
assert cf-foreach-pair [foreach_pairs {x 1 y 2 z 3}] "x=1 y=2 z=3"

# 5.7 foreach — multiple lists
proc foreach_zip {l1 l2} {
    set res {}
    foreach a $l1 b $l2 {
        lappend res "$a:$b"
    }
    return $res
}
assert cf-foreach-zip [foreach_zip {a b c} {1 2 3}] "a:1 b:2 c:3"

# 5.8 switch — string (JumptableInfo AuxData)
proc color_code {c} {
    switch $c {
        red     { return "#FF0000" }
        green   { return "#00FF00" }
        blue    { return "#0000FF" }
        default { return "#UNKNOWN" }
    }
}
assert cf-switch-str-1 [color_code red] "#FF0000"
assert cf-switch-str-2 [color_code blue] "#0000FF"
assert cf-switch-str-3 [color_code purple] "#UNKNOWN"

# 5.9 try/catch/finally
proc safe_div {a b} {
    try {
        set r [expr {$a / $b}]
        return "ok:$r"
    } on error {msg} {
        return "err:$msg"
    } finally {
        # no-op, but exercises the finally path
    }
}
assert cf-try-ok [safe_div 10 3] "ok:3"
assert_match cf-try-err [safe_div 10 0] "err:*zero*"

# 5.10 try with throw
proc check_range {x} {
    try {
        if {$x < 0 || $x > 100} {
            throw {RANGE ERROR} "out of range: $x"
        }
        return "ok:$x"
    } on error {msg opts} {
        return "caught:[dict get $opts -errorcode]"
    }
}
assert cf-throw-ok [check_range 50] "ok:50"
assert cf-throw-err [check_range 200] "caught:RANGE ERROR"

# 5.11 Nested loops with break/continue
proc matrix_search {rows cols target} {
    set val 0
    for {set r 0} {$r < $rows} {incr r} {
        for {set c 0} {$c < $cols} {incr c} {
            incr val
            if {$val == $target} {
                return "found:$r,$c"
            }
        }
    }
    return "not-found"
}
assert cf-nested-loop [matrix_search 5 5 13] "found:2,2"
assert cf-nested-notfound [matrix_search 3 3 100] "not-found"

# ======================================================================
# SECTION 6: Lambdas / apply
# ======================================================================

# 6.1 Basic apply
assert lambda-basic [apply {x {expr {$x * 2}}} 21] 42

# 6.2 Apply with multiple args
assert lambda-multi [apply {{a b} {expr {$a + $b}}} 10 32] 42

# 6.3 Apply with default
assert lambda-default [apply {{x {y 10}} {expr {$x + $y}}} 5] 15
assert lambda-default-override [apply {{x {y 10}} {expr {$x + $y}}} 5 20] 25

# 6.4 Lambda stored in variable
set double {x {expr {$x * 2}}}
set triple {x {expr {$x * 3}}}
assert lambda-var-1 [apply $double 7] 14
assert lambda-var-2 [apply $triple 7] 21

# 6.5 Lambda with namespace
namespace eval ::lns {
    variable factor 10
}
# Note: namespace-qualified lambdas use 3-element form
set nslam {{x} {variable factor; expr {$x * $factor}} ::lns}
assert lambda-ns [apply $nslam 5] 50

# 6.6 Higher-order: map via apply
proc lmap_apply {fn lst} {
    set res {}
    foreach x $lst {
        lappend res [apply $fn $x]
    }
    return $res
}
assert lambda-map [lmap_apply {x {expr {$x * $x}}} {1 2 3 4 5}] "1 4 9 16 25"

# 6.7 Higher-order: filter
proc lfilter {fn lst} {
    set res {}
    foreach x $lst {
        if {[apply $fn $x]} { lappend res $x }
    }
    return $res
}
assert lambda-filter [lfilter {x {expr {$x > 3}}} {1 2 3 4 5 6}] "4 5 6"

# 6.8 Higher-order: reduce/fold
proc lfold {fn init lst} {
    set acc $init
    foreach x $lst {
        set acc [apply $fn $acc $x]
    }
    return $acc
}
assert lambda-fold [lfold {{a b} {expr {$a + $b}}} 0 {1 2 3 4 5}] 15
assert lambda-fold-mul [lfold {{a b} {expr {$a * $b}}} 1 {1 2 3 4 5}] 120

# 6.9 Lambda shimmer stress — force re-use of lambda objects
proc lambda_shimmer_test {} {
    set fn {x {expr {$x + 1}}}
    set results {}
    for {set i 0} {$i < 20} {incr i} {
        lappend results [apply $fn $i]
        # Force shimmer by treating fn as a string
        string length $fn
    }
    return [llength $results]
}
assert lambda-shimmer [lambda_shimmer_test] 20

# ======================================================================
# SECTION 7: Expressions — math, string ops, type coercion
# ======================================================================

assert expr-arith [expr {2 + 3 * 4}] 14
assert expr-paren [expr {(2 + 3) * 4}] 20
assert expr-mod [expr {17 % 5}] 2
assert expr-div [expr {17 / 5}] 3
assert expr-fdiv [expr {17.0 / 5}] 3.4
assert expr-pow [expr {2 ** 10}] 1024
assert expr-neg [expr {-(3 + 4)}] -7
assert expr-bit-and [expr {0xFF & 0x0F}] 15
assert expr-bit-or [expr {0xF0 | 0x0F}] 255
assert expr-bit-xor [expr {0xFF ^ 0x0F}] 240
assert expr-shift-l [expr {1 << 10}] 1024
assert expr-shift-r [expr {1024 >> 3}] 128
assert expr-ternary [expr {1 > 0 ? "yes" : "no"}] "yes"
assert expr-bool-and [expr {1 && 1}] 1
assert expr-bool-or [expr {0 || 1}] 1
assert expr-bool-not [expr {!0}] 1
assert expr-eq [expr {"abc" eq "abc"}] 1
assert expr-ne [expr {"abc" ne "def"}] 1
assert expr-lt [expr {3 < 5}] 1
assert expr-le [expr {5 <= 5}] 1
assert expr-abs [expr {abs(-42)}] 42
assert expr-min [expr {min(3, 7)}] 3
assert expr-max [expr {max(3, 7)}] 7
assert expr-round [expr {round(3.7)}] 4
assert expr-int-fn [expr {int(3.9)}] 3
assert expr-double-fn [expr {double(42)}] 42.0

# ======================================================================
# SECTION 8: Dict operations — dict update triggers DictUpdateInfo AuxData
# ======================================================================

proc dict_update_test {d} {
    dict update d name n age a {
        set n [string toupper $n]
        set a [expr {$a + 1}]
    }
    return $d
}
set du [dict_update_test [dict create name "alice" age 30]]
assert dict-upd-name [dict get $du name] "ALICE"
assert dict-upd-age [dict get $du age] 31

proc dict_with_test {d} {
    dict with d {
        return "$x:$y"
    }
}
assert dict-with [dict_with_test [dict create x 10 y 20]] "10:20"

proc dict_lappend_test {} {
    set d [dict create tags {}]
    dict lappend d tags "a" "b" "c"
    return [dict get $d tags]
}
assert dict-lappend [dict_lappend_test] "a b c"

proc dict_incr_test {} {
    set d [dict create hits 0]
    for {set i 0} {$i < 5} {incr i} {
        dict incr d hits
    }
    return [dict get $d hits]
}
assert dict-incr [dict_incr_test] 5

proc dict_for_test {d} {
    set keys {}
    dict for {k v} $d {
        lappend keys $k
    }
    return [lsort $keys]
}
assert dict-for [dict_for_test [dict create c 3 a 1 b 2]] "a b c"

# Nested dict operations
proc nested_dict {} {
    set d [dict create]
    dict set d users alice age 30
    dict set d users alice role "admin"
    dict set d users bob age 25
    dict set d users bob role "user"
    return [dict get $d users alice role]
}
assert dict-nested [nested_dict] "admin"

# ======================================================================
# SECTION 9: Unicode strings
# ======================================================================

set uniA "caf\u00E9"
assert uni-cafe $uniA "caf\u00E9"
assert uni-len [string length $uniA] 4

set uniB "\u00FCber"
assert uni-uber $uniB "\u00FCber"

set uniC "\u4e16\u754c"
assert uni-cjk [string length $uniC] 2

set uniD "\U0001F600"
assert uni-emoji [string length $uniD] 1

set uniE "na\u00EFve"
assert uni-naive $uniE "na\u00EFve"

proc uni_reverse {s} {
    set r ""
    for {set i [expr {[string length $s] - 1}]} {$i >= 0} {incr i -1} {
        append r [string index $s $i]
    }
    return $r
}
assert uni-reverse [uni_reverse "abc"] "cba"

# ======================================================================
# SECTION 10: Numeric patterns & large integer operations
# ======================================================================

proc fib {n} {
    set a 0; set b 1
    for {set i 0} {$i < $n} {incr i} {
        set t $b
        set b [expr {$a + $b}]
        set a $t
    }
    return $a
}
assert fib-0 [fib 0] 0
assert fib-1 [fib 1] 1
assert fib-10 [fib 10] 55
assert fib-20 [fib 20] 6765
assert fib-30 [fib 30] 832040

# Large integer arithmetic (bignum path)
proc big_power {base exp} {
    set result 1
    for {set i 0} {$i < $exp} {incr i} {
        set result [expr {$result * $base}]
    }
    return $result
}
assert big-pow-2-64 [big_power 2 64] 18446744073709551616
assert big-pow-10-20 [big_power 10 20] 100000000000000000000

# ======================================================================
# SECTION 11: String operations (many patterns, builds literal pool)
# ======================================================================

proc str_repeat {s n} {
    set r ""
    for {set i 0} {$i < $n} {incr i} { append r $s }
    return $r
}
assert str-repeat [str_repeat "ab" 4] "abababab"

assert str-range [string range "hello world" 6 10] "world"
assert str-index [string index "abcdef" 3] "d"
assert str-first [string first "ll" "hello world"] 2
assert str-last [string last "l" "hello world"] 9
assert str-map [string map {a A e E i I o O u U} "hello"] "hEllO"
assert str-toupper [string toupper "hello"] "HELLO"
assert str-tolower [string tolower "HELLO"] "hello"
assert str-trim [string trim "  hello  "] "hello"
assert str-triml [string trimleft "  hello  "] "hello  "
assert str-trimr [string trimright "  hello  "] "  hello"
assert str-replace [string replace "hello world" 5 5 "_"]  "hello_world"
assert str-reverse [string reverse "abcde"] "edcba"
assert str-is-int [string is integer 42] 1
assert str-is-alpha [string is alpha "hello"] 1
assert str-is-digit [string is digit "12345"] 1
assert str-match [string match "h*d" "hello world"] 1
assert str-wordend [string wordend "hello world" 0] 5

# ======================================================================
# SECTION 12: List operations
# ======================================================================

assert list-lindex [lindex {a b c d e} 2] "c"
assert list-lrange [lrange {a b c d e} 1 3] "b c d"
assert list-linsert [linsert {a b c} 1 X] "a X b c"
assert list-lreplace [lreplace {a b c d e} 1 2 X Y] "a X Y d e"
assert list-lsort [lsort {c a b e d}] "a b c d e"
assert list-lsort-int [lsort -integer {3 1 4 1 5 9}] "1 1 3 4 5 9"
assert list-lsort-dec [lsort -decreasing {1 2 3}] "3 2 1"
assert list-lsearch [lsearch {a b c d e} "c"] 2
assert list-lsearch-glob [lsearch -glob {apple banana cherry} "b*"] 1
assert list-lreverse [lreverse {1 2 3 4 5}] "5 4 3 2 1"
assert list-concat [concat {a b} {c d}] "a b c d"
assert list-join [join {a b c} ","] "a,b,c"
assert list-split [split "a,b,c" ","] "a b c"
assert list-lrepeat [lrepeat 3 "x"] "x x x"
# Real lassign test:
set _rest [lassign {1 2 3 4 5} _a _b _c]
assert list-lassign-a $_a 1
assert list-lassign-b $_b 2
assert list-lassign-c $_c 3
assert list-lassign-rest $_rest "4 5"

# ======================================================================
# SECTION 13: Complex OO patterns — deep hierarchies, mixins, delegation
# ======================================================================

# 13.1 Three-level inheritance
oo::class create Base13 {
    constructor {{v 0}} { my variable val_; set val_ $v }
    method getval {} { my variable val_; return $val_ }
}
oo::class create Mid13
oo::define Mid13 superclass Base13
oo::define Mid13 constructor {{v 0}} { next $v }
oo::define Mid13 method doubled {} { return [expr {[my getval] * 2}] }

oo::class create Leaf13
oo::define Leaf13 superclass Mid13
oo::define Leaf13 constructor {{v 0}} { next $v }
oo::define Leaf13 method tripled {} { return [expr {[my getval] * 3}] }
set l13 [Leaf13 new 7]
assert oo-3level-val [$l13 getval] 7
assert oo-3level-dbl [$l13 doubled] 14
assert oo-3level-tri [$l13 tripled] 21
$l13 destroy

# 13.2 Stack class
oo::class create Stack {
    constructor {} { my variable items_; set items_ {} }
    method push {v} { my variable items_; lappend items_ $v }
    method pop {} {
        my variable items_
        set v [lindex $items_ end]
        set items_ [lrange $items_ 0 end-1]
        return $v
    }
    method peek {} { my variable items_; return [lindex $items_ end] }
    method size {} { my variable items_; return [llength $items_] }
    method tolist {} { my variable items_; return $items_ }
}
set stk [Stack new]
$stk push 10; $stk push 20; $stk push 30
assert oo-stack-size [$stk size] 3
assert oo-stack-peek [$stk peek] 30
assert oo-stack-pop [$stk pop] 30
assert oo-stack-size2 [$stk size] 2
assert oo-stack-list [$stk tolist] "10 20"
$stk destroy

# 13.3 Queue class
oo::class create Queue {
    constructor {} { my variable items_; set items_ {} }
    method enqueue {v} { my variable items_; lappend items_ $v }
    method dequeue {} {
        my variable items_
        set v [lindex $items_ 0]
        set items_ [lrange $items_ 1 end]
        return $v
    }
    method size {} { my variable items_; return [llength $items_] }
    method empty {} { my variable items_; return [expr {[llength $items_] == 0}] }
}
set q [Queue new]
$q enqueue "first"; $q enqueue "second"; $q enqueue "third"
assert oo-queue-size [$q size] 3
assert oo-queue-deq [$q dequeue] "first"
assert oo-queue-deq2 [$q dequeue] "second"
assert oo-queue-size2 [$q size] 1
$q destroy

# ======================================================================
# SECTION 14: Procs inside namespaces with all control-flow patterns
# ======================================================================

namespace eval ::algo {
    proc bubblesort {lst} {
        set n [llength $lst]
        for {set i 0} {$i < $n} {incr i} {
            for {set j 0} {$j < [expr {$n - $i - 1}]} {incr j} {
                set a [lindex $lst $j]
                set b [lindex $lst [expr {$j + 1}]]
                if {$a > $b} {
                    set lst [lreplace $lst $j [expr {$j + 1}] $b $a]
                }
            }
        }
        return $lst
    }

    proc binary_search {lst target} {
        set lo 0
        set hi [expr {[llength $lst] - 1}]
        while {$lo <= $hi} {
            set mid [expr {($lo + $hi) / 2}]
            set v [lindex $lst $mid]
            if {$v == $target} { return $mid }
            if {$v < $target} { set lo [expr {$mid + 1}] } else { set hi [expr {$mid - 1}] }
        }
        return -1
    }

    proc gcd {a b} {
        while {$b != 0} {
            set t $b
            set b [expr {$a % $b}]
            set a $t
        }
        return $a
    }

    proc prime {n} {
        if {$n < 2} { return 0 }
        if {$n < 4} { return 1 }
        if {$n % 2 == 0} { return 0 }
        for {set i 3} {$i * $i <= $n} {incr i 2} {
            if {$n % $i == 0} { return 0 }
        }
        return 1
    }

    proc sieve {max} {
        set primes {}
        for {set i 2} {$i <= $max} {incr i} {
            if {[prime $i]} { lappend primes $i }
        }
        return $primes
    }
}

assert algo-bubble [::algo::bubblesort {5 3 8 1 9 2}] "1 2 3 5 8 9"
set sorted {1 2 3 5 8 9 12 15 20}
assert algo-bsearch-found [::algo::binary_search $sorted 8] 4
assert algo-bsearch-notfound [::algo::binary_search $sorted 7] -1
assert algo-gcd [::algo::gcd 48 18] 6
assert algo-prime-7 [::algo::prime 7] 1
assert algo-prime-10 [::algo::prime 10] 0
assert algo-sieve [::algo::sieve 20] "2 3 5 7 11 13 17 19"

# ======================================================================
# SECTION 15: Bulk generated tests — arithmetic, string, list, dict
# ======================================================================

proc _arith_0 {} { return [expr {3 + 11}] }
assert bulk-arith-0 [_arith_0] 14

proc _arith_1 {} { return [expr {10 + 24}] }
assert bulk-arith-1 [_arith_1] 34

proc _arith_2 {} { return [expr {17 + 37}] }
assert bulk-arith-2 [_arith_2] 54

proc _arith_3 {} { return [expr {24 + 50}] }
assert bulk-arith-3 [_arith_3] 74

proc _arith_4 {} { return [expr {31 + 63}] }
assert bulk-arith-4 [_arith_4] 94

proc _arith_5 {} { return [expr {38 + 76}] }
assert bulk-arith-5 [_arith_5] 114

proc _arith_6 {} { return [expr {45 + 89}] }
assert bulk-arith-6 [_arith_6] 134

proc _arith_7 {} { return [expr {52 + 102}] }
assert bulk-arith-7 [_arith_7] 154

proc _arith_8 {} { return [expr {59 + 115}] }
assert bulk-arith-8 [_arith_8] 174

proc _arith_9 {} { return [expr {66 + 128}] }
assert bulk-arith-9 [_arith_9] 194

proc _arith_10 {} { return [expr {73 + 141}] }
assert bulk-arith-10 [_arith_10] 214

proc _arith_11 {} { return [expr {80 + 154}] }
assert bulk-arith-11 [_arith_11] 234

proc _arith_12 {} { return [expr {87 + 167}] }
assert bulk-arith-12 [_arith_12] 254

proc _arith_13 {} { return [expr {94 + 180}] }
assert bulk-arith-13 [_arith_13] 274

proc _arith_14 {} { return [expr {101 + 193}] }
assert bulk-arith-14 [_arith_14] 294

proc _arith_15 {} { return [expr {108 + 206}] }
assert bulk-arith-15 [_arith_15] 314

proc _arith_16 {} { return [expr {115 + 219}] }
assert bulk-arith-16 [_arith_16] 334

proc _arith_17 {} { return [expr {122 + 232}] }
assert bulk-arith-17 [_arith_17] 354

proc _arith_18 {} { return [expr {129 + 245}] }
assert bulk-arith-18 [_arith_18] 374

proc _arith_19 {} { return [expr {136 + 258}] }
assert bulk-arith-19 [_arith_19] 394

proc _arith_20 {} { return [expr {143 + 271}] }
assert bulk-arith-20 [_arith_20] 414

proc _arith_21 {} { return [expr {150 + 284}] }
assert bulk-arith-21 [_arith_21] 434

proc _arith_22 {} { return [expr {157 + 297}] }
assert bulk-arith-22 [_arith_22] 454

proc _arith_23 {} { return [expr {164 + 310}] }
assert bulk-arith-23 [_arith_23] 474

proc _arith_24 {} { return [expr {171 + 323}] }
assert bulk-arith-24 [_arith_24] 494

proc _arith_25 {} { return [expr {178 + 336}] }
assert bulk-arith-25 [_arith_25] 514

proc _arith_26 {} { return [expr {185 + 349}] }
assert bulk-arith-26 [_arith_26] 534

proc _arith_27 {} { return [expr {192 + 362}] }
assert bulk-arith-27 [_arith_27] 554

proc _arith_28 {} { return [expr {199 + 375}] }
assert bulk-arith-28 [_arith_28] 574

proc _arith_29 {} { return [expr {206 + 388}] }
assert bulk-arith-29 [_arith_29] 594

proc _arith_30 {} { return [expr {213 + 401}] }
assert bulk-arith-30 [_arith_30] 614

proc _arith_31 {} { return [expr {220 + 414}] }
assert bulk-arith-31 [_arith_31] 634

proc _arith_32 {} { return [expr {227 + 427}] }
assert bulk-arith-32 [_arith_32] 654

proc _arith_33 {} { return [expr {234 + 440}] }
assert bulk-arith-33 [_arith_33] 674

proc _arith_34 {} { return [expr {241 + 453}] }
assert bulk-arith-34 [_arith_34] 694

proc _arith_35 {} { return [expr {248 + 466}] }
assert bulk-arith-35 [_arith_35] 714

proc _arith_36 {} { return [expr {255 + 479}] }
assert bulk-arith-36 [_arith_36] 734

proc _arith_37 {} { return [expr {262 + 492}] }
assert bulk-arith-37 [_arith_37] 754

proc _arith_38 {} { return [expr {269 + 6}] }
assert bulk-arith-38 [_arith_38] 275

proc _arith_39 {} { return [expr {276 + 19}] }
assert bulk-arith-39 [_arith_39] 295

proc _arith_40 {} { return [expr {283 + 32}] }
assert bulk-arith-40 [_arith_40] 315

proc _arith_41 {} { return [expr {290 + 45}] }
assert bulk-arith-41 [_arith_41] 335

proc _arith_42 {} { return [expr {297 + 58}] }
assert bulk-arith-42 [_arith_42] 355

proc _arith_43 {} { return [expr {304 + 71}] }
assert bulk-arith-43 [_arith_43] 375

proc _arith_44 {} { return [expr {311 + 84}] }
assert bulk-arith-44 [_arith_44] 395

proc _arith_45 {} { return [expr {318 + 97}] }
assert bulk-arith-45 [_arith_45] 415

proc _arith_46 {} { return [expr {325 + 110}] }
assert bulk-arith-46 [_arith_46] 435

proc _arith_47 {} { return [expr {332 + 123}] }
assert bulk-arith-47 [_arith_47] 455

proc _arith_48 {} { return [expr {339 + 136}] }
assert bulk-arith-48 [_arith_48] 475

proc _arith_49 {} { return [expr {346 + 149}] }
assert bulk-arith-49 [_arith_49] 495

proc _arith_50 {} { return [expr {353 + 162}] }
assert bulk-arith-50 [_arith_50] 515

proc _arith_51 {} { return [expr {360 + 175}] }
assert bulk-arith-51 [_arith_51] 535

proc _arith_52 {} { return [expr {367 + 188}] }
assert bulk-arith-52 [_arith_52] 555

proc _arith_53 {} { return [expr {374 + 201}] }
assert bulk-arith-53 [_arith_53] 575

proc _arith_54 {} { return [expr {381 + 214}] }
assert bulk-arith-54 [_arith_54] 595

proc _arith_55 {} { return [expr {388 + 227}] }
assert bulk-arith-55 [_arith_55] 615

proc _arith_56 {} { return [expr {395 + 240}] }
assert bulk-arith-56 [_arith_56] 635

proc _arith_57 {} { return [expr {402 + 253}] }
assert bulk-arith-57 [_arith_57] 655

proc _arith_58 {} { return [expr {409 + 266}] }
assert bulk-arith-58 [_arith_58] 675

proc _arith_59 {} { return [expr {416 + 279}] }
assert bulk-arith-59 [_arith_59] 695

proc _arith_60 {} { return [expr {423 + 292}] }
assert bulk-arith-60 [_arith_60] 715

proc _arith_61 {} { return [expr {430 + 305}] }
assert bulk-arith-61 [_arith_61] 735

proc _arith_62 {} { return [expr {437 + 318}] }
assert bulk-arith-62 [_arith_62] 755

proc _arith_63 {} { return [expr {444 + 331}] }
assert bulk-arith-63 [_arith_63] 775

proc _arith_64 {} { return [expr {451 + 344}] }
assert bulk-arith-64 [_arith_64] 795

proc _arith_65 {} { return [expr {458 + 357}] }
assert bulk-arith-65 [_arith_65] 815

proc _arith_66 {} { return [expr {465 + 370}] }
assert bulk-arith-66 [_arith_66] 835

proc _arith_67 {} { return [expr {472 + 383}] }
assert bulk-arith-67 [_arith_67] 855

proc _arith_68 {} { return [expr {479 + 396}] }
assert bulk-arith-68 [_arith_68] 875

proc _arith_69 {} { return [expr {486 + 409}] }
assert bulk-arith-69 [_arith_69] 895

proc _arith_70 {} { return [expr {493 + 422}] }
assert bulk-arith-70 [_arith_70] 915

proc _arith_71 {} { return [expr {500 + 435}] }
assert bulk-arith-71 [_arith_71] 935

proc _arith_72 {} { return [expr {507 + 448}] }
assert bulk-arith-72 [_arith_72] 955

proc _arith_73 {} { return [expr {514 + 461}] }
assert bulk-arith-73 [_arith_73] 975

proc _arith_74 {} { return [expr {521 + 474}] }
assert bulk-arith-74 [_arith_74] 995

proc _arith_75 {} { return [expr {528 + 487}] }
assert bulk-arith-75 [_arith_75] 1015

proc _arith_76 {} { return [expr {535 + 1}] }
assert bulk-arith-76 [_arith_76] 536

proc _arith_77 {} { return [expr {542 + 14}] }
assert bulk-arith-77 [_arith_77] 556

proc _arith_78 {} { return [expr {549 + 27}] }
assert bulk-arith-78 [_arith_78] 576

proc _arith_79 {} { return [expr {556 + 40}] }
assert bulk-arith-79 [_arith_79] 596

proc _arith_80 {} { return [expr {563 + 53}] }
assert bulk-arith-80 [_arith_80] 616

proc _arith_81 {} { return [expr {570 + 66}] }
assert bulk-arith-81 [_arith_81] 636

proc _arith_82 {} { return [expr {577 + 79}] }
assert bulk-arith-82 [_arith_82] 656

proc _arith_83 {} { return [expr {584 + 92}] }
assert bulk-arith-83 [_arith_83] 676

proc _arith_84 {} { return [expr {591 + 105}] }
assert bulk-arith-84 [_arith_84] 696

proc _arith_85 {} { return [expr {598 + 118}] }
assert bulk-arith-85 [_arith_85] 716

proc _arith_86 {} { return [expr {605 + 131}] }
assert bulk-arith-86 [_arith_86] 736

proc _arith_87 {} { return [expr {612 + 144}] }
assert bulk-arith-87 [_arith_87] 756

proc _arith_88 {} { return [expr {619 + 157}] }
assert bulk-arith-88 [_arith_88] 776

proc _arith_89 {} { return [expr {626 + 170}] }
assert bulk-arith-89 [_arith_89] 796

proc _arith_90 {} { return [expr {633 + 183}] }
assert bulk-arith-90 [_arith_90] 816

proc _arith_91 {} { return [expr {640 + 196}] }
assert bulk-arith-91 [_arith_91] 836

proc _arith_92 {} { return [expr {647 + 209}] }
assert bulk-arith-92 [_arith_92] 856

proc _arith_93 {} { return [expr {654 + 222}] }
assert bulk-arith-93 [_arith_93] 876

proc _arith_94 {} { return [expr {661 + 235}] }
assert bulk-arith-94 [_arith_94] 896

proc _arith_95 {} { return [expr {668 + 248}] }
assert bulk-arith-95 [_arith_95] 916

proc _arith_96 {} { return [expr {675 + 261}] }
assert bulk-arith-96 [_arith_96] 936

proc _arith_97 {} { return [expr {682 + 274}] }
assert bulk-arith-97 [_arith_97] 956

proc _arith_98 {} { return [expr {689 + 287}] }
assert bulk-arith-98 [_arith_98] 976

proc _arith_99 {} { return [expr {696 + 300}] }
assert bulk-arith-99 [_arith_99] 996

proc _arith_100 {} { return [expr {703 + 313}] }
assert bulk-arith-100 [_arith_100] 1016

proc _arith_101 {} { return [expr {710 + 326}] }
assert bulk-arith-101 [_arith_101] 1036

proc _arith_102 {} { return [expr {717 + 339}] }
assert bulk-arith-102 [_arith_102] 1056

proc _arith_103 {} { return [expr {724 + 352}] }
assert bulk-arith-103 [_arith_103] 1076

proc _arith_104 {} { return [expr {731 + 365}] }
assert bulk-arith-104 [_arith_104] 1096

proc _arith_105 {} { return [expr {738 + 378}] }
assert bulk-arith-105 [_arith_105] 1116

proc _arith_106 {} { return [expr {745 + 391}] }
assert bulk-arith-106 [_arith_106] 1136

proc _arith_107 {} { return [expr {752 + 404}] }
assert bulk-arith-107 [_arith_107] 1156

proc _arith_108 {} { return [expr {759 + 417}] }
assert bulk-arith-108 [_arith_108] 1176

proc _arith_109 {} { return [expr {766 + 430}] }
assert bulk-arith-109 [_arith_109] 1196

proc _arith_110 {} { return [expr {773 + 443}] }
assert bulk-arith-110 [_arith_110] 1216

proc _arith_111 {} { return [expr {780 + 456}] }
assert bulk-arith-111 [_arith_111] 1236

proc _arith_112 {} { return [expr {787 + 469}] }
assert bulk-arith-112 [_arith_112] 1256

proc _arith_113 {} { return [expr {794 + 482}] }
assert bulk-arith-113 [_arith_113] 1276

proc _arith_114 {} { return [expr {801 + 495}] }
assert bulk-arith-114 [_arith_114] 1296

proc _arith_115 {} { return [expr {808 + 9}] }
assert bulk-arith-115 [_arith_115] 817

proc _arith_116 {} { return [expr {815 + 22}] }
assert bulk-arith-116 [_arith_116] 837

proc _arith_117 {} { return [expr {822 + 35}] }
assert bulk-arith-117 [_arith_117] 857

proc _arith_118 {} { return [expr {829 + 48}] }
assert bulk-arith-118 [_arith_118] 877

proc _arith_119 {} { return [expr {836 + 61}] }
assert bulk-arith-119 [_arith_119] 897

proc _arith_120 {} { return [expr {843 + 74}] }
assert bulk-arith-120 [_arith_120] 917

proc _arith_121 {} { return [expr {850 + 87}] }
assert bulk-arith-121 [_arith_121] 937

proc _arith_122 {} { return [expr {857 + 100}] }
assert bulk-arith-122 [_arith_122] 957

proc _arith_123 {} { return [expr {864 + 113}] }
assert bulk-arith-123 [_arith_123] 977

proc _arith_124 {} { return [expr {871 + 126}] }
assert bulk-arith-124 [_arith_124] 997

proc _arith_125 {} { return [expr {878 + 139}] }
assert bulk-arith-125 [_arith_125] 1017

proc _arith_126 {} { return [expr {885 + 152}] }
assert bulk-arith-126 [_arith_126] 1037

proc _arith_127 {} { return [expr {892 + 165}] }
assert bulk-arith-127 [_arith_127] 1057

proc _arith_128 {} { return [expr {899 + 178}] }
assert bulk-arith-128 [_arith_128] 1077

proc _arith_129 {} { return [expr {906 + 191}] }
assert bulk-arith-129 [_arith_129] 1097

proc _arith_130 {} { return [expr {913 + 204}] }
assert bulk-arith-130 [_arith_130] 1117

proc _arith_131 {} { return [expr {920 + 217}] }
assert bulk-arith-131 [_arith_131] 1137

proc _arith_132 {} { return [expr {927 + 230}] }
assert bulk-arith-132 [_arith_132] 1157

proc _arith_133 {} { return [expr {934 + 243}] }
assert bulk-arith-133 [_arith_133] 1177

proc _arith_134 {} { return [expr {941 + 256}] }
assert bulk-arith-134 [_arith_134] 1197

proc _arith_135 {} { return [expr {948 + 269}] }
assert bulk-arith-135 [_arith_135] 1217

proc _arith_136 {} { return [expr {955 + 282}] }
assert bulk-arith-136 [_arith_136] 1237

proc _arith_137 {} { return [expr {962 + 295}] }
assert bulk-arith-137 [_arith_137] 1257

proc _arith_138 {} { return [expr {969 + 308}] }
assert bulk-arith-138 [_arith_138] 1277

proc _arith_139 {} { return [expr {976 + 321}] }
assert bulk-arith-139 [_arith_139] 1297

proc _arith_140 {} { return [expr {983 + 334}] }
assert bulk-arith-140 [_arith_140] 1317

proc _arith_141 {} { return [expr {990 + 347}] }
assert bulk-arith-141 [_arith_141] 1337

proc _arith_142 {} { return [expr {0 + 360}] }
assert bulk-arith-142 [_arith_142] 360

proc _arith_143 {} { return [expr {7 + 373}] }
assert bulk-arith-143 [_arith_143] 380

proc _arith_144 {} { return [expr {14 + 386}] }
assert bulk-arith-144 [_arith_144] 400

proc _arith_145 {} { return [expr {21 + 399}] }
assert bulk-arith-145 [_arith_145] 420

proc _arith_146 {} { return [expr {28 + 412}] }
assert bulk-arith-146 [_arith_146] 440

proc _arith_147 {} { return [expr {35 + 425}] }
assert bulk-arith-147 [_arith_147] 460

proc _arith_148 {} { return [expr {42 + 438}] }
assert bulk-arith-148 [_arith_148] 480

proc _arith_149 {} { return [expr {49 + 451}] }
assert bulk-arith-149 [_arith_149] 500

proc _arith_150 {} { return [expr {56 + 464}] }
assert bulk-arith-150 [_arith_150] 520

proc _arith_151 {} { return [expr {63 + 477}] }
assert bulk-arith-151 [_arith_151] 540

proc _arith_152 {} { return [expr {70 + 490}] }
assert bulk-arith-152 [_arith_152] 560

proc _arith_153 {} { return [expr {77 + 4}] }
assert bulk-arith-153 [_arith_153] 81

proc _arith_154 {} { return [expr {84 + 17}] }
assert bulk-arith-154 [_arith_154] 101

proc _arith_155 {} { return [expr {91 + 30}] }
assert bulk-arith-155 [_arith_155] 121

proc _arith_156 {} { return [expr {98 + 43}] }
assert bulk-arith-156 [_arith_156] 141

proc _arith_157 {} { return [expr {105 + 56}] }
assert bulk-arith-157 [_arith_157] 161

proc _arith_158 {} { return [expr {112 + 69}] }
assert bulk-arith-158 [_arith_158] 181

proc _arith_159 {} { return [expr {119 + 82}] }
assert bulk-arith-159 [_arith_159] 201

proc _arith_160 {} { return [expr {126 + 95}] }
assert bulk-arith-160 [_arith_160] 221

proc _arith_161 {} { return [expr {133 + 108}] }
assert bulk-arith-161 [_arith_161] 241

proc _arith_162 {} { return [expr {140 + 121}] }
assert bulk-arith-162 [_arith_162] 261

proc _arith_163 {} { return [expr {147 + 134}] }
assert bulk-arith-163 [_arith_163] 281

proc _arith_164 {} { return [expr {154 + 147}] }
assert bulk-arith-164 [_arith_164] 301

proc _arith_165 {} { return [expr {161 + 160}] }
assert bulk-arith-165 [_arith_165] 321

proc _arith_166 {} { return [expr {168 + 173}] }
assert bulk-arith-166 [_arith_166] 341

proc _arith_167 {} { return [expr {175 + 186}] }
assert bulk-arith-167 [_arith_167] 361

proc _arith_168 {} { return [expr {182 + 199}] }
assert bulk-arith-168 [_arith_168] 381

proc _arith_169 {} { return [expr {189 + 212}] }
assert bulk-arith-169 [_arith_169] 401

proc _arith_170 {} { return [expr {196 + 225}] }
assert bulk-arith-170 [_arith_170] 421

proc _arith_171 {} { return [expr {203 + 238}] }
assert bulk-arith-171 [_arith_171] 441

proc _arith_172 {} { return [expr {210 + 251}] }
assert bulk-arith-172 [_arith_172] 461

proc _arith_173 {} { return [expr {217 + 264}] }
assert bulk-arith-173 [_arith_173] 481

proc _arith_174 {} { return [expr {224 + 277}] }
assert bulk-arith-174 [_arith_174] 501

proc _arith_175 {} { return [expr {231 + 290}] }
assert bulk-arith-175 [_arith_175] 521

proc _arith_176 {} { return [expr {238 + 303}] }
assert bulk-arith-176 [_arith_176] 541

proc _arith_177 {} { return [expr {245 + 316}] }
assert bulk-arith-177 [_arith_177] 561

proc _arith_178 {} { return [expr {252 + 329}] }
assert bulk-arith-178 [_arith_178] 581

proc _arith_179 {} { return [expr {259 + 342}] }
assert bulk-arith-179 [_arith_179] 601

proc _arith_180 {} { return [expr {266 + 355}] }
assert bulk-arith-180 [_arith_180] 621

proc _arith_181 {} { return [expr {273 + 368}] }
assert bulk-arith-181 [_arith_181] 641

proc _arith_182 {} { return [expr {280 + 381}] }
assert bulk-arith-182 [_arith_182] 661

proc _arith_183 {} { return [expr {287 + 394}] }
assert bulk-arith-183 [_arith_183] 681

proc _arith_184 {} { return [expr {294 + 407}] }
assert bulk-arith-184 [_arith_184] 701

proc _arith_185 {} { return [expr {301 + 420}] }
assert bulk-arith-185 [_arith_185] 721

proc _arith_186 {} { return [expr {308 + 433}] }
assert bulk-arith-186 [_arith_186] 741

proc _arith_187 {} { return [expr {315 + 446}] }
assert bulk-arith-187 [_arith_187] 761

proc _arith_188 {} { return [expr {322 + 459}] }
assert bulk-arith-188 [_arith_188] 781

proc _arith_189 {} { return [expr {329 + 472}] }
assert bulk-arith-189 [_arith_189] 801

proc _arith_190 {} { return [expr {336 + 485}] }
assert bulk-arith-190 [_arith_190] 821

proc _arith_191 {} { return [expr {343 + 498}] }
assert bulk-arith-191 [_arith_191] 841

proc _arith_192 {} { return [expr {350 + 12}] }
assert bulk-arith-192 [_arith_192] 362

proc _arith_193 {} { return [expr {357 + 25}] }
assert bulk-arith-193 [_arith_193] 382

proc _arith_194 {} { return [expr {364 + 38}] }
assert bulk-arith-194 [_arith_194] 402

proc _arith_195 {} { return [expr {371 + 51}] }
assert bulk-arith-195 [_arith_195] 422

proc _arith_196 {} { return [expr {378 + 64}] }
assert bulk-arith-196 [_arith_196] 442

proc _arith_197 {} { return [expr {385 + 77}] }
assert bulk-arith-197 [_arith_197] 462

proc _arith_198 {} { return [expr {392 + 90}] }
assert bulk-arith-198 [_arith_198] 482

proc _arith_199 {} { return [expr {399 + 103}] }
assert bulk-arith-199 [_arith_199] 502

proc _str_0 {} { return [string toupper "word_0000_test"] }
assert bulk-str-0 [_str_0] "WORD_0000_TEST"

proc _str_1 {} { return [string toupper "word_0001_test"] }
assert bulk-str-1 [_str_1] "WORD_0001_TEST"

proc _str_2 {} { return [string toupper "word_0002_test"] }
assert bulk-str-2 [_str_2] "WORD_0002_TEST"

proc _str_3 {} { return [string toupper "word_0003_test"] }
assert bulk-str-3 [_str_3] "WORD_0003_TEST"

proc _str_4 {} { return [string toupper "word_0004_test"] }
assert bulk-str-4 [_str_4] "WORD_0004_TEST"

proc _str_5 {} { return [string toupper "word_0005_test"] }
assert bulk-str-5 [_str_5] "WORD_0005_TEST"

proc _str_6 {} { return [string toupper "word_0006_test"] }
assert bulk-str-6 [_str_6] "WORD_0006_TEST"

proc _str_7 {} { return [string toupper "word_0007_test"] }
assert bulk-str-7 [_str_7] "WORD_0007_TEST"

proc _str_8 {} { return [string toupper "word_0008_test"] }
assert bulk-str-8 [_str_8] "WORD_0008_TEST"

proc _str_9 {} { return [string toupper "word_0009_test"] }
assert bulk-str-9 [_str_9] "WORD_0009_TEST"

proc _str_10 {} { return [string toupper "word_0010_test"] }
assert bulk-str-10 [_str_10] "WORD_0010_TEST"

proc _str_11 {} { return [string toupper "word_0011_test"] }
assert bulk-str-11 [_str_11] "WORD_0011_TEST"

proc _str_12 {} { return [string toupper "word_0012_test"] }
assert bulk-str-12 [_str_12] "WORD_0012_TEST"

proc _str_13 {} { return [string toupper "word_0013_test"] }
assert bulk-str-13 [_str_13] "WORD_0013_TEST"

proc _str_14 {} { return [string toupper "word_0014_test"] }
assert bulk-str-14 [_str_14] "WORD_0014_TEST"

proc _str_15 {} { return [string toupper "word_0015_test"] }
assert bulk-str-15 [_str_15] "WORD_0015_TEST"

proc _str_16 {} { return [string toupper "word_0016_test"] }
assert bulk-str-16 [_str_16] "WORD_0016_TEST"

proc _str_17 {} { return [string toupper "word_0017_test"] }
assert bulk-str-17 [_str_17] "WORD_0017_TEST"

proc _str_18 {} { return [string toupper "word_0018_test"] }
assert bulk-str-18 [_str_18] "WORD_0018_TEST"

proc _str_19 {} { return [string toupper "word_0019_test"] }
assert bulk-str-19 [_str_19] "WORD_0019_TEST"

proc _str_20 {} { return [string toupper "word_0020_test"] }
assert bulk-str-20 [_str_20] "WORD_0020_TEST"

proc _str_21 {} { return [string toupper "word_0021_test"] }
assert bulk-str-21 [_str_21] "WORD_0021_TEST"

proc _str_22 {} { return [string toupper "word_0022_test"] }
assert bulk-str-22 [_str_22] "WORD_0022_TEST"

proc _str_23 {} { return [string toupper "word_0023_test"] }
assert bulk-str-23 [_str_23] "WORD_0023_TEST"

proc _str_24 {} { return [string toupper "word_0024_test"] }
assert bulk-str-24 [_str_24] "WORD_0024_TEST"

proc _str_25 {} { return [string toupper "word_0025_test"] }
assert bulk-str-25 [_str_25] "WORD_0025_TEST"

proc _str_26 {} { return [string toupper "word_0026_test"] }
assert bulk-str-26 [_str_26] "WORD_0026_TEST"

proc _str_27 {} { return [string toupper "word_0027_test"] }
assert bulk-str-27 [_str_27] "WORD_0027_TEST"

proc _str_28 {} { return [string toupper "word_0028_test"] }
assert bulk-str-28 [_str_28] "WORD_0028_TEST"

proc _str_29 {} { return [string toupper "word_0029_test"] }
assert bulk-str-29 [_str_29] "WORD_0029_TEST"

proc _str_30 {} { return [string toupper "word_0030_test"] }
assert bulk-str-30 [_str_30] "WORD_0030_TEST"

proc _str_31 {} { return [string toupper "word_0031_test"] }
assert bulk-str-31 [_str_31] "WORD_0031_TEST"

proc _str_32 {} { return [string toupper "word_0032_test"] }
assert bulk-str-32 [_str_32] "WORD_0032_TEST"

proc _str_33 {} { return [string toupper "word_0033_test"] }
assert bulk-str-33 [_str_33] "WORD_0033_TEST"

proc _str_34 {} { return [string toupper "word_0034_test"] }
assert bulk-str-34 [_str_34] "WORD_0034_TEST"

proc _str_35 {} { return [string toupper "word_0035_test"] }
assert bulk-str-35 [_str_35] "WORD_0035_TEST"

proc _str_36 {} { return [string toupper "word_0036_test"] }
assert bulk-str-36 [_str_36] "WORD_0036_TEST"

proc _str_37 {} { return [string toupper "word_0037_test"] }
assert bulk-str-37 [_str_37] "WORD_0037_TEST"

proc _str_38 {} { return [string toupper "word_0038_test"] }
assert bulk-str-38 [_str_38] "WORD_0038_TEST"

proc _str_39 {} { return [string toupper "word_0039_test"] }
assert bulk-str-39 [_str_39] "WORD_0039_TEST"

proc _str_40 {} { return [string toupper "word_0040_test"] }
assert bulk-str-40 [_str_40] "WORD_0040_TEST"

proc _str_41 {} { return [string toupper "word_0041_test"] }
assert bulk-str-41 [_str_41] "WORD_0041_TEST"

proc _str_42 {} { return [string toupper "word_0042_test"] }
assert bulk-str-42 [_str_42] "WORD_0042_TEST"

proc _str_43 {} { return [string toupper "word_0043_test"] }
assert bulk-str-43 [_str_43] "WORD_0043_TEST"

proc _str_44 {} { return [string toupper "word_0044_test"] }
assert bulk-str-44 [_str_44] "WORD_0044_TEST"

proc _str_45 {} { return [string toupper "word_0045_test"] }
assert bulk-str-45 [_str_45] "WORD_0045_TEST"

proc _str_46 {} { return [string toupper "word_0046_test"] }
assert bulk-str-46 [_str_46] "WORD_0046_TEST"

proc _str_47 {} { return [string toupper "word_0047_test"] }
assert bulk-str-47 [_str_47] "WORD_0047_TEST"

proc _str_48 {} { return [string toupper "word_0048_test"] }
assert bulk-str-48 [_str_48] "WORD_0048_TEST"

proc _str_49 {} { return [string toupper "word_0049_test"] }
assert bulk-str-49 [_str_49] "WORD_0049_TEST"

proc _str_50 {} { return [string toupper "word_0050_test"] }
assert bulk-str-50 [_str_50] "WORD_0050_TEST"

proc _str_51 {} { return [string toupper "word_0051_test"] }
assert bulk-str-51 [_str_51] "WORD_0051_TEST"

proc _str_52 {} { return [string toupper "word_0052_test"] }
assert bulk-str-52 [_str_52] "WORD_0052_TEST"

proc _str_53 {} { return [string toupper "word_0053_test"] }
assert bulk-str-53 [_str_53] "WORD_0053_TEST"

proc _str_54 {} { return [string toupper "word_0054_test"] }
assert bulk-str-54 [_str_54] "WORD_0054_TEST"

proc _str_55 {} { return [string toupper "word_0055_test"] }
assert bulk-str-55 [_str_55] "WORD_0055_TEST"

proc _str_56 {} { return [string toupper "word_0056_test"] }
assert bulk-str-56 [_str_56] "WORD_0056_TEST"

proc _str_57 {} { return [string toupper "word_0057_test"] }
assert bulk-str-57 [_str_57] "WORD_0057_TEST"

proc _str_58 {} { return [string toupper "word_0058_test"] }
assert bulk-str-58 [_str_58] "WORD_0058_TEST"

proc _str_59 {} { return [string toupper "word_0059_test"] }
assert bulk-str-59 [_str_59] "WORD_0059_TEST"

proc _str_60 {} { return [string toupper "word_0060_test"] }
assert bulk-str-60 [_str_60] "WORD_0060_TEST"

proc _str_61 {} { return [string toupper "word_0061_test"] }
assert bulk-str-61 [_str_61] "WORD_0061_TEST"

proc _str_62 {} { return [string toupper "word_0062_test"] }
assert bulk-str-62 [_str_62] "WORD_0062_TEST"

proc _str_63 {} { return [string toupper "word_0063_test"] }
assert bulk-str-63 [_str_63] "WORD_0063_TEST"

proc _str_64 {} { return [string toupper "word_0064_test"] }
assert bulk-str-64 [_str_64] "WORD_0064_TEST"

proc _str_65 {} { return [string toupper "word_0065_test"] }
assert bulk-str-65 [_str_65] "WORD_0065_TEST"

proc _str_66 {} { return [string toupper "word_0066_test"] }
assert bulk-str-66 [_str_66] "WORD_0066_TEST"

proc _str_67 {} { return [string toupper "word_0067_test"] }
assert bulk-str-67 [_str_67] "WORD_0067_TEST"

proc _str_68 {} { return [string toupper "word_0068_test"] }
assert bulk-str-68 [_str_68] "WORD_0068_TEST"

proc _str_69 {} { return [string toupper "word_0069_test"] }
assert bulk-str-69 [_str_69] "WORD_0069_TEST"

proc _str_70 {} { return [string toupper "word_0070_test"] }
assert bulk-str-70 [_str_70] "WORD_0070_TEST"

proc _str_71 {} { return [string toupper "word_0071_test"] }
assert bulk-str-71 [_str_71] "WORD_0071_TEST"

proc _str_72 {} { return [string toupper "word_0072_test"] }
assert bulk-str-72 [_str_72] "WORD_0072_TEST"

proc _str_73 {} { return [string toupper "word_0073_test"] }
assert bulk-str-73 [_str_73] "WORD_0073_TEST"

proc _str_74 {} { return [string toupper "word_0074_test"] }
assert bulk-str-74 [_str_74] "WORD_0074_TEST"

proc _str_75 {} { return [string toupper "word_0075_test"] }
assert bulk-str-75 [_str_75] "WORD_0075_TEST"

proc _str_76 {} { return [string toupper "word_0076_test"] }
assert bulk-str-76 [_str_76] "WORD_0076_TEST"

proc _str_77 {} { return [string toupper "word_0077_test"] }
assert bulk-str-77 [_str_77] "WORD_0077_TEST"

proc _str_78 {} { return [string toupper "word_0078_test"] }
assert bulk-str-78 [_str_78] "WORD_0078_TEST"

proc _str_79 {} { return [string toupper "word_0079_test"] }
assert bulk-str-79 [_str_79] "WORD_0079_TEST"

proc _str_80 {} { return [string toupper "word_0080_test"] }
assert bulk-str-80 [_str_80] "WORD_0080_TEST"

proc _str_81 {} { return [string toupper "word_0081_test"] }
assert bulk-str-81 [_str_81] "WORD_0081_TEST"

proc _str_82 {} { return [string toupper "word_0082_test"] }
assert bulk-str-82 [_str_82] "WORD_0082_TEST"

proc _str_83 {} { return [string toupper "word_0083_test"] }
assert bulk-str-83 [_str_83] "WORD_0083_TEST"

proc _str_84 {} { return [string toupper "word_0084_test"] }
assert bulk-str-84 [_str_84] "WORD_0084_TEST"

proc _str_85 {} { return [string toupper "word_0085_test"] }
assert bulk-str-85 [_str_85] "WORD_0085_TEST"

proc _str_86 {} { return [string toupper "word_0086_test"] }
assert bulk-str-86 [_str_86] "WORD_0086_TEST"

proc _str_87 {} { return [string toupper "word_0087_test"] }
assert bulk-str-87 [_str_87] "WORD_0087_TEST"

proc _str_88 {} { return [string toupper "word_0088_test"] }
assert bulk-str-88 [_str_88] "WORD_0088_TEST"

proc _str_89 {} { return [string toupper "word_0089_test"] }
assert bulk-str-89 [_str_89] "WORD_0089_TEST"

proc _str_90 {} { return [string toupper "word_0090_test"] }
assert bulk-str-90 [_str_90] "WORD_0090_TEST"

proc _str_91 {} { return [string toupper "word_0091_test"] }
assert bulk-str-91 [_str_91] "WORD_0091_TEST"

proc _str_92 {} { return [string toupper "word_0092_test"] }
assert bulk-str-92 [_str_92] "WORD_0092_TEST"

proc _str_93 {} { return [string toupper "word_0093_test"] }
assert bulk-str-93 [_str_93] "WORD_0093_TEST"

proc _str_94 {} { return [string toupper "word_0094_test"] }
assert bulk-str-94 [_str_94] "WORD_0094_TEST"

proc _str_95 {} { return [string toupper "word_0095_test"] }
assert bulk-str-95 [_str_95] "WORD_0095_TEST"

proc _str_96 {} { return [string toupper "word_0096_test"] }
assert bulk-str-96 [_str_96] "WORD_0096_TEST"

proc _str_97 {} { return [string toupper "word_0097_test"] }
assert bulk-str-97 [_str_97] "WORD_0097_TEST"

proc _str_98 {} { return [string toupper "word_0098_test"] }
assert bulk-str-98 [_str_98] "WORD_0098_TEST"

proc _str_99 {} { return [string toupper "word_0099_test"] }
assert bulk-str-99 [_str_99] "WORD_0099_TEST"

proc _str_100 {} { return [string toupper "word_0100_test"] }
assert bulk-str-100 [_str_100] "WORD_0100_TEST"

proc _str_101 {} { return [string toupper "word_0101_test"] }
assert bulk-str-101 [_str_101] "WORD_0101_TEST"

proc _str_102 {} { return [string toupper "word_0102_test"] }
assert bulk-str-102 [_str_102] "WORD_0102_TEST"

proc _str_103 {} { return [string toupper "word_0103_test"] }
assert bulk-str-103 [_str_103] "WORD_0103_TEST"

proc _str_104 {} { return [string toupper "word_0104_test"] }
assert bulk-str-104 [_str_104] "WORD_0104_TEST"

proc _str_105 {} { return [string toupper "word_0105_test"] }
assert bulk-str-105 [_str_105] "WORD_0105_TEST"

proc _str_106 {} { return [string toupper "word_0106_test"] }
assert bulk-str-106 [_str_106] "WORD_0106_TEST"

proc _str_107 {} { return [string toupper "word_0107_test"] }
assert bulk-str-107 [_str_107] "WORD_0107_TEST"

proc _str_108 {} { return [string toupper "word_0108_test"] }
assert bulk-str-108 [_str_108] "WORD_0108_TEST"

proc _str_109 {} { return [string toupper "word_0109_test"] }
assert bulk-str-109 [_str_109] "WORD_0109_TEST"

proc _str_110 {} { return [string toupper "word_0110_test"] }
assert bulk-str-110 [_str_110] "WORD_0110_TEST"

proc _str_111 {} { return [string toupper "word_0111_test"] }
assert bulk-str-111 [_str_111] "WORD_0111_TEST"

proc _str_112 {} { return [string toupper "word_0112_test"] }
assert bulk-str-112 [_str_112] "WORD_0112_TEST"

proc _str_113 {} { return [string toupper "word_0113_test"] }
assert bulk-str-113 [_str_113] "WORD_0113_TEST"

proc _str_114 {} { return [string toupper "word_0114_test"] }
assert bulk-str-114 [_str_114] "WORD_0114_TEST"

proc _str_115 {} { return [string toupper "word_0115_test"] }
assert bulk-str-115 [_str_115] "WORD_0115_TEST"

proc _str_116 {} { return [string toupper "word_0116_test"] }
assert bulk-str-116 [_str_116] "WORD_0116_TEST"

proc _str_117 {} { return [string toupper "word_0117_test"] }
assert bulk-str-117 [_str_117] "WORD_0117_TEST"

proc _str_118 {} { return [string toupper "word_0118_test"] }
assert bulk-str-118 [_str_118] "WORD_0118_TEST"

proc _str_119 {} { return [string toupper "word_0119_test"] }
assert bulk-str-119 [_str_119] "WORD_0119_TEST"

proc _str_120 {} { return [string toupper "word_0120_test"] }
assert bulk-str-120 [_str_120] "WORD_0120_TEST"

proc _str_121 {} { return [string toupper "word_0121_test"] }
assert bulk-str-121 [_str_121] "WORD_0121_TEST"

proc _str_122 {} { return [string toupper "word_0122_test"] }
assert bulk-str-122 [_str_122] "WORD_0122_TEST"

proc _str_123 {} { return [string toupper "word_0123_test"] }
assert bulk-str-123 [_str_123] "WORD_0123_TEST"

proc _str_124 {} { return [string toupper "word_0124_test"] }
assert bulk-str-124 [_str_124] "WORD_0124_TEST"

proc _str_125 {} { return [string toupper "word_0125_test"] }
assert bulk-str-125 [_str_125] "WORD_0125_TEST"

proc _str_126 {} { return [string toupper "word_0126_test"] }
assert bulk-str-126 [_str_126] "WORD_0126_TEST"

proc _str_127 {} { return [string toupper "word_0127_test"] }
assert bulk-str-127 [_str_127] "WORD_0127_TEST"

proc _str_128 {} { return [string toupper "word_0128_test"] }
assert bulk-str-128 [_str_128] "WORD_0128_TEST"

proc _str_129 {} { return [string toupper "word_0129_test"] }
assert bulk-str-129 [_str_129] "WORD_0129_TEST"

proc _str_130 {} { return [string toupper "word_0130_test"] }
assert bulk-str-130 [_str_130] "WORD_0130_TEST"

proc _str_131 {} { return [string toupper "word_0131_test"] }
assert bulk-str-131 [_str_131] "WORD_0131_TEST"

proc _str_132 {} { return [string toupper "word_0132_test"] }
assert bulk-str-132 [_str_132] "WORD_0132_TEST"

proc _str_133 {} { return [string toupper "word_0133_test"] }
assert bulk-str-133 [_str_133] "WORD_0133_TEST"

proc _str_134 {} { return [string toupper "word_0134_test"] }
assert bulk-str-134 [_str_134] "WORD_0134_TEST"

proc _str_135 {} { return [string toupper "word_0135_test"] }
assert bulk-str-135 [_str_135] "WORD_0135_TEST"

proc _str_136 {} { return [string toupper "word_0136_test"] }
assert bulk-str-136 [_str_136] "WORD_0136_TEST"

proc _str_137 {} { return [string toupper "word_0137_test"] }
assert bulk-str-137 [_str_137] "WORD_0137_TEST"

proc _str_138 {} { return [string toupper "word_0138_test"] }
assert bulk-str-138 [_str_138] "WORD_0138_TEST"

proc _str_139 {} { return [string toupper "word_0139_test"] }
assert bulk-str-139 [_str_139] "WORD_0139_TEST"

proc _str_140 {} { return [string toupper "word_0140_test"] }
assert bulk-str-140 [_str_140] "WORD_0140_TEST"

proc _str_141 {} { return [string toupper "word_0141_test"] }
assert bulk-str-141 [_str_141] "WORD_0141_TEST"

proc _str_142 {} { return [string toupper "word_0142_test"] }
assert bulk-str-142 [_str_142] "WORD_0142_TEST"

proc _str_143 {} { return [string toupper "word_0143_test"] }
assert bulk-str-143 [_str_143] "WORD_0143_TEST"

proc _str_144 {} { return [string toupper "word_0144_test"] }
assert bulk-str-144 [_str_144] "WORD_0144_TEST"

proc _str_145 {} { return [string toupper "word_0145_test"] }
assert bulk-str-145 [_str_145] "WORD_0145_TEST"

proc _str_146 {} { return [string toupper "word_0146_test"] }
assert bulk-str-146 [_str_146] "WORD_0146_TEST"

proc _str_147 {} { return [string toupper "word_0147_test"] }
assert bulk-str-147 [_str_147] "WORD_0147_TEST"

proc _str_148 {} { return [string toupper "word_0148_test"] }
assert bulk-str-148 [_str_148] "WORD_0148_TEST"

proc _str_149 {} { return [string toupper "word_0149_test"] }
assert bulk-str-149 [_str_149] "WORD_0149_TEST"

proc _lst_0 {} { return [lsort -integer {0 1 2 3 4 5}] }
assert bulk-lst-0 [_lst_0] "0 1 2 3 4 5"

proc _lst_1 {} { return [lsort -integer {3 4 5 6 7 8}] }
assert bulk-lst-1 [_lst_1] "3 4 5 6 7 8"

proc _lst_2 {} { return [lsort -integer {6 7 8 9 10 11}] }
assert bulk-lst-2 [_lst_2] "6 7 8 9 10 11"

proc _lst_3 {} { return [lsort -integer {9 10 11 12 13 14}] }
assert bulk-lst-3 [_lst_3] "9 10 11 12 13 14"

proc _lst_4 {} { return [lsort -integer {12 13 14 15 16 17}] }
assert bulk-lst-4 [_lst_4] "12 13 14 15 16 17"

proc _lst_5 {} { return [lsort -integer {15 16 17 18 19 20}] }
assert bulk-lst-5 [_lst_5] "15 16 17 18 19 20"

proc _lst_6 {} { return [lsort -integer {18 19 20 21 22 23}] }
assert bulk-lst-6 [_lst_6] "18 19 20 21 22 23"

proc _lst_7 {} { return [lsort -integer {21 22 23 24 25 26}] }
assert bulk-lst-7 [_lst_7] "21 22 23 24 25 26"

proc _lst_8 {} { return [lsort -integer {24 25 26 27 28 29}] }
assert bulk-lst-8 [_lst_8] "24 25 26 27 28 29"

proc _lst_9 {} { return [lsort -integer {27 28 29 30 31 32}] }
assert bulk-lst-9 [_lst_9] "27 28 29 30 31 32"

proc _lst_10 {} { return [lsort -integer {30 31 32 33 34 35}] }
assert bulk-lst-10 [_lst_10] "30 31 32 33 34 35"

proc _lst_11 {} { return [lsort -integer {33 34 35 36 37 38}] }
assert bulk-lst-11 [_lst_11] "33 34 35 36 37 38"

proc _lst_12 {} { return [lsort -integer {36 37 38 39 40 41}] }
assert bulk-lst-12 [_lst_12] "36 37 38 39 40 41"

proc _lst_13 {} { return [lsort -integer {39 40 41 42 43 44}] }
assert bulk-lst-13 [_lst_13] "39 40 41 42 43 44"

proc _lst_14 {} { return [lsort -integer {42 43 44 45 46 47}] }
assert bulk-lst-14 [_lst_14] "42 43 44 45 46 47"

proc _lst_15 {} { return [lsort -integer {45 46 47 48 49 50}] }
assert bulk-lst-15 [_lst_15] "45 46 47 48 49 50"

proc _lst_16 {} { return [lsort -integer {48 49 50 51 52 53}] }
assert bulk-lst-16 [_lst_16] "48 49 50 51 52 53"

proc _lst_17 {} { return [lsort -integer {51 52 53 54 55 56}] }
assert bulk-lst-17 [_lst_17] "51 52 53 54 55 56"

proc _lst_18 {} { return [lsort -integer {54 55 56 57 58 59}] }
assert bulk-lst-18 [_lst_18] "54 55 56 57 58 59"

proc _lst_19 {} { return [lsort -integer {57 58 59 60 61 62}] }
assert bulk-lst-19 [_lst_19] "57 58 59 60 61 62"

proc _lst_20 {} { return [lsort -integer {60 61 62 63 64 65}] }
assert bulk-lst-20 [_lst_20] "60 61 62 63 64 65"

proc _lst_21 {} { return [lsort -integer {63 64 65 66 67 68}] }
assert bulk-lst-21 [_lst_21] "63 64 65 66 67 68"

proc _lst_22 {} { return [lsort -integer {66 67 68 69 70 71}] }
assert bulk-lst-22 [_lst_22] "66 67 68 69 70 71"

proc _lst_23 {} { return [lsort -integer {69 70 71 72 73 74}] }
assert bulk-lst-23 [_lst_23] "69 70 71 72 73 74"

proc _lst_24 {} { return [lsort -integer {72 73 74 75 76 77}] }
assert bulk-lst-24 [_lst_24] "72 73 74 75 76 77"

proc _lst_25 {} { return [lsort -integer {75 76 77 78 79 80}] }
assert bulk-lst-25 [_lst_25] "75 76 77 78 79 80"

proc _lst_26 {} { return [lsort -integer {78 79 80 81 82 83}] }
assert bulk-lst-26 [_lst_26] "78 79 80 81 82 83"

proc _lst_27 {} { return [lsort -integer {81 82 83 84 85 86}] }
assert bulk-lst-27 [_lst_27] "81 82 83 84 85 86"

proc _lst_28 {} { return [lsort -integer {84 85 86 87 88 89}] }
assert bulk-lst-28 [_lst_28] "84 85 86 87 88 89"

proc _lst_29 {} { return [lsort -integer {87 88 89 90 91 92}] }
assert bulk-lst-29 [_lst_29] "87 88 89 90 91 92"

proc _lst_30 {} { return [lsort -integer {90 91 92 93 94 95}] }
assert bulk-lst-30 [_lst_30] "90 91 92 93 94 95"

proc _lst_31 {} { return [lsort -integer {93 94 95 96 97 98}] }
assert bulk-lst-31 [_lst_31] "93 94 95 96 97 98"

proc _lst_32 {} { return [lsort -integer {96 97 98 99 0 1}] }
assert bulk-lst-32 [_lst_32] "0 1 96 97 98 99"

proc _lst_33 {} { return [lsort -integer {99 0 1 2 3 4}] }
assert bulk-lst-33 [_lst_33] "0 1 2 3 4 99"

proc _lst_34 {} { return [lsort -integer {2 3 4 5 6 7}] }
assert bulk-lst-34 [_lst_34] "2 3 4 5 6 7"

proc _lst_35 {} { return [lsort -integer {5 6 7 8 9 10}] }
assert bulk-lst-35 [_lst_35] "5 6 7 8 9 10"

proc _lst_36 {} { return [lsort -integer {8 9 10 11 12 13}] }
assert bulk-lst-36 [_lst_36] "8 9 10 11 12 13"

proc _lst_37 {} { return [lsort -integer {11 12 13 14 15 16}] }
assert bulk-lst-37 [_lst_37] "11 12 13 14 15 16"

proc _lst_38 {} { return [lsort -integer {14 15 16 17 18 19}] }
assert bulk-lst-38 [_lst_38] "14 15 16 17 18 19"

proc _lst_39 {} { return [lsort -integer {17 18 19 20 21 22}] }
assert bulk-lst-39 [_lst_39] "17 18 19 20 21 22"

proc _lst_40 {} { return [lsort -integer {20 21 22 23 24 25}] }
assert bulk-lst-40 [_lst_40] "20 21 22 23 24 25"

proc _lst_41 {} { return [lsort -integer {23 24 25 26 27 28}] }
assert bulk-lst-41 [_lst_41] "23 24 25 26 27 28"

proc _lst_42 {} { return [lsort -integer {26 27 28 29 30 31}] }
assert bulk-lst-42 [_lst_42] "26 27 28 29 30 31"

proc _lst_43 {} { return [lsort -integer {29 30 31 32 33 34}] }
assert bulk-lst-43 [_lst_43] "29 30 31 32 33 34"

proc _lst_44 {} { return [lsort -integer {32 33 34 35 36 37}] }
assert bulk-lst-44 [_lst_44] "32 33 34 35 36 37"

proc _lst_45 {} { return [lsort -integer {35 36 37 38 39 40}] }
assert bulk-lst-45 [_lst_45] "35 36 37 38 39 40"

proc _lst_46 {} { return [lsort -integer {38 39 40 41 42 43}] }
assert bulk-lst-46 [_lst_46] "38 39 40 41 42 43"

proc _lst_47 {} { return [lsort -integer {41 42 43 44 45 46}] }
assert bulk-lst-47 [_lst_47] "41 42 43 44 45 46"

proc _lst_48 {} { return [lsort -integer {44 45 46 47 48 49}] }
assert bulk-lst-48 [_lst_48] "44 45 46 47 48 49"

proc _lst_49 {} { return [lsort -integer {47 48 49 50 51 52}] }
assert bulk-lst-49 [_lst_49] "47 48 49 50 51 52"

proc _lst_50 {} { return [lsort -integer {50 51 52 53 54 55}] }
assert bulk-lst-50 [_lst_50] "50 51 52 53 54 55"

proc _lst_51 {} { return [lsort -integer {53 54 55 56 57 58}] }
assert bulk-lst-51 [_lst_51] "53 54 55 56 57 58"

proc _lst_52 {} { return [lsort -integer {56 57 58 59 60 61}] }
assert bulk-lst-52 [_lst_52] "56 57 58 59 60 61"

proc _lst_53 {} { return [lsort -integer {59 60 61 62 63 64}] }
assert bulk-lst-53 [_lst_53] "59 60 61 62 63 64"

proc _lst_54 {} { return [lsort -integer {62 63 64 65 66 67}] }
assert bulk-lst-54 [_lst_54] "62 63 64 65 66 67"

proc _lst_55 {} { return [lsort -integer {65 66 67 68 69 70}] }
assert bulk-lst-55 [_lst_55] "65 66 67 68 69 70"

proc _lst_56 {} { return [lsort -integer {68 69 70 71 72 73}] }
assert bulk-lst-56 [_lst_56] "68 69 70 71 72 73"

proc _lst_57 {} { return [lsort -integer {71 72 73 74 75 76}] }
assert bulk-lst-57 [_lst_57] "71 72 73 74 75 76"

proc _lst_58 {} { return [lsort -integer {74 75 76 77 78 79}] }
assert bulk-lst-58 [_lst_58] "74 75 76 77 78 79"

proc _lst_59 {} { return [lsort -integer {77 78 79 80 81 82}] }
assert bulk-lst-59 [_lst_59] "77 78 79 80 81 82"

proc _lst_60 {} { return [lsort -integer {80 81 82 83 84 85}] }
assert bulk-lst-60 [_lst_60] "80 81 82 83 84 85"

proc _lst_61 {} { return [lsort -integer {83 84 85 86 87 88}] }
assert bulk-lst-61 [_lst_61] "83 84 85 86 87 88"

proc _lst_62 {} { return [lsort -integer {86 87 88 89 90 91}] }
assert bulk-lst-62 [_lst_62] "86 87 88 89 90 91"

proc _lst_63 {} { return [lsort -integer {89 90 91 92 93 94}] }
assert bulk-lst-63 [_lst_63] "89 90 91 92 93 94"

proc _lst_64 {} { return [lsort -integer {92 93 94 95 96 97}] }
assert bulk-lst-64 [_lst_64] "92 93 94 95 96 97"

proc _lst_65 {} { return [lsort -integer {95 96 97 98 99 0}] }
assert bulk-lst-65 [_lst_65] "0 95 96 97 98 99"

proc _lst_66 {} { return [lsort -integer {98 99 0 1 2 3}] }
assert bulk-lst-66 [_lst_66] "0 1 2 3 98 99"

proc _lst_67 {} { return [lsort -integer {1 2 3 4 5 6}] }
assert bulk-lst-67 [_lst_67] "1 2 3 4 5 6"

proc _lst_68 {} { return [lsort -integer {4 5 6 7 8 9}] }
assert bulk-lst-68 [_lst_68] "4 5 6 7 8 9"

proc _lst_69 {} { return [lsort -integer {7 8 9 10 11 12}] }
assert bulk-lst-69 [_lst_69] "7 8 9 10 11 12"

proc _lst_70 {} { return [lsort -integer {10 11 12 13 14 15}] }
assert bulk-lst-70 [_lst_70] "10 11 12 13 14 15"

proc _lst_71 {} { return [lsort -integer {13 14 15 16 17 18}] }
assert bulk-lst-71 [_lst_71] "13 14 15 16 17 18"

proc _lst_72 {} { return [lsort -integer {16 17 18 19 20 21}] }
assert bulk-lst-72 [_lst_72] "16 17 18 19 20 21"

proc _lst_73 {} { return [lsort -integer {19 20 21 22 23 24}] }
assert bulk-lst-73 [_lst_73] "19 20 21 22 23 24"

proc _lst_74 {} { return [lsort -integer {22 23 24 25 26 27}] }
assert bulk-lst-74 [_lst_74] "22 23 24 25 26 27"

proc _lst_75 {} { return [lsort -integer {25 26 27 28 29 30}] }
assert bulk-lst-75 [_lst_75] "25 26 27 28 29 30"

proc _lst_76 {} { return [lsort -integer {28 29 30 31 32 33}] }
assert bulk-lst-76 [_lst_76] "28 29 30 31 32 33"

proc _lst_77 {} { return [lsort -integer {31 32 33 34 35 36}] }
assert bulk-lst-77 [_lst_77] "31 32 33 34 35 36"

proc _lst_78 {} { return [lsort -integer {34 35 36 37 38 39}] }
assert bulk-lst-78 [_lst_78] "34 35 36 37 38 39"

proc _lst_79 {} { return [lsort -integer {37 38 39 40 41 42}] }
assert bulk-lst-79 [_lst_79] "37 38 39 40 41 42"

proc _lst_80 {} { return [lsort -integer {40 41 42 43 44 45}] }
assert bulk-lst-80 [_lst_80] "40 41 42 43 44 45"

proc _lst_81 {} { return [lsort -integer {43 44 45 46 47 48}] }
assert bulk-lst-81 [_lst_81] "43 44 45 46 47 48"

proc _lst_82 {} { return [lsort -integer {46 47 48 49 50 51}] }
assert bulk-lst-82 [_lst_82] "46 47 48 49 50 51"

proc _lst_83 {} { return [lsort -integer {49 50 51 52 53 54}] }
assert bulk-lst-83 [_lst_83] "49 50 51 52 53 54"

proc _lst_84 {} { return [lsort -integer {52 53 54 55 56 57}] }
assert bulk-lst-84 [_lst_84] "52 53 54 55 56 57"

proc _lst_85 {} { return [lsort -integer {55 56 57 58 59 60}] }
assert bulk-lst-85 [_lst_85] "55 56 57 58 59 60"

proc _lst_86 {} { return [lsort -integer {58 59 60 61 62 63}] }
assert bulk-lst-86 [_lst_86] "58 59 60 61 62 63"

proc _lst_87 {} { return [lsort -integer {61 62 63 64 65 66}] }
assert bulk-lst-87 [_lst_87] "61 62 63 64 65 66"

proc _lst_88 {} { return [lsort -integer {64 65 66 67 68 69}] }
assert bulk-lst-88 [_lst_88] "64 65 66 67 68 69"

proc _lst_89 {} { return [lsort -integer {67 68 69 70 71 72}] }
assert bulk-lst-89 [_lst_89] "67 68 69 70 71 72"

proc _lst_90 {} { return [lsort -integer {70 71 72 73 74 75}] }
assert bulk-lst-90 [_lst_90] "70 71 72 73 74 75"

proc _lst_91 {} { return [lsort -integer {73 74 75 76 77 78}] }
assert bulk-lst-91 [_lst_91] "73 74 75 76 77 78"

proc _lst_92 {} { return [lsort -integer {76 77 78 79 80 81}] }
assert bulk-lst-92 [_lst_92] "76 77 78 79 80 81"

proc _lst_93 {} { return [lsort -integer {79 80 81 82 83 84}] }
assert bulk-lst-93 [_lst_93] "79 80 81 82 83 84"

proc _lst_94 {} { return [lsort -integer {82 83 84 85 86 87}] }
assert bulk-lst-94 [_lst_94] "82 83 84 85 86 87"

proc _lst_95 {} { return [lsort -integer {85 86 87 88 89 90}] }
assert bulk-lst-95 [_lst_95] "85 86 87 88 89 90"

proc _lst_96 {} { return [lsort -integer {88 89 90 91 92 93}] }
assert bulk-lst-96 [_lst_96] "88 89 90 91 92 93"

proc _lst_97 {} { return [lsort -integer {91 92 93 94 95 96}] }
assert bulk-lst-97 [_lst_97] "91 92 93 94 95 96"

proc _lst_98 {} { return [lsort -integer {94 95 96 97 98 99}] }
assert bulk-lst-98 [_lst_98] "94 95 96 97 98 99"

proc _lst_99 {} { return [lsort -integer {97 98 99 0 1 2}] }
assert bulk-lst-99 [_lst_99] "0 1 2 97 98 99"

proc _lst_100 {} { return [lsort -integer {0 1 2 3 4 5}] }
assert bulk-lst-100 [_lst_100] "0 1 2 3 4 5"

proc _lst_101 {} { return [lsort -integer {3 4 5 6 7 8}] }
assert bulk-lst-101 [_lst_101] "3 4 5 6 7 8"

proc _lst_102 {} { return [lsort -integer {6 7 8 9 10 11}] }
assert bulk-lst-102 [_lst_102] "6 7 8 9 10 11"

proc _lst_103 {} { return [lsort -integer {9 10 11 12 13 14}] }
assert bulk-lst-103 [_lst_103] "9 10 11 12 13 14"

proc _lst_104 {} { return [lsort -integer {12 13 14 15 16 17}] }
assert bulk-lst-104 [_lst_104] "12 13 14 15 16 17"

proc _lst_105 {} { return [lsort -integer {15 16 17 18 19 20}] }
assert bulk-lst-105 [_lst_105] "15 16 17 18 19 20"

proc _lst_106 {} { return [lsort -integer {18 19 20 21 22 23}] }
assert bulk-lst-106 [_lst_106] "18 19 20 21 22 23"

proc _lst_107 {} { return [lsort -integer {21 22 23 24 25 26}] }
assert bulk-lst-107 [_lst_107] "21 22 23 24 25 26"

proc _lst_108 {} { return [lsort -integer {24 25 26 27 28 29}] }
assert bulk-lst-108 [_lst_108] "24 25 26 27 28 29"

proc _lst_109 {} { return [lsort -integer {27 28 29 30 31 32}] }
assert bulk-lst-109 [_lst_109] "27 28 29 30 31 32"

proc _lst_110 {} { return [lsort -integer {30 31 32 33 34 35}] }
assert bulk-lst-110 [_lst_110] "30 31 32 33 34 35"

proc _lst_111 {} { return [lsort -integer {33 34 35 36 37 38}] }
assert bulk-lst-111 [_lst_111] "33 34 35 36 37 38"

proc _lst_112 {} { return [lsort -integer {36 37 38 39 40 41}] }
assert bulk-lst-112 [_lst_112] "36 37 38 39 40 41"

proc _lst_113 {} { return [lsort -integer {39 40 41 42 43 44}] }
assert bulk-lst-113 [_lst_113] "39 40 41 42 43 44"

proc _lst_114 {} { return [lsort -integer {42 43 44 45 46 47}] }
assert bulk-lst-114 [_lst_114] "42 43 44 45 46 47"

proc _lst_115 {} { return [lsort -integer {45 46 47 48 49 50}] }
assert bulk-lst-115 [_lst_115] "45 46 47 48 49 50"

proc _lst_116 {} { return [lsort -integer {48 49 50 51 52 53}] }
assert bulk-lst-116 [_lst_116] "48 49 50 51 52 53"

proc _lst_117 {} { return [lsort -integer {51 52 53 54 55 56}] }
assert bulk-lst-117 [_lst_117] "51 52 53 54 55 56"

proc _lst_118 {} { return [lsort -integer {54 55 56 57 58 59}] }
assert bulk-lst-118 [_lst_118] "54 55 56 57 58 59"

proc _lst_119 {} { return [lsort -integer {57 58 59 60 61 62}] }
assert bulk-lst-119 [_lst_119] "57 58 59 60 61 62"

proc _lst_120 {} { return [lsort -integer {60 61 62 63 64 65}] }
assert bulk-lst-120 [_lst_120] "60 61 62 63 64 65"

proc _lst_121 {} { return [lsort -integer {63 64 65 66 67 68}] }
assert bulk-lst-121 [_lst_121] "63 64 65 66 67 68"

proc _lst_122 {} { return [lsort -integer {66 67 68 69 70 71}] }
assert bulk-lst-122 [_lst_122] "66 67 68 69 70 71"

proc _lst_123 {} { return [lsort -integer {69 70 71 72 73 74}] }
assert bulk-lst-123 [_lst_123] "69 70 71 72 73 74"

proc _lst_124 {} { return [lsort -integer {72 73 74 75 76 77}] }
assert bulk-lst-124 [_lst_124] "72 73 74 75 76 77"

proc _lst_125 {} { return [lsort -integer {75 76 77 78 79 80}] }
assert bulk-lst-125 [_lst_125] "75 76 77 78 79 80"

proc _lst_126 {} { return [lsort -integer {78 79 80 81 82 83}] }
assert bulk-lst-126 [_lst_126] "78 79 80 81 82 83"

proc _lst_127 {} { return [lsort -integer {81 82 83 84 85 86}] }
assert bulk-lst-127 [_lst_127] "81 82 83 84 85 86"

proc _lst_128 {} { return [lsort -integer {84 85 86 87 88 89}] }
assert bulk-lst-128 [_lst_128] "84 85 86 87 88 89"

proc _lst_129 {} { return [lsort -integer {87 88 89 90 91 92}] }
assert bulk-lst-129 [_lst_129] "87 88 89 90 91 92"

proc _lst_130 {} { return [lsort -integer {90 91 92 93 94 95}] }
assert bulk-lst-130 [_lst_130] "90 91 92 93 94 95"

proc _lst_131 {} { return [lsort -integer {93 94 95 96 97 98}] }
assert bulk-lst-131 [_lst_131] "93 94 95 96 97 98"

proc _lst_132 {} { return [lsort -integer {96 97 98 99 0 1}] }
assert bulk-lst-132 [_lst_132] "0 1 96 97 98 99"

proc _lst_133 {} { return [lsort -integer {99 0 1 2 3 4}] }
assert bulk-lst-133 [_lst_133] "0 1 2 3 4 99"

proc _lst_134 {} { return [lsort -integer {2 3 4 5 6 7}] }
assert bulk-lst-134 [_lst_134] "2 3 4 5 6 7"

proc _lst_135 {} { return [lsort -integer {5 6 7 8 9 10}] }
assert bulk-lst-135 [_lst_135] "5 6 7 8 9 10"

proc _lst_136 {} { return [lsort -integer {8 9 10 11 12 13}] }
assert bulk-lst-136 [_lst_136] "8 9 10 11 12 13"

proc _lst_137 {} { return [lsort -integer {11 12 13 14 15 16}] }
assert bulk-lst-137 [_lst_137] "11 12 13 14 15 16"

proc _lst_138 {} { return [lsort -integer {14 15 16 17 18 19}] }
assert bulk-lst-138 [_lst_138] "14 15 16 17 18 19"

proc _lst_139 {} { return [lsort -integer {17 18 19 20 21 22}] }
assert bulk-lst-139 [_lst_139] "17 18 19 20 21 22"

proc _lst_140 {} { return [lsort -integer {20 21 22 23 24 25}] }
assert bulk-lst-140 [_lst_140] "20 21 22 23 24 25"

proc _lst_141 {} { return [lsort -integer {23 24 25 26 27 28}] }
assert bulk-lst-141 [_lst_141] "23 24 25 26 27 28"

proc _lst_142 {} { return [lsort -integer {26 27 28 29 30 31}] }
assert bulk-lst-142 [_lst_142] "26 27 28 29 30 31"

proc _lst_143 {} { return [lsort -integer {29 30 31 32 33 34}] }
assert bulk-lst-143 [_lst_143] "29 30 31 32 33 34"

proc _lst_144 {} { return [lsort -integer {32 33 34 35 36 37}] }
assert bulk-lst-144 [_lst_144] "32 33 34 35 36 37"

proc _lst_145 {} { return [lsort -integer {35 36 37 38 39 40}] }
assert bulk-lst-145 [_lst_145] "35 36 37 38 39 40"

proc _lst_146 {} { return [lsort -integer {38 39 40 41 42 43}] }
assert bulk-lst-146 [_lst_146] "38 39 40 41 42 43"

proc _lst_147 {} { return [lsort -integer {41 42 43 44 45 46}] }
assert bulk-lst-147 [_lst_147] "41 42 43 44 45 46"

proc _lst_148 {} { return [lsort -integer {44 45 46 47 48 49}] }
assert bulk-lst-148 [_lst_148] "44 45 46 47 48 49"

proc _lst_149 {} { return [lsort -integer {47 48 49 50 51 52}] }
assert bulk-lst-149 [_lst_149] "47 48 49 50 51 52"

proc _dict_0 {} {
    set d [dict create k0 0 k1 3 k2 6 k3 9]
    return [dict get $d k2]
}
assert bulk-dict-0 [_dict_0] 6

proc _dict_1 {} {
    set d [dict create k0 7 k1 10 k2 13 k3 16]
    return [dict get $d k2]
}
assert bulk-dict-1 [_dict_1] 13

proc _dict_2 {} {
    set d [dict create k0 14 k1 17 k2 20 k3 23]
    return [dict get $d k2]
}
assert bulk-dict-2 [_dict_2] 20

proc _dict_3 {} {
    set d [dict create k0 21 k1 24 k2 27 k3 30]
    return [dict get $d k2]
}
assert bulk-dict-3 [_dict_3] 27

proc _dict_4 {} {
    set d [dict create k0 28 k1 31 k2 34 k3 37]
    return [dict get $d k2]
}
assert bulk-dict-4 [_dict_4] 34

proc _dict_5 {} {
    set d [dict create k0 35 k1 38 k2 41 k3 44]
    return [dict get $d k2]
}
assert bulk-dict-5 [_dict_5] 41

proc _dict_6 {} {
    set d [dict create k0 42 k1 45 k2 48 k3 51]
    return [dict get $d k2]
}
assert bulk-dict-6 [_dict_6] 48

proc _dict_7 {} {
    set d [dict create k0 49 k1 52 k2 55 k3 58]
    return [dict get $d k2]
}
assert bulk-dict-7 [_dict_7] 55

proc _dict_8 {} {
    set d [dict create k0 56 k1 59 k2 62 k3 65]
    return [dict get $d k2]
}
assert bulk-dict-8 [_dict_8] 62

proc _dict_9 {} {
    set d [dict create k0 63 k1 66 k2 69 k3 72]
    return [dict get $d k2]
}
assert bulk-dict-9 [_dict_9] 69

proc _dict_10 {} {
    set d [dict create k0 70 k1 73 k2 76 k3 79]
    return [dict get $d k2]
}
assert bulk-dict-10 [_dict_10] 76

proc _dict_11 {} {
    set d [dict create k0 77 k1 80 k2 83 k3 86]
    return [dict get $d k2]
}
assert bulk-dict-11 [_dict_11] 83

proc _dict_12 {} {
    set d [dict create k0 84 k1 87 k2 90 k3 93]
    return [dict get $d k2]
}
assert bulk-dict-12 [_dict_12] 90

proc _dict_13 {} {
    set d [dict create k0 91 k1 94 k2 97 k3 0]
    return [dict get $d k2]
}
assert bulk-dict-13 [_dict_13] 97

proc _dict_14 {} {
    set d [dict create k0 98 k1 1 k2 4 k3 7]
    return [dict get $d k2]
}
assert bulk-dict-14 [_dict_14] 4

proc _dict_15 {} {
    set d [dict create k0 5 k1 8 k2 11 k3 14]
    return [dict get $d k2]
}
assert bulk-dict-15 [_dict_15] 11

proc _dict_16 {} {
    set d [dict create k0 12 k1 15 k2 18 k3 21]
    return [dict get $d k2]
}
assert bulk-dict-16 [_dict_16] 18

proc _dict_17 {} {
    set d [dict create k0 19 k1 22 k2 25 k3 28]
    return [dict get $d k2]
}
assert bulk-dict-17 [_dict_17] 25

proc _dict_18 {} {
    set d [dict create k0 26 k1 29 k2 32 k3 35]
    return [dict get $d k2]
}
assert bulk-dict-18 [_dict_18] 32

proc _dict_19 {} {
    set d [dict create k0 33 k1 36 k2 39 k3 42]
    return [dict get $d k2]
}
assert bulk-dict-19 [_dict_19] 39

proc _dict_20 {} {
    set d [dict create k0 40 k1 43 k2 46 k3 49]
    return [dict get $d k2]
}
assert bulk-dict-20 [_dict_20] 46

proc _dict_21 {} {
    set d [dict create k0 47 k1 50 k2 53 k3 56]
    return [dict get $d k2]
}
assert bulk-dict-21 [_dict_21] 53

proc _dict_22 {} {
    set d [dict create k0 54 k1 57 k2 60 k3 63]
    return [dict get $d k2]
}
assert bulk-dict-22 [_dict_22] 60

proc _dict_23 {} {
    set d [dict create k0 61 k1 64 k2 67 k3 70]
    return [dict get $d k2]
}
assert bulk-dict-23 [_dict_23] 67

proc _dict_24 {} {
    set d [dict create k0 68 k1 71 k2 74 k3 77]
    return [dict get $d k2]
}
assert bulk-dict-24 [_dict_24] 74

proc _dict_25 {} {
    set d [dict create k0 75 k1 78 k2 81 k3 84]
    return [dict get $d k2]
}
assert bulk-dict-25 [_dict_25] 81

proc _dict_26 {} {
    set d [dict create k0 82 k1 85 k2 88 k3 91]
    return [dict get $d k2]
}
assert bulk-dict-26 [_dict_26] 88

proc _dict_27 {} {
    set d [dict create k0 89 k1 92 k2 95 k3 98]
    return [dict get $d k2]
}
assert bulk-dict-27 [_dict_27] 95

proc _dict_28 {} {
    set d [dict create k0 96 k1 99 k2 2 k3 5]
    return [dict get $d k2]
}
assert bulk-dict-28 [_dict_28] 2

proc _dict_29 {} {
    set d [dict create k0 3 k1 6 k2 9 k3 12]
    return [dict get $d k2]
}
assert bulk-dict-29 [_dict_29] 9

proc _dict_30 {} {
    set d [dict create k0 10 k1 13 k2 16 k3 19]
    return [dict get $d k2]
}
assert bulk-dict-30 [_dict_30] 16

proc _dict_31 {} {
    set d [dict create k0 17 k1 20 k2 23 k3 26]
    return [dict get $d k2]
}
assert bulk-dict-31 [_dict_31] 23

proc _dict_32 {} {
    set d [dict create k0 24 k1 27 k2 30 k3 33]
    return [dict get $d k2]
}
assert bulk-dict-32 [_dict_32] 30

proc _dict_33 {} {
    set d [dict create k0 31 k1 34 k2 37 k3 40]
    return [dict get $d k2]
}
assert bulk-dict-33 [_dict_33] 37

proc _dict_34 {} {
    set d [dict create k0 38 k1 41 k2 44 k3 47]
    return [dict get $d k2]
}
assert bulk-dict-34 [_dict_34] 44

proc _dict_35 {} {
    set d [dict create k0 45 k1 48 k2 51 k3 54]
    return [dict get $d k2]
}
assert bulk-dict-35 [_dict_35] 51

proc _dict_36 {} {
    set d [dict create k0 52 k1 55 k2 58 k3 61]
    return [dict get $d k2]
}
assert bulk-dict-36 [_dict_36] 58

proc _dict_37 {} {
    set d [dict create k0 59 k1 62 k2 65 k3 68]
    return [dict get $d k2]
}
assert bulk-dict-37 [_dict_37] 65

proc _dict_38 {} {
    set d [dict create k0 66 k1 69 k2 72 k3 75]
    return [dict get $d k2]
}
assert bulk-dict-38 [_dict_38] 72

proc _dict_39 {} {
    set d [dict create k0 73 k1 76 k2 79 k3 82]
    return [dict get $d k2]
}
assert bulk-dict-39 [_dict_39] 79

proc _dict_40 {} {
    set d [dict create k0 80 k1 83 k2 86 k3 89]
    return [dict get $d k2]
}
assert bulk-dict-40 [_dict_40] 86

proc _dict_41 {} {
    set d [dict create k0 87 k1 90 k2 93 k3 96]
    return [dict get $d k2]
}
assert bulk-dict-41 [_dict_41] 93

proc _dict_42 {} {
    set d [dict create k0 94 k1 97 k2 0 k3 3]
    return [dict get $d k2]
}
assert bulk-dict-42 [_dict_42] 0

proc _dict_43 {} {
    set d [dict create k0 1 k1 4 k2 7 k3 10]
    return [dict get $d k2]
}
assert bulk-dict-43 [_dict_43] 7

proc _dict_44 {} {
    set d [dict create k0 8 k1 11 k2 14 k3 17]
    return [dict get $d k2]
}
assert bulk-dict-44 [_dict_44] 14

proc _dict_45 {} {
    set d [dict create k0 15 k1 18 k2 21 k3 24]
    return [dict get $d k2]
}
assert bulk-dict-45 [_dict_45] 21

proc _dict_46 {} {
    set d [dict create k0 22 k1 25 k2 28 k3 31]
    return [dict get $d k2]
}
assert bulk-dict-46 [_dict_46] 28

proc _dict_47 {} {
    set d [dict create k0 29 k1 32 k2 35 k3 38]
    return [dict get $d k2]
}
assert bulk-dict-47 [_dict_47] 35

proc _dict_48 {} {
    set d [dict create k0 36 k1 39 k2 42 k3 45]
    return [dict get $d k2]
}
assert bulk-dict-48 [_dict_48] 42

proc _dict_49 {} {
    set d [dict create k0 43 k1 46 k2 49 k3 52]
    return [dict get $d k2]
}
assert bulk-dict-49 [_dict_49] 49

proc _dict_50 {} {
    set d [dict create k0 50 k1 53 k2 56 k3 59]
    return [dict get $d k2]
}
assert bulk-dict-50 [_dict_50] 56

proc _dict_51 {} {
    set d [dict create k0 57 k1 60 k2 63 k3 66]
    return [dict get $d k2]
}
assert bulk-dict-51 [_dict_51] 63

proc _dict_52 {} {
    set d [dict create k0 64 k1 67 k2 70 k3 73]
    return [dict get $d k2]
}
assert bulk-dict-52 [_dict_52] 70

proc _dict_53 {} {
    set d [dict create k0 71 k1 74 k2 77 k3 80]
    return [dict get $d k2]
}
assert bulk-dict-53 [_dict_53] 77

proc _dict_54 {} {
    set d [dict create k0 78 k1 81 k2 84 k3 87]
    return [dict get $d k2]
}
assert bulk-dict-54 [_dict_54] 84

proc _dict_55 {} {
    set d [dict create k0 85 k1 88 k2 91 k3 94]
    return [dict get $d k2]
}
assert bulk-dict-55 [_dict_55] 91

proc _dict_56 {} {
    set d [dict create k0 92 k1 95 k2 98 k3 1]
    return [dict get $d k2]
}
assert bulk-dict-56 [_dict_56] 98

proc _dict_57 {} {
    set d [dict create k0 99 k1 2 k2 5 k3 8]
    return [dict get $d k2]
}
assert bulk-dict-57 [_dict_57] 5

proc _dict_58 {} {
    set d [dict create k0 6 k1 9 k2 12 k3 15]
    return [dict get $d k2]
}
assert bulk-dict-58 [_dict_58] 12

proc _dict_59 {} {
    set d [dict create k0 13 k1 16 k2 19 k3 22]
    return [dict get $d k2]
}
assert bulk-dict-59 [_dict_59] 19

proc _dict_60 {} {
    set d [dict create k0 20 k1 23 k2 26 k3 29]
    return [dict get $d k2]
}
assert bulk-dict-60 [_dict_60] 26

proc _dict_61 {} {
    set d [dict create k0 27 k1 30 k2 33 k3 36]
    return [dict get $d k2]
}
assert bulk-dict-61 [_dict_61] 33

proc _dict_62 {} {
    set d [dict create k0 34 k1 37 k2 40 k3 43]
    return [dict get $d k2]
}
assert bulk-dict-62 [_dict_62] 40

proc _dict_63 {} {
    set d [dict create k0 41 k1 44 k2 47 k3 50]
    return [dict get $d k2]
}
assert bulk-dict-63 [_dict_63] 47

proc _dict_64 {} {
    set d [dict create k0 48 k1 51 k2 54 k3 57]
    return [dict get $d k2]
}
assert bulk-dict-64 [_dict_64] 54

proc _dict_65 {} {
    set d [dict create k0 55 k1 58 k2 61 k3 64]
    return [dict get $d k2]
}
assert bulk-dict-65 [_dict_65] 61

proc _dict_66 {} {
    set d [dict create k0 62 k1 65 k2 68 k3 71]
    return [dict get $d k2]
}
assert bulk-dict-66 [_dict_66] 68

proc _dict_67 {} {
    set d [dict create k0 69 k1 72 k2 75 k3 78]
    return [dict get $d k2]
}
assert bulk-dict-67 [_dict_67] 75

proc _dict_68 {} {
    set d [dict create k0 76 k1 79 k2 82 k3 85]
    return [dict get $d k2]
}
assert bulk-dict-68 [_dict_68] 82

proc _dict_69 {} {
    set d [dict create k0 83 k1 86 k2 89 k3 92]
    return [dict get $d k2]
}
assert bulk-dict-69 [_dict_69] 89

proc _dict_70 {} {
    set d [dict create k0 90 k1 93 k2 96 k3 99]
    return [dict get $d k2]
}
assert bulk-dict-70 [_dict_70] 96

proc _dict_71 {} {
    set d [dict create k0 97 k1 0 k2 3 k3 6]
    return [dict get $d k2]
}
assert bulk-dict-71 [_dict_71] 3

proc _dict_72 {} {
    set d [dict create k0 4 k1 7 k2 10 k3 13]
    return [dict get $d k2]
}
assert bulk-dict-72 [_dict_72] 10

proc _dict_73 {} {
    set d [dict create k0 11 k1 14 k2 17 k3 20]
    return [dict get $d k2]
}
assert bulk-dict-73 [_dict_73] 17

proc _dict_74 {} {
    set d [dict create k0 18 k1 21 k2 24 k3 27]
    return [dict get $d k2]
}
assert bulk-dict-74 [_dict_74] 24

proc _dict_75 {} {
    set d [dict create k0 25 k1 28 k2 31 k3 34]
    return [dict get $d k2]
}
assert bulk-dict-75 [_dict_75] 31

proc _dict_76 {} {
    set d [dict create k0 32 k1 35 k2 38 k3 41]
    return [dict get $d k2]
}
assert bulk-dict-76 [_dict_76] 38

proc _dict_77 {} {
    set d [dict create k0 39 k1 42 k2 45 k3 48]
    return [dict get $d k2]
}
assert bulk-dict-77 [_dict_77] 45

proc _dict_78 {} {
    set d [dict create k0 46 k1 49 k2 52 k3 55]
    return [dict get $d k2]
}
assert bulk-dict-78 [_dict_78] 52

proc _dict_79 {} {
    set d [dict create k0 53 k1 56 k2 59 k3 62]
    return [dict get $d k2]
}
assert bulk-dict-79 [_dict_79] 59

proc _dict_80 {} {
    set d [dict create k0 60 k1 63 k2 66 k3 69]
    return [dict get $d k2]
}
assert bulk-dict-80 [_dict_80] 66

proc _dict_81 {} {
    set d [dict create k0 67 k1 70 k2 73 k3 76]
    return [dict get $d k2]
}
assert bulk-dict-81 [_dict_81] 73

proc _dict_82 {} {
    set d [dict create k0 74 k1 77 k2 80 k3 83]
    return [dict get $d k2]
}
assert bulk-dict-82 [_dict_82] 80

proc _dict_83 {} {
    set d [dict create k0 81 k1 84 k2 87 k3 90]
    return [dict get $d k2]
}
assert bulk-dict-83 [_dict_83] 87

proc _dict_84 {} {
    set d [dict create k0 88 k1 91 k2 94 k3 97]
    return [dict get $d k2]
}
assert bulk-dict-84 [_dict_84] 94

proc _dict_85 {} {
    set d [dict create k0 95 k1 98 k2 1 k3 4]
    return [dict get $d k2]
}
assert bulk-dict-85 [_dict_85] 1

proc _dict_86 {} {
    set d [dict create k0 2 k1 5 k2 8 k3 11]
    return [dict get $d k2]
}
assert bulk-dict-86 [_dict_86] 8

proc _dict_87 {} {
    set d [dict create k0 9 k1 12 k2 15 k3 18]
    return [dict get $d k2]
}
assert bulk-dict-87 [_dict_87] 15

proc _dict_88 {} {
    set d [dict create k0 16 k1 19 k2 22 k3 25]
    return [dict get $d k2]
}
assert bulk-dict-88 [_dict_88] 22

proc _dict_89 {} {
    set d [dict create k0 23 k1 26 k2 29 k3 32]
    return [dict get $d k2]
}
assert bulk-dict-89 [_dict_89] 29

proc _dict_90 {} {
    set d [dict create k0 30 k1 33 k2 36 k3 39]
    return [dict get $d k2]
}
assert bulk-dict-90 [_dict_90] 36

proc _dict_91 {} {
    set d [dict create k0 37 k1 40 k2 43 k3 46]
    return [dict get $d k2]
}
assert bulk-dict-91 [_dict_91] 43

proc _dict_92 {} {
    set d [dict create k0 44 k1 47 k2 50 k3 53]
    return [dict get $d k2]
}
assert bulk-dict-92 [_dict_92] 50

proc _dict_93 {} {
    set d [dict create k0 51 k1 54 k2 57 k3 60]
    return [dict get $d k2]
}
assert bulk-dict-93 [_dict_93] 57

proc _dict_94 {} {
    set d [dict create k0 58 k1 61 k2 64 k3 67]
    return [dict get $d k2]
}
assert bulk-dict-94 [_dict_94] 64

proc _dict_95 {} {
    set d [dict create k0 65 k1 68 k2 71 k3 74]
    return [dict get $d k2]
}
assert bulk-dict-95 [_dict_95] 71

proc _dict_96 {} {
    set d [dict create k0 72 k1 75 k2 78 k3 81]
    return [dict get $d k2]
}
assert bulk-dict-96 [_dict_96] 78

proc _dict_97 {} {
    set d [dict create k0 79 k1 82 k2 85 k3 88]
    return [dict get $d k2]
}
assert bulk-dict-97 [_dict_97] 85

proc _dict_98 {} {
    set d [dict create k0 86 k1 89 k2 92 k3 95]
    return [dict get $d k2]
}
assert bulk-dict-98 [_dict_98] 92

proc _dict_99 {} {
    set d [dict create k0 93 k1 96 k2 99 k3 2]
    return [dict get $d k2]
}
assert bulk-dict-99 [_dict_99] 99

proc _fe_0 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-0 [_fe_0] 6

proc _fe_1 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-1 [_fe_1] 10

proc _fe_2 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-2 [_fe_2] 15

proc _fe_3 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-3 [_fe_3] 21

proc _fe_4 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-4 [_fe_4] 28

proc _fe_5 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-5 [_fe_5] 36

proc _fe_6 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-6 [_fe_6] 45

proc _fe_7 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-7 [_fe_7] 55

proc _fe_8 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-8 [_fe_8] 6

proc _fe_9 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-9 [_fe_9] 10

proc _fe_10 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-10 [_fe_10] 15

proc _fe_11 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-11 [_fe_11] 21

proc _fe_12 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-12 [_fe_12] 28

proc _fe_13 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-13 [_fe_13] 36

proc _fe_14 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-14 [_fe_14] 45

proc _fe_15 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-15 [_fe_15] 55

proc _fe_16 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-16 [_fe_16] 6

proc _fe_17 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-17 [_fe_17] 10

proc _fe_18 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-18 [_fe_18] 15

proc _fe_19 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-19 [_fe_19] 21

proc _fe_20 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-20 [_fe_20] 28

proc _fe_21 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-21 [_fe_21] 36

proc _fe_22 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-22 [_fe_22] 45

proc _fe_23 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-23 [_fe_23] 55

proc _fe_24 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-24 [_fe_24] 6

proc _fe_25 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-25 [_fe_25] 10

proc _fe_26 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-26 [_fe_26] 15

proc _fe_27 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-27 [_fe_27] 21

proc _fe_28 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-28 [_fe_28] 28

proc _fe_29 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-29 [_fe_29] 36

proc _fe_30 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-30 [_fe_30] 45

proc _fe_31 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-31 [_fe_31] 55

proc _fe_32 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-32 [_fe_32] 6

proc _fe_33 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-33 [_fe_33] 10

proc _fe_34 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-34 [_fe_34] 15

proc _fe_35 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-35 [_fe_35] 21

proc _fe_36 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-36 [_fe_36] 28

proc _fe_37 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-37 [_fe_37] 36

proc _fe_38 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-38 [_fe_38] 45

proc _fe_39 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-39 [_fe_39] 55

proc _fe_40 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-40 [_fe_40] 6

proc _fe_41 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-41 [_fe_41] 10

proc _fe_42 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-42 [_fe_42] 15

proc _fe_43 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-43 [_fe_43] 21

proc _fe_44 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-44 [_fe_44] 28

proc _fe_45 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-45 [_fe_45] 36

proc _fe_46 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-46 [_fe_46] 45

proc _fe_47 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-47 [_fe_47] 55

proc _fe_48 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-48 [_fe_48] 6

proc _fe_49 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-49 [_fe_49] 10

proc _fe_50 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-50 [_fe_50] 15

proc _fe_51 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-51 [_fe_51] 21

proc _fe_52 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-52 [_fe_52] 28

proc _fe_53 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-53 [_fe_53] 36

proc _fe_54 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-54 [_fe_54] 45

proc _fe_55 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-55 [_fe_55] 55

proc _fe_56 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-56 [_fe_56] 6

proc _fe_57 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-57 [_fe_57] 10

proc _fe_58 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-58 [_fe_58] 15

proc _fe_59 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-59 [_fe_59] 21

proc _fe_60 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-60 [_fe_60] 28

proc _fe_61 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-61 [_fe_61] 36

proc _fe_62 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-62 [_fe_62] 45

proc _fe_63 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-63 [_fe_63] 55

proc _fe_64 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-64 [_fe_64] 6

proc _fe_65 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-65 [_fe_65] 10

proc _fe_66 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-66 [_fe_66] 15

proc _fe_67 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-67 [_fe_67] 21

proc _fe_68 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-68 [_fe_68] 28

proc _fe_69 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-69 [_fe_69] 36

proc _fe_70 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-70 [_fe_70] 45

proc _fe_71 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-71 [_fe_71] 55

proc _fe_72 {} {
    set s 0
    foreach x {1 2 3} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-72 [_fe_72] 6

proc _fe_73 {} {
    set s 0
    foreach x {1 2 3 4} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-73 [_fe_73] 10

proc _fe_74 {} {
    set s 0
    foreach x {1 2 3 4 5} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-74 [_fe_74] 15

proc _fe_75 {} {
    set s 0
    foreach x {1 2 3 4 5 6} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-75 [_fe_75] 21

proc _fe_76 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-76 [_fe_76] 28

proc _fe_77 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-77 [_fe_77] 36

proc _fe_78 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-78 [_fe_78] 45

proc _fe_79 {} {
    set s 0
    foreach x {1 2 3 4 5 6 7 8 9 10} { set s [expr {$s + $x}] }
    return $s
}
assert bulk-fe-79 [_fe_79] 55

proc _sw_0 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 1 }
        case2 { return 2 }
        case3 { return 3 }
        case4 { return 4 }
        default { return -1 }
    }
}
assert bulk-sw-0 [_sw_0 case2] 2

proc _sw_1 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 2 }
        case2 { return 4 }
        case3 { return 6 }
        case4 { return 8 }
        default { return -1 }
    }
}
assert bulk-sw-1 [_sw_1 case2] 4

proc _sw_2 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 3 }
        case2 { return 6 }
        case3 { return 9 }
        case4 { return 12 }
        default { return -1 }
    }
}
assert bulk-sw-2 [_sw_2 case2] 6

proc _sw_3 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 4 }
        case2 { return 8 }
        case3 { return 12 }
        case4 { return 16 }
        default { return -1 }
    }
}
assert bulk-sw-3 [_sw_3 case2] 8

proc _sw_4 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 5 }
        case2 { return 10 }
        case3 { return 15 }
        case4 { return 20 }
        default { return -1 }
    }
}
assert bulk-sw-4 [_sw_4 case2] 10

proc _sw_5 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 6 }
        case2 { return 12 }
        case3 { return 18 }
        case4 { return 24 }
        default { return -1 }
    }
}
assert bulk-sw-5 [_sw_5 case2] 12

proc _sw_6 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 7 }
        case2 { return 14 }
        case3 { return 21 }
        case4 { return 28 }
        default { return -1 }
    }
}
assert bulk-sw-6 [_sw_6 case2] 14

proc _sw_7 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 8 }
        case2 { return 16 }
        case3 { return 24 }
        case4 { return 32 }
        default { return -1 }
    }
}
assert bulk-sw-7 [_sw_7 case2] 16

proc _sw_8 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 9 }
        case2 { return 18 }
        case3 { return 27 }
        case4 { return 36 }
        default { return -1 }
    }
}
assert bulk-sw-8 [_sw_8 case2] 18

proc _sw_9 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 10 }
        case2 { return 20 }
        case3 { return 30 }
        case4 { return 40 }
        default { return -1 }
    }
}
assert bulk-sw-9 [_sw_9 case2] 20

proc _sw_10 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 11 }
        case2 { return 22 }
        case3 { return 33 }
        case4 { return 44 }
        default { return -1 }
    }
}
assert bulk-sw-10 [_sw_10 case2] 22

proc _sw_11 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 12 }
        case2 { return 24 }
        case3 { return 36 }
        case4 { return 48 }
        default { return -1 }
    }
}
assert bulk-sw-11 [_sw_11 case2] 24

proc _sw_12 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 13 }
        case2 { return 26 }
        case3 { return 39 }
        case4 { return 52 }
        default { return -1 }
    }
}
assert bulk-sw-12 [_sw_12 case2] 26

proc _sw_13 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 14 }
        case2 { return 28 }
        case3 { return 42 }
        case4 { return 56 }
        default { return -1 }
    }
}
assert bulk-sw-13 [_sw_13 case2] 28

proc _sw_14 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 15 }
        case2 { return 30 }
        case3 { return 45 }
        case4 { return 60 }
        default { return -1 }
    }
}
assert bulk-sw-14 [_sw_14 case2] 30

proc _sw_15 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 16 }
        case2 { return 32 }
        case3 { return 48 }
        case4 { return 64 }
        default { return -1 }
    }
}
assert bulk-sw-15 [_sw_15 case2] 32

proc _sw_16 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 17 }
        case2 { return 34 }
        case3 { return 51 }
        case4 { return 68 }
        default { return -1 }
    }
}
assert bulk-sw-16 [_sw_16 case2] 34

proc _sw_17 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 18 }
        case2 { return 36 }
        case3 { return 54 }
        case4 { return 72 }
        default { return -1 }
    }
}
assert bulk-sw-17 [_sw_17 case2] 36

proc _sw_18 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 19 }
        case2 { return 38 }
        case3 { return 57 }
        case4 { return 76 }
        default { return -1 }
    }
}
assert bulk-sw-18 [_sw_18 case2] 38

proc _sw_19 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 20 }
        case2 { return 40 }
        case3 { return 60 }
        case4 { return 80 }
        default { return -1 }
    }
}
assert bulk-sw-19 [_sw_19 case2] 40

proc _sw_20 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 21 }
        case2 { return 42 }
        case3 { return 63 }
        case4 { return 84 }
        default { return -1 }
    }
}
assert bulk-sw-20 [_sw_20 case2] 42

proc _sw_21 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 22 }
        case2 { return 44 }
        case3 { return 66 }
        case4 { return 88 }
        default { return -1 }
    }
}
assert bulk-sw-21 [_sw_21 case2] 44

proc _sw_22 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 23 }
        case2 { return 46 }
        case3 { return 69 }
        case4 { return 92 }
        default { return -1 }
    }
}
assert bulk-sw-22 [_sw_22 case2] 46

proc _sw_23 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 24 }
        case2 { return 48 }
        case3 { return 72 }
        case4 { return 96 }
        default { return -1 }
    }
}
assert bulk-sw-23 [_sw_23 case2] 48

proc _sw_24 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 25 }
        case2 { return 50 }
        case3 { return 75 }
        case4 { return 100 }
        default { return -1 }
    }
}
assert bulk-sw-24 [_sw_24 case2] 50

proc _sw_25 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 26 }
        case2 { return 52 }
        case3 { return 78 }
        case4 { return 104 }
        default { return -1 }
    }
}
assert bulk-sw-25 [_sw_25 case2] 52

proc _sw_26 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 27 }
        case2 { return 54 }
        case3 { return 81 }
        case4 { return 108 }
        default { return -1 }
    }
}
assert bulk-sw-26 [_sw_26 case2] 54

proc _sw_27 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 28 }
        case2 { return 56 }
        case3 { return 84 }
        case4 { return 112 }
        default { return -1 }
    }
}
assert bulk-sw-27 [_sw_27 case2] 56

proc _sw_28 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 29 }
        case2 { return 58 }
        case3 { return 87 }
        case4 { return 116 }
        default { return -1 }
    }
}
assert bulk-sw-28 [_sw_28 case2] 58

proc _sw_29 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 30 }
        case2 { return 60 }
        case3 { return 90 }
        case4 { return 120 }
        default { return -1 }
    }
}
assert bulk-sw-29 [_sw_29 case2] 60

proc _sw_30 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 31 }
        case2 { return 62 }
        case3 { return 93 }
        case4 { return 124 }
        default { return -1 }
    }
}
assert bulk-sw-30 [_sw_30 case2] 62

proc _sw_31 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 32 }
        case2 { return 64 }
        case3 { return 96 }
        case4 { return 128 }
        default { return -1 }
    }
}
assert bulk-sw-31 [_sw_31 case2] 64

proc _sw_32 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 33 }
        case2 { return 66 }
        case3 { return 99 }
        case4 { return 132 }
        default { return -1 }
    }
}
assert bulk-sw-32 [_sw_32 case2] 66

proc _sw_33 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 34 }
        case2 { return 68 }
        case3 { return 102 }
        case4 { return 136 }
        default { return -1 }
    }
}
assert bulk-sw-33 [_sw_33 case2] 68

proc _sw_34 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 35 }
        case2 { return 70 }
        case3 { return 105 }
        case4 { return 140 }
        default { return -1 }
    }
}
assert bulk-sw-34 [_sw_34 case2] 70

proc _sw_35 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 36 }
        case2 { return 72 }
        case3 { return 108 }
        case4 { return 144 }
        default { return -1 }
    }
}
assert bulk-sw-35 [_sw_35 case2] 72

proc _sw_36 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 37 }
        case2 { return 74 }
        case3 { return 111 }
        case4 { return 148 }
        default { return -1 }
    }
}
assert bulk-sw-36 [_sw_36 case2] 74

proc _sw_37 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 38 }
        case2 { return 76 }
        case3 { return 114 }
        case4 { return 152 }
        default { return -1 }
    }
}
assert bulk-sw-37 [_sw_37 case2] 76

proc _sw_38 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 39 }
        case2 { return 78 }
        case3 { return 117 }
        case4 { return 156 }
        default { return -1 }
    }
}
assert bulk-sw-38 [_sw_38 case2] 78

proc _sw_39 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 40 }
        case2 { return 80 }
        case3 { return 120 }
        case4 { return 160 }
        default { return -1 }
    }
}
assert bulk-sw-39 [_sw_39 case2] 80

proc _sw_40 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 41 }
        case2 { return 82 }
        case3 { return 123 }
        case4 { return 164 }
        default { return -1 }
    }
}
assert bulk-sw-40 [_sw_40 case2] 82

proc _sw_41 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 42 }
        case2 { return 84 }
        case3 { return 126 }
        case4 { return 168 }
        default { return -1 }
    }
}
assert bulk-sw-41 [_sw_41 case2] 84

proc _sw_42 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 43 }
        case2 { return 86 }
        case3 { return 129 }
        case4 { return 172 }
        default { return -1 }
    }
}
assert bulk-sw-42 [_sw_42 case2] 86

proc _sw_43 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 44 }
        case2 { return 88 }
        case3 { return 132 }
        case4 { return 176 }
        default { return -1 }
    }
}
assert bulk-sw-43 [_sw_43 case2] 88

proc _sw_44 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 45 }
        case2 { return 90 }
        case3 { return 135 }
        case4 { return 180 }
        default { return -1 }
    }
}
assert bulk-sw-44 [_sw_44 case2] 90

proc _sw_45 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 46 }
        case2 { return 92 }
        case3 { return 138 }
        case4 { return 184 }
        default { return -1 }
    }
}
assert bulk-sw-45 [_sw_45 case2] 92

proc _sw_46 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 47 }
        case2 { return 94 }
        case3 { return 141 }
        case4 { return 188 }
        default { return -1 }
    }
}
assert bulk-sw-46 [_sw_46 case2] 94

proc _sw_47 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 48 }
        case2 { return 96 }
        case3 { return 144 }
        case4 { return 192 }
        default { return -1 }
    }
}
assert bulk-sw-47 [_sw_47 case2] 96

proc _sw_48 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 49 }
        case2 { return 98 }
        case3 { return 147 }
        case4 { return 196 }
        default { return -1 }
    }
}
assert bulk-sw-48 [_sw_48 case2] 98

proc _sw_49 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 50 }
        case2 { return 100 }
        case3 { return 150 }
        case4 { return 200 }
        default { return -1 }
    }
}
assert bulk-sw-49 [_sw_49 case2] 100

proc _sw_50 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 51 }
        case2 { return 102 }
        case3 { return 153 }
        case4 { return 204 }
        default { return -1 }
    }
}
assert bulk-sw-50 [_sw_50 case2] 102

proc _sw_51 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 52 }
        case2 { return 104 }
        case3 { return 156 }
        case4 { return 208 }
        default { return -1 }
    }
}
assert bulk-sw-51 [_sw_51 case2] 104

proc _sw_52 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 53 }
        case2 { return 106 }
        case3 { return 159 }
        case4 { return 212 }
        default { return -1 }
    }
}
assert bulk-sw-52 [_sw_52 case2] 106

proc _sw_53 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 54 }
        case2 { return 108 }
        case3 { return 162 }
        case4 { return 216 }
        default { return -1 }
    }
}
assert bulk-sw-53 [_sw_53 case2] 108

proc _sw_54 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 55 }
        case2 { return 110 }
        case3 { return 165 }
        case4 { return 220 }
        default { return -1 }
    }
}
assert bulk-sw-54 [_sw_54 case2] 110

proc _sw_55 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 56 }
        case2 { return 112 }
        case3 { return 168 }
        case4 { return 224 }
        default { return -1 }
    }
}
assert bulk-sw-55 [_sw_55 case2] 112

proc _sw_56 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 57 }
        case2 { return 114 }
        case3 { return 171 }
        case4 { return 228 }
        default { return -1 }
    }
}
assert bulk-sw-56 [_sw_56 case2] 114

proc _sw_57 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 58 }
        case2 { return 116 }
        case3 { return 174 }
        case4 { return 232 }
        default { return -1 }
    }
}
assert bulk-sw-57 [_sw_57 case2] 116

proc _sw_58 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 59 }
        case2 { return 118 }
        case3 { return 177 }
        case4 { return 236 }
        default { return -1 }
    }
}
assert bulk-sw-58 [_sw_58 case2] 118

proc _sw_59 {v} {
    switch $v {
        case0 { return 0 }
        case1 { return 60 }
        case2 { return 120 }
        case3 { return 180 }
        case4 { return 240 }
        default { return -1 }
    }
}
assert bulk-sw-59 [_sw_59 case2] 120

proc _try_0 {x} {
    try {
        if {$x == 0} { error "zero-0" }
        return [expr {1000 + $x + 0}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-0-ok [_try_0 1] 1001
assert bulk-try-0-err [_try_0 0] "E:zero-0"

proc _try_1 {x} {
    try {
        if {$x == 0} { error "zero-1" }
        return [expr {1000 + $x + 1}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-1-ok [_try_1 1] 1002
assert bulk-try-1-err [_try_1 0] "E:zero-1"

proc _try_2 {x} {
    try {
        if {$x == 0} { error "zero-2" }
        return [expr {1000 + $x + 2}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-2-ok [_try_2 1] 1003
assert bulk-try-2-err [_try_2 0] "E:zero-2"

proc _try_3 {x} {
    try {
        if {$x == 0} { error "zero-3" }
        return [expr {1000 + $x + 3}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-3-ok [_try_3 1] 1004
assert bulk-try-3-err [_try_3 0] "E:zero-3"

proc _try_4 {x} {
    try {
        if {$x == 0} { error "zero-4" }
        return [expr {1000 + $x + 4}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-4-ok [_try_4 1] 1005
assert bulk-try-4-err [_try_4 0] "E:zero-4"

proc _try_5 {x} {
    try {
        if {$x == 0} { error "zero-5" }
        return [expr {1000 + $x + 5}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-5-ok [_try_5 1] 1006
assert bulk-try-5-err [_try_5 0] "E:zero-5"

proc _try_6 {x} {
    try {
        if {$x == 0} { error "zero-6" }
        return [expr {1000 + $x + 6}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-6-ok [_try_6 1] 1007
assert bulk-try-6-err [_try_6 0] "E:zero-6"

proc _try_7 {x} {
    try {
        if {$x == 0} { error "zero-7" }
        return [expr {1000 + $x + 7}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-7-ok [_try_7 1] 1008
assert bulk-try-7-err [_try_7 0] "E:zero-7"

proc _try_8 {x} {
    try {
        if {$x == 0} { error "zero-8" }
        return [expr {1000 + $x + 8}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-8-ok [_try_8 1] 1009
assert bulk-try-8-err [_try_8 0] "E:zero-8"

proc _try_9 {x} {
    try {
        if {$x == 0} { error "zero-9" }
        return [expr {1000 + $x + 9}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-9-ok [_try_9 1] 1010
assert bulk-try-9-err [_try_9 0] "E:zero-9"

proc _try_10 {x} {
    try {
        if {$x == 0} { error "zero-10" }
        return [expr {1000 + $x + 10}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-10-ok [_try_10 1] 1011
assert bulk-try-10-err [_try_10 0] "E:zero-10"

proc _try_11 {x} {
    try {
        if {$x == 0} { error "zero-11" }
        return [expr {1000 + $x + 11}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-11-ok [_try_11 1] 1012
assert bulk-try-11-err [_try_11 0] "E:zero-11"

proc _try_12 {x} {
    try {
        if {$x == 0} { error "zero-12" }
        return [expr {1000 + $x + 12}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-12-ok [_try_12 1] 1013
assert bulk-try-12-err [_try_12 0] "E:zero-12"

proc _try_13 {x} {
    try {
        if {$x == 0} { error "zero-13" }
        return [expr {1000 + $x + 13}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-13-ok [_try_13 1] 1014
assert bulk-try-13-err [_try_13 0] "E:zero-13"

proc _try_14 {x} {
    try {
        if {$x == 0} { error "zero-14" }
        return [expr {1000 + $x + 14}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-14-ok [_try_14 1] 1015
assert bulk-try-14-err [_try_14 0] "E:zero-14"

proc _try_15 {x} {
    try {
        if {$x == 0} { error "zero-15" }
        return [expr {1000 + $x + 15}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-15-ok [_try_15 1] 1016
assert bulk-try-15-err [_try_15 0] "E:zero-15"

proc _try_16 {x} {
    try {
        if {$x == 0} { error "zero-16" }
        return [expr {1000 + $x + 16}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-16-ok [_try_16 1] 1017
assert bulk-try-16-err [_try_16 0] "E:zero-16"

proc _try_17 {x} {
    try {
        if {$x == 0} { error "zero-17" }
        return [expr {1000 + $x + 17}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-17-ok [_try_17 1] 1018
assert bulk-try-17-err [_try_17 0] "E:zero-17"

proc _try_18 {x} {
    try {
        if {$x == 0} { error "zero-18" }
        return [expr {1000 + $x + 18}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-18-ok [_try_18 1] 1019
assert bulk-try-18-err [_try_18 0] "E:zero-18"

proc _try_19 {x} {
    try {
        if {$x == 0} { error "zero-19" }
        return [expr {1000 + $x + 19}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-19-ok [_try_19 1] 1020
assert bulk-try-19-err [_try_19 0] "E:zero-19"

proc _try_20 {x} {
    try {
        if {$x == 0} { error "zero-20" }
        return [expr {1000 + $x + 20}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-20-ok [_try_20 1] 1021
assert bulk-try-20-err [_try_20 0] "E:zero-20"

proc _try_21 {x} {
    try {
        if {$x == 0} { error "zero-21" }
        return [expr {1000 + $x + 21}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-21-ok [_try_21 1] 1022
assert bulk-try-21-err [_try_21 0] "E:zero-21"

proc _try_22 {x} {
    try {
        if {$x == 0} { error "zero-22" }
        return [expr {1000 + $x + 22}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-22-ok [_try_22 1] 1023
assert bulk-try-22-err [_try_22 0] "E:zero-22"

proc _try_23 {x} {
    try {
        if {$x == 0} { error "zero-23" }
        return [expr {1000 + $x + 23}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-23-ok [_try_23 1] 1024
assert bulk-try-23-err [_try_23 0] "E:zero-23"

proc _try_24 {x} {
    try {
        if {$x == 0} { error "zero-24" }
        return [expr {1000 + $x + 24}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-24-ok [_try_24 1] 1025
assert bulk-try-24-err [_try_24 0] "E:zero-24"

proc _try_25 {x} {
    try {
        if {$x == 0} { error "zero-25" }
        return [expr {1000 + $x + 25}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-25-ok [_try_25 1] 1026
assert bulk-try-25-err [_try_25 0] "E:zero-25"

proc _try_26 {x} {
    try {
        if {$x == 0} { error "zero-26" }
        return [expr {1000 + $x + 26}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-26-ok [_try_26 1] 1027
assert bulk-try-26-err [_try_26 0] "E:zero-26"

proc _try_27 {x} {
    try {
        if {$x == 0} { error "zero-27" }
        return [expr {1000 + $x + 27}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-27-ok [_try_27 1] 1028
assert bulk-try-27-err [_try_27 0] "E:zero-27"

proc _try_28 {x} {
    try {
        if {$x == 0} { error "zero-28" }
        return [expr {1000 + $x + 28}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-28-ok [_try_28 1] 1029
assert bulk-try-28-err [_try_28 0] "E:zero-28"

proc _try_29 {x} {
    try {
        if {$x == 0} { error "zero-29" }
        return [expr {1000 + $x + 29}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-29-ok [_try_29 1] 1030
assert bulk-try-29-err [_try_29 0] "E:zero-29"

proc _try_30 {x} {
    try {
        if {$x == 0} { error "zero-30" }
        return [expr {1000 + $x + 30}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-30-ok [_try_30 1] 1031
assert bulk-try-30-err [_try_30 0] "E:zero-30"

proc _try_31 {x} {
    try {
        if {$x == 0} { error "zero-31" }
        return [expr {1000 + $x + 31}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-31-ok [_try_31 1] 1032
assert bulk-try-31-err [_try_31 0] "E:zero-31"

proc _try_32 {x} {
    try {
        if {$x == 0} { error "zero-32" }
        return [expr {1000 + $x + 32}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-32-ok [_try_32 1] 1033
assert bulk-try-32-err [_try_32 0] "E:zero-32"

proc _try_33 {x} {
    try {
        if {$x == 0} { error "zero-33" }
        return [expr {1000 + $x + 33}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-33-ok [_try_33 1] 1034
assert bulk-try-33-err [_try_33 0] "E:zero-33"

proc _try_34 {x} {
    try {
        if {$x == 0} { error "zero-34" }
        return [expr {1000 + $x + 34}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-34-ok [_try_34 1] 1035
assert bulk-try-34-err [_try_34 0] "E:zero-34"

proc _try_35 {x} {
    try {
        if {$x == 0} { error "zero-35" }
        return [expr {1000 + $x + 35}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-35-ok [_try_35 1] 1036
assert bulk-try-35-err [_try_35 0] "E:zero-35"

proc _try_36 {x} {
    try {
        if {$x == 0} { error "zero-36" }
        return [expr {1000 + $x + 36}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-36-ok [_try_36 1] 1037
assert bulk-try-36-err [_try_36 0] "E:zero-36"

proc _try_37 {x} {
    try {
        if {$x == 0} { error "zero-37" }
        return [expr {1000 + $x + 37}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-37-ok [_try_37 1] 1038
assert bulk-try-37-err [_try_37 0] "E:zero-37"

proc _try_38 {x} {
    try {
        if {$x == 0} { error "zero-38" }
        return [expr {1000 + $x + 38}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-38-ok [_try_38 1] 1039
assert bulk-try-38-err [_try_38 0] "E:zero-38"

proc _try_39 {x} {
    try {
        if {$x == 0} { error "zero-39" }
        return [expr {1000 + $x + 39}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-39-ok [_try_39 1] 1040
assert bulk-try-39-err [_try_39 0] "E:zero-39"

proc _try_40 {x} {
    try {
        if {$x == 0} { error "zero-40" }
        return [expr {1000 + $x + 40}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-40-ok [_try_40 1] 1041
assert bulk-try-40-err [_try_40 0] "E:zero-40"

proc _try_41 {x} {
    try {
        if {$x == 0} { error "zero-41" }
        return [expr {1000 + $x + 41}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-41-ok [_try_41 1] 1042
assert bulk-try-41-err [_try_41 0] "E:zero-41"

proc _try_42 {x} {
    try {
        if {$x == 0} { error "zero-42" }
        return [expr {1000 + $x + 42}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-42-ok [_try_42 1] 1043
assert bulk-try-42-err [_try_42 0] "E:zero-42"

proc _try_43 {x} {
    try {
        if {$x == 0} { error "zero-43" }
        return [expr {1000 + $x + 43}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-43-ok [_try_43 1] 1044
assert bulk-try-43-err [_try_43 0] "E:zero-43"

proc _try_44 {x} {
    try {
        if {$x == 0} { error "zero-44" }
        return [expr {1000 + $x + 44}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-44-ok [_try_44 1] 1045
assert bulk-try-44-err [_try_44 0] "E:zero-44"

proc _try_45 {x} {
    try {
        if {$x == 0} { error "zero-45" }
        return [expr {1000 + $x + 45}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-45-ok [_try_45 1] 1046
assert bulk-try-45-err [_try_45 0] "E:zero-45"

proc _try_46 {x} {
    try {
        if {$x == 0} { error "zero-46" }
        return [expr {1000 + $x + 46}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-46-ok [_try_46 1] 1047
assert bulk-try-46-err [_try_46 0] "E:zero-46"

proc _try_47 {x} {
    try {
        if {$x == 0} { error "zero-47" }
        return [expr {1000 + $x + 47}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-47-ok [_try_47 1] 1048
assert bulk-try-47-err [_try_47 0] "E:zero-47"

proc _try_48 {x} {
    try {
        if {$x == 0} { error "zero-48" }
        return [expr {1000 + $x + 48}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-48-ok [_try_48 1] 1049
assert bulk-try-48-err [_try_48 0] "E:zero-48"

proc _try_49 {x} {
    try {
        if {$x == 0} { error "zero-49" }
        return [expr {1000 + $x + 49}]
    } on error {msg} {
        return "E:$msg"
    }
}
assert bulk-try-49-ok [_try_49 1] 1050
assert bulk-try-49-err [_try_49 0] "E:zero-49"

set _lam_0 {x {expr {$x + 0}}}
assert bulk-lam-0 [apply $_lam_0 10] 10

set _lam_1 {x {expr {$x + 7}}}
assert bulk-lam-1 [apply $_lam_1 10] 17

set _lam_2 {x {expr {$x + 14}}}
assert bulk-lam-2 [apply $_lam_2 10] 24

set _lam_3 {x {expr {$x + 21}}}
assert bulk-lam-3 [apply $_lam_3 10] 31

set _lam_4 {x {expr {$x + 28}}}
assert bulk-lam-4 [apply $_lam_4 10] 38

set _lam_5 {x {expr {$x + 35}}}
assert bulk-lam-5 [apply $_lam_5 10] 45

set _lam_6 {x {expr {$x + 42}}}
assert bulk-lam-6 [apply $_lam_6 10] 52

set _lam_7 {x {expr {$x + 49}}}
assert bulk-lam-7 [apply $_lam_7 10] 59

set _lam_8 {x {expr {$x + 56}}}
assert bulk-lam-8 [apply $_lam_8 10] 66

set _lam_9 {x {expr {$x + 63}}}
assert bulk-lam-9 [apply $_lam_9 10] 73

set _lam_10 {x {expr {$x + 70}}}
assert bulk-lam-10 [apply $_lam_10 10] 80

set _lam_11 {x {expr {$x + 77}}}
assert bulk-lam-11 [apply $_lam_11 10] 87

set _lam_12 {x {expr {$x + 84}}}
assert bulk-lam-12 [apply $_lam_12 10] 94

set _lam_13 {x {expr {$x + 91}}}
assert bulk-lam-13 [apply $_lam_13 10] 101

set _lam_14 {x {expr {$x + 98}}}
assert bulk-lam-14 [apply $_lam_14 10] 108

set _lam_15 {x {expr {$x + 5}}}
assert bulk-lam-15 [apply $_lam_15 10] 15

set _lam_16 {x {expr {$x + 12}}}
assert bulk-lam-16 [apply $_lam_16 10] 22

set _lam_17 {x {expr {$x + 19}}}
assert bulk-lam-17 [apply $_lam_17 10] 29

set _lam_18 {x {expr {$x + 26}}}
assert bulk-lam-18 [apply $_lam_18 10] 36

set _lam_19 {x {expr {$x + 33}}}
assert bulk-lam-19 [apply $_lam_19 10] 43

set _lam_20 {x {expr {$x + 40}}}
assert bulk-lam-20 [apply $_lam_20 10] 50

set _lam_21 {x {expr {$x + 47}}}
assert bulk-lam-21 [apply $_lam_21 10] 57

set _lam_22 {x {expr {$x + 54}}}
assert bulk-lam-22 [apply $_lam_22 10] 64

set _lam_23 {x {expr {$x + 61}}}
assert bulk-lam-23 [apply $_lam_23 10] 71

set _lam_24 {x {expr {$x + 68}}}
assert bulk-lam-24 [apply $_lam_24 10] 78

set _lam_25 {x {expr {$x + 75}}}
assert bulk-lam-25 [apply $_lam_25 10] 85

set _lam_26 {x {expr {$x + 82}}}
assert bulk-lam-26 [apply $_lam_26 10] 92

set _lam_27 {x {expr {$x + 89}}}
assert bulk-lam-27 [apply $_lam_27 10] 99

set _lam_28 {x {expr {$x + 96}}}
assert bulk-lam-28 [apply $_lam_28 10] 106

set _lam_29 {x {expr {$x + 3}}}
assert bulk-lam-29 [apply $_lam_29 10] 13

set _lam_30 {x {expr {$x + 10}}}
assert bulk-lam-30 [apply $_lam_30 10] 20

set _lam_31 {x {expr {$x + 17}}}
assert bulk-lam-31 [apply $_lam_31 10] 27

set _lam_32 {x {expr {$x + 24}}}
assert bulk-lam-32 [apply $_lam_32 10] 34

set _lam_33 {x {expr {$x + 31}}}
assert bulk-lam-33 [apply $_lam_33 10] 41

set _lam_34 {x {expr {$x + 38}}}
assert bulk-lam-34 [apply $_lam_34 10] 48

set _lam_35 {x {expr {$x + 45}}}
assert bulk-lam-35 [apply $_lam_35 10] 55

set _lam_36 {x {expr {$x + 52}}}
assert bulk-lam-36 [apply $_lam_36 10] 62

set _lam_37 {x {expr {$x + 59}}}
assert bulk-lam-37 [apply $_lam_37 10] 69

set _lam_38 {x {expr {$x + 66}}}
assert bulk-lam-38 [apply $_lam_38 10] 76

set _lam_39 {x {expr {$x + 73}}}
assert bulk-lam-39 [apply $_lam_39 10] 83

oo::class create _BulkCls0 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 1}] }
}
set _obj0 [_BulkCls0 new 10]
assert bulk-oo-0-get [$_obj0 get] 10
assert bulk-oo-0-comp [$_obj0 compute] 10
$_obj0 destroy

oo::class create _BulkCls1 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 2}] }
}
set _obj1 [_BulkCls1 new 11]
assert bulk-oo-1-get [$_obj1 get] 11
assert bulk-oo-1-comp [$_obj1 compute] 22
$_obj1 destroy

oo::class create _BulkCls2 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 3}] }
}
set _obj2 [_BulkCls2 new 12]
assert bulk-oo-2-get [$_obj2 get] 12
assert bulk-oo-2-comp [$_obj2 compute] 36
$_obj2 destroy

oo::class create _BulkCls3 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 4}] }
}
set _obj3 [_BulkCls3 new 13]
assert bulk-oo-3-get [$_obj3 get] 13
assert bulk-oo-3-comp [$_obj3 compute] 52
$_obj3 destroy

oo::class create _BulkCls4 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 5}] }
}
set _obj4 [_BulkCls4 new 14]
assert bulk-oo-4-get [$_obj4 get] 14
assert bulk-oo-4-comp [$_obj4 compute] 70
$_obj4 destroy

oo::class create _BulkCls5 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 6}] }
}
set _obj5 [_BulkCls5 new 15]
assert bulk-oo-5-get [$_obj5 get] 15
assert bulk-oo-5-comp [$_obj5 compute] 90
$_obj5 destroy

oo::class create _BulkCls6 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 7}] }
}
set _obj6 [_BulkCls6 new 16]
assert bulk-oo-6-get [$_obj6 get] 16
assert bulk-oo-6-comp [$_obj6 compute] 112
$_obj6 destroy

oo::class create _BulkCls7 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 8}] }
}
set _obj7 [_BulkCls7 new 17]
assert bulk-oo-7-get [$_obj7 get] 17
assert bulk-oo-7-comp [$_obj7 compute] 136
$_obj7 destroy

oo::class create _BulkCls8 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 9}] }
}
set _obj8 [_BulkCls8 new 18]
assert bulk-oo-8-get [$_obj8 get] 18
assert bulk-oo-8-comp [$_obj8 compute] 162
$_obj8 destroy

oo::class create _BulkCls9 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 10}] }
}
set _obj9 [_BulkCls9 new 19]
assert bulk-oo-9-get [$_obj9 get] 19
assert bulk-oo-9-comp [$_obj9 compute] 190
$_obj9 destroy

oo::class create _BulkCls10 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 11}] }
}
set _obj10 [_BulkCls10 new 20]
assert bulk-oo-10-get [$_obj10 get] 20
assert bulk-oo-10-comp [$_obj10 compute] 220
$_obj10 destroy

oo::class create _BulkCls11 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 12}] }
}
set _obj11 [_BulkCls11 new 21]
assert bulk-oo-11-get [$_obj11 get] 21
assert bulk-oo-11-comp [$_obj11 compute] 252
$_obj11 destroy

oo::class create _BulkCls12 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 13}] }
}
set _obj12 [_BulkCls12 new 22]
assert bulk-oo-12-get [$_obj12 get] 22
assert bulk-oo-12-comp [$_obj12 compute] 286
$_obj12 destroy

oo::class create _BulkCls13 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 14}] }
}
set _obj13 [_BulkCls13 new 23]
assert bulk-oo-13-get [$_obj13 get] 23
assert bulk-oo-13-comp [$_obj13 compute] 322
$_obj13 destroy

oo::class create _BulkCls14 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 15}] }
}
set _obj14 [_BulkCls14 new 24]
assert bulk-oo-14-get [$_obj14 get] 24
assert bulk-oo-14-comp [$_obj14 compute] 360
$_obj14 destroy

oo::class create _BulkCls15 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 16}] }
}
set _obj15 [_BulkCls15 new 25]
assert bulk-oo-15-get [$_obj15 get] 25
assert bulk-oo-15-comp [$_obj15 compute] 400
$_obj15 destroy

oo::class create _BulkCls16 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 17}] }
}
set _obj16 [_BulkCls16 new 26]
assert bulk-oo-16-get [$_obj16 get] 26
assert bulk-oo-16-comp [$_obj16 compute] 442
$_obj16 destroy

oo::class create _BulkCls17 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 18}] }
}
set _obj17 [_BulkCls17 new 27]
assert bulk-oo-17-get [$_obj17 get] 27
assert bulk-oo-17-comp [$_obj17 compute] 486
$_obj17 destroy

oo::class create _BulkCls18 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 19}] }
}
set _obj18 [_BulkCls18 new 28]
assert bulk-oo-18-get [$_obj18 get] 28
assert bulk-oo-18-comp [$_obj18 compute] 532
$_obj18 destroy

oo::class create _BulkCls19 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 20}] }
}
set _obj19 [_BulkCls19 new 29]
assert bulk-oo-19-get [$_obj19 get] 29
assert bulk-oo-19-comp [$_obj19 compute] 580
$_obj19 destroy

oo::class create _BulkCls20 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 21}] }
}
set _obj20 [_BulkCls20 new 30]
assert bulk-oo-20-get [$_obj20 get] 30
assert bulk-oo-20-comp [$_obj20 compute] 630
$_obj20 destroy

oo::class create _BulkCls21 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 22}] }
}
set _obj21 [_BulkCls21 new 31]
assert bulk-oo-21-get [$_obj21 get] 31
assert bulk-oo-21-comp [$_obj21 compute] 682
$_obj21 destroy

oo::class create _BulkCls22 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 23}] }
}
set _obj22 [_BulkCls22 new 32]
assert bulk-oo-22-get [$_obj22 get] 32
assert bulk-oo-22-comp [$_obj22 compute] 736
$_obj22 destroy

oo::class create _BulkCls23 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 24}] }
}
set _obj23 [_BulkCls23 new 33]
assert bulk-oo-23-get [$_obj23 get] 33
assert bulk-oo-23-comp [$_obj23 compute] 792
$_obj23 destroy

oo::class create _BulkCls24 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 25}] }
}
set _obj24 [_BulkCls24 new 34]
assert bulk-oo-24-get [$_obj24 get] 34
assert bulk-oo-24-comp [$_obj24 compute] 850
$_obj24 destroy

oo::class create _BulkCls25 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 26}] }
}
set _obj25 [_BulkCls25 new 35]
assert bulk-oo-25-get [$_obj25 get] 35
assert bulk-oo-25-comp [$_obj25 compute] 910
$_obj25 destroy

oo::class create _BulkCls26 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 27}] }
}
set _obj26 [_BulkCls26 new 36]
assert bulk-oo-26-get [$_obj26 get] 36
assert bulk-oo-26-comp [$_obj26 compute] 972
$_obj26 destroy

oo::class create _BulkCls27 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 28}] }
}
set _obj27 [_BulkCls27 new 37]
assert bulk-oo-27-get [$_obj27 get] 37
assert bulk-oo-27-comp [$_obj27 compute] 1036
$_obj27 destroy

oo::class create _BulkCls28 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 29}] }
}
set _obj28 [_BulkCls28 new 38]
assert bulk-oo-28-get [$_obj28 get] 38
assert bulk-oo-28-comp [$_obj28 compute] 1102
$_obj28 destroy

oo::class create _BulkCls29 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 30}] }
}
set _obj29 [_BulkCls29 new 39]
assert bulk-oo-29-get [$_obj29 get] 39
assert bulk-oo-29-comp [$_obj29 compute] 1170
$_obj29 destroy

oo::class create _BulkCls30 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 31}] }
}
set _obj30 [_BulkCls30 new 40]
assert bulk-oo-30-get [$_obj30 get] 40
assert bulk-oo-30-comp [$_obj30 compute] 1240
$_obj30 destroy

oo::class create _BulkCls31 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 32}] }
}
set _obj31 [_BulkCls31 new 41]
assert bulk-oo-31-get [$_obj31 get] 41
assert bulk-oo-31-comp [$_obj31 compute] 1312
$_obj31 destroy

oo::class create _BulkCls32 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 33}] }
}
set _obj32 [_BulkCls32 new 42]
assert bulk-oo-32-get [$_obj32 get] 42
assert bulk-oo-32-comp [$_obj32 compute] 1386
$_obj32 destroy

oo::class create _BulkCls33 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 34}] }
}
set _obj33 [_BulkCls33 new 43]
assert bulk-oo-33-get [$_obj33 get] 43
assert bulk-oo-33-comp [$_obj33 compute] 1462
$_obj33 destroy

oo::class create _BulkCls34 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 35}] }
}
set _obj34 [_BulkCls34 new 44]
assert bulk-oo-34-get [$_obj34 get] 44
assert bulk-oo-34-comp [$_obj34 compute] 1540
$_obj34 destroy

oo::class create _BulkCls35 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 36}] }
}
set _obj35 [_BulkCls35 new 45]
assert bulk-oo-35-get [$_obj35 get] 45
assert bulk-oo-35-comp [$_obj35 compute] 1620
$_obj35 destroy

oo::class create _BulkCls36 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 37}] }
}
set _obj36 [_BulkCls36 new 46]
assert bulk-oo-36-get [$_obj36 get] 46
assert bulk-oo-36-comp [$_obj36 compute] 1702
$_obj36 destroy

oo::class create _BulkCls37 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 38}] }
}
set _obj37 [_BulkCls37 new 47]
assert bulk-oo-37-get [$_obj37 get] 47
assert bulk-oo-37-comp [$_obj37 compute] 1786
$_obj37 destroy

oo::class create _BulkCls38 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 39}] }
}
set _obj38 [_BulkCls38 new 48]
assert bulk-oo-38-get [$_obj38 get] 48
assert bulk-oo-38-comp [$_obj38 compute] 1872
$_obj38 destroy

oo::class create _BulkCls39 {
    constructor {v} { my variable v_; set v_ $v }
    method get {} { my variable v_; return $v_ }
    method compute {} { my variable v_; return [expr {$v_ * 40}] }
}
set _obj39 [_BulkCls39 new 49]
assert bulk-oo-39-get [$_obj39 get] 49
assert bulk-oo-39-comp [$_obj39 compute] 1960
$_obj39 destroy

namespace eval ::bulkns0 {
    proc calc {x} { return [expr {$x + 0}] }
}
assert bulk-ns-0 [::bulkns0::calc 100] 100

namespace eval ::bulkns1 {
    proc calc {x} { return [expr {$x + 3}] }
}
assert bulk-ns-1 [::bulkns1::calc 100] 103

namespace eval ::bulkns2 {
    proc calc {x} { return [expr {$x + 6}] }
}
assert bulk-ns-2 [::bulkns2::calc 100] 106

namespace eval ::bulkns3 {
    proc calc {x} { return [expr {$x + 9}] }
}
assert bulk-ns-3 [::bulkns3::calc 100] 109

namespace eval ::bulkns4 {
    proc calc {x} { return [expr {$x + 12}] }
}
assert bulk-ns-4 [::bulkns4::calc 100] 112

namespace eval ::bulkns5 {
    proc calc {x} { return [expr {$x + 15}] }
}
assert bulk-ns-5 [::bulkns5::calc 100] 115

namespace eval ::bulkns6 {
    proc calc {x} { return [expr {$x + 18}] }
}
assert bulk-ns-6 [::bulkns6::calc 100] 118

namespace eval ::bulkns7 {
    proc calc {x} { return [expr {$x + 21}] }
}
assert bulk-ns-7 [::bulkns7::calc 100] 121

namespace eval ::bulkns8 {
    proc calc {x} { return [expr {$x + 24}] }
}
assert bulk-ns-8 [::bulkns8::calc 100] 124

namespace eval ::bulkns9 {
    proc calc {x} { return [expr {$x + 27}] }
}
assert bulk-ns-9 [::bulkns9::calc 100] 127

namespace eval ::bulkns10 {
    proc calc {x} { return [expr {$x + 30}] }
}
assert bulk-ns-10 [::bulkns10::calc 100] 130

namespace eval ::bulkns11 {
    proc calc {x} { return [expr {$x + 33}] }
}
assert bulk-ns-11 [::bulkns11::calc 100] 133

namespace eval ::bulkns12 {
    proc calc {x} { return [expr {$x + 36}] }
}
assert bulk-ns-12 [::bulkns12::calc 100] 136

namespace eval ::bulkns13 {
    proc calc {x} { return [expr {$x + 39}] }
}
assert bulk-ns-13 [::bulkns13::calc 100] 139

namespace eval ::bulkns14 {
    proc calc {x} { return [expr {$x + 42}] }
}
assert bulk-ns-14 [::bulkns14::calc 100] 142

namespace eval ::bulkns15 {
    proc calc {x} { return [expr {$x + 45}] }
}
assert bulk-ns-15 [::bulkns15::calc 100] 145

namespace eval ::bulkns16 {
    proc calc {x} { return [expr {$x + 48}] }
}
assert bulk-ns-16 [::bulkns16::calc 100] 148

namespace eval ::bulkns17 {
    proc calc {x} { return [expr {$x + 51}] }
}
assert bulk-ns-17 [::bulkns17::calc 100] 151

namespace eval ::bulkns18 {
    proc calc {x} { return [expr {$x + 54}] }
}
assert bulk-ns-18 [::bulkns18::calc 100] 154

namespace eval ::bulkns19 {
    proc calc {x} { return [expr {$x + 57}] }
}
assert bulk-ns-19 [::bulkns19::calc 100] 157

namespace eval ::bulkns20 {
    proc calc {x} { return [expr {$x + 60}] }
}
assert bulk-ns-20 [::bulkns20::calc 100] 160

namespace eval ::bulkns21 {
    proc calc {x} { return [expr {$x + 63}] }
}
assert bulk-ns-21 [::bulkns21::calc 100] 163

namespace eval ::bulkns22 {
    proc calc {x} { return [expr {$x + 66}] }
}
assert bulk-ns-22 [::bulkns22::calc 100] 166

namespace eval ::bulkns23 {
    proc calc {x} { return [expr {$x + 69}] }
}
assert bulk-ns-23 [::bulkns23::calc 100] 169

namespace eval ::bulkns24 {
    proc calc {x} { return [expr {$x + 72}] }
}
assert bulk-ns-24 [::bulkns24::calc 100] 172

namespace eval ::bulkns25 {
    proc calc {x} { return [expr {$x + 75}] }
}
assert bulk-ns-25 [::bulkns25::calc 100] 175

namespace eval ::bulkns26 {
    proc calc {x} { return [expr {$x + 78}] }
}
assert bulk-ns-26 [::bulkns26::calc 100] 178

namespace eval ::bulkns27 {
    proc calc {x} { return [expr {$x + 81}] }
}
assert bulk-ns-27 [::bulkns27::calc 100] 181

namespace eval ::bulkns28 {
    proc calc {x} { return [expr {$x + 84}] }
}
assert bulk-ns-28 [::bulkns28::calc 100] 184

namespace eval ::bulkns29 {
    proc calc {x} { return [expr {$x + 87}] }
}
assert bulk-ns-29 [::bulkns29::calc 100] 187

namespace eval ::bulkns30 {
    proc calc {x} { return [expr {$x + 90}] }
}
assert bulk-ns-30 [::bulkns30::calc 100] 190

namespace eval ::bulkns31 {
    proc calc {x} { return [expr {$x + 93}] }
}
assert bulk-ns-31 [::bulkns31::calc 100] 193

namespace eval ::bulkns32 {
    proc calc {x} { return [expr {$x + 96}] }
}
assert bulk-ns-32 [::bulkns32::calc 100] 196

namespace eval ::bulkns33 {
    proc calc {x} { return [expr {$x + 99}] }
}
assert bulk-ns-33 [::bulkns33::calc 100] 199

namespace eval ::bulkns34 {
    proc calc {x} { return [expr {$x + 102}] }
}
assert bulk-ns-34 [::bulkns34::calc 100] 202

namespace eval ::bulkns35 {
    proc calc {x} { return [expr {$x + 105}] }
}
assert bulk-ns-35 [::bulkns35::calc 100] 205

namespace eval ::bulkns36 {
    proc calc {x} { return [expr {$x + 108}] }
}
assert bulk-ns-36 [::bulkns36::calc 100] 208

namespace eval ::bulkns37 {
    proc calc {x} { return [expr {$x + 111}] }
}
assert bulk-ns-37 [::bulkns37::calc 100] 211

namespace eval ::bulkns38 {
    proc calc {x} { return [expr {$x + 114}] }
}
assert bulk-ns-38 [::bulkns38::calc 100] 214

namespace eval ::bulkns39 {
    proc calc {x} { return [expr {$x + 117}] }
}
assert bulk-ns-39 [::bulkns39::calc 100] 217

proc _wh_0 {} {
    set s 0; set j 0
    while {$j < 5} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-0 [_wh_0] 10

proc _wh_1 {} {
    set s 0; set j 0
    while {$j < 6} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-1 [_wh_1] 15

proc _wh_2 {} {
    set s 0; set j 0
    while {$j < 7} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-2 [_wh_2] 21

proc _wh_3 {} {
    set s 0; set j 0
    while {$j < 8} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-3 [_wh_3] 28

proc _wh_4 {} {
    set s 0; set j 0
    while {$j < 9} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-4 [_wh_4] 36

proc _wh_5 {} {
    set s 0; set j 0
    while {$j < 10} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-5 [_wh_5] 45

proc _wh_6 {} {
    set s 0; set j 0
    while {$j < 11} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-6 [_wh_6] 55

proc _wh_7 {} {
    set s 0; set j 0
    while {$j < 12} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-7 [_wh_7] 66

proc _wh_8 {} {
    set s 0; set j 0
    while {$j < 13} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-8 [_wh_8] 78

proc _wh_9 {} {
    set s 0; set j 0
    while {$j < 14} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-9 [_wh_9] 91

proc _wh_10 {} {
    set s 0; set j 0
    while {$j < 5} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-10 [_wh_10] 10

proc _wh_11 {} {
    set s 0; set j 0
    while {$j < 6} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-11 [_wh_11] 15

proc _wh_12 {} {
    set s 0; set j 0
    while {$j < 7} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-12 [_wh_12] 21

proc _wh_13 {} {
    set s 0; set j 0
    while {$j < 8} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-13 [_wh_13] 28

proc _wh_14 {} {
    set s 0; set j 0
    while {$j < 9} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-14 [_wh_14] 36

proc _wh_15 {} {
    set s 0; set j 0
    while {$j < 10} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-15 [_wh_15] 45

proc _wh_16 {} {
    set s 0; set j 0
    while {$j < 11} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-16 [_wh_16] 55

proc _wh_17 {} {
    set s 0; set j 0
    while {$j < 12} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-17 [_wh_17] 66

proc _wh_18 {} {
    set s 0; set j 0
    while {$j < 13} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-18 [_wh_18] 78

proc _wh_19 {} {
    set s 0; set j 0
    while {$j < 14} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-19 [_wh_19] 91

proc _wh_20 {} {
    set s 0; set j 0
    while {$j < 5} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-20 [_wh_20] 10

proc _wh_21 {} {
    set s 0; set j 0
    while {$j < 6} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-21 [_wh_21] 15

proc _wh_22 {} {
    set s 0; set j 0
    while {$j < 7} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-22 [_wh_22] 21

proc _wh_23 {} {
    set s 0; set j 0
    while {$j < 8} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-23 [_wh_23] 28

proc _wh_24 {} {
    set s 0; set j 0
    while {$j < 9} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-24 [_wh_24] 36

proc _wh_25 {} {
    set s 0; set j 0
    while {$j < 10} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-25 [_wh_25] 45

proc _wh_26 {} {
    set s 0; set j 0
    while {$j < 11} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-26 [_wh_26] 55

proc _wh_27 {} {
    set s 0; set j 0
    while {$j < 12} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-27 [_wh_27] 66

proc _wh_28 {} {
    set s 0; set j 0
    while {$j < 13} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-28 [_wh_28] 78

proc _wh_29 {} {
    set s 0; set j 0
    while {$j < 14} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-29 [_wh_29] 91

proc _wh_30 {} {
    set s 0; set j 0
    while {$j < 5} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-30 [_wh_30] 10

proc _wh_31 {} {
    set s 0; set j 0
    while {$j < 6} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-31 [_wh_31] 15

proc _wh_32 {} {
    set s 0; set j 0
    while {$j < 7} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-32 [_wh_32] 21

proc _wh_33 {} {
    set s 0; set j 0
    while {$j < 8} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-33 [_wh_33] 28

proc _wh_34 {} {
    set s 0; set j 0
    while {$j < 9} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-34 [_wh_34] 36

proc _wh_35 {} {
    set s 0; set j 0
    while {$j < 10} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-35 [_wh_35] 45

proc _wh_36 {} {
    set s 0; set j 0
    while {$j < 11} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-36 [_wh_36] 55

proc _wh_37 {} {
    set s 0; set j 0
    while {$j < 12} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-37 [_wh_37] 66

proc _wh_38 {} {
    set s 0; set j 0
    while {$j < 13} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-38 [_wh_38] 78

proc _wh_39 {} {
    set s 0; set j 0
    while {$j < 14} { set s [expr {$s + $j}]; incr j }
    return $s
}
assert bulk-wh-39 [_wh_39] 91

proc _du_0 {} {
    set d [dict create x 0 y 0]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-0 [_du_0] "100:0"

proc _du_1 {} {
    set d [dict create x 1 y 2]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-1 [_du_1] "101:6"

proc _du_2 {} {
    set d [dict create x 2 y 4]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-2 [_du_2] "102:12"

proc _du_3 {} {
    set d [dict create x 3 y 6]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-3 [_du_3] "103:18"

proc _du_4 {} {
    set d [dict create x 4 y 8]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-4 [_du_4] "104:24"

proc _du_5 {} {
    set d [dict create x 5 y 10]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-5 [_du_5] "105:30"

proc _du_6 {} {
    set d [dict create x 6 y 12]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-6 [_du_6] "106:36"

proc _du_7 {} {
    set d [dict create x 7 y 14]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-7 [_du_7] "107:42"

proc _du_8 {} {
    set d [dict create x 8 y 16]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-8 [_du_8] "108:48"

proc _du_9 {} {
    set d [dict create x 9 y 18]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-9 [_du_9] "109:54"

proc _du_10 {} {
    set d [dict create x 10 y 20]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-10 [_du_10] "110:60"

proc _du_11 {} {
    set d [dict create x 11 y 22]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-11 [_du_11] "111:66"

proc _du_12 {} {
    set d [dict create x 12 y 24]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-12 [_du_12] "112:72"

proc _du_13 {} {
    set d [dict create x 13 y 26]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-13 [_du_13] "113:78"

proc _du_14 {} {
    set d [dict create x 14 y 28]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-14 [_du_14] "114:84"

proc _du_15 {} {
    set d [dict create x 15 y 30]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-15 [_du_15] "115:90"

proc _du_16 {} {
    set d [dict create x 16 y 32]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-16 [_du_16] "116:96"

proc _du_17 {} {
    set d [dict create x 17 y 34]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-17 [_du_17] "117:102"

proc _du_18 {} {
    set d [dict create x 18 y 36]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-18 [_du_18] "118:108"

proc _du_19 {} {
    set d [dict create x 19 y 38]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-19 [_du_19] "119:114"

proc _du_20 {} {
    set d [dict create x 20 y 40]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-20 [_du_20] "120:120"

proc _du_21 {} {
    set d [dict create x 21 y 42]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-21 [_du_21] "121:126"

proc _du_22 {} {
    set d [dict create x 22 y 44]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-22 [_du_22] "122:132"

proc _du_23 {} {
    set d [dict create x 23 y 46]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-23 [_du_23] "123:138"

proc _du_24 {} {
    set d [dict create x 24 y 48]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-24 [_du_24] "124:144"

proc _du_25 {} {
    set d [dict create x 25 y 50]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-25 [_du_25] "125:150"

proc _du_26 {} {
    set d [dict create x 26 y 52]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-26 [_du_26] "126:156"

proc _du_27 {} {
    set d [dict create x 27 y 54]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-27 [_du_27] "127:162"

proc _du_28 {} {
    set d [dict create x 28 y 56]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-28 [_du_28] "128:168"

proc _du_29 {} {
    set d [dict create x 29 y 58]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-29 [_du_29] "129:174"

proc _du_30 {} {
    set d [dict create x 30 y 60]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-30 [_du_30] "130:180"

proc _du_31 {} {
    set d [dict create x 31 y 62]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-31 [_du_31] "131:186"

proc _du_32 {} {
    set d [dict create x 32 y 64]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-32 [_du_32] "132:192"

proc _du_33 {} {
    set d [dict create x 33 y 66]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-33 [_du_33] "133:198"

proc _du_34 {} {
    set d [dict create x 34 y 68]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-34 [_du_34] "134:204"

proc _du_35 {} {
    set d [dict create x 35 y 70]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-35 [_du_35] "135:210"

proc _du_36 {} {
    set d [dict create x 36 y 72]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-36 [_du_36] "136:216"

proc _du_37 {} {
    set d [dict create x 37 y 74]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-37 [_du_37] "137:222"

proc _du_38 {} {
    set d [dict create x 38 y 76]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-38 [_du_38] "138:228"

proc _du_39 {} {
    set d [dict create x 39 y 78]
    dict update d x xv y yv {
        set xv [expr {$xv + 100}]
        set yv [expr {$yv * 3}]
    }
    return "[dict get $d x]:[dict get $d y]"
}
assert bulk-du-39 [_du_39] "139:234"


# ========================================================================
# Padding block to reach target file size
# ========================================================================
assert pad-str-0 [string length "The quick brown fox #00000 jumped over the lazy dog"] 51
assert pad-int-1 [expr {48 * 2}] 96
assert pad-list-2 [llength {a b c d e f g h i j}] 10
assert pad-dict-3 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-4 [expr {(4 + 1) * (4 + 2) / 2}] 15
assert pad-str-5 [string length "The quick brown fox #00005 jumped over the lazy dog"] 51
assert pad-int-6 [expr {203 * 2}] 406
assert pad-list-7 [llength {a b c d e f g h i j}] 10
assert pad-dict-8 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-9 [expr {(9 + 1) * (9 + 2) / 2}] 55
assert pad-str-10 [string length "The quick brown fox #00010 jumped over the lazy dog"] 51
assert pad-int-11 [expr {358 * 2}] 716
assert pad-list-12 [llength {a b c d e f g h i j}] 10
assert pad-dict-13 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-14 [expr {(14 + 1) * (14 + 2) / 2}] 120
assert pad-str-15 [string length "The quick brown fox #00015 jumped over the lazy dog"] 51
assert pad-int-16 [expr {513 * 2}] 1026
assert pad-list-17 [llength {a b c d e f g h i j}] 10
assert pad-dict-18 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-19 [expr {(19 + 1) * (19 + 2) / 2}] 210
assert pad-str-20 [string length "The quick brown fox #00020 jumped over the lazy dog"] 51
assert pad-int-21 [expr {668 * 2}] 1336
assert pad-list-22 [llength {a b c d e f g h i j}] 10
assert pad-dict-23 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-24 [expr {(24 + 1) * (24 + 2) / 2}] 325
assert pad-str-25 [string length "The quick brown fox #00025 jumped over the lazy dog"] 51
assert pad-int-26 [expr {823 * 2}] 1646
assert pad-list-27 [llength {a b c d e f g h i j}] 10
assert pad-dict-28 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-29 [expr {(29 + 1) * (29 + 2) / 2}] 465
assert pad-str-30 [string length "The quick brown fox #00030 jumped over the lazy dog"] 51
assert pad-int-31 [expr {978 * 2}] 1956
assert pad-list-32 [llength {a b c d e f g h i j}] 10
assert pad-dict-33 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-34 [expr {(34 + 1) * (34 + 2) / 2}] 630
assert pad-str-35 [string length "The quick brown fox #00035 jumped over the lazy dog"] 51
assert pad-int-36 [expr {1133 * 2}] 2266
assert pad-list-37 [llength {a b c d e f g h i j}] 10
assert pad-dict-38 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-39 [expr {(39 + 1) * (39 + 2) / 2}] 820
assert pad-str-40 [string length "The quick brown fox #00040 jumped over the lazy dog"] 51
assert pad-int-41 [expr {1288 * 2}] 2576
assert pad-list-42 [llength {a b c d e f g h i j}] 10
assert pad-dict-43 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-44 [expr {(44 + 1) * (44 + 2) / 2}] 1035
assert pad-str-45 [string length "The quick brown fox #00045 jumped over the lazy dog"] 51
assert pad-int-46 [expr {1443 * 2}] 2886
assert pad-list-47 [llength {a b c d e f g h i j}] 10
assert pad-dict-48 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-49 [expr {(49 + 1) * (49 + 2) / 2}] 1275
assert pad-str-50 [string length "The quick brown fox #00050 jumped over the lazy dog"] 51
assert pad-int-51 [expr {1598 * 2}] 3196
assert pad-list-52 [llength {a b c d e f g h i j}] 10
assert pad-dict-53 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-54 [expr {(54 + 1) * (54 + 2) / 2}] 1540
assert pad-str-55 [string length "The quick brown fox #00055 jumped over the lazy dog"] 51
assert pad-int-56 [expr {1753 * 2}] 3506
assert pad-list-57 [llength {a b c d e f g h i j}] 10
assert pad-dict-58 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-59 [expr {(59 + 1) * (59 + 2) / 2}] 1830
assert pad-str-60 [string length "The quick brown fox #00060 jumped over the lazy dog"] 51
assert pad-int-61 [expr {1908 * 2}] 3816
assert pad-list-62 [llength {a b c d e f g h i j}] 10
assert pad-dict-63 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-64 [expr {(64 + 1) * (64 + 2) / 2}] 2145
assert pad-str-65 [string length "The quick brown fox #00065 jumped over the lazy dog"] 51
assert pad-int-66 [expr {2063 * 2}] 4126
assert pad-list-67 [llength {a b c d e f g h i j}] 10
assert pad-dict-68 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-69 [expr {(69 + 1) * (69 + 2) / 2}] 2485
assert pad-str-70 [string length "The quick brown fox #00070 jumped over the lazy dog"] 51
assert pad-int-71 [expr {2218 * 2}] 4436
assert pad-list-72 [llength {a b c d e f g h i j}] 10
assert pad-dict-73 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-74 [expr {(74 + 1) * (74 + 2) / 2}] 2850
assert pad-str-75 [string length "The quick brown fox #00075 jumped over the lazy dog"] 51
assert pad-int-76 [expr {2373 * 2}] 4746
assert pad-list-77 [llength {a b c d e f g h i j}] 10
assert pad-dict-78 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-79 [expr {(79 + 1) * (79 + 2) / 2}] 3240
assert pad-str-80 [string length "The quick brown fox #00080 jumped over the lazy dog"] 51
assert pad-int-81 [expr {2528 * 2}] 5056
assert pad-list-82 [llength {a b c d e f g h i j}] 10
assert pad-dict-83 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-84 [expr {(84 + 1) * (84 + 2) / 2}] 3655
assert pad-str-85 [string length "The quick brown fox #00085 jumped over the lazy dog"] 51
assert pad-int-86 [expr {2683 * 2}] 5366
assert pad-list-87 [llength {a b c d e f g h i j}] 10
assert pad-dict-88 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-89 [expr {(89 + 1) * (89 + 2) / 2}] 4095
assert pad-str-90 [string length "The quick brown fox #00090 jumped over the lazy dog"] 51
assert pad-int-91 [expr {2838 * 2}] 5676
assert pad-list-92 [llength {a b c d e f g h i j}] 10
assert pad-dict-93 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-94 [expr {(94 + 1) * (94 + 2) / 2}] 4560
assert pad-str-95 [string length "The quick brown fox #00095 jumped over the lazy dog"] 51
assert pad-int-96 [expr {2993 * 2}] 5986
assert pad-list-97 [llength {a b c d e f g h i j}] 10
assert pad-dict-98 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-99 [expr {(99 + 1) * (99 + 2) / 2}] 5050
assert pad-str-100 [string length "The quick brown fox #00100 jumped over the lazy dog"] 51
assert pad-int-101 [expr {3148 * 2}] 6296
assert pad-list-102 [llength {a b c d e f g h i j}] 10
assert pad-dict-103 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-104 [expr {(104 + 1) * (104 + 2) / 2}] 5565
assert pad-str-105 [string length "The quick brown fox #00105 jumped over the lazy dog"] 51
assert pad-int-106 [expr {3303 * 2}] 6606
assert pad-list-107 [llength {a b c d e f g h i j}] 10
assert pad-dict-108 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-109 [expr {(109 + 1) * (109 + 2) / 2}] 6105
assert pad-str-110 [string length "The quick brown fox #00110 jumped over the lazy dog"] 51
assert pad-int-111 [expr {3458 * 2}] 6916
assert pad-list-112 [llength {a b c d e f g h i j}] 10
assert pad-dict-113 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-114 [expr {(114 + 1) * (114 + 2) / 2}] 6670
assert pad-str-115 [string length "The quick brown fox #00115 jumped over the lazy dog"] 51
assert pad-int-116 [expr {3613 * 2}] 7226
assert pad-list-117 [llength {a b c d e f g h i j}] 10
assert pad-dict-118 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-119 [expr {(119 + 1) * (119 + 2) / 2}] 7260
assert pad-str-120 [string length "The quick brown fox #00120 jumped over the lazy dog"] 51
assert pad-int-121 [expr {3768 * 2}] 7536
assert pad-list-122 [llength {a b c d e f g h i j}] 10
assert pad-dict-123 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-124 [expr {(124 + 1) * (124 + 2) / 2}] 7875
assert pad-str-125 [string length "The quick brown fox #00125 jumped over the lazy dog"] 51
assert pad-int-126 [expr {3923 * 2}] 7846
assert pad-list-127 [llength {a b c d e f g h i j}] 10
assert pad-dict-128 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-129 [expr {(129 + 1) * (129 + 2) / 2}] 8515
assert pad-str-130 [string length "The quick brown fox #00130 jumped over the lazy dog"] 51
assert pad-int-131 [expr {4078 * 2}] 8156
assert pad-list-132 [llength {a b c d e f g h i j}] 10
assert pad-dict-133 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-134 [expr {(134 + 1) * (134 + 2) / 2}] 9180
assert pad-str-135 [string length "The quick brown fox #00135 jumped over the lazy dog"] 51
assert pad-int-136 [expr {4233 * 2}] 8466
assert pad-list-137 [llength {a b c d e f g h i j}] 10
assert pad-dict-138 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-139 [expr {(139 + 1) * (139 + 2) / 2}] 9870
assert pad-str-140 [string length "The quick brown fox #00140 jumped over the lazy dog"] 51
assert pad-int-141 [expr {4388 * 2}] 8776
assert pad-list-142 [llength {a b c d e f g h i j}] 10
assert pad-dict-143 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-144 [expr {(144 + 1) * (144 + 2) / 2}] 10585
assert pad-str-145 [string length "The quick brown fox #00145 jumped over the lazy dog"] 51
assert pad-int-146 [expr {4543 * 2}] 9086
assert pad-list-147 [llength {a b c d e f g h i j}] 10
assert pad-dict-148 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-149 [expr {(149 + 1) * (149 + 2) / 2}] 11325
assert pad-str-150 [string length "The quick brown fox #00150 jumped over the lazy dog"] 51
assert pad-int-151 [expr {4698 * 2}] 9396
assert pad-list-152 [llength {a b c d e f g h i j}] 10
assert pad-dict-153 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-154 [expr {(154 + 1) * (154 + 2) / 2}] 12090
assert pad-str-155 [string length "The quick brown fox #00155 jumped over the lazy dog"] 51
assert pad-int-156 [expr {4853 * 2}] 9706
assert pad-list-157 [llength {a b c d e f g h i j}] 10
assert pad-dict-158 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-159 [expr {(159 + 1) * (159 + 2) / 2}] 12880
assert pad-str-160 [string length "The quick brown fox #00160 jumped over the lazy dog"] 51
assert pad-int-161 [expr {5008 * 2}] 10016
assert pad-list-162 [llength {a b c d e f g h i j}] 10
assert pad-dict-163 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-164 [expr {(164 + 1) * (164 + 2) / 2}] 13695
assert pad-str-165 [string length "The quick brown fox #00165 jumped over the lazy dog"] 51
assert pad-int-166 [expr {5163 * 2}] 10326
assert pad-list-167 [llength {a b c d e f g h i j}] 10
assert pad-dict-168 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-169 [expr {(169 + 1) * (169 + 2) / 2}] 14535
assert pad-str-170 [string length "The quick brown fox #00170 jumped over the lazy dog"] 51
assert pad-int-171 [expr {5318 * 2}] 10636
assert pad-list-172 [llength {a b c d e f g h i j}] 10
assert pad-dict-173 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-174 [expr {(174 + 1) * (174 + 2) / 2}] 15400
assert pad-str-175 [string length "The quick brown fox #00175 jumped over the lazy dog"] 51
assert pad-int-176 [expr {5473 * 2}] 10946
assert pad-list-177 [llength {a b c d e f g h i j}] 10
assert pad-dict-178 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-179 [expr {(179 + 1) * (179 + 2) / 2}] 16290
assert pad-str-180 [string length "The quick brown fox #00180 jumped over the lazy dog"] 51
assert pad-int-181 [expr {5628 * 2}] 11256
assert pad-list-182 [llength {a b c d e f g h i j}] 10
assert pad-dict-183 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-184 [expr {(184 + 1) * (184 + 2) / 2}] 17205
assert pad-str-185 [string length "The quick brown fox #00185 jumped over the lazy dog"] 51
assert pad-int-186 [expr {5783 * 2}] 11566
assert pad-list-187 [llength {a b c d e f g h i j}] 10
assert pad-dict-188 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-189 [expr {(189 + 1) * (189 + 2) / 2}] 18145
assert pad-str-190 [string length "The quick brown fox #00190 jumped over the lazy dog"] 51
assert pad-int-191 [expr {5938 * 2}] 11876
assert pad-list-192 [llength {a b c d e f g h i j}] 10
assert pad-dict-193 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-194 [expr {(194 + 1) * (194 + 2) / 2}] 19110
assert pad-str-195 [string length "The quick brown fox #00195 jumped over the lazy dog"] 51
assert pad-int-196 [expr {6093 * 2}] 12186
assert pad-list-197 [llength {a b c d e f g h i j}] 10
assert pad-dict-198 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-199 [expr {(199 + 1) * (199 + 2) / 2}] 20100
assert pad-str-200 [string length "The quick brown fox #00200 jumped over the lazy dog"] 51
assert pad-int-201 [expr {6248 * 2}] 12496
assert pad-list-202 [llength {a b c d e f g h i j}] 10
assert pad-dict-203 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-204 [expr {(204 + 1) * (204 + 2) / 2}] 21115
assert pad-str-205 [string length "The quick brown fox #00205 jumped over the lazy dog"] 51
assert pad-int-206 [expr {6403 * 2}] 12806
assert pad-list-207 [llength {a b c d e f g h i j}] 10
assert pad-dict-208 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-209 [expr {(209 + 1) * (209 + 2) / 2}] 22155
assert pad-str-210 [string length "The quick brown fox #00210 jumped over the lazy dog"] 51
assert pad-int-211 [expr {6558 * 2}] 13116
assert pad-list-212 [llength {a b c d e f g h i j}] 10
assert pad-dict-213 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-214 [expr {(214 + 1) * (214 + 2) / 2}] 23220
assert pad-str-215 [string length "The quick brown fox #00215 jumped over the lazy dog"] 51
assert pad-int-216 [expr {6713 * 2}] 13426
assert pad-list-217 [llength {a b c d e f g h i j}] 10
assert pad-dict-218 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-219 [expr {(219 + 1) * (219 + 2) / 2}] 24310
assert pad-str-220 [string length "The quick brown fox #00220 jumped over the lazy dog"] 51
assert pad-int-221 [expr {6868 * 2}] 13736
assert pad-list-222 [llength {a b c d e f g h i j}] 10
assert pad-dict-223 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-224 [expr {(224 + 1) * (224 + 2) / 2}] 25425
assert pad-str-225 [string length "The quick brown fox #00225 jumped over the lazy dog"] 51
assert pad-int-226 [expr {7023 * 2}] 14046
assert pad-list-227 [llength {a b c d e f g h i j}] 10
assert pad-dict-228 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-229 [expr {(229 + 1) * (229 + 2) / 2}] 26565
assert pad-str-230 [string length "The quick brown fox #00230 jumped over the lazy dog"] 51
assert pad-int-231 [expr {7178 * 2}] 14356
assert pad-list-232 [llength {a b c d e f g h i j}] 10
assert pad-dict-233 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-234 [expr {(234 + 1) * (234 + 2) / 2}] 27730
assert pad-str-235 [string length "The quick brown fox #00235 jumped over the lazy dog"] 51
assert pad-int-236 [expr {7333 * 2}] 14666
assert pad-list-237 [llength {a b c d e f g h i j}] 10
assert pad-dict-238 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-239 [expr {(239 + 1) * (239 + 2) / 2}] 28920
assert pad-str-240 [string length "The quick brown fox #00240 jumped over the lazy dog"] 51
assert pad-int-241 [expr {7488 * 2}] 14976
assert pad-list-242 [llength {a b c d e f g h i j}] 10
assert pad-dict-243 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-244 [expr {(244 + 1) * (244 + 2) / 2}] 30135
assert pad-str-245 [string length "The quick brown fox #00245 jumped over the lazy dog"] 51
assert pad-int-246 [expr {7643 * 2}] 15286
assert pad-list-247 [llength {a b c d e f g h i j}] 10
assert pad-dict-248 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-249 [expr {(249 + 1) * (249 + 2) / 2}] 31375
assert pad-str-250 [string length "The quick brown fox #00250 jumped over the lazy dog"] 51
assert pad-int-251 [expr {7798 * 2}] 15596
assert pad-list-252 [llength {a b c d e f g h i j}] 10
assert pad-dict-253 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-254 [expr {(254 + 1) * (254 + 2) / 2}] 32640
assert pad-str-255 [string length "The quick brown fox #00255 jumped over the lazy dog"] 51
assert pad-int-256 [expr {7953 * 2}] 15906
assert pad-list-257 [llength {a b c d e f g h i j}] 10
assert pad-dict-258 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-259 [expr {(259 + 1) * (259 + 2) / 2}] 33930
assert pad-str-260 [string length "The quick brown fox #00260 jumped over the lazy dog"] 51
assert pad-int-261 [expr {8108 * 2}] 16216
assert pad-list-262 [llength {a b c d e f g h i j}] 10
assert pad-dict-263 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-264 [expr {(264 + 1) * (264 + 2) / 2}] 35245
assert pad-str-265 [string length "The quick brown fox #00265 jumped over the lazy dog"] 51
assert pad-int-266 [expr {8263 * 2}] 16526
assert pad-list-267 [llength {a b c d e f g h i j}] 10
assert pad-dict-268 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-269 [expr {(269 + 1) * (269 + 2) / 2}] 36585
assert pad-str-270 [string length "The quick brown fox #00270 jumped over the lazy dog"] 51
assert pad-int-271 [expr {8418 * 2}] 16836
assert pad-list-272 [llength {a b c d e f g h i j}] 10
assert pad-dict-273 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-274 [expr {(274 + 1) * (274 + 2) / 2}] 37950
assert pad-str-275 [string length "The quick brown fox #00275 jumped over the lazy dog"] 51
assert pad-int-276 [expr {8573 * 2}] 17146
assert pad-list-277 [llength {a b c d e f g h i j}] 10
assert pad-dict-278 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-279 [expr {(279 + 1) * (279 + 2) / 2}] 39340
assert pad-str-280 [string length "The quick brown fox #00280 jumped over the lazy dog"] 51
assert pad-int-281 [expr {8728 * 2}] 17456
assert pad-list-282 [llength {a b c d e f g h i j}] 10
assert pad-dict-283 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-284 [expr {(284 + 1) * (284 + 2) / 2}] 40755
assert pad-str-285 [string length "The quick brown fox #00285 jumped over the lazy dog"] 51
assert pad-int-286 [expr {8883 * 2}] 17766
assert pad-list-287 [llength {a b c d e f g h i j}] 10
assert pad-dict-288 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-289 [expr {(289 + 1) * (289 + 2) / 2}] 42195
assert pad-str-290 [string length "The quick brown fox #00290 jumped over the lazy dog"] 51
assert pad-int-291 [expr {9038 * 2}] 18076
assert pad-list-292 [llength {a b c d e f g h i j}] 10
assert pad-dict-293 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-294 [expr {(294 + 1) * (294 + 2) / 2}] 43660
assert pad-str-295 [string length "The quick brown fox #00295 jumped over the lazy dog"] 51
assert pad-int-296 [expr {9193 * 2}] 18386
assert pad-list-297 [llength {a b c d e f g h i j}] 10
assert pad-dict-298 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-299 [expr {(299 + 1) * (299 + 2) / 2}] 45150
assert pad-str-300 [string length "The quick brown fox #00300 jumped over the lazy dog"] 51
assert pad-int-301 [expr {9348 * 2}] 18696
assert pad-list-302 [llength {a b c d e f g h i j}] 10
assert pad-dict-303 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-304 [expr {(304 + 1) * (304 + 2) / 2}] 46665
assert pad-str-305 [string length "The quick brown fox #00305 jumped over the lazy dog"] 51
assert pad-int-306 [expr {9503 * 2}] 19006
assert pad-list-307 [llength {a b c d e f g h i j}] 10
assert pad-dict-308 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-309 [expr {(309 + 1) * (309 + 2) / 2}] 48205
assert pad-str-310 [string length "The quick brown fox #00310 jumped over the lazy dog"] 51
assert pad-int-311 [expr {9658 * 2}] 19316
assert pad-list-312 [llength {a b c d e f g h i j}] 10
assert pad-dict-313 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-314 [expr {(314 + 1) * (314 + 2) / 2}] 49770
assert pad-str-315 [string length "The quick brown fox #00315 jumped over the lazy dog"] 51
assert pad-int-316 [expr {9813 * 2}] 19626
assert pad-list-317 [llength {a b c d e f g h i j}] 10
assert pad-dict-318 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-319 [expr {(319 + 1) * (319 + 2) / 2}] 51360
assert pad-str-320 [string length "The quick brown fox #00320 jumped over the lazy dog"] 51
assert pad-int-321 [expr {9968 * 2}] 19936
assert pad-list-322 [llength {a b c d e f g h i j}] 10
assert pad-dict-323 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-324 [expr {(324 + 1) * (324 + 2) / 2}] 52975
assert pad-str-325 [string length "The quick brown fox #00325 jumped over the lazy dog"] 51
assert pad-int-326 [expr {123 * 2}] 246
assert pad-list-327 [llength {a b c d e f g h i j}] 10
assert pad-dict-328 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-329 [expr {(329 + 1) * (329 + 2) / 2}] 54615
assert pad-str-330 [string length "The quick brown fox #00330 jumped over the lazy dog"] 51
assert pad-int-331 [expr {278 * 2}] 556
assert pad-list-332 [llength {a b c d e f g h i j}] 10
assert pad-dict-333 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-334 [expr {(334 + 1) * (334 + 2) / 2}] 56280
assert pad-str-335 [string length "The quick brown fox #00335 jumped over the lazy dog"] 51
assert pad-int-336 [expr {433 * 2}] 866
assert pad-list-337 [llength {a b c d e f g h i j}] 10
assert pad-dict-338 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-339 [expr {(339 + 1) * (339 + 2) / 2}] 57970
assert pad-str-340 [string length "The quick brown fox #00340 jumped over the lazy dog"] 51
assert pad-int-341 [expr {588 * 2}] 1176
assert pad-list-342 [llength {a b c d e f g h i j}] 10
assert pad-dict-343 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-344 [expr {(344 + 1) * (344 + 2) / 2}] 59685
assert pad-str-345 [string length "The quick brown fox #00345 jumped over the lazy dog"] 51
assert pad-int-346 [expr {743 * 2}] 1486
assert pad-list-347 [llength {a b c d e f g h i j}] 10
assert pad-dict-348 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-349 [expr {(349 + 1) * (349 + 2) / 2}] 61425
assert pad-str-350 [string length "The quick brown fox #00350 jumped over the lazy dog"] 51
assert pad-int-351 [expr {898 * 2}] 1796
assert pad-list-352 [llength {a b c d e f g h i j}] 10
assert pad-dict-353 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-354 [expr {(354 + 1) * (354 + 2) / 2}] 63190
assert pad-str-355 [string length "The quick brown fox #00355 jumped over the lazy dog"] 51
assert pad-int-356 [expr {1053 * 2}] 2106
assert pad-list-357 [llength {a b c d e f g h i j}] 10
assert pad-dict-358 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-359 [expr {(359 + 1) * (359 + 2) / 2}] 64980
assert pad-str-360 [string length "The quick brown fox #00360 jumped over the lazy dog"] 51
assert pad-int-361 [expr {1208 * 2}] 2416
assert pad-list-362 [llength {a b c d e f g h i j}] 10
assert pad-dict-363 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-364 [expr {(364 + 1) * (364 + 2) / 2}] 66795
assert pad-str-365 [string length "The quick brown fox #00365 jumped over the lazy dog"] 51
assert pad-int-366 [expr {1363 * 2}] 2726
assert pad-list-367 [llength {a b c d e f g h i j}] 10
assert pad-dict-368 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-369 [expr {(369 + 1) * (369 + 2) / 2}] 68635
assert pad-str-370 [string length "The quick brown fox #00370 jumped over the lazy dog"] 51
assert pad-int-371 [expr {1518 * 2}] 3036
assert pad-list-372 [llength {a b c d e f g h i j}] 10
assert pad-dict-373 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-374 [expr {(374 + 1) * (374 + 2) / 2}] 70500
assert pad-str-375 [string length "The quick brown fox #00375 jumped over the lazy dog"] 51
assert pad-int-376 [expr {1673 * 2}] 3346
assert pad-list-377 [llength {a b c d e f g h i j}] 10
assert pad-dict-378 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-379 [expr {(379 + 1) * (379 + 2) / 2}] 72390
assert pad-str-380 [string length "The quick brown fox #00380 jumped over the lazy dog"] 51
assert pad-int-381 [expr {1828 * 2}] 3656
assert pad-list-382 [llength {a b c d e f g h i j}] 10
assert pad-dict-383 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-384 [expr {(384 + 1) * (384 + 2) / 2}] 74305
assert pad-str-385 [string length "The quick brown fox #00385 jumped over the lazy dog"] 51
assert pad-int-386 [expr {1983 * 2}] 3966
assert pad-list-387 [llength {a b c d e f g h i j}] 10
assert pad-dict-388 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-389 [expr {(389 + 1) * (389 + 2) / 2}] 76245
assert pad-str-390 [string length "The quick brown fox #00390 jumped over the lazy dog"] 51
assert pad-int-391 [expr {2138 * 2}] 4276
assert pad-list-392 [llength {a b c d e f g h i j}] 10
assert pad-dict-393 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-394 [expr {(394 + 1) * (394 + 2) / 2}] 78210
assert pad-str-395 [string length "The quick brown fox #00395 jumped over the lazy dog"] 51
assert pad-int-396 [expr {2293 * 2}] 4586
assert pad-list-397 [llength {a b c d e f g h i j}] 10
assert pad-dict-398 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-399 [expr {(399 + 1) * (399 + 2) / 2}] 80200
assert pad-str-400 [string length "The quick brown fox #00400 jumped over the lazy dog"] 51
assert pad-int-401 [expr {2448 * 2}] 4896
assert pad-list-402 [llength {a b c d e f g h i j}] 10
assert pad-dict-403 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-404 [expr {(404 + 1) * (404 + 2) / 2}] 82215
assert pad-str-405 [string length "The quick brown fox #00405 jumped over the lazy dog"] 51
assert pad-int-406 [expr {2603 * 2}] 5206
assert pad-list-407 [llength {a b c d e f g h i j}] 10
assert pad-dict-408 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-409 [expr {(409 + 1) * (409 + 2) / 2}] 84255
assert pad-str-410 [string length "The quick brown fox #00410 jumped over the lazy dog"] 51
assert pad-int-411 [expr {2758 * 2}] 5516
assert pad-list-412 [llength {a b c d e f g h i j}] 10
assert pad-dict-413 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-414 [expr {(414 + 1) * (414 + 2) / 2}] 86320
assert pad-str-415 [string length "The quick brown fox #00415 jumped over the lazy dog"] 51
assert pad-int-416 [expr {2913 * 2}] 5826
assert pad-list-417 [llength {a b c d e f g h i j}] 10
assert pad-dict-418 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-419 [expr {(419 + 1) * (419 + 2) / 2}] 88410
assert pad-str-420 [string length "The quick brown fox #00420 jumped over the lazy dog"] 51
assert pad-int-421 [expr {3068 * 2}] 6136
assert pad-list-422 [llength {a b c d e f g h i j}] 10
assert pad-dict-423 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-424 [expr {(424 + 1) * (424 + 2) / 2}] 90525
assert pad-str-425 [string length "The quick brown fox #00425 jumped over the lazy dog"] 51
assert pad-int-426 [expr {3223 * 2}] 6446
assert pad-list-427 [llength {a b c d e f g h i j}] 10
assert pad-dict-428 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-429 [expr {(429 + 1) * (429 + 2) / 2}] 92665
assert pad-str-430 [string length "The quick brown fox #00430 jumped over the lazy dog"] 51
assert pad-int-431 [expr {3378 * 2}] 6756
assert pad-list-432 [llength {a b c d e f g h i j}] 10
assert pad-dict-433 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-434 [expr {(434 + 1) * (434 + 2) / 2}] 94830
assert pad-str-435 [string length "The quick brown fox #00435 jumped over the lazy dog"] 51
assert pad-int-436 [expr {3533 * 2}] 7066
assert pad-list-437 [llength {a b c d e f g h i j}] 10
assert pad-dict-438 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-439 [expr {(439 + 1) * (439 + 2) / 2}] 97020
assert pad-str-440 [string length "The quick brown fox #00440 jumped over the lazy dog"] 51
assert pad-int-441 [expr {3688 * 2}] 7376
assert pad-list-442 [llength {a b c d e f g h i j}] 10
assert pad-dict-443 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-444 [expr {(444 + 1) * (444 + 2) / 2}] 99235
assert pad-str-445 [string length "The quick brown fox #00445 jumped over the lazy dog"] 51
assert pad-int-446 [expr {3843 * 2}] 7686
assert pad-list-447 [llength {a b c d e f g h i j}] 10
assert pad-dict-448 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-449 [expr {(449 + 1) * (449 + 2) / 2}] 101475
assert pad-str-450 [string length "The quick brown fox #00450 jumped over the lazy dog"] 51
assert pad-int-451 [expr {3998 * 2}] 7996
assert pad-list-452 [llength {a b c d e f g h i j}] 10
assert pad-dict-453 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-454 [expr {(454 + 1) * (454 + 2) / 2}] 103740
assert pad-str-455 [string length "The quick brown fox #00455 jumped over the lazy dog"] 51
assert pad-int-456 [expr {4153 * 2}] 8306
assert pad-list-457 [llength {a b c d e f g h i j}] 10
assert pad-dict-458 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-459 [expr {(459 + 1) * (459 + 2) / 2}] 106030
assert pad-str-460 [string length "The quick brown fox #00460 jumped over the lazy dog"] 51
assert pad-int-461 [expr {4308 * 2}] 8616
assert pad-list-462 [llength {a b c d e f g h i j}] 10
assert pad-dict-463 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-464 [expr {(464 + 1) * (464 + 2) / 2}] 108345
assert pad-str-465 [string length "The quick brown fox #00465 jumped over the lazy dog"] 51
assert pad-int-466 [expr {4463 * 2}] 8926
assert pad-list-467 [llength {a b c d e f g h i j}] 10
assert pad-dict-468 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-469 [expr {(469 + 1) * (469 + 2) / 2}] 110685
assert pad-str-470 [string length "The quick brown fox #00470 jumped over the lazy dog"] 51
assert pad-int-471 [expr {4618 * 2}] 9236
assert pad-list-472 [llength {a b c d e f g h i j}] 10
assert pad-dict-473 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-474 [expr {(474 + 1) * (474 + 2) / 2}] 113050
assert pad-str-475 [string length "The quick brown fox #00475 jumped over the lazy dog"] 51
assert pad-int-476 [expr {4773 * 2}] 9546
assert pad-list-477 [llength {a b c d e f g h i j}] 10
assert pad-dict-478 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-479 [expr {(479 + 1) * (479 + 2) / 2}] 115440
assert pad-str-480 [string length "The quick brown fox #00480 jumped over the lazy dog"] 51
assert pad-int-481 [expr {4928 * 2}] 9856
assert pad-list-482 [llength {a b c d e f g h i j}] 10
assert pad-dict-483 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-484 [expr {(484 + 1) * (484 + 2) / 2}] 117855
assert pad-str-485 [string length "The quick brown fox #00485 jumped over the lazy dog"] 51
assert pad-int-486 [expr {5083 * 2}] 10166
assert pad-list-487 [llength {a b c d e f g h i j}] 10
assert pad-dict-488 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-489 [expr {(489 + 1) * (489 + 2) / 2}] 120295
assert pad-str-490 [string length "The quick brown fox #00490 jumped over the lazy dog"] 51
assert pad-int-491 [expr {5238 * 2}] 10476
assert pad-list-492 [llength {a b c d e f g h i j}] 10
assert pad-dict-493 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-494 [expr {(494 + 1) * (494 + 2) / 2}] 122760
assert pad-str-495 [string length "The quick brown fox #00495 jumped over the lazy dog"] 51
assert pad-int-496 [expr {5393 * 2}] 10786
assert pad-list-497 [llength {a b c d e f g h i j}] 10
assert pad-dict-498 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-499 [expr {(499 + 1) * (499 + 2) / 2}] 125250
assert pad-str-500 [string length "The quick brown fox #00500 jumped over the lazy dog"] 51
assert pad-int-501 [expr {5548 * 2}] 11096
assert pad-list-502 [llength {a b c d e f g h i j}] 10
assert pad-dict-503 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-504 [expr {(504 + 1) * (504 + 2) / 2}] 127765
assert pad-str-505 [string length "The quick brown fox #00505 jumped over the lazy dog"] 51
assert pad-int-506 [expr {5703 * 2}] 11406
assert pad-list-507 [llength {a b c d e f g h i j}] 10
assert pad-dict-508 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-509 [expr {(509 + 1) * (509 + 2) / 2}] 130305
assert pad-str-510 [string length "The quick brown fox #00510 jumped over the lazy dog"] 51
assert pad-int-511 [expr {5858 * 2}] 11716
assert pad-list-512 [llength {a b c d e f g h i j}] 10
assert pad-dict-513 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-514 [expr {(514 + 1) * (514 + 2) / 2}] 132870
assert pad-str-515 [string length "The quick brown fox #00515 jumped over the lazy dog"] 51
assert pad-int-516 [expr {6013 * 2}] 12026
assert pad-list-517 [llength {a b c d e f g h i j}] 10
assert pad-dict-518 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-519 [expr {(519 + 1) * (519 + 2) / 2}] 135460
assert pad-str-520 [string length "The quick brown fox #00520 jumped over the lazy dog"] 51
assert pad-int-521 [expr {6168 * 2}] 12336
assert pad-list-522 [llength {a b c d e f g h i j}] 10
assert pad-dict-523 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-524 [expr {(524 + 1) * (524 + 2) / 2}] 138075
assert pad-str-525 [string length "The quick brown fox #00525 jumped over the lazy dog"] 51
assert pad-int-526 [expr {6323 * 2}] 12646
assert pad-list-527 [llength {a b c d e f g h i j}] 10
assert pad-dict-528 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-529 [expr {(529 + 1) * (529 + 2) / 2}] 140715
assert pad-str-530 [string length "The quick brown fox #00530 jumped over the lazy dog"] 51
assert pad-int-531 [expr {6478 * 2}] 12956
assert pad-list-532 [llength {a b c d e f g h i j}] 10
assert pad-dict-533 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-534 [expr {(534 + 1) * (534 + 2) / 2}] 143380
assert pad-str-535 [string length "The quick brown fox #00535 jumped over the lazy dog"] 51
assert pad-int-536 [expr {6633 * 2}] 13266
assert pad-list-537 [llength {a b c d e f g h i j}] 10
assert pad-dict-538 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-539 [expr {(539 + 1) * (539 + 2) / 2}] 146070
assert pad-str-540 [string length "The quick brown fox #00540 jumped over the lazy dog"] 51
assert pad-int-541 [expr {6788 * 2}] 13576
assert pad-list-542 [llength {a b c d e f g h i j}] 10
assert pad-dict-543 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-544 [expr {(544 + 1) * (544 + 2) / 2}] 148785
assert pad-str-545 [string length "The quick brown fox #00545 jumped over the lazy dog"] 51
assert pad-int-546 [expr {6943 * 2}] 13886
assert pad-list-547 [llength {a b c d e f g h i j}] 10
assert pad-dict-548 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-549 [expr {(549 + 1) * (549 + 2) / 2}] 151525
assert pad-str-550 [string length "The quick brown fox #00550 jumped over the lazy dog"] 51
assert pad-int-551 [expr {7098 * 2}] 14196
assert pad-list-552 [llength {a b c d e f g h i j}] 10
assert pad-dict-553 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-554 [expr {(554 + 1) * (554 + 2) / 2}] 154290
assert pad-str-555 [string length "The quick brown fox #00555 jumped over the lazy dog"] 51
assert pad-int-556 [expr {7253 * 2}] 14506
assert pad-list-557 [llength {a b c d e f g h i j}] 10
assert pad-dict-558 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-559 [expr {(559 + 1) * (559 + 2) / 2}] 157080
assert pad-str-560 [string length "The quick brown fox #00560 jumped over the lazy dog"] 51
assert pad-int-561 [expr {7408 * 2}] 14816
assert pad-list-562 [llength {a b c d e f g h i j}] 10
assert pad-dict-563 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-564 [expr {(564 + 1) * (564 + 2) / 2}] 159895
assert pad-str-565 [string length "The quick brown fox #00565 jumped over the lazy dog"] 51
assert pad-int-566 [expr {7563 * 2}] 15126
assert pad-list-567 [llength {a b c d e f g h i j}] 10
assert pad-dict-568 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-569 [expr {(569 + 1) * (569 + 2) / 2}] 162735
assert pad-str-570 [string length "The quick brown fox #00570 jumped over the lazy dog"] 51
assert pad-int-571 [expr {7718 * 2}] 15436
assert pad-list-572 [llength {a b c d e f g h i j}] 10
assert pad-dict-573 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-574 [expr {(574 + 1) * (574 + 2) / 2}] 165600
assert pad-str-575 [string length "The quick brown fox #00575 jumped over the lazy dog"] 51
assert pad-int-576 [expr {7873 * 2}] 15746
assert pad-list-577 [llength {a b c d e f g h i j}] 10
assert pad-dict-578 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-579 [expr {(579 + 1) * (579 + 2) / 2}] 168490
assert pad-str-580 [string length "The quick brown fox #00580 jumped over the lazy dog"] 51
assert pad-int-581 [expr {8028 * 2}] 16056
assert pad-list-582 [llength {a b c d e f g h i j}] 10
assert pad-dict-583 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-584 [expr {(584 + 1) * (584 + 2) / 2}] 171405
assert pad-str-585 [string length "The quick brown fox #00585 jumped over the lazy dog"] 51
assert pad-int-586 [expr {8183 * 2}] 16366
assert pad-list-587 [llength {a b c d e f g h i j}] 10
assert pad-dict-588 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-589 [expr {(589 + 1) * (589 + 2) / 2}] 174345
assert pad-str-590 [string length "The quick brown fox #00590 jumped over the lazy dog"] 51
assert pad-int-591 [expr {8338 * 2}] 16676
assert pad-list-592 [llength {a b c d e f g h i j}] 10
assert pad-dict-593 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-594 [expr {(594 + 1) * (594 + 2) / 2}] 177310
assert pad-str-595 [string length "The quick brown fox #00595 jumped over the lazy dog"] 51
assert pad-int-596 [expr {8493 * 2}] 16986
assert pad-list-597 [llength {a b c d e f g h i j}] 10
assert pad-dict-598 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-599 [expr {(599 + 1) * (599 + 2) / 2}] 180300
assert pad-str-600 [string length "The quick brown fox #00600 jumped over the lazy dog"] 51
assert pad-int-601 [expr {8648 * 2}] 17296
assert pad-list-602 [llength {a b c d e f g h i j}] 10
assert pad-dict-603 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-604 [expr {(604 + 1) * (604 + 2) / 2}] 183315
assert pad-str-605 [string length "The quick brown fox #00605 jumped over the lazy dog"] 51
assert pad-int-606 [expr {8803 * 2}] 17606
assert pad-list-607 [llength {a b c d e f g h i j}] 10
assert pad-dict-608 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-609 [expr {(609 + 1) * (609 + 2) / 2}] 186355
assert pad-str-610 [string length "The quick brown fox #00610 jumped over the lazy dog"] 51
assert pad-int-611 [expr {8958 * 2}] 17916
assert pad-list-612 [llength {a b c d e f g h i j}] 10
assert pad-dict-613 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-614 [expr {(614 + 1) * (614 + 2) / 2}] 189420
assert pad-str-615 [string length "The quick brown fox #00615 jumped over the lazy dog"] 51
assert pad-int-616 [expr {9113 * 2}] 18226
assert pad-list-617 [llength {a b c d e f g h i j}] 10
assert pad-dict-618 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-619 [expr {(619 + 1) * (619 + 2) / 2}] 192510
assert pad-str-620 [string length "The quick brown fox #00620 jumped over the lazy dog"] 51
assert pad-int-621 [expr {9268 * 2}] 18536
assert pad-list-622 [llength {a b c d e f g h i j}] 10
assert pad-dict-623 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-624 [expr {(624 + 1) * (624 + 2) / 2}] 195625
assert pad-str-625 [string length "The quick brown fox #00625 jumped over the lazy dog"] 51
assert pad-int-626 [expr {9423 * 2}] 18846
assert pad-list-627 [llength {a b c d e f g h i j}] 10
assert pad-dict-628 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-629 [expr {(629 + 1) * (629 + 2) / 2}] 198765
assert pad-str-630 [string length "The quick brown fox #00630 jumped over the lazy dog"] 51
assert pad-int-631 [expr {9578 * 2}] 19156
assert pad-list-632 [llength {a b c d e f g h i j}] 10
assert pad-dict-633 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-634 [expr {(634 + 1) * (634 + 2) / 2}] 201930
assert pad-str-635 [string length "The quick brown fox #00635 jumped over the lazy dog"] 51
assert pad-int-636 [expr {9733 * 2}] 19466
assert pad-list-637 [llength {a b c d e f g h i j}] 10
assert pad-dict-638 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-639 [expr {(639 + 1) * (639 + 2) / 2}] 205120
assert pad-str-640 [string length "The quick brown fox #00640 jumped over the lazy dog"] 51
assert pad-int-641 [expr {9888 * 2}] 19776
assert pad-list-642 [llength {a b c d e f g h i j}] 10
assert pad-dict-643 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-644 [expr {(644 + 1) * (644 + 2) / 2}] 208335
assert pad-str-645 [string length "The quick brown fox #00645 jumped over the lazy dog"] 51
assert pad-int-646 [expr {43 * 2}] 86
assert pad-list-647 [llength {a b c d e f g h i j}] 10
assert pad-dict-648 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-649 [expr {(649 + 1) * (649 + 2) / 2}] 211575
assert pad-str-650 [string length "The quick brown fox #00650 jumped over the lazy dog"] 51
assert pad-int-651 [expr {198 * 2}] 396
assert pad-list-652 [llength {a b c d e f g h i j}] 10
assert pad-dict-653 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-654 [expr {(654 + 1) * (654 + 2) / 2}] 214840
assert pad-str-655 [string length "The quick brown fox #00655 jumped over the lazy dog"] 51
assert pad-int-656 [expr {353 * 2}] 706
assert pad-list-657 [llength {a b c d e f g h i j}] 10
assert pad-dict-658 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-659 [expr {(659 + 1) * (659 + 2) / 2}] 218130
assert pad-str-660 [string length "The quick brown fox #00660 jumped over the lazy dog"] 51
assert pad-int-661 [expr {508 * 2}] 1016
assert pad-list-662 [llength {a b c d e f g h i j}] 10
assert pad-dict-663 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-664 [expr {(664 + 1) * (664 + 2) / 2}] 221445
assert pad-str-665 [string length "The quick brown fox #00665 jumped over the lazy dog"] 51
assert pad-int-666 [expr {663 * 2}] 1326
assert pad-list-667 [llength {a b c d e f g h i j}] 10
assert pad-dict-668 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-669 [expr {(669 + 1) * (669 + 2) / 2}] 224785
assert pad-str-670 [string length "The quick brown fox #00670 jumped over the lazy dog"] 51
assert pad-int-671 [expr {818 * 2}] 1636
assert pad-list-672 [llength {a b c d e f g h i j}] 10
assert pad-dict-673 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-674 [expr {(674 + 1) * (674 + 2) / 2}] 228150
assert pad-str-675 [string length "The quick brown fox #00675 jumped over the lazy dog"] 51
assert pad-int-676 [expr {973 * 2}] 1946
assert pad-list-677 [llength {a b c d e f g h i j}] 10
assert pad-dict-678 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-679 [expr {(679 + 1) * (679 + 2) / 2}] 231540
assert pad-str-680 [string length "The quick brown fox #00680 jumped over the lazy dog"] 51
assert pad-int-681 [expr {1128 * 2}] 2256
assert pad-list-682 [llength {a b c d e f g h i j}] 10
assert pad-dict-683 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-684 [expr {(684 + 1) * (684 + 2) / 2}] 234955
assert pad-str-685 [string length "The quick brown fox #00685 jumped over the lazy dog"] 51
assert pad-int-686 [expr {1283 * 2}] 2566
assert pad-list-687 [llength {a b c d e f g h i j}] 10
assert pad-dict-688 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-689 [expr {(689 + 1) * (689 + 2) / 2}] 238395
assert pad-str-690 [string length "The quick brown fox #00690 jumped over the lazy dog"] 51
assert pad-int-691 [expr {1438 * 2}] 2876
assert pad-list-692 [llength {a b c d e f g h i j}] 10
assert pad-dict-693 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-694 [expr {(694 + 1) * (694 + 2) / 2}] 241860
assert pad-str-695 [string length "The quick brown fox #00695 jumped over the lazy dog"] 51
assert pad-int-696 [expr {1593 * 2}] 3186
assert pad-list-697 [llength {a b c d e f g h i j}] 10
assert pad-dict-698 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-699 [expr {(699 + 1) * (699 + 2) / 2}] 245350
assert pad-str-700 [string length "The quick brown fox #00700 jumped over the lazy dog"] 51
assert pad-int-701 [expr {1748 * 2}] 3496
assert pad-list-702 [llength {a b c d e f g h i j}] 10
assert pad-dict-703 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-704 [expr {(704 + 1) * (704 + 2) / 2}] 248865
assert pad-str-705 [string length "The quick brown fox #00705 jumped over the lazy dog"] 51
assert pad-int-706 [expr {1903 * 2}] 3806
assert pad-list-707 [llength {a b c d e f g h i j}] 10
assert pad-dict-708 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-709 [expr {(709 + 1) * (709 + 2) / 2}] 252405
assert pad-str-710 [string length "The quick brown fox #00710 jumped over the lazy dog"] 51
assert pad-int-711 [expr {2058 * 2}] 4116
assert pad-list-712 [llength {a b c d e f g h i j}] 10
assert pad-dict-713 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-714 [expr {(714 + 1) * (714 + 2) / 2}] 255970
assert pad-str-715 [string length "The quick brown fox #00715 jumped over the lazy dog"] 51
assert pad-int-716 [expr {2213 * 2}] 4426
assert pad-list-717 [llength {a b c d e f g h i j}] 10
assert pad-dict-718 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-719 [expr {(719 + 1) * (719 + 2) / 2}] 259560
assert pad-str-720 [string length "The quick brown fox #00720 jumped over the lazy dog"] 51
assert pad-int-721 [expr {2368 * 2}] 4736
assert pad-list-722 [llength {a b c d e f g h i j}] 10
assert pad-dict-723 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-724 [expr {(724 + 1) * (724 + 2) / 2}] 263175
assert pad-str-725 [string length "The quick brown fox #00725 jumped over the lazy dog"] 51
assert pad-int-726 [expr {2523 * 2}] 5046
assert pad-list-727 [llength {a b c d e f g h i j}] 10
assert pad-dict-728 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-729 [expr {(729 + 1) * (729 + 2) / 2}] 266815
assert pad-str-730 [string length "The quick brown fox #00730 jumped over the lazy dog"] 51
assert pad-int-731 [expr {2678 * 2}] 5356
assert pad-list-732 [llength {a b c d e f g h i j}] 10
assert pad-dict-733 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-734 [expr {(734 + 1) * (734 + 2) / 2}] 270480
assert pad-str-735 [string length "The quick brown fox #00735 jumped over the lazy dog"] 51
assert pad-int-736 [expr {2833 * 2}] 5666
assert pad-list-737 [llength {a b c d e f g h i j}] 10
assert pad-dict-738 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-739 [expr {(739 + 1) * (739 + 2) / 2}] 274170
assert pad-str-740 [string length "The quick brown fox #00740 jumped over the lazy dog"] 51
assert pad-int-741 [expr {2988 * 2}] 5976
assert pad-list-742 [llength {a b c d e f g h i j}] 10
assert pad-dict-743 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-744 [expr {(744 + 1) * (744 + 2) / 2}] 277885
assert pad-str-745 [string length "The quick brown fox #00745 jumped over the lazy dog"] 51
assert pad-int-746 [expr {3143 * 2}] 6286
assert pad-list-747 [llength {a b c d e f g h i j}] 10
assert pad-dict-748 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-749 [expr {(749 + 1) * (749 + 2) / 2}] 281625
assert pad-str-750 [string length "The quick brown fox #00750 jumped over the lazy dog"] 51
assert pad-int-751 [expr {3298 * 2}] 6596
assert pad-list-752 [llength {a b c d e f g h i j}] 10
assert pad-dict-753 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-754 [expr {(754 + 1) * (754 + 2) / 2}] 285390
assert pad-str-755 [string length "The quick brown fox #00755 jumped over the lazy dog"] 51
assert pad-int-756 [expr {3453 * 2}] 6906
assert pad-list-757 [llength {a b c d e f g h i j}] 10
assert pad-dict-758 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-759 [expr {(759 + 1) * (759 + 2) / 2}] 289180
assert pad-str-760 [string length "The quick brown fox #00760 jumped over the lazy dog"] 51
assert pad-int-761 [expr {3608 * 2}] 7216
assert pad-list-762 [llength {a b c d e f g h i j}] 10
assert pad-dict-763 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-764 [expr {(764 + 1) * (764 + 2) / 2}] 292995
assert pad-str-765 [string length "The quick brown fox #00765 jumped over the lazy dog"] 51
assert pad-int-766 [expr {3763 * 2}] 7526
assert pad-list-767 [llength {a b c d e f g h i j}] 10
assert pad-dict-768 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-769 [expr {(769 + 1) * (769 + 2) / 2}] 296835
assert pad-str-770 [string length "The quick brown fox #00770 jumped over the lazy dog"] 51
assert pad-int-771 [expr {3918 * 2}] 7836
assert pad-list-772 [llength {a b c d e f g h i j}] 10
assert pad-dict-773 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-774 [expr {(774 + 1) * (774 + 2) / 2}] 300700
assert pad-str-775 [string length "The quick brown fox #00775 jumped over the lazy dog"] 51
assert pad-int-776 [expr {4073 * 2}] 8146
assert pad-list-777 [llength {a b c d e f g h i j}] 10
assert pad-dict-778 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-779 [expr {(779 + 1) * (779 + 2) / 2}] 304590
assert pad-str-780 [string length "The quick brown fox #00780 jumped over the lazy dog"] 51
assert pad-int-781 [expr {4228 * 2}] 8456
assert pad-list-782 [llength {a b c d e f g h i j}] 10
assert pad-dict-783 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-784 [expr {(784 + 1) * (784 + 2) / 2}] 308505
assert pad-str-785 [string length "The quick brown fox #00785 jumped over the lazy dog"] 51
assert pad-int-786 [expr {4383 * 2}] 8766
assert pad-list-787 [llength {a b c d e f g h i j}] 10
assert pad-dict-788 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-789 [expr {(789 + 1) * (789 + 2) / 2}] 312445
assert pad-str-790 [string length "The quick brown fox #00790 jumped over the lazy dog"] 51
assert pad-int-791 [expr {4538 * 2}] 9076
assert pad-list-792 [llength {a b c d e f g h i j}] 10
assert pad-dict-793 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-794 [expr {(794 + 1) * (794 + 2) / 2}] 316410
assert pad-str-795 [string length "The quick brown fox #00795 jumped over the lazy dog"] 51
assert pad-int-796 [expr {4693 * 2}] 9386
assert pad-list-797 [llength {a b c d e f g h i j}] 10
assert pad-dict-798 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-799 [expr {(799 + 1) * (799 + 2) / 2}] 320400
assert pad-str-800 [string length "The quick brown fox #00800 jumped over the lazy dog"] 51
assert pad-int-801 [expr {4848 * 2}] 9696
assert pad-list-802 [llength {a b c d e f g h i j}] 10
assert pad-dict-803 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-804 [expr {(804 + 1) * (804 + 2) / 2}] 324415
assert pad-str-805 [string length "The quick brown fox #00805 jumped over the lazy dog"] 51
assert pad-int-806 [expr {5003 * 2}] 10006
assert pad-list-807 [llength {a b c d e f g h i j}] 10
assert pad-dict-808 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-809 [expr {(809 + 1) * (809 + 2) / 2}] 328455
assert pad-str-810 [string length "The quick brown fox #00810 jumped over the lazy dog"] 51
assert pad-int-811 [expr {5158 * 2}] 10316
assert pad-list-812 [llength {a b c d e f g h i j}] 10
assert pad-dict-813 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-814 [expr {(814 + 1) * (814 + 2) / 2}] 332520
assert pad-str-815 [string length "The quick brown fox #00815 jumped over the lazy dog"] 51
assert pad-int-816 [expr {5313 * 2}] 10626
assert pad-list-817 [llength {a b c d e f g h i j}] 10
assert pad-dict-818 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-819 [expr {(819 + 1) * (819 + 2) / 2}] 336610
assert pad-str-820 [string length "The quick brown fox #00820 jumped over the lazy dog"] 51
assert pad-int-821 [expr {5468 * 2}] 10936
assert pad-list-822 [llength {a b c d e f g h i j}] 10
assert pad-dict-823 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-824 [expr {(824 + 1) * (824 + 2) / 2}] 340725
assert pad-str-825 [string length "The quick brown fox #00825 jumped over the lazy dog"] 51
assert pad-int-826 [expr {5623 * 2}] 11246
assert pad-list-827 [llength {a b c d e f g h i j}] 10
assert pad-dict-828 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-829 [expr {(829 + 1) * (829 + 2) / 2}] 344865
assert pad-str-830 [string length "The quick brown fox #00830 jumped over the lazy dog"] 51
assert pad-int-831 [expr {5778 * 2}] 11556
assert pad-list-832 [llength {a b c d e f g h i j}] 10
assert pad-dict-833 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-834 [expr {(834 + 1) * (834 + 2) / 2}] 349030
assert pad-str-835 [string length "The quick brown fox #00835 jumped over the lazy dog"] 51
assert pad-int-836 [expr {5933 * 2}] 11866
assert pad-list-837 [llength {a b c d e f g h i j}] 10
assert pad-dict-838 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-839 [expr {(839 + 1) * (839 + 2) / 2}] 353220
assert pad-str-840 [string length "The quick brown fox #00840 jumped over the lazy dog"] 51
assert pad-int-841 [expr {6088 * 2}] 12176
assert pad-list-842 [llength {a b c d e f g h i j}] 10
assert pad-dict-843 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-844 [expr {(844 + 1) * (844 + 2) / 2}] 357435
assert pad-str-845 [string length "The quick brown fox #00845 jumped over the lazy dog"] 51
assert pad-int-846 [expr {6243 * 2}] 12486
assert pad-list-847 [llength {a b c d e f g h i j}] 10
assert pad-dict-848 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-849 [expr {(849 + 1) * (849 + 2) / 2}] 361675
assert pad-str-850 [string length "The quick brown fox #00850 jumped over the lazy dog"] 51
assert pad-int-851 [expr {6398 * 2}] 12796
assert pad-list-852 [llength {a b c d e f g h i j}] 10
assert pad-dict-853 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-854 [expr {(854 + 1) * (854 + 2) / 2}] 365940
assert pad-str-855 [string length "The quick brown fox #00855 jumped over the lazy dog"] 51
assert pad-int-856 [expr {6553 * 2}] 13106
assert pad-list-857 [llength {a b c d e f g h i j}] 10
assert pad-dict-858 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-859 [expr {(859 + 1) * (859 + 2) / 2}] 370230
assert pad-str-860 [string length "The quick brown fox #00860 jumped over the lazy dog"] 51
assert pad-int-861 [expr {6708 * 2}] 13416
assert pad-list-862 [llength {a b c d e f g h i j}] 10
assert pad-dict-863 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-864 [expr {(864 + 1) * (864 + 2) / 2}] 374545
assert pad-str-865 [string length "The quick brown fox #00865 jumped over the lazy dog"] 51
assert pad-int-866 [expr {6863 * 2}] 13726
assert pad-list-867 [llength {a b c d e f g h i j}] 10
assert pad-dict-868 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-869 [expr {(869 + 1) * (869 + 2) / 2}] 378885
assert pad-str-870 [string length "The quick brown fox #00870 jumped over the lazy dog"] 51
assert pad-int-871 [expr {7018 * 2}] 14036
assert pad-list-872 [llength {a b c d e f g h i j}] 10
assert pad-dict-873 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-874 [expr {(874 + 1) * (874 + 2) / 2}] 383250
assert pad-str-875 [string length "The quick brown fox #00875 jumped over the lazy dog"] 51
assert pad-int-876 [expr {7173 * 2}] 14346
assert pad-list-877 [llength {a b c d e f g h i j}] 10
assert pad-dict-878 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-879 [expr {(879 + 1) * (879 + 2) / 2}] 387640
assert pad-str-880 [string length "The quick brown fox #00880 jumped over the lazy dog"] 51
assert pad-int-881 [expr {7328 * 2}] 14656
assert pad-list-882 [llength {a b c d e f g h i j}] 10
assert pad-dict-883 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-884 [expr {(884 + 1) * (884 + 2) / 2}] 392055
assert pad-str-885 [string length "The quick brown fox #00885 jumped over the lazy dog"] 51
assert pad-int-886 [expr {7483 * 2}] 14966
assert pad-list-887 [llength {a b c d e f g h i j}] 10
assert pad-dict-888 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-889 [expr {(889 + 1) * (889 + 2) / 2}] 396495
assert pad-str-890 [string length "The quick brown fox #00890 jumped over the lazy dog"] 51
assert pad-int-891 [expr {7638 * 2}] 15276
assert pad-list-892 [llength {a b c d e f g h i j}] 10
assert pad-dict-893 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-894 [expr {(894 + 1) * (894 + 2) / 2}] 400960
assert pad-str-895 [string length "The quick brown fox #00895 jumped over the lazy dog"] 51
assert pad-int-896 [expr {7793 * 2}] 15586
assert pad-list-897 [llength {a b c d e f g h i j}] 10
assert pad-dict-898 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-899 [expr {(899 + 1) * (899 + 2) / 2}] 405450
assert pad-str-900 [string length "The quick brown fox #00900 jumped over the lazy dog"] 51
assert pad-int-901 [expr {7948 * 2}] 15896
assert pad-list-902 [llength {a b c d e f g h i j}] 10
assert pad-dict-903 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-904 [expr {(904 + 1) * (904 + 2) / 2}] 409965
assert pad-str-905 [string length "The quick brown fox #00905 jumped over the lazy dog"] 51
assert pad-int-906 [expr {8103 * 2}] 16206
assert pad-list-907 [llength {a b c d e f g h i j}] 10
assert pad-dict-908 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-909 [expr {(909 + 1) * (909 + 2) / 2}] 414505
assert pad-str-910 [string length "The quick brown fox #00910 jumped over the lazy dog"] 51
assert pad-int-911 [expr {8258 * 2}] 16516
assert pad-list-912 [llength {a b c d e f g h i j}] 10
assert pad-dict-913 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-914 [expr {(914 + 1) * (914 + 2) / 2}] 419070
assert pad-str-915 [string length "The quick brown fox #00915 jumped over the lazy dog"] 51
assert pad-int-916 [expr {8413 * 2}] 16826
assert pad-list-917 [llength {a b c d e f g h i j}] 10
assert pad-dict-918 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-919 [expr {(919 + 1) * (919 + 2) / 2}] 423660
assert pad-str-920 [string length "The quick brown fox #00920 jumped over the lazy dog"] 51
assert pad-int-921 [expr {8568 * 2}] 17136
assert pad-list-922 [llength {a b c d e f g h i j}] 10
assert pad-dict-923 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-924 [expr {(924 + 1) * (924 + 2) / 2}] 428275
assert pad-str-925 [string length "The quick brown fox #00925 jumped over the lazy dog"] 51
assert pad-int-926 [expr {8723 * 2}] 17446
assert pad-list-927 [llength {a b c d e f g h i j}] 10
assert pad-dict-928 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-929 [expr {(929 + 1) * (929 + 2) / 2}] 432915
assert pad-str-930 [string length "The quick brown fox #00930 jumped over the lazy dog"] 51
assert pad-int-931 [expr {8878 * 2}] 17756
assert pad-list-932 [llength {a b c d e f g h i j}] 10
assert pad-dict-933 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-934 [expr {(934 + 1) * (934 + 2) / 2}] 437580
assert pad-str-935 [string length "The quick brown fox #00935 jumped over the lazy dog"] 51
assert pad-int-936 [expr {9033 * 2}] 18066
assert pad-list-937 [llength {a b c d e f g h i j}] 10
assert pad-dict-938 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-939 [expr {(939 + 1) * (939 + 2) / 2}] 442270
assert pad-str-940 [string length "The quick brown fox #00940 jumped over the lazy dog"] 51
assert pad-int-941 [expr {9188 * 2}] 18376
assert pad-list-942 [llength {a b c d e f g h i j}] 10
assert pad-dict-943 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-944 [expr {(944 + 1) * (944 + 2) / 2}] 446985
assert pad-str-945 [string length "The quick brown fox #00945 jumped over the lazy dog"] 51
assert pad-int-946 [expr {9343 * 2}] 18686
assert pad-list-947 [llength {a b c d e f g h i j}] 10
assert pad-dict-948 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-949 [expr {(949 + 1) * (949 + 2) / 2}] 451725
assert pad-str-950 [string length "The quick brown fox #00950 jumped over the lazy dog"] 51
assert pad-int-951 [expr {9498 * 2}] 18996
assert pad-list-952 [llength {a b c d e f g h i j}] 10
assert pad-dict-953 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-954 [expr {(954 + 1) * (954 + 2) / 2}] 456490
assert pad-str-955 [string length "The quick brown fox #00955 jumped over the lazy dog"] 51
assert pad-int-956 [expr {9653 * 2}] 19306
assert pad-list-957 [llength {a b c d e f g h i j}] 10
assert pad-dict-958 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-959 [expr {(959 + 1) * (959 + 2) / 2}] 461280
assert pad-str-960 [string length "The quick brown fox #00960 jumped over the lazy dog"] 51
assert pad-int-961 [expr {9808 * 2}] 19616
assert pad-list-962 [llength {a b c d e f g h i j}] 10
assert pad-dict-963 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-964 [expr {(964 + 1) * (964 + 2) / 2}] 466095
assert pad-str-965 [string length "The quick brown fox #00965 jumped over the lazy dog"] 51
assert pad-int-966 [expr {9963 * 2}] 19926
assert pad-list-967 [llength {a b c d e f g h i j}] 10
assert pad-dict-968 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-969 [expr {(969 + 1) * (969 + 2) / 2}] 470935
assert pad-str-970 [string length "The quick brown fox #00970 jumped over the lazy dog"] 51
assert pad-int-971 [expr {118 * 2}] 236
assert pad-list-972 [llength {a b c d e f g h i j}] 10
assert pad-dict-973 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-974 [expr {(974 + 1) * (974 + 2) / 2}] 475800
assert pad-str-975 [string length "The quick brown fox #00975 jumped over the lazy dog"] 51
assert pad-int-976 [expr {273 * 2}] 546
assert pad-list-977 [llength {a b c d e f g h i j}] 10
assert pad-dict-978 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-979 [expr {(979 + 1) * (979 + 2) / 2}] 480690
assert pad-str-980 [string length "The quick brown fox #00980 jumped over the lazy dog"] 51
assert pad-int-981 [expr {428 * 2}] 856
assert pad-list-982 [llength {a b c d e f g h i j}] 10
assert pad-dict-983 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-984 [expr {(984 + 1) * (984 + 2) / 2}] 485605
assert pad-str-985 [string length "The quick brown fox #00985 jumped over the lazy dog"] 51
assert pad-int-986 [expr {583 * 2}] 1166
assert pad-list-987 [llength {a b c d e f g h i j}] 10
assert pad-dict-988 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-989 [expr {(989 + 1) * (989 + 2) / 2}] 490545
assert pad-str-990 [string length "The quick brown fox #00990 jumped over the lazy dog"] 51
assert pad-int-991 [expr {738 * 2}] 1476
assert pad-list-992 [llength {a b c d e f g h i j}] 10
assert pad-dict-993 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-994 [expr {(994 + 1) * (994 + 2) / 2}] 495510
assert pad-str-995 [string length "The quick brown fox #00995 jumped over the lazy dog"] 51
assert pad-int-996 [expr {893 * 2}] 1786
assert pad-list-997 [llength {a b c d e f g h i j}] 10
assert pad-dict-998 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-999 [expr {(999 + 1) * (999 + 2) / 2}] 500500
assert pad-str-1000 [string length "The quick brown fox #01000 jumped over the lazy dog"] 51
assert pad-int-1001 [expr {1048 * 2}] 2096
assert pad-list-1002 [llength {a b c d e f g h i j}] 10
assert pad-dict-1003 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1004 [expr {(1004 + 1) * (1004 + 2) / 2}] 505515
assert pad-str-1005 [string length "The quick brown fox #01005 jumped over the lazy dog"] 51
assert pad-int-1006 [expr {1203 * 2}] 2406
assert pad-list-1007 [llength {a b c d e f g h i j}] 10
assert pad-dict-1008 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1009 [expr {(1009 + 1) * (1009 + 2) / 2}] 510555
assert pad-str-1010 [string length "The quick brown fox #01010 jumped over the lazy dog"] 51
assert pad-int-1011 [expr {1358 * 2}] 2716
assert pad-list-1012 [llength {a b c d e f g h i j}] 10
assert pad-dict-1013 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1014 [expr {(1014 + 1) * (1014 + 2) / 2}] 515620
assert pad-str-1015 [string length "The quick brown fox #01015 jumped over the lazy dog"] 51
assert pad-int-1016 [expr {1513 * 2}] 3026
assert pad-list-1017 [llength {a b c d e f g h i j}] 10
assert pad-dict-1018 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1019 [expr {(1019 + 1) * (1019 + 2) / 2}] 520710
assert pad-str-1020 [string length "The quick brown fox #01020 jumped over the lazy dog"] 51
assert pad-int-1021 [expr {1668 * 2}] 3336
assert pad-list-1022 [llength {a b c d e f g h i j}] 10
assert pad-dict-1023 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1024 [expr {(1024 + 1) * (1024 + 2) / 2}] 525825
assert pad-str-1025 [string length "The quick brown fox #01025 jumped over the lazy dog"] 51
assert pad-int-1026 [expr {1823 * 2}] 3646
assert pad-list-1027 [llength {a b c d e f g h i j}] 10
assert pad-dict-1028 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1029 [expr {(1029 + 1) * (1029 + 2) / 2}] 530965
assert pad-str-1030 [string length "The quick brown fox #01030 jumped over the lazy dog"] 51
assert pad-int-1031 [expr {1978 * 2}] 3956
assert pad-list-1032 [llength {a b c d e f g h i j}] 10
assert pad-dict-1033 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1034 [expr {(1034 + 1) * (1034 + 2) / 2}] 536130
assert pad-str-1035 [string length "The quick brown fox #01035 jumped over the lazy dog"] 51
assert pad-int-1036 [expr {2133 * 2}] 4266
assert pad-list-1037 [llength {a b c d e f g h i j}] 10
assert pad-dict-1038 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1039 [expr {(1039 + 1) * (1039 + 2) / 2}] 541320
assert pad-str-1040 [string length "The quick brown fox #01040 jumped over the lazy dog"] 51
assert pad-int-1041 [expr {2288 * 2}] 4576
assert pad-list-1042 [llength {a b c d e f g h i j}] 10
assert pad-dict-1043 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1044 [expr {(1044 + 1) * (1044 + 2) / 2}] 546535
assert pad-str-1045 [string length "The quick brown fox #01045 jumped over the lazy dog"] 51
assert pad-int-1046 [expr {2443 * 2}] 4886
assert pad-list-1047 [llength {a b c d e f g h i j}] 10
assert pad-dict-1048 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1049 [expr {(1049 + 1) * (1049 + 2) / 2}] 551775
assert pad-str-1050 [string length "The quick brown fox #01050 jumped over the lazy dog"] 51
assert pad-int-1051 [expr {2598 * 2}] 5196
assert pad-list-1052 [llength {a b c d e f g h i j}] 10
assert pad-dict-1053 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1054 [expr {(1054 + 1) * (1054 + 2) / 2}] 557040
assert pad-str-1055 [string length "The quick brown fox #01055 jumped over the lazy dog"] 51
assert pad-int-1056 [expr {2753 * 2}] 5506
assert pad-list-1057 [llength {a b c d e f g h i j}] 10
assert pad-dict-1058 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1059 [expr {(1059 + 1) * (1059 + 2) / 2}] 562330
assert pad-str-1060 [string length "The quick brown fox #01060 jumped over the lazy dog"] 51
assert pad-int-1061 [expr {2908 * 2}] 5816
assert pad-list-1062 [llength {a b c d e f g h i j}] 10
assert pad-dict-1063 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1064 [expr {(1064 + 1) * (1064 + 2) / 2}] 567645
assert pad-str-1065 [string length "The quick brown fox #01065 jumped over the lazy dog"] 51
assert pad-int-1066 [expr {3063 * 2}] 6126
assert pad-list-1067 [llength {a b c d e f g h i j}] 10
assert pad-dict-1068 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1069 [expr {(1069 + 1) * (1069 + 2) / 2}] 572985
assert pad-str-1070 [string length "The quick brown fox #01070 jumped over the lazy dog"] 51
assert pad-int-1071 [expr {3218 * 2}] 6436
assert pad-list-1072 [llength {a b c d e f g h i j}] 10
assert pad-dict-1073 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1074 [expr {(1074 + 1) * (1074 + 2) / 2}] 578350
assert pad-str-1075 [string length "The quick brown fox #01075 jumped over the lazy dog"] 51
assert pad-int-1076 [expr {3373 * 2}] 6746
assert pad-list-1077 [llength {a b c d e f g h i j}] 10
assert pad-dict-1078 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1079 [expr {(1079 + 1) * (1079 + 2) / 2}] 583740
assert pad-str-1080 [string length "The quick brown fox #01080 jumped over the lazy dog"] 51
assert pad-int-1081 [expr {3528 * 2}] 7056
assert pad-list-1082 [llength {a b c d e f g h i j}] 10
assert pad-dict-1083 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1084 [expr {(1084 + 1) * (1084 + 2) / 2}] 589155
assert pad-str-1085 [string length "The quick brown fox #01085 jumped over the lazy dog"] 51
assert pad-int-1086 [expr {3683 * 2}] 7366
assert pad-list-1087 [llength {a b c d e f g h i j}] 10
assert pad-dict-1088 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1089 [expr {(1089 + 1) * (1089 + 2) / 2}] 594595
assert pad-str-1090 [string length "The quick brown fox #01090 jumped over the lazy dog"] 51
assert pad-int-1091 [expr {3838 * 2}] 7676
assert pad-list-1092 [llength {a b c d e f g h i j}] 10
assert pad-dict-1093 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1094 [expr {(1094 + 1) * (1094 + 2) / 2}] 600060
assert pad-str-1095 [string length "The quick brown fox #01095 jumped over the lazy dog"] 51
assert pad-int-1096 [expr {3993 * 2}] 7986
assert pad-list-1097 [llength {a b c d e f g h i j}] 10
assert pad-dict-1098 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1099 [expr {(1099 + 1) * (1099 + 2) / 2}] 605550
assert pad-str-1100 [string length "The quick brown fox #01100 jumped over the lazy dog"] 51
assert pad-int-1101 [expr {4148 * 2}] 8296
assert pad-list-1102 [llength {a b c d e f g h i j}] 10
assert pad-dict-1103 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1104 [expr {(1104 + 1) * (1104 + 2) / 2}] 611065
assert pad-str-1105 [string length "The quick brown fox #01105 jumped over the lazy dog"] 51
assert pad-int-1106 [expr {4303 * 2}] 8606
assert pad-list-1107 [llength {a b c d e f g h i j}] 10
assert pad-dict-1108 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1109 [expr {(1109 + 1) * (1109 + 2) / 2}] 616605
assert pad-str-1110 [string length "The quick brown fox #01110 jumped over the lazy dog"] 51
assert pad-int-1111 [expr {4458 * 2}] 8916
assert pad-list-1112 [llength {a b c d e f g h i j}] 10
assert pad-dict-1113 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1114 [expr {(1114 + 1) * (1114 + 2) / 2}] 622170
assert pad-str-1115 [string length "The quick brown fox #01115 jumped over the lazy dog"] 51
assert pad-int-1116 [expr {4613 * 2}] 9226
assert pad-list-1117 [llength {a b c d e f g h i j}] 10
assert pad-dict-1118 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1119 [expr {(1119 + 1) * (1119 + 2) / 2}] 627760
assert pad-str-1120 [string length "The quick brown fox #01120 jumped over the lazy dog"] 51
assert pad-int-1121 [expr {4768 * 2}] 9536
assert pad-list-1122 [llength {a b c d e f g h i j}] 10
assert pad-dict-1123 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1124 [expr {(1124 + 1) * (1124 + 2) / 2}] 633375
assert pad-str-1125 [string length "The quick brown fox #01125 jumped over the lazy dog"] 51
assert pad-int-1126 [expr {4923 * 2}] 9846
assert pad-list-1127 [llength {a b c d e f g h i j}] 10
assert pad-dict-1128 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1129 [expr {(1129 + 1) * (1129 + 2) / 2}] 639015
assert pad-str-1130 [string length "The quick brown fox #01130 jumped over the lazy dog"] 51
assert pad-int-1131 [expr {5078 * 2}] 10156
assert pad-list-1132 [llength {a b c d e f g h i j}] 10
assert pad-dict-1133 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1134 [expr {(1134 + 1) * (1134 + 2) / 2}] 644680
assert pad-str-1135 [string length "The quick brown fox #01135 jumped over the lazy dog"] 51
assert pad-int-1136 [expr {5233 * 2}] 10466
assert pad-list-1137 [llength {a b c d e f g h i j}] 10
assert pad-dict-1138 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1139 [expr {(1139 + 1) * (1139 + 2) / 2}] 650370
assert pad-str-1140 [string length "The quick brown fox #01140 jumped over the lazy dog"] 51
assert pad-int-1141 [expr {5388 * 2}] 10776
assert pad-list-1142 [llength {a b c d e f g h i j}] 10
assert pad-dict-1143 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1144 [expr {(1144 + 1) * (1144 + 2) / 2}] 656085
assert pad-str-1145 [string length "The quick brown fox #01145 jumped over the lazy dog"] 51
assert pad-int-1146 [expr {5543 * 2}] 11086
assert pad-list-1147 [llength {a b c d e f g h i j}] 10
assert pad-dict-1148 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1149 [expr {(1149 + 1) * (1149 + 2) / 2}] 661825
assert pad-str-1150 [string length "The quick brown fox #01150 jumped over the lazy dog"] 51
assert pad-int-1151 [expr {5698 * 2}] 11396
assert pad-list-1152 [llength {a b c d e f g h i j}] 10
assert pad-dict-1153 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1154 [expr {(1154 + 1) * (1154 + 2) / 2}] 667590
assert pad-str-1155 [string length "The quick brown fox #01155 jumped over the lazy dog"] 51
assert pad-int-1156 [expr {5853 * 2}] 11706
assert pad-list-1157 [llength {a b c d e f g h i j}] 10
assert pad-dict-1158 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1159 [expr {(1159 + 1) * (1159 + 2) / 2}] 673380
assert pad-str-1160 [string length "The quick brown fox #01160 jumped over the lazy dog"] 51
assert pad-int-1161 [expr {6008 * 2}] 12016
assert pad-list-1162 [llength {a b c d e f g h i j}] 10
assert pad-dict-1163 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1164 [expr {(1164 + 1) * (1164 + 2) / 2}] 679195
assert pad-str-1165 [string length "The quick brown fox #01165 jumped over the lazy dog"] 51
assert pad-int-1166 [expr {6163 * 2}] 12326
assert pad-list-1167 [llength {a b c d e f g h i j}] 10
assert pad-dict-1168 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1169 [expr {(1169 + 1) * (1169 + 2) / 2}] 685035
assert pad-str-1170 [string length "The quick brown fox #01170 jumped over the lazy dog"] 51
assert pad-int-1171 [expr {6318 * 2}] 12636
assert pad-list-1172 [llength {a b c d e f g h i j}] 10
assert pad-dict-1173 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1174 [expr {(1174 + 1) * (1174 + 2) / 2}] 690900
assert pad-str-1175 [string length "The quick brown fox #01175 jumped over the lazy dog"] 51
assert pad-int-1176 [expr {6473 * 2}] 12946
assert pad-list-1177 [llength {a b c d e f g h i j}] 10
assert pad-dict-1178 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1179 [expr {(1179 + 1) * (1179 + 2) / 2}] 696790
assert pad-str-1180 [string length "The quick brown fox #01180 jumped over the lazy dog"] 51
assert pad-int-1181 [expr {6628 * 2}] 13256
assert pad-list-1182 [llength {a b c d e f g h i j}] 10
assert pad-dict-1183 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1184 [expr {(1184 + 1) * (1184 + 2) / 2}] 702705
assert pad-str-1185 [string length "The quick brown fox #01185 jumped over the lazy dog"] 51
assert pad-int-1186 [expr {6783 * 2}] 13566
assert pad-list-1187 [llength {a b c d e f g h i j}] 10
assert pad-dict-1188 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1189 [expr {(1189 + 1) * (1189 + 2) / 2}] 708645
assert pad-str-1190 [string length "The quick brown fox #01190 jumped over the lazy dog"] 51
assert pad-int-1191 [expr {6938 * 2}] 13876
assert pad-list-1192 [llength {a b c d e f g h i j}] 10
assert pad-dict-1193 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1194 [expr {(1194 + 1) * (1194 + 2) / 2}] 714610
assert pad-str-1195 [string length "The quick brown fox #01195 jumped over the lazy dog"] 51
assert pad-int-1196 [expr {7093 * 2}] 14186
assert pad-list-1197 [llength {a b c d e f g h i j}] 10
assert pad-dict-1198 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1199 [expr {(1199 + 1) * (1199 + 2) / 2}] 720600
assert pad-str-1200 [string length "The quick brown fox #01200 jumped over the lazy dog"] 51
assert pad-int-1201 [expr {7248 * 2}] 14496
assert pad-list-1202 [llength {a b c d e f g h i j}] 10
assert pad-dict-1203 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1204 [expr {(1204 + 1) * (1204 + 2) / 2}] 726615
assert pad-str-1205 [string length "The quick brown fox #01205 jumped over the lazy dog"] 51
assert pad-int-1206 [expr {7403 * 2}] 14806
assert pad-list-1207 [llength {a b c d e f g h i j}] 10
assert pad-dict-1208 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1209 [expr {(1209 + 1) * (1209 + 2) / 2}] 732655
assert pad-str-1210 [string length "The quick brown fox #01210 jumped over the lazy dog"] 51
assert pad-int-1211 [expr {7558 * 2}] 15116
assert pad-list-1212 [llength {a b c d e f g h i j}] 10
assert pad-dict-1213 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1214 [expr {(1214 + 1) * (1214 + 2) / 2}] 738720
assert pad-str-1215 [string length "The quick brown fox #01215 jumped over the lazy dog"] 51
assert pad-int-1216 [expr {7713 * 2}] 15426
assert pad-list-1217 [llength {a b c d e f g h i j}] 10
assert pad-dict-1218 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1219 [expr {(1219 + 1) * (1219 + 2) / 2}] 744810
assert pad-str-1220 [string length "The quick brown fox #01220 jumped over the lazy dog"] 51
assert pad-int-1221 [expr {7868 * 2}] 15736
assert pad-list-1222 [llength {a b c d e f g h i j}] 10
assert pad-dict-1223 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1224 [expr {(1224 + 1) * (1224 + 2) / 2}] 750925
assert pad-str-1225 [string length "The quick brown fox #01225 jumped over the lazy dog"] 51
assert pad-int-1226 [expr {8023 * 2}] 16046
assert pad-list-1227 [llength {a b c d e f g h i j}] 10
assert pad-dict-1228 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1229 [expr {(1229 + 1) * (1229 + 2) / 2}] 757065
assert pad-str-1230 [string length "The quick brown fox #01230 jumped over the lazy dog"] 51
assert pad-int-1231 [expr {8178 * 2}] 16356
assert pad-list-1232 [llength {a b c d e f g h i j}] 10
assert pad-dict-1233 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1234 [expr {(1234 + 1) * (1234 + 2) / 2}] 763230
assert pad-str-1235 [string length "The quick brown fox #01235 jumped over the lazy dog"] 51
assert pad-int-1236 [expr {8333 * 2}] 16666
assert pad-list-1237 [llength {a b c d e f g h i j}] 10
assert pad-dict-1238 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1239 [expr {(1239 + 1) * (1239 + 2) / 2}] 769420
assert pad-str-1240 [string length "The quick brown fox #01240 jumped over the lazy dog"] 51
assert pad-int-1241 [expr {8488 * 2}] 16976
assert pad-list-1242 [llength {a b c d e f g h i j}] 10
assert pad-dict-1243 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1244 [expr {(1244 + 1) * (1244 + 2) / 2}] 775635
assert pad-str-1245 [string length "The quick brown fox #01245 jumped over the lazy dog"] 51
assert pad-int-1246 [expr {8643 * 2}] 17286
assert pad-list-1247 [llength {a b c d e f g h i j}] 10
assert pad-dict-1248 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1249 [expr {(1249 + 1) * (1249 + 2) / 2}] 781875
assert pad-str-1250 [string length "The quick brown fox #01250 jumped over the lazy dog"] 51
assert pad-int-1251 [expr {8798 * 2}] 17596
assert pad-list-1252 [llength {a b c d e f g h i j}] 10
assert pad-dict-1253 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1254 [expr {(1254 + 1) * (1254 + 2) / 2}] 788140
assert pad-str-1255 [string length "The quick brown fox #01255 jumped over the lazy dog"] 51
assert pad-int-1256 [expr {8953 * 2}] 17906
assert pad-list-1257 [llength {a b c d e f g h i j}] 10
assert pad-dict-1258 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1259 [expr {(1259 + 1) * (1259 + 2) / 2}] 794430
assert pad-str-1260 [string length "The quick brown fox #01260 jumped over the lazy dog"] 51
assert pad-int-1261 [expr {9108 * 2}] 18216
assert pad-list-1262 [llength {a b c d e f g h i j}] 10
assert pad-dict-1263 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1264 [expr {(1264 + 1) * (1264 + 2) / 2}] 800745
assert pad-str-1265 [string length "The quick brown fox #01265 jumped over the lazy dog"] 51
assert pad-int-1266 [expr {9263 * 2}] 18526
assert pad-list-1267 [llength {a b c d e f g h i j}] 10
assert pad-dict-1268 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1269 [expr {(1269 + 1) * (1269 + 2) / 2}] 807085
assert pad-str-1270 [string length "The quick brown fox #01270 jumped over the lazy dog"] 51
assert pad-int-1271 [expr {9418 * 2}] 18836
assert pad-list-1272 [llength {a b c d e f g h i j}] 10
assert pad-dict-1273 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1274 [expr {(1274 + 1) * (1274 + 2) / 2}] 813450
assert pad-str-1275 [string length "The quick brown fox #01275 jumped over the lazy dog"] 51
assert pad-int-1276 [expr {9573 * 2}] 19146
assert pad-list-1277 [llength {a b c d e f g h i j}] 10
assert pad-dict-1278 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1279 [expr {(1279 + 1) * (1279 + 2) / 2}] 819840
assert pad-str-1280 [string length "The quick brown fox #01280 jumped over the lazy dog"] 51
assert pad-int-1281 [expr {9728 * 2}] 19456
assert pad-list-1282 [llength {a b c d e f g h i j}] 10
assert pad-dict-1283 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1284 [expr {(1284 + 1) * (1284 + 2) / 2}] 826255
assert pad-str-1285 [string length "The quick brown fox #01285 jumped over the lazy dog"] 51
assert pad-int-1286 [expr {9883 * 2}] 19766
assert pad-list-1287 [llength {a b c d e f g h i j}] 10
assert pad-dict-1288 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1289 [expr {(1289 + 1) * (1289 + 2) / 2}] 832695
assert pad-str-1290 [string length "The quick brown fox #01290 jumped over the lazy dog"] 51
assert pad-int-1291 [expr {38 * 2}] 76
assert pad-list-1292 [llength {a b c d e f g h i j}] 10
assert pad-dict-1293 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1294 [expr {(1294 + 1) * (1294 + 2) / 2}] 839160
assert pad-str-1295 [string length "The quick brown fox #01295 jumped over the lazy dog"] 51
assert pad-int-1296 [expr {193 * 2}] 386
assert pad-list-1297 [llength {a b c d e f g h i j}] 10
assert pad-dict-1298 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1299 [expr {(1299 + 1) * (1299 + 2) / 2}] 845650
assert pad-str-1300 [string length "The quick brown fox #01300 jumped over the lazy dog"] 51
assert pad-int-1301 [expr {348 * 2}] 696
assert pad-list-1302 [llength {a b c d e f g h i j}] 10
assert pad-dict-1303 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1304 [expr {(1304 + 1) * (1304 + 2) / 2}] 852165
assert pad-str-1305 [string length "The quick brown fox #01305 jumped over the lazy dog"] 51
assert pad-int-1306 [expr {503 * 2}] 1006
assert pad-list-1307 [llength {a b c d e f g h i j}] 10
assert pad-dict-1308 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1309 [expr {(1309 + 1) * (1309 + 2) / 2}] 858705
assert pad-str-1310 [string length "The quick brown fox #01310 jumped over the lazy dog"] 51
assert pad-int-1311 [expr {658 * 2}] 1316
assert pad-list-1312 [llength {a b c d e f g h i j}] 10
assert pad-dict-1313 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1314 [expr {(1314 + 1) * (1314 + 2) / 2}] 865270
assert pad-str-1315 [string length "The quick brown fox #01315 jumped over the lazy dog"] 51
assert pad-int-1316 [expr {813 * 2}] 1626
assert pad-list-1317 [llength {a b c d e f g h i j}] 10
assert pad-dict-1318 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1319 [expr {(1319 + 1) * (1319 + 2) / 2}] 871860
assert pad-str-1320 [string length "The quick brown fox #01320 jumped over the lazy dog"] 51
assert pad-int-1321 [expr {968 * 2}] 1936
assert pad-list-1322 [llength {a b c d e f g h i j}] 10
assert pad-dict-1323 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1324 [expr {(1324 + 1) * (1324 + 2) / 2}] 878475
assert pad-str-1325 [string length "The quick brown fox #01325 jumped over the lazy dog"] 51
assert pad-int-1326 [expr {1123 * 2}] 2246
assert pad-list-1327 [llength {a b c d e f g h i j}] 10
assert pad-dict-1328 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1329 [expr {(1329 + 1) * (1329 + 2) / 2}] 885115
assert pad-str-1330 [string length "The quick brown fox #01330 jumped over the lazy dog"] 51
assert pad-int-1331 [expr {1278 * 2}] 2556
assert pad-list-1332 [llength {a b c d e f g h i j}] 10
assert pad-dict-1333 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1334 [expr {(1334 + 1) * (1334 + 2) / 2}] 891780
assert pad-str-1335 [string length "The quick brown fox #01335 jumped over the lazy dog"] 51
assert pad-int-1336 [expr {1433 * 2}] 2866
assert pad-list-1337 [llength {a b c d e f g h i j}] 10
assert pad-dict-1338 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1339 [expr {(1339 + 1) * (1339 + 2) / 2}] 898470
assert pad-str-1340 [string length "The quick brown fox #01340 jumped over the lazy dog"] 51
assert pad-int-1341 [expr {1588 * 2}] 3176
assert pad-list-1342 [llength {a b c d e f g h i j}] 10
assert pad-dict-1343 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1344 [expr {(1344 + 1) * (1344 + 2) / 2}] 905185
assert pad-str-1345 [string length "The quick brown fox #01345 jumped over the lazy dog"] 51
assert pad-int-1346 [expr {1743 * 2}] 3486
assert pad-list-1347 [llength {a b c d e f g h i j}] 10
assert pad-dict-1348 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1349 [expr {(1349 + 1) * (1349 + 2) / 2}] 911925
assert pad-str-1350 [string length "The quick brown fox #01350 jumped over the lazy dog"] 51
assert pad-int-1351 [expr {1898 * 2}] 3796
assert pad-list-1352 [llength {a b c d e f g h i j}] 10
assert pad-dict-1353 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1354 [expr {(1354 + 1) * (1354 + 2) / 2}] 918690
assert pad-str-1355 [string length "The quick brown fox #01355 jumped over the lazy dog"] 51
assert pad-int-1356 [expr {2053 * 2}] 4106
assert pad-list-1357 [llength {a b c d e f g h i j}] 10
assert pad-dict-1358 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1359 [expr {(1359 + 1) * (1359 + 2) / 2}] 925480
assert pad-str-1360 [string length "The quick brown fox #01360 jumped over the lazy dog"] 51
assert pad-int-1361 [expr {2208 * 2}] 4416
assert pad-list-1362 [llength {a b c d e f g h i j}] 10
assert pad-dict-1363 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1364 [expr {(1364 + 1) * (1364 + 2) / 2}] 932295
assert pad-str-1365 [string length "The quick brown fox #01365 jumped over the lazy dog"] 51
assert pad-int-1366 [expr {2363 * 2}] 4726
assert pad-list-1367 [llength {a b c d e f g h i j}] 10
assert pad-dict-1368 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1369 [expr {(1369 + 1) * (1369 + 2) / 2}] 939135
assert pad-str-1370 [string length "The quick brown fox #01370 jumped over the lazy dog"] 51
assert pad-int-1371 [expr {2518 * 2}] 5036
assert pad-list-1372 [llength {a b c d e f g h i j}] 10
assert pad-dict-1373 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1374 [expr {(1374 + 1) * (1374 + 2) / 2}] 946000
assert pad-str-1375 [string length "The quick brown fox #01375 jumped over the lazy dog"] 51
assert pad-int-1376 [expr {2673 * 2}] 5346
assert pad-list-1377 [llength {a b c d e f g h i j}] 10
assert pad-dict-1378 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1379 [expr {(1379 + 1) * (1379 + 2) / 2}] 952890
assert pad-str-1380 [string length "The quick brown fox #01380 jumped over the lazy dog"] 51
assert pad-int-1381 [expr {2828 * 2}] 5656
assert pad-list-1382 [llength {a b c d e f g h i j}] 10
assert pad-dict-1383 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1384 [expr {(1384 + 1) * (1384 + 2) / 2}] 959805
assert pad-str-1385 [string length "The quick brown fox #01385 jumped over the lazy dog"] 51
assert pad-int-1386 [expr {2983 * 2}] 5966
assert pad-list-1387 [llength {a b c d e f g h i j}] 10
assert pad-dict-1388 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1389 [expr {(1389 + 1) * (1389 + 2) / 2}] 966745
assert pad-str-1390 [string length "The quick brown fox #01390 jumped over the lazy dog"] 51
assert pad-int-1391 [expr {3138 * 2}] 6276
assert pad-list-1392 [llength {a b c d e f g h i j}] 10
assert pad-dict-1393 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1394 [expr {(1394 + 1) * (1394 + 2) / 2}] 973710
assert pad-str-1395 [string length "The quick brown fox #01395 jumped over the lazy dog"] 51
assert pad-int-1396 [expr {3293 * 2}] 6586
assert pad-list-1397 [llength {a b c d e f g h i j}] 10
assert pad-dict-1398 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1399 [expr {(1399 + 1) * (1399 + 2) / 2}] 980700
assert pad-str-1400 [string length "The quick brown fox #01400 jumped over the lazy dog"] 51
assert pad-int-1401 [expr {3448 * 2}] 6896
assert pad-list-1402 [llength {a b c d e f g h i j}] 10
assert pad-dict-1403 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1404 [expr {(1404 + 1) * (1404 + 2) / 2}] 987715
assert pad-str-1405 [string length "The quick brown fox #01405 jumped over the lazy dog"] 51
assert pad-int-1406 [expr {3603 * 2}] 7206
assert pad-list-1407 [llength {a b c d e f g h i j}] 10
assert pad-dict-1408 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1409 [expr {(1409 + 1) * (1409 + 2) / 2}] 994755
assert pad-str-1410 [string length "The quick brown fox #01410 jumped over the lazy dog"] 51
assert pad-int-1411 [expr {3758 * 2}] 7516
assert pad-list-1412 [llength {a b c d e f g h i j}] 10
assert pad-dict-1413 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1414 [expr {(1414 + 1) * (1414 + 2) / 2}] 1001820
assert pad-str-1415 [string length "The quick brown fox #01415 jumped over the lazy dog"] 51
assert pad-int-1416 [expr {3913 * 2}] 7826
assert pad-list-1417 [llength {a b c d e f g h i j}] 10
assert pad-dict-1418 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1419 [expr {(1419 + 1) * (1419 + 2) / 2}] 1008910
assert pad-str-1420 [string length "The quick brown fox #01420 jumped over the lazy dog"] 51
assert pad-int-1421 [expr {4068 * 2}] 8136
assert pad-list-1422 [llength {a b c d e f g h i j}] 10
assert pad-dict-1423 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1424 [expr {(1424 + 1) * (1424 + 2) / 2}] 1016025
assert pad-str-1425 [string length "The quick brown fox #01425 jumped over the lazy dog"] 51
assert pad-int-1426 [expr {4223 * 2}] 8446
assert pad-list-1427 [llength {a b c d e f g h i j}] 10
assert pad-dict-1428 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1429 [expr {(1429 + 1) * (1429 + 2) / 2}] 1023165
assert pad-str-1430 [string length "The quick brown fox #01430 jumped over the lazy dog"] 51
assert pad-int-1431 [expr {4378 * 2}] 8756
assert pad-list-1432 [llength {a b c d e f g h i j}] 10
assert pad-dict-1433 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1434 [expr {(1434 + 1) * (1434 + 2) / 2}] 1030330
assert pad-str-1435 [string length "The quick brown fox #01435 jumped over the lazy dog"] 51
assert pad-int-1436 [expr {4533 * 2}] 9066
assert pad-list-1437 [llength {a b c d e f g h i j}] 10
assert pad-dict-1438 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1439 [expr {(1439 + 1) * (1439 + 2) / 2}] 1037520
assert pad-str-1440 [string length "The quick brown fox #01440 jumped over the lazy dog"] 51
assert pad-int-1441 [expr {4688 * 2}] 9376
assert pad-list-1442 [llength {a b c d e f g h i j}] 10
assert pad-dict-1443 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1444 [expr {(1444 + 1) * (1444 + 2) / 2}] 1044735
assert pad-str-1445 [string length "The quick brown fox #01445 jumped over the lazy dog"] 51
assert pad-int-1446 [expr {4843 * 2}] 9686
assert pad-list-1447 [llength {a b c d e f g h i j}] 10
assert pad-dict-1448 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1449 [expr {(1449 + 1) * (1449 + 2) / 2}] 1051975
assert pad-str-1450 [string length "The quick brown fox #01450 jumped over the lazy dog"] 51
assert pad-int-1451 [expr {4998 * 2}] 9996
assert pad-list-1452 [llength {a b c d e f g h i j}] 10
assert pad-dict-1453 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1454 [expr {(1454 + 1) * (1454 + 2) / 2}] 1059240
assert pad-str-1455 [string length "The quick brown fox #01455 jumped over the lazy dog"] 51
assert pad-int-1456 [expr {5153 * 2}] 10306
assert pad-list-1457 [llength {a b c d e f g h i j}] 10
assert pad-dict-1458 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1459 [expr {(1459 + 1) * (1459 + 2) / 2}] 1066530
assert pad-str-1460 [string length "The quick brown fox #01460 jumped over the lazy dog"] 51
assert pad-int-1461 [expr {5308 * 2}] 10616
assert pad-list-1462 [llength {a b c d e f g h i j}] 10
assert pad-dict-1463 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1464 [expr {(1464 + 1) * (1464 + 2) / 2}] 1073845
assert pad-str-1465 [string length "The quick brown fox #01465 jumped over the lazy dog"] 51
assert pad-int-1466 [expr {5463 * 2}] 10926
assert pad-list-1467 [llength {a b c d e f g h i j}] 10
assert pad-dict-1468 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1469 [expr {(1469 + 1) * (1469 + 2) / 2}] 1081185
assert pad-str-1470 [string length "The quick brown fox #01470 jumped over the lazy dog"] 51
assert pad-int-1471 [expr {5618 * 2}] 11236
assert pad-list-1472 [llength {a b c d e f g h i j}] 10
assert pad-dict-1473 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1474 [expr {(1474 + 1) * (1474 + 2) / 2}] 1088550
assert pad-str-1475 [string length "The quick brown fox #01475 jumped over the lazy dog"] 51
assert pad-int-1476 [expr {5773 * 2}] 11546
assert pad-list-1477 [llength {a b c d e f g h i j}] 10
assert pad-dict-1478 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1479 [expr {(1479 + 1) * (1479 + 2) / 2}] 1095940
assert pad-str-1480 [string length "The quick brown fox #01480 jumped over the lazy dog"] 51
assert pad-int-1481 [expr {5928 * 2}] 11856
assert pad-list-1482 [llength {a b c d e f g h i j}] 10
assert pad-dict-1483 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1484 [expr {(1484 + 1) * (1484 + 2) / 2}] 1103355
assert pad-str-1485 [string length "The quick brown fox #01485 jumped over the lazy dog"] 51
assert pad-int-1486 [expr {6083 * 2}] 12166
assert pad-list-1487 [llength {a b c d e f g h i j}] 10
assert pad-dict-1488 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1489 [expr {(1489 + 1) * (1489 + 2) / 2}] 1110795
assert pad-str-1490 [string length "The quick brown fox #01490 jumped over the lazy dog"] 51
assert pad-int-1491 [expr {6238 * 2}] 12476
assert pad-list-1492 [llength {a b c d e f g h i j}] 10
assert pad-dict-1493 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1494 [expr {(1494 + 1) * (1494 + 2) / 2}] 1118260
assert pad-str-1495 [string length "The quick brown fox #01495 jumped over the lazy dog"] 51
assert pad-int-1496 [expr {6393 * 2}] 12786
assert pad-list-1497 [llength {a b c d e f g h i j}] 10
assert pad-dict-1498 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1499 [expr {(1499 + 1) * (1499 + 2) / 2}] 1125750
assert pad-str-1500 [string length "The quick brown fox #01500 jumped over the lazy dog"] 51
assert pad-int-1501 [expr {6548 * 2}] 13096
assert pad-list-1502 [llength {a b c d e f g h i j}] 10
assert pad-dict-1503 [dict size [dict create a 1 b 2 c 3 d 4 e 5]] 5
assert pad-expr-1504 [expr {(1504 + 1) * (1504 + 2) / 2}] 1133265
assert pad-str-1505 [string length "The quick brown fox #01505 jumped over the lazy dog"] 51
assert pad-int-1506 [expr {6703 * 2}] 13406

# ======================================================================
# EPILOGUE: Report results
# ======================================================================

set _total [expr {$::_pass + $::_fail}]
if {$::_fail == 0} {
    puts "TBCX_STRESS: ALL $_total TESTS PASSED"
} else {
    puts "TBCX_STRESS: $_pass/$_total passed, $::_fail FAILED"
    foreach e $::_errors {
        puts "  $e"
    }
    exit 1
}
