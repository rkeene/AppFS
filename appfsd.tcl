#! /usr/bin/env tclsh

package require http 2.7
package require sqlite3

namespace eval ::appfs {
	variable cachedir "/tmp/appfs-cache"
	variable ttl 3600

	proc _hash_sep {hash {seps 4}} {
		for {set idx 0} {$idx < $seps} {incr idx} {
			append retval "[string range $hash [expr {$idx * 2}] [expr {($idx * 2) + 1}]]/"
		}
		append retval "[string range $hash [expr {$idx * 2}] end]"

		return $retval
	}

	proc _cachefile {url key {keyIsHash 1}} {
		set filekey $key
		if {$keyIsHash} {
			set filekey [_hash_sep $filekey]
		}

		set file [file join $::appfs::cachedir $filekey]

		file mkdir [file dirname $file]

		if {![file exists $file]} {
			set tmpfile "${file}.new"

			set fd [open $tmpfile "w"]
			fconfigure $fd -translation binary

			catch {
				set token [::http::geturl $url -channel $fd -binary true]
			}

			if {[info exists token]} {
				set ncode [::http::ncode $token]
				::http::reset $token
			} else {
				set ncode "900"
			}

			close $fd

			if {$keyIsHash} {
				catch {
					set hash [string tolower [exec openssl sha1 $tmpfile]]
					regsub {.*= *} $hash {} hash
				}
			} else {
				set hash $key
			}

			if {$ncode == "200" && $hash == $key} {
				file rename -force -- $tmpfile $file
			} else {
				file delete -force -- $tmpfile
			}
		}

		return $file
	}

	proc _db {args} {
		return [uplevel 1 [list ::appfs::db {*}$args]]
	}

	proc init {} {
		if {[info exists ::appfs::init_called]} {
			return
		}

		set ::appfs::init_called 1

		if {![info exists ::appfs::db]} {
			file mkdir $::appfs::cachedir

			sqlite3 ::appfs::db [file join $::appfs::cachedir cache.db]
		}

		_db eval {CREATE TABLE IF NOT EXISTS sites(hostname PRIMARY KEY, lastUpdate);}
		_db eval {CREATE TABLE IF NOT EXISTS packages(hostname, sha1, package, version, os, cpuArch, isLatest, haveManifest);}
		_db eval {CREATE TABLE IF NOT EXISTS files(package_sha1, type, time, source, size, perms, file_sha1, file_name, file_directory);}
	}

	proc download {hostname hash {method sha1}} {
		set url "http://$hostname/appfs/$method/$hash"
		set file [_cachefile $url $hash]

		if {![file exists $file]} {
			return -code error "Unable to fetch"
		}

		return $file
	}

	proc getindex {hostname} {
		set now [clock seconds]

		set lastUpdates [_db eval {SELECT lastUpdate FROM sites WHERE hostname = $hostname LIMIT 1;}]
		if {[llength $lastUpdates] == 0} {
			set lastUpdate 0
		} else {
			set lastUpdate [lindex $lastUpdates 0]
		}

		if {$now < ($lastUpdate + $::appfs::ttl)} {
			return COMPLETE
		}

		if {[string match "*\[/~\]*" $hostname]} {
			return -code error "Invalid hostname"
		}

		set url "http://$hostname/appfs/index"

		catch {
			set token [::http::geturl $url]
			if {[::http::ncode $token] == "200"} {
				set indexhash_data [::http::data $token]
			}
			::http::reset $token
			$token cleanup
		}

		if {![info exists indexhash_data]} {
			return -code error "Unable to fetch $url"
		}

		set indexhash [lindex [split $indexhash_data ","] 0]

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

			# Do not do any additional work if we already have this package
			set existing_packages [_db eval {SELECT package FROM packages WHERE hostname = $hostname AND sha1 = $pkgInfo(hash);}]
			if {[lsearch -exact $existing_packages $pkgInfo(package)] != -1} {
				continue
			}

			if {$pkgInfo(isLatest)} {
				_db eval {UPDATE packages SET isLatest = 0 WHERE hostname = $hostname AND package = $pkgInfo($package) AND os = $pkgInfo($package) AND cpuArch = $pkgInfo(cpuArch);}
			}

			_db eval {INSERT INTO packages (hostname, sha1, package, version, os, cpuArch, isLatest, haveManifest) VALUES ($hostname, $pkgInfo(hash), $pkgInfo(package), $pkgInfo(version), $pkgInfo(os), $pkgInfo(cpuArch), $pkgInfo(isLatest), 0);}

		}

		_db eval {INSERT OR REPLACE INTO sites (hostname, lastUpdate) VALUES ($hostname, $now);}

		return COMPLETE
	}

	proc getpkgmanifest {hostname package_sha1} {
		set haveManifests [_db eval {SELECT haveManifest FROM packages WHERE sha1 = $package_sha1 LIMIT 1;}]
		set haveManifest [lindex $haveManifests 0]

		if {$haveManifest} {
			return COMPLETE
		}

		set file [download $hostname $package_sha1]
		set fd [open $file]
		set pkgdata [read $fd]
		close $fd

		foreach line [split $pkgdata "\n"] {
			set line [string trim $line]

			if {$line == ""} {
				continue
			}

			set work [split $line ","]

			unset -nocomplain fileInfo
			set fileInfo(type) [lindex $work 0]
			set fileInfo(time) [lindex $work 1]
			set fileInfo(name) [lindex $work end]

			set fileInfo(name) [split [string trim $fileInfo(name) "/"] "/"]
			set fileInfo(directory) [join [lrange $fileInfo(name) 0 end-1] "/"]
			set fileInfo(name) [lindex $fileInfo(name) end]

			set work [lrange $work 2 end-1]
			switch -- $fileInfo(type) {
				"file" {
					set fileInfo(size) [lindex $work 0]
					set fileInfo(perms) [lindex $work 1]
					set fileInfo(sha1) [lindex $work 2]
				}
				"symlink" {
					set fileInfo(source) [lindex $work 0]
				}
			}

			_db eval {INSERT INTO files (package_sha1, type, time, source, size, perms, file_sha1, file_name, file_directory) VALUES ($package_sha1, $fileInfo(type), $fileInfo(time), $fileInfo(source), $fileInfo(size), $fileInfo(perms), $fileInfo(sha1), $fileInfo(name), $fileInfo(directory) );}
			_db eval {UPDATE packages SET haveManifest = 1 WHERE sha1 = $package_sha1;}
		}

		return COMPLETE
	}
}
