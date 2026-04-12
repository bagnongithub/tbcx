#!/usr/bin/env tclsh
# ============================================================================
# comprehensive.tcl
#
# Exhaustive Tcl test script designed to exercise EVERY language artifact
# relevant to tbcx bytecode serialization.  Compile with:
#
#     tbcx::save comprehensive.tcl out.tbcx
#     set result [tbcx::load out.tbcx]
#
# Returns a flat list of {tag value} pairs.  Expected result at bottom.
# ============================================================================

set ::results {}
proc assert {tag got exp} {
    if {$got ne $exp} {
        lappend ::results "FAIL:${tag}:got=${got}:exp=${exp}"
    } else {
        lappend ::results "ok:${tag}"
    }
}

# ============================================================================
# 1. SCALAR VARIABLES
# ============================================================================
set x 42
set y "hello world"
set z {braced string with $dollar and [brackets]}
assert var-int $x 42
assert var-str $y "hello world"
assert var-brace $z {braced string with $dollar and [brackets]}

# ============================================================================
# 2. EXPRESSIONS & ARITHMETIC
# ============================================================================
assert expr-add  [expr {10 + 20}]        30
assert expr-mul  [expr {6 * 7}]          42
assert expr-div  [expr {100 / 3}]        33
assert expr-mod  [expr {17 % 5}]         2
assert expr-pow  [expr {2 ** 10}]        1024
assert expr-neg  [expr {-42}]            -42
assert expr-dbl  [expr {3.14 * 2}]       6.28
assert expr-bool [expr {1 && 0}]         0
assert expr-cmp  [expr {10 > 5}]         1
assert expr-eq   [expr {"abc" eq "abc"}] 1
assert expr-ne   [expr {"abc" ne "xyz"}] 1
assert expr-tri  [expr {1 ? "yes" : "no"}] yes
assert expr-wide [expr {1000000000 * 1000000000}] 1000000000000000000
assert expr-bits [expr {0xFF & 0x0F}]    15
assert expr-shift [expr {1 << 10}]       1024

# ============================================================================
# 3. STRING OPERATIONS
# ============================================================================
assert str-len    [string length "hello"]            5
assert str-idx    [string index "abcdef" 2]          c
assert str-range  [string range "abcdef" 1 3]        bcd
assert str-upper  [string toupper "hello"]           HELLO
assert str-lower  [string tolower "WORLD"]           world
assert str-trim   [string trim "  hi  "]             hi
assert str-match  [string match "h*o" "hello"]       1
assert str-first  [string first "ll" "hello"]        2
assert str-last   [string last "l" "hello world"]    9
assert str-rev    [string reverse "abcde"]           edcba
assert str-rep    [string repeat "ab" 3]             ababab
assert str-map    [string map {a A e E} "abeced"]    AbEcEd
assert str-is-int [string is integer "42"]           1
assert str-is-alp [string is alpha "hello"]          1
assert str-cat    [string cat "foo" "bar" "baz"]     foobarbaz
assert str-replace [string replace "hello" 1 3 "XY"] hXYo

# ============================================================================
# 4. LIST OPERATIONS
# ============================================================================
set lst {a b c d e}
assert list-len   [llength $lst]                     5
assert list-idx   [lindex $lst 2]                    c
assert list-range [lrange $lst 1 3]                  {b c d}
assert list-sort  [lsort {3 1 4 1 5}]               {1 1 3 4 5}
assert list-rev   [lreverse {1 2 3}]                 {3 2 1}
assert list-srch  [lsearch $lst "c"]                 2
assert list-join  [join {a b c} "-"]                 a-b-c
assert list-split [split "a.b.c" "."]                {a b c}
assert list-map   [lmap x {1 2 3} {expr {$x * $x}}] {1 4 9}
assert list-assign [lassign {10 20 30} a b c]$a/$b/$c 10/20/30

set lst2 {1 2 3}
lappend lst2 4
assert list-append $lst2 {1 2 3 4}

set lst3 [lrepeat 3 "x"]
assert list-repeat $lst3 {x x x}

set lst4 [lreplace {a b c d} 1 2 X Y Z]
assert list-replace $lst4 {a X Y Z d}

set lst5 [linsert {a b c} 1 X]
assert list-insert $lst5 {a X b c}

# ============================================================================
# 5. DICT OPERATIONS
# ============================================================================
set d [dict create name Alice age 30 city Boston]
assert dict-get   [dict get $d name]                 Alice
assert dict-size  [dict size $d]                     3
assert dict-exists [dict exists $d age]              1
assert dict-keys  [lsort [dict keys $d]]             {age city name}
assert dict-vals  [lsort [dict values $d]]           {30 Alice Boston}

dict set d email "alice@test.com"
assert dict-set   [dict get $d email]                alice@test.com

dict unset d email
assert dict-unset [dict exists $d email]             0

dict incr d age 5
assert dict-incr  [dict get $d age]                  35

dict lappend d tags "vip"
dict lappend d tags "member"
assert dict-lappend [dict get $d tags]               {vip member}

set acc ""
dict for {k v} {x 1 y 2 z 3} {
    append acc "$k=$v "
}
assert dict-for [string trim $acc] "x=1 y=2 z=3"

dict with d {
    set name_upper [string toupper $name]
}
assert dict-with $name_upper ALICE

set d2 [dict create k val]
dict update d2 k K {
    set K "UPDATED"
}
assert dict-update [dict get $d2 k] UPDATED

dict map {k v} {a 1 b 2 c 3} {
    expr {$v * 10}
}
assert dict-map [dict map {k v} {a 1 b 2 c 3} {expr {$v * 10}}] {a 10 b 20 c 30}

# ============================================================================
# 6. ARRAY OPERATIONS
# ============================================================================
array set myarr {one 1 two 2 three 3}
assert arr-get    $myarr(two)                        2
assert arr-size   [array size myarr]                 3
assert arr-exists [array exists myarr]               1
assert arr-names  [lsort [array names myarr]]        {one three two}

set myarr(four) 4
assert arr-set    $myarr(four)                       4

array unset myarr three
assert arr-unset  [array size myarr]                 3

# ============================================================================
# 7. CONTROL FLOW — if/elseif/else
# ============================================================================
proc classify {n} {
    if {$n < 0} {
        return "negative"
    } elseif {$n == 0} {
        return "zero"
    } elseif {$n < 10} {
        return "small"
    } else {
        return "large"
    }
}
assert if-neg   [classify -5]  negative
assert if-zero  [classify 0]   zero
assert if-small [classify 7]   small
assert if-large [classify 42]  large

# ============================================================================
# 8. CONTROL FLOW — while
# ============================================================================
proc sum_while {n} {
    set s 0
    set i 1
    while {$i <= $n} {
        incr s $i
        incr i
    }
    return $s
}
assert while-sum [sum_while 10] 55

# ============================================================================
# 9. CONTROL FLOW — for
# ============================================================================
proc sum_for {n} {
    set s 0
    for {set i 1} {$i <= $n} {incr i} {
        incr s $i
    }
    return $s
}
assert for-sum [sum_for 10] 55

# ============================================================================
# 10. CONTROL FLOW — foreach
# ============================================================================
set total 0
foreach x {10 20 30 40} {
    incr total $x
}
assert foreach-sum $total 100

# Multi-variable foreach
set pairs {}
foreach {k v} {a 1 b 2 c 3} {
    lappend pairs "$k:$v"
}
assert foreach-multi [join $pairs ","] "a:1,b:2,c:3"

# Nested foreach
set matrix {}
foreach row {{1 2} {3 4} {5 6}} {
    set rowsum 0
    foreach val $row {
        incr rowsum $val
    }
    lappend matrix $rowsum
}
assert foreach-nested $matrix {3 7 11}

# ============================================================================
# 11. CONTROL FLOW — switch
# ============================================================================
proc day_type {day} {
    switch -exact -- $day {
        Mon - Tue - Wed - Thu - Fri { return "weekday" }
        Sat - Sun { return "weekend" }
        default { return "unknown" }
    }
}
assert switch-wd  [day_type Mon] weekday
assert switch-we  [day_type Sat] weekend
assert switch-unk [day_type X]   unknown

# Switch with -glob
proc match_glob {s} {
    switch -glob -- $s {
        *.tcl  { return "tcl" }
        *.txt  { return "text" }
        default { return "other" }
    }
}
assert switch-glob1 [match_glob "foo.tcl"] tcl
assert switch-glob2 [match_glob "bar.txt"] text
assert switch-glob3 [match_glob "baz.py"]  other

# ============================================================================
# 12. CONTROL FLOW — try/on/trap/finally
# ============================================================================
proc safe_div {a b} {
    try {
        return [expr {$a / $b}]
    } on error {msg} {
        return "error:$msg"
    }
}
assert try-ok  [safe_div 10 2] 5
assert try-err [string match "error:*zero*" [safe_div 10 0]] 1

# try + trap
proc trapped_div {a b} {
    try {
        expr {$a / $b}
    } trap {ARITH DIVZERO} {msg} {
        return "trapped:$msg"
    } on error {msg} {
        return "generic:$msg"
    }
}
assert trap-divz [string match "trapped:*" [trapped_div 1 0]] 1

# try + finally
proc with_finally {} {
    set log {}
    try {
        lappend log "body"
        error "boom"
    } on error {msg} {
        lappend log "handler:$msg"
    } finally {
        lappend log "finally"
    }
    return $log
}
assert try-finally [list {*}[with_finally]] [list body handler:boom finally]

# try + on ok
proc try_on_ok {} {
    try {
        expr {10 + 20}
    } on ok {result} {
        return "ok:$result"
    } on error {msg} {
        return "err:$msg"
    }
}
assert try-on-ok [try_on_ok] ok:30

# ============================================================================
# 13. CONTROL FLOW — catch
# ============================================================================
set code [catch {expr {10 / 0}} msg opts]
assert catch-code $code 1
assert catch-opts [dict get $opts -errorcode] {ARITH DIVZERO {divide by zero}}

set code2 [catch {expr {6 * 7}} result2]
assert catch-ok $code2 0
assert catch-val $result2 42

# ============================================================================
# 14. CONTROL FLOW — break/continue
# ============================================================================
set found -1
foreach x {10 20 30 40 50} {
    if {$x == 30} {
        set found $x
        break
    }
}
assert break-found $found 30

set evens {}
foreach x {1 2 3 4 5 6 7 8} {
    if {$x % 2 != 0} continue
    lappend evens $x
}
assert continue-evens $evens {2 4 6 8}

# ============================================================================
# 15. PROCS — simple, default args, args, recursive
# ============================================================================
proc greet {name} {
    return "Hello, $name!"
}
assert proc-simple [greet "World"] "Hello, World!"

proc greet2 {name {greeting "Hi"}} {
    return "$greeting, $name!"
}
assert proc-default1 [greet2 "Alice"]        "Hi, Alice!"
assert proc-default2 [greet2 "Alice" "Hey"]  "Hey, Alice!"

proc variadic {first args} {
    return "$first:[join $args ,]"
}
assert proc-args [variadic a b c d] "a:b,c,d"

proc factorial {n} {
    if {$n <= 1} { return 1 }
    return [expr {$n * [factorial [expr {$n - 1}]]}]
}
assert proc-recurse [factorial 10] 3628800

# ============================================================================
# 16. NAMESPACES
# ============================================================================
namespace eval ::mylib {
    variable version 2.0
    variable counter 0

    proc init {} {
        variable counter
        set counter 0
    }

    proc increment {{by 1}} {
        variable counter
        incr counter $by
        return $counter
    }

    proc get_version {} {
        variable version
        return $version
    }

    namespace eval ::mylib::sub {
        proc deep {} {
            return "deep-[::mylib::get_version]"
        }
    }
}

::mylib::init
::mylib::increment
::mylib::increment 5
assert ns-counter [::mylib::increment 0]    6
assert ns-version [::mylib::get_version]     2.0
assert ns-nested  [::mylib::sub::deep]       deep-2.0

# Namespace import/export
namespace eval ::exportns {
    namespace export greet
    proc greet {who} { return "hey $who" }
    proc private {} { return "secret" }
}
namespace eval ::importns {
    namespace import ::exportns::greet
}
assert ns-import [::importns::greet "you"] "hey you"

# ============================================================================
# 17. UPVAR & UPLEVEL
# ============================================================================
proc set_caller_var {name value} {
    upvar 1 $name var
    set var $value
}
set_caller_var myvar 99
assert upvar-set $myvar 99

proc eval_in_caller {script} {
    uplevel 1 $script
}
set uplevel_x 0
eval_in_caller {set uplevel_x 42}
assert uplevel-exec $uplevel_x 42

# ============================================================================
# 18. LAMBDA / APPLY
# ============================================================================
assert lambda-basic [apply {{x} {expr {$x + 1}}} 41] 42
assert lambda-multi [apply {{a b} {expr {$a * $b}}} 6 7] 42
assert lambda-default [apply {{x {y 10}} {expr {$x + $y}}} 5] 15

# Lambda with namespace
namespace eval ::lamns {
    variable factor 10
}
assert lambda-ns [apply {{x} {expr {$x * $::lamns::factor}} ::lamns} 4] 40

# Lambda stored in variable
set adder [list {a b} {expr {$a + $b}}]
assert lambda-var [apply $adder 20 22] 42

# Higher-order with lambda
proc mymap {fn lst} {
    set res {}
    foreach x $lst {
        lappend res [apply $fn $x]
    }
    return $res
}
assert lambda-ho [mymap {x {expr {$x * $x}}} {1 2 3 4 5}] {1 4 9 16 25}

proc myfilter {fn lst} {
    set res {}
    foreach x $lst {
        if {[apply $fn $x]} { lappend res $x }
    }
    return $res
}
assert lambda-filter [myfilter {x {expr {$x > 3}}} {1 2 3 4 5 6}] {4 5 6}

proc myreduce {fn init lst} {
    set acc $init
    foreach x $lst {
        set acc [apply $fn $acc $x]
    }
    return $acc
}
assert lambda-reduce [myreduce {{a b} {expr {$a + $b}}} 0 {1 2 3 4 5}] 15

# ============================================================================
# 19. CLOSURES VIA FORMAT / LIST
# ============================================================================
proc make_adder {n} {
    return [list {x} [format {expr {$x + %d}} $n]]
}
set add5  [make_adder 5]
set add10 [make_adder 10]
assert closure-5  [apply $add5 3]  8
assert closure-10 [apply $add10 3] 13

proc make_multiplier {n} {
    return [list {x} [format {expr {$x * %d}} $n]]
}
set mul3 [make_multiplier 3]
assert closure-mul [apply $mul3 7] 21

# ============================================================================
# 20. OO — BASIC CLASS
# ============================================================================
oo::class create Animal {
    variable name sound

    constructor {n s} {
        set name $n
        set sound $s
    }

    method speak {} {
        return "$name says $sound"
    }

    method get_name {} {
        return $name
    }
}

set dog [Animal new "Rex" "woof"]
set cat [Animal new "Whiskers" "meow"]
assert oo-speak1 [$dog speak] "Rex says woof"
assert oo-speak2 [$cat speak] "Whiskers says meow"
assert oo-name   [$dog get_name] Rex

# ============================================================================
# 21. OO — INHERITANCE
# ============================================================================
oo::class create Shape {
    variable type

    constructor {t} {
        set type $t
    }

    method type {} {
        return $type
    }

    method area {} {
        return 0
    }
}

oo::class create Rectangle {
    superclass Shape
    variable width height

    constructor {w h} {
        next "rectangle"
        set width $w
        set height $h
    }

    method area {} {
        return [expr {$width * $height}]
    }

    method perimeter {} {
        return [expr {2 * ($width + $height)}]
    }
}

oo::class create Square {
    superclass Rectangle

    constructor {side} {
        next $side $side
    }
}

set r [Rectangle new 3 4]
set s [Square new 5]
assert oo-rect-area [$r area]       12
assert oo-rect-peri [$r perimeter]  14
assert oo-rect-type [$r type]       rectangle
assert oo-sq-area   [$s area]       25
assert oo-sq-peri   [$s perimeter]  20

# ============================================================================
# 22. OO — MIXINS
# ============================================================================
oo::class create Printable {
    method to_string {} {
        return "[info object class [self]]"
    }
}

oo::class create Loggable {
    variable log

    constructor {} {
        set log {}
        next
    }

    method add_log {msg} {
        lappend log $msg
    }

    method get_log {} {
        return $log
    }
}

oo::class create Widget {
    mixin Printable
    variable id

    constructor {name} {
        set id $name
    }

    method id {} { return $id }
}

set w [Widget new "btn1"]
assert oo-mixin-id   [$w id]         btn1
assert oo-mixin-str  [string match "::Widget" [$w to_string]] 1

# ============================================================================
# 23. OO — METACLASS (self method + inheritance)
# ============================================================================
oo::class create Factory {
    variable label

    constructor {{l "default"}} {
        set label $l
    }

    method label {} { return $label }

    self method make {args} {
        set obj [my new {*}$args]
        return $obj
    }
}

set p1 [Factory make "direct"]
assert oo-meta-direct [$p1 label] direct

# Self method is per-object on the Factory class — NOT inherited by Product.
# Product uses direct instantiation instead.
oo::class create Product {
    superclass Factory
    variable extra

    constructor {l {e ""}} {
        next $l
        set extra $e
    }

    method extra {} { return $extra }
}

set p2 [Product new "inherited" "bonus"]
assert oo-meta-inherit [$p2 label] inherited
assert oo-meta-extra   [$p2 extra] bonus

# ============================================================================
# 24. OO — DESTRUCTOR
# ============================================================================
oo::class create Tracked {
    variable id
    constructor {i} { set id $i }
    destructor {
        # Destructor body — no observable side-effect here,
        # but tests that destructors compile correctly
    }
    method id {} { return $id }
}
set t [Tracked new 42]
assert oo-dtor-id [$t id] 42
$t destroy

# ============================================================================
# 25. OO — FORWARD & RENAME
# ============================================================================
oo::class create Wrapper {
    variable data

    constructor {d} { set data $d }

    method get {} { return $data }

    forward length string length
}

set wr [Wrapper new "hello"]
assert oo-forward [$wr length "testing"] 7
assert oo-get     [$wr get] hello

# ============================================================================
# 26. FORMAT & SCAN
# ============================================================================
assert fmt-int   [format "%05d" 42]          00042
assert fmt-hex   [format "0x%X" 255]         0xFF
assert fmt-float [format "%.2f" 3.14159]     3.14
assert fmt-str   [format "%-10s|" "hi"]      "hi        |"
assert fmt-multi [format "%s=%d" "x" 42]     x=42

set scanned [scan "42 3.14 hello" "%d %f %s"]
assert scan-int [lindex $scanned 0] 42
assert scan-str [lindex $scanned 2] hello

# ============================================================================
# 27. REGULAR EXPRESSIONS
# ============================================================================
assert regexp-match  [regexp {^\d+$} "12345"]            1
assert regexp-nomatch [regexp {^\d+$} "12x45"]           0
assert regexp-sub    [regsub -all {[aeiou]} "hello" "*"] "h*ll*"

regexp {(\d+)-(\d+)-(\d+)} "2024-03-25" -> y m d
assert regexp-cap-y $y 2024
assert regexp-cap-m $m 03
assert regexp-cap-d $d 25

# ============================================================================
# 28. BINARY DATA
# ============================================================================
set packed [binary format "ii" 12345 67890]
binary scan $packed "ii" a b
assert binary-scan-a $a 12345
assert binary-scan-b $b 67890

set bytes [binary format "c*" {72 101 108 108 111}]
assert binary-str $bytes "Hello"

# ============================================================================
# 29. MATH FUNCTIONS
# ============================================================================
assert math-abs   [expr {abs(-42)}]          42
assert math-max   [expr {max(10, 20, 5)}]    20
assert math-min   [expr {min(10, 20, 5)}]    5
assert math-round [expr {round(3.7)}]        4
assert math-int   [expr {int(3.9)}]          3
assert math-double [expr {double(42)}]       42.0
assert math-sqrt  [format "%.4f" [expr {sqrt(2.0)}]] 1.4142
assert math-rand  [expr {int(rand() * 1000) >= 0}]   1

# ============================================================================
# 30. INCR / APPEND / LAPPEND
# ============================================================================
set counter 0
incr counter
incr counter 5
incr counter -2
assert incr-val $counter 4

set buf ""
append buf "hello"
append buf " " "world"
assert append-val $buf "hello world"

set collected {}
lappend collected a
lappend collected b c
assert lappend-val $collected {a b c}

# ============================================================================
# 31. EVAL & SUBST
# ============================================================================
set eval_result [eval {expr {6 * 7}}]
assert eval-basic $eval_result 42

set tmpl {The answer is $x}
set x 42
assert subst-basic [subst $tmpl] "The answer is 42"

set tmpl2 {[expr {2 + 3}] is five}
assert subst-cmd [subst $tmpl2] "5 is five"

# ============================================================================
# 32. TIME & TIMERATE (functional, not timing)
# ============================================================================
set time_result [time {set _t [expr {1+1}]} 1]
assert time-runs [string is integer [lindex $time_result 0]] 1

# ============================================================================
# 33. MULTI-INTERP
# ============================================================================
set child [interp create]
set child_result [interp eval $child {
    set x 21
    expr {$x * 2}
}]
interp delete $child
assert interp-eval $child_result 42

# ============================================================================
# 34. ERROR HANDLING PATTERNS
# ============================================================================
proc must_be_positive {n} {
    if {$n <= 0} {
        error "must be positive" "" {MYAPP BADVAL}
    }
    return $n
}

set errcode ""
try {
    must_be_positive -1
} on error {msg opts} {
    set errcode [dict get $opts -errorcode]
}
assert error-custom $errcode {MYAPP BADVAL}

# throw
set throwcode ""
try {
    throw {MY THROW CODE} "thrown error"
} on error {msg opts} {
    set throwcode [dict get $opts -errorcode]
}
assert throw-code $throwcode {MY THROW CODE}

# ============================================================================
# 35. TAILCALL
# ============================================================================
proc tc_sum {n acc} {
    if {$n <= 0} { return $acc }
    tailcall tc_sum [expr {$n - 1}] [expr {$acc + $n}]
}
assert tailcall-sum [tc_sum 100 0] 5050

# ============================================================================
# 36. COROUTINES
# ============================================================================
proc counter_coro {} {
    set i 0
    while 1 {
        yield $i
        incr i
    }
}
coroutine mycounter counter_coro
set c0 [mycounter]
set c1 [mycounter]
set c2 [mycounter]
rename mycounter ""
assert coro-0 $c0 1
assert coro-1 $c1 2
assert coro-2 $c2 3

# Generator pattern
proc range_gen {from to} {
    for {set i $from} {$i <= $to} {incr i} {
        yield $i
    }
}
coroutine myrange range_gen 10 13
set rng {}
while {![catch {myrange} val]} {
    if {$val ne ""} { lappend rng $val }
}
assert coro-range $rng {11 12 13}

# ============================================================================
# 37. RENAME & INFO
# ============================================================================
proc orig_cmd {} { return "original" }
rename orig_cmd renamed_cmd
assert rename-call [renamed_cmd] original
rename renamed_cmd ""

assert info-exists [info exists x] 1
assert info-procs  [expr {"greet" in [info procs]}] 1
assert info-level  [info level] 0
assert info-cmdcnt [expr {[info cmdcount] > 0}] 1

# ============================================================================
# 38. ENCODING
# ============================================================================
assert enc-utf8   [encoding system]    utf-8
assert enc-conv   [encoding convertto utf-8 "hello"] hello

# ============================================================================
# 39. COMPLEX DATA STRUCTURES
# ============================================================================

# Nested dicts
set people [dict create]
dict set people alice name "Alice"
dict set people alice age 30
dict set people alice hobbies {reading coding}
dict set people bob name "Bob"
dict set people bob age 25
dict set people bob hobbies {gaming music}

assert nested-dict-name [dict get $people alice name] Alice
assert nested-dict-age  [dict get $people bob age] 25
assert nested-dict-hob  [lindex [dict get $people alice hobbies] 1] coding

# List of dicts
set records [list \
    [dict create id 1 val "first"] \
    [dict create id 2 val "second"] \
    [dict create id 3 val "third"]]
assert lod-get [dict get [lindex $records 1] val] second

# ============================================================================
# 40. PACKAGE-LIKE INITIALIZATION PATTERN
# ============================================================================
namespace eval ::mypkg {
    variable version "1.0.0"
    variable initialized 0

    proc init {} {
        variable initialized
        if {$initialized} return
        set initialized 1
    }

    proc version {} {
        variable version
        return $version
    }

    proc status {} {
        variable initialized
        return $initialized
    }

    proc compute {x y} {
        return [expr {$x * $y + 1}]
    }
}

::mypkg::init
assert pkg-version [::mypkg::version]     "1.0.0"
assert pkg-status  [::mypkg::status]       1
assert pkg-compute [::mypkg::compute 6 7]  43

# ============================================================================
# 41. DYNAMIC PROC CREATION (strcat body)
# ============================================================================
proc make_const_proc {name value} {
    proc $name {} "return $value"
}
make_const_proc get_pi 3.14159
make_const_proc get_e  2.71828
assert dynproc-pi [get_pi]   3.14159
assert dynproc-e  [get_e]    2.71828

# ============================================================================
# 42. OO — COMPLEX CLASS HIERARCHY
# ============================================================================
namespace eval ::app {
    oo::class create Base {
        variable id

        constructor {{i ""}} {
            if {$i eq ""} {
                set id [format "obj%04d" [clock clicks]]
            } else {
                set id $i
            }
        }

        method id {} { return $id }
    }

    oo::class create Service {
        superclass Base
        variable running

        constructor {name} {
            next $name
            set running 0
        }

        method start {} {
            set running 1
            return "started:[my id]"
        }

        method stop {} {
            set running 0
            return "stopped:[my id]"
        }

        method running? {} { return $running }
    }
}

set svc [::app::Service new "svc1"]
assert oo-hier-start [$svc start]    "started:svc1"
assert oo-hier-run   [$svc running?] 1
assert oo-hier-stop  [$svc stop]     "stopped:svc1"
assert oo-hier-run2  [$svc running?] 0

# ============================================================================
# 43. COMPLEX DISPATCH TABLE
# ============================================================================
proc op_add  {a b} { expr {$a + $b} }
proc op_sub  {a b} { expr {$a - $b} }
proc op_mul  {a b} { expr {$a * $b} }
proc op_div  {a b} { expr {$a / $b} }

proc calculate {op a b} {
    op_$op $a $b
}
assert dispatch-add [calculate add 10 3] 13
assert dispatch-sub [calculate sub 10 3] 7
assert dispatch-mul [calculate mul 10 3] 30

# ============================================================================
# 44. ENSEMBLE COMMANDS
# ============================================================================
namespace eval ::calc {
    namespace export add sub
    namespace ensemble create

    proc add {a b} { expr {$a + $b} }
    proc sub {a b} { expr {$a - $b} }
}
assert ensemble-add [calc add 10 5] 15
assert ensemble-sub [calc sub 10 5] 5

# ============================================================================
# 45. TRACE (variable write trace)
# ============================================================================
set trace_log {}
proc trace_handler {name1 name2 op} {
    upvar 1 $name1 v
    lappend ::trace_log "$op:$v"
}
set traced_var 0
trace add variable traced_var write trace_handler
set traced_var 10
set traced_var 20
trace remove variable traced_var write trace_handler
assert trace-log $trace_log {write:10 write:20}

# ============================================================================
# 46. OO — DEEP INHERITANCE CHAIN (4 levels)
# ============================================================================
oo::class create Vehicle {
    variable type speed
    constructor {t} { set type $t; set speed 0 }
    method type {} { return $type }
    method speed {} { return $speed }
    method accelerate {delta} { incr speed $delta; return $speed }
}

oo::class create Car {
    superclass Vehicle
    variable doors
    constructor {d} { next "car"; set doors $d }
    method doors {} { return $doors }
}

oo::class create ElectricCar {
    superclass Car
    variable battery
    constructor {d b} { next $d; set battery $b }
    method battery {} { return $battery }
    method describe {} {
        return "[my type]-[my doors]d-[my battery]kWh"
    }
}

oo::class create SportEV {
    superclass ElectricCar
    variable mode
    constructor {d b} { next $d $b; set mode "normal" }
    method sport {} { set mode "sport"; return $mode }
    method mode {} { return $mode }
    method full_desc {} {
        return "[my describe]-[my mode]"
    }
}

set ev [SportEV new 4 100]
assert deep-type    [$ev type]        car
assert deep-doors   [$ev doors]       4
assert deep-batt    [$ev battery]     100
assert deep-desc    [$ev describe]    car-4d-100kWh
assert deep-accel   [$ev accelerate 60] 60
assert deep-speed   [$ev speed]       60
assert deep-sport   [$ev sport]       sport
assert deep-full    [$ev full_desc]   car-4d-100kWh-sport

# ============================================================================
# 47. OO — MULTIPLE MIXINS & INTERACTION
# ============================================================================
oo::class create Serializable {
    method serialize {} {
        set pairs {}
        foreach v [info object variables [self]] {
            my variable $v
            lappend pairs $v [set $v]
        }
        return $pairs
    }
}

oo::class create Comparable {
    method equals {other} {
        return [expr {[my key] eq [$other key]}]
    }
}

oo::class create Identifiable {
    method identity {} {
        return "[info object class [self]]#[my key]"
    }
}

oo::class create Entity {
    mixin Serializable Comparable Identifiable
    variable name value

    constructor {n v} {
        set name $n
        set value $v
    }

    method key {} { return $name }
    method value {} { return $value }
}

set e1 [Entity new "alpha" 100]
set e2 [Entity new "alpha" 200]
set e3 [Entity new "beta" 300]
assert mixin-multi-key   [$e1 key]            alpha
assert mixin-multi-val   [$e1 value]          100
assert mixin-multi-eq    [$e1 equals $e2]     1
assert mixin-multi-neq   [$e1 equals $e3]     0
assert mixin-multi-id    [string match "::Entity#alpha" [$e1 identity]] 1

# ============================================================================
# 48. OO — VARIABLE SCOPING & MY VARIABLE
# ============================================================================
oo::class create Counter {
    variable count step

    constructor {{s 1}} {
        set count 0
        set step $s
    }

    method incr {} {
        incr count $step
        return $count
    }

    method reset {} {
        set count 0
    }

    method value {} { return $count }

    method add_to {other_counter} {
        # Access another object's method
        return [expr {$count + [$other_counter value]}]
    }
}

set c1 [Counter new]
set c2 [Counter new 5]
$c1 incr; $c1 incr; $c1 incr
$c2 incr; $c2 incr
assert varscope-c1    [$c1 value]   3
assert varscope-c2    [$c2 value]   10
assert varscope-add   [$c1 add_to $c2] 13
$c1 reset
assert varscope-reset [$c1 value]   0

# ============================================================================
# 49. OO — METHOD OVERRIDE & NEXT CHAIN
# ============================================================================
oo::class create Logger {
    variable entries

    constructor {} { set entries {} }

    method log {msg} {
        lappend entries "LOG:$msg"
    }

    method entries {} { return $entries }
}

oo::class create TimedLogger {
    superclass Logger
    variable timestamps

    constructor {} {
        next
        set timestamps {}
    }

    method log {msg} {
        lappend timestamps "T"
        next "timed-$msg"
    }

    method stamp_count {} { return [llength $timestamps] }
}

oo::class create FilteredLogger {
    superclass TimedLogger

    method log {msg} {
        if {$msg ne "skip"} {
            next $msg
        }
    }
}

set fl [FilteredLogger new]
$fl log "hello"
$fl log "skip"
$fl log "world"
assert next-chain-entries [$fl entries]     {LOG:timed-hello LOG:timed-world}
assert next-chain-stamps  [$fl stamp_count] 2

# ============================================================================
# 50. OO — FORWARD DELEGATION
# ============================================================================
oo::class create StringWrapper {
    variable data

    constructor {s} { set data $s }

    # Forward methods to string commands
    forward length ::tcl::string::length
    forward toupper ::tcl::string::toupper
    forward tolower ::tcl::string::tolower
    forward reverse ::tcl::string::reverse

    method get {} { return $data }
}

set sw [StringWrapper new "Hello"]
assert fwd-get     [$sw get]              Hello
assert fwd-len     [$sw length "test"]    4
assert fwd-upper   [$sw toupper "abc"]    ABC
assert fwd-rev     [$sw reverse "abcd"]   dcba

# ============================================================================
# 51. OO — FILTER
# ============================================================================
set ::filter_log {}
oo::class create Audited {
    filter audit_filter

    method audit_filter {args} {
        lappend ::filter_log "call"
        return [next {*}$args]
    }

    method action_a {} { return "A" }
    method action_b {} { return "B" }
}

set au [Audited new]
set ::filter_log {}
$au action_a
$au action_b
$au action_a
assert filter-count [llength $::filter_log] 3
assert filter-val [$au action_b] B

# ============================================================================
# 52. OO — SELF INTROSPECTION
# ============================================================================
oo::class create Reflective {
    variable data

    constructor {d} { set data $d }

    method class_name {} {
        return [info object class [self]]
    }

    method has_method {name} {
        return [expr {$name in [info object methods [self] -all]}]
    }

    method obj_vars {} {
        return [lsort [info object variables [self]]]
    }
}

set rf [Reflective new 42]
assert reflect-class [$rf class_name]           ::Reflective
assert reflect-has1  [$rf has_method class_name] 1
assert reflect-has2  [$rf has_method nonexist]   0

# ============================================================================
# 53. OO — UNEXPORT / EXPORT
# ============================================================================
oo::class create Access {
    method public_method {} { return "public" }
    method helper {} { return "helper-result" }

    # Call private helper from public method
    method use_helper {} {
        return [my helper]
    }

    unexport helper
}

set acc [Access new]
assert export-pub  [$acc public_method]   public
assert export-priv [$acc use_helper]      helper-result
# Calling unexported method directly should fail
assert export-nodir [catch {$acc helper}] 1

# ============================================================================
# 54. OO — ABSTRACT BASE / INTERFACE PATTERN
# ============================================================================
oo::class create Renderable {
    method render {} {
        error "subclass must implement render"
    }

    method display {} {
        return ">> [my render] <<"
    }
}

oo::class create TextWidget {
    superclass Renderable
    variable text

    constructor {t} { set text $t }

    method render {} {
        return "TEXT:$text"
    }
}

oo::class create ButtonWidget {
    superclass Renderable
    variable label

    constructor {l} { set label $l }

    method render {} {
        return "BTN:$label"
    }
}

set tw [TextWidget new "hello"]
set bw [ButtonWidget new "OK"]
assert abstract-text   [$tw display] ">> TEXT:hello <<"
assert abstract-btn    [$bw display] ">> BTN:OK <<"
assert abstract-err    [catch {[Renderable new] render}] 1

# ============================================================================
# 55. LAMBDA — ADVANCED PATTERNS
# ============================================================================

# Nested apply
assert lambda-nested [apply {{x} {
    apply {{y} {expr {$y * $y}}} [expr {$x + 1}]
}} 4] 25

# Lambda with args (varargs)
assert lambda-args [apply {{first args} {
    return "$first:[llength $args]"
}} "x" "a" "b" "c"] "x:3"

# Lambda composition
proc compose {f g} {
    return [list x [format {apply {%s} [apply {%s} $x]} $f $g]]
}
set double [list x {expr {$x * 2}}]
set add3   [list x {expr {$x + 3}}]
set composed [compose $double $add3]
assert lambda-compose [apply $composed 5] 16

# Lambda in lsort
set data {3 1 4 1 5 9 2 6}
assert lambda-sort [lsort -command {apply {{a b} {expr {$a - $b}}}} $data] {1 1 2 3 4 5 6 9}

# Lambda as dict transform
proc dict_map_values {fn d} {
    set result {}
    dict for {k v} $d {
        dict set result $k [apply $fn $v]
    }
    return $result
}
set prices {apple 1.50 banana 0.75 cherry 2.00}
set doubled [dict_map_values {{v} {expr {$v * 2}}} $prices]
assert lambda-dictmap [dict get $doubled banana] 1.5

# Lambda pipeline (fold-left)
proc pipeline {val args} {
    foreach fn $args {
        set val [apply $fn $val]
    }
    return $val
}
assert lambda-pipe [pipeline 3 \
    {{x} {expr {$x * 2}}} \
    {{x} {expr {$x + 10}}} \
    {{x} {expr {$x * $x}}}] 256

# ============================================================================
# 56. COROUTINES — ADVANCED PATTERNS
# ============================================================================

# Coroutine with arguments passed via yield
proc accumulator {} {
    set total 0
    while 1 {
        set val [yield $total]
        incr total $val
    }
}
coroutine accum accumulator
accum 0   ;# creation consumed first yield; resume with 0 (no-op incr)
accum 10
accum 20
assert coro-accum [accum 5] 35
rename accum ""

# Coroutine-based state machine
proc state_machine {} {
    set state "idle"
    while 1 {
        set event [yield $state]
        switch -exact -- $state {
            idle {
                if {$event eq "start"} { set state "running" }
            }
            running {
                if {$event eq "pause"} { set state "paused" }
                if {$event eq "stop"}  { set state "idle" }
            }
            paused {
                if {$event eq "resume"} { set state "running" }
                if {$event eq "stop"}   { set state "idle" }
            }
        }
    }
}
coroutine sm state_machine
assert coro-sm1 [sm start]   running
assert coro-sm2 [sm pause]   paused
assert coro-sm3 [sm resume]  running
assert coro-sm4 [sm stop]    idle
rename sm ""

# Fibonacci coroutine
proc fib_gen {} {
    set a 0
    set b 1
    while 1 {
        yield $a
        set tmp $b
        set b [expr {$a + $b}]
        set a $tmp
    }
}
coroutine fib fib_gen
set fibs {}
for {set _i 0} {$_i < 7} {incr _i} {
    lappend fibs [fib]
}
rename fib ""
assert coro-fib $fibs {1 1 2 3 5 8 13}

# Multiple concurrent coroutines
proc ticker {prefix} {
    set n 0
    while 1 {
        yield "$prefix$n"
        incr n
    }
}
coroutine tickA ticker A
coroutine tickB ticker B
set interleaved {}
for {set _i 0} {$_i < 3} {incr _i} {
    lappend interleaved [tickA]
    lappend interleaved [tickB]
}
rename tickA ""
rename tickB ""
assert coro-interleave $interleaved {A1 B1 A2 B2 A3 B3}

# ============================================================================
# 57. TCL 9.1 — lseq, lpop, ledit, lremove
# ============================================================================
# lseq — arithmetic sequence generation
assert lseq-basic [lseq 5]                        {0 1 2 3 4}
assert lseq-range [lseq 2 to 6]                   {2 3 4 5 6}
assert lseq-step  [lseq 0 to 10 by 2]             {0 2 4 6 8 10}
assert lseq-count [llength [lseq 100]]             100

# lpop — pop element from list
set lpoplist {a b c d e}
set popped [lpop lpoplist end]
assert lpop-val  $popped     e
assert lpop-rest $lpoplist   {a b c d}
set popped2 [lpop lpoplist 1]
assert lpop-mid  $popped2    b
assert lpop-rem  $lpoplist   {a c d}

# ledit — edit list in place
set editlist {a b c d e}
ledit editlist 1 2 X Y Z
assert ledit-val $editlist {a X Y Z d e}

# lremove — remove indices from list
set rmlist {a b c d e f}
assert lremove-val [lremove $rmlist 1 3 5] {a c e}

# ============================================================================
# 58. DICT — ADVANCED OPERATIONS
# ============================================================================
# dict filter
set inventory {apple 5 banana 12 cherry 3 date 8}
set plenty [dict filter $inventory script {k v} {expr {$v >= 5}}]
assert dict-filter-keys [lsort [dict keys $plenty]] {apple banana date}

# dict merge
set d1 [dict create a 1 b 2]
set d2 [dict create b 3 c 4]
set merged [dict merge $d1 $d2]
assert dict-merge-b [dict get $merged b]    3
assert dict-merge-c [dict get $merged c]    4
assert dict-merge-a [dict get $merged a]    1

# dict create
set dc [dict create name "test" version 2 active true]
assert dict-create-v [dict get $dc version] 2
assert dict-create-n [dict get $dc name]    test

# Nested dict manipulation
set config [dict create]
dict set config server host "localhost"
dict set config server port 8080
dict set config db name "mydb"
dict set config db pool 10
assert dict-nested-h [dict get $config server host] localhost
assert dict-nested-p [dict get $config server port] 8080
assert dict-nested-d [dict get $config db name]     mydb

# ============================================================================
# 59. STRING — ADVANCED OPERATIONS
# ============================================================================
# string is
assert stris-int    [string is integer "42"]     1
assert stris-dbl    [string is double "3.14"]    1
assert stris-alpha  [string is alpha "hello"]    1
assert stris-noint  [string is integer "abc"]    0

# string map with multiple pairs
set template "Hello NAME, welcome to PLACE!"
assert strmap-multi [string map {NAME Alice PLACE Wonderland} $template] \
    "Hello Alice, welcome to Wonderland!"

# string repeat
assert strrep-val [string repeat "ab" 4] "abababab"

# string reverse
assert strrev-val [string reverse "Hello"] "olleH"

# string cat
assert strcat-val [string cat "foo" "bar" "baz"] "foobarbaz"

# ============================================================================
# 60. CONTROL FLOW — ADVANCED PATTERNS
# ============================================================================
# Nested try/catch
proc nested_try {} {
    try {
        try {
            error "inner"
        } on error {msg} {
            return "caught:$msg"
        }
    } on error {msg} {
        return "outer:$msg"
    }
}
assert nested-try [nested_try] caught:inner

# Multiple on handlers — use -level 0 to directly set the return code
# (default -level 1 always produces TCL_RETURN inside try body)
proc multi_handler {code} {
    try {
        return -code $code -level 0 "value"
    } on ok {v} {
        return "ok:$v"
    } on error {v} {
        return "err:$v"
    } on return {v} {
        return "ret:$v"
    }
}
assert multi-on-ok  [multi_handler 0] ok:value
assert multi-on-err [multi_handler 1] err:value
assert multi-on-ret [multi_handler 2] ret:value

# for/break with result
proc find_first {lst pred} {
    foreach item $lst {
        if {[apply $pred $item]} {
            return $item
        }
    }
    return ""
}
assert find-first [find_first {3 7 2 9 4} {{x} {expr {$x > 5}}}] 7

# ============================================================================
# 61. PROC — ADVANCED PATTERNS
# ============================================================================
# Proc with defaults and args
proc fmt_msg {prefix {sep ": "} args} {
    return "$prefix$sep[join $args ,]"
}
assert proc-mixed1 [fmt_msg "LOG" ": " a b c]  "LOG: a,b,c"
assert proc-mixed2 [fmt_msg "ERR" " - " x]     "ERR - x"

# Recursive with accumulator
proc flatten {lst} {
    set acc {}
    foreach item $lst {
        if {[llength $item] > 1} {
            foreach sub [flatten $item] { lappend acc $sub }
        } else {
            lappend acc $item
        }
    }
    return $acc
}
assert proc-flatten [flatten {1 {2 3} {4 {5 6}} 7}] {1 2 3 4 5 6 7}

# ============================================================================
# 62. OO — OBJECT COPY & DESTROY
# ============================================================================
oo::class create Copyable {
    variable val
    constructor {v} { set val $v }
    method val {} { return $val }
    method set_val {v} { set val $v }
}

set orig [Copyable new 42]
set copy [oo::copy $orig]
$copy set_val 99
assert oo-copy-orig [$orig val] 42
assert oo-copy-copy [$copy val] 99
$orig destroy
# copy should still work after original is destroyed
assert oo-copy-indep [$copy val] 99
$copy destroy

# ============================================================================
# 63. OO — CLASS-LEVEL SELF METHOD (non-inherited)
# ============================================================================
oo::class create Registry {
    variable name

    constructor {n} { set name $n }
    method name {} { return $name }

    self method create_named {n} {
        set obj [my new $n]
        return $obj
    }

    self method class_info {} {
        return "Registry-class"
    }
}

set r1 [Registry create_named "item1"]
assert self-meth-name [$r1 name] item1
assert self-meth-info [Registry class_info] Registry-class

# ============================================================================
# 64. NAMESPACE — ADVANCED PATTERNS
# ============================================================================
namespace eval ::testns {
    variable counter 0

    proc bump {} {
        variable counter
        incr counter
    }

    proc get {} {
        variable counter
        return $counter
    }

    namespace eval inner {
        proc greet {} {
            return "inner-hello"
        }
    }

    namespace export bump get
}

namespace eval ::consumer {
    namespace import ::testns::*
}

::consumer::bump
::consumer::bump
::consumer::bump
assert ns-import-val [::consumer::get] 3
assert ns-inner [::testns::inner::greet] inner-hello

# Namespace ensemble (manual)
namespace eval ::mymath {
    proc add {a b} { expr {$a + $b} }
    proc mul {a b} { expr {$a * $b} }
    namespace export add mul
    namespace ensemble create
}
assert ns-ens-add [mymath add 3 4] 7
assert ns-ens-mul [mymath mul 3 4] 12

# ============================================================================
# 65. ADVANCED FOREACH PATTERNS
# ============================================================================
# foreach with multiple variables
set keys {}; set vals {}
foreach {k v} {a 1 b 2 c 3} {
    lappend keys $k
    lappend vals $v
}
assert foreach-multi-k $keys {a b c}
assert foreach-multi-v $vals {1 2 3}

# foreach with multiple lists
set zipped {}
foreach x {1 2 3} y {a b c} {
    lappend zipped "$x-$y"
}
assert foreach-zip $zipped {1-a 2-b 3-c}

# Nested foreach
set combos {}
foreach x {A B} {
    foreach y {1 2} {
        lappend combos "$x$y"
    }
}
assert foreach-nested $combos {A1 A2 B1 B2}

# ============================================================================
# 66. COMPLEX DICT ITERATION PATTERNS
# ============================================================================
# Transform dict to list of formatted strings
set people {alice 30 bob 25 carol 35}
set formatted {}
dict for {name age} $people {
    lappend formatted "$name=$age"
}
assert dict-for-fmt [lsort $formatted] {alice=30 bob=25 carol=35}

# dict with (modify in place)
set record {name "test" count 0 active 1}
dict with record {
    incr count
    set name "updated"
}
assert dict-with-name  [dict get $record name]  updated
assert dict-with-count [dict get $record count] 1

# ============================================================================
# 67. OO — MIXIN APPLIED AT RUNTIME
# ============================================================================
oo::class create Cacheable {
    variable cache

    method cache_get {key} {
        if {[info exists cache] && [dict exists $cache $key]} {
            return [dict get $cache $key]
        }
        return ""
    }

    method cache_set {key val} {
        if {![info exists cache]} { set cache {} }
        dict set cache $key $val
    }
}

oo::class create DataService {
    variable svc_name

    constructor {n} { set svc_name $n }

    method fetch {key} {
        return "data-$key"
    }

    method name {} { return $svc_name }
}

# Apply mixin at runtime
set ds [DataService new "svc1"]
assert rt-mixin-pre  [$ds name] svc1
oo::objdefine $ds mixin Cacheable
$ds cache_set "x" "cached-x"
assert rt-mixin-cache [$ds cache_get "x"] cached-x
assert rt-mixin-fetch [$ds fetch "y"]     data-y

# ============================================================================
# 68. EXPR — BITWISE OPERATIONS (bitwiseAnd/Or/Xor/Not, lshift, rshift)
# ============================================================================
assert bit-and    [expr {0xFF & 0x0F}]     15
assert bit-or     [expr {0xF0 | 0x0F}]     255
assert bit-xor    [expr {0xFF ^ 0x0F}]     240
assert bit-not    [expr {~0}]              -1
assert bit-lshift [expr {1 << 10}]         1024
assert bit-rshift [expr {1024 >> 3}]       128

# ============================================================================
# 69. EXPR — MODULO, EXPONENT, NEGATION, POWER
# ============================================================================
assert expr-mod2  [expr {17 % 5}]          2
assert expr-neg2  [expr {-42}]             -42
assert expr-exp2  [expr {2 ** 20}]         1048576
assert expr-neq   [expr {3 != 4}]          1
assert expr-neq2  [expr {3 != 3}]          0

# ============================================================================
# 70. EXPR — LOGICAL OPERATORS (land, lor, lnot)
# ============================================================================
assert logic-and1 [expr {1 && 1}]          1
assert logic-and2 [expr {1 && 0}]          0
assert logic-or1  [expr {0 || 1}]          1
assert logic-or2  [expr {0 || 0}]          0
assert logic-not1 [expr {!0}]             1
assert logic-not2 [expr {!1}]             0

# ============================================================================
# 71. STRING — strlen, totitle, trimleft, trimright
# ============================================================================
assert strlen-val    [string length "hello world"]  11
assert strtitle-val  [string totitle "hello WORLD"] "Hello world"
assert triml-val     [string trimleft  "  hello  "] "hello  "
assert trimr-val     [string trimright "  hello  "] "  hello"

# ============================================================================
# 72. ARRAY — set/unset (arraySetStk, arrayUnsetStk)
# ============================================================================
array set testarr {alpha 1 beta 2 gamma 3}
assert arrset-get $testarr(beta) 2
array unset testarr beta
assert arrset-exists [info exists testarr(beta)] 0
assert arrset-alpha  $testarr(alpha) 1

# ============================================================================
# 73. DICT — for, incr, append, lappend, update, unset, merge
# ============================================================================
# dict for (dictFirst, dictNext, dictDone)
set dsum 0
dict for {dk dv} {a 10 b 20 c 30} {
    incr dsum $dv
}
assert dictfor-sum $dsum 60

# dict incr (dictIncrImm)
set dinc {count 0 total 100}
dict incr dinc count
dict incr dinc count
dict incr dinc total 5
assert dictincr-count [dict get $dinc count] 2
assert dictincr-total [dict get $dinc total] 105

# dict append
set dapp {msg "hello"}
dict append dapp msg " world"
assert dictappend-val [dict get $dapp msg] "hello world"

# dict lappend
set dlap {tags {}}
dict lappend dlap tags "first"
dict lappend dlap tags "second"
assert dictlappend-val [dict get $dlap tags] {first second}

# dict update (dictUpdateStart/dictUpdateEnd)
set dupd {name "Alice" age 30}
dict update dupd name n age a {
    set n "Bob"
    incr a 5
}
assert dictupdate-name [dict get $dupd name] Bob
assert dictupdate-age  [dict get $dupd age]  35

# dict unset (dictUnsetN)
set duns {a 1 b 2 c 3}
dict unset duns b
assert dictunset-exists [dict exists $duns b] 0
assert dictunset-a [dict get $duns a] 1

# ============================================================================
# 74. LSET (lsetFlat)
# ============================================================================
set lsetvar {a b c d e}
lset lsetvar 2 "X"
assert lset-simple $lsetvar {a b X d e}
set lsetvar2 {{1 2} {3 4} {5 6}}
lset lsetvar2 1 0 "Z"
assert lset-nested $lsetvar2 {{1 2} {Z 4} {5 6}}

# ============================================================================
# 75. UNSET (unsetStk)
# ============================================================================
set unset_test_var 42
assert unset-before [info exists unset_test_var] 1
unset unset_test_var
assert unset-after [info exists unset_test_var] 0

# ============================================================================
# 76. GLOBAL / NAMESPACE UPVAR (nsupvar)
# ============================================================================
set ::globalvar 999
proc read_global {} {
    global globalvar
    return $globalvar
}
assert global-read [read_global] 999

namespace eval ::nstest2 {
    variable nsvar2 "nsval"
}
proc read_nsvar {} {
    namespace upvar ::nstest2 nsvar2 local
    return $local
}
assert nsupvar-read [read_nsvar] nsval

# ============================================================================
# 77. INFO COROUTINE (infoCoroutine)
# ============================================================================
proc coro_identity {} {
    set myname [info coroutine]
    yield $myname
}
set icresult [coroutine idcoro coro_identity]
assert info-coro $icresult ::idcoro

# ============================================================================
# 78. CONCAT
# ============================================================================
set clist1 {a b}
set clist2 {c d}
assert concat-val [concat $clist1 $clist2 e] {a b c d e}

# ============================================================================
# 79. OO — SELF / NEXT / MY (tclooSelf, tclooNext)
# ============================================================================
oo::class create SelfBase {
    method tag {} { return "base" }
}

oo::class create SelfDerived {
    superclass SelfBase
    method tag {} { return "derived+[next]" }
    method ident {} { return [self] }
}

set sd [SelfDerived new]
assert oo-next-chain [$sd tag]   "derived+base"
assert oo-self-obj   [string match "::oo::Obj*" [$sd ident]] 1
$sd destroy

# ============================================================================
# 80. YIELDTO
# ============================================================================
proc coro_yieldto {} {
    yieldto return -level 0 "yielded-value"
}
set ytresult [coroutine ytcoro coro_yieldto]
assert yieldto-val $ytresult yielded-value
rename ytcoro ""

# ============================================================================
# 81. CONST (Tcl 9.x — constStk)
# ============================================================================
proc test_const {} {
    const PI 3.14159
    return $PI
}
assert const-val [test_const] 3.14159

# ============================================================================
# FINAL RESULT
# ============================================================================

# Count passes and failures
set pass 0
set fail 0
set failures {}
foreach r $::results {
    if {[string match "ok:*" $r]} {
        incr pass
    } else {
        incr fail
        lappend failures $r
    }
}

if {$fail > 0} {
    return "$pass passed, $fail failed: $failures"
} else {
    return "$pass passed, 0 failed"
}
