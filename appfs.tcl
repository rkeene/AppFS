#! /usr/bin/env tclsh

package require http

namespace eval ::appfs {
	variable sites [list]
	variable cachedir "/tmp/appfs-cache"

	proc _hash_sep {hash {seps 4}} {
		for {set idx 0} {$idx < $seps} {incr idx} {
			append retval "[string range $hash [expr {$idx * 2}] [expr {($idx * 2) + 1}]]/"
		}
		append retval "[string range $hash [expr {$idx * 2}] end]"

		return $retval
	}

	proc _cachefile {url key {keyIsHash 1}} {
		if {$keyIsHash} {
			set key [_hash_sep $key]
		}

		set file [file join $::appfs::cachedir $key]

		file mkdir [file dirname $file]

		if {![file exists $file]} {
			set tmpfile "${file}.new"

			set fd [open $tmpfile "w"]

			set token [::http::geturl $url -channel $fd]
			set ncode [::http::ncode $token]
			::http::reset $token
			close $fd

			if {$ncode == "200"} {
				file rename -force -- $tmpfile $file
			} else {
				file delete -force -- $tmpfile
			}
		}

		return $file
	}

	proc getindex {hostname} {
		if {[string match "*\[/~\]*" $hostname]} {
			return -code error "Invalid hostname"
		}

		set url "http://$hostname/appfs/index"

		set indexcachefile [_cachefile $url "SERVERS/[string tolower $hostname]" 0]

		if {![file exists $indexcachefile]} {
			return -code error "Unable to fetch $url"
		}

		set fd [open $indexcachefile]
		gets $fd indexhash_data
		set indexhash [lindex [split $indexhash_data ","] 0]
		close $fd

		set file [download $hostname $indexhash]
		set fd [open $file]
		set data [read $fd]
		close $fd

		array set packages [list]
		foreach line [split $data "\n"] {
			set line [string trim $line]

			if {[string match "*/*" $line]} {
				continue
			}

			if {$line == ""} {
				continue
			}

			set work [split $line ","]

			unset -nocomplain pkgInfo
			set pkgInfo(package)  [lindex $work 0]
			set pkgInfo(version)  [lindex $work 1]
			set pkgInfo(os)       [lindex $work 2]
			set pkgInfo(cpuArch)  [lindex $work 3]
			set pkgInfo(hash)     [string tolower [lindex $work 4]]
			set pkgInfo(hash_type) "sha1"
			set pkgInfo(isLatest) [expr {!![lindex $work 5]}]

			if {[string length $pkgInfo(hash)] != 40} {
				continue
			}

			if {![regexp {^[0-9a-f]*$} $pkgInfo(hash)]} {
				continue
			}

			set packages($pkgInfo(package)) [array get pkgInfo]
		}

		return [array get packages]
	}

	proc download {hostname hash {method sha1}} {
		set url "http://$hostname/appfs/$method/$hash"
		set file [_cachefile $url $hash]

		if {![file exists $file]} {
			return -code error "Unable to fetch"
		}

		return $file
	}
}
