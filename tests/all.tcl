# all.tcl for tbcx tests -*-Tcl-*-

package require Tcl 9.1
package require tcltest
namespace import ::tcltest::*

configure \
    -testdir   [file dirname [info script]] \
    -verbose   {body error} \
    -preservecore 0

runAllTests

cleanupTests