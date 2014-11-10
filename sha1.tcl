#! /usr/bin/env tclsh

proc sha1::sha1 args {
	set outputmode "hex"

	if {[lindex $args 0] == "-hex"} {
		set outputmode "hex"

		set args [lrange $args 1 end]
	} elseif {[lindex $args 0] == "-bin"} {
		set outputmode "binary"

		set args [lrange $args 1 end]
	}

	if {[llength $args] == 2} {
		set mode [lindex $args 0]
	} elseif {[llength $args] == 1} {
		set mode "-string"
	} else {
		return -code error "wrong # args: sha1::sha1 ?-bin|-hex? ?-channel channel|-file file|string?"
	}

	switch -- $mode {
		"-channel" {
			return -code error "Not implemented"
		}
		"-file" {
			set output [_sha1_file [lindex $args end]]
		}
		"-string" {
			set output [_sha1_string [lindex $args end]]
		}
		default {
			return -code error "invalid mode: $mode, must be one of -channel or -file (or a plain string)"
		}
	}

	if {$outputmode == "hex"} {
		binary scan $output H* output
	}

	return $output
}
