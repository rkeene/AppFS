#! /usr/bin/env tclsh

proc stringifyfile {filename {key 0}} {
	catch {
		set fd [open $filename r]
	}

	if {![info exists fd]} {
		return ""
	}

	set data [read -nonewline $fd]
	close $fd

	foreach line [split $data \n] {
		set line [string map [list "\\" "\\\\" "\"" "\\\""] $line]
		append ret "	\"$line\\n\"\n"
	}

	return $ret
}

foreach file $argv {
	puts -nonewline [stringifyfile $file]
}

exit 0
