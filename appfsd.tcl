#! /usr/bin/env tclsh

package require http 2.7
package require sqlite3
package require sha1
package require appfsd

namespace eval ::appfs {
	variable cachedir "/tmp/appfs-cache"
	variable ttl 3600
	variable nttl 60

	# User-replacable function to convert a hostname/hash/method to an URL
	proc _construct_url {hostname hash method} {
		return "http://$hostname/appfs/$method/$hash"
	}

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

		if {[file exists $file]} {
			return $file
		}

		set tmpfile "${file}.[expr {rand()}]"

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
			set hash [string tolower [sha1::sha1 -hex -file $tmpfile]]
		} else {
			set hash $key
		}

		if {$ncode == "200" && $hash == $key} {
			file rename -force -- $tmpfile $file
		} else {
			file delete -force -- $tmpfile
		}

		return $file
	}


	proc _isHash {value} {
		set value [string tolower $value]

		if {[string length $value] != 40} {
			return false
		}

		if {![regexp {^[0-9a-f]*$} $value]} {
			return false
		}

		return true
	}

	proc _normalizeOS {os} {
		set os [string tolower [string trim $os]]

		switch -- $os {
			"linux" - "freebsd" - "openbsd" - "netbsd" {
				return $os
			}
			"sunos" {
				return "solaris"
			}
			"noarch" - "none" - "any" - "all" {
				return "noarch"
			}
		}

		return -code error "Unable to normalize OS: $os"
	}

	proc _normalizeCPU {cpu} {
		set cpu [string tolower [string trim $cpu]]

		switch -glob -- $cpu {
			"i?86" {
				return "ix86"
			}
			"x86_64" {
				return $cpu
			}
			"noarch" - "none" - "any" - "all" {
				return "noarch"
			}
		}

		return -code error "Unable to normalize CPU: $cpu"
	}

	proc init {} {
		if {[info exists ::appfs::init_called]} {
			return
		}

		# Force [parray] to be loaded
		catch {
			parray does_not_exist
		}

		set ::appfs::init_called 1

		# Load configuration file
		set config_file [file join $::appfs::cachedir config]
		if {[file exists $config_file]} {
			source $config_file
		}

		if {![info exists ::appfs::db]} {
			file mkdir $::appfs::cachedir

			sqlite3 ::appfs::db [file join $::appfs::cachedir cache.db]
		}

		# Create tables
		db eval {CREATE TABLE IF NOT EXISTS sites(hostname PRIMARY KEY, lastUpdate, ttl);}
		db eval {CREATE TABLE IF NOT EXISTS packages(hostname, sha1, package, version, os, cpuArch, isLatest, haveManifest);}
		db eval {CREATE TABLE IF NOT EXISTS files(package_sha1, type, time, source, size, perms, file_sha1, file_name, file_directory);}

		# Create indexes
		db eval {CREATE INDEX IF NOT EXISTS sites_index ON sites (hostname);}
		db eval {CREATE INDEX IF NOT EXISTS packages_index ON packages (hostname, package, version, os, cpuArch);}
		db eval {CREATE INDEX IF NOT EXISTS files_index ON files (package_sha1, file_name, file_directory);}
	}

	proc download {hostname hash {method sha1}} {
		set url [_construct_url $hostname $hash $method]
		set file [_cachefile $url $hash]

		if {![file exists $file]} {
			return -code error "Unable to fetch (file does not exist: $file)"
		}

		return $file
	}

	proc getindex {hostname} {
		set now [clock seconds]

		set lastUpdates [db eval {SELECT lastUpdate, ttl FROM sites WHERE hostname = $hostname LIMIT 1;}]
		if {[llength $lastUpdates] == 0} {
			set lastUpdate 0
			set ttl 0
		} else {
			set lastUpdate [lindex $lastUpdates 0]
			set ttl [lindex $lastUpdates 1]
		}

		if {$now < ($lastUpdate + $ttl)} {
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
			::http::cleanup $token
		}

		if {![info exists indexhash_data]} {
			# Cache this result for 60 seconds
			db eval {INSERT OR REPLACE INTO sites (hostname, lastUpdate, ttl) VALUES ($hostname, $now, $::appfs::nttl);}

			return -code error "Unable to fetch $url"
		}

		set indexhash [lindex [split $indexhash_data ","] 0]

		if {![_isHash $indexhash]} {
			return -code error "Invalid hash: $indexhash"
		}

		set file [download $hostname $indexhash]
		set fd [open $file]
		set data [read $fd]
		close $fd

		set curr_packages [list]
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
			if {[catch {
				set pkgInfo(package)  [lindex $work 0]
				set pkgInfo(version)  [lindex $work 1]
				set pkgInfo(os)       [_normalizeOS [lindex $work 2]]
				set pkgInfo(cpuArch)  [_normalizeCPU [lindex $work 3]]
				set pkgInfo(hash)     [string tolower [lindex $work 4]]
				set pkgInfo(hash_type) "sha1"
				set pkgInfo(isLatest) [expr {!![lindex $work 5]}]
			}]} {
				continue
			}

			if {![_isHash $pkgInfo(hash)]} {
				continue
			}

			lappend curr_packages $pkgInfo(hash)

			# Do not do any additional work if we already have this package
			set existing_packages [db eval {SELECT package FROM packages WHERE hostname = $hostname AND sha1 = $pkgInfo(hash);}]
			if {[lsearch -exact $existing_packages $pkgInfo(package)] != -1} {
				continue
			}

			if {$pkgInfo(isLatest)} {
				db eval {UPDATE packages SET isLatest = 0 WHERE hostname = $hostname AND package = $pkgInfo($package) AND os = $pkgInfo($package) AND cpuArch = $pkgInfo(cpuArch);}
			}

			db eval {INSERT INTO packages (hostname, sha1, package, version, os, cpuArch, isLatest, haveManifest) VALUES ($hostname, $pkgInfo(hash), $pkgInfo(package), $pkgInfo(version), $pkgInfo(os), $pkgInfo(cpuArch), $pkgInfo(isLatest), 0);}
		}

		# Look for packages that have been deleted
		set found_packages [db eval {SELECT sha1 FROM packages WHERE hostname = $hostname;}]
		foreach package $found_packages {
			set found_packages_arr($package) 1
		}

		foreach package $curr_packages {
			unset -nocomplain found_packages_arr($package)
		}

		foreach package [array names found_packages_arr] {
			db eval {DELETE FROM packages WHERE hostname = $hostname AND sha1 = $package;}
		}

		db eval {INSERT OR REPLACE INTO sites (hostname, lastUpdate, ttl) VALUES ($hostname, $now, $::appfs::ttl);}

		return COMPLETE
	}

	proc getpkgmanifest {hostname package_sha1} {
		set haveManifests [db eval {SELECT haveManifest FROM packages WHERE sha1 = $package_sha1 LIMIT 1;}]
		set haveManifest [lindex $haveManifests 0]

		if {$haveManifest} {
			return COMPLETE
		}

		if {![_isHash $package_sha1]} {
			return FAIL
		}

		set file [download $hostname $package_sha1]
		set fd [open $file]
		set pkgdata [read $fd]
		close $fd

		db transaction {
			foreach line [split $pkgdata "\n"] {
				set line [string trim $line]

				if {$line == ""} {
					continue
				}

				set work [split $line ","]

				unset -nocomplain fileInfo
				set fileInfo(type) [lindex $work 0]
				set fileInfo(time) [lindex $work 1]

				set work [lrange $work 2 end]
				switch -- $fileInfo(type) {
					"file" {
						set fileInfo(size) [lindex $work 0]
						set fileInfo(perms) [lindex $work 1]
						set fileInfo(sha1) [lindex $work 2]

						set work [lrange $work 3 end]
					}
					"symlink" {
						set fileInfo(source) [lindex $work 0]
						set work [lrange $work 1 end]
					}
				}

				set fileInfo(name) [join $work ","]
				set fileInfo(name) [split [string trim $fileInfo(name) "/"] "/"]
				set fileInfo(directory) [join [lrange $fileInfo(name) 0 end-1] "/"]
				set fileInfo(name) [lindex $fileInfo(name) end]

				db eval {INSERT INTO files (package_sha1, type, time, source, size, perms, file_sha1, file_name, file_directory) VALUES ($package_sha1, $fileInfo(type), $fileInfo(time), $fileInfo(source), $fileInfo(size), $fileInfo(perms), $fileInfo(sha1), $fileInfo(name), $fileInfo(directory) );}
				db eval {UPDATE packages SET haveManifest = 1 WHERE sha1 = $package_sha1;}
			}
		}

		return COMPLETE
	}

	proc _localpath {package hostname file} {
		set homedir [::appfsd::get_homedir]
		set dir [file join $homedir .appfs "./${package}@${hostname}" "./${file}"]
	}

	proc _parsepath {path} {
		set path [string trim $path "/"]
		set path [split $path "/"]
		set pathlen [llength $path]

		array set retval [list _children sites _type toplevel]

		if {$pathlen > 0} {
			set retval(hostname) [lindex $path 0]
			set retval(_children) packages
			set retval(_type) sites

			if {$pathlen > 1} {
				set package [lindex $path 1]
				if {[string length $package] == "40" && [regexp {^[a-fA-F0-9]*$} $package]} {
					set retval(package_sha1) $package
					set retval(_children) files
					set retval(_type) files

					::appfs::db eval {SELECT package, os, cpuArch, version FROM packages WHERE sha1 = $retval(package_sha1);} pkginfo {}
					set retval(package) $pkginfo(package)
					set retval(os) $pkginfo(os)
					set retval(cpu) $pkginfo(cpuArch)
					set retval(version) $pkginfo(version)

					if {$pathlen > 2} {
						set retval(file) [join [lrange $path 2 end] "/"]
					} else {
						set retval(file) ""
					}
				} else {
					set retval(package) $package
					set retval(_children) os-cpu
					set retval(_type) packages

					if {$pathlen > 2} {
						set os_cpu [lindex $path 2]
						set os_cpu [split $os_cpu "-"]

						set retval(os) [lindex $os_cpu 0]
						set retval(cpu) [lindex $os_cpu 1]
						set retval(_children) versions
						set retval(_type) os-cpu

						if {$pathlen > 3} {
							set retval(version) [lindex $path 3]
							set retval(_children) files
							set retval(_type) versions

							set retval(package_sha1) [::appfs::db onecolumn {SELECT sha1 FROM packages WHERE hostname = $retval(hostname) AND os = $retval(os) AND cpuArch = $retval(cpu) AND version = $retval(version);}]
							if {$retval(package_sha1) == ""} {
								return [list]
							}

							if {$pathlen > 4} {
								set retval(_type) files
								set retval(file) [join [lrange $path 4 end] "/"]
							} else {
								set retval(file) ""
							}
						}
					}
				}
			}
		}

		return [array get retval]
	}

	proc getchildren {dir} {
		array set pathinfo [_parsepath $dir]

		switch -- $pathinfo(_children) {
			"sites" {
				return [::appfs::db eval {SELECT DISTINCT hostname FROM packages;}]
			}
			"packages" {
				catch {
					::appfs::getindex $pathinfo(hostname)
				}

				return [::appfs::db eval {SELECT DISTINCT package FROM packages WHERE hostname = $pathinfo(hostname);}]
			}
			"os-cpu" {
				set retval [::appfs::db eval {SELECT DISTINCT os || "-" || cpuArch FROM packages WHERE hostname = $pathinfo(hostname) AND package = $pathinfo(package);}]

				lappend retval "platform"

				return $retval
			}
			"versions" {
				set retval [::appfs::db eval {
					SELECT DISTINCT version FROM packages WHERE hostname = $pathinfo(hostname) AND package = $pathinfo(package) AND os = $pathinfo(os) AND cpuArch = $pathinfo(cpu);
				}]

				lappend retval "latest"

				return $retval
			}
			"files" {
				catch {
					::appfs::getpkgmanifest $pathinfo(hostname) $pathinfo(package_sha1)
				}

				set retval [::appfs::db eval {SELECT DISTINCT file_name FROM files WHERE package_sha1 = $pathinfo(package_sha1) AND file_directory = $pathinfo(file);}]

				if {[info exists pathinfo(package)] && [info exists pathinfo(hostname)] && [info exists pathinfo(file)]} {
					set dir [_localpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)]
					foreach file [glob -nocomplain -tails -directory $dir -types {d f l} {{.,}*}] {
						if {$file == "." || $file == ".."} {
							continue
						}

						if {[lsearch -exact $retval $file] != -1} {
							continue
						}

						lappend retval $file
					}
				}

				return $retval
			}
		}

		return -code error "Invalid or unacceptable path: $dir"
	}

	proc getattr {path} {
		array set pathinfo [_parsepath $path]
		array set retval [list]

		switch -- $pathinfo(_type) {
			"toplevel" - "sites" - "packages" - "os-cpu" - "versions" {
				set retval(type) directory
				set retval(childcount) 2;
			}
			"files" {
				set localpath [_localpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)]
				if {[file exists $localpath]} {
					catch {
						file lstat $localpath localpathinfo
						set retval(time) $localpathinfo(mtime)

						switch -- $localpathinfo(type) {
							"directory" {
								set retval(type) "directory"
								set retval(childcount) 2
							}
							"file" {
								set retval(type) "file"
								set retval(size) $localpathinfo(size)
								if {[file executable $localpath]} {
									set retval(perms) "x"
								} else {
									set retval(perms) ""
								}
							}
							"link" {
								set retval(type) "symlink"
								set retval(source) [file readlink $localpath]
							}
						}
					} err
				} else {
					set work [split $pathinfo(file) "/"]
					set directory [join [lrange $work 0 end-1] "/"]
					set file [lindex $work end]
					::appfs::db eval {SELECT type, time, source, size, perms FROM files WHERE package_sha1 = $pathinfo(package_sha1) AND file_directory = $directory AND file_name = $file;} retval {}
					unset -nocomplain retval(*)
				}

			}
		}

		if {![info exists retval(type)]} {
			return -code error "No such file or directory"
		}

		return [array get retval]
	}

	proc openpath {path mode} {
	}
}
