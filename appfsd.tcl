#! /usr/bin/env tclsh

#
# Copyright (c) 2014  Roy Keene
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#

package require http 2.7
package require sqlite3
package require sha1
package require appfsd
package require platform
package require pki

# Functions specifically meant for users to replace as a part of configuration
namespace eval ::appfs::user {
	variable download_method "tcl"

	# User-replacable function to convert a hostname/hash/method to an URL
	proc construct_url {hostname hash method} {
		return "http://$hostname/appfs/$method/$hash"
	}

	# User-replaceable function get the home directory of the current user
	proc get_homedir {} {
		return [::appfsd::get_homedir]
	}

	# User-replacable function to update permissions
	proc change_perms {file hash perms {hashMethod "sha1"}} {
		if {[info exists ::appfs::user::add_perms($file)]} {
			append perms $::appfs::user::add_perms($file)
		}

		if {[info exists ::appfs::user::add_perms([list $hashMethod $hash])]} {
			append perms $::appfs::user::add_perms([list $hashMethod $hash])
		} elseif {$hashMethod eq "sha1" && [info exists ::appfs::user::add_perms($hash)]} {
			append perms $::appfs::user::add_perms($hash)
		}

		return $perms
	}

	# User-replacable function to fetch a remote file
	proc download_file {url {outputChannel ""}} {
		switch -- $::appfs::user::download_method {
			"curl" {
				if {$outputChannel eq ""} {
					return [exec curl -sS -L -- $url]
				} else {
					exec curl -sS -L -- $url >@ $outputChannel

					return ""
				}
			}
			"tcl" {
				catch {
					if {$outputChannel eq ""} {
						set token [http::geturl $url]
						set retval [http::data $token]
					} else {
						set token [http::geturl $url -binary true -channel $outputChannel]
						set retval ""
					}
				} err

				if {![info exists token]} {
					return -code error "Unable to download \"$url\": $err"
				}

				set tokenCode [http::ncode $token]

				http::cleanup $token

				if {$tokenCode != "200"} {
					return -code error "Unable to download \"$url\": Site did not return a 200 (returned $tokenCode)"
				}

				if {![info exists retval]} {
					return -code error "Unable to download \"$url\": Site did not return proper data: $err"
				}

				return $retval
			}

		}

		return -code error "Unable to download"
	}
}

namespace eval ::appfs {
	variable cachedir "/tmp/appfs-cache"
	variable ttl 3600
	variable nttl 60
	variable trusted_cas [list]
	variable platform [::platform::generic]

	proc _hash_sep {hash {seps 4}} {
		for {set idx 0} {$idx < $seps} {incr idx} {
			append retval "[string range $hash [expr {$idx * 2}] [expr {($idx * 2) + 1}]]/"
		}
		append retval "[string range $hash [expr {$idx * 2}] end]"

		return $retval
	}

	proc _cachefile {url key method {keyIsHash 1}} {
		if {$keyIsHash && $method != "sha1"} {
			return -code error "Only SHA1 hashing method is supported"
		}

		set filekey $key
		if {$keyIsHash} {
			set filekey [_hash_sep $filekey]
		}

		set file [file join $::appfs::cachedir $filekey]

		file mkdir [file dirname $file]

		if {[file exists $file]} {
			return $file
		}

		set tmpfile "${file}.[expr {rand()}][clock clicks]"

		set fd [open $tmpfile "w"]
		fconfigure $fd -translation binary

		catch {
			::appfs::user::download_file $url $fd
		}

		close $fd

		if {$keyIsHash} {
			set hash [string tolower [sha1::sha1 -hex -file $tmpfile]]
		} else {
			set hash $key
		}

		if {$hash == $key} {
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

	proc _verifySignatureAndCertificate {hostname certificate signature hash} {
		set certificate [binary format "H*" $certificate]
		set signature   [binary format "H*" $signature]

		set certificate [::pki::x509::parse_cert $certificate]

		array set certificate_arr $certificate
		set certificate_cn [::pki::x509::_dn_to_cn $certificate_arr(subject)]

		if {![::pki::verify $signature "$hash,sha1" $certificate]} {
			return false
		}

		if {[string tolower $certificate_cn] != [string tolower $hostname]} {
			return false
		}

		if {![::pki::x509::verify_cert $certificate $::appfs::trusted_cas]} {
			return false
		}

		return true
	}

	proc _normalizeOS {os {tolerant 0}} {
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

		if {$tolerant} {
			return $os
		}

		return -code error "Unable to normalize OS: $os"
	}

	proc _normalizeCPU {cpu {tolerant 0}} {
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

		if {$tolerant} {
			return $cpu
		}

		return -code error "Unable to normalize CPU: $cpu"
	}

	proc _as_user {code} {
		::appfsd::simulate_user_fs_enter

		set retcode [catch [list uplevel $code] retstr]

		::appfsd::simulate_user_fs_leave

		return -code $retcode $retstr
	}

	proc init {} {
		if {[info exists ::appfs::init_called]} {
			return
		}

		# Force [parray] and [clock] to be loaded
		catch {
			parray does_not_exist
		}
		catch {
			clock seconds
		}
		catch {
			clock add [clock seconds] 3 seconds
		}

		set ::appfs::init_called 1

		# Add a default CA to list of trusted CAs
		lappend ::appfs::trusted_cas [::pki::x509::parse_cert {
-----BEGIN CERTIFICATE-----
MIIC7DCCAdSgAwIBAgIBATANBgkqhkiG9w0BAQUFADAvMRIwEAYDVQQKEwlSb3kg
S2VlbmUxGTAXBgNVBAMTEEFwcEZTIEtleSBNYXN0ZXIwHhcNMTQxMTE3MjAxNzI4
WhcNMTkxMTE3MjAxNzI4WjAvMRIwEAYDVQQKEwlSb3kgS2VlbmUxGTAXBgNVBAMT
EEFwcEZTIEtleSBNYXN0ZXIwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIB
AQCq6uSK46yG5b6RJWwRlvw5glAnjsc1GiX3duXA0vG4qnKUnDtl/jcMmq2GMOB9
Iy1tjabEHA0MhW2j7Vwe/O9MLFJkJ30M1PVD7YZRRNaAsz3UWIKEjPI7BBc32KOm
BL3CTXCCdzllL1HhVbnM5iCAmgHcg1DUk/EvWXvnEDxXRy2lV9mQsmDedrffY7Wl
Or57nlczaMuPLpyRSkv75PAnjQJxT3sWlBpy+/H9ImudQdpJNf/FtxcqN7iDwH5B
vIceYEtDVxFsvo5HOVkSl9jeo5E4Gpe3wyfRhoqB2UkaW1Kq0iH5R+00S760xQMx
LL9L1duhu1dL7HsmEw7IeYURAgMBAAGjEzARMA8GA1UdEwEB/wQFMAMBAf8wDQYJ
KoZIhvcNAQEFBQADggEBAKhO4ZSzYP37BqixNHKK9+gSeC6Fga85iLWhwpPW0kSl
z03hal80KZ+kPMzb8C52N283tQNAqJ9Q8akDPZxSzzMUVOGpGw2pJ7ZswKDz0ZTa
0edq/gdT/HrdegvNtDPc2jona5FVOYqwdcz5kbl1UWBaBp3VXUgcYjXSRaBK43Wd
cveiDUeZw7gHqRSN/AyYUCtJzWmvGsJuIFhMBonuz8jylhyMJCYJFT4iMUC8MNIw
niX1xx+Nu6fPV5ZZHj9rbhiBaLjm+tkDwtPgA3j2pxvHKYptuWxeYO+9DDNa9sCb
E5AnJIlOnd/tGe0Chf0sFQg+l9nNiNrWGgzdd9ZPJK4=
-----END CERTIFICATE-----
}]

		# Load configuration file
		set config_file [file join $::appfs::cachedir config]
		if {[file exists $config_file]} {
			source $config_file
		}

		if {![info exists ::appfs::db]} {
			file mkdir $::appfs::cachedir

			sqlite3 ::appfs::db [file join $::appfs::cachedir cache.db]

			::appfs::db timeout 30000
		}

		# Create tables
		db eval {CREATE TABLE IF NOT EXISTS sites(hostname PRIMARY KEY, lastUpdate, ttl);}
		db eval {CREATE TABLE IF NOT EXISTS packages(hostname, sha1, package, version, os, cpuArch, isLatest, haveManifest);}
		db eval {CREATE TABLE IF NOT EXISTS files(package_sha1, type, time, source, size, perms, file_sha1, file_name, file_directory);}

		# Create indexes
		db eval {CREATE INDEX IF NOT EXISTS sites_index ON sites (hostname);}
		db eval {CREATE INDEX IF NOT EXISTS packages_index ON packages (hostname, sha1, package, version, os, cpuArch);}
		db eval {CREATE INDEX IF NOT EXISTS files_index ON files (package_sha1, file_name, file_directory);}
	}

	proc download {hostname hash {method sha1}} {
		set url [::appfs::user::construct_url $hostname $hash $method]
		set file [_cachefile $url $hash $method]

		if {![file exists $file]} {
			return -code error "Unable to fetch (file does not exist: $file)"
		}

		return $file
	}

	proc getindex {hostname} {
		if {[string match "*\[/~\]*" $hostname]} {
			return -code error "Invalid hostname"
		}

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

		set url "http://$hostname/appfs/index"

		catch {
			set indexhash_data [::appfs::user::download_file $url]
		}

		# Note that we attempted to fetch this index and do not try
		# again for a while
		db eval {INSERT OR REPLACE INTO sites (hostname, lastUpdate, ttl) VALUES ($hostname, $now, $::appfs::nttl);}

		if {![info exists indexhash_data]} {
			return -code error "Unable to fetch $url"
		}

		set indexhash_data [string trim $indexhash_data "\r\n"]
		set indexhash_data [split $indexhash_data ","]
		set indexhash       [lindex $indexhash_data 0]
		set indexhashmethod [lindex $indexhash_data 1]
		set indexhashcert   [lindex $indexhash_data 2]
		set indexhashsig    [lindex $indexhash_data 3]

		if {![_isHash $indexhash]} {
			return -code error "Invalid hash: $indexhash"
		}

		if {![_verifySignatureAndCertificate $hostname $indexhashcert $indexhashsig $indexhash]} {
			return -code error "Invalid signature or certificate from $hostname"
		}

		set file [download $hostname $indexhash]
		catch {
			set fd [open $file]
		}

		if {![info exists fd]} {
			return -code error "Unable to download or open $file"
		}

		unset -nocomplain data
		catch {
			set data [read $fd]
		}

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
				db eval {UPDATE packages SET isLatest = 0 WHERE hostname = $hostname AND package = $pkgInfo(package) AND os = $pkgInfo(os) AND cpuArch = $pkgInfo(cpuArch);}
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

		appfsd::get_path_info_cache_flush

		return COMPLETE
	}

	proc getpkgmanifest {hostname package_sha1} {
		set haveManifest [db onecolumn {SELECT haveManifest FROM packages WHERE sha1 = $package_sha1 LIMIT 1;}]

		if {$haveManifest == "1"} {
			return COMPLETE
		}

		if {![_isHash $package_sha1]} {
			return FAIL
		}

		set file [download $hostname $package_sha1]

		catch {
			set fd [open $file]
		}

		if {![info exists fd]} {
			return -code error "Unable to download or open $file"
		}

		catch {
			set pkgdata [read $fd]
		}

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
					"#manifestmetadata" {
						unset -nocomplain fileInfo
						continue
					}
					"file" {
						set fileInfo(size) [lindex $work 0]

						# We lower-case the permissions because upper-case permissions
						# should not be set remotely as they may influence the security
						# of the system.
						set fileInfo(perms) [string tolower [lindex $work 1]]

						set fileInfo(sha1) [lindex $work 2]

						set work [lrange $work 3 end]
					}
					"symlink" {
						set fileInfo(source) [lindex $work 0]
						set work [lrange $work 1 end]
					}
					"directory" {
						# No extra data required
					}
					default {
						# Handle unknown types
						if {[string index $fileInfo(type) 0] == "#"} {
							# Metadata type, ignore
							# it if we don't
							# understand this type
							continue
						} else {
							# Unknown type,
							# generate an error
							error "Manifest cannot be parsed"
						}
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

		appfsd::get_path_info_cache_flush

		return COMPLETE
	}

	proc _localpath {package hostname file} {
		set dir ""
		catch {
			set homedir [::appfs::user::get_homedir]
			set dir [file join $homedir .appfs "./${package}@${hostname}" "./${file}"]
		}
		return $dir
	}

	proc _whiteoutpath {package hostname file} {
		set dir ""
		catch {
			set homedir [::appfs::user::get_homedir]
			set dir [file join $homedir .appfs "./${package}@${hostname}" ".APPFS.WHITEOUT" "./${file}.APPFS.WHITEOUT"]
		}
		return $dir
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

						set retval(os) [_normalizeOS [lindex $os_cpu 0] 1]
						set retval(cpu) [_normalizeCPU [lindex $os_cpu 1] 1]
						set retval(_children) versions
						set retval(_type) os-cpu

						if {$pathlen > 3} {
							set retval(version) [lindex $path 3]
							set retval(_children) files
							set retval(_type) versions

							set retval(package_sha1) [::appfs::db onecolumn {SELECT sha1 FROM packages WHERE hostname = $retval(hostname) AND package = $retval(package) AND os = $retval(os) AND cpuArch = $retval(cpu) AND version = $retval(version);}]
							if {$retval(package_sha1) == ""} {
								set retval(_children) dead
								return [array get retval]
							}

							if {$pathlen > 4} {
								set retval(_type) files
								set retval(file) [join [lrange $path 4 end] "/"]
							} else {
								set retval(_type) files
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

				::appfs::db eval {SELECT version FROM packages WHERE isLatest = 1 AND hostname = $pathinfo(hostname) AND package = $pathinfo(package) AND os = $pathinfo(os) AND cpuArch = $pathinfo(cpu) LIMIT 1;} latest_info {}

				if {[info exists latest_info(version)]} {
					lappend retval "latest"
				}

				return $retval
			}
			"files" {
				catch {
					::appfs::getindex $pathinfo(hostname)
					::appfs::getpkgmanifest $pathinfo(hostname) $pathinfo(package_sha1)
				}

				set retval [::appfs::db eval {SELECT DISTINCT file_name FROM files WHERE package_sha1 = $pathinfo(package_sha1) AND file_directory = $pathinfo(file);}]

				if {[info exists pathinfo(package)] && [info exists pathinfo(hostname)] && [info exists pathinfo(file)]} {
					_as_user {
						set dir [_localpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)]
						set whiteoutdir [string range [_whiteoutpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)] 0 end-15]

						if {$whiteoutdir != ""} {
							foreach file [glob -nocomplain -tails -directory $whiteoutdir {{.,}*.APPFS.WHITEOUT}] {
								set remove [string range $file 0 end-15]
								set idx [lsearch -exact $retval $remove]
								if {$idx != -1} {
									set retval [lreplace $retval $idx $idx]
								}
							}
						}

						if {$dir != ""} {
							foreach file [glob -nocomplain -tails -directory $dir {{.,}*}] {
								if {$file == "." || $file == ".."} {
									continue
								}

								if {$file == ".APPFS.WHITEOUT"} {
									continue
								}

								if {[lsearch -exact $retval $file] != -1} {
									continue
								}

								lappend retval $file
							}
						}
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

		catch {
			::appfs::getindex $pathinfo(hostname)
			::appfs::getpkgmanifest $pathinfo(hostname) $pathinfo(package_sha1)
		}

		set retval(path_type) $pathinfo(_type)

		switch -- $pathinfo(_type) {
			"toplevel" {
				set retval(type) directory
				set retval(childcount) [llength [getchildren $path]]
			}
			"sites" {
				set check [::appfs::db onecolumn {SELECT 1 FROM packages WHERE hostname = $pathinfo(hostname);}]
				if {$check == "1"} {
					set retval(type) directory
					set retval(childcount) 0
				}
			}
			"packages" {
				set check [::appfs::db onecolumn {SELECT 1 FROM packages WHERE hostname = $pathinfo(hostname) AND package = $pathinfo(package);}]
				if {$check == "1"} {
					set retval(type) directory
					set retval(childcount) [llength [getchildren $path]]
				}
			}
			"os-cpu" {
				if {$pathinfo(os) == "platform" && $pathinfo(cpu) == ""} {
					set check [::appfs::db eval {
						SELECT DISTINCT os, cpuArch FROM packages WHERE hostname = $pathinfo(hostname) AND package = $pathinfo(package);
					}]

					set retval(type) symlink

					if {$check == [list "noarch" "noarch"]} {
						set retval(source) "noarch-noarch"
					} else {
						set retval(source) $::appfs::platform
					}
				} else {
					set check [::appfs::db onecolumn {
						SELECT 1 FROM packages WHERE hostname = $pathinfo(hostname) AND package = $pathinfo(package) AND os = $pathinfo(os) AND cpuArch = $pathinfo(cpu);
					}]
					if {$check == "1"} {
						set retval(type) directory
						set retval(childcount) [llength [getchildren $path]]
					}
				}
			}
			"versions" {
				if {$pathinfo(version) == "latest"} {
					::appfs::db eval {SELECT version FROM packages WHERE isLatest = 1 AND hostname = $pathinfo(hostname) AND package = $pathinfo(package) AND os = $pathinfo(os) AND cpuArch = $pathinfo(cpu) LIMIT 1;} latest_info {}

					if {[info exists latest_info(version)]} {
						set retval(type) symlink
						set retval(source) $latest_info(version)
					}
				} else {
					if {[info exists pathinfo(package_sha1)] && $pathinfo(package_sha1) != ""} {
						set retval(type) directory
						set retval(childcount) [llength [getchildren $path]]
					}
				}
			}
			"files" {
				set retval(packaged) 1

				set localpath [_localpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)]
				set whiteoutpath  [_whiteoutpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)]

				set retval(localpath) $localpath
				set retval(whiteoutpath) $whiteoutpath

				unset -nocomplain localpathinfo
				if {$localpath != ""} {
					_as_user {
						catch {
							file lstat $localpath localpathinfo
						}
					}
				}

				if {$localpath != "" && [info exists localpathinfo]} {
					set retval(is_localfile) 1
					unset retval(packaged)
					catch {
						set retval(time) $localpathinfo(mtime)

						switch -- $localpathinfo(type) {
							"directory" {
								set retval(type) "directory"
								set retval(childcount) [llength [getchildren $path]]
							}
							"file" {
								set retval(type) "file"
								set retval(size) $localpathinfo(size)

								# Once the user writes to a file, all its other
								# attributes (such as suid) are lost

								_as_user {
									if {[file executable $localpath]} {
										set retval(perms) "x-"
									} else {
										set retval(perms) "-"
									}
								}
							}
							"link" {
								set retval(type) "symlink"

								_as_user {
									set retval(source) [file readlink $localpath]
								}
							}
							"fifo" {
								# Capitalized so that the first char is unique
								set retval(type) "Fifo"
							}
							"socket" {
								# Capitalized so that the first char is unique
								set retval(type) "Socket"
							}
						}
					} err
				} else {
					if {$whiteoutpath == "" || ![file exists $whiteoutpath]} {
						set retval(is_remotefile) 1

						set work [split $pathinfo(file) "/"]
						set directory [join [lrange $work 0 end-1] "/"]
						set file [lindex $work end]

						if {$directory == "" && $file == ""} {
							array set retval [list type directory]
						}

						::appfs::db eval {SELECT type, time, source, size, perms, file_sha1 FROM files WHERE package_sha1 = $pathinfo(package_sha1) AND file_directory = $directory AND file_name = $file;} retval {}

						# Allow an administrator to supply additional permissions to remote files
						if {[info exists retval(perms)]} {
							# Lower case this in case an upper-cased value was put in
							# the database before we started lowercasing them
							set retval(perms) [string tolower $retval(perms)]

							set retval(perms) [::appfs::user::change_perms $path $retval(file_sha1) $retval(perms)]
						}

						if {[info exists retval(type)] && $retval(type) == "directory"} {
							set retval(childcount) [llength [getchildren $path]]
						}

						unset -nocomplain retval(*)
					}
				}

			}
		}

		if {![info exists retval(type)]} {
			return -code error "No such file or directory"
		}

		return [array get retval]
	}

	proc openpath {path mode} {
		array set pathinfo [_parsepath $path]

		if {$pathinfo(_type) != "files"} {
			return -code error "invalid path type: Got \"$pathinfo(_type)\", need \"files\""
		}

		set localpath [_localpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)]

		if {$mode == "create"} {
			if {$localpath == ""} {
				return -code error "Asked to create, but no home directory."
			}

			return $localpath
		}

		if {$localpath != "" && [file exists $localpath]} {
			return $localpath
		}

		set work [split $pathinfo(file) "/"]
		set directory [join [lrange $work 0 end-1] "/"]
		set file [lindex $work end]
		::appfs::db eval {SELECT file_sha1, perms FROM files WHERE package_sha1 = $pathinfo(package_sha1) AND file_name = $file AND file_directory = $directory;} pkgpathinfo {}

		if {$pkgpathinfo(file_sha1) == ""} {
			return -code error "No such file or directory"
		}

		set localcachefile [download $pathinfo(hostname) $pkgpathinfo(file_sha1)]

		if {$mode == "write"} {
			_as_user {
				set tmplocalpath "${localpath}.[expr rand()][clock clicks]"

				set failed 0
				if {[catch {
					file mkdir [file dirname $localpath]
					file copy -force -- $localcachefile $tmplocalpath

					if {$pkgpathinfo(perms) == "x"} {
						file attributes $tmplocalpath -permissions +x
					}

					file rename -force -- $tmplocalpath $localpath
				} err]} {
					set failed 1
				}
				catch {
					file delete -force -- $tmplocalpath
				}
			}

			if {$failed} {
				return -code error $err
			}

			return $localpath
		}

		return $localcachefile
	}

	proc localpath {path} {
		array set pathinfo [_parsepath $path]

		if {$pathinfo(_type) != "files"} {
			return -code error "invalid path type: Got \"$pathinfo(_type)\", need \"files\""
		}

		set localpath [_localpath $pathinfo(package) $pathinfo(hostname) $pathinfo(file)]

		return $localpath
	}

	proc exists {path} {
		catch {
			set info [getattr $path]
		} err

		if {![info exists info]} {
			if {$err == "No such file or directory"} {
				return [list]
			} else {
				return -code error $err
			}
		}

		return $info
	}

	proc prepare_to_create {path {must_not_exist 1}} {
		if {$must_not_exist} {
			if {[exists $path] != ""} {
				return -code error "File already exists"
			}
		}

		set filename [localpath $path]

		if {$filename == ""} {
			return -code error "Asked to create, but no home directory."
		}

		set dirname [file dirname $filename]

		_as_user {
			file mkdir $dirname
		}

		return $filename
	}

	proc unlinkpath {path} {
		array set pathattrs [exists $path]

		if {$pathattrs(path_type) != "files"} {
			return -code error "invalid path type: can only delete type \"files\" this is type \"$pathattrs(path_type)\""
		}

		set localpath $pathattrs(localpath)

		if {$localpath == ""} {
			return -code error "Asked to delete, but no home directory."
		}

		if {[info exists pathattrs(is_localfile)]} {
			if {[file isdirectory $localpath]} {
				set children [getchildren $path]

				if {[llength $children] != 0} {
					return -code error "Asked to delete non-empty directory"
				}
			}

			_as_user {
				file delete -force -- $localpath
			}
		} elseif {[info exists pathattrs(is_remotefile)]} {
			if {$pathattrs(type) == "directory"} {
				set children [getchildren $path]

				if {[llength $children] != 0} {
					return -code error "Asked to delete non-empty directory"
				}
			}
		} else {
			return -code error "Unknown if file is remote or local !?"
		}

		set whiteoutfile $pathattrs(whiteoutpath)
		set whiteoutdir [file dirname $whiteoutfile]

		_as_user {
			file mkdir $whiteoutdir
			close [open $whiteoutfile w]
		}
	}
}
