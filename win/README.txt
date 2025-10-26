
Building the tbcx package for Windows
=============================================

o. Microsoft MSVC++ build:
--------------------------

 To build tbcx you need to have the source code of a Tcl 9.1+ distribution, also compiled and
 installed. Invoke the following command:

 nmake -f makefile.vc TCLDIR=<path-to-tcl-sources> INSTALLDIR=<path-to-installed-tcl>

Please look into the makefile.vc file for more options.

To install the extension, invoke the following command:

    nmake -f makefile.vc install
